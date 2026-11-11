// The landform solver (docs/subsystems/landform.md, "The step"; docs/experiments/landform.md has
// every source and constant). One step: uplift, the flood and its receivers, the drainage area,
// the rivers (stream-power incision and deposition solved together along the flood's order, the
// lakes and the sea taking what reaches them), hillslope diffusion, the slope limiter. Every
// decision is IEEE `+ - * /`, `sqrt`, `floor` and integers; the loops the job system runs are the
// ones whose points read nothing another point of the same loop writes.
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>
#include <domain/gfx/formation.h>
#include <domain/landform/landform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
// And the uplift's unevenness over the plan.
constexpr u64 k_plan_salt = 0x5851F42D4C957F2Dull;
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
  Vector<u64> members;    // sill above cell, sorted: each lake's cells are one run
  Vector<u64> sort_from;  // the order's sort: height above index, and its other buffer
  Vector<u64> sort_to;
  Vector<Lake> lakes;
};

// The flood of every point, as Barnes et al. state it: the queue seeded by the held points, the
// order the pops'. What `flood` falls back to where most of the ground might not drain (a rough
// plain, the first steps of a run), since there the queue has nearly every point anyway and the
// sort would be work on top of it.
template <class Q>
bool flood_all(u32 w, u32 h, const u8* held, Q&& units, Routing& r) {
  const u32 n = w * h;
  r.filled.assign(n, k_unvisited);
  r.heap.clear();
  i64* filled = r.filled.data();
  u32* order = r.order.data();
  u32* sill = r.sill.data();
  for (u32 i = 0; i < n; ++i) {
    if (held[i] == 0) continue;
    filled[i] = units(i);
    heap_push(r.heap, key_of(filled[i], i));
  }
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
      if (own <= fc) sill[ni] = sill[c] != k_no_sill ? sill[c] : c;
      heap_push(r.heap, key_of(f, ni));
    }
  }
  return count == n;
}

// Priority-Flood with epsilon (Barnes, Lehman and Mulla 2014, Algorithm 3) from the held points://
// pop the lowest (height, index), raise each unvisited neighbour to at least one unit above it. A
// push is never below the pop that made it, so pops ascend, and `order` is both the flood's order
// and the flow tree's: a point's receiver, which is strictly lower on the filled surface, was
// popped before it.
//
// **The queue only sees the ground that might not drain** (landform.md, "The flood's cost"). A
// point with a path to a held point that falls by a unit or more at every step is never raised:
// its filled height is its own. So the lattice is first walked in memory order: each point
// finds its lowest neighbour, and a point whose chain of lowest neighbours ends at a held point
// is **drained** and done. The queue then floods only the rest, seeded by the drained points
// that touch them, which pop in the same order among themselves as they would have among all
// the points; and the order is the sort of every point by (filled height, index), which is what
// the pops of the whole lattice were. The filled surface, the sills and the order are the same
// bytes as the flood of every point gives (the tests' pins did not move). Where more than half the
// points might not drain the flood of every point is run as it is (`flood_all`).
template <class Q>
bool flood(u32 w, u32 h, const u8* held, Q&& units, jobs::JobSystem* jobs, Routing& r) {
  const u32 n = w * h;
  r.filled.resize(n);
  r.order.resize(n);
  r.receiver.resize(n);
  r.sill.assign(n, k_no_sill);
  r.heap.clear();
  i64* filled = r.filled.data();
  u32* order = r.order.data();
  u32* sill = r.sill.data();
  // Until the receivers are found their array holds each point's state, and until the order is
  // sorted its array holds each point's lowest neighbour.
  u8* state = r.receiver.data();
  u32* lowest = order;
  constexpr u8 k_unknown = 0, k_drained = 1, k_undrained = 2, k_seed = 3;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(h);
  for_rows(jobs, h, [&](u32 z0, u32 z1) {
    for (u32 p = z0 * w; p < z1 * w; ++p)
      filled[p] = units(p);
  });
  for_rows(jobs, h, [&](u32 z0, u32 z1) {
    for (u32 z = z0; z < z1; ++z)
      for (u32 x = 0; x < w; ++x) {
        const u32 p = z * w + x;
        if (held[p] != 0) {
          state[p] = k_drained;
          lowest[p] = p;
          continue;
        }
        u32 best = p;
        i64 best_height = filled[p];
        for (u32 k = 0; k < 8; ++k) {
          const i32 nx = static_cast<i32>(x) + k_receiver_dx[k];
          const i32 nz = static_cast<i32>(z) + k_receiver_dz[k];
          if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) continue;
          const u32 ni = static_cast<u32>(nz) * w + static_cast<u32>(nx);
          if (filled[ni] < best_height) {
            best_height = filled[ni];
            best = ni;
          }
        }
        state[p] = k_unknown;
        lowest[p] = best;  // itself: a pit or a flat
      }
  });
  u32 held_points = 0;
  for (u32 p = 0; p < n; ++p) {
    if (held[p] != 0) ++held_points;
    if (state[p] != k_unknown) continue;
    u32 q = p;
    while (state[q] == k_unknown && lowest[q] != q)
      q = lowest[q];
    const u8 found = state[q] == k_drained ? k_drained : k_undrained;
    for (u32 c = p; state[c] == k_unknown; c = lowest[c]) {
      state[c] = found;
      if (lowest[c] == c) break;
    }
  }
  if (held_points == 0) return false;
  u32 pending = 0;
  for (u32 p = 0; p < n; ++p)
    if (state[p] == k_undrained) ++pending;
  if (u64{pending} * 2 > n) return flood_all(w, h, held, units, r);
  // The queue's part: the points that might be raised, from the drained points beside them.
  for (u32 p = 0; p < n; ++p) {
    if (state[p] != k_undrained) continue;
    const i32 px = static_cast<i32>(p % w);
    const i32 pz = static_cast<i32>(p / w);
    for (u32 k = 0; k < 8; ++k) {
      const i32 nx = px + k_receiver_dx[k];
      const i32 nz = pz + k_receiver_dz[k];
      if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) continue;
      const u32 ni = static_cast<u32>(nz) * w + static_cast<u32>(nx);
      if (state[ni] != k_drained) continue;
      state[ni] = k_seed;
      heap_push(r.heap, key_of(filled[ni], ni));
    }
  }
  for (u32 p = 0; p < n; ++p)
    if (state[p] == k_undrained) filled[p] = k_unvisited;
  u32 count = 0;
  while (r.heap.size() != 0) {
    const u64 key = heap_pop(r.heap);
    const u32 c = static_cast<u32>(key & k_index_mask);
    const i64 fc = static_cast<i64>(key >> k_index_bits) - k_height_bias;
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
      ++count;
      // A raised point belongs to the depression of the point that raised it, or begins one
      // that spills over that point.
      if (own <= fc) sill[ni] = sill[c] != k_no_sill ? sill[c] : c;
      heap_push(r.heap, key_of(f, ni));
    }
  }
  if (count != pending) return false;
  // The order: every point by (filled height, index). The points start in index order, so a
  // stable sort by height alone is the whole key: twelve bits a pass over the bits the heights
  // span.
  i64 low = filled[0], high = filled[0];
  for (u32 p = 1; p < n; ++p) {
    if (filled[p] < low) low = filled[p];
    if (filled[p] > high) high = filled[p];
  }
  r.sort_from.resize(n);
  r.sort_to.resize(n);
  u64* from = r.sort_from.data();
  u64* to = r.sort_to.data();
  for_rows(jobs, h, [&](u32 z0, u32 z1) {
    for (u32 p = z0 * w; p < z1 * w; ++p)
      from[p] = (static_cast<u64>(filled[p] - low) << k_index_bits) | p;
  });
  constexpr u32 k_digit_bits = 12;
  constexpr u32 k_buckets = 1u << k_digit_bits;
  u32 start[k_buckets];
  const u64 span = static_cast<u64>(high - low);
  for (u32 shift = k_index_bits; shift < 64 && (span >> (shift - k_index_bits)) != 0;
       shift += k_digit_bits) {
    for (u32 b = 0; b < k_buckets; ++b)
      start[b] = 0;
    for (u32 p = 0; p < n; ++p)
      ++start[(from[p] >> shift) & (k_buckets - 1)];
    u32 running = 0;
    for (u32 b = 0; b < k_buckets; ++b) {
      const u32 here = start[b];
      start[b] = running;
      running += here;
    }
    for (u32 p = 0; p < n; ++p)
      to[start[(from[p] >> shift) & (k_buckets - 1)]++] = from[p];
    u64* t = from;
    from = to;
    to = t;
  }
  for_rows(jobs, h, [&](u32 z0, u32 z1) {
    for (u32 p = z0 * w; p < z1 * w; ++p)
      order[p] = static_cast<u32>(from[p] & k_index_mask);
  });
  return true;
}

// The lakes of a flooded ground (landform.md, "Deposition"): every raised point standing
// `k_lake_min_units` or more under its filled level, grouped by the sill its depression spills
// over. Shallower raised points lose their sill and count as dry. A sorted run a lake, so the
// table is the same whatever order the flood met the points in.
// A point under a glacier (`iced`, where given) is no lake's: the ice crosses a hollow in its bed
// and wears it, and the hollow is a lake once the ice has gone.
template <class Q>
void find_lakes(u32 n, Q&& units, const u8* iced, Routing& r) {
  r.members.clear();
  r.lakes.clear();
  u32* sill = r.sill.data();
  const i64* filled = r.filled.data();
  for (u32 p = 0; p < n; ++p) {
    if (sill[p] == k_no_sill) continue;
    if (filled[p] - units(p) >= k_lake_min_units && (iced == nullptr || iced[p] == 0)) {
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

// Seeded noise over the plan, in [-1, 1]: the same four octaves as `line_noise`, of value noise
// on a square lattice of the world's own metres, so a place has its number whatever lattice
// reads it (landform.md, "Valleys that are not a comb").
f64 plan_noise(u64 seed, f64 x, f64 z, f64 length) noexcept {
  f64 sum = 0.0;
  f64 amplitude = 1.0;
  f64 wavelength = length;
  for (u32 octave = 0; octave < 4; ++octave) {
    const f64 ax = x / wavelength;
    const f64 az = z / wavelength;
    const f64 cx = std::floor(ax);
    const f64 cz = std::floor(az);
    const f64 tx = ax - cx;
    const f64 tz = az - cz;
    const u64 ix = static_cast<u64>(static_cast<i64>(cx));
    const u64 iz = static_cast<u64>(static_cast<i64>(cz));
    const auto corner = [seed, octave](u64 gx, u64 gz) {
      return signed_unit(mix(seed ^ mix((gz << 32) ^ (gx & 0xFFFFFFFFull) ^ (u64{octave} << 60))));
    };
    const f64 sx = tx * tx * (3.0 - 2.0 * tx);
    const f64 sz = tz * tz * (3.0 - 2.0 * tz);
    const f64 a = corner(ix, iz);
    const f64 b = corner(ix + 1, iz);
    const f64 c = corner(ix, iz + 1);
    const f64 d = corner(ix + 1, iz + 1);
    const f64 front = a + (b - a) * sx;
    const f64 back = c + (d - c) * sx;
    sum += amplitude * (front + (back - front) * sz);
    amplitude *= k_inv_sqrt2;
    wavelength *= 0.5;
  }
  return sum / 2.5606601717798214;
}

// The largest ridge kernel at a point, mm/yr: (1 - (r / reach)^2)^2 of the line's rate, the rate
// that of the line's nearest station.
f64 ridge_rate(const Sketch& s, f64 px, f64 pz, f64& envelope) noexcept {
  f64 best = 0.0;
  envelope = 0.0;
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
    // What the unevenness over the plan is a fraction of: the line's rate falling off as the
    // kernel's square root, so a flank is as uneven as the crest.
    if (ridge.rate_mm_per_yr * k1 > envelope) envelope = ridge.rate_mm_per_yr * k1;
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

// ---- ice ---------------------------------------------------------------------------------------
//
// Valley glaciers by Hergarten 2021's stream-power form (landform.md, "Ice";
// docs/experiments/landform.md, "Ice: the sources"). The ice's thickness is not solved from its
// flow: a glacier is as wide as its flux makes it (w ~ flux^0.3) and as thick as a fixed part of
// its width, its flux is what its catchment made less what melted, routed down the bed's own
// receivers, and it wears its bed by the same law the rivers do with its own erodibility, its
// own "area" (the flux, spread over its width) and the slope of its own surface.

// A glacier's width goes with its flux to this power (Hergarten 2021, equation 13: Bahr 1997's
// width to length, 0.6, times the catchment's length to area, 0.52).
constexpr f64 k_ice_width_power = 0.3;
// A point looks for the glacier that holds it no farther than this many points down its line.
constexpr i32 k_ice_reach_cells = 96;
// The walk down a glacier's line to where its erosion reads the ice's surface ends after this
// many points whatever the ice's thickness.
constexpr u32 k_ice_coupling_points = 64;

struct IceField {
  bool on = false;  // the ice stands this step
  f64 erodibility = 0.0;
  Vector<f64> flux;      // the ice a point passes on, in cells making ice in full
  Vector<f64> arriving;  // what reaches it from above
  Vector<f64> reach;     // the largest flux of a glacier whose swath holds the point
  Vector<f64> own;       // the ice the point's own flux makes, metres (its line's thickness)
  Vector<f64> thick;     // the ice over the point this step, metres
  Vector<f64> before;    // and last step
  Vector<f64> melt;      // the share of the ice reaching the point that melts there
  Vector<f64> add;       // till to lay once the step's books are done
  Vector<f64> till;      // till laid so far
  Vector<u8> iced;       // the point is under a glacier this step
  Vector<u8> was;        // and last step
  // The point each iced point's erosion reads the ice's surface at, a coupling's reach down
  // its line, and how far that is in spacings (landform.md, "Basins the ice digs").
  Vector<u32> far;
  Vector<f64> far_cells;
};

f64 ice_width_m(const Ice& c, f64 cells, f64 cell_km2) noexcept {
  return c.width_m * det::exp(k_ice_width_power * det::log(cells * cell_km2));
}

// This step's ice: the flux, the swaths, the thickness, what melts where.
void glaciate(const Sketch& s, const Frame& frame, const u8* held, const f64* z, const Routing& r,
              IceField& ice) {
  const Ice& c = *s.ice;
  const u32 w = frame.w;
  const u32 n = w * frame.h;
  const Offsets o = neighbour_offsets(w);
  const u32* order = r.order.data();
  const u8* receiver = r.receiver.data();
  const f64 cell_km2 = frame.spacing_m * frame.spacing_m * 1.0e-6;
  ice.before.swap(ice.thick);
  ice.was.swap(ice.iced);
  const u8* was = ice.was.data();
  f64* flux = ice.flux.data();
  f64* arriving = ice.arriving.data();
  f64* reach = ice.reach.data();
  f64* thick = ice.thick.data();
  const f64* before = ice.before.data();
  f64* melt = ice.melt.data();
  u8* iced = ice.iced.data();
  for (u32 p = 0; p < n; ++p) {
    flux[p] = 0.0;
    arriving[p] = 0.0;
    reach[p] = 0.0;
    thick[p] = 0.0;
    melt[p] = 0.0;
    iced[p] = 0;
  }
  // The flux, every donor before its receiver (Hergarten 2021, equation 21, kept to the ice). A
  // point adds the share of its snowfall that becomes ice, read on the ice's surface as it
  // stood last step: above the line a part of it, up to all; below the line a loss. Bare
  // ground below the line makes and loses nothing. A point the glacier covered last step
  // passes its loss on down the bed's receivers, toward the glacier's line, so **a glacier
  // pays for the melt over the whole of its swath** and ends where its catchment's ice is
  // spent. (Hergarten's sum takes the loss of every point of the catchment, ice on it or not;
  // taking it of the glacier's own surface is the same balance with the glacier's own area.)
  for (u32 k = n; k-- > 0;) {
    const u32 p = order[k];
    if (held[p] != 0) continue;
    const f64 above = z[p] + before[p] - c.ela_m;
    f64 made = above >= 0.0 ? above / c.full_m : above / c.melt_m;
    if (made > 1.0) made = 1.0;
    const f64 from_above = arriving[p];
    if (made < 0.0 && !(from_above > 0.0) && was[p] == 0) continue;
    const f64 passed = from_above + made;
    if (passed > 0.0) {
      flux[p] = passed;
      if (made < 0.0) melt[p] = -made < from_above ? -made / from_above : 1.0;
      arriving[neighbour(p, o.at[receiver[p]])] += passed;
    } else {
      if (from_above > 0.0) melt[p] = 1.0;
      if (was[p] != 0) arriving[neighbour(p, o.at[receiver[p]])] += passed;
    }
  }
  // Each line's own ice: as thick as its flux makes it, and half its width in points.
  f64* own = ice.own.data();
  const f64 half_cells = 0.5 / (c.thickness_ratio * frame.spacing_m);
  i32 widest = 0;
  for (u32 p = 0; p < n; ++p) {
    own[p] = 0.0;
    if (!(flux[p] > 0.0)) continue;
    own[p] = c.thickness_ratio * ice_width_m(c, flux[p], cell_km2);
    i32 radius = static_cast<i32>(own[p] * half_cells);
    if (radius > k_ice_reach_cells) radius = k_ice_reach_cells;
    if (radius > widest) widest = radius;
  }
  // The swaths (Hergarten 2021, section 3: "Let u be any donor of c0 ... The point u is added
  // to the swath and assigned to c0 if the distance between u and ci is not greater than"
  // half the width at ci "for any i >= 0", ci the points down the line from c0; "the same
  // procedure is applied to all donors of these points", and a point takes the largest flux
  // of the lines that hold it). **A swath is made of the points that drain to its line**: a
  // point beside a glacier that drains away from it is not under it. Stage C stamped a disc
  // about each point of a line instead, which put a glacier's ice on points that drain out of
  // its swath (a region's held edge beside the line, a slope past a low divide), and each of
  // those read the swath's margin as a snout a glacier thick and was worn below its receiver
  // by that much: the trench W106 found, counted on the grounding page. So: each point walks
  // down its own line; a point q on it stands for every line point from the receiver to q once
  // the point is within half of q's width of q (and its bed is not above q's ice, this
  // solver's clip); the point takes the largest flux of those, **and never more than its
  // receiver has**, which is the quoted recursion's consequence (a swath is entered through
  // its own points) and is what makes a step down in the ice to a receiver always the
  // glacier's own thinning. In flood order, so a receiver is settled before its donors.
  const i32 widest2 = widest * widest;
  for (u32 k = 0; k < n; ++k) {
    const u32 u = order[k];
    if (held[u] != 0) continue;
    f64 most = 0.0, most_deep = 0.0;  // the largest flux down the line so far, and its ice
    f64 found = 0.0, found_deep = 0.0;
    u32 q = u;
    i32 dx = 0, dy = 0;
    for (i32 step = 0; step < k_ice_reach_cells && widest > 0; ++step) {
      const u8 code = receiver[q];
      if (code == k_no_receiver) break;
      q = neighbour(q, o.at[code]);
      if (held[q] != 0) break;
      dx += k_receiver_dx[code];
      dy += k_receiver_dz[code];
      const i32 far2 = dx * dx + dy * dy;
      // Past the widest glacier's half width nothing down the line holds the point.
      if (far2 > widest2) break;
      if (flux[q] > most) {
        most = flux[q];
        most_deep = own[q];
      }
      i32 radius = static_cast<i32>(own[q] * half_cells);
      if (radius > k_ice_reach_cells) radius = k_ice_reach_cells;
      if (radius < 1 || far2 > radius * radius || z[u] > z[q] + own[q]) continue;
      found = most;
      found_deep = most_deep;
    }
    if (found > 0.0) {
      const u32 below = neighbour(u, o.at[receiver[u]]);
      if (held[below] == 0 && found > reach[below]) {
        found = reach[below];
        found_deep = thick[below];
      }
    }
    if (flux[u] >= found) {
      found = flux[u];
      found_deep = own[u];
    }
    reach[u] = found;
    thick[u] = found_deep;
    iced[u] = found > 0.0 ? u8{1} : u8{0};
  }
  // **The region's edge is not a snout.** A held point is outside the model: no flux is
  // counted at it and no swath is found from it, so what the ice does there is a boundary
  // condition, and the one that adds nothing is that the ice goes on as it came: a held point
  // carries the thickest ice of the points that drain to it. A glacier that leaves the region
  // then reads no step in its surface at the edge, and a thinner thread that reaches the same
  // held point hangs above it as it would above any trunk.
  for (u32 p = 0; p < n; ++p) {
    if (held[p] != 0 || iced[p] == 0) continue;
    const u32 below = neighbour(p, o.at[receiver[p]]);
    if (held[below] != 0 && thick[below] < thick[p]) thick[below] = thick[p];
  }
  // Where each iced point's erosion reads the ice's surface (landform.md, "Basins the ice
  // digs"). Ice flows down its surface, and its surface is smooth over several thicknesses
  // whatever the bed does under it (Kamb and Echelmeyer 1986: a coupling length of one to
  // three thicknesses for a valley glacier, the averaging window four of those). So **along a
  // glacier's own line** the slope is read to the point twice the coupling length down the
  // line, the downstream half of that window, and the ice goes on wearing a bed that rises
  // under it for as long as its surface falls over the reach. A point of the swath beside the
  // line keeps the step to its receiver: the surface across a glacier is level, the cross
  // fall the swath wears away would be divided by the reach's length, and W106 measured the
  // trough's section gone when every point of the swath read the reach. One step at least,
  // so a coupling of nothing reads the slope to the receiver everywhere.
  u32* far = ice.far.data();
  f64* far_cells = ice.far_cells.data();
  const f64 window = 2.0 * c.coupling / frame.spacing_m;
  for (u32 p = 0; p < n; ++p) {
    far[p] = p;
    far_cells[p] = 0.0;
    if (iced[p] == 0) continue;
    const f64 want = flux[p] >= reach[p] ? window * thick[p] : 0.0;
    u32 q = p;
    f64 walked = 0.0;
    for (u32 k = 0; k < k_ice_coupling_points; ++k) {
      const u8 code = receiver[q];
      q = neighbour(q, o.at[code]);
      walked += (code & 1u) != 0 ? k_sqrt2 : 1.0;
      if (!(walked < want) || receiver[q] == k_no_receiver) break;
    }
    far[p] = q;
    far_cells[p] = walked;
  }
}

// What melts out of the ice at a point, laid as till over the glacier's width there: `drop`
// metres over one cell shared by the points within half the width that are under the ice (or
// the point alone, at a snout). It is kept aside and laid once the step's books are done.
f64 lay_till(const Sketch& s, const Frame& frame, const u8* held, u32 p, f64 drop, IceField& ice) {
  const u32 w = frame.w;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(frame.h);
  const f64 cell_km2 = frame.spacing_m * frame.spacing_m * 1.0e-6;
  const f64 through = ice.arriving[p] > 1.0 ? ice.arriving[p] : 1.0;
  const f64 wide = ice_width_m(*s.ice, through, cell_km2);
  // A moraine stands no higher than the ice that built it: a point takes till up to the
  // thickness of the glacier here, and what it has no room for goes on with the meltwater.
  const f64 deep = s.ice->thickness_ratio * wide;
  i32 radius = static_cast<i32>(0.5 * wide / frame.spacing_m);
  if (radius > k_ice_reach_cells) radius = k_ice_reach_cells;
  const i32 px = static_cast<i32>(p % w);
  const i32 py = static_cast<i32>(p / w);
  const u8* iced = ice.iced.data();
  const f64* till = ice.till.data();
  f64* add = ice.add.data();
  u32 count = 0;
  f64 laid = 0.0;
  for (u32 pass = 0; pass < 2; ++pass) {
    for (i32 dy = -radius; dy <= radius; ++dy) {
      const i32 y = py + dy;
      if (y < 0 || y >= hi) continue;
      for (i32 dx = -radius; dx <= radius; ++dx) {
        const i32 x = px + dx;
        if (x < 0 || x >= wi || dx * dx + dy * dy > radius * radius) continue;
        const u32 u = static_cast<u32>(y) * w + static_cast<u32>(x);
        if (held[u] != 0 || (u != p && iced[u] == 0)) continue;
        if (pass == 0) {
          ++count;
          continue;
        }
        const f64 room = deep - till[u] - add[u];
        const f64 share = drop / static_cast<f64>(count);
        const f64 given = share < room ? share : (room > 0.0 ? room : 0.0);
        add[u] += given;
        laid += given;
      }
    }
    if (count == 0) return 0.0;
  }
  return laid;
}

// ---- the river network of one step -------------------------------------------------------------
//
// The rivers' solve is sweeps over the flow network (landform.md, "The rivers' sweeps"). The
// flood's order visits the lattice by height, which is no order in memory at all, and a sweep in
// it recomputed a square root and three divisions a point. So each step lays the network out once
// **in the sequence the sweeps walk it**: the points sorted by their level in the network (a held
// point is level 0, any other one more than its highest receiver) and by cell within a level,
// each with the constants its update needs and the places of its receivers and its donors beside
// it. A sweep is then two walks of that sequence reading its own arrays in order.
//
// A level's points read only other levels (a point's receivers are lower, its donors higher), so
// a level runs on the job system in chunks, each point writing its own entries: the same bytes at
// any thread count. A point's flux is a gather over its donors in the fixed order of the eight
// directions, never a scatter whose order the threads would decide.

// What a point is to its river this step.
constexpr u8 k_node_held = 0;    // an outlet: it takes what reaches it
constexpr u8 k_node_keeps = 1;   // a lake's member: what reaches it settles in the lake
constexpr u8 k_node_passes = 2;  // under the sea, or a slope too small for a channel: no cut
constexpr u8 k_node_reach = 3;   // a river's reach: it cuts and lays down

struct Network {
  // By cell.
  Vector<u16> share;      // eight a point: each neighbour's share of its flow, of k_share_one
  Vector<u64> discharge;  // the cells draining through the point, in 1 / k_share_one of a cell
  Vector<u32> level;
  Vector<u8> anchor;     // the receiver that set the point's level
  Vector<u32> by_level;  // the cells by level, on the way to the sequence
  Vector<u32> position;  // the cell's place in the sequence
  // By place in the sequence.
  Vector<u32> cell;
  Vector<u32> level_begin;  // levels + 1 places
  Vector<u32> cursor;
  Vector<u8> kind;
  Vector<f64> ground;    // the height before the rivers
  Vector<f64> iterate;   // the solve's height
  Vector<f64> arriving;  // the sediment flux that reaches the point, metres over one cell
  Vector<f64> passed;    // what it passes on
  Vector<f64> carried;   // what its slope shed last step
  Vector<f64> deposit;   // the share of what arrives that it lays down
  Vector<f64> loose;     // its sediment
  Vector<f64> soft;      // K dt of sediment times the point's stream power
  Vector<f64> firm;      // K dt of its rock times the same
  Vector<f64> ratio;     // the rock's erodibility over the sediment's
  // Under ice (zero with none): the ice's share of the point's stream power, the place of the
  // point its erosion reads the ice's surface at, the ice's thickness there less here, and the
  // part of an adverse bed the published limit lets it keep wearing.
  Vector<f64> ice_share;
  Vector<u32> far_place;
  Vector<f64> far_step;
  Vector<f64> far_limit;
  // The network's edges, each point's a run: its receivers with the weight each one's height has
  // in the point's base level, and its donors with the share of each one's flow that comes here.
  Vector<u32> down_begin;
  Vector<u32> down_place;
  Vector<f64> down_weight;
  Vector<u32> up_begin;
  Vector<u32> up_place;
  Vector<f64> up_weight;
};

// What the network is built from: the step's ground and the sketch's fields over it.
struct RiverInputs {
  const Sketch* sketch = nullptr;
  u32 w = 0;
  u32 h = 0;
  const u8* held = nullptr;
  const f64* z = nullptr;
  const f64* sed = nullptr;
  const f64* carry = nullptr;
  // What the rivers' solve moved each reach by last step: this step's first guess.
  const f64* drift = nullptr;
  const f64* settle = nullptr;
  const f64* hardness = nullptr;
  const f32* resist = nullptr;
  bool has_sea = false;
  f64 sea = 0.0;
  f64 channel_cells = 0.0;
  f64 loose_dt = 0.0;
  // The steepest fall, in fill units over one spacing, under which a point's flow divides.
  f64 divide_units = 0.0;
  // The step's ice, or null.
  const IceField* ice = nullptr;
  u32 grain = 4096;
};

// Places [begin, end) in chunks on the job system where there are enough of them to be worth a
// dispatch, or here.
template <class F>
void for_span(jobs::JobSystem* jobs, u32 begin, u32 end, u32 grain, F body) {
  const u32 count = end - begin;
  if (jobs == nullptr || grain == 0 || count < 2 * grain) {
    body(begin, end);
    return;
  }
  jobs->parallel_for(jobs::Pool::Performance, count, grain,
                     [begin, &body](u32 a, u32 b) { body(begin + a, begin + b); });
}

// The largest of the values the chunks report: the same number whatever order they report in.
void raise_to(std::atomic<f64>& worst, f64 v) noexcept {
  f64 seen = worst.load(std::memory_order_relaxed);
  while (v > seen && !worst.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
  }
}

// Each point's shares of its flow among its lower neighbours (landform.md, "Flow that divides").
// Where the steepest fall on the filled surface is `divide_units` (fill units over one spacing)
// or more, all of it goes to the receiver. Where the ground is gentler the flow divides among
// every lower neighbour in proportion to its slope times the contour length it crosses (Quinn,
// Beven, Chevallier and Planchon 1991: half a spacing toward a cardinal neighbour, sqrt(2) / 4
// toward a diagonal one), which over the distances 1 and sqrt(2) is the fall times two and the
// fall times one: whole numbers. The shares are whole units of `k_share_one` and the receiver
// takes what the divisions left, so they sum to it exactly. `single`, where given, marks points
// that never divide.
void find_shares(u32 w, u32 h, f64 divide_units, const u8* single, jobs::JobSystem* jobs,
                 const Routing& r, Network& net) {
  net.share.resize(w * h * 8);
  u16* share = net.share.data();
  const u8* receiver = r.receiver.data();
  const i64* filled = r.filled.data();
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(h);
  for_rows(jobs, h, [&](u32 j0, u32 j1) {
    for (u32 z = j0; z < j1; ++z)
      for (u32 x = 0; x < w; ++x) {
        const u32 p = z * w + x;
        u16* s = share + p * 8;
        for (u32 k = 0; k < 8; ++k)
          s[k] = 0;
        const u8 code = receiver[p];
        if (code == k_no_receiver) continue;
        s[code] = static_cast<u16>(k_share_one);
        if (!(divide_units > 0.0) || (single != nullptr && single[p] != 0)) continue;
        u64 raw[8];
        u64 total = 0;
        f64 steepest = 0.0;
        for (u32 k = 0; k < 8; ++k) {
          raw[k] = 0;
          const i32 nx = static_cast<i32>(x) + k_receiver_dx[k];
          const i32 nz = static_cast<i32>(z) + k_receiver_dz[k];
          if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) continue;
          const i64 drop = filled[p] - filled[static_cast<u32>(nz) * w + static_cast<u32>(nx)];
          if (drop <= 0) continue;
          const f64 fall = static_cast<f64>(drop) * ((k & 1u) != 0 ? k_inv_sqrt2 : 1.0);
          if (fall > steepest) steepest = fall;
          raw[k] = static_cast<u64>(drop) * ((k & 1u) != 0 ? 1u : 2u);
          total += raw[k];
        }
        if (!(steepest < divide_units) || total == raw[code]) continue;
        u32 rest = k_share_one;
        for (u32 k = 0; k < 8; ++k) {
          if (k == code || raw[k] == 0) continue;
          const u32 part = static_cast<u32>((raw[k] * k_share_one) / total);
          s[k] = static_cast<u16>(part);
          rest -= part;
        }
        s[code] = static_cast<u16>(rest);
      }
  });
}

// The discharge of every point, in reverse flood order (every donor before its receivers): its
// own cell and what its donors sent. A point's shares are whole units and the receiver takes
// what the division left, so nothing is lost or made: the held points' discharges sum to the
// lattice exactly.
void accumulate_shares(u32 w, u32 h, const Routing& r, Network& net) {
  const u32 n = w * h;
  net.discharge.assign(n, u64{k_share_one});
  u64* discharge = net.discharge.data();
  const u16* share = net.share.data();
  const u32* order = r.order.data();
  const u8* receiver = r.receiver.data();
  const Offsets o = neighbour_offsets(w);
  for (u32 k = n; k-- > 0;) {
    const u32 p = order[k];
    const u8 code = receiver[p];
    if (code == k_no_receiver) continue;
    const u16* s = share + p * 8;
    const u64 q = discharge[p];
    u64 rest = q;
    if (s[code] != k_share_one) {
      for (u32 j = 0; j < 8; ++j) {
        if (j == code || s[j] == 0) continue;
        const u64 part = (q * s[j]) / k_share_one;
        discharge[neighbour(p, o.at[j])] += part;
        rest -= part;
      }
    }
    discharge[neighbour(p, o.at[code])] += rest;
  }
}

// The network of this step's ground, laid out in the sweeps' sequence.
void build_network(const RiverInputs& in, const Routing& r, jobs::JobSystem* jobs, Network& net) {
  const Sketch& s = *in.sketch;
  const u32 w = in.w;
  const u32 n = w * in.h;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(in.h);
  const Offsets o = neighbour_offsets(w);
  const u8* receiver = r.receiver.data();
  const u32* order = r.order.data();
  const u32* sill = r.sill.data();
  const bool icy = in.ice != nullptr && in.ice->on;
  const u8* iced = icy ? in.ice->iced.data() : nullptr;
  const f64* ice_flux = icy ? in.ice->flux.data() : nullptr;
  const f64* ice_reach = icy ? in.ice->reach.data() : nullptr;
  const f64* ice_thick = icy ? in.ice->thick.data() : nullptr;
  const f64 ice_k = icy ? in.ice->erodibility : 0.0;
  find_shares(w, in.h, in.divide_units, iced, jobs, r, net);
  accumulate_shares(w, in.h, r, net);
  const u16* share = net.share.data();
  const u64* discharge = net.discharge.data();
  // Levels, in flood order: a point's receivers are lower on the filled surface, so each was
  // given its level before the point is reached.
  net.level.resize(n);
  net.anchor.resize(n);
  u32* level = net.level.data();
  u8* anchor = net.anchor.data();
  u32 top = 0;
  for (u32 k = 0; k < n; ++k) {
    const u32 p = order[k];
    if (receiver[p] == k_no_receiver) {
      level[p] = 0;
      anchor[p] = k_no_receiver;
      continue;
    }
    const u16* sp = share + p * 8;
    u32 above = 0;
    u8 from = receiver[p];
    for (u32 j = 0; j < 8; ++j) {
      if (sp[j] == 0) continue;
      const u32 l = level[neighbour(p, o.at[j])];
      if (l > above) {
        above = l;
        from = static_cast<u8>(j);
      }
    }
    // With one receiver that is it; with several, the first of the highest.
    if (level[neighbour(p, o.at[receiver[p]])] == above) from = receiver[p];
    level[p] = above + 1;
    anchor[p] = from;
    if (above + 1 > top) top = above + 1;
  }
  // The sequence. First a counting sort by level, the cells of a level in their own order;
  // then each level by the place of the receiver that set it (its anchor, in the level below),
  // so the donors of points that follow each other follow each other too, and a sweep's reads
  // of the level above and the level below both run forward through memory.
  const u32 levels = top + 1;
  net.level_begin.assign(levels + 1, 0u);
  u32* level_begin = net.level_begin.data();
  for (u32 p = 0; p < n; ++p)
    ++level_begin[level[p] + 1];
  u32 widest = 0;
  for (u32 l = 0; l < levels; ++l) {
    if (level_begin[l + 1] > widest) widest = level_begin[l + 1];
    level_begin[l + 1] += level_begin[l];
  }
  net.cursor.resize(levels > widest + 1 ? levels : widest + 1);
  u32* cursor = net.cursor.data();
  for (u32 l = 0; l < levels; ++l)
    cursor[l] = level_begin[l];
  net.by_level.resize(n);
  net.cell.resize(n);
  net.position.resize(n);
  u32* by_level = net.by_level.data();
  u32* cell = net.cell.data();
  u32* position = net.position.data();
  for (u32 p = 0; p < n; ++p)
    by_level[cursor[level[p]]++] = p;
  for (u32 i = 0; i < level_begin[1]; ++i) {
    cell[i] = by_level[i];
    position[by_level[i]] = i;
  }
  for (u32 l = 1; l < levels; ++l) {
    const u32 below = level_begin[l - 1];
    const u32 begin = level_begin[l];
    const u32 end = level_begin[l + 1];
    const u32 slots = begin - below;
    for (u32 c = 0; c <= slots; ++c)
      cursor[c] = 0;
    for (u32 k = begin; k < end; ++k) {
      const u32 q = by_level[k];
      ++cursor[position[neighbour(q, o.at[anchor[q]])] - below + 1];
    }
    for (u32 c = 0; c < slots; ++c)
      cursor[c + 1] += cursor[c];
    for (u32 k = begin; k < end; ++k) {
      const u32 q = by_level[k];
      const u32 i = begin + cursor[position[neighbour(q, o.at[anchor[q]])] - below]++;
      cell[i] = q;
      position[q] = i;
    }
  }
  // Each point's kind and constants.
  net.kind.resize(n);
  net.ground.resize(n);
  net.iterate.resize(n);
  net.arriving.resize(n);
  net.passed.resize(n);
  net.carried.resize(n);
  net.deposit.resize(n);
  net.loose.resize(n);
  net.soft.resize(n);
  net.firm.resize(n);
  net.ratio.resize(n);
  net.down_begin.resize(n + 1);
  net.up_begin.resize(n + 1);
  u8* kind = net.kind.data();
  u32* down_begin = net.down_begin.data();
  u32* up_begin = net.up_begin.data();
  // A reach's stream power in cells, its receivers' terms summed: sqrt(the discharge that goes
  // to the receiver) over the distance to it. One receiver's is the single-flow law's sqrt(N),
  // over sqrt(2) on a diagonal.
  const auto term = [](f64 cells, u16 part, u32 j) {
    const f64 through = part == k_share_one
                            ? cells
                            : cells * (static_cast<f64>(part) / static_cast<f64>(k_share_one));
    return std::sqrt(through) * ((j & 1u) != 0 ? k_inv_sqrt2 : 1.0);
  };
  f64* ground = net.ground.data();
  f64* iterate = net.iterate.data();
  f64* arriving = net.arriving.data();
  f64* passed = net.passed.data();
  f64* carried = net.carried.data();
  f64* deposit = net.deposit.data();
  f64* loose = net.loose.data();
  f64* soft = net.soft.data();
  f64* firm = net.firm.data();
  f64* ratio = net.ratio.data();
  net.ice_share.resize(n);
  net.far_place.resize(n);
  net.far_step.resize(n);
  net.far_limit.resize(n);
  f64* ice_share = net.ice_share.data();
  u32* far_place = net.far_place.data();
  f64* far_step = net.far_step.data();
  f64* far_limit = net.far_limit.data();
  const u32* ice_far = icy ? in.ice->far.data() : nullptr;
  const f64* ice_far_cells = icy ? in.ice->far_cells.data() : nullptr;
  const f64 adverse = icy ? s.ice->adverse_slope : 0.0;
  for_span(jobs, 0, n, 4096, [&](u32 i0, u32 i1) {
    for (u32 i = i0; i < i1; ++i) {
      const u32 p = cell[i];
      ice_share[i] = 0.0;
      far_place[i] = i;
      far_step[i] = 0.0;
      far_limit[i] = 0.0;
      ground[i] = in.z[p];
      iterate[i] = in.z[p];
      arriving[i] = 0.0;
      passed[i] = 0.0;
      carried[i] = in.carry[p];
      deposit[i] = 0.0;
      loose[i] = 0.0;
      soft[i] = 0.0;
      firm[i] = 0.0;
      ratio[i] = 0.0;
      if (in.held[p] != 0) {
        kind[i] = k_node_held;
        continue;
      }
      const u32 tag = sill[p];
      if (tag != k_no_sill && (tag & k_sill_flag) == 0) {
        kind[i] = k_node_keeps;
        continue;
      }
      const f64 cells = static_cast<f64>(discharge[p]) / static_cast<f64>(k_share_one);
      const bool under_ice = iced != nullptr && iced[p] != 0;
      if ((in.has_sea && in.z[p] <= in.sea) || (cells < in.channel_cells && !under_ice)) {
        kind[i] = k_node_passes;
        continue;
      }
      kind[i] = k_node_reach;
      const u16* sp = share + p * 8;
      f64 power = 0.0;
      for (u32 j = 0; j < 8; ++j)
        if (sp[j] != 0) power += term(cells, sp[j], j);
      if (under_ice) {
        // Under a glacier (Hergarten 2021, equations 19, 22 and 25): the ice's own term, its
        // erodibility times the square root of its effective flux (the catchment's cells to
        // the 1/4 times the swath's flux to the 3/4) on the slope of the ice's surface, beside
        // the meltwater's, the rivers' law on the cells that are not ice. One update takes
        // both: the power is their sum, and the base level is the receiver's bed drawn, by the
        // ice's share, toward the level at which the ice's surface would be flat from here to
        // the point a coupling's reach down the line (landform.md, "Basins the ice digs"):
        // that point's bed and ice less the ice here. Beside a line that point is the
        // receiver, so a tributary's bed hangs above its trunk's by what the trunk's ice is
        // thicker; on a line a bed is worn below its receiver's wherever the surface still
        // falls over the reach: a basin. The ice's slope is taken over that reach, so its
        // power is over the reach's length and not the step's.
        const u8 code = receiver[p];
        const f64 along = (code & 1u) != 0 ? k_inv_sqrt2 : 1.0;
        const f64 root = std::sqrt(ice_reach[p]);
        const f64 reach_cells = ice_far_cells[p];
        const f64 glacial =
            ice_k * std::sqrt(std::sqrt(std::sqrt(cells)) * root * std::sqrt(root)) / reach_cells;
        const f64 water = cells > ice_flux[p] ? std::sqrt(cells - ice_flux[p]) * along : 0.0;
        power = glacial + water;
        if (power > 0.0) {
          const u32 q = ice_far[p];
          ice_share[i] = glacial / power;
          far_place[i] = position[q];
          far_step[i] = ice_thick[q] - ice_thick[p];
          // The published limit (an adverse bed slope of `adverse_slope` times the surface's
          // slope shuts the erosion down) as the part of an adverse bed the ice may still
          // wear: with D the reach and d the step to the receiver, both in spacings,
          // (b_r - b) / d <= c (b + h - s_q) / D gives b >= flat + (b_r - flat) D / (D + c d).
          far_limit[i] = reach_cells / (reach_cells + adverse * ((code & 1u) != 0 ? k_sqrt2 : 1.0));
        }
      }
      const f64 hard = in.hardness[p] * erodibility_of(s, static_cast<f64>(in.resist[p]));
      // The sweeps start from where last step's ended, relative to this step's ground: a river
      // near its grade moves by nearly the same amount each step, and the sweeps that would
      // find that again from nothing are saved (the answer is the same fixed point, to the
      // tolerance the sweeps end at).
      iterate[i] = in.z[p] + in.drift[p];
      // Ice lays nothing down under itself here: what it carries melts out in the step's books.
      deposit[i] = under_ice ? 0.0 : (in.settle[p] < cells ? in.settle[p] / cells : 1.0);
      loose[i] = in.sed[p];
      soft[i] = in.loose_dt * power;
      firm[i] = hard * power;
      ratio[i] = hard / in.loose_dt;
    }
  });
  // The edges: count, lay the runs end to end, fill. A donor is a neighbour that passes
  // something and has a share for this point; its direction to here is the opposite code.
  const auto donor_of = [&](u32 p, u32 j, u32& from, f64& weight) {
    const i32 nx = static_cast<i32>(p % w) + k_receiver_dx[j];
    const i32 nz = static_cast<i32>(p / w) + k_receiver_dz[j];
    if (nx < 0 || nz < 0 || nx >= wi || nz >= hi) return false;
    const u32 q = static_cast<u32>(nz) * w + static_cast<u32>(nx);
    const u16 part = share[q * 8 + ((j + 4) & 7u)];
    if (part == 0) return false;
    from = position[q];
    if (kind[from] != k_node_passes && kind[from] != k_node_reach) return false;
    weight = part == k_share_one ? 1.0 : static_cast<f64>(part) / static_cast<f64>(k_share_one);
    return true;
  };
  for_span(jobs, 0, n, 4096, [&](u32 i0, u32 i1) {
    for (u32 i = i0; i < i1; ++i) {
      const u32 p = cell[i];
      u32 down = 0, up = 0;
      if (kind[i] == k_node_reach)
        for (u32 j = 0; j < 8; ++j)
          if (share[p * 8 + j] != 0) ++down;
      if (kind[i] == k_node_passes || kind[i] == k_node_reach) {
        u32 from = 0;
        f64 weight = 0.0;
        for (u32 j = 0; j < 8; ++j)
          if (donor_of(p, j, from, weight)) ++up;
      }
      down_begin[i + 1] = down;
      up_begin[i + 1] = up;
    }
  });
  down_begin[0] = 0;
  up_begin[0] = 0;
  for (u32 i = 0; i < n; ++i) {
    down_begin[i + 1] += down_begin[i];
    up_begin[i + 1] += up_begin[i];
  }
  net.down_place.resize(down_begin[n]);
  net.down_weight.resize(down_begin[n]);
  net.up_place.resize(up_begin[n]);
  net.up_weight.resize(up_begin[n]);
  u32* down_place = net.down_place.data();
  f64* down_weight = net.down_weight.data();
  u32* up_place = net.up_place.data();
  f64* up_weight = net.up_weight.data();
  for_span(jobs, 0, n, 4096, [&](u32 i0, u32 i1) {
    for (u32 i = i0; i < i1; ++i) {
      const u32 p = cell[i];
      if (kind[i] == k_node_reach) {
        const f64 cells = static_cast<f64>(discharge[p]) / static_cast<f64>(k_share_one);
        const u16* sp = share + p * 8;
        f64 power = 0.0;
        for (u32 j = 0; j < 8; ++j)
          if (sp[j] != 0) power += term(cells, sp[j], j);
        u32 e = down_begin[i];
        for (u32 j = 0; j < 8; ++j) {
          if (sp[j] == 0) continue;
          down_place[e] = position[neighbour(p, o.at[j])];
          // One receiver: its height is the base level, exactly.
          down_weight[e] = sp[j] == k_share_one ? 1.0 : term(cells, sp[j], j) / power;
          ++e;
        }
      }
      if (kind[i] == k_node_passes || kind[i] == k_node_reach) {
        u32 e = up_begin[i];
        u32 from = 0;
        f64 weight = 0.0;
        for (u32 j = 0; j < 8; ++j) {
          if (!donor_of(p, j, from, weight)) continue;
          up_place[e] = from;
          up_weight[e] = weight;
          ++e;
        }
      }
    }
  });
}

// Keeps the banks' sides apart from the other numbers hashed from the seed and the cell.
constexpr u64 k_bank_salt = 0xA24BAED4963EE407ull;
// A reach draining fewer cells than this takes no bank: its lateral rate, which goes with the
// area, would not move a bank in a run, and the pass is not worth its walk of them.
constexpr f64 k_bank_min_cells = 64.0;

struct BankInputs {
  const Sketch* sketch = nullptr;
  u32 w = 0;
  u32 h = 0;
  f64 spacing_m = 0.0;
  const u8* held = nullptr;
  const f64* hardness = nullptr;  // K dt times the point's variation
  const f32* resist = nullptr;
  const u8* iced = nullptr;  // points under a glacier this step, or null
  u32 step = 0;
};

// Lateral erosion (landform.md, "Lateral erosion"): Langston and Tucker 2018's model as their
// own implementation states it (Landlab's LateralEroder). A reach wears the bank beside it, on
// the outside of its bend, at K_l A S / R: A its drainage area, S its slope, 1 / R the bend's
// curvature from the directions the water arrives by and leaves by (0.23, 0.67 or 1.37 over the
// spacing for straight, a 45 or 135 degree bend, a right angle), K_l the bank's erodibility
// times the sketch's ratio. The volume worn, that rate over the water's depth and one spacing,
// is kept at the bank's point from step to step; when it reaches what stands there above the
// reach's receiver (all of the rock, and of the sediment what the water reaches: rock has to be
// worn through, loose ground slumps when it is undercut) the point is lowered to the receiver's
// height, and what it lost is the next step's rivers' to carry from there. The floor is then a
// point wider, the next flood may send the river over it, and the slope limiter brings the wall
// above down behind it. Each point is visited in cell order, one thread: the same bytes at any
// worker count. Returns the banks taken.
u32 erode_banks(const BankInputs& in, const Routing& r, const Network& net, f64* z, f64* sed,
                f64* carry, f64* bank, f64& eroded) {
  const Sketch& s = *in.sketch;
  const u32 w = in.w;
  const u32 n = w * in.h;
  const i32 wi = static_cast<i32>(w);
  const i32 hi = static_cast<i32>(in.h);
  const Offsets o = neighbour_offsets(w);
  const u8* receiver = r.receiver.data();
  const u16* share = net.share.data();
  const u64* discharge = net.discharge.data();
  const u32* position = net.position.data();
  const u8* kind = net.kind.data();
  const f64 cell_m2 = in.spacing_m * in.spacing_m;
  const u64 seed = mix(s.seed ^ k_bank_salt) + in.step;
  u32 taken = 0;
  for (u32 p = 0; p < n; ++p) {
    if (kind[position[p]] != k_node_reach) continue;
    if (in.iced != nullptr && in.iced[p] != 0) continue;  // a glacier widens its own bed
    const f64 cells = static_cast<f64>(discharge[p]) / static_cast<f64>(k_share_one);
    if (cells < k_bank_min_cells) continue;
    const u8 code = receiver[p];
    const i32 x = static_cast<i32>(p % w);
    const i32 y = static_cast<i32>(p / w);
    // The donor the most water comes from; the lowest direction on a tie.
    u32 from = 8;
    u64 most = 0;
    for (u32 j = 0; j < 8; ++j) {
      const i32 nx = x + k_receiver_dx[j];
      const i32 ny = y + k_receiver_dz[j];
      if (nx < 0 || ny < 0 || nx >= wi || ny >= hi) continue;
      const u32 q = static_cast<u32>(ny) * w + static_cast<u32>(nx);
      const u64 part = discharge[q] * share[q * 8 + ((j + 4) & 7u)];
      if (part > most) {
        most = part;
        from = j;
      }
    }
    if (from == 8) continue;  // a head: no bend to speak of
    // The turn, in eighths, from the direction the water arrives by to the one it leaves by:
    // the codes run clockwise, so one to three is a turn to the right.
    const u32 turn = (code - ((from + 4) & 7u)) & 7u;
    if (turn == 4) continue;
    const f64 curvature = turn == 0 ? 0.23 : ((turn == 2 || turn == 6) ? 1.37 : 0.67);
    bool left = turn >= 1 && turn <= 3;  // the outside of a right turn is the left bank
    if (turn == 0) left = (mix(seed ^ mix(p)) & 1u) != 0;
    // The bank beside the reach, square to the way it leaves; from a diagonal, the cardinal
    // neighbour on that side, which touches the receiver too.
    const u32 swing = (code & 1u) != 0 ? 1u : 2u;
    const u32 side = left ? (code + 8 - swing) & 7u : (code + swing) & 7u;
    const i32 bx = x + k_receiver_dx[side];
    const i32 by = y + k_receiver_dz[side];
    if (bx < 0 || by < 0 || bx >= wi || by >= hi) continue;
    const u32 b = static_cast<u32>(by) * w + static_cast<u32>(bx);
    if (in.held[b] != 0 || !(z[b] > z[p])) continue;
    const u32 below = neighbour(p, o.at[code]);
    const f64 fall = z[p] - z[below];
    if (!(fall > 0.0)) continue;
    const f64 slope = fall / (in.spacing_m * ((code & 1u) != 0 ? k_sqrt2 : 1.0));
    const f64 soft =
        sed[b] > 0.0 ? s.sediment_erodibility : erodibility_of(s, static_cast<f64>(in.resist[b]));
    // K_l dt A S / R, metres of bank this step; A / R is cells times the spacing.
    const f64 retreat =
        in.hardness[b] * soft * s.lateral_erosion * cells * in.spacing_m * slope * curvature;
    // The water's depth, metres: 0.4 Q^0.35, Q the area times 4e-6 m/s (the implementation's
    // own constants). The one power that is not a square root: `det::exp` and `det::log`.
    const f64 depth = 0.4 * det::exp(0.35 * det::log(cells * cell_m2 * 4.0e-6));
    bank[b] += retreat * depth / in.spacing_m;
    const f64 stands = z[b] - z[below];
    const f64 loose = sed[b] < stands ? sed[b] : stands;
    const f64 reach = z[p] + depth - z[below];
    const f64 needed = (stands - loose) + (loose < reach ? loose : reach);
    if (bank[b] < needed) continue;
    z[b] = z[below];
    sed[b] -= loose;
    carry[b] += stands;
    eroded += stands;
    bank[b] = 0.0;
    ++taken;
  }
  return taken;
}

// The rivers: erosion and deposition together, implicit (Yuan, Braun, Guerit, Rouby and
// Cordonnier 2019, their equation 20). A reach moves to (z + d + F b) / (1 + F): F = K dt P, P
// its stream power in cells (the spacing cancels at m = 1/2, n = 1), b its base level (its
// receiver's height, no lower than the sea), d the deposit, the reach's share of the flux that
// reaches it, which is what the points above lost. The flux depends on the answer, so it is
// found by sweeps: the flux of the last iterate, each level after the levels above it, then the
// heights, each level after the levels below, until no height moves by `k_river_tolerance_m`.
// Standing water neither cuts nor lays down here: the step's books give it what reaches it.
// Returns the sweeps it took; `net.iterate` is the answer, by place.
u32 sweep_rivers(const RiverInputs& in, jobs::JobSystem* jobs, Network& net) {
  const u32 levels = net.level_begin.size() - 1;
  const u32* level_begin = net.level_begin.data();
  const u8* kind = net.kind.data();
  const f64* ground = net.ground.data();
  f64* iterate = net.iterate.data();
  f64* arriving = net.arriving.data();
  f64* passed = net.passed.data();
  const f64* carried = net.carried.data();
  const f64* deposit = net.deposit.data();
  const f64* loose_of = net.loose.data();
  const f64* soft = net.soft.data();
  const f64* firm = net.firm.data();
  const f64* ratio = net.ratio.data();
  const f64* ice_share = net.ice_share.data();
  const u32* far_place = net.far_place.data();
  const f64* far_step = net.far_step.data();
  const f64* far_limit = net.far_limit.data();
  const u32* down_begin = net.down_begin.data();
  const u32* down_place = net.down_place.data();
  const f64* down_weight = net.down_weight.data();
  const u32* up_begin = net.up_begin.data();
  const u32* up_place = net.up_place.data();
  const f64* up_weight = net.up_weight.data();
  const bool has_sea = in.has_sea;
  const f64 sea = in.sea;
  u32 sweeps = 0;
  for (u32 sweep = 0; sweep < k_river_sweeps; ++sweep) {
    ++sweeps;
    for (u32 l = levels; l-- > 1;) {
      for_span(jobs, level_begin[l], level_begin[l + 1], in.grain, [&](u32 i0, u32 i1) {
        for (u32 i = i0; i < i1; ++i) {
          if (kind[i] < k_node_passes) continue;
          f64 flux = 0.0;
          for (u32 e = up_begin[i]; e < up_begin[i + 1]; ++e)
            flux += up_weight[e] * passed[up_place[e]];
          arriving[i] = flux;
          const f64 out = flux + carried[i] + (ground[i] - iterate[i]);
          passed[i] = out > 0.0 ? out : 0.0;
        }
      });
    }
    std::atomic<f64> worst{0.0};
    for (u32 l = 1; l < levels; ++l) {
      for_span(jobs, level_begin[l], level_begin[l + 1], in.grain, [&](u32 i0, u32 i1) {
        f64 most = 0.0;
        for (u32 i = i0; i < i1; ++i) {
          if (kind[i] != k_node_reach) continue;
          const f64 down = arriving[i] * deposit[i];
          const f64 top = ground[i] + down;
          f64 base = 0.0;
          for (u32 e = down_begin[i]; e < down_begin[i + 1]; ++e) {
            f64 below = iterate[down_place[e]];
            if (has_sea && below < sea) below = sea;
            base += down_weight[e] * below;
          }
          // Sediment first, at its own erodibility: the point's own and what this step lays.
          // Where the cut goes through it into rock the height is the one at which the time
          // to strip the sediment and the time to cut the rock make the step, each at the rate
          // the final slope gives, which is the single formula again when the two erode alike.
          const f64 loose = loose_of[i] + down;
          const f64 thin = ratio[i];
          const auto cut = [top, loose, thin](f64 soft_k, f64 firm_k, f64 level) {
            if (!(top > level)) return top;
            const f64 through = (top + soft_k * level) / (1.0 + soft_k);
            if (top - through <= loose) return through;
            return (top - loose * (1.0 - thin) + firm_k * level) / (1.0 + firm_k);
          };
          f64 to;
          if (ice_share[i] > 0.0) {
            // Under ice: the ice's term draws the bed toward the level that would lay its
            // surface flat over the coupling's reach, and where that is below the receiver's
            // bed (an adverse bed) no lower than the published limit lets the ice wear it; the
            // meltwater's term draws it toward the receiver's bed. Both at once is one update
            // toward their mean by their powers.
            const f64 flat = iterate[far_place[i]] + far_step[i];
            const f64 target = base > flat ? flat + (base - flat) * far_limit[i] : flat;
            to = cut(soft[i], firm[i], base + ice_share[i] * (target - base));
            // **Water wears a bed down and never fills it.** Where that update ends below the
            // receiver's bed the meltwater's term has pulled the bed up toward the receiver,
            // which is deposition by an erosion law; there the ice works alone, with its own
            // part of the power, toward its own level. (Without this a basin is as deep as
            // the ice's share of the power lets it be, and where meltwater is plentiful, the
            // lower reach of every glacier, that is next to nothing.)
            if (to < base && target < base)
              to = cut(soft[i] * ice_share[i], firm[i] * ice_share[i], target);
          } else {
            to = cut(soft[i], firm[i], base);
          }
          const f64 moved = to > iterate[i] ? to - iterate[i] : iterate[i] - to;
          if (moved > most) most = moved;
          iterate[i] = to;
        }
        raise_to(worst, most);
      });
    }
    if (worst.load(std::memory_order_relaxed) < k_river_tolerance_m) break;
  }
  return sweeps;
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
  if (!in_range(s.uplift.variation, 0.0, 0.9))
    return refuse("a sketch's uplift.variation is 0 to 0.9");
  if (!in_range(s.uplift.variation_length_m, 10.0, 1000000.0))
    return refuse("a sketch's uplift.variation_length_m is 10 to 1,000,000 m");
  if (!in_range(s.divide_below_slope, 0.0, 1.0))
    return refuse("a sketch's divide_below_slope is 0 to 1");
  if (!in_range(s.lateral_erosion, 0.0, 10.0))
    return refuse("a sketch's lateral_erosion is 0 to 10");
  if (!in_range(s.channel_width_m, 0.1, 1000.0))
    return refuse("a sketch's channel_width_m is 0.1 to 1,000 m");
  if (s.ice) {
    if (!in_range(s.ice->ela_m, -11000.0, 9000.0))
      return refuse("a sketch's ice.ela_m is -11,000 to 9,000 m");
    if (!in_range(s.ice->full_m, 10.0, 5000.0))
      return refuse("a sketch's ice.full_m is 10 to 5,000 m");
    if (!in_range(s.ice->melt_m, 10.0, 5000.0))
      return refuse("a sketch's ice.melt_m is 10 to 5,000 m");
    if (!in_range(s.ice->erodibility, 0.0, 100.0))
      return refuse("a sketch's ice.erodibility is 0 to 100");
    if (!in_range(s.ice->width_m, 10.0, 10000.0))
      return refuse("a sketch's ice.width_m is 10 to 10,000 m");
    if (!in_range(s.ice->thickness_ratio, 0.01, 1.0))
      return refuse("a sketch's ice.thickness_ratio is 0.01 to 1");
    if (!in_range(s.ice->coupling, 0.0, 10.0)) return refuse("a sketch's ice.coupling is 0 to 10");
    if (!in_range(s.ice->adverse_slope, 0.5, 11.0))
      return refuse("a sketch's ice.adverse_slope is 0.5 to 11");
    if (s.ice->from_yr < 0 || (s.ice->until_yr && *s.ice->until_yr < s.ice->from_yr))
      return refuse("a sketch's ice stands from from_yr, 0 or later, until until_yr, no earlier");
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
      [mm](u32 i) { return static_cast<i64>(mm[i]) * k_fill_units_per_mm; }, jobs, r);
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

bool route_divided(u32 width, u32 height, i64 spacing_mm, const Vector<i32>& height_mm,
                   const Vector<u8>& held, u64 seed, f64 divide_below_slope, jobs::JobSystem* jobs,
                   Vector<u8>& receiver, Vector<u16>& share, Vector<u64>& discharge) {
  const u32 n = width * height;
  if (n == 0 || height_mm.size() != n || held.size() != n || spacing_mm <= 0) return false;
  Routing r;
  r.receiver.swap(receiver);
  Network net;
  net.share.swap(share);
  net.discharge.swap(discharge);
  const i32* mm = height_mm.data();
  const bool ok = flood(
      width, height, held.data(),
      [mm](u32 i) { return static_cast<i64>(mm[i]) * k_fill_units_per_mm; }, jobs, r);
  if (ok) {
    find_receivers(width, height, held.data(), seed, jobs, r);
    find_shares(
        width, height,
        divide_below_slope * static_cast<f64>(spacing_mm) * static_cast<f64>(k_fill_units_per_mm),
        nullptr, jobs, r, net);
    accumulate_shares(width, height, r, net);
  }
  r.receiver.swap(receiver);
  net.share.swap(share);
  net.discharge.swap(discharge);
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
        f64 envelope = 0.0;
        f64 rate = s.uplift.regional_mm_per_yr + ridge_rate(s, frame.x(i), frame.z(j), envelope);
        envelope += s.uplift.regional_mm_per_yr;
        if (s.uplift.raster) {
          const f64 from_raster = raster_rate(*s.uplift.raster, frame, i, j);
          rate += from_raster;
          envelope += from_raster;
        }
        // Uneven over the plan, so a range is not a pair of ramps: the seeded noise times a
        // fraction of the rate about the point (a ridge's rate falling off as its kernel's
        // square root, so its flanks are as uneven as its crest), and never below nothing.
        if (s.uplift.variation > 0.0) {
          rate += s.uplift.variation * envelope *
                  plan_noise(mix(s.seed ^ k_plan_salt), frame.x(i), frame.z(j),
                             s.uplift.variation_length_m);
          if (rate < 0.0) rate = 0.0;
        }
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
  Network network;
  RiverInputs rivers;
  rivers.sketch = &s;
  rivers.w = w;
  rivers.h = h;
  rivers.held = held;
  rivers.z = z;
  rivers.sed = sed;
  rivers.carry = carry;
  Vector<f64> drift_field(n, 0.0);
  f64* drift = drift_field.data();
  rivers.drift = drift;
  rivers.settle = settle;
  rivers.hardness = hardness;
  rivers.resist = resist;
  rivers.has_sea = has_sea;
  rivers.sea = sea;
  rivers.channel_cells = channel_cells;
  rivers.loose_dt = loose_dt;
  rivers.grain = options.level_grain;
  rivers.divide_units = s.divide_below_slope * frame.spacing_m * k_units_per_m;
  // What lateral erosion has worn from each bank and not yet taken, metres over one cell.
  Vector<f64> bank_field(n, 0.0);
  BankInputs banks;
  banks.sketch = &s;
  banks.w = w;
  banks.h = h;
  banks.spacing_m = frame.spacing_m;
  banks.held = held;
  banks.hardness = hardness;
  banks.resist = resist;
  u32 banks_taken = 0;
  // The ice age, where the sketch has one (landform.md, "Ice").
  IceField ice;
  const bool has_ice = s.ice.has_value();
  u32 ice_steps = 0;
  if (has_ice) {
    ice.erodibility = s.ice->erodibility;
    ice.flux.assign(n, 0.0);
    ice.arriving.assign(n, 0.0);
    ice.reach.assign(n, 0.0);
    ice.own.assign(n, 0.0);
    ice.thick.assign(n, 0.0);
    ice.before.assign(n, 0.0);
    ice.melt.assign(n, 0.0);
    ice.add.assign(n, 0.0);
    ice.till.assign(n, 0.0);
    ice.iced.assign(n, u8{0});
    ice.was.assign(n, u8{0});
    ice.far.assign(n, 0u);
    ice.far_cells.assign(n, 0.0);
    rivers.ice = &ice;
  }
  // Where the seconds go: a clock read between the parts, which no decision reads.
  Timing timing;
  auto tick = std::chrono::steady_clock::now();
  const auto lap = [&tick](f64& into) {
    const auto now = std::chrono::steady_clock::now();
    into += std::chrono::duration<f64>(now - tick).count();
    tick = now;
  };
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
    lap(timing.uplift_s);
    if (!flood(w, h, held, [z](u32 i) { return quantize(z[i]); }, jobs, routing)) {
      error = "the flood did not reach every point";
      return false;
    }
    find_receivers(w, h, held, s.seed ^ k_route_salt, jobs, routing);
    // The ice, where the sketch has an ice age and this step is in it: before the lakes, since
    // a hollow under a glacier is not one.
    if (has_ice) {
      const i64 at = static_cast<i64>(step) * s.step_yr;
      ice.on = at >= s.ice->from_yr && (!s.ice->until_yr || at < *s.ice->until_yr);
      if (ice.on) {
        // A glacier pays for the melt over its swath as it stood last step. The step the ice
        // comes there is none, and a glacier found without one melts along its line only and
        // runs to the region's edge whatever the line's height: so that step finds the ice
        // twice, the second time over the first's swath.
        if (ice_steps == 0) glaciate(s, frame, held, z, routing, ice);
        glaciate(s, frame, held, z, routing, ice);
        ++ice_steps;
      }
    }
    banks.iced = ice.on ? ice.iced.data() : nullptr;
    find_lakes(n, [z](u32 i) { return quantize(z[i]); }, banks.iced, routing);
    const u32* order = routing.order.data();
    const u8* receiver = routing.receiver.data();
    const u32* sill = routing.sill.data();
    // The rivers' solve over this step's network (`sweep_rivers`). z stays the ground before the
    // rivers; `hk` takes the answer, by cell.
    rivers.z = z;
    lap(timing.flood_s);
    build_network(rivers, routing, jobs, network);
    lap(timing.network_s);
    sweeps += sweep_rivers(rivers, jobs, network);
    {
      const u32* cell = network.cell.data();
      const f64* iterate = network.iterate.data();
      const f64* unmoved = network.ground.data();
      for_span(jobs, 0, n, 4096, [&](u32 i0, u32 i1) {
        for (u32 i = i0; i < i1; ++i) {
          hk[cell[i]] = iterate[i];
          drift[cell[i]] = iterate[i] - unmoved[i];
        }
      });
    }
    lap(timing.sweeps_s);
    // The solve's result, each point after its donors, with the books kept on what each point
    // truly lost or gained: nothing is laid down that did not arrive. Standing water takes its
    // share here, and a sill what its lake could not hold.
    f64* flux = scratch_field.data();
    for (u32 p = 0; p < n; ++p)
      flux[p] = 0.0;
    Lake* lakes = routing.lakes.data();
    const bool settles = s.deposition > 0.0;
    const u16* share = network.share.data();
    // What a point passes on goes to its receivers by their shares of its flow.
    const auto send = [&](u32 p, f64 amount) {
      const u8 code = receiver[p];
      if (code == k_no_receiver) return;
      const u16* sp = share + p * 8;
      if (sp[code] == k_share_one) {
        flux[neighbour(p, o.at[code])] += amount;
        return;
      }
      for (u32 j = 0; j < 8; ++j)
        if (sp[j] != 0)
          flux[neighbour(p, o.at[j])] +=
              amount * (static_cast<f64>(sp[j]) / static_cast<f64>(k_share_one));
    };
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
      if (settles && ice.on && ice.melt[p] > 0.0 && q > 0.0) {
        // What the ice carried melts out with the ice: that share is till here.
        const f64 stays = lay_till(s, frame, held, p, q * ice.melt[p], ice);
        laid += stays;
        q -= stays;
      }
      if (!settles) {
        // G = 0: nothing settles, in standing water either. What is cut leaves.
        const f64 lost = z[p] - hk[p];
        eroded += lost;
        sed[p] = sed[p] > lost ? sed[p] - lost : 0.0;
        z[p] = hk[p];
        send(p, q + lost);
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
        if (q > 0.0) send(p, spread(z, sed, held, p, q, sea, w, h, laid));
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
      send(p, q + lost);
    }
    if (has_ice) {
      // The till the ice dropped this step, now that no point's books read the ground; and till
      // is never more than the sediment that is still there.
      f64* add = ice.add.data();
      f64* till = ice.till.data();
      for (u32 p = 0; p < n; ++p) {
        if (add[p] > 0.0) {
          z[p] += add[p];
          sed[p] += add[p];
          till[p] += add[p];
          add[p] = 0.0;
        }
        if (till[p] > sed[p]) till[p] = sed[p];
      }
    }
    lap(timing.books_s);
    if (s.lateral_erosion > 0.0) {
      banks.step = step;
      banks_taken += erode_banks(banks, routing, network, z, sed, carry, bank_field.data(), eroded);
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
    lap(timing.slopes_s);
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

  lap(timing.settle_s);

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
  out.network_levels = network.level_begin.size() > 0 ? network.level_begin.size() - 1 : 0;
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
          w, h, held, [mm](u32 i) { return static_cast<i64>(mm[i]) * k_fill_units_per_mm; }, jobs,
          routing)) {
    error = "the flood did not reach every point";
    return false;
  }
  find_receivers(w, h, held, s.seed ^ k_route_salt, jobs, routing);
  accumulate(w, h, routing);
  // The divided discharge of the same ground, and a channel's width from it.
  find_shares(w, h, rivers.divide_units, nullptr, jobs, routing, network);
  accumulate_shares(w, h, routing, network);
  out.discharge.resize(n);
  out.channel_width.resize(n);
  out.banks_taken = banks_taken;
  out.ice_steps = ice_steps;
  out.ice.assign(n, 0u);
  out.till.assign(n, 0u);
  if (ice_steps > 0) {
    for (u32 p = 0; p < n; ++p) {
      out.ice[p] = static_cast<u32>(clamp_i32(ice.thick[p] * 1000.0));
      const f64 kept = ice.till[p] < sed[p] ? ice.till[p] : sed[p];
      out.till[p] = static_cast<u32>(clamp_i32(kept * 1000.0));
    }
  }
  {
    const u64* units = network.discharge.data();
    u32* discharge = out.discharge.data();
    u32* wide = out.channel_width.data();
    const f64 cell_km2 = frame.spacing_m * frame.spacing_m * 1.0e-6;
    const f64 width_mm = s.channel_width_m * 1000.0;
    for_rows(jobs, h, [&](u32 j0, u32 j1) {
      for (u32 p = j0 * w; p < j1 * w; ++p) {
        discharge[p] = static_cast<u32>((units[p] + k_share_one / 2) / k_share_one);
        const f64 km2 = static_cast<f64>(units[p]) / static_cast<f64>(k_share_one) * cell_km2;
        wide[p] = static_cast<u32>(clamp_i32(width_mm * std::sqrt(km2)));
      }
    });
  }
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
  lap(timing.fields_s);
  out.timing = timing;
  return true;
}

}  // namespace engine::landform
