#pragma once

// **The two rates, on four keys** (docs/subsystems/apps.md, "The two rates on four keys";
// renderer.md, "One world clock, and the day's length"; roadmap R88): `[` and `]` step the **day's
// length** — the sky's rate per real second — down and up its ladder, `,` and `.` the **world's
// rate**, the clock every animation reads (the water, the smoke, the fire's flicker, the sway, the
// cloud drift) and the dunes' time-lapse rides. The two are independent on purpose: the owner
// inspects the world's events — the sand moving, a fire's smoke, the waves — at any speed, from a
// pause to a fast-forward, while the sky keeps the pace its day length gives it, and watches the
// sky turn at any speed over a world that runs at real time. What is here is the arithmetic and
// the text: the ladders and a step along one, the clocks the host keeps, and how a rate and a day
// are written in the window title. Nothing here sees a window or an input state; the keys reach
// the host as the fly session's control presses (fly_camera.h, `ViewControl`: the actions keep
// their names, `sun_slower`/`sun_faster` and `dunes_slower`/`dunes_faster`, which the committed
// map's hash and sessions are pinned on), and the dunes' motion takes the product of the world
// rate and `--time-rate` through `renderer::TerrainMotion::set_rate`, whose rules a change goes
// through.

#include <core/base/types.h>

#include <span>

namespace engine::view {

// **The day's ladder**, as sun rates (game seconds of the sky's day per real second; the day's
// length is 86,400 over the rung): still, a day of 24 h (real), 4 h, **1 h** (the owner's
// production target, 2026-10-20), 24 min, 4 min, 1 min and 10 s. A sixty-minute day is a rung so a
// tester reaches it from real time in two presses; the 10 s day is the fastest the sky is worth
// watching at — past it the sun strobes.
inline constexpr f64 k_sun_ladder[] = {0.0, 1.0, 6.0, 24.0, 60.0, 360.0, 1'440.0, 8'640.0};
// **The world's ladder**, world seconds per real second: a pause, slow motion (an eighth, a
// quarter, a half), real time, fast-forward (2, 4 and 8), and the time-lapses the dunes are
// watched at (a minute, ten minutes, an hour and a day a second; at `--time-rate 1` these are the
// rates the dunes' own keys used to give them). The rung at 1 is where a tester steps back to.
inline constexpr f64 k_world_ladder[] = {0.0, 0.125, 0.25, 0.5,   1.0,     2.0,
                                         4.0, 8.0,   60.0, 600.0, 3'600.0, 86'400.0};

// The rung after `rate` going up (`up`) or down: from a rate between two rungs (a `--sun-rate
// 5000`), the rung on that side of it; past an end, that end; at an end going outwards, the same
// rate. The ladder is ascending.
f64 ladder_step(std::span<const f64> ladder, f64 rate, bool up) noexcept;

// **A clock the host keeps at a rate a key may change**: how far it has run (`time_s`) and how
// fast. The time only ever accumulates the rate times the real seconds the frames simulated (their
// ticks), so a change of rate changes how fast the clock runs and never where it is. The sky's day
// is one (`time_s` the sky's seconds past its start, handed to the renderer as
// `FrameDesc::sky_time_s` with the `--time-of-day` offset), the world's another (`time_s` the world
// clock, `FrameDesc::world_time_s`).
struct RateClock {
  f64 rate = 0.0;
  f64 time_s = 0.0;
  f64 start_rate = 0.0;
  u32 changes = 0;  // rate changes that changed the rate

  void start(f64 at_rate, f64 at_time_s = 0.0) noexcept {
    rate = at_rate > 0.0 ? at_rate : 0.0;
    start_rate = rate;
    time_s = at_time_s;
    changes = 0;
  }
  // A frame's worth of real seconds (an interactive frame's ticks, or a sixtieth of a second).
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
using SunDay = RateClock;
using WorldDay = RateClock;

// A rate as the title and the console write it: a whole number with thousands separators
// ("86,400", "604,800", "0"), and one decimal where it has a fraction ("5,000.5"); below one, up
// to three ("0.125"). Into `out`, which it returns; `size` of 32 holds any rate the flags accept.
const char* format_rate(f64 rate, char* out, usize size) noexcept;
// A day's length for a sun rate as the title writes it: "still" for a held sky, else the length
// in the largest unit it reaches — "24 h", "1 h", "24 min", "10 s", "1.5 h" — with one decimal
// where it has a fraction. Into `out`, which it returns.
const char* format_day_length(f64 sun_rate, char* out, usize size) noexcept;
// An hour of the day as "07:30" (apparent solar time; a negative or past-midnight hour wraps).
const char* format_hour(f64 hour, char* out, usize size) noexcept;

// **The window title's status**, in UTF-8: `walking · 07:30 · day 1 h · world ×1 · mouse captured
// (Esc frees it)`. The mode comes first (`flying`, `walking`, or `walking (ground only)` for a
// walker that follows the ground and collides with nothing, walk.h), and not at all for a caller
// that gives none; the sky's hour (`has_hour`, a scene with a sky) next; then the day's length and
// the world's rate; a replay (`live` false) has no pointer of its own and says nothing about it.
// Returns the length written, which a `size` of 160 always holds.
enum class TitleMode : u8 { none, flying, walking, walking_ground };
struct TitleStatus {
  TitleMode mode = TitleMode::none;
  bool has_hour = false;
  f64 hour = 0.0;  // the sky's local hour, [0, 24)
  f64 sun_rate = 0.0;
  f64 world_rate = 1.0;
  bool live = true;
  bool captured = false;
};
usize format_status(const TitleStatus& status, char* out, usize size) noexcept;

}  // namespace engine::view
