#include "game_props.h"

#include <core/json/json.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <schemas/scene.h>

namespace engine::view {

using hotload::Result;

GameProps::GameProps() = default;

void GameProps::declare(const Vector<renderer::SceneProp>& props) {
  props_ = props;
  attached_ = false;
  slots_.clear();
  slots_.resize(k_max_props);
  tail_.clear();
  live_ = 0;
  dirty_ = false;
  tail_stale_ = false;
}

bool GameProps::attach(const renderer::SceneData& scene, std::string* error) {
  if (scene.props.size() != props_.size()) {
    if (error != nullptr) {
      *error = "the scene loaded " + std::to_string(scene.props.size()) +
               " props and the game runner declared " + std::to_string(props_.size());
    }
    return false;
  }
  for (u32 p = 0; p < props_.size(); ++p) {
    if (scene.props[p].name != props_[p].name) {
      if (error != nullptr) {
        *error = "the scene's prop " + std::to_string(p) + " is \"" + scene.props[p].name +
                 "\" and the game runner declared \"" + props_[p].name + "\"";
      }
      return false;
    }
    props_[p].mesh = scene.props[p].mesh;
  }
  attached_ = true;
  tail_stale_ = true;  // the instances name scene meshes, known from here on
  return true;
}

const std::string& GameProps::name_of(u32 prop_mesh) const noexcept {
  static const std::string none;
  return prop_mesh < props_.size() ? props_[prop_mesh].name : none;
}

Result GameProps::mesh_of(std::string_view name, u32& out) const noexcept {
  if (props_.empty()) return Result::Unsupported;
  for (u32 p = 0; p < props_.size(); ++p) {
    if (props_[p].name == name) {
      out = p;
      return Result::Ok;
    }
  }
  return Result::NotFound;
}

bool GameProps::valid_position(const f64* position) const noexcept {
  if (position == nullptr) return false;
  for (u32 k = 0; k < 3; ++k) {
    if (!std::isfinite(position[k])) return false;
  }
  return world_cell_valid(WorldPos{position[0], position[1], position[2]});
}

GameProps::Slot* GameProps::find(u32 id) noexcept {
  const u32 slot = id & (k_max_props - 1);
  const u32 generation = id >> k_prop_slot_bits;
  if (slot >= slots_.size() || generation == 0) return nullptr;
  Slot& s = slots_[slot];
  return s.live && s.generation == generation ? &s : nullptr;
}

Result GameProps::place(u32 mesh, const f64* position, f32 yaw, f32 scale, u32& out_prop) noexcept {
  if (props_.empty() || !attached_) return Result::Unsupported;
  if (mesh >= props_.size()) return Result::NotFound;
  if (!valid_position(position) || !std::isfinite(yaw) || !std::isfinite(scale) || !(scale > 0.0f))
    return Result::InvalidArgument;
  // The lowest free slot, so a run places the same ids whatever happened to the machine.
  u32 slot = 0;
  while (slot < slots_.size() && slots_[slot].live)
    ++slot;
  if (slot >= slots_.size()) return Result::OutOfRange;
  Slot& s = slots_[slot];
  s.live = true;
  s.prop = mesh;
  // The generation wraps inside the id's upper bits; 2^22 placements of one slot is not a run.
  s.generation = (s.generation + 1) & ((1u << (32 - k_prop_slot_bits)) - 1);
  if (s.generation == 0) s.generation = 1;
  s.position = WorldPos{position[0], position[1], position[2]};
  s.yaw = yaw;
  s.scale = scale;
  ++live_;
  ++stats_.placed;
  dirty_ = true;
  tail_stale_ = true;
  out_prop = id_of(slot, s.generation);
  return Result::Ok;
}

Result GameProps::move(u32 prop, const f64* position, f32 yaw) noexcept {
  if (props_.empty() || !attached_) return Result::Unsupported;
  Slot* s = find(prop);
  if (s == nullptr) return Result::NotFound;
  if (!valid_position(position) || !std::isfinite(yaw)) return Result::InvalidArgument;
  s->position = WorldPos{position[0], position[1], position[2]};
  s->yaw = yaw;
  ++stats_.moved;
  dirty_ = true;
  tail_stale_ = true;
  return Result::Ok;
}

Result GameProps::remove(u32 prop) noexcept {
  if (props_.empty() || !attached_) return Result::Unsupported;
  Slot* s = find(prop);
  if (s == nullptr) return Result::NotFound;
  s->live = false;
  --live_;
  ++stats_.removed;
  dirty_ = true;
  tail_stale_ = true;
  return Result::Ok;
}

Result GameProps::nearest(u32 mesh, const WorldPos& from, f64 radius, u32& out_prop, WorldPos& out,
                          f64& distance) noexcept {
  if (props_.empty()) return Result::Unsupported;
  ++stats_.queries;
  if (!(radius > 0.0) || !std::isfinite(radius)) return Result::NotFound;
  if (mesh != k_any_prop && mesh >= props_.size()) return Result::NotFound;
  f64 best = radius * radius;
  bool found = false;
  for (u32 i = 0; i < slots_.size(); ++i) {
    const Slot& s = slots_[i];
    if (!s.live || (mesh != k_any_prop && s.prop != mesh)) continue;
    const f64 dx = s.position.x - from.x;
    const f64 dz = s.position.z - from.z;
    const f64 d2 = dx * dx + dz * dz;
    if (d2 > best || (found && d2 == best)) continue;
    best = d2;
    found = true;
    out_prop = id_of(i, s.generation);
    out = s.position;
  }
  if (!found) return Result::NotFound;
  ++stats_.found;
  distance = std::sqrt(best);
  return Result::Ok;
}

const Vector<renderer::SceneInstance>& GameProps::tail() {
  if (tail_stale_) {
    tail_stale_ = false;
    tail_.clear();
    if (attached_) {
      for (const Slot& s : slots_) {
        if (!s.live) continue;
        renderer::SceneInstance instance;
        instance.mesh = props_[s.prop].mesh;
        instance.origin = s.position;  // the world position whole, the transform local (ADR-0053)
        instance.transform.rotation = quat_from_axis_angle(Vec3::unit_y(), s.yaw);
        instance.transform.scale = Vec3{s.scale, s.scale, s.scale};
        tail_.push_back(instance);
      }
    }
  }
  return tail_;
}

void GameProps::handed() noexcept {
  if (dirty_) ++stats_.rewrites;
  dirty_ = false;
}

JsonValue GameProps::to_json() const {
  JsonValue out = JsonValue::array();
  for (u32 i = 0; i < slots_.size(); ++i) {
    const Slot& s = slots_[i];
    if (!s.live) continue;
    JsonValue p = JsonValue::object();
    p.set("id", static_cast<u64>(id_of(i, s.generation)));
    p.set("prop", props_[s.prop].name);
    JsonValue position = JsonValue::array();
    position.push_back(JsonValue(s.position.x));
    position.push_back(JsonValue(s.position.y));
    position.push_back(JsonValue(s.position.z));
    p.set("position", std::move(position));
    p.set("yaw", static_cast<f64>(s.yaw));
    p.set("scale", static_cast<f64>(s.scale));
    out.push_back(std::move(p));
  }
  return out;
}

bool GameProps::from_json(const JsonValue& value, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "props: " + why;
    return false;
  };
  if (!value.is_array()) return fail("not an array");
  if (props_.empty() && value.size() > 0) return fail("the scene declares no props");
  for (u32 i = 0; i < value.size(); ++i) {
    const JsonValue& p = value[i];
    const JsonValue* id = p.find("id");
    const JsonValue* prop = p.find("prop");
    const JsonValue* position = p.find("position");
    const JsonValue* yaw = p.find("yaw");
    const JsonValue* scale = p.find("scale");
    u64 id_value = 0;
    f64 angle = 0.0;
    f64 size = 1.0;
    f64 c[3] = {0.0, 0.0, 0.0};
    if (!p.is_object() || id == nullptr || !id->get_u64(id_value) || prop == nullptr ||
        !prop->is_string() || position == nullptr || !position->is_array() ||
        position->size() != 3 || yaw == nullptr || !yaw->get_f64(angle) || scale == nullptr ||
        !scale->get_f64(size)) {
      return fail("entry " + std::to_string(i) + " is missing a field");
    }
    for (u32 k = 0; k < 3; ++k) {
      if (!(*position)[k].get_f64(c[k])) return fail("entry " + std::to_string(i) + ": position");
    }
    u32 mesh = 0;
    if (mesh_of(prop->as_string(), mesh) != Result::Ok) {
      return fail("entry " + std::to_string(i) + " names the prop \"" +
                  std::string(prop->as_string()) + "\", which this scene does not declare");
    }
    const u32 slot = static_cast<u32>(id_value) & (k_max_props - 1);
    const u32 generation = static_cast<u32>(id_value >> k_prop_slot_bits);
    if (id_value > 0xFFFFFFFFull || generation == 0 || slot >= slots_.size()) {
      return fail("entry " + std::to_string(i) + ": id " + std::to_string(id_value) +
                  " is not a prop id");
    }
    if (slots_[slot].live) return fail("entry " + std::to_string(i) + ": two props in one slot");
    if (!valid_position(c) || !std::isfinite(angle) || !std::isfinite(size) || !(size > 0.0)) {
      return fail("entry " + std::to_string(i) +
                  ": a number is not finite, or the scale is not "
                  "positive, or the position is outside the world");
    }
    Slot& s = slots_[slot];
    s.live = true;
    s.prop = mesh;
    s.generation = generation;
    s.position = WorldPos{c[0], c[1], c[2]};
    s.yaw = static_cast<f32>(angle);
    s.scale = static_cast<f32>(size);
    ++live_;
  }
  dirty_ = live_ > 0;
  tail_stale_ = true;
  return true;
}

JsonValue GameProps::summary_json() const {
  JsonValue out = JsonValue::object();
  out.set("declared", static_cast<u64>(props_.size()));
  out.set("placed", stats_.placed);
  out.set("moved", stats_.moved);
  out.set("removed", stats_.removed);
  out.set("live", static_cast<u64>(live_));
  out.set("rewrites", stats_.rewrites);
  out.set("queries", stats_.queries);
  out.set("found", stats_.found);
  return out;
}

// One schema read of the file, as `renderer::read_scene_props` makes for the props: the scene is
// loaded only once there is a device, and the module's `on_load` runs before one. The grammar is
// the schema's, so a file this refuses the scene reader refuses too.
bool read_scene_actors(const std::string& path, bool animate, Vector<DeclaredActor>& out,
                       std::string& error) {
  out.clear();
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + std::string(io::status_name(status));
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  scene::Scene file;
  schema::ReadContext context;
  if (!schema::from_json(file, root, context) || !context.ok()) {
    error = path + ": " +
            (context.diagnostics.empty()
                 ? std::string("not a scene")
                 : context.diagnostics.front().path + ": " + context.diagnostics.front().message);
    return false;
  }
  for (u32 i = 0; i < file.instances.size(); ++i) {
    const scene::Instance& instance = file.instances[i];
    if (instance.name.empty() || (!instance.animation.has_value() && !animate)) continue;
    bool taken = false;
    for (const DeclaredActor& earlier : out)
      taken = taken || earlier.name == instance.name;
    if (taken) {
      std::fprintf(stderr,
                   "engine-view: game: instance %u is named \"%s\" as an earlier one is; the actor "
                   "of that name is the first\n",
                   i, instance.name.c_str());
      continue;
    }
    out.push_back(DeclaredActor{instance.name, i});
  }
  return true;
}

void keep_actors(const Vector<DeclaredActor>& declared, renderer::SceneDesc& desc) {
  desc.authored.keep.clear();
  desc.authored.keep.reserve(declared.size());
  for (const DeclaredActor& a : declared)
    desc.authored.keep.push_back(a.instance);
}

bool loaded_actor(const DeclaredActor& actor, std::span<const u32> loaded_of_read, u32 read_count,
                  u32 loaded_count, u32& loaded, std::string& error) {
  if (loaded_of_read.empty()) {
    loaded = actor.instance < loaded_count ? actor.instance : renderer::k_not_loaded;
    return true;
  }
  loaded = actor.instance < loaded_of_read.size() ? loaded_of_read[actor.instance]
                                                  : renderer::k_not_loaded;
  if (loaded != renderer::k_not_loaded || actor.instance >= read_count) return true;
  error = "game: the actor \"" + actor.name + "\" is the scene's instance " +
          std::to_string(actor.instance) +
          ", which the streamed world handed to its tail, where nothing animates or moves it; an "
          "actor stays in the loaded prefix when the host names it before the load (keep_actors; "
          "renderer.md, \"An actor is never streamed\")";
  return false;
}

}  // namespace engine::view
