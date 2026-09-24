#include "fly_camera.h"

#include <core/hash/hash.h>
#include <foundation/input/input_log.h>

#include <bit>
#include <cmath>
#include <cstdio>

namespace engine::view {

namespace {

// SDL scancodes, the numbering a key binding's code is in (docs/subsystems/input.md).
constexpr u32 k_key_a = 4;
constexpr u32 k_key_d = 7;
constexpr u32 k_key_e = 8;
constexpr u32 k_key_m = 16;
constexpr u32 k_key_q = 20;
constexpr u32 k_key_s = 22;
constexpr u32 k_key_w = 26;
constexpr u32 k_key_escape = 41;
constexpr u32 k_key_space = 44;
constexpr u32 k_key_lctrl = 224;
constexpr u32 k_key_lshift = 225;
constexpr u32 k_key_lalt = 226;
constexpr u32 k_key_rshift = 229;
constexpr u32 k_key_ralt = 230;
// window::GamepadAxis and window::GamepadButton, spelled as numbers because this file is the
// camera and not the window: the map is data, and these are its values.
constexpr u32 k_pad_left_x = 0;
constexpr u32 k_pad_left_y = 1;
constexpr u32 k_pad_right_x = 2;
constexpr u32 k_pad_right_y = 3;
constexpr u32 k_pad_left_trigger = 4;
constexpr u32 k_pad_right_trigger = 5;
constexpr u32 k_pad_north = 4;
constexpr u32 k_pad_back = 5;
constexpr u32 k_pad_left_stick = 8;
constexpr u32 k_pad_right_stick = 9;
// A stick's rest wanders by a few percent on every pad; the radial deadzone takes it out.
constexpr f32 k_stick_deadzone = 0.15f;
constexpr f32 k_trigger_deadzone = 0.05f;

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_two_pi = 6.28318530717958647692;
constexpr f64 k_half_pi = 1.57079632679489661923;
constexpr f32 k_pi_f = 3.14159265f;
constexpr f32 k_two_pi_f = 6.28318531f;
// Further than this from zero an angle is not one the camera can reach by turning; it is a broken
// input, and the answer is the one for zero rather than a loop that never ends.
constexpr f64 k_angle_limit = 1.0e6;

u32 bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;  // -0 and +0 are the same camera
  return std::bit_cast<u32>(v);
}

JsonValue vec3_json(const Vec3& v) {
  JsonValue out = JsonValue::array();
  out.push_back(JsonValue(v.x));
  out.push_back(JsonValue(v.y));
  out.push_back(JsonValue(v.z));
  return out;
}

bool read_f32(const JsonValue& object, const char* key, f32& out) {
  const JsonValue* v = object.find(key);
  f64 value = 0.0;
  if (v == nullptr || !v->get_f64(value)) return false;
  out = static_cast<f32>(value);
  return true;
}

bool read_u32(const JsonValue& object, const char* key, u32& out) {
  const JsonValue* v = object.find(key);
  u64 value = 0;
  if (v == nullptr || !v->get_u64(value) || value > 0xFFFFFFFFull) return false;
  out = static_cast<u32>(value);
  return true;
}

bool read_string(const JsonValue& object, const char* key, std::string& out) {
  const JsonValue* v = object.find(key);
  std::string_view text;
  if (v == nullptr || !v->get_string(text)) return false;
  out = std::string(text);
  return true;
}

bool read_vec3(const JsonValue& object, const char* key, Vec3& out) {
  const JsonValue* v = object.find(key);
  if (v == nullptr || !v->is_array() || v->size() != 3) return false;
  f64 c[3] = {};
  for (u32 i = 0; i < 3; ++i) {
    if (!(*v)[i].get_f64(c[i])) return false;
  }
  out = Vec3{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2])};
  return true;
}

JsonValue state_json(const FlyState& state) {
  JsonValue out = JsonValue::object();
  out.set("position", vec3_json(state.position));
  out.set("yaw", JsonValue(state.yaw));
  out.set("pitch", JsonValue(state.pitch));
  return out;
}

bool read_state(const JsonValue& object, FlyState& out) {
  return object.is_object() && read_vec3(object, "position", out.position) &&
         read_f32(object, "yaw", out.yaw) && read_f32(object, "pitch", out.pitch);
}

std::string hex16(u64 value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

}  // namespace

input::ActionMap default_fly_map() {
  using input::Binding;
  using input::Source;
  input::ActionMap map;
  // The order is the file's order and part of the hash: append, never reorder, or every recorded
  // session stops replaying (the log refuses a map whose hash moved).
  const input::ActionId move = map.add_action("move", input::ActionKind::Axis2);
  map.bind(move, Binding{Source::Key, k_key_d, 1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_a, -1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_w, 1.0f}, 1);
  map.bind(move, Binding{Source::Key, k_key_s, -1.0f}, 1);
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, k_stick_deadzone}, 0);
  // SDL's stick y grows downwards (window.md), so forward on the stick is -1 scaled to +1.
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_y, -1.0f, k_stick_deadzone}, 1);

  const input::ActionId lift = map.add_action("lift", input::ActionKind::Axis);
  map.bind(lift, Binding{Source::Key, k_key_e, 1.0f});
  map.bind(lift, Binding{Source::Key, k_key_space, 1.0f});
  map.bind(lift, Binding{Source::Key, k_key_q, -1.0f});
  map.bind(lift, Binding{Source::Key, k_key_lctrl, -1.0f});
  map.bind(lift, Binding{Source::GamepadAxis, k_pad_right_trigger, 1.0f, k_trigger_deadzone});
  map.bind(lift, Binding{Source::GamepadAxis, k_pad_left_trigger, -1.0f, k_trigger_deadzone});

  // Pointer motion, read as a distance (`InputState::delta2`); window y grows downwards, so -1
  // makes "up" positive, and a player who wants it inverted edits this scale.
  const input::ActionId look = map.add_action("look", input::ActionKind::Axis2);
  map.bind(look, Binding{Source::MouseAxis, static_cast<u32>(input::MouseAxisCode::X), 1.0f}, 0);
  map.bind(look, Binding{Source::MouseAxis, static_cast<u32>(input::MouseAxisCode::Y), -1.0f}, 1);

  // The right stick, read as a rate (`InputState::axis2`, with the radial deadzone a pair of one
  // source's axes gets).
  const input::ActionId turn = map.add_action("turn", input::ActionKind::Axis2);
  map.bind(turn, Binding{Source::GamepadAxis, k_pad_right_x, 1.0f, k_stick_deadzone}, 0);
  map.bind(turn, Binding{Source::GamepadAxis, k_pad_right_y, -1.0f, k_stick_deadzone}, 1);

  const input::ActionId fast = map.add_action("fast", input::ActionKind::Button);
  map.bind(fast, Binding{Source::Key, k_key_lshift});
  map.bind(fast, Binding{Source::Key, k_key_rshift});
  map.bind(fast, Binding{Source::GamepadButton, k_pad_left_stick});

  const input::ActionId slow = map.add_action("slow", input::ActionKind::Button);
  map.bind(slow, Binding{Source::Key, k_key_lalt});
  map.bind(slow, Binding{Source::Key, k_key_ralt});
  map.bind(slow, Binding{Source::GamepadButton, k_pad_right_stick});

  const input::ActionId capture = map.add_action("capture", input::ActionKind::Button);
  map.bind(capture, Binding{Source::Key, k_key_escape});
  map.bind(capture, Binding{Source::GamepadButton, k_pad_back});

  const input::ActionId marker = map.add_action("marker", input::ActionKind::Button);
  map.bind(marker, Binding{Source::Key, k_key_m});
  map.bind(marker, Binding{Source::GamepadButton, k_pad_north});
  return map;
}

bool resolve_fly_actions(const input::ActionMap& map, FlyActions& out, std::string* error) {
  struct Wanted {
    const char* name;
    input::ActionKind kind;
    input::ActionId* slot;
  };
  const Wanted wanted[] = {
      {"move", input::ActionKind::Axis2, &out.move},
      {"lift", input::ActionKind::Axis, &out.lift},
      {"look", input::ActionKind::Axis2, &out.look},
      {"turn", input::ActionKind::Axis2, &out.turn},
      {"fast", input::ActionKind::Button, &out.fast},
      {"slow", input::ActionKind::Button, &out.slow},
      {"capture", input::ActionKind::Button, &out.capture},
      {"marker", input::ActionKind::Button, &out.marker},
  };
  for (const Wanted& w : wanted) {
    const input::ActionId id = map.find_action(w.name);
    if (id == input::k_invalid_action || map.action_kind(id) != w.kind) {
      if (error != nullptr) {
        *error = std::string("the action map has no ") + input::action_kind_name(w.kind) +
                 " action named '" + w.name + "', which the fly camera reads";
      }
      return false;
    }
    *w.slot = id;
  }
  return true;
}

void fly_sin_cos(f32 angle, f32& sin_out, f32& cos_out) noexcept {
  f64 x = static_cast<f64>(angle);
  if (!(x >= -k_angle_limit && x <= k_angle_limit)) x = 0.0;
  while (x > k_pi)
    x -= k_two_pi;
  while (x < -k_pi)
    x += k_two_pi;
  // Into [-pi/2, pi/2]: sin(pi - x) = sin x and cos(pi - x) = -cos x, and the same about -pi.
  f64 cos_sign = 1.0;
  if (x > k_half_pi) {
    x = k_pi - x;
    cos_sign = -1.0;
  } else if (x < -k_half_pi) {
    x = -k_pi - x;
    cos_sign = -1.0;
  }
  // Taylor series through x^17 and x^16: at |x| = pi/2 the first terms left out are 4e-14 and
  // 5e-13, a hundred thousand times under an f32's ulp at 1. Every coefficient is one division of
  // two exactly representable integers, which every compiler folds to the same correctly rounded
  // double, and Horner's order is the source's (no contraction, ADR-0035).
  constexpr f64 s3 = -1.0 / 6.0;
  constexpr f64 s5 = 1.0 / 120.0;
  constexpr f64 s7 = -1.0 / 5040.0;
  constexpr f64 s9 = 1.0 / 362880.0;
  constexpr f64 s11 = -1.0 / 39916800.0;
  constexpr f64 s13 = 1.0 / 6227020800.0;
  constexpr f64 s15 = -1.0 / 1307674368000.0;
  constexpr f64 s17 = 1.0 / 355687428096000.0;
  constexpr f64 c2 = -1.0 / 2.0;
  constexpr f64 c4 = 1.0 / 24.0;
  constexpr f64 c6 = -1.0 / 720.0;
  constexpr f64 c8 = 1.0 / 40320.0;
  constexpr f64 c10 = -1.0 / 3628800.0;
  constexpr f64 c12 = 1.0 / 479001600.0;
  constexpr f64 c14 = -1.0 / 87178291200.0;
  constexpr f64 c16 = 1.0 / 20922789888000.0;
  const f64 x2 = x * x;
  f64 s = s17;
  s = s * x2 + s15;
  s = s * x2 + s13;
  s = s * x2 + s11;
  s = s * x2 + s9;
  s = s * x2 + s7;
  s = s * x2 + s5;
  s = s * x2 + s3;
  s = s * x2 + 1.0;
  s = s * x;
  f64 c = c16;
  c = c * x2 + c14;
  c = c * x2 + c12;
  c = c * x2 + c10;
  c = c * x2 + c8;
  c = c * x2 + c6;
  c = c * x2 + c4;
  c = c * x2 + c2;
  c = c * x2 + 1.0;
  sin_out = static_cast<f32>(s);
  cos_out = static_cast<f32>(cos_sign * c);
}

Vec3 fly_forward(const FlyState& state) noexcept {
  f32 sy = 0.0f;
  f32 cy = 0.0f;
  f32 sp = 0.0f;
  f32 cp = 0.0f;
  fly_sin_cos(state.yaw, sy, cy);
  fly_sin_cos(state.pitch, sp, cp);
  // rotate(quat(+y, yaw) * quat(+x, pitch), -z)
  return Vec3{-(cp * sy), sp, -(cp * cy)};
}

Vec3 fly_right(const FlyState& state) noexcept {
  f32 sy = 0.0f;
  f32 cy = 0.0f;
  fly_sin_cos(state.yaw, sy, cy);
  return Vec3{cy, 0.0f, -sy};  // rotate(quat(+y, yaw), +x)
}

void fly_tick(FlyState& state, const input::InputState& input, const FlyActions& actions,
              const FlyParams& params) noexcept {
  const f32 dt = 1.0f / static_cast<f32>(params.tick_hz);
  // ---- orientation: a mouse says how far, a stick how fast ----
  const Vec2 look = input.delta2(actions.look);
  const Vec2 turn = input.axis2(actions.turn);
  const f32 turn_step = params.turn_rate * dt;
  // Pointer right turns right, which is a negative turn about +y; up is positive pitch.
  f32 yaw = state.yaw - (look.x * params.look + turn.x * turn_step);
  f32 pitch = state.pitch + (look.y * params.look + turn.y * turn_step);
  if (pitch > params.pitch_limit) pitch = params.pitch_limit;
  if (pitch < -params.pitch_limit) pitch = -params.pitch_limit;
  if (!(yaw >= -1.0e6f && yaw <= 1.0e6f)) yaw = 0.0f;
  while (yaw > k_pi_f)
    yaw -= k_two_pi_f;
  while (yaw < -k_pi_f)
    yaw += k_two_pi_f;
  state.yaw = yaw;
  state.pitch = pitch;

  // ---- position, along the orientation this tick ended with ----
  Vec2 move = input.axis2(actions.move);
  const f32 length2 = move.x * move.x + move.y * move.y;
  if (length2 > 1.0f) {
    // Two keys at once are a diagonal of length sqrt(2); flying faster sideways is a bug, not a
    // technique. A stick is already inside its disc and never gets here.
    const f32 inverse = 1.0f / std::sqrt(length2);
    move.x = move.x * inverse;
    move.y = move.y * inverse;
  }
  const f32 lift = input.axis(actions.lift);
  f32 speed = params.speed;
  if (input.held(actions.fast)) speed = speed * params.fast;
  if (input.held(actions.slow)) speed = speed * params.slow;
  const f32 step = speed * dt;
  const Vec3 forward = fly_forward(state);
  const Vec3 right = fly_right(state);
  state.position.x = state.position.x + (forward.x * move.y + right.x * move.x) * step;
  state.position.y = state.position.y + (forward.y * move.y + lift) * step;
  state.position.z = state.position.z + (forward.z * move.y + right.z * move.x) * step;
}

renderer::Camera fly_view(const FlyState& state, f32 fov_y, f32 znear) noexcept {
  constexpr f32 k_reach = 100.0f;
  const Vec3 forward = fly_forward(state);
  renderer::Camera camera;
  camera.position = state.position;
  camera.target =
      Vec3{state.position.x + forward.x * k_reach, state.position.y + forward.y * k_reach,
           state.position.z + forward.z * k_reach};
  camera.fov_y = fov_y;
  camera.znear = znear;
  return camera;
}

FlyState fly_state_from_camera(const renderer::Camera& camera) noexcept {
  FlyState out;
  out.position = camera.position;
  const Vec3 d{camera.target.x - camera.position.x, camera.target.y - camera.position.y,
               camera.target.z - camera.position.z};
  const f32 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(length > 0.0f)) return out;
  const f32 y = d.y / length;
  out.pitch = std::asin(y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y));
  // forward = (-cos p sin yaw, sin p, -cos p cos yaw)
  out.yaw = std::atan2(-d.x, -d.z);
  return out;
}

// ---- the session header ----------------------------------------------------------------------

JsonValue session_to_json(const SessionHeader& header) {
  JsonValue out = JsonValue::object();
  out.set("format", k_session_format);
  out.set("version", static_cast<u64>(header.version));
  out.set("recorded_by", header.recorded_by);
  JsonValue engine = JsonValue::object();
  engine.set("commit", header.engine_commit);
  engine.set("dirty", header.engine_dirty);
  out.set("engine", std::move(engine));
  JsonValue scene = JsonValue::object();
  scene.set("scene", header.scene);
  scene.set("mesh", header.mesh);
  scene.set("procedural", header.procedural);
  scene.set("grid", static_cast<u64>(header.grid));
  scene.set("grid_instances", static_cast<u64>(header.grid_instances));
  out.set("scene", std::move(scene));
  out.set("start", state_json(header.start));
  JsonValue camera = JsonValue::object();
  camera.set("fov_y", JsonValue(header.fov_y));
  camera.set("znear", JsonValue(header.znear));
  out.set("camera", std::move(camera));
  JsonValue fly = JsonValue::object();
  fly.set("tick_hz", static_cast<u64>(header.params.tick_hz));
  fly.set("speed", JsonValue(header.params.speed));
  fly.set("fast", JsonValue(header.params.fast));
  fly.set("slow", JsonValue(header.params.slow));
  fly.set("look", JsonValue(header.params.look));
  fly.set("turn_rate", JsonValue(header.params.turn_rate));
  fly.set("pitch_limit", JsonValue(header.params.pitch_limit));
  out.set("fly", std::move(fly));
  out.set("ticks", header.ticks);
  return out;
}

bool session_from_json(const JsonValue& value, SessionHeader& out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = "session header: " + why;
    return false;
  };
  if (!value.is_object()) {
    return fail(
        "the log carries no session block; it was not recorded by engine-view --record-input");
  }
  std::string format;
  if (!read_string(value, "format", format) || format != k_session_format) {
    return fail(std::string("the session block is not an ") + k_session_format + " block");
  }
  SessionHeader h;
  if (!read_u32(value, "version", h.version)) return fail("no version");
  if (h.version != k_fly_version) {
    return fail("recorded with fly camera integration version " + std::to_string(h.version) +
                ", and this engine-view integrates version " + std::to_string(k_fly_version) +
                "; a trajectory from another version would go somewhere else without saying so, "
                "so the log is refused rather than misread");
  }
  (void)read_string(value, "recorded_by", h.recorded_by);
  if (const JsonValue* engine = value.find("engine"); engine != nullptr && engine->is_object()) {
    (void)read_string(*engine, "commit", h.engine_commit);
    if (const JsonValue* dirty = engine->find("dirty"); dirty != nullptr) {
      (void)dirty->get_bool(h.engine_dirty);
    }
  }
  const JsonValue* scene = value.find("scene");
  if (scene == nullptr || !scene->is_object() || !read_string(*scene, "scene", h.scene) ||
      !read_string(*scene, "mesh", h.mesh) || !read_string(*scene, "procedural", h.procedural) ||
      !read_u32(*scene, "grid", h.grid) || !read_u32(*scene, "grid_instances", h.grid_instances)) {
    return fail("'scene' is missing a field");
  }
  const JsonValue* start = value.find("start");
  if (start == nullptr || !read_state(*start, h.start)) return fail("'start' is not a camera");
  const JsonValue* camera = value.find("camera");
  if (camera == nullptr || !camera->is_object() || !read_f32(*camera, "fov_y", h.fov_y) ||
      !read_f32(*camera, "znear", h.znear)) {
    return fail("'camera' is missing its field of view or near plane");
  }
  const JsonValue* fly = value.find("fly");
  if (fly == nullptr || !fly->is_object() || !read_u32(*fly, "tick_hz", h.params.tick_hz) ||
      !read_f32(*fly, "speed", h.params.speed) || !read_f32(*fly, "fast", h.params.fast) ||
      !read_f32(*fly, "slow", h.params.slow) || !read_f32(*fly, "look", h.params.look) ||
      !read_f32(*fly, "turn_rate", h.params.turn_rate) ||
      !read_f32(*fly, "pitch_limit", h.params.pitch_limit)) {
    return fail("'fly' is missing a parameter");
  }
  if (h.params.tick_hz == 0 || h.params.tick_hz > 100000) return fail("'tick_hz' is out of range");
  const JsonValue* ticks = value.find("ticks");
  if (ticks == nullptr || !ticks->get_u64(h.ticks)) return fail("no 'ticks'");
  out = std::move(h);
  return true;
}

// ---- the trajectory ----------------------------------------------------------------------------

u64 hash_fly_state(u64 seed, const FlyState& state) noexcept {
  u64 h = hash_combine(seed, bits(state.position.x));
  h = hash_combine(h, bits(state.position.y));
  h = hash_combine(h, bits(state.position.z));
  h = hash_combine(h, bits(state.yaw));
  return hash_combine(h, bits(state.pitch));
}

u64 trajectory_seed(const FlyState& start) noexcept {
  return hash_fly_state(hash_combine(k_hash_seed, k_fly_version), start);
}

JsonValue trajectory_to_json(const Trajectory& trajectory, u32 tick_hz) {
  JsonValue out = JsonValue::object();
  out.set("format", k_trajectory_format);
  out.set("version", static_cast<u64>(k_fly_version));
  out.set("tick_hz", static_cast<u64>(tick_hz));
  out.set("ticks", trajectory.ticks);
  out.set("hash", hex16(trajectory.hash));
  out.set("last", state_json(trajectory.last));
  JsonValue markers = JsonValue::array();
  for (const FlyMarker& marker : trajectory.markers) {
    JsonValue m = state_json(marker.state);
    m.set("tick", marker.tick);
    markers.push_back(std::move(m));
  }
  out.set("markers", std::move(markers));
  return out;
}

// ---- the session -------------------------------------------------------------------------------

bool FlySession::start(const input::ActionMap& map, const SessionHeader& header,
                       std::string* error) {
  if (!resolve_fly_actions(map, actions_, error)) return false;
  if (header.params.tick_hz == 0) {
    if (error != nullptr) *error = "the fly camera needs a tick rate above zero";
    return false;
  }
  input_.set_map(map);
  header_ = header;
  state_ = header.start;
  tick_ = SimTick{0};
  trajectory_ = Trajectory{};
  trajectory_.hash = trajectory_seed(state_);
  trajectory_.last = state_;
  capture_presses_ = 0;
  return true;
}

void FlySession::on_tick(void* user, SimTick tick, const input::InputState& state) {
  FlySession& self = *static_cast<FlySession*>(user);
  fly_tick(self.state_, state, self.actions_, self.header_.params);
  self.tick_ = tick;
  self.trajectory_.ticks = tick.value;
  self.trajectory_.hash = hash_fly_state(self.trajectory_.hash, self.state_);
  self.trajectory_.last = self.state_;
  if (state.pressed(self.actions_.capture)) ++self.capture_presses_;
  if (state.pressed(self.actions_.marker)) {
    self.trajectory_.markers.push_back(FlyMarker{tick.value, self.state_});
  }
}

u32 FlySession::run(std::span<const input::RawEvent> events, u32 cursor, SimTick to) {
  return input::feed_ticks(input_, events, cursor, next(), to,
                           input::ReplayOptions{&FlySession::on_tick, this});
}

}  // namespace engine::view
