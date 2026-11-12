// The routes planner (docs/subsystems/routes.md, "The plan"): the lattice, the search, the network,
// the lines and their stations; and what a plan is written as (the treads' edits, their mask
// regions and footprints) and how its wear is asked.
#include "detail.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::routes {

namespace {

bool fail(std::string& error, std::string_view field) {
  error = "engine.routes: invalid " + std::string(field);
  return false;
}
bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }  // a NaN fails
f64 wide(f32 v) { return static_cast<f64>(v); }
// Whole centimetres and millimetres, halves away from zero: exact operations, the same everywhere.
f64 to_cm(f64 metres) { return static_cast<f64>(std::llround(metres * 100.0)) / 100.0; }
f64 to_mm(f64 metres) { return static_cast<f64>(std::llround(metres * 1000.0)) / 1000.0; }

u64 g_plans_made = 0;

// The sixteen headings a lattice step may take, in order of their angle: the eight neighbours and
// the eight knight's moves between them, so a line on the lattice holds a heading within about 13
// degrees of any.
constexpr i32 k_dx[16] = {1, 2, 1, 1, 0, -1, -1, -2, -1, -2, -1, -1, 0, 1, 1, 2};
constexpr i32 k_dz[16] = {0, 1, 1, 2, 1, 2, 1, 1, 0, -1, -1, -2, -1, -2, -1, -1};
// The square of each step's length in spacings.
constexpr i32 k_len2[16] = {1, 5, 2, 5, 1, 5, 2, 5, 1, 5, 2, 5, 1, 5, 2, 5};

// How far two headings turn, in steps of the sixteen (0 to 8): 1 is under about 27 degrees, 2 is
// 45, 4 is 90, past 4 is a switchback.
u32 turn_steps(u32 a, u32 b) {
  const u32 d = a > b ? a - b : b - a;
  return d > 8 ? 16 - d : d;
}

struct Lattice {
  u32 width = 0, depth = 0;
  i64 x_mm = 0, z_mm = 0, s_mm = 0;
  Vector<i64> h;  // whole millimetres
  u32 count() const { return width * depth; }
  i64 xi(u32 i) const { return x_mm + static_cast<i64>(i % width) * s_mm; }
  i64 zi(u32 i) const { return z_mm + static_cast<i64>(i / width) * s_mm; }
  // The neighbour of `i` along heading `d`, or ~0u off the lattice.
  u32 step(u32 i, u32 d) const {
    const i64 x = static_cast<i64>(i % width) + k_dx[d];
    const i64 z = static_cast<i64>(i / width) + k_dz[d];
    if (x < 0 || z < 0 || x >= static_cast<i64>(width) || z >= static_cast<i64>(depth)) return ~0u;
    return static_cast<u32>(z) * width + static_cast<u32>(x);
  }
};

bool sample(const RouteLayer& p, const scene_gen::Ground& ground, jobs::JobSystem* jobs, Lattice& n,
            std::string& error) {
  n = {};
  if (!ground.present()) return fail(error, "ground (required)");
  if (!layer_in_range(p, error)) return false;
  n.s_mm = scene_gen::nearest_mm(wide(p.spacing_m));
  n.x_mm = scene_gen::nearest_mm(p.region_min[0]);
  n.z_mm = scene_gen::nearest_mm(p.region_min[1]);
  const u64 w = static_cast<u64>((scene_gen::nearest_mm(p.region_max[0]) - n.x_mm) / n.s_mm) + 1;
  const u64 h = static_cast<u64>((scene_gen::nearest_mm(p.region_max[1]) - n.z_mm) / n.s_mm) + 1;
  if (w * h > p.max_samples) return fail(error, "max_samples (region exceeds the lattice budget)");
  n.width = static_cast<u32>(w);
  n.depth = static_cast<u32>(h);
  n.h.resize(n.count());
  Vector<u8> bad(n.depth, 0);
  // A row a job: each writes its own heights, so the bytes are the same whatever ran them.
  auto rows = [&](u32 begin, u32 end) {
    for (u32 r = begin; r < end; ++r)
      for (u32 c = 0; c < n.width; ++c) {
        const u32 i = r * n.width + c;
        const f64 y = ground.surface_mm(n.xi(i), n.zi(i));
        if (!in(y, -1e6, 1e6)) {
          bad[r] = 1;
          n.h[i] = 0;
        } else {
          n.h[i] = std::llround(y * 1000.0);
        }
      }
  };
  if (jobs != nullptr && n.depth > 1)
    jobs->parallel_for(jobs::Pool::Performance, n.depth, 8,
                       [&](u32 begin, u32 end) { rows(begin, end); });
  else
    rows(0, n.depth);
  for (const u8 b : bad)
    if (b) return fail(error, "ground height (finite, +/-1e6 m)");
  return true;
}

// **A step's time and whether it is steep** (routes.md, "The plan"). Tobler's hiking function,
// W = 6 exp(-3.5 |g + 0.05|) km/h for the grade g, walked both ways: the step's time is its length
// over each direction's speed, averaged — 0.6 (exp(3.5 |g + 0.05|) + exp(3.5 |g - 0.05|)) / 2
// seconds a metre — rounded once to whole milliseconds. Steep: the rise is more than the cap
// times the run, products and one correctly rounded square root.
struct StepCost {
  u32 ms = 0;
  bool steep = false;
};
StepCost step_cost(const Lattice& n, u32 i, u32 j, u32 d, f64 cap) {
  const f64 run_mm = static_cast<f64>(n.s_mm) * std::sqrt(static_cast<f64>(k_len2[d]));
  const f64 rise_mm = static_cast<f64>(n.h[j] - n.h[i]);
  const f64 g = rise_mm / run_mm;
  const f64 up = det::exp(3.5 * std::fabs(g + 0.05));
  const f64 down = det::exp(3.5 * std::fabs(g - 0.05));
  const f64 seconds = run_mm / 1000.0 * 0.6 * (up + down) * 0.5;
  StepCost c;
  const f64 ms = seconds * 1000.0;
  c.ms = ms >= 4e9 ? 4000000000u : static_cast<u32>(std::llround(ms));
  c.steep = std::fabs(rise_mm) > cap * run_mm;
  return c;
}

// Integer shares, read once from the layer: a step's time times `permille` over a thousand.
u64 scaled(u64 ms, u64 permille) { return (ms * permille + 500) / 1000; }

struct Search {
  const Lattice* n = nullptr;
  // A step's cost from `i` along heading `d`, for the eight headings 0 to 7 (the other eight are
  // the same steps the other way: heading d + 8 from i is heading d from the neighbour).
  Vector<u32> ms;
  Vector<u8> steep;
  Vector<u8> trodden;               // 1 where an earlier route runs
  const Vector<u8>* wet = nullptr;  // the lattice's water (`RouteNetwork::wet`), or none
  u64 trodden_pm = 500, steep_pm = 20000, ford_pm = 4000, turn_ms = 2000, switchback_ms = 30000;
  Vector<u64> dist;
  Vector<u32> prev;
  struct Item {
    u64 cost;
    u32 state;
  };
  Vector<Item> heap;

  bool cost_of(u32 i, u32 d, u32& j, u64& out, bool allow_steep, bool& is_steep) const {
    j = n->step(i, d);
    if (j == ~0u) return false;
    const u32 from = d < 8 ? i : j;
    const u32 k = (d < 8 ? d : d - 8);
    const u64 base = ms[from * 8 + k];
    is_steep = steep[from * 8 + k] != 0;
    if (is_steep && !allow_steep) return false;
    u64 c = base;
    if (wet != nullptr && !wet->empty()) {
      const u8 a = (*wet)[i], b = (*wet)[j];
      if (a == 2 || b == 2) return false;  // never into a water body
      if (a == 1 || b == 1) c = scaled(c, ford_pm);
    }
    if (trodden[i] && trodden[j]) c = scaled(c, trodden_pm);
    if (is_steep) c = scaled(c, steep_pm);
    out = c;
    return true;
  }
  u64 turn(u32 a, u32 b) const {
    const u32 t = turn_steps(a, b);
    return t <= 1 ? 0 : (t <= 4 ? turn_ms : switchback_ms);
  }
  // The least-cost path from `src` to `dst`, cells in order; false when there is none.
  bool run(u32 src, u32 dst, bool allow_steep, Vector<u32>& path) {
    path.clear();
    if (src == dst) {
      path.push_back(src);
      return true;
    }
    const u32 states = n->count() * 16;
    dist.assign(states, ~0ull);
    prev.assign(states, ~0u);
    heap.clear();
    const auto greater = [](const Item& a, const Item& b) {
      return a.cost != b.cost ? a.cost > b.cost : a.state > b.state;
    };
    const auto push = [&](u64 c, u32 s, u32 from) {
      if (c >= dist[s]) return;
      dist[s] = c;
      prev[s] = from;
      heap.push_back({c, s});
      std::push_heap(heap.begin(), heap.end(), greater);
    };
    constexpr u32 k_source = ~1u;
    for (u32 d = 0; d < 16; ++d) {
      u32 j = 0;
      u64 c = 0;
      bool st = false;
      if (cost_of(src, d, j, c, allow_steep, st)) push(c, j * 16 + d, k_source);
    }
    u32 found = ~0u;
    while (!heap.empty()) {
      std::pop_heap(heap.begin(), heap.end(), greater);
      const Item top = heap.back();
      heap.pop_back();
      if (top.cost != dist[top.state]) continue;
      const u32 cell = top.state / 16, d = top.state % 16;
      if (cell == dst) {
        found = top.state;
        break;
      }
      for (u32 e = 0; e < 16; ++e) {
        u32 j = 0;
        u64 c = 0;
        bool st = false;
        if (!cost_of(cell, e, j, c, allow_steep, st)) continue;
        push(top.cost + c + turn(d, e), j * 16 + e, top.state);
      }
    }
    if (found == ~0u) return false;
    for (u32 s = found; s != k_source; s = prev[s])
      path.push_back(s / 16);
    path.push_back(src);
    std::reverse(path.begin(), path.end());
    return true;
  }
};

// The lattice cell nearest a waypoint, clamped into the region.
u32 cell_of(const Lattice& n, const WorldPos& at) {
  const auto index = [&](f64 v, i64 origin, u32 size) {
    const i64 q = (scene_gen::nearest_mm(v) - origin) * 2 + n.s_mm;
    i64 k = q >= 0 ? q / (2 * n.s_mm) : -((-q + 2 * n.s_mm - 1) / (2 * n.s_mm));
    return static_cast<u32>(std::clamp<i64>(k, 0, static_cast<i64>(size) - 1));
  };
  return index(at.z, n.z_mm, n.depth) * n.width + index(at.x, n.x_mm, n.width);
}

struct PolyPoint {
  f64 x = 0, z = 0;
  u32 flags = 0;  // the stretch to the next point: k_station_steep, k_station_ford
};

f64 ground_at(const scene_gen::Ground& g, f64 x, f64 z) {
  return to_mm(g.surface_mm(scene_gen::nearest_mm(x), scene_gen::nearest_mm(z)));
}

// **The corners cut** (Chaikin's passes): a corner at b between a and c becomes the points a
// quarter of the way back toward a and a quarter on toward c, unless the stretch between them is
// over the cap where neither of the two it replaces was. The ends stay.
void smooth(Vector<PolyPoint>& line, u32 passes, f64 cap, const scene_gen::Ground& ground) {
  for (u32 pass = 0; pass < passes && line.size() >= 3; ++pass) {
    Vector<PolyPoint> out;
    out.push_back(line[0]);
    for (u32 k = 1; k + 1 < line.size(); ++k) {
      const PolyPoint& a = line[k - 1];
      const PolyPoint& b = line[k];
      const PolyPoint& c = line[k + 1];
      PolyPoint r{.75 * b.x + .25 * a.x, .75 * b.z + .25 * a.z, a.flags};
      PolyPoint q{.75 * b.x + .25 * c.x, .75 * b.z + .25 * c.z, b.flags};
      const bool steep = ((a.flags | b.flags) & k_station_steep) != 0;
      const f64 dx = q.x - r.x, dz = q.z - r.z;
      const f64 run = std::sqrt(dx * dx + dz * dz);
      const f64 rise = std::fabs(ground_at(ground, q.x, q.z) - ground_at(ground, r.x, r.z));
      if (!steep && rise > cap * run) {
        out.push_back(b);
        continue;
      }
      // The stretch r to q is the corner's: steep or a ford if either side was.
      r.flags = a.flags | b.flags;
      out.push_back(r);
      out.push_back(q);
    }
    out.push_back(line.back());
    // A point's flags are its stretch to the next: the corner's own (set on r) and b's onward.
    line = std::move(out);
  }
}

// **The stations** (routes.md, "The plan"): `count + 1` points spaced evenly
// along the line, x and z whole centimetres; each one's lowest ground across the tread; the tread's
// top that, held under the cap along the line on the stretches that are not steep, then sunk.
void station(const RouteLayer& p, const Vector<PolyPoint>& line, const scene_gen::Ground& ground,
             const scene_gen::Exclusion* water, RouteLine& out) {
  Vector<f64> along(line.size(), 0.0);
  for (u32 k = 1; k < line.size(); ++k) {
    const f64 dx = line[k].x - line[k - 1].x, dz = line[k].z - line[k - 1].z;
    along[k] = along[k - 1] + std::sqrt(dx * dx + dz * dz);
  }
  const f64 total = along.back();
  const f64 step = wide(p.station_m);
  const u32 count = std::max<u32>(1, static_cast<u32>(std::llround(total / step)));
  out.stations.clear();
  u32 seg = 0;
  for (u32 k = 0; k <= count; ++k) {
    const f64 s = total * static_cast<f64>(k) / static_cast<f64>(count);
    while (seg + 2 < line.size() && along[seg + 1] <= s)
      ++seg;
    const f64 len = along[seg + 1] - along[seg];
    const f64 t = len > 0 ? std::clamp((s - along[seg]) / len, 0.0, 1.0) : 0.0;
    Station st;
    st.at.x = to_cm(line[seg].x + t * (line[seg + 1].x - line[seg].x));
    st.at.z = to_cm(line[seg].z + t * (line[seg + 1].z - line[seg].z));
    st.flags = k < count ? (line[seg].flags & k_station_steep) : 0u;
    out.stations.push_back(st);
  }
  const u32 m = out.stations.size();
  // A ford: a stretch with an end in a channel's corridor, asked at the stations themselves.
  if (water != nullptr && !water->empty()) {
    Vector<u8> inside(m, 0);
    for (u32 k = 0; k < m; ++k)
      inside[k] = water->excluded_mm(ground, scene_gen::nearest_mm(out.stations[k].at.x),
                                     scene_gen::nearest_mm(out.stations[k].at.z),
                                     scene_gen::k_exclude_channels)
                      ? 1
                      : 0;
    for (u32 k = 0; k + 1 < m; ++k)
      if (inside[k] || inside[k + 1]) out.stations[k].flags |= k_station_ford;
  }
  const f64 half = .5 * wide(p.tread_width_m);
  Vector<i64> low(m, 0);
  for (u32 k = 0; k < m; ++k) {
    Station& st = out.stations[k];
    const Station& a = out.stations[k > 0 ? k - 1 : 0];
    const Station& b = out.stations[k + 1 < m ? k + 1 : m - 1];
    f64 tx = b.at.x - a.at.x, tz = b.at.z - a.at.z;
    const f64 tl = std::sqrt(tx * tx + tz * tz);
    tx = tl > 0 ? tx / tl : 1.0;
    tz = tl > 0 ? tz / tl : 0.0;
    const f64 c = ground_at(ground, st.at.x, st.at.z);
    const f64 l = ground_at(ground, st.at.x - tz * half, st.at.z + tx * half);
    const f64 r = ground_at(ground, st.at.x + tz * half, st.at.z - tx * half);
    st.ground = static_cast<f32>(c);
    low[k] = std::llround(std::min(c, std::min(l, r)) * 1000.0);
  }
  // The lower envelope of cones of slope `cap` over the lowest ground, in whole millimetres, a
  // pass each way; a steep stretch passes nothing on. Whole millimetres of rise allowed per stretch
  // (the floor of cap times its run), so a held stretch is under the cap exactly.
  Vector<i64> rise(m, 0);
  for (u32 k = 0; k + 1 < m; ++k) {
    const f64 dx = out.stations[k + 1].at.x - out.stations[k].at.x;
    const f64 dz = out.stations[k + 1].at.z - out.stations[k].at.z;
    const f64 ds = std::sqrt(dx * dx + dz * dz);
    out.length_m += ds;
    rise[k] = static_cast<i64>(wide(p.grade_cap) * ds * 1000.0);
  }
  Vector<i64> top = low;
  for (u32 k = 1; k < m; ++k)
    if ((out.stations[k - 1].flags & k_station_steep) == 0)
      top[k] = std::min(top[k], top[k - 1] + rise[k - 1]);
  for (u32 k = m - 1; k-- > 0;)
    if ((out.stations[k].flags & k_station_steep) == 0)
      top[k] = std::min(top[k], top[k + 1] + rise[k]);
  const i64 sink = std::llround(wide(p.sink_m) * 1000.0);
  for (u32 k = 0; k < m; ++k) {
    out.max_hold_m = std::max(out.max_hold_m, static_cast<f64>(low[k] - top[k]) / 1000.0);
    out.stations[k].at.y = static_cast<f64>(top[k] - sink) / 1000.0;
  }
  for (u32 k = 0; k + 1 < m; ++k) {
    const i64 dy = top[k + 1] - top[k];
    if ((dy < 0 ? -dy : dy) > rise[k]) {
      const f64 dx = out.stations[k + 1].at.x - out.stations[k].at.x;
      const f64 dz = out.stations[k + 1].at.z - out.stations[k].at.z;
      out.over_cap_m += std::sqrt(dx * dx + dz * dz);
    }
  }
}

// The runs of ford stretches of a line: run r is stations `from[r]` to `to[r]` (its last stretch's
// far end).
void ford_runs(const RouteLine& l, Vector<u32>& from, Vector<u32>& to) {
  from.clear();
  to.clear();
  for (u32 k = 0; k + 1 < l.stations.size(); ++k) {
    if ((l.stations[k].flags & k_station_ford) == 0) continue;
    if (to.empty() || to.back() != k) {
      from.push_back(k);
      to.push_back(k + 1);
    } else {
      to.back() = k + 1;
    }
  }
}

}  // namespace

bool layer_in_range(const RouteLayer& p, std::string& error) {
  const f64 s = wide(p.spacing_m);
  if (!in(s, .5, 16) || scene_gen::nearest_mm(s) <= 0) return fail(error, "spacing_m (0.5 to 16)");
  const i64 smm = scene_gen::nearest_mm(s);
  for (u32 a = 0; a < 2; ++a) {
    if (!in(p.region_min[a], -1e9, 1e9) || !in(p.region_max[a], -1e9, 1e9) ||
        !(p.region_max[a] > p.region_min[a]))
      return fail(error, "region (finite, min below max)");
    const i64 lo = scene_gen::nearest_mm(p.region_min[a]);
    const i64 hi = scene_gen::nearest_mm(p.region_max[a]);
    if ((hi - lo) % smm != 0 || (hi - lo) / smm < 2)
      return fail(error, "region (a whole number of spacings, at least two)");
  }
  if (!in(static_cast<f64>(p.max_samples), 4, 4194304)) return fail(error, "max_samples");
  if (!in(wide(p.grade_cap), .02, 1)) return fail(error, "grade_cap (0.02 to 1)");
  if (!in(wide(p.steep_penalty), 1, 1000)) return fail(error, "steep_penalty (1 to 1000)");
  if (!in(wide(p.trodden), .05, 1)) return fail(error, "trodden (0.05 to 1)");
  if (!in(wide(p.turn_s), 0, 600) || !in(wide(p.switchback_s), 0, 600))
    return fail(error, "turn_s and switchback_s (0 to 600)");
  if (!in(wide(p.ford_penalty), 1, 100)) return fail(error, "ford_penalty (1 to 100)");
  if (p.smooth_passes > 4) return fail(error, "smooth_passes (0 to 4)");
  if (!in(wide(p.station_m), .1, 4)) return fail(error, "station_m (0.1 to 4)");
  if (!in(wide(p.tread_width_m), .3, 1.5)) return fail(error, "tread_width_m (0.3 to 1.5)");
  if (!in(wide(p.sink_m), 0, .3)) return fail(error, "sink_m (0 to 0.3)");
  if (!in(wide(p.backslope), .25, 8)) return fail(error, "backslope (0.25 to 8)");
  if (!in(wide(p.fill_slope), .1, 8)) return fail(error, "fill_slope (0.1 to 8)");
  if (!in(wide(p.edge_round), .1, 1)) return fail(error, "edge_round (0.1 to 1)");
  if (!in(wide(p.face_m), .1, 10) || !in(wide(p.fill_m), .1, 10))
    return fail(error, "face_m and fill_m (0.1 to 10)");
  if (!in(wide(p.wear_m), 0, 20)) return fail(error, "wear_m (0 to 20)");
  if (!in(wide(p.tread_cover), 0, 1)) return fail(error, "tread_cover (0 to 1)");
  if (!in(wide(p.tread_feather_m), 0, 2)) return fail(error, "tread_feather_m (0 to 2)");
  for (const Route& r : p.routes) {
    if (r.waypoints.size() < 2) return fail(error, "routes (two or more waypoints each)");
    for (const WorldPos& w : r.waypoints)
      if (!in(w.x, -1e9, 1e9) || !in(w.z, -1e9, 1e9)) return fail(error, "a waypoint (finite)");
  }
  return true;
}

bool detail::sample_lattice(const RouteLayer& p, const scene_gen::Ground& ground,
                            jobs::JobSystem* jobs, RouteNetwork& out, std::string& error,
                            const scene_gen::Exclusion* water) {
  out = {};
  Lattice n;
  if (!sample(p, ground, jobs, n, error)) return false;
  out.width = n.width;
  out.depth = n.depth;
  out.x_mm = n.x_mm;
  out.z_mm = n.z_mm;
  out.spacing_mm = n.s_mm;
  // **Water** (routes.md, "Water"): a body's inside is 2, a channel's corridor 1, asked of the
  // exclusion at each lattice point, whose answers are integer millimetres in.
  if (water != nullptr && !water->empty()) {
    out.wet.assign(n.count(), 0);
    for (u32 i = 0; i < n.count(); ++i) {
      const i64 x = n.xi(i), z = n.zi(i);
      if (water->excluded_mm(ground, x, z, scene_gen::k_exclude_water))
        out.wet[i] = 2;
      else if (water->excluded_mm(ground, x, z, scene_gen::k_exclude_channels))
        out.wet[i] = 1;
    }
  }
  out.ground_mm = std::move(n.h);
  return true;
}

bool plan_routes(const RouteLayer& p, const scene_gen::Ground& ground, jobs::JobSystem* jobs,
                 RouteNetwork& out, std::string& error, const scene_gen::Exclusion* water) {
  return detail::sample_lattice(p, ground, jobs, out, error, water) &&
         detail::plan_from_lattice(p, ground, out, error, water);
}

bool detail::plan_from_lattice(const RouteLayer& p, const scene_gen::Ground& ground,
                               RouteNetwork& out, std::string& error,
                               const scene_gen::Exclusion* water) {
  if (out.ground_mm.size() != out.width * out.depth || out.width < 2 || out.depth < 2)
    return fail(error, "lattice (sample it first)");
  Lattice n;
  n.width = out.width;
  n.depth = out.depth;
  n.x_mm = out.x_mm;
  n.z_mm = out.z_mm;
  n.s_mm = out.spacing_mm;
  n.h = std::move(out.ground_mm);
  Vector<u8> wet = std::move(out.wet);
  if (!wet.empty() && wet.size() != n.count()) return fail(error, "wet (one a lattice point)");
  out = {};
  ++g_plans_made;
  const u32 cells = n.count();
  const f64 cap = wide(p.grade_cap);
  Search search;
  search.n = &n;
  search.ms.resize(cells * 8);
  search.steep.resize(cells * 8);
  search.trodden.assign(cells, 0);
  for (u32 i = 0; i < cells; ++i)
    for (u32 d = 0; d < 8; ++d) {
      const u32 j = n.step(i, d);
      if (j == ~0u) continue;
      const StepCost c = step_cost(n, i, j, d, cap);
      search.ms[i * 8 + d] = c.ms;
      search.steep[i * 8 + d] = c.steep ? 1 : 0;
    }
  search.trodden_pm = static_cast<u64>(std::llround(wide(p.trodden) * 1000.0));
  search.steep_pm = static_cast<u64>(std::llround(wide(p.steep_penalty) * 1000.0));
  search.turn_ms = static_cast<u64>(std::llround(wide(p.turn_s) * 1000.0));
  search.switchback_ms = static_cast<u64>(std::llround(wide(p.switchback_s) * 1000.0));
  search.ford_pm = static_cast<u64>(std::llround(wide(p.ford_penalty) * 1000.0));
  search.wet = &wet;

  // **The legs**, route by route in the layer's order and waypoint pair by pair: each searched
  // under the cap, and again with steep steps at their penalty where that finds nothing. Every
  // step taken joins the union of edges and makes its cells trodden for the legs after it.
  Vector<u64> edges;  // (lower cell << 32 | higher cell)
  Vector<u8> stop(cells, 0);
  Vector<u32> path;
  for (const Route& r : p.routes) {
    for (u32 w = 0; w + 1 < r.waypoints.size(); ++w) {
      const u32 a = cell_of(n, r.waypoints[w]);
      const u32 b = cell_of(n, r.waypoints[w + 1]);
      stop[a] = 1;
      stop[b] = 1;
      ++out.legs;
      if (!search.run(a, b, false, path)) {
        ++out.steep_legs;
        if (!search.run(a, b, true, path)) {
          // Not steep: cut off (a water body between the two, or a waypoint inside one).
          --out.steep_legs;
          ++out.unreached;
          continue;
        }
      }
      for (u32 k = 0; k < path.size(); ++k) {
        search.trodden[path[k]] = 1;
        if (k + 1 < path.size()) {
          const u32 lo = std::min(path[k], path[k + 1]), hi = std::max(path[k], path[k + 1]);
          edges.push_back(static_cast<u64>(lo) << 32 | hi);
        }
      }
    }
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  // **The network**: the union of the steps, cut into lines at every end, junction and waypoint.
  // Adjacency in ascending order of the neighbour, so every walk below is a total order.
  Vector<u32> degree(cells + 1, 0);
  for (const u64 e : edges) {
    ++degree[static_cast<u32>(e >> 32)];
    ++degree[static_cast<u32>(e)];
  }
  Vector<u32> first(cells + 1, 0);
  for (u32 i = 0; i < cells; ++i)
    first[i + 1] = first[i] + degree[i];
  Vector<u32> adj(first[cells], 0), edge_of(first[cells], 0), fill = first;
  for (u32 k = 0; k < edges.size(); ++k) {
    const u32 a = static_cast<u32>(edges[k] >> 32), b = static_cast<u32>(edges[k]);
    edge_of[fill[a]] = k;
    adj[fill[a]++] = b;
    edge_of[fill[b]] = k;
    adj[fill[b]++] = a;
  }
  const auto node = [&](u32 i) { return degree[i] != 2 || stop[i] != 0; };
  Vector<u8> used(edges.size(), 0);
  Vector<Vector<u32>> chains;
  const auto walk = [&](u32 start, u32 slot) {
    Vector<u32> chain;
    chain.push_back(start);
    u32 at = start;
    for (;;) {
      used[edge_of[slot]] = 1;
      const u32 next = adj[slot];
      chain.push_back(next);
      at = next;
      if (node(at) || at == start) break;
      u32 onward = ~0u;
      for (u32 q = first[at]; q < first[at + 1]; ++q)
        if (!used[edge_of[q]]) {
          onward = q;
          break;
        }
      if (onward == ~0u) break;
      slot = onward;
    }
    chains.push_back(std::move(chain));
  };
  for (u32 i = 0; i < cells; ++i)
    if (degree[i] > 0 && node(i))
      for (u32 q = first[i]; q < first[i + 1]; ++q)
        if (!used[edge_of[q]]) walk(i, q);
  for (u32 i = 0; i < cells; ++i)  // loops with no node on them
    for (u32 q = first[i]; q < first[i + 1]; ++q)
      if (!used[edge_of[q]]) walk(i, q);

  // **The lines**: each chain's points on the lattice, its corners cut, stationed.
  for (Vector<u32>& chain : chains) {
    RouteLine line;
    line.cells = chain;
    Vector<PolyPoint> poly;
    for (u32 k = 0; k < chain.size(); ++k) {
      PolyPoint pt{static_cast<f64>(n.xi(chain[k])) / 1000.0,
                   static_cast<f64>(n.zi(chain[k])) / 1000.0, 0};
      if (k + 1 < chain.size()) {
        const u32 i = chain[k], j = chain[k + 1];
        const i64 dx = static_cast<i64>(j % n.width) - static_cast<i64>(i % n.width);
        const i64 dz = static_cast<i64>(j / n.width) - static_cast<i64>(i / n.width);
        u32 d = 0;
        while (d < 16 && (k_dx[d] != dx || k_dz[d] != dz))
          ++d;
        const bool steep = d < 16 && step_cost(n, i, j, d, cap).steep;
        pt.flags = steep ? k_station_steep : 0u;
        line.steep_steps += steep ? 1u : 0u;
        if (k > 0) {
          const u32 h = chain[k - 1];
          const i64 px = static_cast<i64>(i % n.width) - static_cast<i64>(h % n.width);
          const i64 pz = static_cast<i64>(i / n.width) - static_cast<i64>(h / n.width);
          u32 e = 0;
          while (e < 16 && (k_dx[e] != px || k_dz[e] != pz))
            ++e;
          if (d < 16 && e < 16 && turn_steps(d, e) > 4) ++line.switchbacks;
        }
      }
      poly.push_back(pt);
    }
    smooth(poly, p.smooth_passes, cap, ground);
    station(p, poly, ground, water, line);
    out.length_m += line.length_m;
    out.over_cap_m += line.over_cap_m;
    out.switchbacks += line.switchbacks;
    Vector<u32> from, to;
    ford_runs(line, from, to);
    out.fords += from.size();
    for (u32 r = 0; r < from.size(); ++r)
      for (u32 k = from[r]; k < to[r]; ++k) {
        const f64 dx = line.stations[k + 1].at.x - line.stations[k].at.x;
        const f64 dz = line.stations[k + 1].at.z - line.stations[k].at.z;
        out.ford_m += std::sqrt(dx * dx + dz * dz);
      }
    out.lines.push_back(std::move(line));
  }
  out.width = n.width;
  out.depth = n.depth;
  out.x_mm = n.x_mm;
  out.z_mm = n.z_mm;
  out.spacing_mm = n.s_mm;
  out.ground_mm = std::move(n.h);
  out.wet = std::move(wet);
  return true;
}

JsonValue crossings_json(const RouteNetwork& net) {
  JsonValue list = JsonValue::array();
  Vector<u32> from, to;
  for (u32 k = 0; k < net.lines.size(); ++k) {
    const RouteLine& l = net.lines[k];
    ford_runs(l, from, to);
    for (u32 r = 0; r < from.size(); ++r) {
      f64 length = 0;
      for (u32 q = from[r]; q < to[r]; ++q) {
        const f64 dx = l.stations[q + 1].at.x - l.stations[q].at.x;
        const f64 dz = l.stations[q + 1].at.z - l.stations[q].at.z;
        length += std::sqrt(dx * dx + dz * dz);
      }
      const Station& mid = l.stations[(from[r] + to[r]) / 2];
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(mid.at.x));
      at.push_back(JsonValue(mid.at.y));
      at.push_back(JsonValue(mid.at.z));
      JsonValue o = JsonValue::object();
      o["line"] = JsonValue(k);
      o["from"] = JsonValue(from[r]);
      o["to"] = JsonValue(to[r]);
      o["at"] = std::move(at);
      o["length_m"] = JsonValue(to_cm(length));
      list.push_back(std::move(o));
    }
  }
  return list;
}

u64 plans_made() noexcept { return g_plans_made; }

// ---- what a plan is written as
// -------------------------------------------------------------------

namespace {

// The stations a straight run between two of them misses by no more than `tol` metres in x and z
// (and in y with `with_y`): the first, the last and every one a run had to stop at.
Vector<u32> thinned(const Vector<Station>& st, bool with_y, f64 tol = 0.005, u32 longest = 64) {
  Vector<u32> keep;
  if (st.empty()) return keep;
  keep.push_back(0);
  u32 a = 0;
  while (a + 1 < st.size()) {
    u32 b = a + 1;
    while (b + 1 < st.size() && b + 1 - a <= longest) {
      const u32 c = b + 1;
      const f64 dx = st[c].at.x - st[a].at.x, dz = st[c].at.z - st[a].at.z;
      const f64 len2 = dx * dx + dz * dz;
      bool ok = len2 > 0;
      for (u32 q = a + 1; q < c && ok; ++q) {
        const f64 px = st[q].at.x - st[a].at.x, pz = st[q].at.z - st[a].at.z;
        const f64 t = std::clamp((px * dx + pz * dz) / len2, 0.0, 1.0);
        const f64 ex = px - t * dx, ez = pz - t * dz;
        if (ex * ex + ez * ez > tol * tol) ok = false;
        if (with_y && std::fabs(st[a].at.y + t * (st[c].at.y - st[a].at.y) - st[q].at.y) > tol)
          ok = false;
      }
      if (!ok) break;
      b = c;
    }
    keep.push_back(b);
    a = b;
  }
  return keep;
}

JsonValue point_json(f64 x, f64 y, f64 z) {
  JsonValue at = JsonValue::array();
  at.push_back(JsonValue(x));
  at.push_back(JsonValue(y));
  at.push_back(JsonValue(z));
  return at;
}

std::string line_name(const RouteLayer& p, u32 k) { return p.name + " line " + std::to_string(k); }

}  // namespace

JsonValue tread_edits_json(const RouteLayer& p, const RouteNetwork& net) {
  JsonValue list = JsonValue::array();
  for (u32 k = 0; k < net.lines.size(); ++k) {
    const Vector<Station>& st = net.lines[k].stations;
    if (st.size() < 2) continue;
    const Vector<u32> keep = thinned(st, true);
    JsonValue points = JsonValue::array(), widths = JsonValue::array();
    for (const u32 i : keep) {
      points.push_back(point_json(st[i].at.x, st[i].at.y, st[i].at.z));
      widths.push_back(JsonValue(static_cast<f64>(p.tread_width_m)));
    }
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(line_name(p, k));
    o["mode"] = JsonValue("Bench");
    o["points"] = std::move(points);
    o["width_m"] = std::move(widths);
    o["bank_slope"] = JsonValue(static_cast<f64>(p.backslope));
    o["fill_slope"] = JsonValue(static_cast<f64>(p.fill_slope));
    o["bank_round"] = JsonValue(static_cast<f64>(p.edge_round));
    o["face_m"] = JsonValue(static_cast<f64>(p.face_m));
    o["fill_m"] = JsonValue(static_cast<f64>(p.fill_m));
    list.push_back(std::move(o));
  }
  return list;
}

JsonValue tread_regions_json(const RouteLayer& p, const RouteNetwork& net) {
  JsonValue list = JsonValue::array();
  if (p.tread_material.empty()) return list;
  const f64 half = .5 * wide(p.tread_width_m);
  for (u32 k = 0; k < net.lines.size(); ++k) {
    const Vector<Station>& st = net.lines[k].stations;
    if (st.size() < 2) continue;
    const Vector<u32> keep = thinned(st, false);
    Vector<f64> lx, lz, rx, rz;
    for (u32 q = 0; q < keep.size(); ++q) {
      const Station& a = st[keep[q > 0 ? q - 1 : 0]];
      const Station& b = st[keep[q + 1 < keep.size() ? q + 1 : q]];
      f64 tx = b.at.x - a.at.x, tz = b.at.z - a.at.z;
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      if (!(tl > 0)) continue;
      tx /= tl;
      tz /= tl;
      const Station& c = st[keep[q]];
      lx.push_back(to_cm(c.at.x - tz * half));
      lz.push_back(to_cm(c.at.z + tx * half));
      rx.push_back(to_cm(c.at.x + tz * half));
      rz.push_back(to_cm(c.at.z - tx * half));
    }
    if (lx.size() < 2) continue;
    JsonValue polygon = JsonValue::array();
    for (u32 q = 0; q < lx.size(); ++q)
      polygon.push_back(point_json(lx[q], 0.0, lz[q]));
    for (u32 q = rx.size(); q-- > 0;)
      polygon.push_back(point_json(rx[q], 0.0, rz[q]));
    JsonValue o = JsonValue::object();
    o["name"] = JsonValue(line_name(p, k) + " tread");
    o["polygon"] = std::move(polygon);
    o["material"] = JsonValue(p.tread_material);
    o["feather_m"] = JsonValue(static_cast<f64>(p.tread_feather_m));
    list.push_back(std::move(o));
  }
  return list;
}

void tread_footprints(const RouteLayer& p, const RouteNetwork& net,
                      Vector<scene_gen::Footprint>& out) {
  const i64 half = std::llround(.5 * wide(p.tread_width_m) * 1000.0);
  for (const RouteLine& line : net.lines) {
    if (line.stations.size() < 2) continue;
    const Vector<u32> keep = thinned(line.stations, false);
    for (u32 q = 0; q + 1 < keep.size(); ++q) {
      const Station& a = line.stations[keep[q]];
      const Station& b = line.stations[keep[q + 1]];
      out.push_back(scene_gen::Footprint::band(
          scene_gen::nearest_mm(a.at.x), scene_gen::nearest_mm(a.at.z),
          scene_gen::nearest_mm(b.at.x), scene_gen::nearest_mm(b.at.z), half));
    }
  }
}

// ---- wear
// ----------------------------------------------------------------------------------------

f64 wear_factor(f64 d, f64 wear_m, f64 tread_cover) noexcept {
  if (!(d > 0)) return tread_cover;
  if (!(d < wear_m)) return 1.0;
  const f64 t = d / wear_m;
  return tread_cover + (1.0 - tread_cover) * (t * t * (3.0 - 2.0 * t));
}

namespace {
constexpr f64 k_wear_cell_m = 16.0;
i64 cell_key(f64 v) {
  const f64 q = v / k_wear_cell_m;
  const i64 t = static_cast<i64>(q);
  return static_cast<f64>(t) > q ? t - 1 : t;
}
}  // namespace

void RouteWear::build(const RouteLayer& p, const RouteNetwork& net) {
  segments_.clear();
  cells_.clear();
  wear_m_ = wide(p.wear_m);
  tread_cover_ = wide(p.tread_cover);
  const f64 half = .5 * wide(p.tread_width_m);
  for (const RouteLine& line : net.lines) {
    if (line.stations.size() < 2) continue;
    const Vector<u32> keep = thinned(line.stations, false);
    for (u32 q = 0; q + 1 < keep.size(); ++q) {
      const Station& a = line.stations[keep[q]];
      const Station& b = line.stations[keep[q + 1]];
      segments_.push_back({a.at.x, a.at.z, b.at.x, b.at.z, half});
    }
  }
  // Each segment filed in every cell its box, grown by the half-width and the wear, touches: a
  // query within the wear reads its own cell alone.
  const f64 grow = half + wear_m_;
  for (u32 k = 0; k < segments_.size(); ++k) {
    const Segment& s = segments_[k];
    const i64 x0 = cell_key(std::min(s.ax, s.bx) - grow),
              x1 = cell_key(std::max(s.ax, s.bx) + grow);
    const i64 z0 = cell_key(std::min(s.az, s.bz) - grow),
              z1 = cell_key(std::max(s.az, s.bz) + grow);
    for (i64 z = z0; z <= z1; ++z)
      for (i64 x = x0; x <= x1; ++x)
        cells_.push_back({z, x, k});
  }
  std::sort(cells_.begin(), cells_.end(), [](const Cell& a, const Cell& b) {
    if (a.key_z != b.key_z) return a.key_z < b.key_z;
    if (a.key_x != b.key_x) return a.key_x < b.key_x;
    return a.segment < b.segment;
  });
}

f64 RouteWear::edge_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept {
  constexpr f64 k_far = 1e30;
  if (segments_.empty()) return k_far;
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  // Past the wear the cells hold no more than they were filed for: read every cell in reach.
  const f64 extra = reach_m > wear_m_ ? reach_m - wear_m_ : 0.0;
  const i64 x0 = cell_key(x - extra), x1 = cell_key(x + extra);
  const i64 z0 = cell_key(z - extra), z1 = cell_key(z + extra);
  f64 best = k_far;
  for (i64 cz = z0; cz <= z1; ++cz)
    for (i64 cx = x0; cx <= x1; ++cx) {
      const auto lo = std::lower_bound(
          cells_.begin(), cells_.end(), Cell{cz, cx, 0}, [](const Cell& a, const Cell& b) {
            return a.key_z != b.key_z ? a.key_z < b.key_z : a.key_x < b.key_x;
          });
      for (auto it = lo; it != cells_.end() && it->key_z == cz && it->key_x == cx; ++it) {
        const Segment& s = segments_[it->segment];
        const f64 dx = s.bx - s.ax, dz = s.bz - s.az;
        const f64 len2 = dx * dx + dz * dz;
        const f64 px = x - s.ax, pz = z - s.az;
        const f64 t = len2 > 0 ? std::clamp((px * dx + pz * dz) / len2, 0.0, 1.0) : 0.0;
        const f64 ex = px - t * dx, ez = pz - t * dz;
        const f64 d = std::sqrt(ex * ex + ez * ez) - s.half;
        best = std::min(best, d);
      }
    }
  return best <= reach_m ? best : k_far;
}

f64 RouteWear::cover_mm(i64 x_mm, i64 z_mm) const noexcept {
  if (segments_.empty()) return 1.0;
  return wear_factor(edge_distance_mm(x_mm, z_mm, wear_m_), wear_m_, tread_cover_);
}

}  // namespace engine::routes
