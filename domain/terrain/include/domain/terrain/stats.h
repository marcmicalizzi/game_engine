#pragma once

// Statistics that stand in for a picture (docs/subsystems/terrain.md, "What the numbers say"). The
// field is drawn on the owner's machine; here it is measured, and these are the numbers his eye
// would check on a flight: how tall the dunes stand above the floor between them, how steep the
// sand is (and whether any of it stands past the angle of repose), how much of the ground is flat,
// how much crest there is per square kilometre in each band, and how tall and how far apart the
// largest dunes are.
//
// **Deterministic, and the same on every toolchain.** Heights are the field's integers. A slope
// is compared with each bin's edge as tan^2 against the squared central difference, in doubles —
// exact for these magnitudes, IEEE-rounded identically everywhere, and with no library call — so
// every count is an integer the golden tests pin. The only floats a report carries are the
// integers divided by the vertex count at the end.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/terrain/dunes.h>

namespace engine::jobs {
class JobSystem;
}

namespace engine::terrain {

// The slope histogram's upper edges, degrees; the last bin is everything steeper than 36.
inline constexpr u32 k_slope_bins = 9;
inline constexpr u32 k_slope_edges_deg[k_slope_bins - 1] = {5, 10, 15, 20, 25, 30, 34, 36};
// What "flat" means: within half a metre of the interdune floor.
inline constexpr i64 k_flat_um = 500'000;

struct BandStats {
  u32 primitives = 0;  // centred in the region
  i64 crest_mm = 0;    // crest line inside the region
  i64 tallest_um = 0;
};

struct FieldStats {
  i64 x0 = 0, z0 = 0;  // the region's corner, mm
  u32 nx = 0, nz = 0;  // vertices a side
  i64 spacing_mm = 0;
  i64 time_us = 0;
  u64 vertices = 0;
  u64 sand_vertices = 0;  // not rock: the slope histogram's population
  // Height above the interdune floor over every vertex, µm: the 10th, 50th, 90th and 99th
  // percentiles and the largest.
  i64 above_floor_um[5] = {};
  u64 slope[k_slope_bins] = {};  // sand vertices by slope
  u64 flat = 0;                  // vertices within `k_flat_um` of the floor
  u32 bands = 0;
  BandStats band[k_max_bands];
  // The tallest primitive centred in the region, its band, and that band's spacing: the mean
  // distance from each of its primitives in the region to the nearest other, mm.
  i64 tallest_um = 0;
  u32 tallest_band = 0;
  i64 tallest_spacing_mm = 0;

  u64 over_repose() const noexcept { return slope[k_slope_bins - 2] + slope[k_slope_bins - 1]; }
  u64 over_36() const noexcept { return slope[k_slope_bins - 1]; }
  // Everything, as one number for a golden.
  u64 hash() const noexcept;
};

// The slope bin of a central difference: height differences (µm) across 2 * spacing (mm).
u32 slope_bin(i64 dx_um, i64 dz_um, i64 spacing_mm) noexcept;

// The statistics of the field over `nx` x `nz` vertices from (x0, z0), `spacing_mm` apart, at a
// time and a detail. On the job system's performance pool when one is given: every block writes
// its own slots and the counts are sums, so the result is the same on any number of threads.
void field_stats(const DuneField& field, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                 i64 time_us, Detail detail, const LagField* lag, jobs::JobSystem* jobs,
                 FieldStats& out);

// The height grid itself, the same way: `nx * nz` heights in µm, row-major with z rows, from
// (x0, z0) `spacing_mm` apart, a gather per 64 x 64 block, into `heights_um` (resized, so a grid
// kept between calls is reused). What the statistics, the rings and the time-lapse re-evaluation
// all read (terrain.md, "Re-evaluation"); the same bytes on any number of threads.
void evaluate_grid(const DuneField& field, i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm,
                   i64 time_us, Detail detail, const LagField* lag, jobs::JobSystem* jobs,
                   Vector<i64>& heights_um);

}  // namespace engine::terrain
