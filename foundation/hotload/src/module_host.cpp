#include "dynamic_library.h"

#include <core/log/log.h>
#include <core/memory/memory.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <foundation/hotload/module_host.h>
#include <foundation/io/vfs.h>

#include <cstring>
#include <string>
#include <utility>

namespace engine::hotload {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_hotload, "hotload");

mem::TagId hotload_tag() {
  static const mem::TagId tag = mem::register_tag("hotload");
  return tag;
}

using schema::Kind;
using schema::TypeInfo;
using schema::TypeRef;

std::string stem_of(std::string_view path) {
  std::string_view name = io::file_name(path);
  const usize dot = name.rfind('.');
  return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

std::string extension_of(std::string_view path) {
  std::string_view name = io::file_name(path);
  const usize dot = name.rfind('.');
  return dot == std::string_view::npos ? std::string() : std::string(name.substr(dot));
}

u64 read_unsigned(Kind kind, const u8* p) {
  switch (kind) {
    case Kind::U8: return *p;
    case Kind::U16: {
      u16 v;
      std::memcpy(&v, p, 2);
      return v;
    }
    case Kind::U32: {
      u32 v;
      std::memcpy(&v, p, 4);
      return v;
    }
    default: {
      u64 v;
      std::memcpy(&v, p, 8);
      return v;
    }
  }
}

i64 read_signed(Kind kind, const u8* p) {
  switch (kind) {
    case Kind::I8: {
      i8 v;
      std::memcpy(&v, p, 1);
      return v;
    }
    case Kind::I16: {
      i16 v;
      std::memcpy(&v, p, 2);
      return v;
    }
    case Kind::I32: {
      i32 v;
      std::memcpy(&v, p, 4);
      return v;
    }
    default: {
      i64 v;
      std::memcpy(&v, p, 8);
      return v;
    }
  }
}

// One field's value, the way the Luau binding pushes one (docs/subsystems/scripting.md, "The
// generated bindings"), into a plain struct instead of onto a VM stack.
Result read_value(const TypeRef& ref, const u8* p, Value& out) {
  switch (ref.kind) {
    case Kind::Bool:
      out.kind = ValueKind::Bool;
      out.b = *reinterpret_cast<const bool*>(p);
      return Result::Ok;
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
      out.kind = ValueKind::UInt;
      out.u = read_unsigned(ref.kind, p);
      return Result::Ok;
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
      out.kind = ValueKind::Int;
      out.i = read_signed(ref.kind, p);
      return Result::Ok;
    case Kind::F32: {
      f32 v;
      std::memcpy(&v, p, 4);
      out.kind = ValueKind::Float;
      out.f = static_cast<f64>(v);
      return Result::Ok;
    }
    case Kind::F64:
      out.kind = ValueKind::Float;
      std::memcpy(&out.f, p, 8);
      return Result::Ok;
    case Kind::String: {
      const std::string& s = *reinterpret_cast<const std::string*>(p);
      out.kind = ValueKind::String;
      out.str.data = s.data();
      out.str.length = s.size();
      return Result::Ok;
    }
    case Kind::Enum: {
      const Kind underlying = ref.type->enum_underlying;
      const i64 value = schema::is_unsigned(underlying)
                            ? static_cast<i64>(read_unsigned(underlying, p))
                            : read_signed(underlying, p);
      out.kind = ValueKind::Enum;
      out.i = value;
      if (const schema::EnumValueInfo* name = ref.type->find_value(value)) {
        out.str.data = name->name;
        out.str.length = std::strlen(name->name);
      }
      return Result::Ok;
    }
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Vec4:
    case Kind::Quat: {
      const u32 n = ref.kind == Kind::Vec2 ? 2u : ref.kind == Kind::Vec3 ? 3u : 4u;
      out.kind = ValueKind::Vector;
      out.count = n;
      std::memcpy(out.v, p, n * sizeof(f32));
      return Result::Ok;
    }
    case Kind::WorldPos:
    case Kind::DVec3:
      out.kind = ValueKind::Vector;
      out.count = 3;
      std::memcpy(out.d, p, 3 * sizeof(f64));
      return Result::Ok;
    case Kind::Id128:
      out.kind = ValueKind::Id128;
      std::memcpy(out.id, p, 16);
      return Result::Ok;
    case Kind::Optional:
      if (!ref.optional_ops->has_value(p)) {
        out.kind = ValueKind::Nil;
        return Result::Ok;
      }
      return read_value(*ref.element, static_cast<const u8*>(ref.optional_ops->get(p)), out);
    case Kind::Array:
    case Kind::FixedArray:
      out.kind = ValueKind::Array;
      out.count = static_cast<u32>(ref.array_ops->size(p));
      return Result::Ok;
    case Kind::Struct:
    case Kind::Json:
    case Kind::Bytes:
    case Kind::Map: break;
  }
  return Result::Unsupported;
}

}  // namespace

// ---- one loaded build ---------------------------------------------------------------------------

struct ModuleHost::Module {
  void* library = nullptr;
  std::string copy_path;
  std::string name;
  u32 abi_version = 0;  // what the build declared, k_abi_oldest..k_abi_version
  const TypeInfo* state_type = nullptr;
  void* state = nullptr;
  i32 (*on_load)(ModuleContext*) = nullptr;
  void (*on_unload)(ModuleContext*) = nullptr;
  struct Fn {
    std::string name;
    u32 signature = 0;
    void (*pointer)() = nullptr;
  };
  Vector<Fn> exports;
};

// ---- the HostApi the module calls ---------------------------------------------------------------

struct HostCallbacks {
  static ModuleHost& self(void* host) { return *static_cast<ModuleHost*>(host); }

  static void* allocate(void*, u64 bytes, u64 align) {
    mem::TagScope scope(hotload_tag());
    return mem::try_allocate(static_cast<usize>(bytes), static_cast<usize>(align));
  }
  static void deallocate(void*, void* p, u64 bytes, u64 align) {
    mem::deallocate(p, static_cast<usize>(bytes), static_cast<usize>(align));
  }
  static void log(void* host, LogLevel level, const char* message, u64 length) {
    const ModuleHost& h = self(host);
    log::Level l = log::Level::Info;
    switch (level) {
      case LogLevel::Trace: l = log::Level::Trace; break;
      case LogLevel::Debug: l = log::Level::Debug; break;
      case LogLevel::Info: l = log::Level::Info; break;
      case LogLevel::Warn: l = log::Level::Warn; break;
      case LogLevel::Error: l = log::Level::Error; break;
    }
    if (!log_hotload.enabled(l)) return;
    const log::Field fields[] = {log::field("module", h.module_name())};
    log::emit(log_hotload, l, nullptr, 0, std::string_view(message, static_cast<usize>(length)),
              fields);
  }
  static u64 tick(void* host) { return self(host).config_.tick; }
  static f64 fixed_step(void* host) { return self(host).config_.fixed_step; }
  static Result resolve(void* host, const char* type, u64 type_length, const char* path,
                        u64 path_length, FieldRef* out) {
    if (out == nullptr) return Result::InvalidArgument;
    return self(host).resolve(std::string_view(type, static_cast<usize>(type_length)),
                              std::string_view(path, static_cast<usize>(path_length)), *out);
  }
  static Result enum_value(void* host, FieldRef field, const char* name, u64 length, i64* out) {
    ModuleHost& h = self(host);
    if (out == nullptr || field.index >= h.fields_.size()) return Result::InvalidArgument;
    const ModuleHost::Field& f = h.fields_[field.index];
    const TypeRef* ref = f.type;
    if (ref->kind == Kind::Optional && ref->element != nullptr) ref = ref->element;
    const std::string_view wanted(name, static_cast<usize>(length));
    if (ref->kind != Kind::Enum || ref->type == nullptr) {
      h.pending_unresolved_.push_back(f.type_name + "." + f.path + " is not an enum (asked for '" +
                                      std::string(wanted) + "')");
      return Result::WrongType;
    }
    if (const schema::EnumValueInfo* v = ref->type->find_value(wanted)) {
      *out = v->value;
      return Result::Ok;
    }
    std::string names;
    for (const schema::EnumValueInfo& v : ref->type->values)
      names += std::string(names.empty() ? "" : ", ") + v.name;
    h.pending_unresolved_.push_back(std::string(ref->type->qualified_name) +
                                    " has no enumerator '" + std::string(wanted) + "' (it has " +
                                    names + ")");
    return Result::NotFound;
  }
  static Result read(void* host, View view, FieldRef field, Value* out) {
    return self(host).read(view, field, *out);
  }
  static Result read_element(void* host, View view, FieldRef field, u64 index, Value* out) {
    ModuleHost& h = self(host);
    *out = Value{};
    if (view.slot >= h.slots_.size() || field.index >= h.fields_.size()) {
      ++h.stats_.refused_reads;
      return Result::InvalidArgument;
    }
    const ModuleHost::Slot& s = h.slots_[view.slot];
    if (!s.live || s.version != view.version) {
      ++h.stats_.refused_reads;
      return Result::StaleView;
    }
    const ModuleHost::Field& f = h.fields_[field.index];
    if (f.root != s.type) {
      ++h.stats_.refused_reads;
      return Result::WrongType;
    }
    if (f.kind != Kind::Array && f.kind != Kind::FixedArray) return Result::WrongType;
    const u8* p = static_cast<const u8*>(s.object) + f.offset;
    if (index >= f.type->array_ops->size(p)) return Result::OutOfRange;
    const u8* element = static_cast<const u8*>(f.type->array_ops->at(p, static_cast<usize>(index)));
    return read_value(*f.type->element, element, *out);
  }
  static void fail(void* host, const char* message, u64 length) {
    self(host).pending_failure_.assign(message, static_cast<usize>(length));
  }

  // ---- the game runner's services (ABI 2): forwarded to HostConfig::game ----
  static Result game_action(void* host, const char* name, u64 length, u32* out_bit) {
    ModuleHost& h = self(host);
    if (out_bit == nullptr || name == nullptr) return Result::InvalidArgument;
    const std::string_view wanted(name, static_cast<usize>(length));
    const GameServices& g = h.game_;
    if (g.action == nullptr) {
      h.pending_unresolved_.push_back("game action '" + std::string(wanted) +
                                      "': this host runs no game");
      return Result::Unsupported;
    }
    std::string why;
    const Result r = g.action(g.user, wanted, out_bit, &why);
    if (r != Result::Ok) {
      h.pending_unresolved_.push_back(why.empty() ? "no game action '" + std::string(wanted) + "'"
                                                  : why);
    }
    return r;
  }
  static void hud_text(void* host, u32 line, const char* text, u64 length) {
    const GameServices& g = self(host).game_;
    if (g.hud_text == nullptr || text == nullptr) return;
    g.hud_text(g.user, line, std::string_view(text, static_cast<usize>(length)));
  }
  static void hud_meter(void* host, const char* name, u64 length, f64 value, f64 max) {
    const GameServices& g = self(host).game_;
    if (g.hud_meter == nullptr || name == nullptr) return;
    g.hud_meter(g.user, std::string_view(name, static_cast<usize>(length)), value, max);
  }
  static void end_run(void* host, const char* json, u64 length) {
    const GameServices& g = self(host).game_;
    if (g.end_run == nullptr || json == nullptr) return;
    g.end_run(g.user, std::string_view(json, static_cast<usize>(length)));
  }
  static Result nearest_placement(void* host, const char* kind, u64 length, f64 radius,
                                  f64* out_position, f64* out_distance) {
    const GameServices& g = self(host).game_;
    if (kind == nullptr || out_position == nullptr || out_distance == nullptr)
      return Result::InvalidArgument;
    if (g.nearest_placement == nullptr) return Result::Unsupported;
    return g.nearest_placement(g.user, std::string_view(kind, static_cast<usize>(length)), radius,
                               out_position, out_distance);
  }

  // ---- a game acts on the world (ABI 3): forwarded the same way ----
  // The two `on_load` lookups: `NotFound` is remembered with the service's sentence, so the load is
  // refused naming it beside the field paths; `Unsupported` (no props, no sound in this session; or
  // a host that runs no game, which says so) is an answer the module acts on.
  static Result prop_mesh(void* host, const char* name, u64 length, u32* out_mesh) {
    ModuleHost& h = self(host);
    if (out_mesh == nullptr || name == nullptr) return Result::InvalidArgument;
    const std::string_view wanted(name, static_cast<usize>(length));
    const GameServices& g = h.game_;
    if (g.prop_mesh == nullptr) return Result::Unsupported;
    std::string why;
    const Result r = g.prop_mesh(g.user, wanted, out_mesh, &why);
    if (r == Result::NotFound) {
      h.pending_unresolved_.push_back(
          why.empty() ? "no prop '" + std::string(wanted) + "' in the scene" : why);
    }
    return r;
  }
  static Result prop_place(void* host, u32 mesh, const f64* position, f32 yaw, f32 scale,
                           u32* out_prop) {
    const GameServices& g = self(host).game_;
    if (position == nullptr || out_prop == nullptr) return Result::InvalidArgument;
    if (g.prop_place == nullptr) return Result::Unsupported;
    return g.prop_place(g.user, mesh, position, yaw, scale, out_prop);
  }
  static Result prop_move(void* host, u32 prop, const f64* position, f32 yaw) {
    const GameServices& g = self(host).game_;
    if (position == nullptr) return Result::InvalidArgument;
    if (g.prop_move == nullptr) return Result::Unsupported;
    return g.prop_move(g.user, prop, position, yaw);
  }
  static Result prop_remove(void* host, u32 prop) {
    const GameServices& g = self(host).game_;
    if (g.prop_remove == nullptr) return Result::Unsupported;
    return g.prop_remove(g.user, prop);
  }
  static Result nearest_prop(void* host, u32 mesh, f64 radius, u32* out_prop, f64* out_position,
                             f64* out_distance) {
    const GameServices& g = self(host).game_;
    if (out_prop == nullptr || out_position == nullptr || out_distance == nullptr)
      return Result::InvalidArgument;
    if (g.nearest_prop == nullptr) return Result::Unsupported;
    return g.nearest_prop(g.user, mesh, radius, out_prop, out_position, out_distance);
  }
  static Result walker_set(void* host, f32 speed_scale, f32 sprint_scale) {
    const GameServices& g = self(host).game_;
    if (g.walker_set == nullptr) return Result::Unsupported;
    return g.walker_set(g.user, speed_scale, sprint_scale);
  }
  static Result walker_teleport(void* host, const f64* position, f32 heading, f32 pitch) {
    const GameServices& g = self(host).game_;
    if (position == nullptr) return Result::InvalidArgument;
    if (g.walker_teleport == nullptr) return Result::Unsupported;
    return g.walker_teleport(g.user, position, heading, pitch);
  }
  static Result sound_clip(void* host, const char* name, u64 length, u32* out_clip) {
    ModuleHost& h = self(host);
    if (out_clip == nullptr || name == nullptr) return Result::InvalidArgument;
    const std::string_view wanted(name, static_cast<usize>(length));
    const GameServices& g = h.game_;
    if (g.sound_clip == nullptr) return Result::Unsupported;
    std::string why;
    const Result r = g.sound_clip(g.user, wanted, out_clip, &why);
    if (r == Result::NotFound) {
      h.pending_unresolved_.push_back(
          why.empty() ? "no sound '" + std::string(wanted) + "' in the scene" : why);
    }
    return r;
  }
  static Result sound_play(void* host, u32 clip, const f64* position, f32 gain, u32* out_voice) {
    const GameServices& g = self(host).game_;
    if (out_voice == nullptr) return Result::InvalidArgument;
    if (g.sound_play == nullptr) return Result::Unsupported;
    return g.sound_play(g.user, clip, position, gain, out_voice);
  }
  static Result sound_stop(void* host, u32 voice) {
    const GameServices& g = self(host).game_;
    if (g.sound_stop == nullptr) return Result::Unsupported;
    return g.sound_stop(g.user, voice);
  }
  // ---- ABI 4 ----
  // A struct is read only when its `size` says the module built at least ABI 4's, and up to this
  // host's own: a later struct's extra fields are not this host's to read.
  // From ABI 6 the element itself grows (`font`, the spacing, the shadow): the host takes any from
  // ABI 4's size up and copies it into one of its own up to the module's `size`, so the fields an
  // older module never had are the defaults and nothing past its struct is read.
  static Result hud_element(void* host, const HudElement* element) {
    const GameServices& g = self(host).game_;
    if (element == nullptr || element->size < k_hud_element_v4_size) return Result::InvalidArgument;
    if (g.hud_element == nullptr) return Result::Unsupported;
    HudElement e;
    const usize n = element->size < sizeof(HudElement) ? element->size : sizeof(HudElement);
    std::memcpy(static_cast<void*>(&e), element, n);
    e.size = static_cast<u32>(n);
    return g.hud_element(g.user, e);
  }
  static Result hud_remove(void* host, u32 id) {
    const GameServices& g = self(host).game_;
    if (g.hud_remove == nullptr) return Result::Unsupported;
    return g.hud_remove(g.user, id);
  }
  static Result graphics_state(void* host, GraphicsState* out) {
    const GameServices& g = self(host).game_;
    if (out == nullptr || out->size < sizeof(GraphicsState)) return Result::InvalidArgument;
    if (g.graphics_state == nullptr) return Result::Unsupported;
    GraphicsState state;  // the host's own size, copied back over the module's
    const Result r = g.graphics_state(g.user, state);
    if (r == Result::Ok) {
      const u32 size = out->size;
      *out = state;
      out->size = size;
    }
    return r;
  }
  static Result request_graphics(void* host, const GraphicsRequest* request) {
    const GameServices& g = self(host).game_;
    if (request == nullptr || request->size < sizeof(GraphicsRequest))
      return Result::InvalidArgument;
    if (g.request_graphics == nullptr) return Result::Unsupported;
    return g.request_graphics(g.user, *request);
  }
  // ---- ABI 5: actors and the ground ----
  static Result actor_find(void* host, const char* name, u64 length, u32* out_actor) {
    ModuleHost& h = self(host);
    if (out_actor == nullptr || name == nullptr) return Result::InvalidArgument;
    const std::string_view wanted(name, static_cast<usize>(length));
    const GameServices& g = h.game_;
    if (g.actor_find == nullptr) return Result::Unsupported;
    std::string why;
    const Result r = g.actor_find(g.user, wanted, out_actor, &why);
    // Remembered inside `on_load` only, like a field path that does not resolve; at a tick it is a
    // plain answer (a module may look an actor up whenever it likes), and remembering it there
    // would grow the list a tick at a time.
    if (r == Result::NotFound && h.in_load_hook_) {
      h.pending_unresolved_.push_back(
          why.empty() ? "no actor '" + std::string(wanted) + "' in the scene" : why);
    }
    return r;
  }
  static Result actor_spawn(void* host, const char* mesh, u64 length, const f64* position, f32 yaw,
                            u32* out_actor) {
    const GameServices& g = self(host).game_;
    if (mesh == nullptr || position == nullptr || out_actor == nullptr)
      return Result::InvalidArgument;
    if (g.actor_spawn == nullptr) return Result::Unsupported;
    return g.actor_spawn(g.user, std::string_view(mesh, static_cast<usize>(length)), position, yaw,
                         out_actor);
  }
  static Result actor_place(void* host, u32 actor, const f64* position, f32 yaw) {
    const GameServices& g = self(host).game_;
    if (position == nullptr) return Result::InvalidArgument;
    if (g.actor_place == nullptr) return Result::Unsupported;
    return g.actor_place(g.user, actor, position, yaw);
  }
  static Result actor_play(void* host, u32 actor, const char* clip, u64 length, f32 fade_s,
                           u32 looping) {
    const GameServices& g = self(host).game_;
    if (clip == nullptr) return Result::InvalidArgument;
    for (u64 i = 0; i < length; ++i) {
      if (clip[i] == '\0') return Result::InvalidArgument;  // shorter than its length says
    }
    if (g.actor_play == nullptr) return Result::Unsupported;
    return g.actor_play(g.user, actor, std::string_view(clip, static_cast<usize>(length)), fade_s,
                        looping != 0);
  }
  static Result actor_stop(void* host, u32 actor, f32 fade_s) {
    const GameServices& g = self(host).game_;
    if (g.actor_stop == nullptr) return Result::Unsupported;
    return g.actor_stop(g.user, actor, fade_s);
  }
  static Result actor_speed(void* host, u32 actor, f32 speed) {
    const GameServices& g = self(host).game_;
    if (g.actor_speed == nullptr) return Result::Unsupported;
    return g.actor_speed(g.user, actor, speed);
  }
  static Result actor_state(void* host, u32 actor, ActorState* out) {
    const GameServices& g = self(host).game_;
    if (out == nullptr || out->size < sizeof(ActorState)) return Result::InvalidArgument;
    if (g.actor_state == nullptr) return Result::Unsupported;
    ActorState state;  // the host's own size, copied back over the module's
    const Result r = g.actor_state(g.user, actor, state);
    if (r == Result::Ok) {
      const u32 size = out->size;
      *out = state;
      out->size = size;
    }
    return r;
  }
  static Result ground_height(void* host, f64 x, f64 z, f64* out_height) {
    const GameServices& g = self(host).game_;
    if (out_height == nullptr) return Result::InvalidArgument;
    if (g.ground_height == nullptr) return Result::Unsupported;
    return g.ground_height(g.user, x, z, out_height);
  }
  // ---- ABI 6 ----
  static Result hud_image(void* host, const HudImage* image) {
    const GameServices& g = self(host).game_;
    if (image == nullptr || image->size < sizeof(HudImage)) return Result::InvalidArgument;
    if (image->path == nullptr && image->path_length > 0) return Result::InvalidArgument;
    if (g.hud_image == nullptr) return Result::Unsupported;
    return g.hud_image(g.user, *image);
  }
  static Result hud_font(void* host, const char* path, u64 length, f32 size_px, u32* out_font) {
    const GameServices& g = self(host).game_;
    if (out_font == nullptr || (path == nullptr && length > 0)) return Result::InvalidArgument;
    if (g.hud_font == nullptr) return Result::Unsupported;
    return g.hud_font(g.user,
                      std::string_view(path != nullptr ? path : "", static_cast<usize>(length)),
                      size_px, out_font);
  }
};

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotLoaded: return "NotLoaded";
    case Status::CopyFailed: return "CopyFailed";
    case Status::LoadFailed: return "LoadFailed";
    case Status::NoEntryPoint: return "NoEntryPoint";
    case Status::DescribeFailed: return "DescribeFailed";
    case Status::AbiMismatch: return "AbiMismatch";
    case Status::BadDescriptor: return "BadDescriptor";
    case Status::StateIncompatible: return "StateIncompatible";
    case Status::LoadHookFailed: return "LoadHookFailed";
    case Status::MissingFunction: return "MissingFunction";
    case Status::SignatureMismatch: return "SignatureMismatch";
    case Status::NotFound: return "NotFound";
    case Status::InvalidArgument: return "InvalidArgument";
  }
  return "?";
}

ModuleHost::ModuleHost(HostConfig config) : config_(std::move(config)) {
  api_.abi_version = k_abi_version;
  api_.size = static_cast<u32>(sizeof(HostApi));
  api_.host = this;
  api_.allocate = &HostCallbacks::allocate;
  api_.deallocate = &HostCallbacks::deallocate;
  api_.log = &HostCallbacks::log;
  api_.tick = &HostCallbacks::tick;
  api_.fixed_step = &HostCallbacks::fixed_step;
  api_.resolve = &HostCallbacks::resolve;
  api_.enum_value = &HostCallbacks::enum_value;
  api_.read = &HostCallbacks::read;
  api_.read_element = &HostCallbacks::read_element;
  api_.fail = &HostCallbacks::fail;
  api_.game_action = &HostCallbacks::game_action;
  api_.hud_text = &HostCallbacks::hud_text;
  api_.hud_meter = &HostCallbacks::hud_meter;
  api_.end_run = &HostCallbacks::end_run;
  api_.nearest_placement = &HostCallbacks::nearest_placement;
  api_.prop_mesh = &HostCallbacks::prop_mesh;
  api_.prop_place = &HostCallbacks::prop_place;
  api_.prop_move = &HostCallbacks::prop_move;
  api_.prop_remove = &HostCallbacks::prop_remove;
  api_.nearest_prop = &HostCallbacks::nearest_prop;
  api_.walker_set = &HostCallbacks::walker_set;
  api_.walker_teleport = &HostCallbacks::walker_teleport;
  api_.sound_clip = &HostCallbacks::sound_clip;
  api_.sound_play = &HostCallbacks::sound_play;
  api_.sound_stop = &HostCallbacks::sound_stop;
  api_.hud_element = &HostCallbacks::hud_element;
  api_.hud_remove = &HostCallbacks::hud_remove;
  api_.graphics_state = &HostCallbacks::graphics_state;
  api_.request_graphics = &HostCallbacks::request_graphics;
  api_.actor_find = &HostCallbacks::actor_find;
  api_.actor_spawn = &HostCallbacks::actor_spawn;
  api_.actor_place = &HostCallbacks::actor_place;
  api_.actor_play = &HostCallbacks::actor_play;
  api_.actor_stop = &HostCallbacks::actor_stop;
  api_.actor_speed = &HostCallbacks::actor_speed;
  api_.actor_state = &HostCallbacks::actor_state;
  api_.ground_height = &HostCallbacks::ground_height;
  api_.hud_image = &HostCallbacks::hud_image;
  api_.hud_font = &HostCallbacks::hud_font;
  context_.api = &api_;
  context_.state = nullptr;
}

ModuleHost::~ModuleHost() { unload(); }

std::string ModuleHost::take_failure() {
  std::string out;
  out.swap(pending_failure_);
  return out;
}

const std::string& ModuleHost::module_name() const noexcept {
  static const std::string none;
  return module_ ? module_->name : none;
}

u32 ModuleHost::module_abi() const noexcept { return module_ ? module_->abi_version : 0u; }

const std::string& ModuleHost::loaded_copy_path() const noexcept {
  static const std::string none;
  return module_ ? module_->copy_path : none;
}

const TypeInfo* ModuleHost::state_type() const noexcept {
  return module_ ? module_->state_type : nullptr;
}

void ModuleHost::fail(Status status, std::string message) {
  last_error_ = std::string(status_name(status)) + ": " + std::move(message);
  ++stats_.failed_loads;
  ENGINE_LOG_WARN(log_hotload, "module load refused", log::field("library", library_path_),
                  log::field("error", last_error_));
}

// ---- loading ------------------------------------------------------------------------------------

Status ModuleHost::load(std::string_view library_path) {
  unload();
  functions_.clear();
  library_path_ = std::string(library_path);
  generation_ = 0;
  return reload();
}

Status ModuleHost::reload() {
  if (library_path_.empty()) return Status::NotLoaded;
  io::FileInfo info;
  if (io::stat_file(library_path_, info) == io::Status::Ok) {
    watched_size_ = info.size;
    watched_modified_ms_ = info.modified_unix_ms;
  }

  // The per-load copy: Windows keeps a loaded DLL's file locked, so the build could not replace
  // the watched file while the host runs it; and glibc's dlopen returns the handle it already has
  // for a path it has loaded, so the same name would never load the new code.
  std::string dir =
      config_.shadow_directory.empty() ? library_path_ + ".hotload" : config_.shadow_directory;
  if (io::make_directories(dir) != io::Status::Ok) {
    fail(Status::CopyFailed, "cannot create " + dir);
    return Status::CopyFailed;
  }
  const i64 copy_start = time::monotonic_ns();
  const u32 next = generation_ + 1;
  const std::string copy =
      io::join_path(dir, stem_of(library_path_) + "." + std::to_string(detail::process_id()) + "." +
                             std::to_string(next) + extension_of(library_path_));
  {
    std::string bytes;
    if (io::read_file(library_path_, bytes) != io::Status::Ok || bytes.empty()) {
      fail(Status::CopyFailed, "cannot read " + library_path_);
      return Status::CopyFailed;
    }
    if (io::write_file(copy, bytes) != io::Status::Ok) {
      fail(Status::CopyFailed, "cannot write " + copy);
      return Status::CopyFailed;
    }
  }

  pending_copy_ns_ = time::monotonic_ns() - copy_start;
  auto build = std::make_unique<Module>();
  build->copy_path = copy;
  const i64 load_start = time::monotonic_ns();
  const Status loaded = load_build(*build, copy);
  pending_load_ns_ = time::monotonic_ns() - load_start;
  if (loaded != Status::Ok) {
    destroy_build(build);
    return loaded;
  }
  return adopt(std::move(build));
}

Status ModuleHost::load_build(Module& build, const std::string& copy_path) {
  std::string error;
  build.library = detail::open_library(copy_path, error);
  if (build.library == nullptr) {
    fail(Status::LoadFailed, copy_path + ": " + error);
    return Status::LoadFailed;
  }
  auto describe = reinterpret_cast<DescribeFn>(detail::find_symbol(build.library, k_entry_point));
  if (describe == nullptr) {
    fail(
        Status::NoEntryPoint,
        library_path_ + " exports no " + k_entry_point +
            " (declare it with ENGINE_HOTLOAD_DESCRIBE, which makes it extern \"C\" and exported)");
    return Status::NoEntryPoint;
  }
  ModuleDesc desc{};
  pending_failure_.clear();
  const i32 rc = describe(&api_, &desc);
  if (rc != 0) {
    fail(Status::DescribeFailed, "describe returned " + std::to_string(rc) +
                                     (pending_failure_.empty() ? "" : ": " + pending_failure_));
    return Status::DescribeFailed;
  }
  // ABI 3 and later are one table that only grows (module_abi.h, `k_abi_oldest`): a module built
  // against an older one calls a prefix of this host's table. A newer one would call past its end.
  if (desc.abi_version < k_abi_oldest || desc.abi_version > k_abi_version ||
      desc.desc_size != sizeof(ModuleDesc)) {
    fail(Status::AbiMismatch,
         "the module was built against hotload ABI " + std::to_string(desc.abi_version) + " (a " +
             std::to_string(desc.desc_size) + "-byte ModuleDesc); this host is ABI " +
             std::to_string(k_abi_version) + " and loads " + std::to_string(k_abi_oldest) + " to " +
             std::to_string(k_abi_version) + " (" + std::to_string(sizeof(ModuleDesc)) +
             " bytes): rebuild the module against this engine");
    return Status::AbiMismatch;
  }
  build.abi_version = desc.abi_version;
  build.name = desc.name != nullptr ? desc.name : "unnamed";
  build.on_load = desc.on_load;
  build.on_unload = desc.on_unload;
  if (desc.state_type != nullptr) {
    if (desc.type_info_size != sizeof(TypeInfo)) {
      fail(Status::AbiMismatch,
           "the state descriptor was built against a " + std::to_string(desc.type_info_size) +
               "-byte TypeInfo; this host's is " + std::to_string(sizeof(TypeInfo)));
      return Status::AbiMismatch;
    }
    std::string why;
    if (!check_state_type(*desc.state_type, why)) {
      fail(Status::BadDescriptor, why);
      return Status::BadDescriptor;
    }
    build.state_type = desc.state_type;
  }
  if (desc.export_count > 4096 || (desc.export_count > 0 && desc.exports == nullptr)) {
    fail(Status::BadDescriptor, "an export table of " + std::to_string(desc.export_count) +
                                    " entries at " + (desc.exports ? "an address" : "null"));
    return Status::BadDescriptor;
  }
  for (u32 i = 0; i < desc.export_count; ++i) {
    const Export& e = desc.exports[i];
    if (e.name == nullptr || e.function == nullptr || e.signature == 0 ||
        e.signature > k_signature_max) {
      fail(Status::BadDescriptor,
           "export " + std::to_string(i) + " has no name, no function or an unknown signature");
      return Status::BadDescriptor;
    }
    for (const Module::Fn& seen : build.exports) {
      if (seen.name == e.name) {
        fail(Status::BadDescriptor, std::string("two exports are named ") + e.name);
        return Status::BadDescriptor;
      }
    }
    build.exports.push_back(Module::Fn{e.name, e.signature, e.function});
  }
  return Status::Ok;
}

Status ModuleHost::adopt(std::unique_ptr<Module> build) {
  const i64 start = time::monotonic_ns();
  // The functions the host already holds must still be there, under the same signature: a call
  // site compiled against one signature must never reach a function of another.
  Vector<void (*)()> pointers;
  pointers.reserve(functions_.size());
  for (const FunctionSlot& held : functions_) {
    const Module::Fn* found = nullptr;
    for (const Module::Fn& fn : build->exports)
      if (fn.name == held.name) found = &fn;
    if (found == nullptr) {
      fail(Status::MissingFunction, "the new build no longer exports '" + held.name +
                                        "', which the host holds; export it or stop calling it");
      destroy_build(build);
      return Status::MissingFunction;
    }
    if (found->signature != held.signature) {
      fail(Status::SignatureMismatch,
           "'" + held.name + "' is exported with signature " + std::to_string(found->signature) +
               "; the host calls it as " + std::to_string(held.signature));
      destroy_build(build);
      return Status::SignatureMismatch;
    }
    pointers.push_back(found->pointer);
  }

  // The new state: constructed by the new build, then filled from the old one through the two
  // descriptors.
  ReloadReport report;
  if (build->state_type != nullptr) {
    const TypeInfo& type = *build->state_type;
    {
      mem::TagScope scope(hotload_tag());
      build->state = mem::allocate(type.size, type.align);
    }
    type.ops->construct(build->state);
    if (module_ && module_->state_type != nullptr) {
      std::string why;
      if (!migrate_state(*module_->state_type, module_->state, type, build->state, report, why)) {
        fail(Status::StateIncompatible, why);
        destroy_build(build);
        return Status::StateIncompatible;
      }
    } else if (module_ && module_->state_type == nullptr) {
      for (const schema::FieldInfo& f : type.fields)
        report.added.push_back(f.name);
    }
  } else if (module_ && module_->state_type != nullptr) {
    for (const schema::FieldInfo& f : module_->state_type->fields)
      report.removed.push_back(f.name);
  }

  // on_load on the new build, before it replaces anything: it resolves its field paths, and
  // every one that does not resolve is reported together.
  if (build->on_load != nullptr) {
    ModuleContext trial{&api_, build->state};
    pending_failure_.clear();
    pending_unresolved_.clear();
    in_load_hook_ = true;
    const i32 rc = build->on_load(&trial);
    in_load_hook_ = false;
    if (rc != 0 || !pending_unresolved_.empty()) {
      std::string message = "on_load";
      if (rc != 0) message += " returned " + std::to_string(rc);
      if (!pending_failure_.empty()) message += ": " + pending_failure_;
      for (const std::string& line : pending_unresolved_)
        message += "; " + line;
      pending_unresolved_.clear();
      fail(Status::LoadHookFailed, message);
      destroy_build(build);
      return Status::LoadHookFailed;
    }
  }

  // The swap. The old build hears it is leaving while it can still run, then its state is
  // destroyed by its own code, then it is unloaded.
  report.migrate_ns = time::monotonic_ns() - start;
  const i64 unload_start = time::monotonic_ns();
  if (module_) {
    if (module_->on_unload != nullptr) module_->on_unload(&context_);
    destroy_build(module_);
  }
  report.unload_ns = time::monotonic_ns() - unload_start;
  report.copy_ns = pending_copy_ns_;
  report.load_ns = pending_load_ns_;
  module_ = std::move(build);
  context_.state = module_->state;
  for (usize i = 0; i < functions_.size(); ++i)
    functions_[static_cast<u32>(i)].pointer = pointers[static_cast<u32>(i)];
  ++generation_;
  ++stats_.loads;
  report.generation = generation_;
  report_ = std::move(report);
  last_error_.clear();
  ENGINE_LOG_INFO(log_hotload, "module loaded", log::field("module", module_->name),
                  log::field("generation", generation_),
                  log::field("added", static_cast<u64>(report_.added.size())),
                  log::field("removed", static_cast<u64>(report_.removed.size())),
                  log::field("us", (time::monotonic_ns() - start) / 1000));
  return Status::Ok;
}

void ModuleHost::destroy_build(std::unique_ptr<Module>& build) {
  if (!build) return;
  if (build->state != nullptr && build->state_type != nullptr) {
    build->state_type->ops->destroy(build->state);
    mem::deallocate(build->state, build->state_type->size, build->state_type->align);
    build->state = nullptr;
  }
  detail::close_library(build->library);
  build->library = nullptr;
  if (!build->copy_path.empty()) io::remove_file(build->copy_path);
  build.reset();
}

u32 ModuleHost::poll_changes() {
  if (library_path_.empty()) return 0;
  io::FileInfo info;
  // A file that is missing for a moment (a linker that deletes before it writes) keeps the build.
  if (io::stat_file(library_path_, info) != io::Status::Ok) return 0;
  if (info.size == watched_size_ && info.modified_unix_ms == watched_modified_ms_) return 0;
  return reload() == Status::Ok ? 1u : 0u;
}

void ModuleHost::unload() {
  if (!module_) return;
  if (module_->on_unload != nullptr) module_->on_unload(&context_);
  destroy_build(module_);
  context_.state = nullptr;
}

bool ModuleHost::reset_state() {
  if (!module_ || module_->state == nullptr || module_->state_type == nullptr) return false;
  // The build's own code destroys and constructs the object, as a load does: a state is
  // fixed-layout values only (`check_state_type`), so neither call reaches past it.
  const TypeInfo& type = *module_->state_type;
  type.ops->destroy(module_->state);
  type.ops->construct(module_->state);
  return true;
}

// ---- exports ------------------------------------------------------------------------------------

Status ModuleHost::find_function(std::string_view name, u32 signature, FunctionId& out) {
  out = FunctionId{};
  for (usize i = 0; i < functions_.size(); ++i) {
    const FunctionSlot& slot = functions_[static_cast<u32>(i)];
    if (slot.name != name) continue;
    if (slot.signature != signature) return Status::SignatureMismatch;
    out.index = static_cast<u32>(i);
    return Status::Ok;
  }
  if (!module_) return Status::NotLoaded;
  for (const Module::Fn& fn : module_->exports) {
    if (fn.name != name) continue;
    if (fn.signature != signature) {
      last_error_ = "SignatureMismatch: '" + fn.name + "' is exported with signature " +
                    std::to_string(fn.signature) + ", asked for as " + std::to_string(signature);
      return Status::SignatureMismatch;
    }
    out.index = static_cast<u32>(functions_.size());
    functions_.push_back(FunctionSlot{fn.name, fn.signature, fn.pointer});
    return Status::Ok;
  }
  return Status::NotFound;
}

// ---- views --------------------------------------------------------------------------------------

View ModuleHost::bind(const void* object, const TypeInfo& type) {
  u32 index;
  if (!free_slots_.empty()) {
    index = free_slots_.back();
    free_slots_.pop_back();
  } else {
    index = static_cast<u32>(slots_.size());
    slots_.push_back(Slot{});
  }
  Slot& slot = slots_[index];
  slot.object = object;
  slot.type = &type;
  slot.live = true;
  return View{index, slot.version};
}

void ModuleHost::rebind(View& view, const void* object) {
  if (view.slot >= slots_.size()) return;
  Slot& slot = slots_[view.slot];
  if (!slot.live || slot.version != view.version) return;
  slot.object = object;
  ++slot.version;
  view.version = slot.version;
}

void ModuleHost::unbind(View view) {
  if (view.slot >= slots_.size()) return;
  Slot& slot = slots_[view.slot];
  if (!slot.live || slot.version != view.version) return;
  slot.live = false;
  slot.object = nullptr;
  ++slot.version;
  free_slots_.push_back(view.slot);
}

Result ModuleHost::read(View view, FieldRef field, Value& out) noexcept {
  out = Value{};
  if (view.slot >= slots_.size() || field.index >= fields_.size()) [[unlikely]] {
    ++stats_.refused_reads;
    return Result::InvalidArgument;
  }
  const Slot& slot = slots_[view.slot];
  if (!slot.live || slot.version != view.version) [[unlikely]] {
    ++stats_.refused_reads;
    return Result::StaleView;
  }
  const Field& f = fields_[field.index];
  if (f.root != slot.type) [[unlikely]] {
    ++stats_.refused_reads;
    return Result::WrongType;
  }
  return read_value(*f.type, static_cast<const u8*>(slot.object) + f.offset, out);
}

Result ModuleHost::resolve(std::string_view type_name, std::string_view path, FieldRef& out) {
  out = FieldRef{};
  for (usize i = 0; i < fields_.size(); ++i) {
    const Field& f = fields_[static_cast<u32>(i)];
    if (f.type_name == type_name && f.path == path) {
      out.index = static_cast<u32>(i);
      return Result::Ok;
    }
  }
  const TypeInfo* root = schema::Registry::global().find(type_name);
  if (root == nullptr || root->kind != Kind::Struct) {
    pending_unresolved_.push_back("no schema struct named '" + std::string(type_name) + "'");
    return Result::NotFound;
  }
  const TypeInfo* type = root;
  const TypeRef* ref = nullptr;
  u32 offset = 0;
  std::string_view rest = path;
  while (true) {
    const usize dot = rest.find('.');
    const std::string_view segment = rest.substr(0, dot);
    const schema::FieldInfo* field = type->find_field(segment);
    if (field == nullptr) {
      pending_unresolved_.push_back(std::string(type->qualified_name) + " has no field '" +
                                    std::string(segment) + "'" +
                                    (type == root ? "" : " (in '" + std::string(path) + "')"));
      return Result::NotFound;
    }
    offset += field->offset;
    ref = &field->type;
    if (dot == std::string_view::npos) break;
    if (ref->kind != Kind::Struct || ref->type == nullptr) {
      pending_unresolved_.push_back(std::string(type->qualified_name) + "." + std::string(segment) +
                                    " is not a struct, so '" + std::string(path) +
                                    "' does not name a field");
      return Result::NotFound;
    }
    type = ref->type;
    rest = rest.substr(dot + 1);
  }
  out.index = static_cast<u32>(fields_.size());
  fields_.push_back(Field{root, ref, offset, ref->kind, std::string(type_name), std::string(path)});
  return Result::Ok;
}

bool ModuleHost::state_json(JsonValue& out) const {
  if (!module_ || module_->state_type == nullptr) return false;
  return schema::to_json(*module_->state_type, module_->state, out);
}

}  // namespace engine::hotload
