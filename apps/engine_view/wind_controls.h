#pragma once

// **The wind, on three keys** (docs/subsystems/apps.md, "The wind on three keys"; gfx.md, "The live
// wind"; roadmap W151): `;` and `'` step the mean wind a Beaufort force down and up, and `G` held
// forces a gust that rises over ten seconds, lasts as long as the key is down and dies away over
// ten more when it is let go, so the trees' and the water's answer to a storm can be seen on
// demand. A `--game` module reads and sets the same state (module_abi.h, `wind_state`, `wind_set`,
// `wind_gust`). What is here is the state and its arithmetic, stepped on the session's ticks
// (`FlySession`), so the wind a session had is a function of its recorded presses and its module's
// calls; nothing here sees a window, and nothing here enters the trajectory's hash. The renderer
// takes `live()` as the frame's `FrameDesc::wind`.

#include <core/base/types.h>
#include <domain/gfx/wind.h>

namespace engine::view {

class WindControls {
 public:
  // **Where the session starts**: the scene's roughness z0 (what a forced gust adds; the scene's
  // `Sky.wind.roughness_m`, 0.03 without one) and the base (`wind_start`). The live wind is unset —
  // every reader takes the scene's wind or the hand tunable's — until a key or a module moves it,
  // and a step is taken from the base the host last said (`set_base`).
  void start(f64 roughness_m, f64 base_mean_m_s, f64 base_to_deg) noexcept;
  // The scene's wind now (its mean and direction at the sky's hour), which a first step steps from.
  // Ignored once the live wind is in force.
  void set_base(f64 mean_m_s, f64 to_deg) noexcept;

  // **A key's step** (`wind_weaker`, `wind_stronger`): the mean to the next Beaufort force's speed
  // below or above it (`gfx::beaufort_speed`: 0 to 34.8 m/s), from the live mean or, unset, from
  // the base. True when the mean changed (a step past an end changes nothing).
  bool step(bool up) noexcept;
  // **A module's set** (`wind_set`): the mean, held to [0, k_wind_max_m_s], and where it blows to
  // (degrees; below 0 keeps the direction). The live wind is in force from then.
  void set(f64 mean_m_s, f64 to_deg) noexcept;
  // **A module's gust** (`wind_gust`): held while `held`, beside the key.
  void set_game_gust(bool held) noexcept { game_gust_ = held; }
  // **One tick**: `dt` seconds of the session, the gust key held or not. A gust held while the live
  // wind is unset puts it in force at the base (a gust is a fluctuation of the wind that blows).
  void tick(f64 dt, bool key_held) noexcept;

  // The frame's wind (`FrameDesc::wind`): unset until a key or a module moved it.
  gfx::WindLive live() const noexcept;
  bool set_now() const noexcept { return set_; }
  f64 mean_m_s() const noexcept { return set_ ? mean_m_s_ : base_mean_m_s_; }
  f64 to_deg() const noexcept { return set_ ? to_deg_ : base_to_deg_; }
  f64 gust_level() const noexcept { return gfx::wind_gust_level(phase_); }
  bool gust_held() const noexcept { return held_; }
  f64 roughness_m() const noexcept { return roughness_m_; }
  // The wind with the forced gust, m/s (`gfx::wind_live_speed`).
  f64 speed_m_s() const noexcept { return gfx::wind_live_speed(live(), roughness_m_); }
  // Changes of the mean (keys and sets that moved it), the module's sets, and ticks a gust was
  // held.
  u32 steps() const noexcept { return steps_; }
  u32 sets() const noexcept { return sets_; }
  u64 gust_ticks() const noexcept { return gust_ticks_; }

 private:
  f64 roughness_m_ = 0.03;
  bool set_ = false;
  f64 mean_m_s_ = 0.0;
  f64 to_deg_ = 0.0;
  f64 base_mean_m_s_ = 0.0;
  f64 base_to_deg_ = 0.0;
  f64 phase_ = 0.0;
  bool held_ = false;
  bool game_gust_ = false;
  u32 steps_ = 0;
  u32 sets_ = 0;
  u64 gust_ticks_ = 0;
};

// **Where a live session's wind starts** (the session header's `wind` block): the scene's
// roughness (0.03 without a scene wind), and the base a first key steps from — the hand wind of
// `renderer.wind.speed_m_s` when it is set (towards `renderer.wind.to_deg` when that is), else a
// fixed scene wind's, else calm. The live wind itself starts unset: until a key or a module moves
// it, every reader draws what it drew before this row.
struct WindStart {
  f64 mean_m_s = 0.0;
  f64 to_deg = 0.0;
  f64 roughness_m = 0.03;
};
WindStart wind_start(const gfx::WindDesc& scene_wind) noexcept;

// The title's and the console's words for the live wind: `wind 18.9 m/s (Bft 8)`, with ` gusting
// 27.6` while a forced gust stands over the mean (its speed now). Into `out`, which it returns.
const char* format_wind(const WindControls& wind, char* out, usize size) noexcept;

}  // namespace engine::view
