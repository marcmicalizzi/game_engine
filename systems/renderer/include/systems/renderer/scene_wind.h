#pragma once

// **The scene's wind, as the renderer reads and draws it** (docs/subsystems/gfx.md, "The wind a scene names"; roadmap W136;
// the model is domain/gfx's wind.h). Three things live here: the scene file's `Sky.wind` read and
// checked into a `gfx::WindDesc`; the hand wind a walker sets with the `renderer.wind.*` tunables;
// and the per-frame redraw of a water body's wave table and surface as the sea state moves, which
// the GPU scene keeps a record of per body (`WaterWindBody`) and copies into the material table and
// the surfaces buffer with the frame's "water wind" transfer (scene_renderer.cpp).
//
// **What it costs**: nothing for a body whose wind holds still — the sea state's key is the one
// already written and nothing is copied. A varying wind costs a sea state (144 steps of the
// history) every ten seconds of the sky's clock, and a frame whose key moved one copy of 512 bytes
// and one of 648 a body, behind a barrier that waits for the frames in flight.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
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
  f64 slope = 0.0;             // the foreshore's mean slope, for the run-up
  f64 bank_max = 0.0;
  f64 period_s = 0.0;  // the train's period at load (the arrival field's), unrounded
  gfx::WaterWave load_waves[gfx::k_water_waves]{};
  gfx::WaterSurface load_surface{};
  // What was last written: 0 the load's table, 1 a hand wind, 2 the scene's wind at `key_time`.
  u32 key_kind = 0;
  f64 key_a = 0.0;
  f64 key_b = 0.0;
};

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
void water_wind_frame(std::span<WaterWindBody> bodies, const gfx::WindDesc& wind, f64 sky_time_s,
                      Vector<WaterWindUpload>& uploads);

// The trees' wind from the scene's (`GpuScene::flora_frame`): where it blows to and its gust at
// the origin over `k_flora_wind_mean_m_s`. False when the scene has none.
bool scene_flora_wind(const gfx::WindDesc& wind, f64 sky_time_s, Vec2& to, f32& strength) noexcept;

}  // namespace engine::renderer
