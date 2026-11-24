#pragma once

// **The scene's wind, as the renderer reads and draws it** (docs/subsystems/gfx.md, "The wind a
// scene names"; roadmap W136; the model is domain/gfx's wind.h). Three things live here: the scene
// file's `Sky.wind` read and checked into a `gfx::WindDesc`; the hand wind a walker sets with the
// `renderer.wind.*` tunables; and the per-frame redraw of a water body's wave table and surface as
// the sea state moves, which the GPU scene keeps a record of per body (`WaterWindBody`) and copies
// into the material table and the surfaces buffer with the frame's "water wind" transfer
// (scene_renderer.cpp).
//
// **What it costs**: nothing for a body whose wind holds still — the sea state's key is the one
// already written and nothing is copied. A varying wind costs a sea state (144 steps of the
// history) every ten seconds of the sky's clock, and a frame whose key moved one copy of 512 bytes
// and one of 648 a body, behind a barrier that waits for the frames in flight.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/water_flux.h>
#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wind.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::renderer {

// `Sky.wind` checked into `out`; false with a sentence naming the file and the field.
bool read_scene_wind(const scene::Wind& wind, const std::string& path, gfx::WindDesc& out,
                     std::string& error);

// The hand wind (`renderer.wind.speed_m_s`, `renderer.wind.to_deg`): a speed of 0 or more holds
// every body that draws a wave table at that wind, at once (the sea as if it had blown for ever,
// so a walker sees the gale's sea when they ask for it), from `to_deg` when that is 0 or more and
// from the body's own direction otherwise. Below 0, the default, the scene's wind.
struct WindHand {
  bool set = false;
  f64 speed_m_s = 0.0;
  bool turned = false;
  f64 to_deg = 0.0;
};
WindHand wind_hand_tunable() noexcept;

// `Sky.swell` checked into `out`; false with a sentence naming the file and the field (W136-3).
bool read_scene_swell(const scene::Swell& swell, const std::string& path, gfx::SwellDesc& out,
                      std::string& error);

// **The hand swell** (`renderer.swell.height_m`, `renderer.swell.period_s`,
// `renderer.swell.to_deg`; W136-3): a height of 0 or more holds every open body that draws a wave
// table at that arriving swell, at once, at `period_s` (12 s by default), travelling to `to_deg`
// when that is 0 or more and otherwise as the body's shore train does. Below 0, the default, the
// scene's swell (or none).
struct SwellHand {
  bool set = false;
  f64 height_m = 0.0;
  f64 period_s = 12.0;
  bool turned = false;
  f64 to_deg = 0.0;
};
SwellHand swell_hand_tunable() noexcept;

// The mean wind the trees' sway calls 1 (`FloraWind::strength`; flora_motion.h), m/s: a scene's
// wind reaches the trees as its gust over this. 5 m/s, a land site's typical mean at 10 m
// (docs/experiments/wind.md, "How strong").
inline constexpr f64 k_flora_wind_mean_m_s = 5.0;

// One water body that draws a wave table (it had a wind at load), as the GPU scene keeps it.
struct WaterWindBody {
  u32 mesh = 0;
  u64 entry_offset = ~u64{0};    // bytes into the material table of its first wave entry
  u64 surface_offset = ~u64{0};  // bytes into the surfaces buffer of its `WaterSurface`, or none
  bool scene_wind = false;       // it follows a varying scene wind (`SceneWater::scene_wind`)
  gfx::WaterWaveGrid grid{};     // that wind's grid
  f64 fetch_m = 0.0;
  u32 seed = 1;
  Vec2 direction{1.0f, 0.0f};  // the load's wind direction
  bool swell = false;          // its own swell is the train that comes ashore: its height holds
  // W136-3: an open body (a hand swell reaches it); the wind and swell it draws
  // (`SceneWater::sea`; kind none: its own wind, `load_wind_m_s`); whether its first train is the
  // sky's swell (`SceneWater::scene_swell`); where its first train travels to (a hand swell's
  // default direction); and the hand state its grid was last drawn for.
  bool open = false;
  gfx::WindDesc sea{};
  bool sky_swell = false;
  f64 load_wind_m_s = 0.0;
  Vec2 train_to{1.0f, 0.0f};
  f64 hand_key[5]{-2.0, -2.0, -2.0, -2.0, -2.0};
  gfx::WaterWaveGrid hand_grid{};
  f64 slope = 0.0;  // the foreshore's mean slope, for the run-up
  f64 bank_max = 0.0;
  f64 period_s = 0.0;  // the train's period at load (the arrival field's), unrounded
  gfx::WaterWave load_waves[gfx::k_water_waves]{};
  gfx::WaterSurface load_surface{};
  // What was last written: 0 the load's table, 1 a hand wind, 2 the scene's wind at `key_time`.
  u32 key_kind = 0;
  f64 key_a = 0.0;
  f64 key_b = 0.0;
  f64 key_c = 0.0;  // W136-3: the hand swell's height, period and direction
  f64 key_d = 0.0;
  f64 key_e = 0.0;
  // **The lift's bound follows the sea drawn** (gfx.md, "What the scene's wind does not do"): the
  // instance whose surface this is (~0: none), its `bounds_padding` as the load set it, and the
  // padding the sea last written needs (`water_lift_padding`). The GPU scene holds the instance at
  // the larger of the two, so a hand gale on a body loaded calm keeps every lifted crest in its
  // cluster's sphere.
  u32 instance = ~0u;
  f32 load_padding = 0.0f;
  f32 padding = 0.0f;
  // **The live wind** (W151; gfx.md, "The live wind"): the frames this body was taken through, the
  // sea it last drew and the grid it drew it on (what a live wind starts from, so no wave jumps
  // when a key first moves it), and under a live wind its own sea, stepped by the world clock's
  // seconds
  // (`gfx::sea_step`), the grid it is drawn on for the session, and what was last written.
  u64 frames = 0;
  gfx::SeaState drawn_sea{};
  gfx::WaterWaveGrid drawn_grid{};
  bool live = false;
  gfx::SeaState live_sea{};
  gfx::WaterWaveGrid live_grid{};
  f64 live_world_s = 0.0;
  f64 live_key[5]{-1.0, -1.0, -1.0, -1.0, -1.0};
};

// How far a lifted body's vertices can leave its rest mesh (mesh space, metres): the bank's highest
// ground (a parked vertex), twice the train's height, the run-up, the tide, every wave of the table
// and the four sines' reach, and half a metre. What the load pads the instance by and what the
// wind's redraw pads it by, one function.
inline f32 water_lift_padding(const gfx::WaterSurface& surface,
                              std::span<const gfx::WaterWave> waves, f32 bank_max) noexcept {
  f32 reach = surface.height_m * 2.0f + surface.run_up_m + surface.tide_m;
  for (const gfx::WaterWave& wave : waves)
    reach += wave.amplitude;
  reach += surface.amplitude * 2.1f;
  return bank_max + reach + 0.5f;
}

// **The shore's two trains span a varying wind's seas** (W136-2; gfx.md, "The shore's train follows
// the sea"): the first at the period of the largest sea the wind raises (the load's), the second an
// octave above it on the group's lattice (fourteen waves in the first's seven) along the same
// direction, each with its own arrival field, so as the sea grows its energy moves from the second
// to the first and the surf's period follows it with no phase that jumps. `height_m` is the load's
// sea; the split is the mean shape's at the two frequencies, which the first frame redraws.
gfx::WaterCross water_cross_of_span(f64 height_m, f64 period_s, Vec2 direction) noexcept;

// The heights of the shore's two trains (deep water) for a sea state: its energy shared by its
// spectral density per log frequency at the two trains' frequencies, so H1² + H2² = H², and the
// period those weights give (for the run-up and the reach). False when the state holds neither.
bool water_train_heights(const gfx::SeaState& state, f64 period_1, f64 period_2, f64& h1, f64& h2,
                         f64& period) noexcept;

// One copy the frame makes: into the material table (`surface` false) or the surfaces buffer.
struct WaterWindUpload {
  bool surface = false;
  u64 offset = 0;
  u32 bytes = 0;
  alignas(16) u8 data[sizeof(gfx::WaterSurface)]{};
};

// The quantum of the sky's clock a varying sea state is taken at: ten seconds, under which no
// state the history steps through moves by a part in a thousand.
inline constexpr f64 k_water_wind_quantum_s = 10.0;

// For sky time `sky_time_s`, the copies that bring each body to the table and surface it draws now
// (appended to `uploads`, which is cleared first); a body already there adds none.
// Each body draws its own `sea` (the scene's wind and swell as it takes them; W136-3).
//
// **Under a live wind** (`live.set`; W151, gfx.md "The live wind") every body draws a sea of its
// own that follows the live wind by the sea's own growth law (`gfx::sea_step`, the equivalent-
// duration method the scene's varying wind is stepped by) over the world clock's seconds since the
// last frame (`world_time_s`): it starts from the sea the body drew the frame before, on the grid
// it drew it on, so a key's step moves no wave's phase and pops no height; a session whose live
// wind is set from its first frame (the hand tunable at launch) starts from that wind's fully
// grown sea, the picture the hand wind always drew. `z0` is the scene's roughness, which sets how
// much a forced gust adds (`gfx::wind_gust_factor`).
void water_wind_frame(std::span<WaterWindBody> bodies, f64 sky_time_s,
                      Vector<WaterWindUpload>& uploads, const gfx::WindLive& live = {},
                      f64 world_time_s = 0.0, f64 z0 = 0.03);

// The trees' wind from the scene's (`GpuScene::flora_frame`): where it blows to and its gust at
// the origin over `k_flora_wind_mean_m_s`. False when the scene has none. A live wind (W151) wins
// over the scene's and the hand tunable's: its mean and direction with the scene's turbulence, and
// the forced gust's speed over the mean added (`gfx::wind_live_speed`); the trees answer it at once
// through their own sway, since a part's lean is a function of the strength now.
// **Two clocks** (S118; clocks.h's rule): the mean and the direction are the weather's, at the
// sky's hour `sky_time_s`; the turbulence over them is the medium moving, at the world's
// `world_time_s`, so a held sky (`--time-rate 0`) still gusts and a paused world holds the gust.
// The sound's wind (`world_sound::WorldWind`) takes the same two, so what is heard is what is
// drawn.
bool scene_flora_wind(const gfx::WindDesc& wind, f64 sky_time_s, f64 world_time_s, Vec2& to,
                      f32& strength, const gfx::WindLive& live = {}) noexcept;
// **The wind the sway uses when no live wind is set** (S112-2, so the sound reads it as the trees
// do): the hand tunable's (`renderer.flora.wind_*`) written into `held` — a fixed wind at the
// scene's roughness, no track, so nothing allocates — and returned, or the scene's own when the
// tunable is not set. `scene_flora_wind`'s own choice, shared.
const gfx::WindDesc& scene_wind_in_use(const gfx::WindDesc& wind, gfx::WindDesc& held) noexcept;

// **The trees' wind's ceiling** (W154-2; renderer.md, "What a storm costs"): the strength, over the
// 5 m/s mean day, the trees' wind of `scene_flora_wind` may reach over the next seconds, which the
// padding ladder chooses its rung from: the mean times the three-second gust factor of the scene's
// roughness (`gfx::wind_gust_factor`: the peak of its turbulence), plus the mean times the factor
// less one again while a forced gust is on at any level (its whole rise, so the rung is up before
// the gust is). The live mean is the larger of the trees' lagged one (`lagged`) and the live one,
// so a key's step up moves the rung before the trees move. False where `scene_flora_wind` is.
bool scene_flora_wind_ceiling(const gfx::WindDesc& wind, f64 sky_time_s,
                              const gfx::WindLive& lagged, const gfx::WindLive& live,
                              f32& ceiling) noexcept;

// **A step of the live mean reaches the trees through their own response** (W154; flora.md, "Motion
// in a storm"). A key's step moves the live mean by a Beaufort force in one frame, and the sway's
// lean is quasi-static (a function of the load now), so taken as it is the step is a jump of every
// tree. A tree is a damped oscillator whose fundamental sway is a fraction of a hertz, so a step of
// its load reaches its mean lean over about half its period. The trees take the live mean through
// a critically damped second-order response at `k_flora_wind_lag_hz` (no overshoot, its rate
// continuous), stepped exactly over the world clock's seconds between frames, once a frame by the
// frame's wind pass (GpuScene::water_wind_frame, trees or none) and read by the poses; the scene's
// turbulence and the forced gust (smooth already, W151's rise and decay) pass as they are. Unset,
// or after a jump of the world clock (backwards or more than `k_flora_wind_lag_snap_s`), it stands
// at the live mean. Presentation: it enters no hash.
inline constexpr f64 k_flora_wind_lag_hz = 0.3;
inline constexpr f64 k_flora_wind_lag_snap_s = 10.0;
struct FloraWindLag {
  bool set = false;
  f64 mean_m_s = 0.0;
  f64 rate = 0.0;  // m/s per second
  f64 at_s = 0.0;  // the world time it stands at
  // Steps to `world_time_s` towards the live mean; returns the live wind with the trees' mean.
  gfx::WindLive follow(const gfx::WindLive& live, f64 world_time_s) noexcept;
  // The live wind with the mean it stands at now, without stepping (what the frame's stats say).
  gfx::WindLive held(const gfx::WindLive& live) const noexcept;
};

}  // namespace engine::renderer
