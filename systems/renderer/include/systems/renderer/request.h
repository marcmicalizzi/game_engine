#pragma once

// **What a host asks the renderer for, translated once** (docs/subsystems/renderer.md, "One
// request, two hosts"). engine-view's flags and engine-host's `render.*` fields describe the same
// things — the settings, the world's clock, the ground's time — and each host used to turn its own
// spelling into `RenderSettings` and `FrameDesc` by hand, which is how the sky's hour, the
// exposure, the ground's time and the ground's layout came to exist behind flags with no protocol
// field. A `RenderRequest` is the one description; engine-view fills it from its flags, engine-host
// from the schema's types through `read_protocol_settings` and `read_protocol_clock`, and
// everything after that is these functions, which both call:
//
//   scene_file_options     the request's part of reading a scene file (the ground's time)
//   settings_for           the settings against the loaded scene (a page budget's percentage,
//                          the morph weights by name and the channels' authored defaults)
//   view_set_desc          the renderer's view layout from the resolved settings
//   frame_clock, frame_at  the frame's clock and the frame: `FrameDesc::sun_time_s`
//   flight_clock           the same clock for a flight (`FlightOptions`)
//   MovingGround           the terrain levels a run moves: the time-lapse, the rings, the tiles
//   sun_summary, time_lapse_summary   the summary blocks both hosts report
//
// `request_tests.cpp` holds the line: every field of `RenderSettings` and of `FrameDesc` is named
// there with the protocol field that reaches it or the reason none does, by a structured binding
// that stops compiling when a field is added, so the next setting cannot reach one host only.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <systems/renderer/flythrough.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_rings.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/renderer/terrain_time.h>
#include <systems/renderer/view_set.h>

#include <memory>
#include <optional>
#include <string>

namespace engine::jobs {
class JobSystem;
}
namespace engine::protocol {
struct RenderSettings;
struct RenderClock;
}  // namespace engine::protocol

namespace engine::renderer {

// A frame is a sixtieth of a second of the world's clock, whatever the display does: the frame
// index's clock, which the sun's day, the time-lapse and a flight all count by.
inline constexpr u32 k_frame_hz = 60;

// The world's clock as a host is asked for it (renderer.md, "One clock"): engine-view's
// `--time-of-day` and `--sun-rate`, `render.*`'s `clock`.
struct ClockRequest {
  // A sky's hour, local apparent solar time, [0, 24): the offset that puts the ground's day there.
  // Unset: the ground's own hour. Nothing without a sky.
  std::optional<f64> time_of_day_h;
  // The sun's day, game seconds per real second. Unset: the `renderer.sun.rate` tunable.
  std::optional<f64> sun_rate;
};

// Everything a host is asked that decides a picture, in one place.
struct RenderRequest {
  RenderSettings settings;
  ClockRequest clock;
  // The game time the ground stands at, in place of the scene file's (`SceneFileOptions`).
  std::optional<f64> ground_time_s;
  // The residency budget as a share of the loaded scene's page bytes, 1..100; 0 leaves
  // `settings.page_budget_bytes` as it is. Needs the scene, so `settings_for` applies it.
  u32 page_budget_pct = 0;
  // `<name|index>=<weight>` over the scene's morph channels, for the static shape stage.
  Vector<std::string> morph;
  // Every channel starts at its authored default weight. False when a pose stage plays the
  // channels instead (engine-view's `--morph-animate`), which starts from them itself.
  bool morph_defaults = true;
};

// The request's part of reading a scene file: the ground's time (scene.h, `SceneFileOptions`).
void scene_file_options(const RenderRequest& request, SceneFileOptions& out);

// The settings a request asks of a loaded scene: `request.settings` with the page budget's
// percentage and the static shape stage's weights resolved against it. False, with a sentence,
// for a morph entry that names no channel or a weight that is not a number.
bool settings_for(const RenderRequest& request, const SceneData& scene, RenderSettings& out,
                  std::string* error = nullptr);

// The renderer's view layout over a target (`SceneRenderer::Desc::views`), from the settings
// `resolve_settings` left — the ones the GPU scene's per-view working set was sized by.
ViewSetDesc view_set_desc(const RenderSettings& resolved);

// **The frame's clock** (renderer.md, "One clock", "The sun's day"): frame f stands
// `offset_s + sun_rate * f / 60` game seconds into the world's day, on top of the ground's own
// time. `offset_s` is a sky's `time_of_day` offset — the hours asked for less how far into its day
// the ground already stands — and 0 without one, whose stand-in sun has no hour.
struct FrameClock {
  f64 offset_s = 0.0;
  f64 sun_rate = 0.0;
  f64 at(u64 frame_index) const noexcept {
    return offset_s + sun_rate * static_cast<f64>(frame_index) / static_cast<f64>(k_frame_hz);
  }
};
FrameClock frame_clock(const ClockRequest& clock, const SceneData& scene);
// One frame of a run: its camera, its number and its place on the clock.
FrameDesc frame_at(const FrameClock& clock, const Camera& camera, u64 frame_index);
// A flight's clock: path frame f at `clock.at(f)`.
void flight_clock(const FrameClock& clock, FlightOptions& out);

// **The ground a run moves** (renderer.md, "The dunes in time-lapse", "The rings in the scene",
// "The ground from the world's tiles"): the time-lapse's job pool, the rings or the world's tiles
// laid out round the first camera, and the motion that blends their fields. Both hosts drive it
// the same way, frame by frame; what stays a host's is where the world's tiles come from — a
// world ring hands them over (engine-view's `ViewWorld`), and a host without one has them follow
// the camera by the ring's first-fill rule (`follow_tiles`). Nothing at all for a scene whose
// terrain is not drawn as levels.
class MovingGround {
 public:
  MovingGround();
  ~MovingGround();
  MovingGround(const MovingGround&) = delete;
  MovingGround& operator=(const MovingGround&) = delete;

  // Before `GpuScene::create`, which reserves the rings' or the tiles' slots: the job pool and the
  // layout round `first` (the run's first camera). False with a sentence; `stage()` then names
  // what failed ("terrain-rings" or "terrain-tiles").
  // `window` is a presenting host's: its pool leaves the frame's thread CPUs of their own
  // (`terrain_window_workers`, `renderer.terrain.workers`); an offscreen run's takes the default.
  bool prepare(const SceneData& data, const ResolvedSettings& resolved, const Camera& first,
               std::string* error = nullptr, bool window = false);
  // What `GpuScene::create` takes as its last argument: the rings, the tiles, or null.
  TerrainLevelSet* level_set() noexcept;
  TerrainTileSet* tiles() noexcept { return tiles_.get(); }
  const scene_gen::TileSource* tile_source() const noexcept;
  const char* stage() const noexcept { return stage_; }

  // After the GPU scene is made: the motion at the settings' rate. `wait` is an offscreen run's,
  // whose fields are waited for so that its frames draw the same pictures however fast the machine
  // is; a window's never waits. Nothing when the terrain has no levels.
  bool start(GpuScene& scene, const ResolvedSettings& resolved, bool wait,
             std::string* error = nullptr);
  // The tiles round `camera` by the ring's first-fill rule, for a host with no world ring.
  void follow_tiles(Vec3 camera);
  // Before a frame: the motion a sixtieth of a second on, its rings following `camera`.
  void frame(const Camera& camera);
  void finish();

  bool active() const noexcept { return motion_.active(); }
  // The rate is above zero: a call's frames differ from frame to frame.
  bool moving() const noexcept { return motion_.active() && motion_.config().rate > 0.0; }
  TerrainMotion& motion() noexcept { return motion_; }
  const TerrainMotion& motion() const noexcept { return motion_; }

 private:
  std::unique_ptr<jobs::JobSystem> jobs_;
  std::unique_ptr<TerrainRingSet> rings_;
  std::unique_ptr<TerrainSampler> ground_;
  std::unique_ptr<TerrainTileSet> tiles_;
  Vector<TerrainTile> round_;  // the first-fill rule's tiles, kept between frames
  const char* stage_ = "";
  // Last, so it goes first: it holds the sets and the job pool, which must outlive it.
  TerrainMotion motion_;
};

// "grid", "rings" or "tiles": how a motion's levels are laid out; "" without one.
const char* ground_layout(const TerrainMotion& motion) noexcept;

// **The summaries both hosts report** (apps.md, "--time-rate", "--sun-rate"). The sun's day as the
// host kept it: the rate at the end and at the start, how often it changed, and how far into the
// day the last frame was (game seconds, the clock's `at`).
struct SunDayState {
  f64 rate = 0.0;
  f64 start_rate = 0.0;
  u32 changes = 0;
  f64 time_s = 0.0;
};
// The `sun` block: the stand-in's arc and where its sun stood at `day.time_s`, and with a sky,
// the `sky` block of what drew the frame (`Stats::sky`).
JsonValue sun_summary(const RenderSettings& settings, const SunDayState& day);
JsonValue sun_summary(const RenderSettings& settings, const SunDayState& day, const Stats& stats);
// The `time_lapse` block: the rate and its rules, the clock, every level's state and, with rings
// or tiles, the `rings` block. Null JSON without a motion.
JsonValue time_lapse_summary(const TerrainMotion& lapse, const Stats& stats, const GpuScene& scene);
// A flight record's `terrain` object: the motion as the frame drew it. Null without one.
std::optional<scene::FrameTerrain> frame_terrain(const TerrainMotion& lapse);

// **The protocol's spelling** (schemas/protocol.schema, `engine.protocol.RenderSettings` and
// `RenderClock`) into the request: the one reader of the wire's settings, so a field the schema
// grows reaches the renderer here or `request_tests.cpp` says it does not. False, with the
// sentence a 1004 carries, for a value out of range or a spelling no host knows; `out.settings`,
// `out.page_budget_pct` and `out.morph` are written, the rest of `out` is left alone.
bool read_protocol_settings(const protocol::RenderSettings& in, RenderRequest& out,
                            std::string& error);
bool read_protocol_clock(const protocol::RenderClock& in, ClockRequest& out, std::string& error);

}  // namespace engine::renderer
