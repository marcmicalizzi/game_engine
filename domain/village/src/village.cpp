// The village's plan (docs/subsystems/village.md, "The plan"): ways found over the ground between
// the places they pass, a green where they meet, plots taken up along them in an order, a house on
// each plot the ground allows, back lanes behind the rows, and what the plan is handed over as —
// ground edits, footprints, mask regions, placements. Every number is grounded, or marked as a
// choice, in docs/experiments/village-growth-grounding.md and village-plan-grounding.md.
#include <core/hash/hash.h>
#include <domain/village/village.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>

namespace engine::village {

namespace {

// Half a metre each side between a house and its plot's side boundaries.
constexpr i64 k_side_gap_mm = 500;
// **The way's cost** (village-growth-grounding.md, "The ways"; choices): a step's length times
// 1 + (grade / 8 %)^2, ten times that over a 25 % grade, plus the wander, up to a quarter of the
// length at `irregularity` 1, over a lattice of 12 m. The search's corridor reaches past the two
// places by a quarter of their distance, 20 to 40 m.
constexpr i64 k_grade_knee = 80;   // per mille
constexpr i64 k_grade_wall = 250;  // per mille
constexpr i64 k_wander_lattice_mm = 12000;
constexpr i64 k_corridor_min_mm = 20000, k_corridor_max_mm = 40000;
// The street's edge wanders over a lattice of 16 m along the way, by up to a quarter of its width.
constexpr i64 k_edge_lattice_mm = 16000;
// A plot turns off square to its way by up to tan 20 degrees.
constexpr f64 k_max_turn = 0.364;
// The church's plot: West Wykeham's churchyard (the grounding's "Where the church and the well
// stand").
constexpr i64 k_church_frontage_mm = 42000, k_church_depth_mm = 24000;
// A back lane: its tread's half width, and how far its line stands behind the plots' backs (a
// toft's margin of 1.5 m and its own half width).
constexpr i64 k_back_half_tread_mm = 1000, k_back_offset_mm = 2500;
// The well's disc, and the worn ground round it.
constexpr i64 k_well_radius_mm = 1500, k_well_yard_mm = 3000;

i64 mm_of(f64 metres) noexcept { return scene_gen::nearest_mm(metres); }
// a / b to the nearest whole, halves away from zero; b positive.
constexpr i64 div_round(i64 a, i64 b) noexcept {
  return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}
constexpr i64 floor_div(i64 a, i64 b) noexcept { return a / b - ((a % b != 0 && (a < 0)) ? 1 : 0); }
bool in_range(f64 v, f64 lo, f64 hi) noexcept { return v >= lo && v <= hi; }  // false for a NaN

// A draw in [0, 1000) from a seed and a salt: the engine's hash, so the same everywhere.
i64 draw(u64 seed, u64 salt) noexcept {
  return static_cast<i64>(mix64(hash_combine(seed, salt)) % 1000u);
}

struct Dir {
  f64 x = 1.0, z = 0.0;
  f64 length_mm = 0.0;
};
// The unit direction from a to b and its length: products, sums, one square root, two divisions.
Dir direction(i64 ax, i64 az, i64 bx, i64 bz) noexcept {
  const f64 dx = static_cast<f64>(bx - ax), dz = static_cast<f64>(bz - az);
  Dir d;
  d.length_mm = std::sqrt(dx * dx + dz * dz);
  if (d.length_mm > 0.0) {
    d.x = dx / d.length_mm;
    d.z = dz / d.length_mm;
  }
  return d;
}

// A quarter turn about y by the engine's rotation (x' = x cos + z sin, z' = -x sin + z cos).
constexpr void quarter(i64& x, i64& z) noexcept {
  const i64 nx = z, nz = -x;
  x = nx;
  z = nz;
}
// The cosine and sine of a built plot's whole turn: the plot's (the house frame's x along its
// front, its z into the plot) after the house's quarter turns.
void turn_of(const Plot& p, f64& c, f64& s) noexcept {
  c = p.along_x;
  s = -p.along_z;
  for (u8 q = 0; q < p.quarter_turns; ++q) {
    const f64 nc = -s, ns = c;
    c = nc;
    s = ns;
  }
}

// A point of the plot's frame (u along its front from the front's middle, v into the plot) in the
// world, to the millimetre.
void plot_point(const Plot& p, f64 u_mm, f64 v_mm, i64& x, i64& z) noexcept {
  x = p.front_x_mm + mm_of((p.along_x * u_mm + p.inward_x * v_mm) / 1000.0);
  z = p.front_z_mm + mm_of((p.along_z * u_mm + p.inward_z * v_mm) / 1000.0);
}
// Where a world point stands in a plot's frame, millimetres.
void plot_frame(const Plot& p, i64 x_mm, i64 z_mm, f64& u, f64& v) noexcept {
  const f64 dx = static_cast<f64>(x_mm - p.front_x_mm), dz = static_cast<f64>(z_mm - p.front_z_mm);
  u = dx * p.along_x + dz * p.along_z;
  v = dx * p.inward_x + dz * p.inward_z;
}

bool refuse(std::string& error, std::string why) {
  error = std::move(why);
  return false;
}

std::string lane_name(const VillageLayer& layer, const VillagePlan* plan, u32 k) {
  if (k < layer.lanes.size() && !layer.lanes[k].name.empty()) return layer.lanes[k].name;
  char text[48];
  if (plan != nullptr && k < plan->lanes.size() && plan->lanes[k].back)
    std::snprintf(text, sizeof(text), "back lane %u", k);
  else
    std::snprintf(text, sizeof(text), "lane %u", k);
  return text;
}

JsonValue point_json(f64 x, f64 y, f64 z) {
  JsonValue p = JsonValue::array();
  p.push_back(JsonValue(x));
  p.push_back(JsonValue(y));
  p.push_back(JsonValue(z));
  return p;
}
f64 metres(i64 mm) noexcept { return static_cast<f64>(mm) / 1000.0; }

const char* state_name(PlotState s) noexcept {
  switch (s) {
    case PlotState::built: return "built";
    case PlotState::spare: return "spare";
    case PlotState::steep: return "steep";
    case PlotState::wet: return "wet";
    case PlotState::no_fit: return "no_fit";
    case PlotState::refused: return "refused";
    case PlotState::reserved: return "reserved";
    case PlotState::outside: return "outside";
  }
  return "spare";
}
const char* kind_name(PlotKind k) noexcept {
  switch (k) {
    case PlotKind::tenement: return "tenement";
    case PlotKind::cottage: return "cottage";
    case PlotKind::church: return "church";
  }
  return "tenement";
}

// ---- the ways ----------------------------------------------------------------------------------

// The wander's value at a world point, 0 to 1000: the engine's hash at the corners of a 12 m
// lattice, blended across the cell in whole numbers. A function of the place and the seed alone,
// so two searches over one place see one field.
i64 wander_at(u64 seed, i64 x_mm, i64 z_mm) noexcept {
  const i64 l = k_wander_lattice_mm;
  const i64 ix = floor_div(x_mm, l), iz = floor_div(z_mm, l);
  const i64 fx = x_mm - ix * l, fz = z_mm - iz * l;
  const auto node = [&](i64 a, i64 b) {
    return static_cast<i64>(
        mix64(hash_combine(hash_combine(seed, static_cast<u64>(a)), static_cast<u64>(b))) % 1001u);
  };
  const i64 v = node(ix, iz) * (l - fx) / l * (l - fz) + node(ix + 1, iz) * fx / l * (l - fz) +
                node(ix, iz + 1) * (l - fx) / l * fz + node(ix + 1, iz + 1) * fx / l * fz;
  return v / l;
}

// **What a way went round** (village-growth-grounding.md, "What a way went round"): the holdings,
// the wet places, the copses and the parks that stood when the way was first trodden and are gone
// now, as the extra a step through them costs, 0 to 3000 per mille. One seeded disc in three cells
// of four on a 40 m lattice, of a radius from 8 to 18 m, the whole of the extra inside seven
// tenths of its radius and falling to none at its rim. A function of the place and the seed alone,
// in whole numbers, so every way through one place goes round the same things. (A 32 m lattice of
// 6 to 16 m discs bent the ways so tightly that the tofts on the inside of a bend stood in the
// street further round it.)
constexpr i64 k_obstacle_lattice_mm = 40000;
constexpr i64 k_obstacle_extra = 3000;
i64 obstacle_at(u64 seed, i64 x_mm, i64 z_mm) noexcept {
  const i64 l = k_obstacle_lattice_mm;
  const i64 ix = floor_div(x_mm, l), iz = floor_div(z_mm, l);
  i64 most = 0;
  for (i64 dj = -1; dj <= 1; ++dj)
    for (i64 di = -1; di <= 1; ++di) {
      const i64 a = ix + di, b = iz + dj;
      const u64 h = mix64(hash_combine(hash_combine(seed ^ 0x0b57ac1eull, static_cast<u64>(a)),
                                       static_cast<u64>(b)));
      if (h % 1000u >= 750u) continue;
      const i64 cx = a * l + static_cast<i64>((h >> 10) % static_cast<u64>(l));
      const i64 cz = b * l + static_cast<i64>((h >> 30) % static_cast<u64>(l));
      const i64 r = 8000 + static_cast<i64>((h >> 50) % 10001u);
      const i64 dx = x_mm - cx, dz = z_mm - cz;
      const i64 d2 = dx * dx + dz * dz, r2 = r * r, inner2 = r2 * 49 / 100;
      if (d2 >= r2) continue;
      const i64 v = d2 <= inner2 ? k_obstacle_extra : k_obstacle_extra * (r2 - d2) / (r2 - inner2);
      most = std::max(most, v);
    }
  return most;
}

// **The lie of the land a way followed** at a scale no test scene's ground has: the grain of old
// fields, a wetter hollow, the line a herd took, as a smooth field 0 to 1000 over a 48 m lattice
// (the engine's hash at the corners, a smoothstep between them in whole numbers, so the valleys a
// way follows have no corner at a lattice line). A way at `irregularity` 1 pays up to three times
// its length again across its ridges, so it winds along its valleys with a wavelength of the
// lattice's order and never runs straight for long.
constexpr i64 k_meander_lattice_mm = 48000;
i64 meander_at(u64 seed, i64 x_mm, i64 z_mm) noexcept {
  const i64 l = k_meander_lattice_mm;
  const i64 ix = floor_div(x_mm, l), iz = floor_div(z_mm, l);
  const auto smooth = [](i64 w) { return w * w * (3000 - 2 * w) / 1000000; };  // 0..1000
  const i64 sx = smooth((x_mm - ix * l) * 1000 / l), sz = smooth((z_mm - iz * l) * 1000 / l);
  const auto node = [&](i64 a, i64 b) {
    return static_cast<i64>(
        mix64(hash_combine(hash_combine(seed ^ 0x3ea7d3ull, static_cast<u64>(a)),
                           static_cast<u64>(b))) %
        1001u);
  };
  const i64 top = node(ix, iz) * (1000 - sx) + node(ix + 1, iz) * sx;
  const i64 bottom = node(ix, iz + 1) * (1000 - sx) + node(ix + 1, iz + 1) * sx;
  return (top * (1000 - sz) + bottom * sz) / 1000000;
}

// A way's extra cost at a place, per mille: a quarter of the fine wander, what it went round and
// the lie of the land it followed, all scaled by `irregularity` (per mille).
i64 way_extra(u64 seed, i64 irregularity, i64 x_mm, i64 z_mm) noexcept {
  if (irregularity == 0) return 0;
  return irregularity *
         (wander_at(seed, x_mm, z_mm) / 4 + obstacle_at(seed, x_mm, z_mm) +
          meander_at(seed, x_mm, z_mm) * 3) /
         1000;
}

// The same along a way, for its edges: 0 to 1000 at arc length `s_mm`, on a 16 m lattice.
i64 edge_wander(u64 seed, i64 s_mm) noexcept {
  const i64 l = k_edge_lattice_mm;
  const i64 i = floor_div(s_mm, l), f = s_mm - i * l;
  const i64 a = static_cast<i64>(mix64(hash_combine(seed, static_cast<u64>(i))) % 1001u);
  const i64 b = static_cast<i64>(mix64(hash_combine(seed, static_cast<u64>(i + 1))) % 1001u);
  return (a * (l - f) + b * f) / l;
}

struct HeapItem {
  u64 cost = 0;
  u32 cell = 0;
};
// A min-heap on (cost, cell): ties go to the lower cell, so the search is the same everywhere.
bool heap_after(const HeapItem& a, const HeapItem& b) noexcept {
  return a.cost != b.cost ? a.cost > b.cost : a.cell > b.cell;
}

// **A way between two places found over the ground** (village.md, "The ways"): the least-cost
// path over a grid of `cell_mm` cells in a corridor round the two, each step to one of sixteen
// neighbours (the eight and the knight's moves, so a way runs at any of sixteen headings before it
// is smoothed) costing its length times the grade's factor and the wander. Integer costs: the same
// path on every toolchain. The points, x and z pairs, run from `a` to `b` exactly.
bool find_way(const scene_gen::Ground& ground, i64 ax, i64 az, i64 bx, i64 bz, i64 cell_mm,
              i64 irregularity, u64 seed, Vector<i64>& out, std::string& error) {
  const i64 apart = static_cast<i64>(direction(ax, az, bx, bz).length_mm);
  const i64 margin = std::clamp<i64>(apart / 4, k_corridor_min_mm, k_corridor_max_mm);
  const i64 x0 = std::min(ax, bx) - margin, z0 = std::min(az, bz) - margin;
  const i64 nx = (std::max(ax, bx) + margin - x0) / cell_mm + 1;
  const i64 nz = (std::max(az, bz) + margin - z0) / cell_mm + 1;
  const u32 cells = static_cast<u32>(nx * nz);
  constexpr i64 k_none = std::numeric_limits<i64>::min();
  Vector<i64> height, extra;
  height.resize(cells);
  extra.resize(cells);
  for (i64 j = 0; j < nz; ++j)
    for (i64 i = 0; i < nx; ++i) {
      const f64 h = ground.surface_mm(x0 + i * cell_mm, z0 + j * cell_mm);
      height[static_cast<u32>(j * nx + i)] = std::isfinite(h) ? mm_of(h) : k_none;
      extra[static_cast<u32>(j * nx + i)] =
          way_extra(seed, irregularity, x0 + i * cell_mm, z0 + j * cell_mm);
    }
  const auto cell_of = [&](i64 x, i64 z) {
    const i64 i = std::clamp<i64>(div_round(x - x0, cell_mm), 0, nx - 1);
    const i64 j = std::clamp<i64>(div_round(z - z0, cell_mm), 0, nz - 1);
    return static_cast<u32>(j * nx + i);
  };
  const u32 start = cell_of(ax, az), goal = cell_of(bx, bz);
  struct Step {
    i64 di, dj, length;
  };
  Step steps[16];
  {
    const i64 offsets[16][2] = {{1, 0},  {-1, 0},  {0, 1},   {0, -1}, {1, 1},  {1, -1},
                                {-1, 1}, {-1, -1}, {1, 2},   {2, 1},  {-1, 2}, {-2, 1},
                                {1, -2}, {2, -1},  {-1, -2}, {-2, -1}};
    for (u32 k = 0; k < 16; ++k) {
      const f64 d = std::sqrt(
          static_cast<f64>(offsets[k][0] * offsets[k][0] + offsets[k][1] * offsets[k][1]));
      steps[k] = Step{offsets[k][0], offsets[k][1], mm_of(d * static_cast<f64>(cell_mm) / 1000.0)};
    }
  }
  constexpr u64 k_unreached = std::numeric_limits<u64>::max();
  Vector<u64> best;
  best.resize(cells);
  Vector<u32> from;
  from.resize(cells);
  for (u32 k = 0; k < cells; ++k) {
    best[k] = k_unreached;
    from[k] = k;
  }
  Vector<HeapItem> heap;
  if (height[start] == k_none || height[goal] == k_none)
    return refuse(error, "village: a way's end stands off the ground");
  best[start] = 0;
  heap.push_back(HeapItem{0, start});
  while (!heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), heap_after);
    const HeapItem at = heap.back();
    heap.pop_back();
    if (at.cost != best[at.cell]) continue;
    if (at.cell == goal) break;
    const i64 i = static_cast<i64>(at.cell) % nx, j = static_cast<i64>(at.cell) / nx;
    for (const Step& st : steps) {
      const i64 ni = i + st.di, nj = j + st.dj;
      if (ni < 0 || nj < 0 || ni >= nx || nj >= nz) continue;
      const u32 next = static_cast<u32>(nj * nx + ni);
      if (height[next] == k_none) continue;
      const i64 rise = height[next] - height[at.cell];
      const i64 grade = (rise < 0 ? -rise : rise) * 1000 / st.length;  // per mille
      i64 factor = 1000 + grade * grade * 1000 / (k_grade_knee * k_grade_knee);
      if (grade > k_grade_wall) factor *= 10;
      factor += extra[next];
      const u64 cost = at.cost + static_cast<u64>(st.length * factor / 1000);
      if (cost < best[next]) {
        best[next] = cost;
        from[next] = at.cell;
        heap.push_back(HeapItem{cost, next});
        std::push_heap(heap.begin(), heap.end(), heap_after);
      }
    }
  }
  if (best[goal] == k_unreached)
    return refuse(error, "village: no way over the ground joins two places of a lane");
  Vector<u32> cells_back;
  for (u32 c = goal; c != start; c = from[c])
    cells_back.push_back(c);
  // The path as points with the cost to each, the two ends the places themselves.
  Vector<i64> px, pz;
  Vector<u64> pc;
  px.push_back(ax);
  pz.push_back(az);
  pc.push_back(0);
  for (u32 k = cells_back.size(); k-- > 1;) {
    const u32 c = cells_back[k];
    px.push_back(x0 + static_cast<i64>(c % static_cast<u32>(nx)) * cell_mm);
    pz.push_back(z0 + static_cast<i64>(c / static_cast<u32>(nx)) * cell_mm);
    pc.push_back(best[c]);
  }
  px.push_back(bx);
  pz.push_back(bz);
  pc.push_back(best[goal]);
  // **Pulled straight** wherever a straight stretch costs no more than the grid's steps between
  // its ends (by the same cost, sampled every cell along it): the grid's sixteen headings give way
  // to any heading, so a way on level ground with no wander is the straight line between its
  // places, and a wandering way keeps only the bends its cost asked for.
  const auto straight = [&](u32 i, u32 j) {
    const Dir d = direction(px[i], pz[i], px[j], pz[j]);
    const i64 n = std::max<i64>(1, static_cast<i64>(d.length_mm) / cell_mm + 1);
    u64 cost = 0;
    // (The ground's own heights along the stretch, and the extra of the search's nearest cell, so
    // the hash is not asked again.)
    i64 lx = px[i], lz = pz[i];
    f64 lh = ground.surface_mm(lx, lz);
    for (i64 q = 1; q <= n; ++q) {
      const i64 x = px[i] + div_round((px[j] - px[i]) * q, n);
      const i64 z = pz[i] + div_round((pz[j] - pz[i]) * q, n);
      const u32 c = cell_of(x, z);
      const f64 h = ground.surface_mm(x, z);
      if (!std::isfinite(h) || !std::isfinite(lh)) return std::numeric_limits<u64>::max();
      const i64 length = std::max<i64>(1, static_cast<i64>(direction(lx, lz, x, z).length_mm));
      const i64 rise = mm_of(h) - mm_of(lh);
      const i64 grade = (rise < 0 ? -rise : rise) * 1000 / length;
      i64 factor = 1000 + grade * grade * 1000 / (k_grade_knee * k_grade_knee);
      if (grade > k_grade_wall) factor *= 10;
      factor += extra[c];
      cost += static_cast<u64>(length * factor / 1000);
      lx = x;
      lz = z;
      lh = h;
    }
    return cost;
  };
  out.clear();
  u32 i = 0;
  const u32 last = px.size() - 1;
  out.push_back(px[0]);
  out.push_back(pz[0]);
  // A laid-out way (`irregularity` 0) is pulled as straight as its cost allows, the whole rest
  // first (on level ground it is one stretch), then at most 64 cells ahead, so the pull costs the
  // path's length and not its square. A grown way is pulled over no more than ten cells, 20 m at
  // the default station: the grid's zigzag goes, the bends its cost asked for stay, and the
  // smoothing after makes them curves.
  const u32 window = irregularity > 0 ? 10u : 64u;
  while (i < last) {
    u32 reach = i + 1;
    if (irregularity == 0 && last > i + 64 && straight(i, last) <= pc[last] - pc[i]) reach = last;
    for (u32 j = i + 2; j <= last && j <= i + window && reach < last; ++j) {
      if (straight(i, j) <= pc[j] - pc[i])
        reach = j;
      else if (j > reach + 8)
        break;  // a stretch this much past the last straight one is not
    }
    out.push_back(px[reach]);
    out.push_back(pz[reach]);
    i = reach;
  }
  return true;
}

// A polyline's points (x, z pairs) twice through a five-point mean, its ends kept: a grid's steps
// become a way's bends.
void smooth(Vector<i64>& pts) {
  const i64 n = static_cast<i64>(pts.size() / 2);
  if (n < 3) return;
  Vector<i64> was;
  for (u32 pass = 0; pass < 2; ++pass) {
    was = pts;
    for (i64 k = 1; k + 1 < n; ++k)
      for (i64 axis = 0; axis < 2; ++axis) {
        i64 sum = 0;
        for (i64 q = -2; q <= 2; ++q)
          sum += was[static_cast<u32>(std::clamp<i64>(k + q, 0, n - 1) * 2 + axis)];
        pts[static_cast<u32>(k * 2 + axis)] = div_round(sum, 5);
      }
  }
}

// Stations at equal steps no longer than `station_mm` along a polyline, both ends kept; and the
// line's length.
void station(const Vector<i64>& pts, i64 station_mm, LaneLine& line) {
  const u32 n = pts.size() / 2;
  Vector<f64> arc;
  arc.push_back(0.0);
  for (u32 k = 1; k < n; ++k)
    arc.push_back(arc[k - 1] +
                  direction(pts[2 * k - 2], pts[2 * k - 1], pts[2 * k], pts[2 * k + 1]).length_mm);
  const f64 total = arc[n - 1];
  line.length_mm = mm_of(total / 1000.0);
  const i64 steps = std::max<i64>(1, (line.length_mm + station_mm - 1) / station_mm);
  u32 seg = 0;
  for (i64 q = 0; q <= steps; ++q) {
    const f64 at = total * static_cast<f64>(q) / static_cast<f64>(steps);
    while (seg + 2 < n && arc[seg + 1] < at)
      ++seg;
    const f64 span = arc[seg + 1] - arc[seg];
    const f64 t = span > 0.0 ? std::clamp((at - arc[seg]) / span, 0.0, 1.0) : 0.0;
    LaneStation s;
    s.x_mm = pts[2 * seg] + mm_of(t * static_cast<f64>(pts[2 * seg + 2] - pts[2 * seg]) / 1000.0);
    s.z_mm = pts[2 * seg + 1] +
             mm_of(t * static_cast<f64>(pts[2 * seg + 3] - pts[2 * seg + 1]) / 1000.0);
    line.stations.push_back(s);
  }
}

// The tread's top at each station: the ground there through a five-point mean, so a hummock
// under one station does not put a step in the tread.
bool tops(const scene_gen::Ground& ground, LaneLine& line) {
  Vector<i64> raw;
  for (const LaneStation& s : line.stations) {
    const f64 h = ground.surface_mm(s.x_mm, s.z_mm);
    if (!std::isfinite(h)) return false;
    raw.push_back(mm_of(h));
  }
  const i64 count = static_cast<i64>(raw.size());
  for (i64 k = 0; k < count; ++k) {
    i64 sum = 0;
    for (i64 q = -2; q <= 2; ++q)
      sum += raw[static_cast<u32>(std::clamp<i64>(k + q, 0, count - 1))];
    line.stations[static_cast<u32>(k)].top_mm = div_round(sum, 5);
  }
  return true;
}

// The arc length of each station from the way's first.
void arcs(const LaneLine& line, Vector<i64>& out) {
  out.clear();
  i64 s = 0;
  for (u32 k = 0; k < line.stations.size(); ++k) {
    if (k > 0)
      s += static_cast<i64>(direction(line.stations[k - 1].x_mm, line.stations[k - 1].z_mm,
                                      line.stations[k].x_mm, line.stations[k].z_mm)
                                .length_mm);
    out.push_back(s);
  }
}

// The station nearest a point, and the square of its distance.
u32 nearest_station(const LaneLine& lane, i64 x, i64 z, f64* d2 = nullptr) noexcept {
  u32 best = 0;
  f64 best_d = 1e300;
  for (u32 k = 0; k < lane.stations.size(); ++k) {
    const f64 dx = static_cast<f64>(lane.stations[k].x_mm - x),
              dz = static_cast<f64>(lane.stations[k].z_mm - z);
    const f64 d = dx * dx + dz * dz;
    if (d < best_d) {
      best_d = d;
      best = k;
    }
  }
  if (d2) *d2 = best_d;
  return best;
}

// The way's direction at a station: from two stations before it to two after.
Dir tangent(const LaneLine& lane, u32 k) noexcept {
  const u32 n = lane.stations.size();
  const u32 a = k >= 2 ? k - 2 : 0, b = std::min(n - 1, k + 2);
  return direction(lane.stations[a].x_mm, lane.stations[a].z_mm, lane.stations[b].x_mm,
                   lane.stations[b].z_mm);
}

// The station at or before arc length `s` (`arc` the way's arcs).
u32 station_at(const Vector<i64>& arc, i64 s) noexcept {
  const auto it = std::upper_bound(arc.begin(), arc.end(), s);
  const u32 k = static_cast<u32>(it - arc.begin());
  return k > 0 ? k - 1 : 0;
}

// ---- plots -------------------------------------------------------------------------------------

// A plot not yet taken: where along which way and side, its kind, its sizes and its place in the
// order.
struct Candidate {
  u32 lane = 0;
  u8 side = 0;
  PlotKind kind = PlotKind::tenement;
  i64 s_mm = 0;  // its front's middle, along the way
  i64 frontage_mm = 0, depth_mm = 0;
  i64 edge_mm = -1;  // the street's edge it stands at, or -1 for the way's own there
  // A toft of a row: its front the chord of the street's edge between its corners, so it shares
  // its corners with its neighbours. Otherwise (the church, a cottage on the green) its front is
  // square to its way, at the furthest the edge stands along it, turned by the slope and the sun.
  bool chord = false;
  i64 push_mm = 0;  // a chord front moved this far into the plot (the church's, on a bend)
  u64 seed = 0;
  u64 key = 0;    // the order: the lower first
  u32 index = 0;  // its place in the list made, which settles a tie
};

// A plot's rectangle as four corners, for the overlap test.
struct Rect {
  f64 x[4] = {0, 0, 0, 0};
  f64 z[4] = {0, 0, 0, 0};
};
Rect rect_of(const Plot& p, f64 shrink) noexcept {
  const f64 hu = static_cast<f64>(p.frontage_mm) * 0.5 - shrink;
  const f64 v0 = shrink, v1 = static_cast<f64>(p.depth_mm) - shrink;
  const f64 us[4] = {-hu, hu, hu, -hu}, vs[4] = {v0, v0, v1, v1};
  Rect r;
  for (u32 c = 0; c < 4; ++c) {
    r.x[c] = static_cast<f64>(p.front_x_mm) + p.along_x * us[c] + p.inward_x * vs[c];
    r.z[c] = static_cast<f64>(p.front_z_mm) + p.along_z * us[c] + p.inward_z * vs[c];
  }
  return r;
}
// Whether two convex quadrilaterals overlap: the separating axes of their edges.
bool overlap(const Rect& a, const Rect& b) noexcept {
  const Rect* rs[2] = {&a, &b};
  for (u32 r = 0; r < 2; ++r)
    for (u32 e = 0; e < 4; ++e) {
      const Rect& q = *rs[r];
      const f64 ax = -(q.z[(e + 1) % 4] - q.z[e]), az = q.x[(e + 1) % 4] - q.x[e];
      f64 lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
      for (u32 w = 0; w < 2; ++w)
        for (u32 c = 0; c < 4; ++c) {
          const f64 d = rs[w]->x[c] * ax + rs[w]->z[c] * az;
          lo[w] = std::min(lo[w], d);
          hi[w] = std::max(hi[w], d);
        }
      if (hi[0] <= lo[1] || hi[1] <= lo[0]) return false;
    }
  return true;
}
// Whether a world point stands inside a plot, more than `margin_mm` in from its front and sides.
bool inside_plot(const Plot& p, f64 x, f64 z, f64 margin_mm) noexcept {
  const f64 dx = x - static_cast<f64>(p.front_x_mm), dz = z - static_cast<f64>(p.front_z_mm);
  const f64 u = dx * p.along_x + dz * p.along_z, v = dx * p.inward_x + dz * p.inward_z;
  return u > -static_cast<f64>(p.frontage_mm) * 0.5 + margin_mm &&
         u < static_cast<f64>(p.frontage_mm) * 0.5 - margin_mm && v > margin_mm &&
         v < static_cast<f64>(p.depth_mm) - margin_mm;
}

// How far apart two plots' pads stand, millimetres: 0 where they overlap, else the least distance
// from a corner of either to an edge of the other (two convex quadrilaterals).
f64 pad_gap(const Plot& a, const Plot& b) noexcept {
  const auto inside = [](const Plot& q, f64 x, f64 z) {
    bool left = false, right = false;
    for (u32 e = 0; e < 4; ++e) {
      const u32 n = (e + 1) % 4;
      const f64 ex = static_cast<f64>(q.pad_x_mm[n] - q.pad_x_mm[e]);
      const f64 ez = static_cast<f64>(q.pad_z_mm[n] - q.pad_z_mm[e]);
      const f64 c =
          ex * (z - static_cast<f64>(q.pad_z_mm[e])) - ez * (x - static_cast<f64>(q.pad_x_mm[e]));
      left = left || c > 0.0;
      right = right || c < 0.0;
    }
    return !(left && right);
  };
  const auto to_edges = [](const Plot& q, f64 x, f64 z) {
    f64 best = 1e300;
    for (u32 e = 0; e < 4; ++e) {
      const u32 n = (e + 1) % 4;
      const f64 ax = static_cast<f64>(q.pad_x_mm[e]), az = static_cast<f64>(q.pad_z_mm[e]);
      const f64 ex = static_cast<f64>(q.pad_x_mm[n]) - ax,
                ez = static_cast<f64>(q.pad_z_mm[n]) - az;
      const f64 ee = ex * ex + ez * ez;
      const f64 t = ee > 0.0 ? std::clamp(((x - ax) * ex + (z - az) * ez) / ee, 0.0, 1.0) : 0.0;
      const f64 dx = ax + t * ex - x, dz = az + t * ez - z;
      best = std::min(best, dx * dx + dz * dz);
    }
    return best;
  };
  f64 best = 1e300;
  const Plot* both[2] = {&a, &b};
  for (u32 w = 0; w < 2; ++w)
    for (u32 c = 0; c < 4; ++c) {
      const f64 x = static_cast<f64>(both[w]->pad_x_mm[c]),
                z = static_cast<f64>(both[w]->pad_z_mm[c]);
      const Plot& other = *both[1 - w];
      if (inside(other, x, z)) return 0.0;
      best = std::min(best, to_edges(other, x, z));
    }
  return std::sqrt(best);
}

}  // namespace

bool layer_in_range(const VillageLayer& p, std::string& error) {
  const auto bad = [&](const char* what) {
    return refuse(error, std::string("engine.village.VillageLayer: ") + what);
  };
  if (p.lanes.empty()) return bad("no lanes (a village grows along at least one way)");
  for (const Lane& lane : p.lanes) {
    if (lane.waypoints.size() < 2) return bad("a lane of fewer than two waypoints");
    for (const WorldPos& w : lane.waypoints)
      if (!std::isfinite(w.x) || !std::isfinite(w.z)) return bad("a waypoint that is not finite");
    for (u32 k = 0; k + 1 < lane.waypoints.size(); ++k)
      if (mm_of(lane.waypoints[k].x) == mm_of(lane.waypoints[k + 1].x) &&
          mm_of(lane.waypoints[k].z) == mm_of(lane.waypoints[k + 1].z))
        return bad("two waypoints of a lane at one place");
  }
  if (p.houses < 1 || p.houses > 256) return bad("houses out of range (1 to 256)");
  if (!in_range(static_cast<f64>(p.perch_m), 4.5, 6.1))
    return bad("perch_m out of range (4.5 to 6.1)");
  if (p.frontage_min_perches < 1 || p.frontage_max_perches > 12 ||
      p.frontage_min_perches > p.frontage_max_perches)
    return bad("frontage perches out of range (1 to 12, the least no more than the most)");
  if (p.depth_perches < 2 || p.depth_perches > 30)
    return bad("depth_perches out of range (2 to 30)");
  if (!in_range(static_cast<f64>(p.street_width_m), 2.0, 40.0))
    return bad("street_width_m out of range (2 to 40)");
  if (!in_range(static_cast<f64>(p.tread_width_m), 1.0, static_cast<f64>(p.street_width_m)))
    return bad("tread_width_m out of range (1 to the street's width)");
  if (!in_range(static_cast<f64>(p.setback_m), 0.0, 20.0))
    return bad("setback_m out of range (0 to 20)");
  if (!in_range(static_cast<f64>(p.station_m), 0.5, 8.0))
    return bad("station_m out of range (0.5 to 8)");
  if (!in_range(static_cast<f64>(p.pad_margin_m), 0.0, 4.0))
    return bad("pad_margin_m out of range (0 to 4)");
  if (!in_range(static_cast<f64>(p.max_pad_step_m), 0.1, 10.0))
    return bad("max_pad_step_m out of range (0.1 to 10)");
  if (!in_range(static_cast<f64>(p.path_width_m), 0.0, 3.0))
    return bad("path_width_m out of range (0 to 3)");
  if (!in_range(static_cast<f64>(p.yard_m), 0.0, 6.0)) return bad("yard_m out of range (0 to 6)");
  if (!in_range(static_cast<f64>(p.bank_slope), 0.25, 8.0) ||
      !in_range(static_cast<f64>(p.fill_slope), 0.1, 8.0))
    return bad("bank_slope (0.25 to 8) or fill_slope (0.1 to 8) out of range");
  if (!in_range(static_cast<f64>(p.face_m), 0.1, 10.0))
    return bad("face_m out of range (0.1 to 10)");
  if (p.storeys < 1 || p.storeys > 2) return bad("storeys out of range (1 or 2)");
  if (!in_range(static_cast<f64>(p.irregularity), 0.0, 1.0))
    return bad("irregularity out of range (0 to 1)");
  if (!(p.green_m == 0.0f) &&
      !in_range(static_cast<f64>(p.green_m), static_cast<f64>(p.street_width_m), 80.0))
    return bad("green_m out of range (0, or the street's width to 80)");
  return true;
}

bool plan_village(const VillageLayer& layer, const scene_gen::Ground& ground,
                  const scene_gen::Context& context, HouseFn house_fn, void* house_context,
                  VillagePlan& out, std::string& error) {
  out = {};
  if (!layer_in_range(layer, error)) return false;
  if (house_fn == nullptr) return refuse(error, "village: no house filler was handed to the plan");
  const i64 perch = mm_of(static_cast<f64>(layer.perch_m));
  const i64 depth = perch * static_cast<i64>(layer.depth_perches);
  const i64 half_street = mm_of(static_cast<f64>(layer.street_width_m) * 0.5);
  const i64 half_tread = mm_of(static_cast<f64>(layer.tread_width_m) * 0.5);
  const i64 station_mm = mm_of(static_cast<f64>(layer.station_m));
  const i64 setback = mm_of(static_cast<f64>(layer.setback_m));
  const i64 margin = mm_of(static_cast<f64>(layer.pad_margin_m));
  const i64 max_step = mm_of(static_cast<f64>(layer.max_pad_step_m));
  const i64 irr = mm_of(static_cast<f64>(layer.irregularity));  // per mille
  const i64 green = mm_of(static_cast<f64>(layer.green_m));
  const u32 spread = layer.frontage_max_perches - layer.frontage_min_perches + 1;
  const u32 ways = layer.lanes.size();

  // **The ways** (village.md, "The ways"): between each two places a lane passes, the least-cost
  // path over the ground, the pieces joined, smoothed and stationed.
  // **A way joins the ways before it**: an end within 40 m of an earlier way's line is moved onto
  // its nearest station, so ways meet rather than stop short; the first such meeting is the focus.
  constexpr i64 k_join_mm = 40000;
  bool joined = false;
  u32 focus_lane = 0;
  for (u32 li = 0; li < ways; ++li) {
    const Lane& lane = layer.lanes[li];
    Vector<i64> wx, wz;
    for (const WorldPos& w : lane.waypoints) {
      wx.push_back(mm_of(w.x));
      wz.push_back(mm_of(w.z));
    }
    for (u32 end = 0; end < 2; ++end) {
      const u32 at = end == 0 ? 0 : wx.size() - 1;
      f64 nearest = static_cast<f64>(k_join_mm) * static_cast<f64>(k_join_mm);
      for (u32 lj = 0; lj < li; ++lj) {
        f64 d2 = 0.0;
        const u32 k = nearest_station(out.lanes[lj], wx[at], wz[at], &d2);
        if (d2 <= nearest) {
          nearest = d2;
          wx[at] = out.lanes[lj].stations[k].x_mm;
          wz[at] = out.lanes[lj].stations[k].z_mm;
          if (!joined) {
            out.focus_x_mm = wx[at];
            out.focus_z_mm = wz[at];
            focus_lane = lj;
          }
        }
      }
      if (nearest < static_cast<f64>(k_join_mm) * static_cast<f64>(k_join_mm)) joined = true;
    }
    Vector<i64> pts, piece;
    for (u32 k = 0; k + 1 < wx.size(); ++k) {
      if (wx[k] == wx[k + 1] && wz[k] == wz[k + 1]) continue;
      if (!find_way(ground, wx[k], wz[k], wx[k + 1], wz[k + 1], station_mm, irr,
                    hash_combine(layer.seed, 0x77616e646572ull), piece, error))
        return refuse(error, error + " (" + lane_name(layer, nullptr, li) + ")");
      for (u32 q = pts.empty() ? 0 : 2; q < piece.size(); ++q)
        pts.push_back(piece[q]);
    }
    if (pts.size() < 4)
      return refuse(error, "village: " + lane_name(layer, nullptr, li) + " joins itself");
    // Stationed densely, smoothed, and stationed again: the pulled path's corners become bends.
    LaneLine dense;
    station(pts, station_mm, dense);
    pts.clear();
    for (const LaneStation& s : dense.stations) {
      pts.push_back(s.x_mm);
      pts.push_back(s.z_mm);
    }
    smooth(pts);
    LaneLine line;
    station(pts, station_mm, line);
    line.half_tread_mm = half_tread;
    line.from_lane = li;
    if (!tops(ground, line))
      return refuse(
          error, "village: the ground under " + lane_name(layer, nullptr, li) + " is not finite");
    out.lanes.push_back(std::move(line));
  }
  Vector<Vector<i64>> arc;
  for (u32 li = 0; li < ways; ++li) {
    Vector<i64> a;
    arcs(out.lanes[li], a);
    arc.push_back(std::move(a));
  }

  // **The focus**: the first meeting of a way with one before it (above), or the first way's
  // middle. The green opens there and the well stands at it, and the plots are taken nearest it
  // first.
  {
    if (!joined) {
      const LaneLine& first = out.lanes[0];
      const LaneStation& mid = first.stations[station_at(arc[0], first.length_mm / 2)];
      out.focus_x_mm = mid.x_mm;
      out.focus_z_mm = mid.z_mm;
    }
    const f64 h = ground.surface_mm(out.focus_x_mm, out.focus_z_mm);
    out.focus_top_mm = std::isfinite(h) ? mm_of(h) : 0;
  }

  // **The street's edges**: the entry's half width, wandering by up to a quarter of the street's
  // width with `irregularity`, and opening into the green — a lens three times as long as it is
  // wide, each side's edge a parabola — on every way that passes within the green's half width of
  // the focus.
  const i64 green_half_length = green;
  for (u32 li = 0; li < ways; ++li) {
    LaneLine& line = out.lanes[li];
    f64 d2 = 0.0;
    const u32 k_green = nearest_station(line, out.focus_x_mm, out.focus_z_mm, &d2);
    const bool greened =
        green > 2 * half_street && d2 <= static_cast<f64>(std::max<i64>(green / 2, 10000)) *
                                             static_cast<f64>(std::max<i64>(green / 2, 10000));
    const i64 s_green = arc[li][k_green];
    for (u32 k = 0; k < line.stations.size(); ++k) {
      const i64 s = arc[li][k];
      i64 open = 0;
      if (greened && s > s_green - green_half_length && s < s_green + green_half_length) {
        // Its funnel mouths (the grounding's "The green"): the street opens over the outer third
        // at each end, a smoothstep, and runs at the green's breadth along its middle third, so
        // the tofts along each side stand in a row facing across it rather than fanning round a
        // curve.
        const i64 t = (s - s_green) * 1000 / green_half_length;  // -1000 to 1000
        const i64 u = std::min<i64>(1000, (1000 - (t < 0 ? -t : t)) * 3);
        open = (green / 2 - half_street) * (u * u * (3000 - 2 * u) / 1000000) / 1000;
      }
      for (u32 side = 0; side < 2; ++side) {
        const i64 w = edge_wander(hash_combine(hash_combine(layer.seed, li), side + 0xed6eull), s);
        const i64 e = half_street + open + irr * half_street / 2 * (2 * w - 1000) / 1000000;
        (side == 0 ? line.stations[k].left_mm : line.stations[k].right_mm) =
            std::max(e, half_tread + 300);
      }
      // The tread: its own wander, a quarter of its width each way at `irregularity` 1 over the
      // edges' lattice, and up to half again as wide where the ways meet, where every cart turns.
      const i64 wt = edge_wander(hash_combine(hash_combine(layer.seed, li), 0x7ead), s);
      i64 tread = half_tread + irr * half_tread / 2 * (2 * wt - 1000) / 1000000;
      if (open > 0 && green > 2 * half_street)
        tread += half_tread / 2 * open / (green / 2 - half_street);
      // (At most 1.9 m each side: a bench line wider than about 4.4 m is not level at its middle in
      // the tile set, 41 mm low at 4.5 to 4.8 m, which the benches' case caught; tileset.md.)
      const i64 room =
          std::min({line.stations[k].left_mm - 300, line.stations[k].right_mm - 300, i64{1900}});
      line.stations[k].tread_mm = std::clamp<i64>(tread, std::min<i64>(500, room), room);
    }
  }
  // The green's outline: the way through the focus, its two edges where it opens.
  if (green > 2 * half_street) {
    const LaneLine& line = out.lanes[focus_lane];
    const i64 s_green = arc[focus_lane][nearest_station(line, out.focus_x_mm, out.focus_z_mm)];
    Vector<i64> rx, rz;
    for (u32 k = 0; k < line.stations.size(); ++k) {
      const i64 s = arc[focus_lane][k];
      if (s <= s_green - green_half_length || s >= s_green + green_half_length) continue;
      const LaneStation& st = line.stations[k];
      const Dir t = tangent(line, k);
      out.green.push_back(st.x_mm - mm_of(t.z * static_cast<f64>(st.left_mm) / 1000.0));
      out.green.push_back(st.z_mm + mm_of(t.x * static_cast<f64>(st.left_mm) / 1000.0));
      rx.push_back(st.x_mm + mm_of(t.z * static_cast<f64>(st.right_mm) / 1000.0));
      rz.push_back(st.z_mm - mm_of(t.x * static_cast<f64>(st.right_mm) / 1000.0));
    }
    for (u32 k = rx.size(); k-- > 0;) {
      out.green.push_back(rx[k]);
      out.green.push_back(rz[k]);
    }
  }

  // **The candidates** (village.md, "The tofts"): along each way and side, out
  // from the station nearest the focus both ways, founding tenements of whole perches stretched by
  // `irregularity`, with a seeded gap after each; a cottage in each gap wide enough for one; and,
  // on a way through the green, cottages standing at the street's own edge on the green's.
  Vector<Candidate> cands;
  const auto add = [&](Candidate c) {
    c.index = cands.size();
    cands.push_back(c);
  };
  for (u32 li = 0; li < ways; ++li) {
    const Lane& lane = layer.lanes[li];
    const LaneLine& line = out.lanes[li];
    const i64 length = line.length_mm;
    const i64 s0 = arc[li][nearest_station(line, out.focus_x_mm, out.focus_z_mm)];
    for (u8 side = 0; side < 2; ++side) {
      if ((side == 0 && lane.rows == Rows::Right) || (side == 1 && lane.rows == Rows::Left))
        continue;
      for (i32 dir = 1; dir >= -1; dir -= 2) {
        i64 cursor = 0;
        for (u32 n = 0; n < 4096; ++n) {
          const u64 seed =
              hash_combine(hash_combine(hash_combine(hash_combine(layer.seed, li), side),
                                        static_cast<u64>(dir + 1)),
                           n);
          const i64 whole = perch * static_cast<i64>(layer.frontage_min_perches +
                                                     static_cast<u32>(mix64(seed) % spread));
          const i64 frontage =
              div_round(whole + div_round(whole * irr * (draw(seed, 1) - 500), 2000000), 10) * 10;
          // A gap after it is the exception in a grown row (one tenement in seven at
          // `irregularity` 1), one to two perches, where a cottage comes later; none at 0.
          const i64 gap = draw(seed, 7) < irr * 15 / 100 ? perch + perch * draw(seed, 2) / 1000 : 0;
          const i64 deep = depth + div_round(depth * irr * 6 * (draw(seed, 3) - 500), 10000000);
          const i64 lo = dir > 0 ? s0 + cursor : s0 - cursor - frontage;
          if (lo < 0 || lo + frontage > length) break;
          // **Subdivided** (village-growth-grounding.md, "Plots"): a tenement split between two
          // households, lengthwise, unevenly (35 to 65 %), in six of ten at `irregularity` 1, each
          // toft at least 1.1 perches on the street. The two keep the croft's depth.
          const i64 least = perch * 11 / 10;
          i64 parts[2] = {frontage, 0};
          if (draw(seed, 8) < irr * 6 / 10 && frontage >= 2 * least) {
            const i64 share = 350 + draw(seed, 9) * 3 / 10;  // per mille
            parts[0] =
                std::clamp<i64>(div_round(frontage * share, 10000) * 10, least, frontage - least);
            parts[1] = frontage - parts[0];
          }
          i64 at = cursor;
          for (u32 q = 0; q < 2 && parts[q] > 0; ++q) {
            Candidate c;
            c.lane = li;
            c.side = side;
            c.chord = true;
            c.s_mm = dir > 0 ? s0 + at + parts[q] / 2 : s0 - at - parts[q] + parts[q] / 2;
            c.frontage_mm = parts[q];
            c.depth_mm = deep;
            c.seed = parts[1] == 0 ? seed : hash_combine(seed, 0x5b1u + q);
            add(c);
            at += parts[q];
          }
          // A cottage in the gap after it, where the gap holds one.
          const i64 cottage = std::min(gap - 400, perch * 3 / 2);
          if (gap > 0 && cottage >= perch * 6 / 5) {
            Candidate q;
            q.lane = li;
            q.side = side;
            q.chord = true;
            q.kind = PlotKind::cottage;
            q.s_mm = s0 + dir * (cursor + frontage + gap / 2);
            q.frontage_mm = cottage;
            q.depth_mm = deep / 2;
            q.seed = hash_combine(seed, 0xc0u);
            if (q.s_mm - cottage / 2 >= 0 && q.s_mm + cottage / 2 <= length) add(q);
          }
          cursor += frontage + gap;
        }
      }
      // Onto the green: at the street's own edge, in front of the green's tenements.
      if (green > 2 * half_street && irr > 0) {
        const i64 step = perch * 3 / 2 + perch / 2;
        for (i64 s = s0 - green_half_length; s <= s0 + green_half_length; s += step) {
          if (s < 0 || s > length) continue;
          const u32 k = station_at(arc[li], s);
          const LaneStation& st = line.stations[k];
          const i64 edge = side == 0 ? st.left_mm : st.right_mm;
          const i64 own = half_street;
          const i64 room = edge - own - 1000;
          const u64 seed = hash_combine(hash_combine(hash_combine(layer.seed, li), side),
                                        static_cast<u64>(s) ^ 0x6772ull);
          if (room < 8000 || draw(seed, 4) >= irr / 2) continue;
          Candidate q;
          q.lane = li;
          q.side = side;
          q.kind = PlotKind::cottage;
          q.s_mm = s;
          q.frontage_mm = perch * 3 / 2;
          q.depth_mm = std::min(depth / 2, room);
          q.edge_mm = own;
          q.seed = seed;
          add(q);
        }
      }
    }
  }

  // The plot a candidate would be: its frame on its way, at its edge, turned (village.md, "The
  // tofts").
  // A point of a way's street edge, on a side, at arc length `s`, to the millimetre: the line there
  // and the edge's distance from it both between the two stations about it, out along the normal
  // of the tangent between theirs. The same `s` is the same point, so two tofts meeting at it
  // share their corner.
  const auto edge_point = [&](u32 li, i64 s, u8 side, i64& x, i64& z) {
    const LaneLine& line = out.lanes[li];
    const Vector<i64>& a = arc[li];
    s = std::clamp<i64>(s, 0, a.back());
    const u32 k = station_at(a, s);
    const u32 k1 = std::min<u32>(k + 1, line.stations.size() - 1);
    const LaneStation& st = line.stations[k];
    const LaneStation& sn = line.stations[k1];
    const i64 span = a[k1] - a[k], f = s - a[k];
    const auto lerp = [&](i64 u, i64 v) {
      return span > 0 && f > 0 ? u + div_round((v - u) * f, span) : u;
    };
    const i64 lx = lerp(st.x_mm, sn.x_mm), lz = lerp(st.z_mm, sn.z_mm);
    const i64 e = side == 0 ? lerp(st.left_mm, sn.left_mm) : lerp(st.right_mm, sn.right_mm);
    const Dir t0 = tangent(line, k);
    f64 tx = t0.x, tz = t0.z;
    if (span > 0 && f > 0) {
      const Dir t1 = tangent(line, k1);
      const f64 w1 = static_cast<f64>(f) / static_cast<f64>(span);
      tx = t0.x * (1.0 - w1) + t1.x * w1;
      tz = t0.z * (1.0 - w1) + t1.z * w1;
      const f64 tl = std::sqrt(tx * tx + tz * tz);
      tx /= tl;
      tz /= tl;
    }
    const f64 nx = side == 0 ? -tz : tz, nz = side == 0 ? tx : -tx;
    x = lx + mm_of(nx * static_cast<f64>(e) / 1000.0);
    z = lz + mm_of(nz * static_cast<f64>(e) / 1000.0);
  };
  const auto frame = [&](const Candidate& c, Plot& p, bool square = false) {
    if (c.chord) {
      // A toft of a row: its front the chord of the street's edge between its corners, the
      // corner at u = -half first (the lower `s` on the left, the higher on the right).
      const i64 lo = c.s_mm - c.frontage_mm / 2, hi = lo + c.frontage_mm;
      i64 ax = 0, az = 0, bx = 0, bz = 0;
      edge_point(c.lane, c.side == 0 ? lo : hi, c.side, ax, az);
      edge_point(c.lane, c.side == 0 ? hi : lo, c.side, bx, bz);
      const Dir d = direction(ax, az, bx, bz);
      p = {};
      p.lane = c.lane;
      p.side = c.side;
      p.kind = c.kind;
      p.seed = c.seed;
      // (The churchyard keeps its own 42 m; a toft is as wide as its chord.)
      p.frontage_mm = c.kind == PlotKind::church ? c.frontage_mm : mm_of(d.length_mm / 1000.0);
      p.depth_mm = c.depth_mm;
      p.along_mm = c.s_mm;
      p.along_x = d.x;
      p.along_z = d.z;
      p.inward_x = -d.z;
      p.inward_z = d.x;
      p.front_x_mm =
          div_round(ax + bx, 2) + mm_of(p.inward_x * static_cast<f64>(c.push_mm) / 1000.0);
      p.front_z_mm =
          div_round(az + bz, 2) + mm_of(p.inward_z * static_cast<f64>(c.push_mm) / 1000.0);
      return;
    }
    const LaneLine& line = out.lanes[c.lane];
    const u32 k = station_at(arc[c.lane], c.s_mm);
    const LaneStation& st = line.stations[k];
    const Dir t = tangent(line, k);
    // The point of the line at s, between station k and the next.
    i64 lx = st.x_mm, lz = st.z_mm;
    if (k + 1 < line.stations.size()) {
      const i64 span = arc[c.lane][k + 1] - arc[c.lane][k];
      if (span > 0) {
        lx += div_round((line.stations[k + 1].x_mm - st.x_mm) * (c.s_mm - arc[c.lane][k]), span);
        lz += div_round((line.stations[k + 1].z_mm - st.z_mm) * (c.s_mm - arc[c.lane][k]), span);
      }
    }
    const f64 nx = c.side == 0 ? -t.z : t.z, nz = c.side == 0 ? t.x : -t.x;
    f64 ax = c.side == 0 ? t.x : -t.x, az = c.side == 0 ? t.z : -t.z;
    // The street's edge: the furthest out it stands along the plot's frontage, so a plot by the
    // green's widening stands behind its outline and not in it.
    i64 edge = c.edge_mm;
    if (edge < 0) {
      const u32 k0 = station_at(arc[c.lane], c.s_mm - c.frontage_mm / 2);
      const u32 k1 = station_at(arc[c.lane], c.s_mm + c.frontage_mm / 2);
      for (u32 q = k0; q <= std::min<u32>(k1 + 1, line.stations.size() - 1); ++q)
        edge = std::max(edge, c.side == 0 ? line.stations[q].left_mm : line.stations[q].right_mm);
    }
    // The turn: toward the contour where the ground falls, toward a long axis east and west where
    // it does not, and a seeded part; at most tan 20 degrees, all of it scaled by `irregularity`.
    f64 turn = 0.0;
    if (irr > 0 && c.kind != PlotKind::church && !square) {
      const f64 mx = static_cast<f64>(lx) + nx * static_cast<f64>(edge + c.depth_mm / 2);
      const f64 mz = static_cast<f64>(lz) + nz * static_cast<f64>(edge + c.depth_mm / 2);
      const i64 cx = mm_of(mx / 1000.0), cz = mm_of(mz / 1000.0);
      const f64 gx = (ground.surface_mm(cx + 2000, cz) - ground.surface_mm(cx - 2000, cz)) / 4.0;
      const f64 gz = (ground.surface_mm(cx, cz + 2000) - ground.surface_mm(cx, cz - 2000)) / 4.0;
      f64 want = 0.0, weight_slope = 0.0;
      if (std::isfinite(gx) && std::isfinite(gz)) {
        const f64 grade = std::sqrt(gx * gx + gz * gz);
        if (grade > 0.005) {
          // The contour, the long axis's side of it.
          f64 kx = -gz / grade, kz = gx / grade;
          if (kx * ax + kz * az < 0.0) {
            kx = -kx;
            kz = -kz;
          }
          const f64 q = kx * ax + kz * az;
          if (q > 0.2) want = (kx * nx + kz * nz) / q;
          weight_slope = std::min(1.0, grade / 0.06);
        }
      }
      // East and west, the long axis's side.
      const f64 ex = ax >= 0.0 ? 1.0 : -1.0;
      f64 sun = 0.0;
      if (std::fabs(ax) > 0.3) sun = (ex * nx) / (ex * ax);
      // The seeded part wanders along the way (a 16 m lattice), so neighbours lean alike and a
      // row fans rather than crossing itself.
      const i64 lean_draw = edge_wander(
          hash_combine(hash_combine(hash_combine(layer.seed, c.lane), c.side), 0x7a11ull),
          c.s_mm / 3);
      const f64 jitter = static_cast<f64>(lean_draw - 500) / 500.0 * 0.15;
      turn = weight_slope * std::clamp(want, -1.0, 1.0) +
             (1.0 - weight_slope) * 0.5 * std::clamp(sun, -1.0, 1.0) + jitter;
      const f64 most = k_max_turn * static_cast<f64>(irr) / 1000.0;
      turn = std::clamp(turn * static_cast<f64>(irr) / 1000.0, -most, most);
    }
    const f64 bx = ax + turn * nx, bz = az + turn * nz;
    const f64 bl = std::sqrt(bx * bx + bz * bz);
    ax = bx / bl;
    az = bz / bl;
    p = {};
    p.lane = c.lane;
    p.side = c.side;
    p.kind = c.kind;
    p.seed = c.seed;
    p.frontage_mm = c.frontage_mm;
    p.depth_mm = c.depth_mm;
    p.along_mm = c.s_mm;
    p.along_x = ax;
    p.along_z = az;
    p.inward_x = -az;
    p.inward_z = ax;
    // The front's middle far enough out that its near corner stands on the street's edge.
    const f64 lean = std::fabs(turn) / bl * static_cast<f64>(c.frontage_mm) * 0.5;
    p.front_x_mm = lx + mm_of(nx * (static_cast<f64>(edge) + lean) / 1000.0);
    p.front_z_mm = lz + mm_of(nz * (static_cast<f64>(edge) + lean) / 1000.0);
  };

  // **The order**: a tenement's key is its front's distance from the focus, lengthened by up to
  // 15 % with `irregularity` (so the village stays nucleated: a toft further out is rarely taken
  // before a nearer one); a cottage comes after three fifths of the houses' tenements, then by a
  // quarter of its distance. The church takes, of the tenements just past the houses' count (the
  // village's end, the grounding's "Where the church and the well stand"), the one on the highest
  // ground.
  {
    Vector<u64> tenement_keys;
    for (Candidate& c : cands) {
      Plot p;
      frame(c, p);
      const f64 d = direction(p.front_x_mm, p.front_z_mm, out.focus_x_mm, out.focus_z_mm).length_mm;
      c.key =
          static_cast<u64>(d * static_cast<f64>(1000 + irr * draw(c.seed, 6) * 3 / 20000) / 1000.0);
      if (c.kind == PlotKind::tenement) tenement_keys.push_back(c.key);
    }
    std::sort(tenement_keys.begin(), tenement_keys.end());
    const u64 later =
        tenement_keys.empty()
            ? 0
            : tenement_keys[std::min<u32>(tenement_keys.size() - 1, layer.houses * 3 / 5)];
    for (Candidate& c : cands)
      if (c.kind == PlotKind::cottage) c.key = later + c.key / 4;
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
      return a.key != b.key ? a.key < b.key : a.index < b.index;
    });
  }

  // **Taken up in order**: a candidate that would stand in a street (any way's, its green
  // included) or on another plot or the well is never a plot; the rest stand as plots, the houses
  // asked until the entry's count stands.
  Vector<Rect> rects;
  const auto in_a_street = [&](const Plot& p) {
    for (u32 li = 0; li < out.lanes.size(); ++li) {
      const LaneLine& line = out.lanes[li];
      const f64 reach = static_cast<f64>(p.frontage_mm + p.depth_mm + 40000);
      for (u32 k = 0; k < line.stations.size(); ++k) {
        const LaneStation& st = line.stations[k];
        const f64 dx = static_cast<f64>(st.x_mm - p.front_x_mm),
                  dz = static_cast<f64>(st.z_mm - p.front_z_mm);
        if (dx * dx + dz * dz > reach * reach) continue;
        const Dir t = tangent(line, k);
        const f64 offsets[5] = {
            -static_cast<f64>(st.right_mm) * 0.75, -static_cast<f64>(st.right_mm) * 0.5, 0.0,
            static_cast<f64>(st.left_mm) * 0.5, static_cast<f64>(st.left_mm) * 0.75};
        for (const f64 o : offsets)
          if (inside_plot(p, static_cast<f64>(st.x_mm) - t.z * o,
                          static_cast<f64>(st.z_mm) + t.x * o, 300.0))
            return true;
      }
    }
    return false;
  };
  const f64 well_clear = static_cast<f64>(k_well_radius_mm + 1000);
  // Where a candidate stands, if anywhere: where its whole depth would stand on a street, a plot or
  // the well, the same plot square to its way, then shortened to three quarters and a little over
  // a half, turned and square, as a later plot squeezed in is. A toft of a row keeps its front and
  // gives up depth: its croft shortened by steps to three tenths, as one on the inside of a bend is
  // beside its neighbours'.
  const auto fit = [&](const Candidate& c, Plot& p, Rect& r) {
    bool crowded = true;
    const u32 attempts = 6u;
    for (u32 attempt = 0; attempt < attempts && crowded; ++attempt) {
      const i64 shares[3] = {1000, 750, 550};
      const i64 chord_shares[6] = {1000, 850, 700, 550, 400, 300};
      frame(c, p, (attempt & 1u) != 0);
      p.depth_mm = c.chord ? c.depth_mm * chord_shares[attempt] / 1000
                           : c.depth_mm * shares[attempt / 2] / 1000;
      if (in_a_street(p)) continue;
      r = rect_of(p, 10.0);
      // A croft ends where the ground does: a back corner off the heightfield shortens it.
      bool on_ground = true;
      for (u32 q = 2; q < 4; ++q)
        on_ground = on_ground && std::isfinite(ground.surface_mm(mm_of(r.x[q] / 1000.0),
                                                                 mm_of(r.z[q] / 1000.0)));
      if (!on_ground) continue;
      crowded = false;
      for (const Rect& o : rects)
        crowded = crowded || overlap(r, o);
      if (green > 2 * half_street) {
        Plot grown = p;
        grown.front_x_mm -= mm_of(p.inward_x * well_clear / 1000.0);
        grown.front_z_mm -= mm_of(p.inward_z * well_clear / 1000.0);
        grown.frontage_mm += 2 * static_cast<i64>(well_clear);
        grown.depth_mm += 2 * static_cast<i64>(well_clear);
        crowded = crowded || inside_plot(grown, static_cast<f64>(out.focus_x_mm),
                                         static_cast<f64>(out.focus_z_mm), 0.0);
      }
    }
    return !crowded;
  };
  // **The church** (the grounding's "Where the church and the well stand": usually at the
  // village's end): set out when the houses stand, just past an end of the built houses along a
  // way, on either side (the first place out from each end where its 42 by 24 m stands on no
  // street, plot or well and wholly on the ground), the one on the highest ground.
  bool church_tried = !layer.church;
  const auto set_out_church = [&](u32 from) {
    church_tried = true;
    Plot best;
    Rect best_rect;
    i64 highest = std::numeric_limits<i64>::min();
    (void)from;
    // Past each end of the houses along each way, on either side, a little further out step by
    // step until it stands.
    for (u32 li = 0; li < ways; ++li)
      for (u8 side = 0; side < 2; ++side)
        for (i32 dir = 1; dir >= -1; dir -= 2) {
          i64 end = dir > 0 ? std::numeric_limits<i64>::min() : std::numeric_limits<i64>::max();
          for (const Plot& q : out.plots)
            if (q.lane == li && q.state == PlotState::built)
              end = dir > 0 ? std::max(end, q.along_mm + q.frontage_mm / 2)
                            : std::min(end, q.along_mm - q.frontage_mm / 2);
          if (end == std::numeric_limits<i64>::min() || end == std::numeric_limits<i64>::max())
            continue;
          for (i64 step = 0; step < 36; ++step) {
            // Out along the way by 5 m steps; at each, its front the chord of the street's edge,
            // and moved back by 0 to 6 m where a bend brings the street into it.
            Candidate cc;
            cc.lane = li;
            cc.side = side;
            cc.kind = PlotKind::church;
            cc.chord = true;
            cc.push_mm = (step % 4) * 2000;
            cc.s_mm = end + dir * (k_church_frontage_mm / 2 + 2000 + (step / 4) * 5000);
            cc.frontage_mm = k_church_frontage_mm;
            cc.depth_mm = k_church_depth_mm;
            if (cc.s_mm - k_church_frontage_mm / 2 < 0 ||
                cc.s_mm + k_church_frontage_mm / 2 > out.lanes[cc.lane].length_mm)
              continue;
            Plot p;
            Rect r;
            if (!fit(cc, p, r) || p.depth_mm != k_church_depth_mm) continue;
            bool on = true;
            for (u32 q = 0; q < 4; ++q)
              on = on &&
                   std::isfinite(ground.surface_mm(mm_of(r.x[q] / 1000.0), mm_of(r.z[q] / 1000.0)));
            i64 x = 0, z = 0;
            plot_point(p, 0.0, static_cast<f64>(p.depth_mm) * 0.5, x, z);
            const f64 h = ground.surface_mm(x, z);
            if (!on || !std::isfinite(h)) continue;
            if (mm_of(h) > highest) {
              highest = mm_of(h);
              best = p;
              best_rect = r;
            }
            break;
          }
        }
    if (highest == std::numeric_limits<i64>::min()) return;
    best.order = out.plots.size();
    best.state = PlotState::reserved;
    rects.push_back(best_rect);
    out.church = true;
    out.plots.push_back(best);
  };
  for (u32 ci = 0; ci < cands.size(); ++ci) {
    const Candidate& c = cands[ci];
    if (out.built >= layer.houses && !church_tried) set_out_church(ci);
    Plot p;
    Rect r;
    if (!fit(c, p, r)) continue;
    p.order = out.plots.size();
    rects.push_back(r);
    if (out.built >= layer.houses) {
      p.state = PlotState::spare;
      ++out.spare;
      out.plots.push_back(p);
      continue;
    }
    // **The setback**, uneven (the grounding's "Plots"): the entry's at `irregularity` 0, and
    // drawn from 0.15 to 1.85 times it at 1, so one house stands on the street's edge and its
    // neighbour back behind a yard.
    const i64 back_by =
        setback + div_round(setback * irr * (draw(p.seed, 10) - 500) * 17, 10000000);
    HouseAsk ask;
    ask.frontage_mm = p.frontage_mm;
    ask.depth_mm = p.depth_mm;
    ask.setback_mm = back_by;
    ask.seed = p.seed;
    ask.storeys = c.kind == PlotKind::tenement ? layer.storeys : 1u;
    ask.index = p.order;
    p.storeys = ask.storeys;
    House h;
    std::string why;
    if (!house_fn(house_context, ask, h, why)) {
      p.state = PlotState::refused;
      ++out.refused;
      if (out.first_refusal.empty()) out.first_refusal = why;
      out.plots.push_back(p);
      continue;
    }
    // Turned by quarters until its door faces the street (-z of the plot's frame).
    i64 ox = h.out_x, oz = h.out_z;
    i64 cx[4] = {h.min_x_mm, h.max_x_mm, h.max_x_mm, h.min_x_mm};
    i64 cz[4] = {h.min_z_mm, h.min_z_mm, h.max_z_mm, h.max_z_mm};
    i64 door_x = h.door_x_mm, door_z = h.door_z_mm;
    u8 turns = 0;
    while (!(ox == 0 && oz == -1) && turns < 4) {
      quarter(ox, oz);
      quarter(door_x, door_z);
      for (u32 q = 0; q < 4; ++q)
        quarter(cx[q], cz[q]);
      ++turns;
    }
    const i64 x0 = std::min(std::min(cx[0], cx[1]), std::min(cx[2], cx[3]));
    const i64 x1 = std::max(std::max(cx[0], cx[1]), std::max(cx[2], cx[3]));
    const i64 z0 = std::min(std::min(cz[0], cz[1]), std::min(cz[2], cz[3]));
    const i64 z1 = std::max(std::max(cz[0], cz[1]), std::max(cz[2], cz[3]));
    if (turns == 4 || x1 - x0 + 2 * k_side_gap_mm > p.frontage_mm ||
        back_by + (z1 - z0) > p.depth_mm) {
      p.state = PlotState::no_fit;
      ++out.no_fit;
      if (out.first_refusal.empty()) {
        char text[200];
        std::snprintf(
            text, sizeof(text),
            "a house %.2f m along the street by %.2f m deep does not fit a plot of %.2f m "
            "frontage (half a metre each side) and %.2f m behind its setback",
            metres(x1 - x0), metres(z1 - z0), metres(p.frontage_mm), metres(p.depth_mm - back_by));
        out.first_refusal = text;
      }
      out.plots.push_back(p);
      continue;
    }
    p.quarter_turns = turns;
    // The box's middle on the plot's middle, its front face `setback` behind the plot's front.
    const i64 origin_u = -div_round(x0 + x1, 2), origin_v = back_by - z0;
    const auto to_world = [&](i64 hx, i64 hz, i64& x, i64& z) {
      plot_point(p, static_cast<f64>(origin_u + hx), static_cast<f64>(origin_v + hz), x, z);
    };
    to_world(0, 0, p.house_x_mm, p.house_z_mm);
    // The ground under the footprint: its four corners and its middle.
    const i64 sx[5] = {x0, x1, x1, x0, div_round(x0 + x1, 2)};
    const i64 sz[5] = {z0, z0, z1, z1, div_round(z0 + z1, 2)};
    i64 lo = 0, hi = 0, sum = 0;
    bool wet = false, off = false;
    for (u32 q = 0; q < 5; ++q) {
      i64 x = 0, z = 0;
      to_world(sx[q], sz[q], x, z);
      const f64 gh = ground.surface_mm(x, z);
      if (!std::isfinite(gh)) {
        off = true;
        break;
      }
      const i64 g = mm_of(gh);
      lo = q == 0 ? g : std::min(lo, g);
      hi = q == 0 ? g : std::max(hi, g);
      sum += g;
      wet = wet || context.excluded_mm(x, z,
                                       scene_gen::k_exclude_holes | scene_gen::k_exclude_channels |
                                           scene_gen::k_exclude_water);
    }
    if (off) {
      p.state = PlotState::outside;
      ++out.outside;
      out.plots.push_back(p);
      continue;
    }
    if (wet) {
      p.state = PlotState::wet;
      ++out.wet;
      out.plots.push_back(p);
      continue;
    }
    // The pad: `pad_margin_m` past the walls before and behind, and no more than half the side gap
    // to each side, where a neighbour's stands close in a row.
    // In front, no further than the plot's front.
    const i64 side_margin = std::min(margin, k_side_gap_mm / 2);
    const i64 front_margin = std::min(margin, back_by);
    const i64 px[4] = {x0 - side_margin, x1 + side_margin, x1 + side_margin, x0 - side_margin};
    const i64 pz[4] = {z0 - front_margin, z0 - front_margin, z1 + margin, z1 + margin};
    for (u32 q = 0; q < 4; ++q)
      to_world(px[q], pz[q], p.pad_x_mm[q], p.pad_z_mm[q]);
    // **The pad's level** (village.md, "Pads in a row"): the mean of the ground under the
    // footprint, unless a built pad near it holds it closer. A pad's bank rises from its edge at
    // `bank_slope` and its fill falls at `fill_slope`, so a neighbour `g` apart whose level differs
    // by more than the gentler of the two times `g` would cut into this footprint or fill it; the
    // level is kept within that of every pad within 6 m, and where no level is, or the one there is
    // asks more than `max_pad_step_m` of cut or fill under the walls, the plot is too steep.
    i64 level = div_round(sum, 5);
    {
      const f64 slope =
          std::min(static_cast<f64>(layer.bank_slope), static_cast<f64>(layer.fill_slope));
      i64 lo_level = std::numeric_limits<i64>::min(), hi_level = std::numeric_limits<i64>::max();
      for (const Plot& q : out.plots) {
        if (q.state != PlotState::built) continue;
        const f64 g = pad_gap(p, q);
        if (g >= 6000.0) continue;
        const i64 step = static_cast<i64>(slope * g);
        lo_level = std::max(lo_level, q.pad_mm - step);
        hi_level = std::min(hi_level, q.pad_mm + step);
      }
      if (lo_level > hi_level) hi = lo + max_step + 1;  // no level stands beside them all
      level = std::clamp(level, std::min(lo_level, hi_level), hi_level);
    }
    if (hi - lo > max_step || hi - level > max_step || level - lo > max_step) {
      p.state = PlotState::steep;
      ++out.steep;
      out.plots.push_back(p);
      continue;
    }
    p.pad_mm = level;
    p.floor_mm = h.floor_mm;
    to_world(door_x, door_z, p.door_x_mm, p.door_z_mm);
    // The path: square out of the door to the gate in the plot's front, then to the nearest
    // point of its way's tread edge.
    plot_point(p, static_cast<f64>(origin_u + door_x), 0.0, p.gate_x_mm, p.gate_z_mm);
    // The gate stands at the threshold's level: the yard before the door is level with the pad, and
    // the path falls or climbs to the tread from the gate. (A path sloping from the door would
    // cross the pad's level at its front, where the smooth minimum of the two edits dips: with the
    // uneven setbacks a house on the street's edge had its front cut by up to 5 cm.)
    p.gate_top_mm = p.pad_mm;
    // The nearest point of the way's line to the gate, and from it the tread's edge toward the
    // gate.
    const LaneLine& line = out.lanes[p.lane];
    f64 best = 1e300;
    i64 tread_here = line.half_tread_mm;
    for (u32 k = 0; k + 1 < line.stations.size(); ++k) {
      const LaneStation& a = line.stations[k];
      const LaneStation& b = line.stations[k + 1];
      const f64 ex = static_cast<f64>(b.x_mm - a.x_mm), ez = static_cast<f64>(b.z_mm - a.z_mm);
      const f64 gx = static_cast<f64>(p.gate_x_mm - a.x_mm),
                gz = static_cast<f64>(p.gate_z_mm - a.z_mm);
      const f64 ee = ex * ex + ez * ez;
      const f64 t = ee > 0.0 ? std::clamp((gx * ex + gz * ez) / ee, 0.0, 1.0) : 0.0;
      const f64 dx = gx - t * ex, dz = gz - t * ez;
      if (dx * dx + dz * dz < best) {
        best = dx * dx + dz * dz;
        p.lane_x_mm = a.x_mm + mm_of(t * ex / 1000.0);
        p.lane_z_mm = a.z_mm + mm_of(t * ez / 1000.0);
        p.path_top_mm = t < 0.5 ? a.top_mm : b.top_mm;
        tread_here = std::max(a.tread_mm, b.tread_mm);
      }
    }
    const Dir toward = direction(p.lane_x_mm, p.lane_z_mm, p.gate_x_mm, p.gate_z_mm);
    p.path_x_mm = p.lane_x_mm + mm_of(toward.x * static_cast<f64>(tread_here) / 1000.0);
    p.path_z_mm = p.lane_z_mm + mm_of(toward.z * static_cast<f64>(tread_here) / 1000.0);
    p.state = PlotState::built;
    p.house = out.houses.size();
    out.houses.push_back(std::move(h));
    ++out.built;
    if (c.kind == PlotKind::cottage) ++out.cottages;
    out.plots.push_back(p);
  }
  out.well = green > 2 * half_street;

  // **Back lanes** (village.md, "Back lanes"): behind each run of three or more built tenements of
  // one row whose fronts follow each other within two perches along their way, a lane along their
  // backs, kept only where it crosses no plot.
  if (layer.back_lanes) {
    for (u32 li = 0; li < ways; ++li)
      for (u8 side = 0; side < 2; ++side) {
        Vector<u32> row;
        for (u32 k = 0; k < out.plots.size(); ++k) {
          const Plot& p = out.plots[k];
          if (p.lane == li && p.side == side && p.state == PlotState::built &&
              p.kind == PlotKind::tenement)
            row.push_back(k);
        }
        std::sort(row.begin(), row.end(),
                  [&](u32 a, u32 b) { return out.plots[a].along_mm < out.plots[b].along_mm; });
        u32 first = 0;
        for (u32 k = 1; k <= row.size(); ++k) {
          const bool follows =
              k < row.size() &&
              out.plots[row[k]].along_mm - out.plots[row[k]].frontage_mm / 2 -
                      (out.plots[row[k - 1]].along_mm + out.plots[row[k - 1]].frontage_mm / 2) <=
                  2 * perch;
          if (follows) continue;
          if (k - first >= 3) {
            // Behind the deepest of the run, so the lane runs past every back and not into one.
            i64 deepest = 0;
            for (u32 q = first; q < k; ++q)
              deepest = std::max(deepest, out.plots[row[q]].depth_mm);
            Vector<i64> pts;
            for (u32 q = first; q < k; ++q) {
              const Plot& p = out.plots[row[q]];
              // Its two back corners, the earlier along the way first.
              const f64 half = static_cast<f64>(p.frontage_mm) * 0.5;
              const f64 back = static_cast<f64>(deepest + k_back_offset_mm);
              const f64 us[2] = {side == 0 ? -half : half, side == 0 ? half : -half};
              for (const f64 u : us) {
                i64 x = 0, z = 0;
                plot_point(p, u, back, x, z);
                pts.push_back(x);
                pts.push_back(z);
              }
            }
            smooth(pts);
            LaneLine line;
            station(pts, station_mm, line);
            line.half_tread_mm = k_back_half_tread_mm;
            line.back = true;
            line.from_lane = li;
            bool clear = tops(ground, line);
            for (const LaneStation& st : line.stations)
              for (const Plot& p : out.plots)
                clear = clear && !inside_plot(p, static_cast<f64>(st.x_mm),
                                              static_cast<f64>(st.z_mm), -500.0);
            // Nor into a street or the green: a back lane runs behind the crofts, never across
            // the way they front.
            for (const LaneStation& st : line.stations)
              for (u32 lj = 0; lj < ways && clear; ++lj) {
                f64 d2 = 0.0;
                const LaneStation& beside =
                    out.lanes[lj].stations[nearest_station(out.lanes[lj], st.x_mm, st.z_mm, &d2)];
                const f64 reach = static_cast<f64>(std::max(beside.left_mm, beside.right_mm) +
                                                   k_back_half_tread_mm + 1000);
                clear = d2 >= reach * reach;
              }
            if (clear) {
              for (LaneStation& st : line.stations) {
                st.left_mm = k_back_half_tread_mm + 500;
                st.right_mm = k_back_half_tread_mm + 500;
                st.tread_mm = k_back_half_tread_mm;
              }
              out.lanes.push_back(std::move(line));
              ++out.back_lanes;
            }
          }
          first = k;
        }
      }
  }
  out.size_class = out.built >= 6 ? "village" : "hamlet";
  return true;
}

u64 plan_hash(const VillagePlan& plan) {
  u64 h = hash_combine(k_village_version, plan.lanes.size());
  for (const LaneLine& lane : plan.lanes) {
    h = hash_combine(hash_combine(hash_combine(h, static_cast<u64>(lane.length_mm)),
                                  static_cast<u64>(lane.half_tread_mm)),
                     (lane.back ? 1u : 0u) + 2u * lane.from_lane);
    for (const LaneStation& s : lane.stations) {
      const i64 words[] = {s.x_mm, s.z_mm, s.top_mm, s.left_mm, s.right_mm, s.tread_mm};
      for (const i64 w : words)
        h = hash_combine(h, static_cast<u64>(w));
    }
  }
  const auto bits = [](f64 v) {
    u64 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
  };
  for (const Plot& p : plan.plots) {
    const u64 words[] = {p.lane,
                         p.order,
                         p.side,
                         static_cast<u64>(p.state),
                         static_cast<u64>(p.kind),
                         p.quarter_turns,
                         p.storeys,
                         static_cast<u64>(p.front_x_mm),
                         static_cast<u64>(p.front_z_mm),
                         bits(p.along_x),
                         bits(p.along_z),
                         static_cast<u64>(p.frontage_mm),
                         static_cast<u64>(p.depth_mm),
                         p.seed,
                         static_cast<u64>(p.house_x_mm),
                         static_cast<u64>(p.house_z_mm),
                         static_cast<u64>(p.pad_mm),
                         static_cast<u64>(p.door_x_mm),
                         static_cast<u64>(p.door_z_mm),
                         static_cast<u64>(p.gate_x_mm),
                         static_cast<u64>(p.gate_z_mm),
                         static_cast<u64>(p.gate_top_mm),
                         static_cast<u64>(p.path_x_mm),
                         static_cast<u64>(p.path_z_mm),
                         static_cast<u64>(p.path_top_mm)};
    for (const u64 w : words)
      h = hash_combine(h, w);
    for (u32 c = 0; c < 4; ++c)
      h = hash_combine(hash_combine(h, static_cast<u64>(p.pad_x_mm[c])),
                       static_cast<u64>(p.pad_z_mm[c]));
  }
  for (const House& house : plan.houses) {
    const i64 words[] = {house.min_x_mm, house.min_z_mm,  house.max_x_mm,
                         house.max_z_mm, house.door_x_mm, house.door_z_mm,
                         house.out_x,    house.out_z,     house.floor_mm};
    for (const i64 w : words)
      h = hash_combine(h, static_cast<u64>(w));
    h = hash_combine(h, hash_bytes(house.recipe.data(), house.recipe.size()));
  }
  h = hash_combine(hash_combine(h, static_cast<u64>(plan.focus_x_mm)),
                   static_cast<u64>(plan.focus_z_mm));
  for (const i64 g : plan.green)
    h = hash_combine(h, static_cast<u64>(g));
  return hash_combine(h, (plan.well ? 1u : 0u) + (plan.church ? 2u : 0u));
}

void ground_edits(const VillageLayer& layer, const VillagePlan& plan,
                  Vector<scene::TerrainEdit>& out) {
  const auto shaped = [&](scene::TerrainEdit& e) {
    e.mode = scene::TerrainEditMode::Bench;
    e.bank_slope = layer.bank_slope;
    e.fill_slope = layer.fill_slope;
    e.face_m = layer.face_m;
    e.fill_m = layer.face_m;
  };
  const std::string prefix = layer.name.empty() ? std::string("village") : layer.name;
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    const LaneLine& lane = plan.lanes[k];
    if (lane.stations.size() < 2) continue;
    scene::TerrainEdit e;
    e.name = prefix + " " + lane_name(layer, &plan, k) + " tread";
    shaped(e);
    for (const LaneStation& s : lane.stations) {
      e.points.push_back(WorldPos{metres(s.x_mm), metres(s.top_mm), metres(s.z_mm)});
      e.width_m.push_back(static_cast<f32>(metres(2 * s.tread_mm)));
    }
    out.push_back(std::move(e));
  }
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& p = plan.plots[k];
    if (p.state != PlotState::built) continue;
    char number[24];
    std::snprintf(number, sizeof(number), " plot %u", k);
    scene::TerrainEdit pad;
    pad.name = prefix + number + " pad";
    shaped(pad);
    for (u32 c = 0; c < 4; ++c)
      pad.outline.push_back(
          WorldPos{metres(p.pad_x_mm[c]), metres(p.pad_mm), metres(p.pad_z_mm[c])});
    out.push_back(std::move(pad));
    if (!(layer.path_width_m > 0.0f)) continue;
    // The path: the door, the gate in the plot's front, the tread's edge.
    scene::TerrainEdit path;
    path.name = prefix + number + " path";
    shaped(path);
    const i64 xs[3] = {p.door_x_mm, p.gate_x_mm, p.path_x_mm};
    const i64 zs[3] = {p.door_z_mm, p.gate_z_mm, p.path_z_mm};
    const i64 ys[3] = {p.pad_mm, p.gate_top_mm, p.path_top_mm};
    for (u32 q = 0; q < 3; ++q) {
      if (q > 0 && xs[q] == xs[q - 1] && zs[q] == zs[q - 1]) continue;
      path.points.push_back(WorldPos{metres(xs[q]), metres(ys[q]), metres(zs[q])});
      path.width_m.push_back(layer.path_width_m);
    }
    if (path.points.size() >= 2) out.push_back(std::move(path));
  }
}

void footprints(const VillageLayer& layer, const VillagePlan& plan, scene_gen::FootprintList& out,
                i64 tile_mm, i64 tile_x, i64 tile_z, i64 reach_mm) {
  const auto wanted = [&](const scene_gen::Footprint& f) {
    if (tile_mm <= 0) return true;
    const i64 r = static_cast<i64>(f.radius * 1000.0) + 1 + reach_mm;
    return f.x_mm + r >= tile_x * tile_mm && f.x_mm - r < (tile_x + 1) * tile_mm &&
           f.z_mm + r >= tile_z * tile_mm && f.z_mm - r < (tile_z + 1) * tile_mm;
  };
  const i64 half_path = mm_of(static_cast<f64>(layer.path_width_m) * 0.5);
  for (const LaneLine& lane : plan.lanes)
    for (u32 k = 0; k + 1 < lane.stations.size(); ++k) {
      const LaneStation& a = lane.stations[k];
      const LaneStation& b = lane.stations[k + 1];
      const scene_gen::Footprint band = scene_gen::Footprint::band(
          a.x_mm, a.z_mm, b.x_mm, b.z_mm, std::max(a.tread_mm, b.tread_mm));
      if (wanted(band)) out.add(band);
    }
  // **The green** (village-growth-grounding.md, "The green"): common grazing, kept short, so the
  // cover after the village keeps off it. The street's outline over the stretch where it is
  // opened, as one polygon a way that opens.
  if (plan.green.size() >= 6) {
    const u32 n = plan.green.size() / 2;
    Vector<scene_gen::FootprintPoint> pts;
    for (u32 k = 0; k < n; ++k)
      pts.push_back(scene_gen::FootprintPoint{plan.green[2 * k], plan.green[2 * k + 1]});
    const scene_gen::Footprint g = scene_gen::Footprint::polygon(
        std::span<const scene_gen::FootprintPoint>(pts.data(), pts.size()));
    if (wanted(g)) out.add(g);  // the list copies the points
  }
  for (const Plot& p : plan.plots) {
    if (p.state != PlotState::built) continue;
    scene_gen::FootprintPoint corners[4];
    for (u32 c = 0; c < 4; ++c)
      corners[c] = scene_gen::FootprintPoint{p.pad_x_mm[c], p.pad_z_mm[c]};
    const scene_gen::Footprint pad = scene_gen::Footprint::polygon(corners);
    if (wanted(pad)) out.add(pad);  // the list copies the points
    if (half_path < 1) continue;
    const i64 xs[3] = {p.door_x_mm, p.gate_x_mm, p.path_x_mm};
    const i64 zs[3] = {p.door_z_mm, p.gate_z_mm, p.path_z_mm};
    for (u32 q = 0; q + 1 < 3; ++q) {
      if (xs[q] == xs[q + 1] && zs[q] == zs[q + 1]) continue;
      const scene_gen::Footprint path =
          scene_gen::Footprint::band(xs[q], zs[q], xs[q + 1], zs[q + 1], half_path);
      if (wanted(path)) out.add(path);
    }
  }
  if (plan.well) {
    const scene_gen::Footprint well(plan.focus_x_mm, plan.focus_z_mm, metres(k_well_radius_mm));
    if (wanted(well)) out.add(well);
  }
}

namespace {

// A world point (whole millimetres) as a mask region's point: metres to the centimetre.
JsonValue region_point(i64 x_mm, i64 z_mm) {
  return point_json(static_cast<f64>(div_round(x_mm, 10)) / 100.0, 0.0,
                    static_cast<f64>(div_round(z_mm, 10)) / 100.0);
}

JsonValue region(std::string name, JsonValue polygon, std::string_view material, f64 feather_m) {
  JsonValue o = JsonValue::object();
  o["name"] = JsonValue(std::move(name));
  o["polygon"] = std::move(polygon);
  o["material"] = JsonValue(material);
  o["feather_m"] = JsonValue(feather_m);
  return o;
}

// A way's band as an outline: its left edge out, its right edge back, each `left(k)` and
// `right(k)` millimetres from the line at station k. A station on a straight stretch whose widths
// are its neighbours' adds nothing to the outline and is left out.
template <class Left, class Right>
JsonValue band_polygon(const Vector<LaneStation>& st, const Left& left, const Right& right) {
  JsonValue outline = JsonValue::array();
  Vector<i64> rx, rz;
  for (u32 q = 0; q < st.size(); ++q) {
    const LaneStation& a = st[q > 0 ? q - 1 : 0];
    const LaneStation& b = st[q + 1 < st.size() ? q + 1 : q];
    const LaneStation& c = st[q];
    if (q > 0 && q + 1 < st.size()) {
      const Dir in = direction(a.x_mm, a.z_mm, c.x_mm, c.z_mm);
      const Dir on = direction(c.x_mm, c.z_mm, b.x_mm, b.z_mm);
      const f64 turn = in.x * on.z - in.z * on.x;
      const auto same = [](f64 x, f64 y, f64 z) {
        return std::fabs(x - y) < 30.0 && std::fabs(x - z) < 30.0;
      };
      if (turn < 1e-3 && turn > -1e-3 && same(left(q), left(q - 1), left(q + 1)) &&
          same(right(q), right(q - 1), right(q + 1)))
        continue;
    }
    const Dir d = direction(a.x_mm, a.z_mm, b.x_mm, b.z_mm);
    const f64 l = left(q), r = right(q);
    outline.push_back(
        region_point(c.x_mm - mm_of(d.z * l / 1000.0), c.z_mm + mm_of(d.x * l / 1000.0)));
    rx.push_back(c.x_mm + mm_of(d.z * r / 1000.0));
    rz.push_back(c.z_mm - mm_of(d.x * r / 1000.0));
  }
  for (u32 q = rx.size(); q-- > 0;)
    outline.push_back(region_point(rx[q], rz[q]));
  return outline;
}

}  // namespace

JsonValue ground_regions_json(const VillageLayer& layer, const VillagePlan& plan,
                              const GroundLayers& names) {
  JsonValue list = JsonValue::array();
  const f64 half_path = static_cast<f64>(mm_of(static_cast<f64>(layer.path_width_m) * 0.5));
  const f64 yard = static_cast<f64>(mm_of(static_cast<f64>(layer.yard_m)));
  const f64 margin = static_cast<f64>(k_wear_margin_mm);
  const std::string prefix = layer.name.empty() ? std::string("village") : layer.name;
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    const LaneLine& line = plan.lanes[k];
    const Vector<LaneStation>& st = line.stations;
    if (st.size() < 2) continue;
    const std::string lane = prefix + " " + lane_name(layer, &plan, k);
    // The tread's half width at each station (it wanders, and widens where the ways meet), and the
    // narrowest of it, which decides whether a middle is laid at all.
    f64 half_tread = 1e300;
    for (const LaneStation& s : st)
      half_tread = std::min(half_tread, static_cast<f64>(s.tread_mm));
    const auto tread_plus = [&](f64 v) {
      return [&st, v](u32 q) { return static_cast<f64>(st[q].tread_mm) + v; };
    };
    // The verge is the street's whole width, front to front: it follows the street's own wander
    // and opens into the green. A back lane has none: it is a worn track between two hedges.
    f64 narrowest = 1e300;
    for (const LaneStation& s : st)
      narrowest = std::min(narrowest, static_cast<f64>(std::min(s.left_mm, s.right_mm)));
    if (!names.verge.empty() && !line.back && narrowest > half_tread + margin)
      list.push_back(region(lane + " verge",
                            band_polygon(
                                st, [&](u32 q) { return static_cast<f64>(st[q].left_mm); },
                                [&](u32 q) { return static_cast<f64>(st[q].right_mm); }),
                            names.verge,
                            std::min(1.0, (narrowest - half_tread - margin) / 1000.0)));
    if (!names.worn.empty())
      list.push_back(region(lane + " wear",
                            band_polygon(st, tread_plus(margin), tread_plus(margin)), names.worn,
                            2.0 * margin / 1000.0));
    if (!names.tread.empty() && !line.back && half_tread > 2.0 * margin)
      list.push_back(region(lane + " tread",
                            band_polygon(st, tread_plus(-margin), tread_plus(-margin)), names.tread,
                            margin / 1000.0));
  }
  if (names.worn.empty()) return list;
  // The well's trodden ground, an octagon round it.
  if (plan.well) {
    JsonValue outline = JsonValue::array();
    const f64 r = static_cast<f64>(k_well_yard_mm);
    const f64 c8[8][2] = {{1, 0},  {0.7071, 0.7071},   {0, 1},  {-0.7071, 0.7071},
                          {-1, 0}, {-0.7071, -0.7071}, {0, -1}, {0.7071, -0.7071}};
    for (const auto& c : c8)
      outline.push_back(region_point(plan.focus_x_mm + mm_of(c[0] * r / 1000.0),
                                     plan.focus_z_mm + mm_of(c[1] * r / 1000.0)));
    list.push_back(region(prefix + " well", std::move(outline), names.worn, 1.0));
  }
  // A built plot's door yard: the ground from the plot's front to the wall, `yard_m` each side of
  // the door, kept 30 cm inside the plot's sides; and its path from the gate to the tread's edge.
  const f64 half_yard = std::max(yard, half_path);
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& p = plan.plots[k];
    if (p.state != PlotState::built) continue;
    char number[24];
    std::snprintf(number, sizeof(number), " plot %u", k);
    f64 du = 0.0, dv = 0.0;
    plot_frame(p, p.door_x_mm, p.door_z_mm, du, dv);
    const f64 reach = static_cast<f64>(p.frontage_mm) * 0.5 - 300.0;
    const f64 y0 = std::max(du - half_yard, -reach), y1 = std::min(du + half_yard, reach);
    if (half_yard > 0.0 && dv > 1.0 && y1 > y0) {
      JsonValue outline = JsonValue::array();
      const auto at = [&](f64 u, f64 v) {
        i64 x = 0, z = 0;
        plot_point(p, u, v, x, z);
        outline.push_back(region_point(x, z));
      };
      at(y1, -half_path);
      at(y1, dv);
      at(y0, dv);
      at(y0, -half_path);
      list.push_back(region(prefix + number + " yard", std::move(outline), names.worn, 0.4));
    }
    // (Not across the green: a path of more than 8 m from the gate is common grass walked over
    // anywhere, not a worn strip; a test scene drew the green as stripes.)
    const f64 path_length = direction(p.gate_x_mm, p.gate_z_mm, p.path_x_mm, p.path_z_mm).length_mm;
    if (half_path > 0.0 && path_length > 0.0 && path_length <= 8000.0) {
      // The path's band past the tread's wear, from the gate.
      const Dir d = direction(p.gate_x_mm, p.gate_z_mm, p.path_x_mm, p.path_z_mm);
      const f64 on = margin;
      const i64 ex = p.path_x_mm + mm_of(d.x * on / 1000.0),
                ez = p.path_z_mm + mm_of(d.z * on / 1000.0);
      const i64 ox = mm_of(-d.z * half_path / 1000.0), oz = mm_of(d.x * half_path / 1000.0);
      JsonValue outline = JsonValue::array();
      outline.push_back(region_point(p.gate_x_mm + ox, p.gate_z_mm + oz));
      outline.push_back(region_point(ex + ox, ez + oz));
      outline.push_back(region_point(ex - ox, ez - oz));
      outline.push_back(region_point(p.gate_x_mm - ox, p.gate_z_mm - oz));
      list.push_back(region(prefix + number + " path", std::move(outline), names.worn, 0.4));
    }
  }
  return list;
}

void plot_boundaries(const VillageLayer& layer, const VillagePlan& plan,
                     Vector<BoundaryLine>& out) {
  // A side as it stood: from its front corner back, its direction and its length.
  struct Side {
    i64 fx = 0, fz = 0;
    f64 dx = 0.0, dz = 0.0, length = 0.0;
  };
  // Two tofts of one row meet at a corner each computed from its own front, so the two stand
  // within a millimetre or two; two centimetres is far under any boundary's width.
  const auto same = [](i64 ax, i64 az, i64 bx, i64 bz) {
    const i64 dx = ax - bx, dz = az - bz;
    return dx <= 20 && dx >= -20 && dz <= 20 && dz >= -20;
  };
  Vector<Side> stood;
  // How far back from its front corner a side is already a boundary: an earlier side from the
  // same corner along the same line (to 2 cm over its length) stands for as much of it as it
  // reaches, so a deeper croft beside a shallower one carries on alone past its neighbour's back.
  const auto covered = [&](const Side& s) {
    f64 most = 0.0;
    for (const Side& t : stood) {
      if (!same(s.fx, s.fz, t.fx, t.fz)) continue;
      const f64 reach = std::min(s.length, t.length);
      const f64 off = (s.dx * t.dz - s.dz * t.dx) * reach;
      if (off > 20.0 || off < -20.0 || s.dx * t.dx + s.dz * t.dz < 0.0) continue;
      most = std::max(most, reach);
    }
    return most;
  };
  const bool gated = layer.path_width_m > 0.0f;
  for (u32 k = 0; k < plan.plots.size(); ++k) {
    const Plot& p = plan.plots[k];
    if (p.state != PlotState::built) continue;
    const f64 half = static_cast<f64>(p.frontage_mm) * 0.5, depth = static_cast<f64>(p.depth_mm);
    // The corners, round the plot: 0 and 1 on its front, 2 and 3 behind them.
    i64 cx[4] = {0, 0, 0, 0}, cz[4] = {0, 0, 0, 0};
    plot_point(p, -half, 0.0, cx[0], cz[0]);
    plot_point(p, half, 0.0, cx[1], cz[1]);
    plot_point(p, half, depth, cx[2], cz[2]);
    plot_point(p, -half, depth, cx[3], cz[3]);
    const Side far_side{cx[1], cz[1], p.inward_x, p.inward_z, depth};
    const Side near_side{cx[0], cz[0], p.inward_x, p.inward_z, depth};
    const f64 far_done = covered(far_side), near_done = covered(near_side);
    stood.push_back(far_side);
    stood.push_back(near_side);
    // The four edges round the plot, each as much of it as no earlier plot's boundary stands on:
    // the front (with the gateway), the far side from where its neighbour's ends, the back, the
    // near side to where its neighbour's begins. A side within 2 cm of whole is none.
    struct Edge {
      i64 ax, az, bx, bz;
      bool front;
    };
    Edge edges[4];
    u32 count = 0;
    edges[count++] = Edge{cx[0], cz[0], cx[1], cz[1], true};
    if (far_done < depth - 20.0) {
      i64 x = cx[1], z = cz[1];
      if (far_done > 0.0) plot_point(p, half, far_done, x, z);
      edges[count++] = Edge{x, z, cx[2], cz[2], false};
    }
    edges[count++] = Edge{cx[2], cz[2], cx[3], cz[3], false};
    if (near_done < depth - 20.0) {
      i64 x = cx[0], z = cz[0];
      if (near_done > 0.0) plot_point(p, -half, near_done, x, z);
      edges[count++] = Edge{cx[3], cz[3], x, z, false};
    }
    // Joined into lines where one edge ends where the next begins, starting after a break (or at
    // the front, when the four close round).
    u32 start = 0;
    bool closed = true;
    for (u32 e = 0; e < count; ++e) {
      const Edge& prev = edges[(e + count - 1) % count];
      if (prev.bx != edges[e].ax || prev.bz != edges[e].az) {
        start = e;
        closed = false;
        break;
      }
    }
    // The gateway: where the path crosses the front, from corner 0.
    f64 du = 0.0, dv = 0.0;
    plot_frame(p, p.gate_x_mm, p.gate_z_mm, du, dv);
    const f64 gate = std::clamp(du + half, 0.0, 2.0 * half);
    BoundaryLine b;
    f64 run = 0.0;
    const auto flush = [&]() {
      if (b.points_mm.size() >= 4) out.push_back(std::move(b));
      b = BoundaryLine{};
      run = 0.0;
    };
    for (u32 i = 0; i < count; ++i) {
      const Edge& e = edges[(start + i) % count];
      if (!b.points_mm.empty() && (b.points_mm[b.points_mm.size() - 2] != e.ax ||
                                   b.points_mm[b.points_mm.size() - 1] != e.az))
        flush();
      if (b.points_mm.empty()) {
        b.plot = k;
        b.points_mm.push_back(e.ax);
        b.points_mm.push_back(e.az);
      }
      if (e.front && gated) b.gateways_mm.push_back(mm_of((run + gate) / 1000.0));
      run += direction(e.ax, e.az, e.bx, e.bz).length_mm;
      if (closed && i + 1 == count) {
        b.closed = true;  // the last edge ends at the first point
      } else {
        b.points_mm.push_back(e.bx);
        b.points_mm.push_back(e.bz);
      }
    }
    flush();
  }
}

JsonValue plan_json(const VillageLayer& layer, const VillagePlan& plan) {
  JsonValue o = JsonValue::object();
  o["name"] = JsonValue(layer.name);
  o["class"] = JsonValue(plan.size_class);
  o["houses"] = JsonValue(plan.built);
  o["cottages"] = JsonValue(plan.cottages);
  o["plots"] = JsonValue(plan.plots.size());
  o["spare"] = JsonValue(plan.spare);
  o["steep"] = JsonValue(plan.steep);
  o["wet"] = JsonValue(plan.wet);
  o["no_fit"] = JsonValue(plan.no_fit);
  o["refused"] = JsonValue(plan.refused);
  o["outside"] = JsonValue(plan.outside);
  o["church"] = JsonValue(plan.church);
  o["back_lanes"] = JsonValue(plan.back_lanes);
  o["focus"] =
      point_json(metres(plan.focus_x_mm), metres(plan.focus_top_mm), metres(plan.focus_z_mm));
  o["well"] = JsonValue(plan.well);
  char hash[24];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(plan_hash(plan)));
  o["hash"] = JsonValue(hash);
  JsonValue lanes = JsonValue::array();
  for (u32 k = 0; k < plan.lanes.size(); ++k) {
    JsonValue l = JsonValue::object();
    l["name"] = JsonValue(lane_name(layer, &plan, k));
    l["back"] = JsonValue(plan.lanes[k].back);
    l["length_m"] = JsonValue(metres(plan.lanes[k].length_mm));
    l["stations"] = JsonValue(plan.lanes[k].stations.size());
    lanes.push_back(std::move(l));
  }
  o["lanes"] = std::move(lanes);
  JsonValue plots = JsonValue::array();
  for (const Plot& p : plan.plots) {
    JsonValue q = JsonValue::object();
    q["lane"] = JsonValue(p.lane);
    q["order"] = JsonValue(p.order);
    q["kind"] = JsonValue(kind_name(p.kind));
    q["side"] = JsonValue(p.side == 0 ? "left" : "right");
    q["state"] = JsonValue(state_name(p.state));
    q["frontage_m"] = JsonValue(metres(p.frontage_mm));
    q["depth_m"] = JsonValue(metres(p.depth_mm));
    q["front"] = point_json(metres(p.front_x_mm), 0.0, metres(p.front_z_mm));
    if (p.state == PlotState::built) {
      const House& h = plan.houses[p.house];
      q["house"] = JsonValue(h.name);
      q["storeys"] = JsonValue(p.storeys);
      q["at"] = point_json(metres(p.house_x_mm), metres(p.pad_mm), metres(p.house_z_mm));
      q["door"] =
          point_json(metres(p.door_x_mm), metres(p.pad_mm + h.floor_mm), metres(p.door_z_mm));
      q["length_m"] = JsonValue(metres(h.max_x_mm - h.min_x_mm));
      q["width_m"] = JsonValue(metres(h.max_z_mm - h.min_z_mm));
    }
    plots.push_back(std::move(q));
  }
  o["plot_list"] = std::move(plots);
  return o;
}

scene_gen::Placement house_placement(const Plot& plot) {
  f64 c = 1.0, s = 0.0;
  turn_of(plot, c, s);
  // The half angle from the cosine and the sine alone: one square root and one division, by the
  // branch that keeps the divisor away from zero.
  f64 half_c = 1.0, half_s = 0.0;
  if (c >= 0.0) {
    half_c = std::sqrt((1.0 + c) * 0.5);
    half_s = s / (2.0 * half_c);
  } else {
    half_s = std::sqrt((1.0 - c) * 0.5);
    if (s < 0.0) half_s = -half_s;
    half_c = s / (2.0 * half_s);
  }
  scene_gen::Placement out;
  out.position = WorldPos{metres(plot.house_x_mm), metres(plot.pad_mm + plot.floor_mm),
                          metres(plot.house_z_mm)};
  out.rotation = Quat{0.0f, static_cast<f32>(half_s), 0.0f, static_cast<f32>(half_c)};
  return out;
}

WorldPos house_point(const Plot& plot, i64 x_mm, i64 y_mm, i64 z_mm) {
  f64 c = 1.0, s = 0.0;
  turn_of(plot, c, s);
  const f64 x = static_cast<f64>(x_mm), z = static_cast<f64>(z_mm);
  return WorldPos{metres(plot.house_x_mm) + (x * c + z * s) / 1000.0,
                  metres(plot.pad_mm + plot.floor_mm + y_mm),
                  metres(plot.house_z_mm) + (-x * s + z * c) / 1000.0};
}

bool box_house(void* context, const HouseAsk& ask, House& out, std::string&) {
  const BoxHouse defaults;
  const BoxHouse& box = context != nullptr ? *static_cast<const BoxHouse*>(context) : defaults;
  // Long side to the street where the frontage holds it, gable-on where it does not.
  const bool gable = box.length_mm + 2 * k_side_gap_mm > ask.frontage_mm;
  const i64 across = gable ? box.width_mm : box.length_mm;
  const i64 deep = gable ? box.length_mm : box.width_mm;
  out = {};
  out.min_x_mm = -across / 2;
  out.max_x_mm = across / 2;
  out.min_z_mm = -deep / 2;
  out.max_z_mm = deep / 2;
  out.door_x_mm = 0;
  out.door_z_mm = out.min_z_mm;
  out.out_x = 0;
  out.out_z = -1;
  out.name = gable ? "box, gable-on" : "box";
  out.reach =
      static_cast<f32>(std::sqrt(static_cast<f64>(across * across + deep * deep)) * 0.5 / 1000.0);
  return true;
}

}  // namespace engine::village
