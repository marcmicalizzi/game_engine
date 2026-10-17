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

bool GameHud::empty() const noexcept {
  if (meter_count > 0) return false;
  for (const Line& l : lines) {
    if (l.set) return false;
  }
  return true;
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
  void* placements_user = nullptr;
  WorldPos feet{};  // this tick's, for `nearest_placement` and `nearest_prop`
  GameProps props;  // the module's objects (game_props.h)
  // The walker's services (ABI 3): the session they go to, and what they did.
  FlySession* session = nullptr;
  u64 walker_sets = 0;
  u64 teleports = 0;
  f32 speed_scale = 1.0f;
  f32 sprint_scale = 1.0f;
  bool sound_said = false;

  GameHud hud;
  u64 ticks = 0;
  u64 last_tick = 0;
  f64 walked_before = 0.0;

  bool ended = false;
  bool failed = false;
  std::string error;
  JsonValue result;  // end_run's object
  bool restored = false;
  u32 reloads = 0;
  u32 refused_reloads = 0;
  std::string last_refusal;
  u64 placement_queries = 0;
  u64 placement_hits = 0;

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
const GameHud& GameRunner::hud() const noexcept { return impl_->hud; }
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
  // ignored, missing ones at their defaults: the reload's rules).
  if (!config.state_file.empty() && io::exists(config.state_file)) {
    std::string text;
    JsonValue file;
    if (io::read_file(config.state_file, text) != io::Status::Ok || !parse_json(text, file).ok ||
        !file.is_object()) {
      return refuse("--game-state " + config.state_file + " is not a JSON object");
    }
    const JsonValue* format = file.find("format");
    const JsonValue* type = file.find("type");
    const JsonValue* saved = file.find("state");
    const schema::TypeInfo* state_type = host.state_type();
    if (format == nullptr || !format->is_string() || format->as_string() != k_state_format ||
        saved == nullptr) {
      return refuse("--game-state " + config.state_file + " is not an " + k_state_format + " file");
    }
    if (state_type == nullptr || type == nullptr || !type->is_string() ||
        type->as_string() != std::string_view(state_type->qualified_name)) {
      return refuse("--game-state " + config.state_file + " holds " +
                    (type != nullptr && type->is_string() ? std::string(type->as_string())
                                                          : std::string("no type")) +
                    ", and the module keeps " +
                    (state_type != nullptr ? state_type->qualified_name : "no state"));
    }
    schema::ReadContext context;
    context.options.ignore_unknown_fields = true;
    if (!schema::from_json(*state_type, host.context()->state, *saved, context) || !context.ok()) {
      return refuse("--game-state " + config.state_file + ": " +
                    (context.diagnostics.empty() ? std::string("the state did not read")
                                                 : context.diagnostics.front().path + ": " +
                                                       context.diagnostics.front().message));
    }
    // The props the last run left placed (game_props.h), back in their slots at their ids before
    // the first tick, so the module's state — which may hold those ids — means what it meant.
    if (const JsonValue* props = file.find("props"); props != nullptr) {
      std::string why;
      if (!g.props.from_json(*props, &why)) {
        return refuse("--game-state " + config.state_file + ": " + why);
      }
    }
    g.restored = true;
  }
  return true;
}

void GameRunner::set_clock(f64 sun_offset_s, f64 sun_rate, f64 ground_time_s) noexcept {
  impl_->sun_offset_s = sun_offset_s;
  impl_->sun_rate = sun_rate > 0.0 ? sun_rate : 0.0;
  impl_->ground_time_s = ground_time_s;
}

void GameRunner::set_placements(PlacementQuery query, void* user) noexcept {
  impl_->placements = query;
  impl_->placements_user = user;
}

GameProps& GameRunner::props() noexcept { return impl_->props; }
const GameProps& GameRunner::props() const noexcept { return impl_->props; }

const std::string& GameRunner::module_name() const noexcept {
  static const std::string none;
  return impl_->host ? impl_->host->module_name() : none;
}

u32 GameRunner::build() const noexcept { return impl_->host ? impl_->host->generation() : 0u; }

void GameRunner::set_session(FlySession* session) noexcept { impl_->session = session; }

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
  if (!g.host || g.ended) return;
  // The time-lapse keys at the ticks they were pressed on: a rung of the sun's ladder each, as the
  // window's `step_rate` does (time_controls.h); then the day moves by this tick at the rate now.
  // The presses arrive as running counts; anything past the count already acted on is new.
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
  t.sun_time_s = g.sun_offset_s + g.day_s;
  t.world_time_s = g.ground_time_s + t.sun_time_s;
  const f64 days = t.world_time_s / 86'400.0;
  t.day_fraction = days - std::floor(days);
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
  g.hud.clear();
  g.hud.tick = in.tick;
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
    file.set("version", static_cast<u32>(1));
    file.set("module", g.host->module_name());
    file.set("type", g.host->state_type()->qualified_name);
    file.set("tick", g.last_tick);
    file.set("state", state_json());
    file.set("props", g.props.to_json());
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
  out.set("error", g.error);
  out.set("state_file", g.config.state_file);
  out.set("restored", g.restored);
  out.set("state", state_json());
  out.set("hud", g.hud.to_json());
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
  placements.set("world", g.placements != nullptr);
  out.set("placements", std::move(placements));
  out.set("props", g.props.summary_json());
  JsonValue walker = JsonValue::object();
  walker.set("sets", g.walker_sets);
  walker.set("teleports", g.teleports);
  walker.set("speed_scale", JsonValue(g.speed_scale));
  walker.set("sprint_scale", JsonValue(g.sprint_scale));
  walker.set("session", g.session != nullptr);
  out.set("walker", std::move(walker));
  return out;
}

}  // namespace engine::view
