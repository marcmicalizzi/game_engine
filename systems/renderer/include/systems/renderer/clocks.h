#pragma once

// **One world clock, and the day's length** (docs/subsystems/renderer.md, "One world clock, and the
// day's length"; roadmap R88). A session has three times, and every one of them is a function of
// its ticks, so a replay is a replay. With `session_s = ticks / tick_hz` (real seconds, never
// scaled):
//
//   world_time_s  = world_start_s + sum over ticks of dt * world_rate
//                   the animations' clock: the water's waves, the fire's flicker and smoke, the
//                   cloud drift, the flora's sway. `world_rate` is 1 — real time — unless a viewer
//                   scales it to inspect something (engine-view's `,` and `.`); 0 pauses it.
//   sky_time_s    = epoch_s + sky_start_s + sun_rate * session_s
//                   the sky's hour: the sun, the moon, the sky's light and exposure, and the hour
//                   of the wind record the weather reads. The day's length is in real seconds, so
//                   the sky keeps its pace whatever the world rate does.
//   ground_time_s = epoch_s + time_rate * (world_time_s - world_start_s)
//                   the dunes' time-lapse (geology): the time the moving ground is headed for. It
//                   rides the world clock, so a world rate scales the dunes with everything else.
//
// `sun_rate` is the day's length said the other way round — `86,400 / day_length_s` — and is what
// is kept, because every flag, key, request and state file before the day length spoke it, and a
// rate round-tripped through a length would not come back to its own bits. Zero holds the sky (an
// endless day). The rule the readers follow: **a light source is on the sky's clock, a surface or a
// medium that moves is on world time, the ground's geology is on its own time-lapse of it.**
//
// `epoch_s` is the scene's `Terrain.time` (0 without a terrain): where the sky and the ground stand
// at tick 0 unless told otherwise, so the storm erg still opens at 12:50 under a 12:50 sun.
// `sky_start_s` is the offset `--time-of-day` puts on it (`frame_clock`, request.h). The world
// starts at 0, which is the clock the flora stage's frame index at 60 Hz always was.
//
// The sums are in this order on purpose: `epoch + (start + rate * ticks / hz)` is how the sky's
// time was added up before the clocks were split (the ground's time plus the frame's offset), so a
// frame lit by a held or real day draws the bytes it drew.

#include <core/base/types.h>

namespace engine::renderer {

inline constexpr f64 k_day_s = 86'400.0;

// The day's length for a sun rate, and back. Zero (or anything not above zero) is a held sky both
// ways: an endless day, a rate of nothing.
inline f64 day_length_of_rate(f64 sun_rate) noexcept {
  return sun_rate > 0.0 ? k_day_s / sun_rate : 0.0;
}
inline f64 sun_rate_of_day(f64 day_length_s) noexcept {
  return day_length_s > 0.0 ? k_day_s / day_length_s : 0.0;
}

struct WorldClocks {
  f64 epoch_s = 0.0;        // the scene's `Terrain.time`: the sky's and the ground's start
  f64 world_start_s = 0.0;  // world seconds at tick 0 (a restored session's)
  f64 world_rate = 1.0;     // world seconds per real second
  f64 sky_start_s = 0.0;    // the sky's offset from the epoch at tick 0 (`--time-of-day`'s)
  f64 sun_rate = 0.0;       // sky game seconds per real second: 86,400 / day_length_s
  f64 time_rate = 0.0;      // the dunes' game seconds per world second

  f64 day_length_s() const noexcept { return day_length_of_rate(sun_rate); }

  // At `ticks` of a `tick_hz` clock, the rates held the whole way (an offscreen run, a flight, a
  // protocol request). A live session whose keys change a rate keeps its own sums (engine-view's
  // `SunDay` and `WorldDay`, the game runner's clock) and hands the frame what they reached.
  f64 world_time_s(u64 ticks, u32 tick_hz) const noexcept {
    return world_start_s + world_rate * static_cast<f64>(ticks) / static_cast<f64>(tick_hz);
  }
  // The sky's offset from the epoch: the frame's `FrameDesc::sky_time_s` less the epoch.
  f64 sky_offset_s(u64 ticks, u32 tick_hz) const noexcept {
    return sky_start_s + sun_rate * static_cast<f64>(ticks) / static_cast<f64>(tick_hz);
  }
  f64 sky_time_s(u64 ticks, u32 tick_hz) const noexcept {
    return epoch_s + sky_offset_s(ticks, tick_hz);
  }
  f64 ground_time_s(u64 ticks, u32 tick_hz) const noexcept {
    return epoch_s + time_rate * (world_time_s(ticks, tick_hz) - world_start_s);
  }
};

}  // namespace engine::renderer
