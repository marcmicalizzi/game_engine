#pragma once

// **The interactive camera**: a fly camera driven by `input::InputState` at a fixed tick, and the
// session header that makes a recording of it replayable bit for bit (docs/subsystems/apps.md,
// "`--interactive`: a camera somebody flies").
//
// **Why a fixed tick and not the frame's dt.** A camera integrated over wall-clock frame times is
// a function of how long each frame happened to take, so the same keystrokes flown twice go to
// two different places — a replay could only ever be approximately where the session was, and a
// benchmark of a replay would be measuring a different path from the one that was flown. At a
// fixed tick the camera is a function of the tick sequence alone: every tick is the same `dt`,
// every event is stamped with the tick it is fed at, and the frames only *sample* the state at
// tick boundaries. So a session and its replay run the same ticks with the same events and land on
// the same floats, whatever either one's frame times were — which is the whole replay guarantee.
//
// **Why this is not in `systems/renderer` or `foundation/input`.** The camera is a consumer of
// input and a producer of a `renderer::Camera`, and neither module may depend on the other's
// half; the one place they meet is the host that owns both, which is this app — the same reason
// `anim_lod.h` lives beside `main.cpp`. A game writes exactly this: a map, an `InputState`, a
// fixed tick, and a state it integrates.
//
// **Determinism across machines.** The integration is `+ - * /` and `sqrt` on `f32`, which IEEE
// fixes and the tree's `-ffp-contract=off` keeps unfused (ADR-0035), plus sine and cosine — which
// the C library does *not* fix: MSVC's CRT and glibc disagree in the last bit for some arguments.
// So the trigonometry here is `fly_sin_cos`, a polynomial in `f64` that every compiler evaluates
// to the same bits, and the committed trajectory of `content/input-logs/sessions/` is the same
// on Windows, on both Linux compilers and at both CPU baselines. The C library appears exactly
// once, turning the start camera into a yaw and a pitch, and its answer is written into the
// session header so a replay reads the numbers instead of recomputing them.
//
// **Sign conventions** are the camera paths' (`engine.scene.CameraKey.rotation`: -z forward, +y
// up): yaw is a turn about +y, counter-clockwise seen from above, and pitch a turn about the
// camera's right axis, up positive, so the orientation is `quat(+y, yaw) * quat(+x, pitch)` and
// a recorded session's camera at any tick can be written as a camera path key without a sign
// flipping. Forward at zero is -z and right is +x, the same `forward` `--fly` and every
// `CameraPath` look along.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <foundation/input/input.h>
#include <systems/renderer/view_set.h>

#include <span>
#include <string>

namespace engine::view {

// **The integration's version.** Everything a trajectory is a function of besides the events and
// the header's numbers — `fly_tick`'s arithmetic and its order, `fly_sin_cos`, the clamp, the
// wrap, what `frame_index` means — is this number. A session log records the version it was flown
// with, and a replay under another is refused rather than misread: it would fly somewhere else
// and say nothing. Bump it with any such change, and regenerate the committed fixture's
// trajectory (`tests/fly_tests.cpp` writes the new one beside its failure).
inline constexpr u32 k_fly_version = 1;
inline constexpr const char* k_session_format = "engine.view.session";
inline constexpr const char* k_trajectory_format = "engine.view.trajectory";

// The frame rate the renderer's clock is read at: `FrameDesc::frame_index` drives the light orbit
// and the deformation phase at a nominal 60 Hz, so an interactive frame passes the *tick* as a
// frame index at that rate and the lights move with simulated time rather than with frame count.
inline constexpr u32 k_frame_index_hz = 60;

// What the integration is told. Every field is written into the session header and read back
// from it on replay, so a replay flies with the recording's numbers whatever the tunables say now.
struct FlyParams {
  u32 tick_hz = 240;
  f32 speed = 1.0f;      // metres per second, resolved (the `0 = scene` default is gone here)
  f32 fast = 4.0f;       // multiplies speed while `fast` is held
  f32 slow = 0.25f;      // multiplies speed while `slow` is held
  f32 look = 0.0022f;    // radians per pixel of pointer motion
  f32 turn_rate = 2.5f;  // radians per second at full stick
  f32 pitch_limit = 1.553343f;  // radians(89): short of the pole, where yaw stops meaning anything
};

// The camera between ticks. `yaw` is kept in [-pi, pi] and `pitch` in [-limit, limit].
struct FlyState {
  Vec3 position{};
  f32 yaw = 0.0f;
  f32 pitch = 0.0f;
};

// The actions the camera reads, by id in one map. The map is data (`default_fly_map`, a file under
// content/input-maps/, or a player's rebind); these are the names it has to carry.
struct FlyActions {
  input::ActionId move = input::k_invalid_action;     // Axis2: x right, y forward
  input::ActionId lift = input::k_invalid_action;     // Axis: up positive, along world +y
  input::ActionId look = input::k_invalid_action;     // Axis2, read as a delta: pixels, y up
  input::ActionId turn = input::k_invalid_action;     // Axis2, read as a rate: a stick, y up
  input::ActionId fast = input::k_invalid_action;     // Button
  input::ActionId slow = input::k_invalid_action;     // Button
  input::ActionId capture = input::k_invalid_action;  // Button: the window takes the pointer
  input::ActionId marker = input::k_invalid_action;   // Button: remember this tick's camera
};

// The bindings engine-view ships (content/input-maps/engine-view.json is this map, byte for byte,
// and a test holds the two together): WASD to move, E/Space up and Q/Ctrl down, pointer motion to
// look while the window holds the pointer, Shift fast, Alt slow, Escape to take or give back the
// pointer, M to drop a marker; and on a gamepad the left stick to move, the triggers up and down,
// the right stick to turn, the stick clicks fast and slow, Back to capture and North to mark.
input::ActionMap default_fly_map();
// Every action above by name and kind. False, naming the first one missing or of the wrong kind.
bool resolve_fly_actions(const input::ActionMap& map, FlyActions& out, std::string* error);

// sin and cos of `angle` from arithmetic alone (see the file comment): a Taylor polynomial in f64
// on [-pi/2, pi/2] after reduction by whole turns and by pi - x, good to about 1e-13 before the
// round to f32. Not a general replacement for the C library — slower, and meant for angles within
// a few turns of zero — but the same bits on every compiler and platform the tree builds for.
void fly_sin_cos(f32 angle, f32& sin_out, f32& cos_out) noexcept;

// Where the camera looks, and its right, from the state's yaw and pitch through `fly_sin_cos`.
Vec3 fly_forward(const FlyState& state) noexcept;
Vec3 fly_right(const FlyState& state) noexcept;

// One tick of the camera, from the tick's input: look and turn first, then move along the new
// orientation — forward and right in the look direction (pitch included, so W flies where the
// camera points), lift along world up — at `speed` times `fast` and/or `slow`, over one tick's
// `dt = 1 / tick_hz`. A diagonal of two keys is normalized; a stick's is already in its disc.
void fly_tick(FlyState& state, const input::InputState& input, const FlyActions& actions,
              const FlyParams& params) noexcept;

// The camera the renderer draws from: the state's position, a target 100 m along forward (the
// camera paths' reach for an orientation key, far enough that the direction survives the
// subtraction at kilometres from the origin), and the header's field of view and near plane.
renderer::Camera fly_view(const FlyState& state, f32 fov_y, f32 znear) noexcept;
// A start state from any camera. This is where the C library is used, once: its answer goes into
// the session header and a replay reads it back rather than calling this again.
FlyState fly_state_from_camera(const renderer::Camera& camera) noexcept;

// ---- the session header ----------------------------------------------------------------------

// The `session` block of a recorded `input::InputLog` (foundation/input/input_log.h): everything
// a replay needs besides the events and the map, whose hash the log's own header carries.
struct SessionHeader {
  u32 version = k_fly_version;
  std::string recorded_by = "engine-view";
  std::string engine_commit = "unknown";  // the build stamp of the binary that recorded it
  bool engine_dirty = false;
  // What was drawn: the command line's --scene or --mesh, the procedural scene an absent one
  // builds and its grid, and --grid-instances. A replay that names none draws this.
  std::string scene;
  std::string mesh;
  std::string procedural;
  u32 grid = 257;
  u32 grid_instances = 0;
  FlyState start;
  f32 fov_y = 0.9599310886f;
  f32 znear = 0.1f;
  FlyParams params;
  // How many ticks the session ran: a replay runs exactly these, so a camera still moving when
  // the recording stopped stops at the same place.
  u64 ticks = 0;
};

JsonValue session_to_json(const SessionHeader& header);
// False with the reason for anything that is not a session block this engine-view can fly —
// another format, a missing or mistyped field, or **another integration version**, which is
// refused rather than misread.
bool session_from_json(const JsonValue& value, SessionHeader& out, std::string* error);

// ---- the trajectory ----------------------------------------------------------------------------

// A marker: the tick `marker` was pressed on, and the camera after that tick.
struct FlyMarker {
  u64 tick = 0;
  FlyState state;
};

// What a session flew: its length, a hash over every tick's state bit for bit, where it ended,
// and its markers. Two runs with the same hash flew the same path to the last bit; the markers are
// the readable half, and the ticks `--marker-captures` draws.
struct Trajectory {
  u64 ticks = 0;
  u64 hash = 0;
  FlyState last;
  Vector<FlyMarker> markers;
};

// The start of a trajectory's hash chain.
u64 trajectory_seed(const FlyState& start) noexcept;
u64 hash_fly_state(u64 seed, const FlyState& state) noexcept;
// {"format","version","tick_hz","ticks","hash","last","markers":[{"tick","position","yaw","pitch"}]}
// with every float as the f64 of its f32, so the text reads back to the same bits.
JsonValue trajectory_to_json(const Trajectory& trajectory, u32 tick_hz);

// ---- the session -------------------------------------------------------------------------------

// One camera flown by one event stream. The live window loop and a replay drive the same object
// the same way — `run` a tick range over a span of events stamped with the ticks they are fed at
// — through `input::feed_ticks`, the loop `InputLog::replay` itself runs. Nothing here allocates
// once the input state has seen each bound code, except a marker, which is a push onto a list.
class FlySession {
 public:
  // The map must outlive the session. False when it lacks an action the camera reads.
  bool start(const input::ActionMap& map, const SessionHeader& header, std::string* error);

  // Every tick from `next()` to `to`: feed the events stamped with it, integrate, hash, and note a
  // marker or a capture press. Events are read from `cursor` (anything stamped before `next()` is
  // skipped); the return value is the index of the first event not fed. Nothing runs when `to` is
  // before `next()`.
  u32 run(std::span<const input::RawEvent> events, u32 cursor, SimTick to);

  // The last tick run (0 before the first), and the one the next event must be stamped with.
  SimTick tick() const noexcept { return tick_; }
  SimTick next() const noexcept { return SimTick{tick_.value + 1}; }
  const FlyState& state() const noexcept { return state_; }
  renderer::Camera camera() const noexcept {
    return fly_view(state_, header_.fov_y, header_.znear);
  }
  // `FrameDesc::frame_index` for a frame drawn now: the tick at `k_frame_index_hz`.
  u64 frame_index() const noexcept {
    return tick_.value * k_frame_index_hz / header_.params.tick_hz;
  }
  // Presses of `capture` so far. The window owner compares it with the count it last acted on,
  // so the toggle happens once per press, on the tick the press was fed.
  u32 capture_presses() const noexcept { return capture_presses_; }
  const Trajectory& trajectory() const noexcept { return trajectory_; }
  const input::InputState& input() const noexcept { return input_; }
  const SessionHeader& header() const noexcept { return header_; }

 private:
  static void on_tick(void* user, SimTick tick, const input::InputState& state);

  input::InputState input_;
  FlyActions actions_;
  SessionHeader header_;
  FlyState state_;
  SimTick tick_;
  Trajectory trajectory_;
  u32 capture_presses_ = 0;
};

}  // namespace engine::view
