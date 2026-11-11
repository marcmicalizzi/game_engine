// A wall's face as a waller leaves it (docs/subsystems/walls.md, "The stone" and "The face"):
// every face stone a split slab with its arrises knocked back, its face on the batter's line, and
// no line of sight through a standing module's face or a head's end to the far side or into a
// hole in the hearting. Measured on the solved mesh itself, not on the solver's report.

#include "walls_test_support.h"

#include <domain/walls/walls.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::walls;

namespace {

struct V {
  f64 x = 0, y = 0, z = 0;
};
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V crs(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
f64 dt(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V at(const assets::MeshData& m, u32 i) {
  const Vec3 p = m.positions[i];
  return {static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
}

Module module_of(ModuleKind kind, i32 grade, u64 seed) {
  Module m;
  m.kind = kind;
  m.grade_permille = grade;
  m.seed = seed;
  return m;
}

// Half the wall's width at a height above its foot line: the batter's line a face is built to.
f64 half_width(const Module& m, f64 h) {
  const f64 bw = static_cast<f64>(m.base_width_cm) / 100.0,
            tw = static_cast<f64>(m.top_width_cm) / 100.0;
  return 0.5 * (bw - (bw - tw) * std::clamp(h / body_height(m), 0.0, 1.0));
}

// What one face stone shows: its face's triangles (those inside its extent whose normal faces out
// of the wall), each vertex's distance proud of the batter's line, and the share of its face's
// area that is tilted 25 to 80 degrees off the face, the knocked-back arrises.
struct FaceOf {
  u32 triangles = 0;
  f64 proud_max = -1, proud_min = 1, proud_mean = 0;
  f64 arris_share = 0;
  // The face proper (its triangles within 25 degrees of the face) projected on the wall's
  // elevation: its area and its bounding rectangle's, so a rectangle is 1 and a lump less.
  f64 proper_area = 0;
  f64 proper_lo_x = 1e9, proper_hi_x = -1e9, proper_lo_y = 1e9, proper_hi_y = -1e9;
  Vector<V> proper_points;
  f64 rectangle() const { return (proper_hi_x - proper_lo_x) * (proper_hi_y - proper_lo_y); }
  // The largest of the face proper's four corner cuts: how far its bounding rectangle's corner is
  // from the nearest point of it, over the rectangle's shorter side. A rectangle (or one with its
  // corners only clipped) is near 0; a stone with a corner broken off is a fifth or more.
  f64 largest_cut() const {
    const f64 shorter = std::min(proper_hi_x - proper_lo_x, proper_hi_y - proper_lo_y);
    if (shorter <= 1e-9 || proper_points.empty()) return 0;
    f64 most = 0;
    for (const f64 cx : {proper_lo_x, proper_hi_x})
      for (const f64 cy : {proper_lo_y, proper_hi_y}) {
        f64 nearest = 1e9;
        for (const V& p : proper_points)
          nearest = std::min(nearest, std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy)));
        most = std::max(most, nearest);
      }
    return most / shorter;
  }
};

FaceOf face_of(const Module& m, const assets::MeshData& mesh, const Stone& s) {
  const f64 g = static_cast<f64>(m.grade_permille) / 1000.0;
  const f64 sg = static_cast<f64>(s.side);
  // The batter's outward normal on this side.
  const f64 bt = batter(m);
  const f64 nl = std::sqrt(1.0 + bt * bt);
  const V out{0, bt / nl, sg / nl};
  FaceOf f;
  f64 area = 0, tilted = 0, sum = 0;
  u32 verts = 0;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const V a = at(mesh, mesh.indices[t]), b = at(mesh, mesh.indices[t + 1]),
            c = at(mesh, mesh.indices[t + 2]);
    const V mid{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
    // Inside the stone's extent, a millimetre in along it and two up (its corners' jitter: a
    // neighbour's arris is not counted as this stone's).
    if (mid.x <= static_cast<f64>(s.x0) + 0.001 || mid.x >= static_cast<f64>(s.x1) - 0.001)
      continue;
    if (mid.y <= static_cast<f64>(s.bed) + 0.002 || mid.y >= static_cast<f64>(s.top) - 0.002)
      continue;
    if (mid.z * sg < std::abs(static_cast<f64>(s.z1)) - 0.03) continue;
    // A through's or a footing's, proud of the face: not this stone's.
    const f64 reach = std::abs(static_cast<f64>(s.z1)) + 0.02;
    if (a.z * sg > reach || b.z * sg > reach || c.z * sg > reach) continue;
    const V n = crs(sub(b, a), sub(c, a));
    const f64 l = std::sqrt(dt(n, n));
    if (l < 1e-12) continue;
    const f64 cosine = dt(n, out) / l;
    if (cosine < 0.02) continue;  // a bed, an end: not the face
    ++f.triangles;
    area += 0.5 * l;
    if (cosine < std::cos(25.0 * 3.141592653589793 / 180.0)) {
      tilted += 0.5 * l;
    } else {
      f.proper_area += 0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
      for (const V& p : {a, b, c}) {
        f.proper_points.push_back(p);
        f.proper_lo_x = std::min(f.proper_lo_x, p.x);
        f.proper_hi_x = std::max(f.proper_hi_x, p.x);
        f.proper_lo_y = std::min(f.proper_lo_y, p.y);
        f.proper_hi_y = std::max(f.proper_hi_y, p.y);
      }
    }
    // The line at the stone's middle: a level stone on a slope is built to it there (walls.md,
    // "The face").
    const f64 xm = 0.5 * static_cast<f64>(s.x0 + s.x1);
    for (const V& p : {a, b, c}) {
      const f64 d = p.z * sg - half_width(m, p.y - g * xm);
      f.proud_max = std::max(f.proud_max, d);
      f.proud_min = std::min(f.proud_min, d);
      sum += d;
      ++verts;
    }
  }
  if (verts > 0) f.proud_mean = sum / static_cast<f64>(verts);
  if (area > 0) f.arris_share = tilted / area;
  return f;
}

bool inner(const Stone& s) {
  // A face stone of the standing face, inside the module (its ends meet the next module's).
  return s.role == StoneRole::face && s.x0 > -1.4f && s.x1 < 1.4f && s.x1 - s.x0 >= 0.08f &&
         s.top - s.bed >= 0.04f;
}

// The nearest triangle a ray meets, or -1, and whether it is the hearting's (its UVs in the
// palette's last row of swatches).
struct Hits {
  explicit Hits(const assets::MeshData& m) : mesh(m) {
    // The near form's triangles in cells of 2 cm by 2 cm in the wall's elevation (x, y), each in
    // every cell its bounds overlap.
    for (u32 i = 0; i < m.indices.size(); ++i) {
      const Vec3 p = m.positions[m.indices[i]];
      lo_x = std::min(lo_x, static_cast<f64>(p.x));
      lo_y = std::min(lo_y, static_cast<f64>(p.y));
      hi_x = std::max(hi_x, static_cast<f64>(p.x));
      hi_y = std::max(hi_y, static_cast<f64>(p.y));
    }
    nx = static_cast<u32>((hi_x - lo_x) / k_cell) + 1;
    ny = static_cast<u32>((hi_y - lo_y) / k_cell) + 1;
    cells.resize(nx * ny);
    for (u32 t = 0; t + 2 < m.indices.size(); t += 3) {
      f64 ax = 1e9, bx = -1e9, ay = 1e9, by = -1e9;
      for (u32 k = 0; k < 3; ++k) {
        const Vec3 p = m.positions[m.indices[t + k]];
        ax = std::min(ax, static_cast<f64>(p.x));
        bx = std::max(bx, static_cast<f64>(p.x));
        ay = std::min(ay, static_cast<f64>(p.y));
        by = std::max(by, static_cast<f64>(p.y));
      }
      for (u32 j = cy(ay); j <= cy(by); ++j)
        for (u32 i = cx(ax); i <= cx(bx); ++i)
          cells[j * nx + i].push_back(t);
    }
  }
  static constexpr f64 k_cell = 0.02;
  const assets::MeshData& mesh;
  f64 lo_x = 1e9, lo_y = 1e9, hi_x = -1e9, hi_y = -1e9;
  u32 nx = 0, ny = 0;
  Vector<Vector<u32>> cells;
  u32 cx(f64 x) const {
    return static_cast<u32>(
        std::clamp(std::floor((x - lo_x) / k_cell), 0.0, static_cast<f64>(nx - 1)));
  }
  u32 cy(f64 y) const {
    return static_cast<u32>(
        std::clamp(std::floor((y - lo_y) / k_cell), 0.0, static_cast<f64>(ny - 1)));
  }
  // The cells a ray passes, marched a centimetre at a step, each tested once in a row.
  i64 first(V o, V d, f64 reach, f64& t_hit) const {
    i64 best = -1;
    t_hit = reach;
    u32 last = ~0u;
    for (f64 step = 0; step <= reach; step += 0.005) {
      const f64 px = o.x + d.x * step, py = o.y + d.y * step;
      if (px < lo_x - k_cell || px > hi_x + k_cell || py < lo_y - k_cell || py > hi_y + k_cell)
        continue;
      const u32 cell = cy(py) * nx + cx(px);
      if (cell == last) continue;
      last = cell;
      if (step > t_hit + 0.03) break;  // past the nearest hit by more than a cell
      for (u32 t : cells[cell]) {
        const V a = at(mesh, mesh.indices[t]), b = at(mesh, mesh.indices[t + 1]),
                c = at(mesh, mesh.indices[t + 2]);
        const V e1 = sub(b, a), e2 = sub(c, a);
        const V p = crs(d, e2);
        const f64 det = dt(e1, p);
        if (std::abs(det) < 1e-14) continue;
        const f64 inv = 1.0 / det;
        const V s = sub(o, a);
        const f64 u = dt(s, p) * inv;
        if (u < 0 || u > 1) continue;
        const V q = crs(s, e1);
        const f64 v = dt(d, q) * inv;
        if (v < 0 || u + v > 1) continue;
        const f64 tt = dt(e2, q) * inv;
        if (tt > 1e-9 && tt < t_hit) {
          t_hit = tt;
          best = static_cast<i64>(t);
        }
      }
    }
    return best;
  }
  bool hearting(i64 t) const {
    return mesh.uvs[mesh.indices[static_cast<u32>(t)]].y >= 15.0f * 8.0f / 128.0f;
  }
};

// A grid of rays at a face: how many pass through, how many reach the hearting, and the largest
// square of neighbouring rays that all reach it (a hole, where a joint is a line).
struct Sight {
  u32 rays = 0, through = 0, hearting = 0, hole = 0;
  V where{};  // the first ray through, else the largest hole's corner
};

Sight look(const Hits& hits, V origin, V du, V dv, u32 nu, u32 nv, V dir, f64 reach) {
  Sight s;
  Vector<u8> core;
  core.resize(nu * nv, 0);
  for (u32 j = 0; j < nv; ++j)
    for (u32 i = 0; i < nu; ++i) {
      const V o{origin.x + du.x * i + dv.x * j, origin.y + du.y * i + dv.y * j,
                origin.z + du.z * i + dv.z * j};
      f64 t = 0;
      const i64 hit = hits.first(o, dir, reach, t);
      ++s.rays;
      if (hit < 0) {
        if (s.through++ == 0) s.where = o;
      } else if (hits.hearting(hit)) {
        ++s.hearting;
        core[j * nu + i] = 1;
      }
    }
  // The largest k such that some k x k block of rays all reach the hearting.
  for (u32 k = 1; k <= 6; ++k) {
    bool any = false;
    for (u32 j = 0; j + k <= nv && !any; ++j)
      for (u32 i = 0; i + k <= nu && !any; ++i) {
        bool all = true;
        for (u32 b = 0; b < k && all; ++b)
          for (u32 a = 0; a < k && all; ++a)
            all = core[(j + b) * nu + i + a] != 0;
        any = all;
        if (all && s.through == 0)
          s.where = {origin.x + du.x * i + dv.x * j, origin.y + du.y * i + dv.y * j,
                     origin.z + du.z * i + dv.z * j};
      }
    if (!any) break;
    s.hole = k;
  }
  return s;
}

}  // namespace

TEST_CASE("walls: a face stone is a split slab, not a box") {
  // TCV, Breaking and shaping stone: a stone not struck fair "chips and rounds off"; the reels'
  // gneiss shows flat beds and rough faces with broken arrises. Every face stone's face here has
  // relief and a band of knocked-back arrises round it: a box has neither.
  u32 stones = 0, few = 0;
  f64 relief_sum = 0, arris_sum = 0, arris_low = 1;
  for (const u64 seed : {1ull, 2ull})
    for (const i32 grade : {0, 375}) {
      const Module m = module_of(ModuleKind::run, grade, seed);
      assets::MeshData mesh;
      ModuleReport report;
      std::string error;
      REQUIRE(solve_module(m, mesh, report, error));
      if (seed == 1 && grade == 0) {
        const u32* r = report.role_triangles;
        MESSAGE("a 3 m stretch: " << report.near_triangles << " triangles: face " << r[0]
                                  << ", footing " << r[1] << ", through " << r[2] << ", pinning "
                                  << r[4] << ", cover " << r[5] << ", cope " << r[6]
                                  << ", hearting " << r[8] << "; mid " << report.mid_triangles
                                  << ", outline " << report.far_triangles);
      }
      for (const Stone& s : report.stones) {
        if (!inner(s)) continue;
        const FaceOf f = face_of(m, mesh, s);
        // A split face is 20 triangles; the extent's margins leave a small stone fewer.
        if (f.triangles < 6) {
          ++few;
          continue;
        }
        const f64 relief = f.proud_max - f.proud_min;
        CHECK(relief >= 0.003);  // 3 mm or more between the face's high point and its arrises
        CHECK(relief <= 0.035);
        CHECK(f.arris_share >= 0.03);
        relief_sum += relief;
        arris_sum += f.arris_share;
        arris_low = std::min(arris_low, f.arris_share);
        ++stones;
      }
    }
  REQUIRE(stones > 200);
  CHECK(few * 50 <= stones);  // at most 2 % too small to measure
  MESSAGE(stones << " face stones (and " << few << " too small): mean relief "
                 << relief_sum / stones * 1000.0 << " mm, arrises " << arris_sum / stones * 100.0
                 << " % of the face (least " << arris_low * 100.0 << " %)");
  CHECK(relief_sum / stones >= 0.010);
  CHECK(arris_sum / stones >= 0.12);
}

TEST_CASE("walls: a face stone's outline is its own shape, not a rectangle") {
  // The reels' gneiss (r53, r07, r57) and TCV's characteristic walling stones: a face's outline
  // is broken at its corners and its sides are not square to each other, so the face proper (what
  // is within 25 degrees of the face) is a polygon of its own. Measured by its largest corner cut
  // (`FaceOf::largest_cut`): how far a corner of its bounding rectangle is from it, over the
  // rectangle's shorter side. Design values (G145c), not run against the base; G145b's face
  // proper was a rectangle with 10 to 30 mm chips.
  u32 stones = 0, square = 0;
  f64 sum = 0, area = 0;
  for (const u64 seed : {1ull, 2ull, 3ull})
    for (const i32 grade : {0, 375}) {
      const Module m = module_of(ModuleKind::run, grade, seed);
      assets::MeshData mesh;
      ModuleReport report;
      std::string error;
      REQUIRE(solve_module(m, mesh, report, error));
      for (const Stone& s : report.stones) {
        if (!inner(s) || (s.x1 - s.x0) * (s.top - s.bed) < 0.012f) continue;
        const FaceOf f = face_of(m, mesh, s);
        if (f.triangles < 6 || f.proper_area <= 0) continue;
        const f64 cut = f.largest_cut();
        sum += cut;
        area += f.proper_area / std::max(f.rectangle(), 1e-12);
        square += cut < 0.1 ? 1u : 0u;
        ++stones;
      }
    }
  REQUIRE(stones > 150);
  MESSAGE(stones << " face stones of 0.012 m^2 or more: the largest corner cut is " << sum / stones
                 << " of the shorter side on average, " << square
                 << " under 0.1; the face proper covers " << area / stones << " of its rectangle");
  CHECK(sum / stones >= 0.25);
  CHECK(square * 10 <= stones);
}

TEST_CASE("walls: a closing stone is a stone, not a sliver") {
  // What closes a face (a pinning under a stone, a closing stone where a course resumed) is a
  // small stone knocked in, not a vertical sliver: one narrower than 5 cm stands no taller than
  // twice its width and 1 cm. A design value (G145c), run against the base to see it fail:
  // G145b's closing stones were up to 2 by 12 cm and its pinnings 3 by 34.
  u32 small = 0, slivers = 0;
  f64 worst = 0;
  for (const u64 seed : {1ull, 2ull, 3ull})
    for (const i32 grade : {0, 250, 500})
      for (const ModuleKind kind : {ModuleKind::run, ModuleKind::head}) {
        const Module m = module_of(kind, grade, seed);
        assets::MeshData mesh;
        ModuleReport report;
        std::string error;
        REQUIRE(solve_module(m, mesh, report, error));
        for (const Stone& s : report.stones) {
          if (s.side == 0 || (s.role != StoneRole::pinning && s.role != StoneRole::face)) continue;
          if (s.x0 < -1.4f || s.x1 > 1.4f) continue;  // the module's ends meet the next one's
          const f64 w = static_cast<f64>(s.x1 - s.x0), h = static_cast<f64>(s.top - s.bed);
          if (w >= 0.05) continue;
          ++small;
          CHECK(w >= 0.02 - 1e-6);
          if (h > 2.0 * w + 0.01) {
            if (slivers++ == 0)
              MESSAGE("the first sliver: role " << static_cast<u32>(s.role) << ", " << w << " by "
                                                << h << " at " << s.x0 << ", " << s.bed << " (kind "
                                                << static_cast<u32>(kind) << ", grade " << grade
                                                << ", seed " << seed << ")");
          }
          worst = std::max(worst, h / w);
        }
      }
  MESSAGE(small << " stones narrower than 5 cm, " << slivers << " slivers, tallest " << worst
                << " times its width");
  CHECK(slivers == 0);
}

TEST_CASE("walls: the near form keeps its budget") {
  // G145's near form was 1,458 triangles a metre (walls.md, "Budgets": 145,788 for 100 m of
  // wall on a 1 in 8 slope); G145b's split faces took it to 1,979 and three vertices a triangle.
  // Measured the same way, a straight 100 m on 1 in 8, and held at G145's figure and the
  // vertices (an entry's bytes) at 3,500 a metre, a design value; G145b fails both (1,979
  // triangles a metre measured this way, flat-shaded at three vertices a triangle; and 2,532
  // and 7,143 a metre over stretches at grades 0, 250 and 500, run against it).
  Layer layer;
  WallLine line;
  line.points_mm = {0, 0, 100000, 0};
  layer.lines.push_back(line);
  Plan plan;
  std::string why;
  REQUIRE(make_plan(layer, 32000, plan, why));
  Vector<Module> kit;
  kit_recipes(layer, kit);
  f64 tris = 0, verts = 0, steep = 0, steep_m = 0;
  Vector<u32> tris_of, verts_of;
  tris_of.resize(kit.size(), 0);
  verts_of.resize(kit.size(), 0);
  const walls::test::TestPlane plane{0.125, 0.0, 0.0};
  const scene_gen::Ground ground = walls::test::ground_of(plane);
  for (const Slot& s : plan.slots) {
    // The module standing on 1 in 8, as the placement test stands it.
    const u32 k = stand_slot(plan, layer, s, ground).placement.mesh;
    if (tris_of[k] == 0) {
      assets::MeshData mesh;
      ModuleReport report;
      REQUIRE(solve_module(kit[k], mesh, report, why));
      tris_of[k] = report.near_triangles;
      // The finest form's vertices: those its indices name (the coarser forms' come after).
      u32 top = 0;
      for (const u32 i : mesh.indices)
        top = std::max(top, i);
      verts_of[k] = top + 1;
    }
    tris += tris_of[k];
    verts += verts_of[k];
  }
  for (const u64 seed : {1ull, 2ull}) {
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(solve_module(module_of(ModuleKind::run, 500, seed), mesh, report, why));
    steep += report.near_triangles;
    steep_m += report.length_m;
  }
  MESSAGE("near form, 100 m on 1 in 8: " << tris / 100.0 << " triangles and " << verts / 100.0
                                         << " vertices a metre; a stretch at 1 in 2: "
                                         << steep / steep_m << " triangles a metre");
  CHECK(tris / 100.0 <= 1460.0);
  CHECK(verts / 100.0 <= 3500.0);
}

TEST_CASE("walls: the face is flush to the batter's line") {
  // TCV, Building (Placing the stones): each stone "almost touching or just brushing the line",
  // none to "project beyond the general face of the wall". A face stone's high point stands no
  // more than 8 mm proud of the batter's line and no more than 6 mm behind it.
  u32 stones = 0, over = 0;
  f64 worst_proud = -1, worst_sunk = 1;
  for (const u64 seed : {1ull, 2ull, 3ull})
    for (const i32 grade : {0, 250, 500})
      for (const ModuleKind kind : {ModuleKind::run, ModuleKind::head}) {
        const Module m = module_of(kind, grade, seed);
        assets::MeshData mesh;
        ModuleReport report;
        std::string error;
        REQUIRE(solve_module(m, mesh, report, error));
        for (const Stone& s : report.stones) {
          if (!inner(s)) continue;
          const FaceOf f = face_of(m, mesh, s);
          if (f.triangles < 6) continue;
          // At 1 in 2 a level stone's ends stand off a battered line that climbs with the ground.
          CHECK(f.proud_max <= (grade <= 250 ? 0.012 : 0.025));
          over += f.proud_max > 0.008 ? 1u : 0u;
          CHECK(f.proud_max >= -0.006);
          worst_proud = std::max(worst_proud, f.proud_max);
          worst_sunk = std::min(worst_sunk, f.proud_max);
          ++stones;
        }
      }
  REQUIRE(stones > 500);
  CHECK(over * 50 <= stones);  // 98 % within 8 mm
  MESSAGE(stones << " face stones: the most proud " << worst_proud * 1000.0
                 << " mm, the most sunk high point " << worst_sunk * 1000.0 << " mm");
}

TEST_CASE("walls: the cope's stones differ in height, thickness and set") {
  // TCV, Types of coping: random upright coping, neighbours "rarely" more than 50 mm apart in
  // height, the whole the specification's height (250 to 300 mm); slabs and blocks of differing
  // thickness. Not books on a shelf: the thicknesses and heights spread.
  for (const u64 seed : {1ull, 2ull, 3ull}) {
    const Module m = module_of(ModuleKind::run, 0, seed);
    assets::MeshData mesh;
    ModuleReport report;
    std::string error;
    REQUIRE(solve_module(m, mesh, report, error));
    Vector<f64> h, t;
    for (const Stone& s : report.stones) {
      if (s.role != StoneRole::cope) continue;
      h.push_back(static_cast<f64>(s.top - s.bed));
      t.push_back(static_cast<f64>(s.x1 - s.x0));
    }
    REQUIRE(h.size() > 15);
    auto spread = [](const Vector<f64>& v, f64& mean) {
      mean = 0;
      for (f64 x : v)
        mean += x;
      mean /= static_cast<f64>(v.size());
      f64 var = 0;
      for (f64 x : v)
        var += (x - mean) * (x - mean);
      return std::sqrt(var / static_cast<f64>(v.size()));
    };
    f64 mh = 0, mt = 0;
    const f64 sh = spread(h, mh), st = spread(t, mt);
    u32 near_equal = 0;
    for (u32 i = 0; i < h.size(); ++i) {
      // A stone's height over the lower of the cover stones it stands on (they differ by up to
      // 30 mm where two meet under it).
      CHECK(h[i] >= 0.25 * 0.92);
      CHECK(h[i] <= 0.30 + 0.03);
      if (i + 1 < h.size()) CHECK(t[i] >= k_cope_thin_m - 1e-6);  // the last is cut at the end
      CHECK(t[i] <= k_cope_thick_m + 1e-6);
      if (i > 0) {
        CHECK(std::abs(h[i] - h[i - 1]) <= 0.05 + 0.02);  // the top's split on either stone
        if (std::abs(h[i] - h[i - 1]) < 0.005 && std::abs(t[i] - t[i - 1]) < 0.005) ++near_equal;
      }
    }
    INFO("seed " << seed << ": height " << mh << " +- " << sh << ", thickness " << mt << " +- "
                 << st << ", " << near_equal << " neighbours alike");
    CHECK(mh >= 0.25);
    CHECK(mh <= 0.30);
    CHECK(sh >= 0.010);
    CHECK(st >= 0.020);
    CHECK(near_equal * 10 <= h.size());
  }
}

TEST_CASE("walls: no line of sight through a face or a head's end") {
  // Rays at the face of standing modules, square on and at a slant, and at a head's end: none may
  // reach the far side, the hearting may show only in joints (a line, never a 3 cm hole), and
  // the share of rays that meet it is a joint's, not a gap's.
  u32 rays = 0, hearting = 0, slant_hole = 0;
  // Two seeds: 11 s in msvc-debug at G145c's triangles (G145b ran one; two took it 12 to 16 s).
  for (const u64 seed : {1ull, 2ull})
    for (const i32 grade : {0, 375})
      for (const ModuleKind kind : {ModuleKind::run, ModuleKind::head}) {
        const Module m = module_of(kind, grade, seed);
        assets::MeshData mesh;
        ModuleReport report;
        std::string error;
        REQUIRE(solve_module(m, mesh, report, error));
        const Hits hits(mesh);
        const f64 g = static_cast<f64>(grade) / 1000.0;
        const f64 body = body_height(m);
        // The +z face, square on and slanting down and along, over x in [-1.3, 1.3] and the
        // body's height above the ground's line (a slanting ray from 15 cm up, so it cannot pass
        // under the footing).
        const f64 inv = 1.0 / std::sqrt(1.0 + 0.3 * 0.3 + 0.2 * 0.2);
        for (const V dir : {V{0, 0, -1}, V{-0.3 * inv, -0.2 * inv, -inv}}) {
          // A slanting ray runs 0.4 m along the wall in crossing it, so it starts that far in
          // from the module's end it runs toward (past it is the next module's stone).
          // Square on a centimetre apart, slanting two (a hole is then 3 cm or 4 cm across).
          const bool slant = dir.x < 0;
          const f64 from = slant ? 0.15 : 0.04, step = slant ? 0.02 : 0.01;
          const f64 x0 = slant ? -0.9 : -1.3;
          const Sight s = look(hits, {x0, x0 * g + from, 1.0}, {step, step * g, 0}, {0, step, 0},
                               static_cast<u32>((1.3 - x0) / step) + 1,
                               static_cast<u32>((body - 0.02 - from) / step), dir, 3.0);
          INFO("kind " << static_cast<u32>(kind) << " grade " << grade << " seed " << seed
                       << " slant " << slant << " at " << s.where.x << ", " << s.where.y << ", "
                       << s.where.z);
          CHECK(s.through == 0);
          // Square on, the hearting shows only in joints: never a hole 4 cm across. A slanting ray
          // looks down a joint's length, so it is held to reaching nothing.
          if (!slant) CHECK(s.hole < 4u);
          slant_hole = std::max(slant_hole, slant ? s.hole : 0u);
          rays += s.rays;
          hearting += s.hearting;
        }
        if (kind == ModuleKind::head) {
          // The head's end, square on: across the width at each height.
          const f64 y0 = -1.5 * g;
          const Sight s = look(hits, {-2.0, y0 + 0.04, -0.17}, {0, 0, 0.01}, {0, 0.01, 0}, 35,
                               static_cast<u32>((body - 0.06) / 0.01), V{1, 0, 0}, 3.0);
          INFO("head end, grade " << grade << " seed " << seed << " at " << s.where.y << ", "
                                  << s.where.z);
          CHECK(s.through == 0);
          CHECK(s.hole < 4);
          rays += s.rays;
          hearting += s.hearting;
        }
      }
  MESSAGE(rays << " rays, " << hearting << " reach the hearting (" << 100.0 * hearting / rays
               << " %); slanting, the largest square of rays 2 cm apart that all do is "
               << slant_hole);
  CHECK(static_cast<f64>(hearting) <= 0.08 * static_cast<f64>(rays));
}
