#pragma once

// **The time-lapse's two rates, on keys** (docs/subsystems/apps.md, "`--interactive`: a camera
// somebody flies"): `[` and `]` step the sun's day down and up its ladder, `,` and `.` the dunes'.
// The two are independent on purpose — the owner watches the sand move under a still sun, and the
// sun move over still sand. What is here is the arithmetic and the text: the ladders and a step
// along one, the day's clock the host keeps for the sun, and how a rate is written in the window
// title. Nothing here sees a window or an input state; the keys reach the host as the fly
// session's control presses (fly_camera.h, `ViewControl`), and the dunes' rate goes to
// `renderer::TerrainMotion::set_rate`, whose rules a change of rate goes through.

#include <core/base/types.h>

#include <span>

namespace engine::view {

// Game seconds per real second. Still, a minute, ten minutes, an hour, a tenth of a day (8,640,
// the rate the erg's time-lapse was smooth at before the surface clock) and a day; the dunes add a
// week, where a day of the sun's would turn seven times a second.
inline constexpr f64 k_sun_ladder[] = {0.0, 60.0, 600.0, 3'600.0, 8'640.0, 86'400.0};
inline constexpr f64 k_dune_ladder[] = {0.0, 60.0, 600.0, 3'600.0, 8'640.0, 86'400.0, 604'800.0};

// The rung after `rate` going up (`up`) or down: from a rate between two rungs (a `--time-rate
// 5000`), the rung on that side of it; past an end, that end; at an end going outwards, the same
// rate. The ladder is ascending.
f64 ladder_step(std::span<const f64> ladder, f64 rate, bool up) noexcept;

// **The sun's day, as the host keeps it**: how far into it the frames are (`time_s`, handed to
// the renderer as `FrameDesc::sun_time_s`) and how fast it runs. The time only ever accumulates the
// rate times the frames' simulated seconds, so a change of rate changes how fast the sun moves and
// never where it is.
struct SunDay {
  f64 rate = 0.0;
  f64 time_s = 0.0;
  f64 start_rate = 0.0;
  u32 changes = 0;  // rate changes that changed the rate

  void start(f64 at_rate) noexcept {
    rate = at_rate > 0.0 ? at_rate : 0.0;
    start_rate = rate;
    time_s = 0.0;
    changes = 0;
  }
  // A frame's worth of simulated seconds (an interactive frame's ticks, or a sixtieth of a second).
  void advance(f64 seconds) noexcept {
    if (seconds > 0.0) time_s += rate * seconds;
  }
  // True when it changed.
  bool set_rate(f64 next) noexcept {
    const f64 r = next > 0.0 ? next : 0.0;
    if (r == rate) return false;
    rate = r;
    ++changes;
    return true;
  }
};

// A rate as the title and the console write it: a whole number with thousands separators
// ("86,400", "604,800", "0"), and one decimal where it has a fraction ("5,000.5"). Into `out`,
// which it returns; `size` of 32 holds any rate the flags accept.
const char* format_rate(f64 rate, char* out, usize size) noexcept;

// **The window title's status**, in UTF-8: `dunes ×86,400 · sun ×3,600 · mouse captured (Esc frees
// it)`. `dunes` is false for a scene whose terrain cannot move, and then the dunes are left out; a
// replay (`live` false) has no pointer of its own and says nothing about it. Returns the length
// written, which a `size` of 128 always holds.
struct TitleStatus {
  bool dunes = false;
  f64 dune_rate = 0.0;
  f64 sun_rate = 0.0;
  bool live = true;
  bool captured = false;
};
usize format_status(const TitleStatus& status, char* out, usize size) noexcept;

}  // namespace engine::view
