#pragma once

// A tier's members, stepped (rocks.md, "Benches"; docs/experiments/exposed-bedrock.md, "Benches
// on a bedded face"): up a face cut from a bedded formation, a resistant member stands as a riser
// and a weak one lies back as a bench, so the front is a profile of the beds and not a plane. The
// cliff solver draws it and the cliffs generator stands the faces off by how far it can fall
// behind their plane, so both ask this one function with the same arguments.
//
// f64, IEEE arithmetic and `det::` only; one formation query a bed boundary.
#include "detail.h"

#include <domain/gfx/formation.h>

#include <memory>

namespace engine::rocks::detail {

// A riser's lean, run over rise: a resistant member stands at 80 degrees (a free face; design,
// steeper on stronger rock as Selby's rock-mass strength has it).
inline constexpr f64 k_riser_run = 0.17632698070846498;  // cot 80 degrees
// A weak member weathers back to a talus-mantled slope near the angle of repose: 35 degrees
// (Ward, Berlin and Anderson 2011's plinth "perched near the angle of repose"), and never steeper
// than 60 degrees nor gentler than 30 when the ground's own lean leaves it no choice.
inline constexpr f64 k_bench_run = 1.4281480067421146;       // cot 35 degrees
inline constexpr f64 k_bench_run_min = 0.57735026918962573;  // cot 60 degrees
inline constexpr f64 k_bench_run_max = 1.7320508075688772;   // cot 30 degrees
// A member thinner than this along a plumb line steps nothing: a thin shale parting is the
// undercut's notch, not a bench.
inline constexpr f64 k_member_min_m = 1.0;
// The tread at a resistant member's top, where a weak one lies on it: the stripped top of the
// cliff-former, a quarter of the weak member's plumb thickness, 0.3 to 2 m (design).
inline constexpr f64 k_tread_share = 0.25;
inline constexpr f64 k_tread_min_m = 0.3;
inline constexpr f64 k_tread_max_m = 2.0;
// A tread is drawn over this much of the weak member's rise (a few cells' slope, not a shelf).
inline constexpr f64 k_tread_rise_m = 0.5;
inline constexpr u32 k_bench_segments = 96;

// The formation as the steps read it: its frame and beds, when it is bedded (dip 20 degrees or
// less, as the tiers' breaks ask).
struct BenchBeds {
  bool bedded = false;
  gfx::FormationFrame frame{};
  std::unique_ptr<gfx::FormationParams> field;
  f64 ny = 1.0;
};
bool bench_beds(const scene::Formation& formation, BenchBeds& out);

// One face's column: the setback behind the foot's plumb line at each height, piecewise linear in
// up to `k_bench_segments` segments, its top's equal to `batter` times the height (the face's
// chord is its plane's, so tiers meet as they did).
struct BenchProfile {
  u32 count = 0;
  f64 foot = 0.0;
  f64 top = 0.0;
  f64 batter = 0.0;
  f64 y[k_bench_segments + 1] = {};
  f64 s[k_bench_segments + 1] = {};  // the setback at y[k]
  u32 weak[k_bench_segments] = {};   // 1 where segment k is a weak member's (a bench)
  f64 riser_run = k_riser_run;
  f64 bench_run = k_bench_run;
  f64 tread_m = 0.0;  // the widest tread drawn

  // Metres behind the foot's plumb line at height `at` (0 at and under the foot).
  f64 setback(f64 at) const;
  // Whether height `at` is on a bench (a weak member's slope or a tread).
  bool on_bench(f64 at) const;
  // How far the profile falls behind its plane at its worst, metres (0 or more).
  f64 behind() const;
};

// The column whose foot is at world (x, z), from `foot` to `top` (world heights), on a face of
// lean `batter` whose plane goes back into the hill along (ux, uz) (a unit horizontal vector): the
// beds are read on the plane, where the front stands. Without beds, or with no weak member thick
// enough, it is the plane.
void bench_profile(const BenchBeds& beds, f64 x, f64 z, f64 ux, f64 uz, f64 foot, f64 top,
                   f64 batter, BenchProfile& out);

}  // namespace engine::rocks::detail
