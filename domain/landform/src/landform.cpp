// The landform solver (docs/subsystems/landform.md, "The step"; docs/experiments/landform.md has
// every source and constant). One step: uplift, the flood and its receivers, the drainage area,
// the implicit stream-power incision along the flood's order, hillslope diffusion, the slope
// limiter. Every decision is IEEE `+ - * /`, `sqrt`, `floor` and integers; the loops the job
// system runs are the ones whose points read nothing another point of the same loop writes.
#include <core/jobs/job_system.h>
#include <domain/landform/landform.h>

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
// The explicit diffusion's largest step, as D dt / spacing^2 (a quarter is the stability limit).
constexpr f64 k_diffusion_number = 0.2;
// A heightfield's float holds a whole millimetre exactly below 2^24 of them.
constexpr f64 k_max_height_m = 16000.0;

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

struct Routing {
  Vector<i64> filled;
  Vector<u32> order;
  Vector<u8> receiver;
  Vector<u32> area;
  Vector<u64> heap;
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
  r.heap.clear();
  i64* filled = r.filled.data();
  u32* order = r.order.data();
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
      heap_push(r.heap, key_of(f, ni));
    }
  }
  return count == n;
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

// ---- the rock ----------------------------------------------------------------------------------

struct RockRow {
  f64 top = 0.0;
  f64 erodibility = 1.0;  // the factor on K
  f64 critical = 0.9;     // the slope no hillslope of this rock stands above
};

struct RockTable {
  Vector<RockRow> rows;  // ascending top
  RockRow above;
  const RockRow& at(f64 z) const noexcept {
    for (u32 i = 0; i < rows.size(); ++i)
      if (z <= rows[i].top) return rows[i];
    return above;
  }
};

RockRow rock_row(const Sketch& s, f64 top, f64 resistance) noexcept {
  RockRow row;
  row.top = top;
  row.erodibility = 1.0 / (1.0 + resistance * (s.rock.contrast - 1.0));
  row.critical = s.critical_slope + resistance * (s.critical_slope_resistant - s.critical_slope);
  return row;
}

RockTable rock_table(const Sketch& s) {
  RockTable t;
  for (u32 i = 0; i < s.rock.bands.size(); ++i)
    t.rows.push_back(rock_row(s, s.rock.bands[i].top_m, s.rock.bands[i].resistance));
  t.above = rock_row(s, 0.0, s.rock.resistance);
  return t;
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

// The largest ridge kernel at a point, mm/yr: (1 - (r / reach)^2)^2 of the line's rate.
f64 ridge_rate(const Sketch& s, f64 px, f64 pz) noexcept {
  f64 best = 0.0;
  for (u32 r = 0; r < s.uplift.ridges.size(); ++r) {
    const Ridge& ridge = s.uplift.ridges[r];
    const f64 reach2 = ridge.reach_m * ridge.reach_m;
    f64 d2 = reach2;
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
      if (seg2 < d2) d2 = seg2;
    }
    if (d2 >= reach2) continue;
    const f64 k1 = 1.0 - d2 / reach2;
    const f64 rate = ridge.rate_mm_per_yr * k1 * k1;
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

// One point's slope limit (landform.md, "The slope limiter"): the gradient's length is at most
// the critical slope. From the lower neighbour along x and the lower along z it is the upwind
// step of the eikonal equation |grad z| = critical, (a + b + sqrt(2 f^2 - (a - b)^2)) / 2 with
// f the critical slope times the spacing, which keeps a failed slope a cone and not the
// eight-sided pyramid a limit along the lattice's eight directions alone makes; the four
// diagonal neighbours then hold the slope along a diagonal to the same bound, so no pair of
// neighbours stands steeper than the critical slope.
bool limit_point(f64* z, u32 i, i32 x, i32 y, i32 w, i32 h, const RockTable& rock, f64 card,
                 f64 diag) noexcept {
  const f64 zi = z[i];
  const f64 critical = rock.at(zi).critical;
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
  for (u32 i = 0; i < s.rock.bands.size(); ++i) {
    const Band& band = s.rock.bands[i];
    if (!in_range(band.resistance, 0.0, 1.0) || !in_range(band.top_m, -11000.0, 16000.0))
      return refuse(
          "a sketch's rock band is a top of -11,000 to 16,000 m and a resistance of 0 "
          "to 1");
    if (i > 0 && !(band.top_m > s.rock.bands[i - 1].top_m))
      return refuse("a sketch's rock bands ascend by top_m");
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
  const RockTable rock = rock_table(s);

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
  f64* lift = lift_field.data();
  f64* hardness = hardness_field.data();
  const f64 reach = s.border_reach_m ? *s.border_reach_m : 16.0 * frame.spacing_m;
  const f64 k_dt = s.erodibility_per_yr * dt;
  for_rows(jobs, h, [&](u32 j0, u32 j1) {
    for (u32 j = j0; j < j1; ++j)
      for (u32 i = 0; i < w; ++i) {
        const u32 p = j * w + i;
        f64 rate = s.uplift.regional_mm_per_yr + ridge_rate(s, frame.x(i), frame.z(j));
        if (s.uplift.raster) rate += raster_rate(*s.uplift.raster, frame, i, j);
        const f64 d = drain_distance(s, frame, i, j);
        if (d >= 0.0 && reach > 0.0 && d < reach) {
          const f64 t = d / reach;
          rate *= t * t * (3.0 - 2.0 * t);
        }
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
  for (u32 step = 0; step < steps; ++step) {
    for (u32 p = 0; p < n; ++p)
      z[p] += lift[p];
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
    find_receivers(w, h, held, s.seed ^ k_route_salt, jobs, routing);
    accumulate(w, h, routing);
    const u32* order = routing.order.data();
    const u8* receiver = routing.receiver.data();
    const u32* area = routing.area.data();
    // Incision, each point after its receiver: z = (z + F z_r) / (1 + F), F = K dt sqrt(N) / d
    // in cells (the spacing cancels at m = 1/2, n = 1). A river cuts to the sea and no lower.
    for (u32 k = 0; k < n; ++k) {
      const u32 p = order[k];
      const u8 code = receiver[p];
      if (code == k_no_receiver) continue;
      if (static_cast<f64>(area[p]) < channel_cells) continue;  // hillslope: no river here
      const f64 zp = z[p];
      if (has_sea && zp <= sea) continue;
      f64 base = z[neighbour(p, o.at[code])];
      if (has_sea && base < sea) base = sea;
      if (zp <= base) continue;
      const f64 f = hardness[p] * rock.at(zp).erodibility * std::sqrt(static_cast<f64>(area[p])) *
                    ((code & 1u) != 0 ? k_inv_sqrt2 : 1.0);
      z[p] = (zp + f * base) / (1.0 + f);
    }
    // Hillslopes: a Jacobi step reads one surface and writes the other, so rows run in parallel.
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
            dst[p] = c + rr * sum;
          }
      });
      f64* t = from;
      from = to;
      to = t;
    }
    if (from != z)
      for (u32 p = 0; p < n; ++p)
        z[p] = from[p];
    // The limiter in flood order: nearly ascending, so one pass settles almost every slope, and
    // the pass after the last step settles the rest.
    for (u32 k = 0; k < n; ++k) {
      const u32 p = order[k];
      if (held[p] != 0) continue;
      (void)limit_point(z, p, static_cast<i32>(p % w), static_cast<i32>(p / w), wi, hi, rock, card,
                        diag);
    }
  }
  if (steps > 0) {
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
            moved |= limit_point(z, p, x, y, wi, hi, rock, card, diag);
          }
        }
      }
      if (!moved) break;
    }
  }

  // ---- the outputs: whole millimetres, and the fields of exactly that ground -----------------
  out = Landform{};
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
