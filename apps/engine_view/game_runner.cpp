#include "game_runner.h"

#include "time_controls.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <foundation/hotload/module_host.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <schemas/game_tick.h>
#include <string>

namespace engine::view {

namespace {

constexpr const char* k_state_format = "engine.game.state";

void copy_text(char* out, u32 capacity, std::string_view text) noexcept {
  const usize n = text.size() < capacity - 1 ? text.size() : capacity - 1;
  if (n > 0) std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

}  // namespace

// ---- the HUD -----------------------------------------------------------------------------------

void GameHud::clear() noexcept {
  for (Line& l : lines) {
    l.set = false;
    l.text[0] = '\0';
  }
  meter_count = 0;
}

void GameHud::clear_all() noexcept {
  clear();
  element_count = 0;
}

bool GameHud::stack_empty() const noexcept {
  if (meter_count > 0) return false;
  for (const Line& l : lines) {
    if (l.set) return false;
  }
  return true;
}

bool GameHud::empty() const noexcept {
  if (!stack_empty()) return false;
  for (u32 e = 0; e < element_count; ++e) {
    if (elements[e].visible) return false;
  }
  return true;
}

const GameHud::Element* GameHud::find_element(u32 id) const noexcept {
  for (u32 e = 0; e < element_count; ++e) {
    if (elements[e].id == id) return &elements[e];
  }
  return nullptr;
}

hotload::Result GameHud::set_element(const hotload::HudElement& in, u64 said) noexcept {
  Element* slot = nullptr;
  for (u32 e = 0; e < element_count && slot == nullptr; ++e) {
    if (elements[e].id == in.id) slot = &elements[e];
  }
  if (slot == nullptr) {
    if (element_count >= k_elements) return hotload::Result::OutOfRange;
    slot = &elements[element_count++];
  }
  slot->id = in.id;
  slot->anchor = in.anchor;
  slot->align = in.align;
  slot->offset_px[0] = in.offset_px[0];
  slot->offset_px[1] = in.offset_px[1];
  slot->size_px = in.size_px;
  slot->outline_px = in.outline_px;
  std::memcpy(slot->color, in.color, 4);
  std::memcpy(slot->outline_color, in.outline_color, 4);
  slot->panel = in.panel;
  std::memcpy(slot->panel_color, in.panel_color, 4);
  slot->panel_padding_px = in.panel_padding_px;
  slot->visible = in.visible != 0;
  slot->tick = said;
  // The text, cut at a character boundary when it is longer than the slot: a UTF-8 continuation
  // byte (10xxxxxx) is never left at the end without its lead.
  usize n = in.text != nullptr ? static_cast<usize>(in.text_length) : 0u;
  if (n > k_element_chars - 1) {
    n = k_element_chars - 1;
    while (n > 0 && (static_cast<u8>(in.text[n]) & 0xC0u) == 0x80u)
      --n;
  }
  if (n > 0) std::memcpy(slot->text, in.text, n);
  slot->text[n] = '\0';
  return hotload::Result::Ok;
}

hotload::Result GameHud::remove_element(u32 id) noexcept {
  for (u32 e = 0; e < element_count; ++e) {
    if (elements[e].id != id) continue;
    for (u32 k = e + 1; k < element_count; ++k)
      elements[k - 1] = elements[k];
    --element_count;
    return hotload::Result::Ok;
  }
  return hotload::Result::NotFound;
}

const char* hud_anchor_name(hotload::HudAnchor anchor) noexcept {
  switch (anchor) {
    case hotload::HudAnchor::TopLeft: return "top_left";
    case hotload::HudAnchor::Top: return "top";
    case hotload::HudAnchor::TopRight: return "top_right";
    case hotload::HudAnchor::Left: return "left";
    case hotload::HudAnchor::Centre: return "centre";
    case hotload::HudAnchor::Right: return "right";
    case hotload::HudAnchor::BottomLeft: return "bottom_left";
    case hotload::HudAnchor::Bottom: return "bottom";
    case hotload::HudAnchor::BottomRight: return "bottom_right";
  }
  return "?";
}

const char* hud_align_name(hotload::HudAlign align) noexcept {
  switch (align) {
    case hotload::HudAlign::Left: return "left";
    case hotload::HudAlign::Centre: return "centre";
    case hotload::HudAlign::Right: return "right";
  }
  return "?";
}

JsonValue GameHud::to_json() const {
  JsonValue out = JsonValue::object();
  JsonValue text = JsonValue::array();
  for (const Line& l : lines) {
    if (l.set) text.push_back(JsonValue(std::string_view(l.text)));
  }
  out.set("lines", std::move(text));
  JsonValue meters_json = JsonValue::array();
  for (u32 m = 0; m < meter_count; ++m) {
    JsonValue meter = JsonValue::object();
    meter.set("name", std::string_view(meters[m].name));
    meter.set("value", meters[m].value);
    meter.set("max", meters[m].max);
    meters_json.push_back(std::move(meter));
  }
  out.set("meters", std::move(meters_json));
  out.set("elements", element_count);
  out.set("tick", tick);
  return out;
}

// ---- the runner --------------------------------------------------------------------------------

struct GameRunner::Impl {
  Config config;
  std::unique_ptr<hotload::ModuleHost> host;
  hotload::FunctionId game_tick;
  hotload::View view;
  game::GameTick state;  // the object the module's view reads
  const input::ActionMap* map = nullptr;
  u32 tick_hz = 240;
  f64 dt = 1.0 / 240.0;

  // The declared actions, by bit.
  struct Action {
    const char* name = nullptr;
    input::ActionId id = input::k_invalid_action;
  };
  Action actions[k_game_action_count];
  u32 action_count = 0;
  input::ActionId fast = input::k_invalid_action;

  // The sky's clock, a tick at a time.
  f64 sun_offset_s = 0.0;
  f64 sun_rate = 0.0;
  f64 ground_time_s = 0.0;
  f64 day_s = 0.0;  // the day's game seconds so far
  u32 rate_changes = 0;
  u32 last_slower = 0;  // the sun keys' presses acted on
  u32 last_faster = 0;

  PlacementQuery placements = nullptr;
  PlacementStats placement_stats = nullptr;
  void* placements_user = nullptr;
  WorldPos feet{};  // this tick's, for `nearest_placement` and `nearest_prop`
  GameProps props;  // the module's objects (game_props.h)
  // The actors (ABI 5): the scene file's names, the animated world's table once it is bound, and
  // the ground the walker stands on.
  Vector<DeclaredActor> declared_actors;
  ActorHost* actors = nullptr;
  GroundQuery ground = nullptr;
  void* ground_user = nullptr;
  bool spawn_said = false;
  u64 ground_queries = 0;
  // The walker's services (ABI 3): the session they go to, and what they did.
  FlySession* session = nullptr;
  u64 walker_sets = 0;
  u64 teleports = 0;
  f32 speed_scale = 1.0f;
  f32 sprint_scale = 1.0f;
  bool sound_said = false;

  GameHud hud;
  u64 current_tick = 0;  // the tick being run, for the elements it says
  u64 hud_sets = 0;      // hud_element calls that placed or replaced an element
  u64 hud_removes = 0;
  u64 hud_refused = 0;  // hud_element calls refused (InvalidArgument, OutOfRange)
  u64 ticks = 0;
  u64 last_tick = 0;
  f64 walked_before = 0.0;

  bool ended = false;
  bool failed = false;
  std::string error;
  JsonValue result;                        // end_run's object
  u32 runs = 1;                            // this session's runs, the first included
  JsonValue results = JsonValue::array();  // the ended runs' results, in order (`new_run`)
  bool restored = false;
  JsonValue restored_from;  // the object restored from (the file's or the header's), or null
  // The session block (state file version 2): what was restored, and what the next save writes.
  SavedSession saved;
  bool has_saved = false;
  WorldPos last_eye{};
  f32 last_yaw = 0.0f;
  f32 last_pitch = 0.0f;
  bool last_walking = false;
  bool have_pose = false;
  f64 surface_time_s = 0.0;
  bool have_surface_time = false;
  f64 time_rate = 0.0;
  // The graphics state and the requests (ABI 4).
  hotload::GraphicsState graphics;
  bool have_graphics = false;
  bool rt_available = false;
  bool hdr_available = false;
  hotload::GraphicsRequest last_request;
  u32 requests = 0;
  u32 refused_requests = 0;
  std::string last_refused_request;
  u32 reloads = 0;
  u32 refused_reloads = 0;
  std::string last_refusal;
  u64 placement_queries = 0;
  u64 placement_hits = 0;
  u64 placement_refused = 0;  // radii past k_max_placement_query_m

  // ---- the state file's session block ----
  static JsonValue session_json(const Impl& g) {
    JsonValue s = JsonValue::object();
    s.set("version", k_session_version);
    JsonValue position = JsonValue::array();
    for (const f64 v : {g.last_eye.x, g.last_eye.y, g.last_eye.z})
      position.push_back(JsonValue(v));
    s.set("position", std::move(position));
    s.set("yaw", JsonValue(g.last_yaw));
    s.set("pitch", JsonValue(g.last_pitch));
    s.set("walking", g.last_walking);
    s.set("speed_scale", JsonValue(g.speed_scale));
    s.set("sprint_scale", JsonValue(g.sprint_scale));
    JsonValue clock = JsonValue::object();
    clock.set("sun_time_s", g.sun_offset_s + g.day_s);
    clock.set("sun_offset_s", g.sun_offset_s);
    clock.set("day_s", g.day_s);
    clock.set("sun_rate", g.sun_rate);
    clock.set("ground_time_s", g.ground_time_s);
    if (g.have_surface_time) clock.set("surface_time_s", g.surface_time_s);
    clock.set("time_rate", g.time_rate);
    s.set("clock", std::move(clock));
    return s;
  }
  static bool read_session(const JsonValue& s, SavedSession& out, std::string& why) {
    u64 version = 0;
    const JsonValue* v = s.find("version");
    if (!s.is_object() || v == nullptr || !v->get_u64(version)) {
      why = "not an object with a version";
      return false;
    }
    if (version != k_session_version) {
      why = "version " + std::to_string(version) + ", and this build reads " +
            std::to_string(k_session_version);
      return false;
    }
    const auto number = [&](const JsonValue& o, const char* key, f64& to) {
      const JsonValue* f = o.find(key);
      return f != nullptr && f->get_f64(to) && std::isfinite(to);
    };
    const JsonValue* position = s.find("position");
    f64 p[3] = {0.0, 0.0, 0.0};
    if (position == nullptr || !position->is_array() || position->size() != 3) {
      why = "no position";
      return false;
    }
    for (u32 i = 0; i < 3; ++i) {
      if (!(*position)[i].get_f64(p[i]) || !std::isfinite(p[i])) {
        why = "a position that is not three finite numbers";
        return false;
      }
    }
    f64 yaw = 0.0, pitch = 0.0, speed = 1.0, sprint = 1.0;
    const JsonValue* walking = s.find("walking");
    const JsonValue* clock = s.find("clock");
    if (!number(s, "yaw", yaw) || !number(s, "pitch", pitch) || !number(s, "speed_scale", speed) ||
        !number(s, "sprint_scale", sprint) || walking == nullptr || !walking->is_bool() ||
        clock == nullptr || !clock->is_object()) {
      why = "the yaw, pitch, walking, speed scales or clock are missing";
      return false;
    }
    out.eye = WorldPos{p[0], p[1], p[2]};
    if (!world_cell_valid(out.eye)) {
      why = "a position outside the world";
      return false;
    }
    out.yaw = static_cast<f32>(yaw);
    out.pitch = static_cast<f32>(pitch);
    out.walking = walking->as_bool();
    const auto held = [](f64 x) { return static_cast<f32>(x < 0.0 ? 0.0 : x > 4.0 ? 4.0 : x); };
    out.speed_scale = held(speed);
    out.sprint_scale = held(sprint);
    if (!number(*clock, "sun_time_s", out.sun_time_s) ||
        !number(*clock, "sun_rate", out.sun_rate) ||
        !number(*clock, "ground_time_s", out.ground_time_s) ||
        !number(*clock, "time_rate", out.time_rate)) {
      why = "the clock's sun_time_s, sun_rate, ground_time_s or time_rate is missing";
      return false;
    }
    out.has_surface_time = number(*clock, "surface_time_s", out.surface_time_s);
    return true;
  }

  // ---- HostApi's game services (GameServices) ----
  static hotload::Result action(void* user, std::string_view name, u32* out_bit, std::string* why) {
    Impl& self = *static_cast<Impl*>(user);
    for (u32 a = 0; a < self.action_count; ++a) {
      if (name == self.actions[a].name) {
        *out_bit = a;
        return hotload::Result::Ok;
      }
    }
    for (const char* known : k_game_actions) {
      if (name != known) continue;
      if (self.action_count >= k_game_action_count) return hotload::Result::OutOfRange;
      Action& slot = self.actions[self.action_count];
      slot.name = known;
      slot.id = self.map != nullptr ? self.map->find_action(name) : input::k_invalid_action;
      *out_bit = self.action_count++;
      return hotload::Result::Ok;
    }
    if (why != nullptr) {
      *why = "no game action '" + std::string(name) + "' (a module may read";
      for (const char* known : k_game_actions)
        *why += std::string(" ") + known;
      *why += ")";
    }
    return hotload::Result::NotFound;
  }
  static void hud_text(void* user, u32 line, std::string_view text) {
    Impl& self = *static_cast<Impl*>(user);
    if (line >= GameHud::k_lines) return;
    GameHud::Line& l = self.hud.lines[line];
    l.set = true;
    copy_text(l.text, GameHud::k_line_chars, text);
  }
  static void hud_meter(void* user, std::string_view name, f64 value, f64 max) {
    Impl& self = *static_cast<Impl*>(user);
    // A name said again this tick updates its meter; a new one takes the next slot.
    for (u32 m = 0; m < self.hud.meter_count; ++m) {
      GameHud::Meter& meter = self.hud.meters[m];
      if (name == std::string_view(meter.name)) {
        meter.value = value;
        meter.max = max;
        return;
      }
    }
    if (self.hud.meter_count >= GameHud::k_meters) return;
    GameHud::Meter& meter = self.hud.meters[self.hud.meter_count++];
    copy_text(meter.name, GameHud::k_name_chars, name);
    meter.value = value;
    meter.max = max;
  }
  static void end_run(void* user, std::string_view json) {
    Impl& self = *static_cast<Impl*>(user);
    if (self.ended) return;  // the first word stands
    self.ended = true;
    JsonValue parsed;
    if (!parse_json(json, parsed).ok || !parsed.is_object()) {
      self.failed = true;
      self.error = "end_run: the result is not a JSON object: " + std::string(json);
      return;
    }
    self.result = std::move(parsed);
  }
  static hotload::Result nearest(void* user, std::string_view kind, f64 radius, f64* out_position,
                                 f64* out_distance) {
    Impl& self = *static_cast<Impl*>(user);
    ++self.placement_queries;
    if (radius > k_max_placement_query_m) {
      ++self.placement_refused;
      return hotload::Result::InvalidArgument;
    }
    if (self.placements == nullptr || !(radius > 0.0)) return hotload::Result::NotFound;
    WorldPos at{};
    f64 distance = 0.0;
    if (!self.placements(self.placements_user, kind, self.feet, radius, at, distance))
      return hotload::Result::NotFound;
    ++self.placement_hits;
    out_position[0] = at.x;
    out_position[1] = at.y;
    out_position[2] = at.z;
    *out_distance = distance;
    return hotload::Result::Ok;
  }

  // ---- ABI 3: a game acts on the world (game_props.h, fly_camera.h) ----
  static hotload::Result prop_mesh(void* user, std::string_view name, u32* out, std::string* why) {
    Impl& self = *static_cast<Impl*>(user);
    const hotload::Result r = self.props.mesh_of(name, *out);
    if (r == hotload::Result::NotFound && why != nullptr) {
      *why = "no prop '" + std::string(name) + "' in the scene (it declares";
      for (u32 p = 0; p < self.props.declared_count(); ++p)
        *why += " " + self.props.name_of(p);
      *why += ")";
    }
    return r;
  }
  static hotload::Result prop_place(void* user, u32 mesh, const f64* position, f32 yaw, f32 scale,
                                    u32* out_prop) {
    return static_cast<Impl*>(user)->props.place(mesh, position, yaw, scale, *out_prop);
  }
  static hotload::Result prop_move(void* user, u32 prop, const f64* position, f32 yaw) {
    return static_cast<Impl*>(user)->props.move(prop, position, yaw);
  }
  static hotload::Result prop_remove(void* user, u32 prop) {
    return static_cast<Impl*>(user)->props.remove(prop);
  }
  static hotload::Result nearest_prop(void* user, u32 mesh, f64 radius, u32* out_prop,
                                      f64* out_position, f64* out_distance) {
    Impl& self = *static_cast<Impl*>(user);
    WorldPos at{};
    const hotload::Result r =
        self.props.nearest(mesh, self.feet, radius, *out_prop, at, *out_distance);
    if (r != hotload::Result::Ok) return r;
    out_position[0] = at.x;
    out_position[1] = at.y;
    out_position[2] = at.z;
    return r;
  }
  // The scales are held to [0, 4] here and again in the walker; the summary says what stands.
  static hotload::Result walker_set(void* user, f32 speed_scale, f32 sprint_scale) {
    Impl& self = *static_cast<Impl*>(user);
    if (!std::isfinite(speed_scale) || !std::isfinite(sprint_scale))
      return hotload::Result::InvalidArgument;
    if (self.session == nullptr) return hotload::Result::Unsupported;
    const auto held = [](f32 s) { return s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s; };
    self.speed_scale = held(speed_scale);
    self.sprint_scale = held(sprint_scale);
    self.session->set_walk_scale(self.speed_scale, self.sprint_scale);
    ++self.walker_sets;
    return hotload::Result::Ok;
  }
  static hotload::Result walker_teleport(void* user, const f64* position, f32 heading, f32 pitch) {
    Impl& self = *static_cast<Impl*>(user);
    const WorldPos at{position[0], position[1], position[2]};
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) ||
        !std::isfinite(heading) || !std::isfinite(pitch) || !world_cell_valid(at)) {
      return hotload::Result::InvalidArgument;
    }
    if (self.session == nullptr) return hotload::Result::Unsupported;
    self.session->request_teleport(at, heading, pitch);
    ++self.teleports;
    return hotload::Result::Ok;
  }
  // ---- ABI 4: the HUD's structured elements (GameHud) ----
  // The judgement is here and not in the HUD: a reserved id, an enum out of range, a length that is
  // not a finite size, or text the length overruns is `InvalidArgument`, said once on stderr with
  // the first such call's reason, so a module's mistake is seen without a line every tick.
  static hotload::Result hud_element(void* user, const hotload::HudElement& e) {
    Impl& self = *static_cast<Impl*>(user);
    const char* why = nullptr;
    const auto finite = [](f32 v) { return std::isfinite(v); };
    if (e.id >= hotload::k_hud_reserved) {
      why = "an id from k_hud_reserved up is the host's (hud_text's and hud_meter's)";
    } else if (static_cast<u32>(e.anchor) > static_cast<u32>(hotload::HudAnchor::BottomRight) ||
               static_cast<u32>(e.align) > static_cast<u32>(hotload::HudAlign::Right) ||
               static_cast<u32>(e.panel) > static_cast<u32>(hotload::HudPanel::Solid)) {
      why = "an anchor, an alignment or a panel outside its enum";
    } else if (!finite(e.offset_px[0]) || !finite(e.offset_px[1]) || !finite(e.size_px) ||
               !finite(e.outline_px) || !finite(e.panel_padding_px) || !(e.size_px > 0.0f) ||
               e.outline_px < 0.0f || e.panel_padding_px < 0.0f) {
      why =
          "a length that is not finite, a size at or below zero, or a negative outline or padding";
    } else if (e.text == nullptr && e.text_length > 0) {
      why = "a text length with no text";
    } else {
      for (u64 i = 0; i < e.text_length && why == nullptr; ++i) {
        if (e.text[i] == '\0') why = "text shorter than its length says (a NUL inside it)";
      }
    }
    if (why != nullptr) {
      ++self.hud_refused;
      if (self.hud_refused == 1) {
        std::fprintf(stderr, "engine-view: game: hud_element %u refused: %s\n", e.id, why);
      }
      return hotload::Result::InvalidArgument;
    }
    const hotload::Result r = self.hud.set_element(e, self.current_tick);
    if (r == hotload::Result::Ok) {
      ++self.hud_sets;
    } else {
      ++self.hud_refused;
    }
    return r;
  }
  static hotload::Result hud_remove(void* user, u32 id) {
    Impl& self = *static_cast<Impl*>(user);
    if (id >= hotload::k_hud_reserved) return hotload::Result::InvalidArgument;
    const hotload::Result r = self.hud.remove_element(id);
    if (r == hotload::Result::Ok) ++self.hud_removes;
    return r;
  }

  // ---- ABI 4: the graphics state, read-only, and requests recorded ----
  static hotload::Result graphics_state(void* user, hotload::GraphicsState& out) {
    Impl& self = *static_cast<Impl*>(user);
    if (!self.have_graphics) return hotload::Result::Unsupported;  // no device yet (on_load)
    out = self.graphics;
    return hotload::Result::Ok;
  }
  static hotload::Result request_graphics(void* user, const hotload::GraphicsRequest& r) {
    Impl& self = *static_cast<Impl*>(user);
    // A name is one of the host's words or empty (leave it), NUL-terminated inside its array.
    const auto name = [](const char (&field)[24], std::string_view& out) {
      for (u32 i = 0; i < 24; ++i) {
        if (field[i] == '\0') {
          out = std::string_view(field, i);
          return true;
        }
      }
      return false;
    };
    std::string_view raster, shadows, present, display;
    if (!name(r.raster, raster) || !name(r.shadows, shadows) || !name(r.present, present) ||
        !name(r.display, display) || r.width > 16384 || r.height > 16384) {
      return hotload::Result::InvalidArgument;
    }
    const auto one_of = [](std::string_view v, std::initializer_list<std::string_view> words) {
      if (v.empty()) return true;
      for (const std::string_view w : words)
        if (v == w) return true;
      return false;
    };
    const char* why = nullptr;
    if (!one_of(raster, {"direct", "hw", "sw", "auto", "vertex", "rt"})) {
      why = "a raster path engine-view does not have";
    } else if (!one_of(shadows, {"auto", "off", "rt", "csm"})) {
      why = "a shadow mode engine-view does not have";
    } else if (!one_of(present, {"auto", "fifo", "fifo-relaxed", "mailbox", "immediate",
                                 "fifo-latest-ready"})) {
      why = "a present mode engine-view does not have";
    } else if (!one_of(display, {"sdr", "hdr10", "scrgb"})) {
      why = "a display encoding engine-view does not have";
    } else if (!self.rt_available && (raster == "rt" || shadows == "rt")) {
      why = "ray tracing, which this device cannot do";
    } else if (!self.hdr_available && (display == "hdr10" || display == "scrgb")) {
      why = "HDR output, which this session's display cannot show";
    }
    if (why != nullptr) {
      ++self.refused_requests;
      self.last_refused_request = why;
      return hotload::Result::Unsupported;
    }
    self.last_request = r;
    ++self.requests;
    return hotload::Result::Ok;
  }

  // ---- ABI 5: actors (game_props.h `ActorHost`) and the ground ----
  // The runner judges the handle — a declared actor's scene instance, or `NotFound` — and the
  // animated world everything else, so a handle the module made up never reaches the world.
  bool is_actor(u32 instance) const noexcept {
    for (const DeclaredActor& a : declared_actors) {
      if (a.instance == instance) return true;
    }
    return false;
  }
  static hotload::Result actor_find(void* user, std::string_view name, u32* out, std::string* why) {
    Impl& self = *static_cast<Impl*>(user);
    if (self.declared_actors.empty()) return hotload::Result::Unsupported;
    for (const DeclaredActor& a : self.declared_actors) {
      if (name == a.name) {
        *out = a.instance;
        return hotload::Result::Ok;
      }
    }
    if (why != nullptr) {
      *why = "no actor '" + std::string(name) + "' in the scene (it names";
      for (const DeclaredActor& a : self.declared_actors)
        *why += " " + a.name;
      *why += ")";
    }
    return hotload::Result::NotFound;
  }
  // Spawning: not built (apps.md "Actors", what it needs). `Unsupported`, said once.
  static hotload::Result actor_spawn(void* user, std::string_view, const f64*, f32, u32*) {
    Impl& self = *static_cast<Impl*>(user);
    if (!self.spawn_said) {
      self.spawn_said = true;
      std::fprintf(stderr,
                   "engine-view: game: the module spawns an actor, which engine-view cannot draw "
                   "yet (a deformed instance in the renderer's dynamic tail, roadmap R84); "
                   "actor_spawn hears Unsupported\n");
    }
    return hotload::Result::Unsupported;
  }
  // The handle first, then whether there is a world to act on (`on_load`, or a build without one).
  hotload::Result actor_host(u32 actor, ActorHost*& out) const noexcept {
    if (!is_actor(actor)) return hotload::Result::NotFound;
    if (actors == nullptr) return hotload::Result::Unsupported;
    out = actors;
    return hotload::Result::Ok;
  }
  static hotload::Result actor_place(void* user, u32 actor, const f64* position, f32 yaw) {
    ActorHost* host = nullptr;
    const hotload::Result r = static_cast<Impl*>(user)->actor_host(actor, host);
    return r == hotload::Result::Ok ? host->place(actor, position, yaw) : r;
  }
  static hotload::Result actor_play(void* user, u32 actor, std::string_view clip, f32 fade_s,
                                    bool looping) {
    ActorHost* host = nullptr;
    const hotload::Result r = static_cast<Impl*>(user)->actor_host(actor, host);
    return r == hotload::Result::Ok ? host->play(actor, clip, fade_s, looping) : r;
  }
  static hotload::Result actor_stop(void* user, u32 actor, f32 fade_s) {
    ActorHost* host = nullptr;
    const hotload::Result r = static_cast<Impl*>(user)->actor_host(actor, host);
    return r == hotload::Result::Ok ? host->stop(actor, fade_s) : r;
  }
  static hotload::Result actor_speed(void* user, u32 actor, f32 speed) {
    ActorHost* host = nullptr;
    const hotload::Result r = static_cast<Impl*>(user)->actor_host(actor, host);
    return r == hotload::Result::Ok ? host->speed(actor, speed) : r;
  }
  static hotload::Result actor_state(void* user, u32 actor, hotload::ActorState& out) {
    ActorHost* host = nullptr;
    const hotload::Result r = static_cast<Impl*>(user)->actor_host(actor, host);
    return r == hotload::Result::Ok ? host->state(actor, out) : r;
  }
  static hotload::Result ground_height(void* user, f64 x, f64 z, f64* out) {
    Impl& self = *static_cast<Impl*>(user);
    if (!std::isfinite(x) || !std::isfinite(z)) return hotload::Result::InvalidArgument;
    if (self.ground == nullptr) return hotload::Result::Unsupported;
    ++self.ground_queries;
    return self.ground(self.ground_user, x, z, *out) ? hotload::Result::Ok
                                                     : hotload::Result::Unsupported;
  }

  // Sound: not built yet (roadmap P43, the third stage). `Unsupported`, said once.
  static hotload::Result sound_clip(void* user, std::string_view, u32*, std::string*) {
    Impl& self = *static_cast<Impl*>(user);
    if (!self.sound_said) {
      self.sound_said = true;
      std::fprintf(stderr,
                   "engine-view: game: the module asks for sound, which engine-view does not "
                   "play yet (roadmap P43); its sound calls hear Unsupported\n");
    }
    return hotload::Result::Unsupported;
  }
};

GameRunner::GameRunner() : impl_(std::make_unique<Impl>()) {}
GameRunner::~GameRunner() {
  if (impl_->host) impl_->host->unload();
}

bool GameRunner::started() const noexcept { return impl_->host != nullptr; }
bool GameRunner::ended() const noexcept { return impl_->ended; }
bool GameRunner::failed() const noexcept { return impl_->failed; }
const std::string& GameRunner::error() const noexcept { return impl_->error; }
const JsonValue& GameRunner::result() const noexcept { return impl_->result; }
u32 GameRunner::runs() const noexcept { return impl_->runs; }
const GameHud& GameRunner::hud() const noexcept { return impl_->hud; }

bool GameRunner::new_run() {
  Impl& g = *impl_;
  if (!g.host || !g.ended || g.failed) return false;
  if (!g.host->reset_state()) return false;
  g.results.push_back(std::move(g.result));
  g.result = JsonValue();
  g.ended = false;
  g.hud.clear_all();  // the next tick says the new run's, its elements too
  ++g.runs;
  return true;
}
u64 GameRunner::ticks() const noexcept { return impl_->ticks; }

bool GameRunner::start(const Config& config, const input::ActionMap& map, u32 tick_hz,
                       std::string* error) {
  Impl& g = *impl_;
  g.config = config;
  g.map = &map;
  g.tick_hz = tick_hz > 0 ? tick_hz : 240u;
  g.dt = 1.0 / static_cast<f64>(g.tick_hz);
  g.fast = map.find_action("fast");

  hotload::HostConfig host_config;
  host_config.shadow_directory = config.shadow_directory;
  host_config.fixed_step = g.dt;
  g.host = std::make_unique<hotload::ModuleHost>(std::move(host_config));
  hotload::ModuleHost& host = *g.host;
  hotload::GameServices services;
  services.user = &g;
  services.action = &Impl::action;
  services.hud_text = &Impl::hud_text;
  services.hud_meter = &Impl::hud_meter;
  services.end_run = &Impl::end_run;
  services.nearest_placement = &Impl::nearest;
  services.prop_mesh = &Impl::prop_mesh;
  services.prop_place = &Impl::prop_place;
  services.prop_move = &Impl::prop_move;
  services.prop_remove = &Impl::prop_remove;
  services.nearest_prop = &Impl::nearest_prop;
  services.walker_set = &Impl::walker_set;
  services.walker_teleport = &Impl::walker_teleport;
  services.sound_clip = &Impl::sound_clip;  // sound_play and sound_stop: null, Unsupported
  services.hud_element = &Impl::hud_element;
  services.hud_remove = &Impl::hud_remove;
  services.graphics_state = &Impl::graphics_state;
  services.request_graphics = &Impl::request_graphics;
  services.actor_find = &Impl::actor_find;
  services.actor_spawn = &Impl::actor_spawn;
  services.actor_place = &Impl::actor_place;
  services.actor_play = &Impl::actor_play;
  services.actor_stop = &Impl::actor_stop;
  services.actor_speed = &Impl::actor_speed;
  services.actor_state = &Impl::actor_state;
  services.ground_height = &Impl::ground_height;
  host.set_game_services(services);

  const auto refuse = [&](const std::string& why) {
    if (error != nullptr) *error = why;
    host.unload();
    g.host.reset();
    return false;
  };
  const hotload::Status status = host.load(config.module);
  if (status != hotload::Status::Ok) {
    return refuse("the module host refused " + config.module + " (" + hotload::status_name(status) +
                  "): " + host.last_error());
  }
  if (host.find<hotload::TickFn>("game_tick", g.game_tick) != hotload::Status::Ok) {
    return refuse(config.module +
                  " exports no game_tick (a TickFn: void(ModuleContext*, View, "
                  "f32)), which is what the runner calls every tick");
  }
  g.view = host.bind(g.state);

  // --game-state: the state as the last run left it, through the descriptor (unknown fields
  // ignored, missing ones at their defaults: the reload's rules). A replay of a `--game` recording
  // restores what its header says the live session restored, and never the file.
  JsonValue file;
  bool have_file = false;
  std::string source = "--game-state " + config.state_file;
  if (config.restore_from_header) {
    source = "the recording's restored state";
    if (config.restore != nullptr && config.restore->is_object()) {
      file = *config.restore;
      have_file = true;
    }
    if (!config.state_file.empty() && io::exists(config.state_file)) {
      std::fprintf(stderr,
                   "engine-view: game: replaying a --game recording restores what the recording "
                   "restored, not %s (which is written at the end)\n",
                   config.state_file.c_str());
    }
  } else if (!config.state_file.empty() && io::exists(config.state_file)) {
    std::string text;
    if (io::read_file(config.state_file, text) != io::Status::Ok || !parse_json(text, file).ok ||
        !file.is_object()) {
      return refuse("--game-state " + config.state_file + " is not a JSON object");
    }
    have_file = true;
  }
  if (have_file) {
    const JsonValue* format = file.find("format");
    const JsonValue* type = file.find("type");
    const JsonValue* saved = file.find("state");
    const schema::TypeInfo* state_type = host.state_type();
    if (format == nullptr || !format->is_string() || format->as_string() != k_state_format ||
        saved == nullptr) {
      return refuse(source + " is not an " + k_state_format + " file");
    }
    if (state_type == nullptr || type == nullptr || !type->is_string() ||
        type->as_string() != std::string_view(state_type->qualified_name)) {
      return refuse(source + " holds " +
                    (type != nullptr && type->is_string() ? std::string(type->as_string())
                                                          : std::string("no type")) +
                    ", and the module keeps " +
                    (state_type != nullptr ? state_type->qualified_name : "no state"));
    }
    schema::ReadContext context;
    context.options.ignore_unknown_fields = true;
    if (!schema::from_json(*state_type, host.context()->state, *saved, context) || !context.ok()) {
      return refuse(source + ": " +
                    (context.diagnostics.empty() ? std::string("the state did not read")
                                                 : context.diagnostics.front().path + ": " +
                                                       context.diagnostics.front().message));
    }
    // The props the last run left placed (game_props.h), back in their slots at their ids before
    // the first tick, so the module's state — which may hold those ids — means what it meant.
    if (const JsonValue* props = file.find("props"); props != nullptr) {
      std::string why;
      if (!g.props.from_json(*props, &why)) {
        return refuse(source + ": " + why);
      }
    }
    // The actors (ABI 5) are applied by the animated world once it is bound (`restored_actors`);
    // here only their names, so a file naming an actor the scene does not is refused before any
    // device, as a prop is.
    if (const JsonValue* actors = file.find("actors"); actors != nullptr) {
      if (!actors->is_array()) return refuse(source + ": actors: not an array");
      for (u32 i = 0; i < actors->size(); ++i) {
        const JsonValue* name = (*actors)[i].find("name");
        if (name == nullptr || !name->is_string()) {
          return refuse(source + ": actors[" + std::to_string(i) + "] has no name");
        }
        bool declared = false;
        for (const DeclaredActor& a : g.declared_actors)
          declared = declared || a.name == name->as_string();
        if (!declared) {
          return refuse(source + ": actors[" + std::to_string(i) + "] is '" +
                        std::string(name->as_string()) + "', which the scene does not name");
        }
      }
    }
    // The session (version 2): the walker's pose and scales and the clock. A version 1 file has
    // none, and its session starts where the flags say; a block of a later version is refused
    // rather than half read.
    if (const JsonValue* session = file.find("session"); session != nullptr) {
      std::string why;
      if (!Impl::read_session(*session, g.saved, why)) return refuse(source + ": session: " + why);
      g.has_saved = true;
    }
    g.restored_from = file;
    g.restored = true;
  }
  return true;
}

void GameRunner::set_clock(f64 sun_offset_s, f64 sun_rate, f64 ground_time_s) noexcept {
  Impl& g = *impl_;
  g.sun_offset_s = sun_offset_s;
  g.sun_rate = sun_rate > 0.0 ? sun_rate : 0.0;
  g.ground_time_s = ground_time_s;
  // A restored clock: the sky where it stood whatever this session's `--time-of-day` offset is
  // (the day's seconds are what is left of the saved sky time past it), at the rate the keys had
  // left it, on the ground time the world's clock stood on.
  if (g.has_saved) {
    g.day_s = g.saved.sun_time_s - sun_offset_s;
    g.sun_rate = g.saved.sun_rate > 0.0 ? g.saved.sun_rate : 0.0;
    g.ground_time_s = g.saved.ground_time_s;
  }
}

const GameRunner::SavedSession* GameRunner::restored_session() const noexcept {
  return impl_->has_saved ? &impl_->saved : nullptr;
}

const JsonValue& GameRunner::restored_json() const noexcept { return impl_->restored_from; }

void GameRunner::apply_pose(const SavedSession& saved, SessionHeader& header) noexcept {
  header.start.position = saved.eye;
  header.start.yaw = saved.yaw;
  header.start.pitch = saved.pitch;
  header.walking = saved.walking;
}

void GameRunner::note_surface_time(f64 surface_time_s) noexcept {
  impl_->surface_time_s = surface_time_s;
  impl_->have_surface_time = true;
}

void GameRunner::set_time_rate(f64 time_rate) noexcept { impl_->time_rate = time_rate; }

void GameRunner::set_graphics(const hotload::GraphicsState& state, bool ray_tracing_available,
                              bool hdr_available) noexcept {
  Impl& g = *impl_;
  g.graphics = state;
  g.graphics.size = sizeof(hotload::GraphicsState);
  g.have_graphics = true;
  g.rt_available = ray_tracing_available;
  g.hdr_available = hdr_available;
}

u32 GameRunner::graphics_requests() const noexcept { return impl_->requests; }

void GameRunner::set_graphics_display(std::string_view name, bool hdr) noexcept {
  Impl& g = *impl_;
  if (!g.have_graphics) return;
  copy_text(g.graphics.display, sizeof(g.graphics.display), name);
  g.graphics.hdr = hdr ? 1u : 0u;
  g.hdr_available = hdr;
}

f64 GameRunner::sun_time_s() const noexcept { return impl_->sun_offset_s + impl_->day_s; }
f64 GameRunner::day_s() const noexcept { return impl_->day_s; }
f64 GameRunner::sun_rate() const noexcept { return impl_->sun_rate; }
u32 GameRunner::rate_changes() const noexcept { return impl_->rate_changes; }

void GameRunner::set_placements(PlacementQuery query, void* user, PlacementStats stats) noexcept {
  impl_->placements = query;
  impl_->placements_user = user;
  impl_->placement_stats = stats;
}

GameProps& GameRunner::props() noexcept { return impl_->props; }
const GameProps& GameRunner::props() const noexcept { return impl_->props; }

void GameRunner::declare_actors(const Vector<DeclaredActor>& actors) {
  impl_->declared_actors = actors;
}
const Vector<DeclaredActor>& GameRunner::declared_actors() const noexcept {
  return impl_->declared_actors;
}
void GameRunner::set_actors(ActorHost* actors) noexcept { impl_->actors = actors; }
const JsonValue* GameRunner::restored_actors() const noexcept {
  const Impl& g = *impl_;
  if (!g.restored || !g.restored_from.is_object()) return nullptr;
  return g.restored_from.find("actors");
}
void GameRunner::set_ground(GroundQuery query, void* user) noexcept {
  impl_->ground = query;
  impl_->ground_user = user;
}

const std::string& GameRunner::module_name() const noexcept {
  static const std::string none;
  return impl_->host ? impl_->host->module_name() : none;
}

u32 GameRunner::build() const noexcept { return impl_->host ? impl_->host->generation() : 0u; }

void GameRunner::set_session(FlySession* session) noexcept {
  Impl& g = *impl_;
  g.session = session;
  // The restored speed scales, on the walker before the session's first tick, in a live session
  // and its replay alike (both restore the same block), so the first tick walks as the saved run's
  // last did. 1 and 1 — every session that restored nothing — leaves the walker untouched.
  if (session != nullptr && g.has_saved &&
      (g.saved.speed_scale != 1.0f || g.saved.sprint_scale != 1.0f)) {
    g.speed_scale = g.saved.speed_scale;
    g.sprint_scale = g.saved.sprint_scale;
    session->set_walk_scale(g.speed_scale, g.sprint_scale);
  }
}

void GameRunner::observe(void* user, const FlySession& session, const input::InputState& input) {
  GameRunner& self = *static_cast<GameRunner*>(user);
  Impl& g = *self.impl_;
  GameTickInput in;
  in.tick = session.tick().value;
  in.eye = session.state().position;
  in.feet = in.eye;
  in.heading = session.state().yaw;
  in.pitch = session.state().pitch;
  in.walking = session.walking();
  in.input = &input;
  const Walker* walker = session.walker();
  if (walker != nullptr && session.walking()) {
    in.feet = walker->feet();
    in.on_ground = walker->on_ground();
    in.sprinting = g.fast != input::k_invalid_action && input.held(g.fast);
    const f64 walked = walker->stats().distance_m;
    in.moved_m = walked - g.walked_before;
    g.walked_before = walked;
  } else if (walker != nullptr) {
    g.walked_before = walker->stats().distance_m;
  }
  in.sun_slower = session.control_presses(ViewControl::sun_slower);
  in.sun_faster = session.control_presses(ViewControl::sun_faster);
  self.tick(in);
}

void GameRunner::tick(const GameTickInput& in) {
  Impl& g = *impl_;
  if (!g.host) return;
  // The time-lapse keys at the ticks they were pressed on: a rung of the sun's ladder each, as the
  // window's `step_rate` does (time_controls.h); then the day moves by this tick at the rate now.
  // The presses arrive as running counts; anything past the count already acted on is new. The
  // clock steps whether or not the run has ended: the frame draws the sky from it (`sun_time_s`),
  // and a result panel's sky keeps turning; only the module is not ticked past `end_run`.
  const u32 slower = in.sun_slower;
  const u32 faster = in.sun_faster;
  while (g.last_slower < slower) {
    ++g.last_slower;
    g.sun_rate = ladder_step(k_sun_ladder, g.sun_rate, false);
    ++g.rate_changes;
  }
  while (g.last_faster < faster) {
    ++g.last_faster;
    g.sun_rate = ladder_step(k_sun_ladder, g.sun_rate, true);
    ++g.rate_changes;
  }
  g.day_s += g.sun_rate * g.dt;

  game::GameTick& t = g.state;
  // The clock's fields first, so the summary's `clock` is the frame's whether or not the module
  // is ticked below.
  t.sun_time_s = g.sun_offset_s + g.day_s;
  t.world_time_s = g.ground_time_s + t.sun_time_s;
  const f64 days = t.world_time_s / 86'400.0;
  t.day_fraction = days - std::floor(days);
  // The pose the state file keeps (`SavedSession`), the tick's whether or not the module runs.
  g.last_eye = in.eye;
  g.last_yaw = in.heading;
  g.last_pitch = in.pitch;
  g.last_walking = in.walking;
  g.have_pose = true;
  if (g.ended) return;  // the run is over: the module said so, or failed
  t.tick = in.tick;
  t.dt = g.dt;
  t.eye = DVec3{in.eye.x, in.eye.y, in.eye.z};
  t.feet = DVec3{in.feet.x, in.feet.y, in.feet.z};
  t.heading = in.heading;
  t.pitch = in.pitch;
  t.walking = in.walking;
  t.on_ground = in.on_ground;
  t.sprinting = in.sprinting;
  t.moved_m = in.moved_m;
  u64 pressed = 0;
  u64 held = 0;
  u64 released = 0;
  if (in.input != nullptr) {
    for (u32 a = 0; a < g.action_count; ++a) {
      const input::ActionId id = g.actions[a].id;
      if (id == input::k_invalid_action) continue;
      const u64 bit = u64{1} << a;
      if (in.input->pressed(id)) pressed |= bit;
      if (in.input->held(id)) held |= bit;
      if (in.input->released(id)) released |= bit;
    }
  }
  t.actions_pressed = pressed;
  t.actions_held = held;
  t.actions_released = released;
  g.feet = in.feet;

  // This tick's HUD is the module's to say again.
  g.hud.clear();  // the lines and meters; the elements are retained until the module removes them
  g.hud.tick = in.tick;
  g.current_tick = in.tick;
  hotload::ModuleHost& host = *g.host;
  host.set_time(in.tick, g.dt);
  host.function<hotload::TickFn>(g.game_tick)(host.context(), g.view, static_cast<f32>(g.dt));
  ++g.ticks;
  g.last_tick = in.tick;
  std::string failure = host.take_failure();
  if (!failure.empty() && !g.failed) {
    g.failed = true;
    g.ended = true;
    g.error = "game_tick failed at tick " + std::to_string(in.tick) + ": " + failure;
  }
}

u32 GameRunner::poll() {
  Impl& g = *impl_;
  if (!g.host) return 0;
  const hotload::HostStats stats_before = g.host->stats();
  const u32 swapped = g.host->poll_changes();
  if (swapped != 0) {
    ++g.reloads;
    const hotload::ReloadReport& r = g.host->last_report();
    std::string added;
    for (const std::string& f : r.added)
      added += (added.empty() ? "" : ", ") + f;
    std::fprintf(stderr, "engine-view: game: %s rebuilt and swapped in (build %u%s%s)\n",
                 g.host->module_name().c_str(), r.generation, added.empty() ? "" : "; added ",
                 added.c_str());
    return 1;
  }
  if (g.host->stats().failed_loads != stats_before.failed_loads) {
    ++g.refused_reloads;
    g.last_refusal = g.host->last_error();
    std::fprintf(stderr, "engine-view: game: a rebuild was refused, the old build runs on: %s\n",
                 g.last_refusal.c_str());
  }
  return 0;
}

JsonValue GameRunner::state_json() const {
  JsonValue out;
  if (impl_->host) (void)impl_->host->state_json(out);
  return out;
}

bool GameRunner::finish(std::string* error) {
  Impl& g = *impl_;
  if (!g.host) return true;
  bool ok = true;
  if (!g.config.state_file.empty() && g.host->state_type() != nullptr) {
    JsonValue file = JsonValue::object();
    file.set("format", k_state_format);
    file.set("version", k_state_version);
    file.set("module", g.host->module_name());
    file.set("type", g.host->state_type()->qualified_name);
    file.set("tick", g.last_tick);
    file.set("state", state_json());
    file.set("props", g.props.to_json());
    // The actors (ABI 5): each one's placement and player as the last tick left them, for a cold
    // restore; a session whose world never bound keeps what it restored, as the session block does.
    if (g.actors != nullptr) {
      file.set("actors", g.actors->to_json());
    } else if (const JsonValue* restored =
                   g.restored_from.is_object() ? g.restored_from.find("actors") : nullptr;
               restored != nullptr) {
      file.set("actors", *restored);
    }
    // The session (version 2): only once a tick has said where the walker is, so a run that never
    // ticked does not save a pose of zeros over a good one.
    if (g.have_pose) {
      file.set("session", Impl::session_json(g));
    } else if (g.has_saved && g.restored_from.find("session") != nullptr) {
      file.set("session", *g.restored_from.find("session"));
    }
    const io::Status status = io::write_file(
        g.config.state_file, write_json(file, JsonWriteOptions{.pretty = true}) + "\n");
    if (status != io::Status::Ok) {
      ok = false;
      if (error != nullptr) {
        *error = "cannot write --game-state " + g.config.state_file + ": " +
                 std::string(io::status_name(status));
      }
    }
  }
  return ok;
}

JsonValue GameRunner::summary_json() const {
  const Impl& g = *impl_;
  JsonValue out = JsonValue::object();
  out.set("module", g.config.module);
  if (!g.host) {
    out.set("loaded", false);
    out.set("error", g.error);
    return out;
  }
  out.set("loaded", true);
  out.set("name", g.host->module_name());
  out.set("build", g.host->generation());
  out.set("reloads", g.reloads);
  out.set("refused_reloads", g.refused_reloads);
  out.set("last_refusal", g.last_refusal);
  out.set("ticks", g.ticks);
  out.set("last_tick", g.last_tick);
  JsonValue actions = JsonValue::array();
  for (u32 a = 0; a < g.action_count; ++a)
    actions.push_back(JsonValue(g.actions[a].name));
  out.set("actions", std::move(actions));
  out.set("ended", g.ended);
  out.set("result", g.result);
  out.set("runs", g.runs);
  out.set("results", g.results);
  out.set("error", g.error);
  out.set("state_file", g.config.state_file);
  out.set("restored", g.restored);
  out.set("restored_session", g.has_saved);
  // What the next save writes as the session block (state file version 2).
  if (g.have_pose) out.set("session", Impl::session_json(g));
  out.set("state", state_json());
  JsonValue hud = g.hud.to_json();
  hud.set("element_sets", g.hud_sets);
  hud.set("element_removes", g.hud_removes);
  hud.set("element_refused", g.hud_refused);
  out.set("hud", std::move(hud));
  out.set("abi", g.host->module_abi());
  JsonValue clock = JsonValue::object();
  clock.set("sun_time_s", g.state.sun_time_s);
  clock.set("world_time_s", g.state.world_time_s);
  clock.set("day_fraction", g.state.day_fraction);
  clock.set("sun_rate", g.sun_rate);
  clock.set("rate_changes", g.rate_changes);
  out.set("clock", std::move(clock));
  JsonValue placements = JsonValue::object();
  placements.set("queries", g.placement_queries);
  placements.set("found", g.placement_hits);
  placements.set("refused", g.placement_refused);
  placements.set("max_radius_m", k_max_placement_query_m);
  u64 generated = 0;
  f64 generate_ms = 0.0;
  if (g.placement_stats != nullptr) g.placement_stats(g.placements_user, generated, generate_ms);
  placements.set("generated", generated);
  placements.set("generate_ms", generate_ms);
  placements.set("world", g.placements != nullptr);
  out.set("placements", std::move(placements));
  out.set("props", g.props.summary_json());
  // The actors (ABI 5): every bound actor where it stands, its clip, playhead and pose hash, and
  // its path sampled a quarter-second at a time; and what the module called. Without a bound
  // world, the names the scene declared.
  if (g.actors != nullptr) {
    out.set("actors", g.actors->summary_json());
    JsonValue calls = g.actors->calls_json();
    calls.set("ground_queries", g.ground_queries);
    out.set("actor_calls", std::move(calls));
  } else {
    JsonValue names = JsonValue::array();
    for (const DeclaredActor& a : g.declared_actors) {
      JsonValue actor = JsonValue::object();
      actor.set("name", a.name);
      actor.set("instance", a.instance);
      names.push_back(std::move(actor));
    }
    out.set("actors", std::move(names));
    JsonValue calls = JsonValue::object();
    calls.set("bound", false);
    calls.set("ground_queries", g.ground_queries);
    out.set("actor_calls", std::move(calls));
  }
  out.set("ground", g.ground != nullptr);
  JsonValue walker = JsonValue::object();
  walker.set("sets", g.walker_sets);
  walker.set("teleports", g.teleports);
  walker.set("speed_scale", JsonValue(g.speed_scale));
  walker.set("sprint_scale", JsonValue(g.sprint_scale));
  walker.set("session", g.session != nullptr);
  out.set("walker", std::move(walker));
  // The graphics a module reads (ABI 4) and what it asked for: recorded, applied by nothing yet.
  JsonValue graphics = JsonValue::object();
  graphics.set("known", g.have_graphics);
  if (g.have_graphics) {
    const hotload::GraphicsState& s = g.graphics;
    graphics.set("raster", std::string_view(s.raster));
    graphics.set("shadows", std::string_view(s.shadows));
    graphics.set("present", std::string_view(s.present));
    graphics.set("display", std::string_view(s.display));
    graphics.set("tier", std::string_view(s.tier));
    graphics.set("width", s.width);
    graphics.set("height", s.height);
    graphics.set("ray_tracing", s.ray_tracing != 0);
    graphics.set("hdr", s.hdr != 0);
    JsonValue reasons = JsonValue::array();
    for (u32 i = 0; i < s.reason_count && i < 4; ++i)
      reasons.push_back(JsonValue(std::string_view(s.reasons[i])));
    graphics.set("reasons", std::move(reasons));
  }
  graphics.set("requests", g.requests);
  graphics.set("refused_requests", g.refused_requests);
  graphics.set("last_refused", g.last_refused_request);
  if (g.requests > 0) {
    const hotload::GraphicsRequest& r = g.last_request;
    JsonValue pending = JsonValue::object();
    pending.set("raster", std::string_view(r.raster));
    pending.set("shadows", std::string_view(r.shadows));
    pending.set("present", std::string_view(r.present));
    pending.set("display", std::string_view(r.display));
    pending.set("width", r.width);
    pending.set("height", r.height);
    graphics.set("pending", std::move(pending));
  }
  out.set("graphics", std::move(graphics));
  return out;
}

}  // namespace engine::view
