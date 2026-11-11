// The landform solver (docs/subsystems/landform.md, "The step"; docs/experiments/landform.md has
// every source and constant). One step: uplift, the flood and its receivers, the drainage area,
// the rivers (stream-power incision and deposition solved together along the flood's order, the
// lakes and the sea taking what reaches them), hillslope diffusion, the slope limiter. Every
// decision is IEEE `+ - * /`, `sqrt`, `floor` and integers; the loops the job system runs are the
// ones whose points read nothing another point of the same loop writes.
#include <core/jobs/job_system.h>
#include <domain/gfx/formation.h>
#include <domain/landform/landform.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine::landform {

namespace {

// The nearest doubles to the square root of two and its reciprocal: constants, not calls.
constexpr f64 k_sqrt2 = 1.4142135623730951;
constexpr f64 k_inv_sqrt2 = 0.70710678118654757;
// The flood's unit, 1/1024 mm, a metre: the key holds 36 bits of height above 28 of cell index,
// so one integer comparison orders by height and then by index.
constexpr f64 k_units_per_m = 1000.0 * static_cast<f64>(k_fill_units_per_mm);
constexpr u32 k_index_bits = 28;
constexpr u64 k_index_mask = (u64{1} << k_index_bits) - 1;
constexpr i64 k_height_bias = i64{1} << 35;
// Room above the limit for a depression's epsilons: 2^29 units is more points than a lattice has.
constexpr i64 k_height_limit = (i64{1} << 35) - (i64{1} << 29);
constexpr i64 k_unvisited = std::numeric_limits<i64>::min();
constexpr u32 k_row_grain = 16;
// Keeps the receivers' numbers apart from the roughness's, which hash the same seed and cell.
constexpr u64 k_route_salt = 0xD1B54A32D192ED03ull;
// And the ridges' variation apart from both.
constexpr u64 k_ridge_salt = 0x8CB92BA72F3D8DD7ull;
// The explicit diffusion's largest step, as D dt / spacing^2 (a quarter is the stability limit).
constexpr f64 k_diffusion_number = 0.2;
// A heightfield's float holds a whole millimetre exactly below 2^24 of them.
constexpr f64 k_max_height_m = 16000.0;
// The rivers' sweeps end when no height moves by this much (the millimetre the heights round
// to), or after this many: Yuan et al. 2019 count six at G = 1 and nine at G = 2 to a bound of
// a thousandth of a step's uplift, 5 mm at 1 mm/yr and 5,000 years.
constexpr f64 k_river_tolerance_m = 1.0e-3;
constexpr u32 k_river_sweeps = 32;

u64 mix(u64 x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// [-1, 1), from the top 53 bits.
f64 signed_unit(u64 h) noexcept {
  return static_cast<f64>(h >> 11) * (1.0 / 4503599627370496.0) - 1.0;
}

// Rows in chunks on the job system, or all of them here. The body is taken by value because
// `parallel_for` wants an rvalue callable (it keeps a pointer to its own parameter's type).
template <class F>
void for_rows(jobs::JobSystem* jobs, u32 rows, F body) {
  if (jobs == nullptr) {
    body(u32{0}, rows);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, rows, k_row_grain, std::move(body));
}

i64 quantize(f64 z_m) noexcept {
  const f64 q = std::floor(z_m * k_units_per_m + 0.5);
  if (!(q > -static_cast<f64>(k_height_limit))) return -k_height_limit;  // NaN lands here too
  if (q > static_cast<f64>(k_height_limit)) return k_height_limit;
  return static_cast<i64>(q);
}

u64 key_of(i64 units, u32 index) noexcept {
  return (static_cast<u64>(units + k_height_bias) << k_index_bits) | index;
}

// A four-ary min-heap over the keys: the frontier of the flood, a few thousand entries wide.
void heap_push(Vector<u64>& heap, u64 key) {
  heap.push_back(key);
  u64* v = heap.data();
  u32 i = heap.size() - 1;
  while (i > 0) {
    const u32 p = (i - 1) / 4;
    if (v[p] <= key) break;
    v[i] = v[p];
    i = p;
  }
  v[i] = key;
}

u64 heap_pop(Vector<u64>& heap) noexcept {
  u64* v = heap.data();
  const u64 top = v[0];
  const u32 n = heap.size() - 1;
  const u64 last = v[n];
  heap.pop_back();
  if (n == 0) return top;
  u32 i = 0;
  for (;;) {
    const u32 c = 4 * i + 1;
    if (c >= n) break;
    u32 m = c;
    const u32 end = c + 4 < n ? c + 4 : n;
    for (u32 k = c + 1; k < end; ++k)
      if (v[k] < v[m]) m = k;
    if (v[m] >= last) break;
    v[i] = v[m];
    i = m;
  }
  v[i] = last;
  return top;
}

// A point's `sill` entry: none on draining ground; on a point the flood raised, the point its
// depression spills over (the first point the flood popped that was not itself raised); and, once
// `find_lakes` has run, on a sill the index of its lake with `k_sill_flag` set.
constexpr u32 k_no_sill = 0xFFFFFFFFu;
constexpr u32 k_sill_flag = 0x80000000u;
// A raised point this far or more under its spill is standing water (a lake's member); shallower
// is a flat the flood stepped across, which a river crosses and aggrades like any reach.
constexpr i64 k_lake_min_units = 10 * k_fill_units_per_mm;

struct Lake {
  u32 sill = 0;
  u32 begin = 0;  // its members, a run of `Routing::members`
  u32 end = 0;
  f64 pool = 0.0;  // sediment that reached its water this step, metres over one cell
};

struct Routing {
  Vector<i64> filled;
  Vector<u32> order;
  Vector<u8> receiver;
  Vector<u32> area;
  Vector<u64> heap;
  Vector<u32> sill;
  Vector<u64> members;  // sill above cell, sorted: each lake's cells are one run
  Vector<Lake> lakes;
};

// Priority-Flood with epsilon (Barnes, Lehman and Mulla 2014, Algorithm 3) from the held points:
// pop the lowest (height, index), raise each unvisited neighbour to at least one unit above it.
// A push is never below the pop that made it, so pops ascend, and `order` is both the flood's
// order and the flow tree's: a point's receiver, which is strictly lower on the filled surface,
// was popped before it.
template <class Q>
bool flood(u32 w, u32 h, const u8* held, Q&& units, Routing& r) {
  const u32 n = w * h;
  r.filled.assign(n, k_unvisited);
  r.order.resize(n);
  r.sill.assign(n, k_no_sill);
  r.heap.clear();
  i64* filled = r.filled.data();
  u32* order = r.order.data();
  u32* sill = r.sill.data();
  for (u32 i = 0; i < n; ++i) {
    if (held[i] == 0) continue;
    filled[i] = units(i);
    heap_push(r.heap, key_of(filled[i], i));
  }
  if (r.heap.size() == 0) return false;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(h);
  u32 count = 0;
  while (r.heap.size() != 0) {
    const u64 key = heap_pop(r.heap);
    const u32 c = static_cast<u32>(key & k_index_mask);
    const i64 fc = static_cast<i64>(key >> k_index_bits) - k_height_bias;
    order[count++] = c;
    const i32 cx = static_cast<i32>(c % w);
    const i32 cz = static_cast<i32>(c / w);
    for (u32 k = 0; k < 8; ++k) {
      const i32 nx = cx + k_receiver_dx[k];
      const i32 nz = cz + k_receiver_dz[k];
      if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) continue;
      const u32 ni = static_cast<u32>(nz) * w + static_cast<u32>(nx);
      if (filled[ni] != k_unvisited) continue;
      const i64 own = units(ni);
      const i64 f = own > fc ? own : fc + 1;
      filled[ni] = f;
      // A raised point belongs to the depression of the point that raised it, or begins one
      // that spills over that point.
      if (own <= fc) sill[ni] = sill[c] != k_no_sill ? sill[c] : c;
      heap_push(r.heap, key_of(f, ni));
    }
  }
  return count == n;
}

// The lakes of a flooded ground (landform.md, "Deposition"): every raised point standing
// `k_lake_min_units` or more under its filled level, grouped by the sill its depression spills
// over. Shallower raised points lose their sill and count as dry. A sorted run a lake, so the
// table is the same whatever order the flood met the points in.
template <class Q>
void find_lakes(u32 n, Q&& units, Routing& r) {
  r.members.clear();
  r.lakes.clear();
  u32* sill = r.sill.data();
  const i64* filled = r.filled.data();
  for (u32 p = 0; p < n; ++p) {
    if (sill[p] == k_no_sill) continue;
    if (filled[p] - units(p) >= k_lake_min_units) {
      r.members.push_back((static_cast<u64>(sill[p]) << 32) | p);
    } else {
      sill[p] = k_no_sill;
    }
  }
  std::sort(r.members.data(), r.members.data() + r.members.size());
  for (u32 k = 0; k < r.members.size(); ++k) {
    const u32 at = static_cast<u32>(r.members[k] >> 32);
    if (r.lakes.size() == 0 || r.lakes[r.lakes.size() - 1].sill != at) {
      Lake lake;
      lake.sill = at;
      lake.begin = k;
      r.lakes.push_back(lake);
      sill[at] = k_sill_flag | (r.lakes.size() - 1);
    }
    r.lakes[r.lakes.size() - 1].end = k + 1;
  }
}

// What settles at a point of standing water, laid on the lowest ground about it (landform.md,
// "Deposition"): `v` metres over one cell fill the point and those of its eight neighbours that
// stand lower, the lowest first, to one level no higher than `cap`, the water's. Returns what the
// cap left over. Held points are never raised.
f64 spread(f64* z, f64* sed, const u8* held, u32 p, f64 v, f64 cap, u32 w, u32 h,
           f64& laid) noexcept {
  u32 cell[9];
  f64 height[9];
  u32 m = 0;
  if (z[p] < cap) {
    cell[m] = p;
    height[m++] = z[p];
  }
  const i32 x = static_cast<i32>(p % w);
  const i32 y = static_cast<i32>(p / w);
  for (u32 k = 0; k < 8; ++k) {
    const i32 nx = x + k_receiver_dx[k];
    const i32 ny = y + k_receiver_dz[k];
    if (nx < 0 || ny < 0 || nx >= static_cast<i32>(w) || ny >= static_cast<i32>(h)) continue;
    const u32 q = static_cast<u32>(ny) * w + static_cast<u32>(nx);
    if (held[q] != 0 || !(z[q] < cap) || !(z[q] < z[p] + v)) continue;
    // Insertion by height; equal heights keep the code's order.
    u32 at = m++;
    while (at > 0 && height[at - 1] > z[q]) {
      cell[at] = cell[at - 1];
      height[at] = height[at - 1];
      --at;
    }
    cell[at] = q;
    height[at] = z[q];
  }
  if (m == 0) return v;
  f64 level = height[0];
  f64 rest = v;
  u32 k = 1;
  while (k < m) {
    const f64 next = height[k] < cap ? height[k] : cap;
    const f64 room = (next - level) * static_cast<f64>(k);
    if (room > rest) break;
    rest -= room;
    level = next;
    ++k;
  }
  f64 top = level + rest / static_cast<f64>(k);
  if (top > cap) top = cap;
  rest -= (top - level) * static_cast<f64>(k);
  for (u32 i = 0; i < m; ++i) {
    if (!(height[i] < top)) break;
    const f64 rise = top - height[i];
    z[cell[i]] = top;
    sed[cell[i]] += rise;
    laid += rise;
  }
  return rest > 0.0 ? rest : 0.0;
}

struct LakeCell {
  f64 height;
  u32 cell;
};

// A lake's pool laid from its deepest point up to one level, no higher than its sill stands now
// (landform.md, "Deposition"): what settles out of standing water levels its floor. Returns what
// the full lake passes on to its sill.
f64 fill_lake(const Routing& r, const Lake& lake, f64* z, f64* sed, Vector<LakeCell>& scratch,
              f64& laid) {
  const f64 cap = z[lake.sill];
  scratch.clear();
  for (u32 k = lake.begin; k < lake.end; ++k) {
    const u32 p = static_cast<u32>(r.members[k] & 0xFFFFFFFFull);
    if (z[p] < cap) scratch.push_back(LakeCell{z[p], p});
  }
  if (scratch.size() == 0) return lake.pool;
  std::sort(scratch.data(), scratch.data() + scratch.size(),
            [](const LakeCell& a, const LakeCell& b) {
              return a.height < b.height || (a.height == b.height && a.cell < b.cell);
            });
  const u32 m = scratch.size();
  f64 level = scratch[0].height;
  f64 rest = lake.pool;
  u32 k = 1;
  while (k < m) {
    const f64 room = (scratch[k].height - level) * static_cast<f64>(k);
    if (room > rest) break;
    rest -= room;
    level = scratch[k].height;
    ++k;
  }
  f64 top = level + rest / static_cast<f64>(k);
  if (top > cap) top = cap;
  rest -= (top - level) * static_cast<f64>(k);
  for (u32 i = 0; i < m; ++i) {
    if (!(scratch[i].height < top)) break;
    const f64 rise = top - scratch[i].height;
    z[scratch[i].cell] = top;
    sed[scratch[i].cell] += rise;
    laid += rise;
  }
  return rest > 0.0 ? rest : 0.0;
}

// The steepest lower neighbour of eight on the filled surface; the flood guarantees one lower.
// A diagonal's fall is weighed by 1 / (2 - r) and not by 1 / sqrt(2), r a fixed number in [0, 1)
// hashed from the seed and the cell (Fairfield and Leymarie 1991's Rho8, with the random number a
// property of the cell so the network holds still from step to step): on a plane whose fall line
// stands an angle t off an axis the diagonal wins where r > 1 - tan t, so over many cells the
// flow's direction is the plane's own, and its channels do not run in parallel along the lattice.
void find_receivers(u32 w, u32 h, const u8* held, u64 seed, jobs::JobSystem* jobs, Routing& r) {
  r.receiver.resize(w * h);
  const i64* filled = r.filled.data();
  u8* receiver = r.receiver.data();
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(h);
  for_rows(jobs, h, [&](u32 z0, u32 z1) {
    for (u32 z = z0; z < z1; ++z) {
      for (u32 x = 0; x < w; ++x) {
        const u32 i = z * w + x;
        if (held[i] != 0) {
          receiver[i] = k_no_receiver;
          continue;
        }
        u8 best = k_no_receiver;
        f64 best_slope = 0.0;
        const f64 diagonal = 1.0 / (2.0 - 0.5 * (signed_unit(mix(seed ^ mix(i))) + 1.0));
        for (u32 k = 0; k < 8; ++k) {
          const i32 nx = static_cast<i32>(x) + k_receiver_dx[k];
          const i32 nz = static_cast<i32>(z) + k_receiver_dz[k];
          if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) continue;
          const u32 ni = static_cast<u32>(nz) * w + static_cast<u32>(nx);
          const i64 drop = filled[i] - filled[ni];
          if (drop <= 0) continue;
          const f64 slope = static_cast<f64>(drop) * ((k & 1u) != 0 ? diagonal : 1.0);
          if (slope > best_slope) {
            best_slope = slope;
            best = static_cast<u8>(k);
          }
        }
        receiver[i] = best;
      }
    }
  });
}

struct Offsets {
  i32 at[8];
};

Offsets neighbour_offsets(u32 w) noexcept {
  Offsets o{};
  for (u32 k = 0; k < 8; ++k)
    o.at[k] = k_receiver_dz[k] * static_cast<i32>(w) + k_receiver_dx[k];
  return o;
}

u32 neighbour(u32 i, i32 offset) noexcept { return static_cast<u32>(static_cast<i32>(i) + offset); }

// In reverse flood order every donor comes before its receiver.
void accumulate(u32 w, u32 h, Routing& r) {
  const u32 n = w * h;
  r.area.assign(n, 1u);
  u32* area = r.area.data();
  const u32* order = r.order.data();
  const u8* receiver = r.receiver.data();
  const Offsets o = neighbour_offsets(w);
  for (u32 k = n; k-- > 0;) {
    const u32 i = order[k];
    const u8 code = receiver[i];
    if (code != k_no_receiver) area[neighbour(i, o.at[code])] += area[i];
  }
}

// ---- the sketch's fields over the lattice -------------------------------------------------------

bool in_range(f64 v, f64 lo, f64 hi) noexcept { return v >= lo && v <= hi; }

struct Frame {
  u32 w = 0;
  u32 h = 0;
  f64 spacing_m = 0.0;
  f64 origin_x_m = 0.0;
  f64 origin_z_m = 0.0;
  f64 x(u32 i) const noexcept { return origin_x_m + static_cast<f64>(i) * spacing_m; }
  f64 z(u32 j) const noexcept { return origin_z_m + static_cast<f64>(j) * spacing_m; }
};

bool edge_drains(EdgeKind k) noexcept { return k != EdgeKind::Closed; }

// Metres to the nearest open or sea edge; negative when no edge drains.
f64 drain_distance(const Sketch& s, const Frame& f, u32 i, u32 j) noexcept {
  f64 d = -1.0;
  const auto take = [&d](f64 v) {
    if (d < 0.0 || v < d) d = v;
  };
  if (edge_drains(s.edges.west)) take(static_cast<f64>(i) * f.spacing_m);
  if (edge_drains(s.edges.east)) take(static_cast<f64>(f.w - 1 - i) * f.spacing_m);
  if (edge_drains(s.edges.north)) take(static_cast<f64>(j) * f.spacing_m);
  if (edge_drains(s.edges.south)) take(static_cast<f64>(f.h - 1 - j) * f.spacing_m);
  return d;
}

// Metres to the nearest sea edge; negative when there is none.
f64 sea_distance(const Sketch& s, const Frame& f, u32 i, u32 j) noexcept {
  f64 d = -1.0;
  const auto take = [&d](f64 v) {
    if (d < 0.0 || v < d) d = v;
  };
  if (s.edges.west == EdgeKind::Sea) take(static_cast<f64>(i) * f.spacing_m);
  if (s.edges.east == EdgeKind::Sea) take(static_cast<f64>(f.w - 1 - i) * f.spacing_m);
  if (s.edges.north == EdgeKind::Sea) take(static_cast<f64>(j) * f.spacing_m);
  if (s.edges.south == EdgeKind::Sea) take(static_cast<f64>(f.h - 1 - j) * f.spacing_m);
  return d;
}

// Seeded noise along a line, in [-1, 1]: four octaves of value noise from `length` down, each
// half the wavelength and 1 / sqrt(2) the amplitude of the one before, so the power spectrum
// falls as the wavenumber squared (a Brown noise: landform.md, "Uplift along a ridge"). A
// station is a place on the plan's own line, so the same line gives the same numbers whatever
// lattice reads it.
f64 line_noise(u64 seed, f64 station, f64 length) noexcept {
  f64 sum = 0.0;
  f64 amplitude = 1.0;
  f64 wavelength = length;
  for (u32 octave = 0; octave < 4; ++octave) {
    const f64 at = station / wavelength;
    const f64 cell = std::floor(at);
    const f64 t = at - cell;
    const u64 index = static_cast<u64>(static_cast<i64>(cell));
    const f64 a = signed_unit(mix(seed ^ mix(index + (u64{octave} << 56))));
    const f64 b = signed_unit(mix(seed ^ mix(index + 1 + (u64{octave} << 56))));
    sum += amplitude * (a + (b - a) * (t * t * (3.0 - 2.0 * t)));
    amplitude *= k_inv_sqrt2;
    wavelength *= 0.5;
  }
  // 1 + 2^-1/2 + 2^-1 + 2^-3/2, the four amplitudes.
  return sum / 2.5606601717798214;
}

// The largest ridge kernel at a point, mm/yr: (1 - (r / reach)^2)^2 of the line's rate, the rate
// that of the line's nearest station.
f64 ridge_rate(const Sketch& s, f64 px, f64 pz) noexcept {
  f64 best = 0.0;
  for (u32 r = 0; r < s.uplift.ridges.size(); ++r) {
    const Ridge& ridge = s.uplift.ridges[r];
    const f64 reach2 = ridge.reach_m * ridge.reach_m;
    f64 d2 = reach2;
    f64 station = 0.0;
    f64 along = 0.0;  // the line's length before this segment
    for (u32 k = 0; k + 1 < ridge.points.size(); ++k) {
      const f64 ax = ridge.points[k].x;
      const f64 az = ridge.points[k].z;
      const f64 abx = ridge.points[k + 1].x - ax;
      const f64 abz = ridge.points[k + 1].z - az;
      const f64 apx = px - ax;
      const f64 apz = pz - az;
      const f64 len2 = abx * abx + abz * abz;
      f64 t = len2 > 0.0 ? (apx * abx + apz * abz) / len2 : 0.0;
      t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
      const f64 dx = apx - t * abx;
      const f64 dz = apz - t * abz;
      const f64 seg2 = dx * dx + dz * dz;
      const f64 length = std::sqrt(len2);
      if (seg2 < d2) {
        d2 = seg2;
        station = along + t * length;
      }
      along += length;
    }
    if (d2 >= reach2) continue;
    const f64 k1 = 1.0 - d2 / reach2;
    const f64 varied = ridge.variation > 0.0
                           ? 1.0 + ridge.variation * line_noise(mix(s.seed ^ k_ridge_salt) + r,
                                                                station, ridge.variation_length_m)
                           : 1.0;
    const f64 rate = ridge.rate_mm_per_yr * varied * k1 * k1;
    if (rate > best) best = rate;
  }
  return best;
}

f64 raster_rate(const UpliftRaster& raster, const Frame& f, u32 i, u32 j) noexcept {
  const f64 fx =
      static_cast<f64>(i) * static_cast<f64>(raster.width - 1) / static_cast<f64>(f.w - 1);
  const f64 fz =
      static_cast<f64>(j) * static_cast<f64>(raster.height - 1) / static_cast<f64>(f.h - 1);
  u32 ix = static_cast<u32>(fx);
  u32 iz = static_cast<u32>(fz);
  if (ix > raster.width - 2) ix = raster.width - 2;
  if (iz > raster.height - 2) iz = raster.height - 2;
  const f64 tx = fx - static_cast<f64>(ix);
  const f64 tz = fz - static_cast<f64>(iz);
  const f64* v = raster.rates_mm_per_yr.data();
  const f64 a = v[iz * raster.width + ix];
  const f64 b = v[iz * raster.width + ix + 1];
  const f64 c = v[(iz + 1) * raster.width + ix];
  const f64 d = v[(iz + 1) * raster.width + ix + 1];
  const f64 top = a + (b - a) * tx;
  const f64 bottom = c + (d - c) * tx;
  return top + (bottom - top) * tz;
}

// Seeded value noise in [-1, 1): lattice hashes `cells` apart, joined by a smoothstep.
f64 value_noise(u64 seed, u32 i, u32 j, u32 cells) noexcept {
  const u32 gx = i / cells;
  const u32 gz = j / cells;
  const f64 tx = static_cast<f64>(i % cells) / static_cast<f64>(cells);
  const f64 tz = static_cast<f64>(j % cells) / static_cast<f64>(cells);
  const auto corner = [seed](u32 x, u32 z) {
    return signed_unit(mix(seed ^ mix((static_cast<u64>(z) << 32) | x)));
  };
  const f64 sx = tx * tx * (3.0 - 2.0 * tx);
  const f64 sz = tz * tz * (3.0 - 2.0 * tz);
  const f64 a = corner(gx, gz);
  const f64 b = corner(gx + 1, gz);
  const f64 c = corner(gx, gz + 1);
  const f64 d = corner(gx + 1, gz + 1);
  const f64 top = a + (b - a) * sx;
  const f64 bottom = c + (d - c) * sx;
  return top + (bottom - top) * sz;
}

// The starting ground at a lattice point, metres: the start field's bilinear surface.
bool sample_start(const StartField& start, i64 x_mm, i64 z_mm, f64& out) noexcept {
  const f64 fx = static_cast<f64>(x_mm - start.origin_x_mm) / static_cast<f64>(start.spacing_mm);
  const f64 fz = static_cast<f64>(z_mm - start.origin_z_mm) / static_cast<f64>(start.spacing_mm);
  if (!(fx >= 0.0) || !(fz >= 0.0) || fx > static_cast<f64>(start.width - 1) ||
      fz > static_cast<f64>(start.height - 1))
    return false;
  u32 ix = static_cast<u32>(fx);
  u32 iz = static_cast<u32>(fz);
  if (ix > start.width - 2) ix = start.width - 2;
  if (iz > start.height - 2) iz = start.height - 2;
  const f64 tx = fx - static_cast<f64>(ix);
  const f64 tz = fz - static_cast<f64>(iz);
  const f32* v = start.data.data();
  const f64 a = static_cast<f64>(v[iz * start.width + ix]);
  const f64 b = static_cast<f64>(v[iz * start.width + ix + 1]);
  const f64 c = static_cast<f64>(v[(iz + 1) * start.width + ix]);
  const f64 d = static_cast<f64>(v[(iz + 1) * start.width + ix + 1]);
  const f64 top = a + (b - a) * tx;
  const f64 bottom = c + (d - c) * tx;
  out = (top + (bottom - top) * tz) * start.vertical_mm_per_unit / 1000.0;
  return true;
}

// ---- the rock ----------------------------------------------------------------------------------

// The factor on K and the critical slope of rock of a resistance, 0 to 1.
f64 erodibility_of(const Sketch& s, f64 resistance) noexcept {
  return 1.0 / (1.0 + resistance * (s.rock.contrast - 1.0));
}

f64 critical_of(const Sketch& s, f64 resistance) noexcept {
  return s.critical_slope + resistance * (s.critical_slope_resistant - s.critical_slope);
}

// The sketch's formation as the solver reads it (landform.md, "Structure"): the world formation's
// own definition, through its double-precision twin (`domain/gfx/formation.h`), and the sequence
// whose beds a lattice can hold: the macro beds where the formation has them, else the beds.
struct Strata {
  bool on = false;
  gfx::FormationFrame frame;
  gfx::FormationSequence beds;
};

bool read_strata(const Sketch& s, Strata& out, std::string& error) {
  out = Strata{};
  if (!s.rock.formation) return true;
  // A named default, as formation.h builds its own (GCC 13 stops on a temporary of this type).
  gfx::FormationParams block;
  std::string why;
  if (!gfx::pack_formation(*s.rock.formation, block, &why)) {
    error = "a sketch's rock." + why;
    return false;
  }
  out.on = true;
  out.frame = gfx::formation_frame(block);
  out.beds = block.macro.count > 0 ? block.macro : block.beds;
  return true;
}

// The resistance of the rock at the surface of every point, 0 to 1. With a formation it is the
// bed the surface cuts **in the rock's own frame**: a point of rock now at height z stood at
// z - raised before the uplift carried it there (`raised` the point's uplift so far), so the beds
// rise with the rock and the surface lowers through them (landform.md, "Structure"). Each point
// reads its own height and writes its own value: rows run in parallel.
void read_resistance(const Sketch& s, const Strata& strata, const Frame& frame, const f64* z,
                     const f64* lift, f64 steps_raised, jobs::JobSystem* jobs, f32* resistance) {
  const u32 w = frame.w;
  if (!strata.on) {
    const f32 uniform = static_cast<f32>(s.rock.resistance);
    for (u32 p = 0; p < w * frame.h; ++p)
      resistance[p] = uniform;
    return;
  }
  for_rows(jobs, frame.h, [&](u32 j0, u32 j1) {
    for (u32 j = j0; j < j1; ++j)
      for (u32 i = 0; i < w; ++i) {
        const u32 p = j * w + i;
        const gfx::FormationCoordinate c = gfx::formation_coordinate(
            strata.frame, frame.x(i), z[p] - lift[p] * steps_raised, frame.z(j));
        resistance[p] = gfx::formation_bed(strata.beds, c.coordinate).resistance;
      }
  });
}

// One point's slope limit (landform.md, "The slope limiter"): the gradient's length is at most
// the critical slope. From the lower neighbour along x and the lower along z it is the upwind
// step of the eikonal equation |grad z| = critical, (a + b + sqrt(2 f^2 - (a - b)^2)) / 2 with
// f the critical slope times the spacing, which keeps a failed slope a cone and not the
// eight-sided pyramid a limit along the lattice's eight directions alone makes; the four
// diagonal neighbours then hold the slope along a diagonal to the same bound, so no pair of
// neighbours stands steeper than the critical slope.
bool limit_point(f64* z, u32 i, i32 x, i32 y, i32 w, i32 h, f64 critical, f64 card,
                 f64 diag) noexcept {
  const f64 zi = z[i];
  const f64 f = critical * card;
  const f64 none = std::numeric_limits<f64>::infinity();
  f64 a = none;
  f64 b = none;
  if (x > 0 && z[i - 1] < a) a = z[i - 1];
  if (x + 1 < w && z[i + 1] < a) a = z[i + 1];
  if (y > 0 && z[i - static_cast<u32>(w)] < b) b = z[i - static_cast<u32>(w)];
  if (y + 1 < h && z[i + static_cast<u32>(w)] < b) b = z[i + static_cast<u32>(w)];
  const f64 lower = a < b ? a : b;
  const f64 gap = a < b ? b - a : a - b;
  f64 limit = zi;
  if (gap < f) {
    const f64 both = 0.5 * (a + b + std::sqrt(2.0 * f * f - gap * gap));
    if (both < limit) limit = both;
  } else if (lower + f < limit) {
    limit = lower + f;
  }
  for (u32 k = 1; k < 8; k += 2) {
    const i32 nx = x + k_receiver_dx[k];
    const i32 ny = y + k_receiver_dz[k];
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
    const f64 cand = z[static_cast<u32>(ny * w + nx)] + critical * diag;
    if (cand < limit) limit = cand;
  }
  if (limit < zi) {
    z[i] = limit;
    return true;
  }
  return false;
}

i32 clamp_i32(f64 v) noexcept {
  const f64 r = std::floor(v + 0.5);
  if (!(r > -2147483647.0)) return -2147483647;
  if (r > 2147483647.0) return 2147483647;
  return static_cast<i32>(r);
}

}  // namespace

bool validate_sketch(const Sketch& s, std::string& error) {
  const auto refuse = [&error](const char* sentence) {
    error = sentence;
    return false;
  };
  if (s.width < 8 || s.width > 16384 || s.height < 8 || s.height > 16384)
    return refuse("a sketch's width and height are 8 to 16,384 points each");
  if (s.spacing_mm < 100 || s.spacing_mm > 1000000)
    return refuse("a sketch's spacing_mm is 100 to 1,000,000");
  if (s.origin_mm[0] % s.spacing_mm != 0 || s.origin_mm[1] % s.spacing_mm != 0)
    return refuse(
        "a sketch's origin_mm is a whole number of its spacings, so its lattice is the "
        "world's at that spacing");
  if (s.base_level_m && !in_range(*s.base_level_m, -11000.0, 9000.0))
    return refuse("a sketch's base_level_m is -11,000 to 9,000 m");
  const bool sea_edge = s.edges.west == EdgeKind::Sea || s.edges.east == EdgeKind::Sea ||
                        s.edges.north == EdgeKind::Sea || s.edges.south == EdgeKind::Sea;
  if (sea_edge && !s.sea) return refuse("a sketch with a sea edge says what its sea is (sea)");
  if (s.sea) {
    if (!in_range(s.sea->level_m, -11000.0, 9000.0))
      return refuse("a sketch's sea.level_m is -11,000 to 9,000 m");
    if (!in_range(s.sea->shelf_m, 0.0, 1000000.0))
      return refuse("a sketch's sea.shelf_m is 0 to 1,000,000 m");
    if (!in_range(s.sea->depth_m, 0.0, 11000.0))
      return refuse("a sketch's sea.depth_m is 0 to 11,000 m");
  }
  const bool drains = edge_drains(s.edges.west) || edge_drains(s.edges.east) ||
                      edge_drains(s.edges.north) || edge_drains(s.edges.south);
  if (!drains && s.outlets.size() == 0)
    return refuse("a sketch whose four edges are closed names an outlet: nothing else drains it");
  for (u32 i = 0; i < s.outlets.size(); ++i) {
    const Outlet& o = s.outlets[i];
    if (!in_range(o.level_m, -11000.0, 9000.0) || !in_range(o.at.x, -1.0e9, 1.0e9) ||
        !in_range(o.at.z, -1.0e9, 1.0e9))
      return refuse("a sketch's outlet is a point in the world and a level of -11,000 to 9,000 m");
  }
  if (s.border_reach_m && !in_range(*s.border_reach_m, 0.0, 1000000.0))
    return refuse("a sketch's border_reach_m is 0 to 1,000,000 m");
  if (!in_range(s.uplift.regional_mm_per_yr, 0.0, 100.0))
    return refuse("a sketch's uplift.regional_mm_per_yr is 0 to 100");
  for (u32 r = 0; r < s.uplift.ridges.size(); ++r) {
    const Ridge& ridge = s.uplift.ridges[r];
    if (ridge.points.size() < 2) return refuse("a sketch's ridge is two points or more");
    if (!in_range(ridge.rate_mm_per_yr, 0.0, 100.0))
      return refuse("a sketch's ridge rate_mm_per_yr is 0 to 100");
    if (!in_range(ridge.reach_m, 1.0, 1000000.0))
      return refuse("a sketch's ridge reach_m is 1 to 1,000,000 m");
    if (!in_range(ridge.variation, 0.0, 0.9))
      return refuse("a sketch's ridge variation is 0 to 0.9");
    if (!in_range(ridge.variation_length_m, 10.0, 1000000.0))
      return refuse("a sketch's ridge variation_length_m is 10 to 1,000,000 m");
    for (u32 k = 0; k < ridge.points.size(); ++k)
      if (!in_range(ridge.points[k].x, -1.0e9, 1.0e9) ||
          !in_range(ridge.points[k].z, -1.0e9, 1.0e9))
        return refuse("a sketch's ridge points are finite, in the world's metres");
  }
  if (s.uplift.raster) {
    const UpliftRaster& raster = *s.uplift.raster;
    if (raster.width < 2 || raster.width > 4096 || raster.height < 2 || raster.height > 4096)
      return refuse("a sketch's uplift raster is 2 to 4,096 points a side");
    if (raster.rates_mm_per_yr.size() != raster.width * raster.height)
      return refuse("a sketch's uplift raster holds width times height rates");
    for (u32 k = 0; k < raster.rates_mm_per_yr.size(); ++k)
      if (!in_range(raster.rates_mm_per_yr[k], 0.0, 100.0))
        return refuse("a sketch's uplift raster rates are 0 to 100 mm/yr");
  }
  if (!in_range(s.rock.resistance, 0.0, 1.0)) return refuse("a sketch's rock.resistance is 0 to 1");
  if (!in_range(s.rock.contrast, 1.0, 1000.0))
    return refuse("a sketch's rock.contrast is 1 to 1,000");
  if (!in_range(s.rock.variation, 0.0, 0.9)) return refuse("a sketch's rock.variation is 0 to 0.9");
  if (s.rock.variation_cells < 2 || s.rock.variation_cells > 1024)
    return refuse("a sketch's rock.variation_cells is 2 to 1,024");
  if (s.rock.formation) {
    // The world formation's own ranges, in its own words (dip_deg, a bed's thickness_m, ...).
    std::string why;
    if (!gfx::validate_formation(*s.rock.formation, &why)) {
      error = "a sketch's rock." + why;
      return false;
    }
  }
  if (!in_range(s.erodibility_per_yr, 1.0e-8, 1.0e-2))
    return refuse("a sketch's erodibility_per_yr is 1e-8 to 1e-2");
  if (!in_range(s.channel_area_m2, 0.0, 1.0e9))
    return refuse("a sketch's channel_area_m2 is 0 to 1e9");
  if (!in_range(s.diffusivity_m2_per_yr, 0.0, 1.0))
    return refuse("a sketch's diffusivity_m2_per_yr is 0 to 1");
  if (!in_range(s.critical_slope, 0.2, 4.0)) return refuse("a sketch's critical_slope is 0.2 to 4");
  if (!in_range(s.critical_slope_resistant, s.critical_slope, 4.0))
    return refuse("a sketch's critical_slope_resistant is its critical_slope to 4");
  if (!in_range(s.deposition, 0.0, 10.0)) return refuse("a sketch's deposition is 0 to 10");
  if (!in_range(s.sediment_erodibility, 1.0, 100.0))
    return refuse("a sketch's sediment_erodibility is 1 to 100");
  if (!in_range(s.delta_share, 0.0, 1.0)) return refuse("a sketch's delta_share is 0 to 1");
  if (!in_range(s.roughness_m, 0.0, 100.0)) return refuse("a sketch's roughness_m is 0 to 100 m");
  if (s.step_yr < 1 || s.step_yr > 10000000)
    return refuse("a sketch's step_yr is 1 to 10,000,000 years");
  if (s.duration_yr < 0 || s.duration_yr / s.step_yr > 100000)
    return refuse("a sketch's duration_yr is 0 to 100,000 of its steps");
  if (s.duration_yr / s.step_yr == 0 && !s.start)
    return refuse(
        "a sketch of no steps has a starting heightfield: there is nothing else to "
        "write the fields of");
  return true;
}

bool route(u32 width, u32 height, const Vector<i32>& height_mm, const Vector<u8>& held, u64 seed,
           jobs::JobSystem* jobs, Vector<i64>& filled, Vector<u32>& order, Vector<u8>& receiver,
           Vector<u32>& area) {
  const u32 n = width * height;
  if (n == 0 || height_mm.size() != n || held.size() != n) return false;
  Routing r;
  r.filled.swap(filled);
  r.order.swap(order);
  r.receiver.swap(receiver);
  r.area.swap(area);
  const i32* mm = height_mm.data();
  const bool ok = flood(
      width, height, held.data(),
      [mm](u32 i) { return static_cast<i64>(mm[i]) * k_fill_units_per_mm; }, r);
  if (ok) {
    find_receivers(width, height, held.data(), seed, jobs, r);
    accumulate(width, height, r);
  }
  r.filled.swap(filled);
  r.order.swap(order);
  r.receiver.swap(receiver);
  r.area.swap(area);
  return ok;
}

bool solve(const Sketch& s, const StartField* start, const SolveOptions& options, Landform& out,
           std::string& error) {
  if (!validate_sketch(s, error)) return false;
  if (s.start && start == nullptr) {
    error = "the sketch names a starting heightfield and none was read";
    return false;
  }
  jobs::JobSystem* jobs = options.jobs;
  const u32 w = s.width;
  const u32 h = s.height;
  const u32 n = w * h;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(h);
  Frame frame;
  frame.w = w;
  frame.h = h;
  frame.spacing_m = static_cast<f64>(s.spacing_mm) / 1000.0;
  frame.origin_x_m = static_cast<f64>(s.origin_mm[0]) / 1000.0;
  frame.origin_z_m = static_cast<f64>(s.origin_mm[1]) / 1000.0;
  const u32 steps = static_cast<u32>(s.duration_yr / s.step_yr);
  const f64 dt = static_cast<f64>(s.step_yr);
  const bool has_sea = s.sea.has_value();
  const f64 sea = has_sea ? s.sea->level_m : 0.0;
  Strata strata;
  if (!read_strata(s, strata, error)) return false;

  // ---- the starting ground and what is held --------------------------------------------------
  Vector<f64> ground(n);
  Vector<u8> held_points(n, u8{0});
  f64* z = ground.data();
  u8* held = held_points.data();
  if (start != nullptr) {
    if (start->width < 2 || start->height < 2 || start->spacing_mm <= 0 ||
        start->data.size() != start->width * start->height) {
      error = "the starting heightfield is not a grid of two points or more a side";
      return false;
    }
    bool covered = true;
    for (u32 j = 0; j < h && covered; ++j)
      for (u32 i = 0; i < w; ++i) {
        if (!sample_start(*start, s.origin_mm[0] + static_cast<i64>(i) * s.spacing_mm,
                          s.origin_mm[1] + static_cast<i64>(j) * s.spacing_mm, z[j * w + i])) {
          covered = false;
          break;
        }
      }
    if (!covered) {
      error = "the starting heightfield does not cover the sketch's lattice";
      return false;
    }
  } else {
    const f64 plane = s.base_level_m ? *s.base_level_m : sea;
    for (u32 j = 0; j < h; ++j)
      for (u32 i = 0; i < w; ++i) {
        f64 v = plane + s.roughness_m * signed_unit(mix(s.seed ^ mix(u64{j} * w + i)));
        if (has_sea) {
          // The bed: the sea's level a shelf's width in from a sea edge, its depth at the edge.
          const f64 d = sea_distance(s, frame, i, j);
          if (d >= 0.0 && s.sea->shelf_m > 0.0) {
            const f64 bed = sea + s.sea->depth_m * (d / s.sea->shelf_m - 1.0);
            if (bed < v) v = bed;
          }
        }
        z[j * w + i] = v;
      }
  }
  const auto hold_edge = [&](EdgeKind kind, u32 i, u32 j) {
    if (kind == EdgeKind::Closed) return;
    const u32 p = j * w + i;
    held[p] = 1;
    if (kind == EdgeKind::Open) {
      if (s.base_level_m) {
        z[p] = *s.base_level_m;
      } else if (start == nullptr) {
        z[p] = sea;
      }
    } else if (start == nullptr) {
      z[p] = sea - s.sea->depth_m;
    }
  };
  for (u32 j = 0; j < h; ++j) {
    hold_edge(s.edges.west, 0, j);
    hold_edge(s.edges.east, w - 1, j);
  }
  for (u32 i = 0; i < w; ++i) {
    hold_edge(s.edges.north, i, 0);
    hold_edge(s.edges.south, i, h - 1);
  }
  for (u32 k = 0; k < s.outlets.size(); ++k) {
    const Outlet& o = s.outlets[k];
    const f64 fx = std::floor((o.at.x - frame.origin_x_m) / frame.spacing_m + 0.5);
    const f64 fz = std::floor((o.at.z - frame.origin_z_m) / frame.spacing_m + 0.5);
    if (!(fx >= 0.0) || !(fz >= 0.0) || fx > static_cast<f64>(w - 1) ||
        fz > static_cast<f64>(h - 1)) {
      error = "a sketch's outlet lies inside its lattice";
      return false;
    }
    const u32 p = static_cast<u32>(fz) * w + static_cast<u32>(fx);
    held[p] = 1;
    z[p] = o.level_m;
  }
  for (u32 p = 0; p < n; ++p)
    if (!in_range(z[p], -k_max_height_m, k_max_height_m)) {
      error =
          "the starting ground leaves -16,000 to 16,000 m, where a heightfield's float no "
          "longer holds a whole millimetre";
      return false;
    }

  // ---- what each step adds and how hard each point is ---------------------------------------
  Vector<f64> lift_field(n);      // metres a step
  Vector<f64> hardness_field(n);  // K dt times the point's variation
  Vector<f64> settle_field(n);    // G, tapered with the uplift toward an edge that drains
  f64* lift = lift_field.data();
  f64* hardness = hardness_field.data();
  f64* settle = settle_field.data();
  const f64 reach = s.border_reach_m ? *s.border_reach_m : 16.0 * frame.spacing_m;
  const f64 k_dt = s.erodibility_per_yr * dt;
  for_rows(jobs, h, [&](u32 j0, u32 j1) {
    for (u32 j = j0; j < j1; ++j)
      for (u32 i = 0; i < w; ++i) {
        const u32 p = j * w + i;
        f64 rate = s.uplift.regional_mm_per_yr + ridge_rate(s, frame.x(i), frame.z(j));
        if (s.uplift.raster) rate += raster_rate(*s.uplift.raster, frame, i, j);
        const f64 d = drain_distance(s, frame, i, j);
        f64 taper = 1.0;
        if (d >= 0.0 && reach > 0.0 && d < reach) {
          const f64 t = d / reach;
          taper = t * t * (3.0 - 2.0 * t);
        }
        rate *= taper;
        settle[p] = s.deposition * taper;
        lift[p] = held[p] != 0 ? 0.0 : rate * dt / 1000.0;
        hardness[p] =
            k_dt * (1.0 + s.rock.variation * value_noise(s.seed, i, j, s.rock.variation_cells));
      }
  });

  // ---- the steps -----------------------------------------------------------------------------
  Routing routing;
  Vector<f64> scratch_field(n);
  Vector<f64> before_field;  // the last step's ground after its uplift: the erosion rate's datum
  const Offsets o = neighbour_offsets(w);
  const f64 card = frame.spacing_m;
  const f64 diag = frame.spacing_m * k_sqrt2;
  const f64 number = s.diffusivity_m2_per_yr * dt / (frame.spacing_m * frame.spacing_m);
  const u32 substeps = number > 0.0 ? static_cast<u32>(number / k_diffusion_number) + 1 : 0;
  const f64 rr = substeps > 0 ? number / static_cast<f64>(substeps) : 0.0;
  // The channel threshold in cells: a point draining fewer is hillslope.
  const f64 channel_cells = s.channel_area_m2 / (frame.spacing_m * frame.spacing_m);
  // Sediment (landform.md, "Deposition"): its thickness, the iterate of the rivers' solve, what
  // each point's slope shed last step and no river has carried yet, and the budget, all in
  // metres over one cell until the end.
  Vector<f64> sediment_field(n, 0.0);
  Vector<f64> iterate_field(n);
  Vector<f64> carry_field(n, 0.0);
  Vector<LakeCell> lake_scratch;
  Vector<f32> resistance_field(n);  // the rock at each point's surface this step, 0 to 1
  f32* resist = resistance_field.data();
  read_resistance(s, strata, frame, z, lift, 0.0, jobs, resist);
  f64* sed = sediment_field.data();
  f64* hk = iterate_field.data();
  f64* carry = carry_field.data();
  const f64 loose_dt = k_dt * s.sediment_erodibility;
  const auto total = [z, n]() {
    f64 sum = 0.0;
    for (u32 p = 0; p < n; ++p)
      sum += z[p];
    return sum;
  };
  const f64 volume_start = total();
  f64 uplifted = 0.0, eroded = 0.0, laid = 0.0, left = 0.0, crept = 0.0, settled = 0.0;
  u32 sweeps = 0;
  for (u32 step = 0; step < steps; ++step) {
    for (u32 p = 0; p < n; ++p) {
      z[p] += lift[p];
      uplifted += lift[p];
    }
    // The beds rose with the rock; the surface has lowered through them since the last step.
    if (strata.on)
      read_resistance(s, strata, frame, z, lift, static_cast<f64>(step + 1), jobs, resist);
    if (step + 1 == steps) {
      before_field.resize(n);
      f64* before = before_field.data();
      for (u32 p = 0; p < n; ++p)
        before[p] = z[p];
    }
    if (!flood(w, h, held, [z](u32 i) { return quantize(z[i]); }, routing)) {
      error = "the flood did not reach every point";
      return false;
    }
    find_lakes(n, [z](u32 i) { return quantize(z[i]); }, routing);
    find_receivers(w, h, held, s.seed ^ k_route_salt, jobs, routing);
    accumulate(w, h, routing);
    const u32* order = routing.order.data();
    const u8* receiver = routing.receiver.data();
    const u32* area = routing.area.data();
    const u32* sill = routing.sill.data();
    // What a point is to its river this step: held, standing water (a lake's member, or under
    // the sea), a slope too small to hold a channel, or a reach.
    const auto still = [&](u32 p) {
      const u32 tag = sill[p];
      return (tag != k_no_sill && (tag & k_sill_flag) == 0) || (has_sea && z[p] <= sea);
    };
    // The rivers: erosion and deposition together, implicit (Yuan, Braun, Guerit, Rouby and
    // Cordonnier 2019, their equation 20). A reach moves to (z + d + F z_r) / (1 + F), F = K dt
    // sqrt(N) / d in cells (the spacing cancels at m = 1/2, n = 1), d the deposit: G / N of the
    // flux that reaches it, which is what the points above lost. The flux depends on the answer,
    // so it is found by sweeps: the flux of the last iterate, each point after its donors, then
    // the heights, each point after its receiver, until no height moves by `k_river_tolerance_m`.
    // z stays the ground before the rivers; `hk` is the iterate. A river cuts to the sea and no
    // lower, and standing water neither cuts nor lays down here: it takes what reaches it below.
    f64* flux = scratch_field.data();
    for (u32 p = 0; p < n; ++p)
      hk[p] = z[p];
    for (u32 sweep = 0; sweep < k_river_sweeps; ++sweep) {
      ++sweeps;
      for (u32 p = 0; p < n; ++p)
        flux[p] = 0.0;
      for (u32 k = n; k-- > 0;) {
        const u32 p = order[k];
        const u8 code = receiver[p];
        if (code == k_no_receiver) continue;
        const u32 tag = sill[p];
        if (tag != k_no_sill && (tag & k_sill_flag) == 0) continue;  // a lake keeps what it gets
        const f64 passed = flux[p] + carry[p] + (z[p] - hk[p]);
        if (passed > 0.0) flux[neighbour(p, o.at[code])] += passed;
      }
      f64 worst = 0.0;
      for (u32 k = 0; k < n; ++k) {
        const u32 p = order[k];
        const u8 code = receiver[p];
        if (code == k_no_receiver || still(p)) continue;
        const f64 cells = static_cast<f64>(area[p]);
        if (cells < channel_cells) continue;  // hillslope: no river here
        const f64 down = flux[p] * (settle[p] < cells ? settle[p] / cells : 1.0);
        const f64 top = z[p] + down;
        f64 base = hk[neighbour(p, o.at[code])];
        if (has_sea && base < sea) base = sea;
        f64 to = top;
        if (top > base) {
          // Sediment first, at its own erodibility: the point's own and what this step lays.
          // Where the cut goes through it into rock the height is the one at which the time to
          // strip the sediment and the time to cut the rock make the step, each at the rate the
          // final slope gives, which is the single formula again when the two erode alike.
          const f64 power = std::sqrt(cells) * ((code & 1u) != 0 ? k_inv_sqrt2 : 1.0);
          const f64 loose = sed[p] + down;
          const f64 fs = loose_dt * power;
          const f64 through = (top + fs * base) / (1.0 + fs);
          if (top - through <= loose) {
            to = through;
          } else {
            const f64 hard = hardness[p] * erodibility_of(s, static_cast<f64>(resist[p]));
            const f64 fr = hard * power;
            to = (top - loose * (1.0 - hard / loose_dt) + fr * base) / (1.0 + fr);
          }
        }
        const f64 moved = to > hk[p] ? to - hk[p] : hk[p] - to;
        if (moved > worst) worst = moved;
        hk[p] = to;
      }
      if (worst < k_river_tolerance_m) break;
    }
    // The solve's result, each point after its donors, with the books kept on what each point
    // truly lost or gained: nothing is laid down that did not arrive. Standing water takes its
    // share here, and a sill what its lake could not hold.
    for (u32 p = 0; p < n; ++p)
      flux[p] = 0.0;
    Lake* lakes = routing.lakes.data();
    const bool settles = s.deposition > 0.0;
    for (u32 k = n; k-- > 0;) {
      const u32 p = order[k];
      f64 q = flux[p];
      const u32 tag = sill[p];
      const bool is_sill = tag != k_no_sill && (tag & k_sill_flag) != 0;
      if (is_sill && lakes[tag & ~k_sill_flag].pool > 0.0)
        q += fill_lake(routing, lakes[tag & ~k_sill_flag], z, sed, lake_scratch, laid);
      if (held[p] != 0) {
        left += q;
        continue;
      }
      q += carry[p];
      carry[p] = 0.0;
      const u32 below = receiver[p] != k_no_receiver ? neighbour(p, o.at[receiver[p]]) : p;
      if (!settles) {
        // G = 0: nothing settles, in standing water either. What is cut leaves.
        const f64 lost = z[p] - hk[p];
        eroded += lost;
        sed[p] = sed[p] > lost ? sed[p] - lost : 0.0;
        z[p] = hk[p];
        flux[below] += q + lost;
        continue;
      }
      if (tag != k_no_sill && !is_sill) {
        // A lake: a share settles at the mouth, up to the water's level; the rest, and what
        // the mouth had no room for, joins the lake's pool and levels its floor.
        Lake& lake = lakes[sill[tag] & ~k_sill_flag];
        if (q > 0.0) {
          const f64 mouth = q * s.delta_share;
          lake.pool += q - mouth + spread(z, sed, held, p, mouth, z[lake.sill], w, h, laid);
        }
        continue;
      }
      if (has_sea && z[p] <= sea) {
        // The sea: everything settles, the deepest ground about the point first, to the sea's
        // level and no higher; what that leaves goes on along the bed.
        if (q > 0.0) flux[below] += spread(z, sed, held, p, q, sea, w, h, laid);
        continue;
      }
      f64 lost = z[p] - hk[p];
      if (q + lost < 0.0) lost = -q;  // an iterate that laid down more than arrived
      if (lost > 0.0) {
        eroded += lost;
        sed[p] = sed[p] > lost ? sed[p] - lost : 0.0;
      } else {
        laid -= lost;
        sed[p] -= lost;
      }
      z[p] -= lost;
      flux[below] += q + lost;
    }
    // Hillslopes: a Jacobi step reads one surface and writes the other, so rows run in parallel.
    // What creep lowers comes off the sediment first, and what it raises is sediment.
    const f64 before_creep = substeps > 0 ? total() : 0.0;
    f64* from = z;
    f64* to = scratch_field.data();
    for (u32 sub = 0; sub < substeps; ++sub) {
      const f64* src = from;
      f64* dst = to;
      for_rows(jobs, h, [&](u32 j0, u32 j1) {
        for (u32 j = j0; j < j1; ++j)
          for (u32 i = 0; i < w; ++i) {
            const u32 p = j * w + i;
            const f64 c = src[p];
            if (held[p] != 0) {
              dst[p] = c;
              continue;
            }
            f64 sum = 0.0;
            if (i > 0) sum += src[p - 1] - c;
            if (i + 1 < w) sum += src[p + 1] - c;
            if (j > 0) sum += src[p - w] - c;
            if (j + 1 < h) sum += src[p + w] - c;
            const f64 moved = rr * sum;
            dst[p] = c + moved;
            const f64 loose = sed[p] + moved;
            sed[p] = loose > 0.0 ? loose : 0.0;
          }
      });
      f64* t = from;
      from = to;
      to = t;
    }
    if (from != z)
      for (u32 p = 0; p < n; ++p)
        z[p] = from[p];
    if (substeps > 0) crept += before_creep - total();
    // The limiter in flood order: nearly ascending, so one pass settles almost every slope, and
    // the pass after the last step settles the rest. What a slope sheds is the next step's
    // rivers' to carry from that point.
    for (u32 k = 0; k < n; ++k) {
      const u32 p = order[k];
      if (held[p] != 0) continue;
      const f64 zp = z[p];
      if (!limit_point(z, p, static_cast<i32>(p % w), static_cast<i32>(p / w), wi, hi,
                       critical_of(s, static_cast<f64>(resist[p])), card, diag))
        continue;
      const f64 fell = zp - z[p];
      carry[p] = fell;
      eroded += fell;
      sed[p] = sed[p] > fell ? sed[p] - fell : 0.0;
    }
  }
  // What the last step's slopes shed no river carried: it leaves with the settling below.
  f64 unrouted = 0.0;
  for (u32 p = 0; p < n; ++p)
    unrouted += carry[p];
  if (steps > 0) {
    f64* stepped = scratch_field.data();
    for (u32 p = 0; p < n; ++p)
      stepped[p] = z[p];
    // To convergence: sweeps in the four diagonal orders until a round moves nothing. Heights
    // only fall, and a point's limit depends on lower points alone, so it ends; the bound on the
    // rounds is a guard, not a budget (a few rounds settle a range).
    for (u32 round = 0; round < 4096; ++round) {
      bool moved = false;
      for (u32 pass = 0; pass < 4; ++pass) {
        const bool x_up = (pass & 1u) == 0;
        const bool y_up = (pass & 2u) == 0;
        for (i32 jj = 0; jj < hi; ++jj) {
          const i32 y = y_up ? jj : hi - 1 - jj;
          for (i32 ii = 0; ii < wi; ++ii) {
            const i32 x = x_up ? ii : wi - 1 - ii;
            const u32 p = static_cast<u32>(y * wi + x);
            if (held[p] != 0) continue;
            moved |= limit_point(z, p, x, y, wi, hi, critical_of(s, static_cast<f64>(resist[p])),
                                 card, diag);
          }
        }
      }
      if (!moved) break;
    }
    // What the last settling took has nowhere to go: it leaves the model, and the budget says so.
    for (u32 p = 0; p < n; ++p) {
      const f64 fell = stepped[p] - z[p];
      settled += fell;
      sed[p] = sed[p] > fell ? sed[p] - fell : 0.0;
    }
  }

  // ---- the outputs: whole millimetres, and the fields of exactly that ground -----------------
  out = Landform{};
  const f64 cell_m2 = frame.spacing_m * frame.spacing_m;
  out.budget.uplifted_m3 = uplifted * cell_m2;
  out.budget.eroded_m3 = eroded * cell_m2;
  out.budget.deposited_m3 = laid * cell_m2;
  out.budget.left_m3 = left * cell_m2;
  out.budget.crept_out_m3 = crept * cell_m2;
  out.budget.settled_m3 = settled * cell_m2;
  out.budget.unrouted_m3 = unrouted * cell_m2;
  out.budget.change_m3 = (total() - volume_start) * cell_m2;
  out.river_sweeps = sweeps;
  out.sediment.resize(n);
  for (u32 p = 0; p < n; ++p)
    out.sediment[p] = static_cast<u32>(clamp_i32(sed[p] * 1000.0));
  // The rock at the solved surface, where a later solver looks for the beds that stand out.
  read_resistance(s, strata, frame, z, lift, static_cast<f64>(steps), jobs, resist);
  out.resistance.resize(n);
  for (u32 p = 0; p < n; ++p)
    out.resistance[p] = static_cast<u8>(clamp_i32(static_cast<f64>(resist[p]) * 255.0));
  out.width = w;
  out.height = h;
  out.spacing_mm = s.spacing_mm;
  out.origin_x_mm = s.origin_mm[0];
  out.origin_z_mm = s.origin_mm[1];
  out.has_sea = has_sea;
  out.sea_level_mm = has_sea ? static_cast<i64>(std::floor(sea * 1000.0 + 0.5)) : 0;
  out.steps = steps;
  out.step_yr = s.step_yr;
  out.route_seed = s.seed ^ k_route_salt;
  out.height_mm.resize(n);
  out.erosion_rate.assign(n, 0);
  out.slope.resize(n);
  out.fill.resize(n);
  i32* mm = out.height_mm.data();
  i32* erosion = out.erosion_rate.data();
  bool in_bounds = true;
  for (u32 p = 0; p < n; ++p) {
    if (!in_range(z[p], -k_max_height_m, k_max_height_m)) in_bounds = false;
    mm[p] = clamp_i32(z[p] * 1000.0);
  }
  if (!in_bounds) {
    error =
        "the solved ground leaves -16,000 to 16,000 m, where a heightfield's float no longer "
        "holds a whole millimetre";
    return false;
  }
  if (steps > 0) {
    const f64* before = before_field.data();
    for (u32 p = 0; p < n; ++p)
      erosion[p] = held[p] != 0 ? 0 : clamp_i32((before[p] - z[p]) / dt * 1.0e6);
  }
  if (!flood(
          w, h, held, [mm](u32 i) { return static_cast<i64>(mm[i]) * k_fill_units_per_mm; },
          routing)) {
    error = "the flood did not reach every point";
    return false;
  }
  find_receivers(w, h, held, s.seed ^ k_route_salt, jobs, routing);
  accumulate(w, h, routing);
  const i64* filled = routing.filled.data();
  const u8* receiver = routing.receiver.data();
  i32* slope = out.slope.data();
  u32* fill = out.fill.data();
  const f64 card_mm = static_cast<f64>(s.spacing_mm);
  const f64 diag_mm = card_mm * k_sqrt2;
  for_rows(jobs, h, [&](u32 j0, u32 j1) {
    for (u32 p = j0 * w; p < j1 * w; ++p) {
      const u8 code = receiver[p];
      if (code == k_no_receiver) {
        slope[p] = 0;
      } else {
        const i64 drop = static_cast<i64>(mm[p]) - mm[neighbour(p, o.at[code])];
        slope[p] =
            clamp_i32(static_cast<f64>(drop) * 1.0e6 / ((code & 1u) != 0 ? diag_mm : card_mm));
      }
      const i64 depth =
          (filled[p] - static_cast<i64>(mm[p]) * k_fill_units_per_mm) / k_fill_units_per_mm;
      fill[p] = static_cast<u32>(depth);
    }
  });
  out.receiver.swap(routing.receiver);
  out.drainage_area.swap(routing.area);
  return true;
}

}  // namespace engine::landform
