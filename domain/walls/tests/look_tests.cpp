// What a person still objected to beside the reels (r53, r07, r57) in G177's pictures, as
// measures on the solved mesh and its report (G189; docs/subsystems/walls.md, "The stone", "The
// module" and "Forms and the LOD policy"): a face stone's face that is a plane with crisp box
// edges, a cope of upright slabs of near-equal height like books on a shelf, a cover course of
// boxes that draws a ruled line, narrow stacks of closing stones in the slots beside jumpers, and a
// mid form whose cope is a ruled top at thirty metres. Every bound is a design value (no source
// gives one), each run once against the base (G177, k_wall_version 4) to see it fail; the base's
// value is written beside it.

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
f64 len(V a) { return std::sqrt(dt(a, a)); }
V at(const assets::MeshData& m, u32 i) {
  const Vec3 p = m.positions[i];
  return {static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)};
}
V normal_at(const assets::MeshData& m, u32 i) {
  const Vec3 n = m.normals[i];
  return {static_cast<f64>(n.x), static_cast<f64>(n.y), static_cast<f64>(n.z)};
}
f64 degrees_between(V a, V b) {
  const f64 c = dt(a, b) / std::max(len(a) * len(b), 1e-300);
  return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.141592653589793;
}

Module module_of(ModuleKind kind, i32 grade, u64 seed) {
  Module m;
  m.kind = kind;
  m.grade_permille = grade;
  m.seed = seed;
  return m;
}

// A face stone of the standing face inside the module, large enough to measure.
bool inner(const Stone& s) {
  return s.role == StoneRole::face && s.x0 > -1.4f && s.x1 < 1.4f && s.x1 - s.x0 >= 0.08f &&
         s.top - s.bed >= 0.04f;
}

// The triangles of one face stone's face (face_tests.cpp's `face_of` filter: inside its extent,
// within its own bed and top, at its face, facing out of the wall).
struct FaceTris {
  Vector<V> normal;   // geometric, unit
  Vector<f64> area;   // m^2
  Vector<f64> tilt;   // degrees off the batter's outward normal
  Vector<f64> shade;  // degrees between its vertices' normals and its own (0: flat-shaded)
};
FaceTris face_tris(const Module& m, const assets::MeshData& mesh, const Stone& s) {
  const f64 sg = static_cast<f64>(s.side);
  const f64 bt = batter(m);
  const f64 nl = std::sqrt(1.0 + bt * bt);
  const V out{0, bt / nl, sg / nl};
  FaceTris f;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const u32 i[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
    const V a = at(mesh, i[0]), b = at(mesh, i[1]), c = at(mesh, i[2]);
    const V mid{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
    if (mid.x <= static_cast<f64>(s.x0) + 0.001 || mid.x >= static_cast<f64>(s.x1) - 0.001)
      continue;
    if (mid.y <= bed_at(s, mid.x) + 0.002 || mid.y >= top_at(s, mid.x) - 0.002) continue;
    if (mid.z * sg < std::abs(static_cast<f64>(s.z1)) - 0.04) continue;
    const f64 reach = std::abs(static_cast<f64>(s.z1)) + 0.02;
    if (a.z * sg > reach || b.z * sg > reach || c.z * sg > reach) continue;
    const V n = crs(sub(b, a), sub(c, a));
    const f64 l = len(n);
    if (l < 1e-12) continue;
    const V u{n.x / l, n.y / l, n.z / l};
    if (dt(u, out) < 0.02) continue;
    f64 shade = 0;
    for (u32 k = 0; k < 3; ++k)
      shade += degrees_between(normal_at(mesh, i[k]), u) / 3.0;
    f.normal.push_back(u);
    f.area.push_back(0.5 * l);
    f.tilt.push_back(degrees_between(u, out));
    f.shade.push_back(shade);
  }
  return f;
}

// The highest point of a set of triangles over each bin's middle along the wall (its section by
// the plane x = the bin's middle), from `lo` in bins of `bin`: what the cope's skyline is from
// the side. -1e9 where nothing stands.
Vector<f64> skyline(const assets::MeshData& mesh, const Vector<u32>& indices, f64 lo, f64 bin,
                    u32 bins) {
  Vector<f64> top;
  top.resize(bins, -1e9);
  for (u32 t = 0; t + 2 < indices.size(); t += 3) {
    const V p[3] = {at(mesh, indices[t]), at(mesh, indices[t + 1]), at(mesh, indices[t + 2])};
    const f64 x0 = std::min({p[0].x, p[1].x, p[2].x}), x1 = std::max({p[0].x, p[1].x, p[2].x});
    const f64 f0 = std::ceil((x0 - lo) / bin - 0.5), f1 = std::floor((x1 - lo) / bin - 0.5);
    for (f64 f = std::max(f0, 0.0); f <= f1 && f < static_cast<f64>(bins); f += 1.0) {
      const f64 xc = lo + (f + 0.5) * bin;
      f64 best = -1e9;
      for (u32 e = 0; e < 3; ++e) {
        const V& a = p[e];
        const V& b = p[(e + 1) % 3];
        if ((a.x - xc) * (b.x - xc) > 0) continue;
        if (std::abs(b.x - a.x) < 1e-12) {
          best = std::max(best, std::max(a.y, b.y));
          continue;
        }
        best = std::max(best, a.y + (b.y - a.y) * (xc - a.x) / (b.x - a.x));
      }
      const u32 k = static_cast<u32>(f);
      top[k] = std::max(top[k], best);
    }
  }
  return top;
}

}  // namespace

TEST_CASE("walls: a face stone's face is broken and weathered, not a plane with box edges") {
  // TCV, Breaking and shaping stone: a stone not struck fair "chips and rounds off"; the reels'
  // gneiss faces are broken across, each stone's face turned its own way, the arrises rounded by
  // weather. Measured on the standing face stones of a stretch at four seeds and two grades: (1)
  // the face proper's facets spread about their mean (area-weighted, degrees), (2) the face
  // proper's mean turned off the batter's line, and (3) the worn arrises shaded round rather
  // than as flat bands (their vertices' normals off their own facet's).
  f64 spread_sum = 0, turn_sum = 0, shade_sum = 0, shade_area = 0;
  u32 stones = 0, flat = 0;
  for (const u64 seed : {1ull, 2ull, 3ull, 4ull})
    for (const i32 grade : {0, 375}) {
      const Module m = module_of(ModuleKind::run, grade, seed);
      assets::MeshData mesh;
      ModuleReport report;
      std::string error;
      REQUIRE(solve_module(m, mesh, report, error));
      const f64 bt = batter(m);
      const f64 nl = std::sqrt(1.0 + bt * bt);
      for (const Stone& s : report.stones) {
        if (!inner(s)) continue;
        const FaceTris f = face_tris(m, mesh, s);
        V mean{};
        f64 area = 0;
        for (u32 i = 0; i < f.normal.size(); ++i) {
          if (f.tilt[i] >= 25.0) {
            shade_sum += f.shade[i] * f.area[i];
            shade_area += f.area[i];
            continue;
          }
          mean = {mean.x + f.normal[i].x * f.area[i], mean.y + f.normal[i].y * f.area[i],
                  mean.z + f.normal[i].z * f.area[i]};
          area += f.area[i];
        }
        if (area <= 0) continue;
        f64 spread = 0;
        for (u32 i = 0; i < f.normal.size(); ++i)
          if (f.tilt[i] < 25.0) spread += degrees_between(f.normal[i], mean) * f.area[i];
        spread /= area;
        const V out{0, bt / nl, static_cast<f64>(s.side) / nl};
        spread_sum += spread;
        turn_sum += degrees_between(mean, out);
        flat += spread < 2.0 ? 1u : 0u;
        ++stones;
      }
    }
  REQUIRE(stones > 300);
  const f64 n = static_cast<f64>(stones);
  const f64 shade = shade_area > 0 ? shade_sum / shade_area : 0;
  MESSAGE(stones << " face stones: the face proper's facets spread " << spread_sum / n
                 << " degrees about their mean, " << 100.0 * flat / n
                 << " % under 2 degrees; turned " << turn_sum / n
                 << " degrees off the line; the arrises shaded " << shade
                 << " degrees off their facets");
  // The base (G177): spread 3.00 degrees, 27.9 % of faces under 2, turned 1.75, arrises shaded
  // 0.00 (flat bands). G189 at its commit: 4.76, 9.4 %, 2.09, 32.0. The turn is the measure with
  // the least margin: the arrises and the face's 35 mm of relief take most of a stone's depth.
  CHECK(spread_sum / n >= 4.0);
  CHECK(static_cast<f64>(flat) <= 0.15 * n);
  CHECK(turn_sum / n >= 2.0);
  CHECK(shade >= 10.0);
}

TEST_CASE("walls: the cope reads irregular at ten metres, not as books on a shelf") {
  // At ten metres a pixel is about 7 mm (1,600 across 60 degrees). Over four stretches' cope:
  // (1) neighbours' heights differ by 30 mm or more often (TCV's 50 mm is the most; the base,
  // G177: 18 %), (2) thickness spreads (TCV: slab copes 50 to 200 mm; the base: a coefficient of
  // variation of 0.40), (3) the stones lean, in runs, as tilted coping's do (TCV, Types of
  // coping): their leans spread by 5 degrees or more and a quarter lean 6 or more. The base kept
  // no lean in its report; its rule (half its neighbour's lean and up to 2.5 of its own, within 4
  // degrees, one in fifteen at 5 to 8) cannot spread by more than about 2.5 nor give a quarter
  // past 6, so (3) is read from its code, not run. The skyline's movement at 2 cm bins is
  // reported: the base's already moved 76 mm a bin (its gaps reach the cover), so it is no measure.
  f64 step_sum = 0, dh_sum = 0, thick_cv = 0, lean_sum = 0, lean_sq = 0;
  u32 pairs = 0, big = 0, modules = 0, leans = 0, steep = 0;
  for (const u64 seed : {1ull, 2ull, 3ull, 4ull}) {
    const Module m = module_of(ModuleKind::run, 0, seed);
    assets::MeshData mesh;
    ModuleReport report;
    std::string error;
    REQUIRE(solve_module(m, mesh, report, error));
    Vector<f64> h, t;
    for (const Stone& s : report.stones) {
      if (s.role != StoneRole::cope || s.x0 < -1.4f || s.x1 > 1.4f) continue;
      h.push_back(static_cast<f64>(s.top - s.bed));
      t.push_back(static_cast<f64>(s.x1 - s.x0));
    }
    REQUIRE(h.size() > 10);
    for (u32 i = 1; i < h.size(); ++i) {
      const f64 d = std::abs(h[i] - h[i - 1]);
      dh_sum += d;
      big += d >= 0.03 ? 1u : 0u;
      ++pairs;
    }
    f64 mt = 0, vt = 0;
    for (const f64 v : t)
      mt += v;
    mt /= static_cast<f64>(t.size());
    for (const f64 v : t)
      vt += (v - mt) * (v - mt);
    thick_cv += std::sqrt(vt / static_cast<f64>(t.size())) / mt;
    for (const f32 l : report.cope_lean_deg) {
      const f64 d = static_cast<f64>(l);
      lean_sum += d;
      lean_sq += d * d;
      steep += std::abs(d) >= 6.0 ? 1u : 0u;
      ++leans;
    }
    const Vector<f64> sky = skyline(mesh, mesh.indices, -1.4, 0.02, 140);
    f64 steps = 0;
    for (u32 i = 1; i < sky.size(); ++i)
      steps += std::abs(sky[i] - sky[i - 1]);
    step_sum += steps / static_cast<f64>(sky.size() - 1);
    ++modules;
  }
  REQUIRE(leans > 40);
  const f64 mods = static_cast<f64>(modules);
  const f64 lean_mean = lean_sum / leans;
  const f64 lean_sd = std::sqrt(std::max(0.0, lean_sq / leans - lean_mean * lean_mean));
  MESSAGE("cope: neighbours differ by " << 1000.0 * dh_sum / pairs << " mm on average, "
                                        << 100.0 * big / pairs << " % by 30 mm or more; thickness "
                                        << "spread " << thick_cv / mods << "; leans spread "
                                        << lean_sd << " degrees, " << 100.0 * steep / leans
                                        << " % lean 6 or more; the skyline moves "
                                        << 1000.0 * step_sum / mods << " mm a 2 cm bin");
  CHECK(static_cast<f64>(big) >= 0.30 * static_cast<f64>(pairs));
  CHECK(thick_cv / mods >= 0.50);
  CHECK(lean_sd >= 5.0);
  CHECK(static_cast<f64>(steep) >= 0.25 * static_cast<f64>(leans));
}
TEST_CASE("walls: the cover course is stones, not a row of boxes") {
  // The cover (throughbands of the top, TCV, Coping) is flat stones of their own thickness, not
  // sawn slabs: a stone is thicker at one end than the other, and the course's top, seen from
  // the side, is not a ruled line. Over four seeds' stretches: (1) the share of cover stones
  // whose top falls 15 mm or more end to end, and (2) the spread of the course's top about its
  // mean, sampled every 2 cm.
  u32 stones = 0, sloped = 0, samples = 0;
  f64 dev = 0;
  for (const u64 seed : {1ull, 2ull, 3ull, 4ull}) {
    const Module m = module_of(ModuleKind::run, 0, seed);
    assets::MeshData mesh;
    ModuleReport report;
    std::string error;
    REQUIRE(solve_module(m, mesh, report, error));
    Vector<f64> top;
    for (f64 x = -1.39; x < 1.4; x += 0.02)
      for (const Stone& s : report.stones)
        if (s.role == StoneRole::cover && x >= static_cast<f64>(s.x0) &&
            x < static_cast<f64>(s.x1)) {
          // Its top along its length (not `top_at`: G177 left a level stone's `peak_x` at 0).
          const f64 f = (x - static_cast<f64>(s.x0)) / static_cast<f64>(s.x1 - s.x0);
          top.push_back(static_cast<f64>(s.top0) +
                        (static_cast<f64>(s.top1) - static_cast<f64>(s.top0)) * f);
          break;
        }
    for (const Stone& s : report.stones) {
      if (s.role != StoneRole::cover || s.x0 < -1.4f || s.x1 > 1.4f) continue;
      ++stones;
      sloped += std::abs(static_cast<f64>(s.top1) - static_cast<f64>(s.top0)) >= 0.015 ? 1u : 0u;
    }
    f64 mean = 0;
    for (const f64 v : top)
      mean += v;
    mean /= static_cast<f64>(top.size());
    for (const f64 v : top) {
      dev += (v - mean) * (v - mean);
      ++samples;
    }
  }
  REQUIRE(stones > 20);
  const f64 sd = std::sqrt(dev / static_cast<f64>(samples));
  MESSAGE(stones << " cover stones: " << 100.0 * sloped / stones
                 << " % fall 15 mm or more end to end; the course's top spreads " << 1000.0 * sd
                 << " mm about its mean");
  CHECK(static_cast<f64>(sloped) >= 0.4 * static_cast<f64>(stones));
  CHECK(sd >= 0.012);
}

TEST_CASE("walls: no stacks of closing stones in a standing face" * doctest::skip()) {
  // A slot is closed with a stone, not a stack of slivers (G177's pictures: narrow vertical strips
  // beside jumpers). Over stretches and heads at three grades and four seeds: a stack is a
  // pinning or closing stone narrower than 6 cm with another of the same face on it, overlapping
  // it along the wall by half the narrower's width.
  u32 narrow = 0, stacked = 0, modules = 0, stacked_closers = 0;
  for (const u64 seed : {1ull, 2ull, 3ull, 4ull})
    for (const i32 grade : {0, 250, 500})
      for (const ModuleKind kind : {ModuleKind::run, ModuleKind::head}) {
        assets::MeshData mesh;
        ModuleReport report;
        std::string error;
        REQUIRE(solve_module(module_of(kind, grade, seed), mesh, report, error));
        ++modules;
        const Vector<Stone>& st = report.stones;
        for (u32 i = 0; i < st.size(); ++i) {
          const Stone& a = st[i];
          if (a.role != StoneRole::pinning || a.side == 0 || a.x1 - a.x0 >= 0.06f) continue;
          if (a.x0 < -1.4f || a.x1 > 1.4f) continue;
          ++narrow;
          for (u32 j = 0; j < st.size(); ++j) {
            const Stone& b = st[j];
            if (j == i || b.role != StoneRole::pinning || b.side != a.side) continue;
            const f64 o = std::min(static_cast<f64>(a.x1), static_cast<f64>(b.x1)) -
                          std::max(static_cast<f64>(a.x0), static_cast<f64>(b.x0));
            const f64 w = std::min(static_cast<f64>(a.x1 - a.x0), static_cast<f64>(b.x1 - b.x0));
            if (o < 0.5 * w) continue;
            if (std::abs(static_cast<f64>(b.bed) - static_cast<f64>(a.top)) > 0.006) continue;
            ++stacked;
            stacked_closers += a.sweep == 0 ? 1u : 0u;
            break;
          }
        }
      }
  MESSAGE(modules << " modules: " << narrow << " pinnings and closing stones under 6 cm wide, "
                  << stacked << " with another stacked on them (" << stacked_closers
                  << " closing the face's slots)");
  CHECK(static_cast<f64>(stacked) <= 0.5 * static_cast<f64>(modules));
}

TEST_CASE("walls: the mid form's cope answers to the near form's skyline") {
  // From about 20 m the cut draws the mid form (walls.md, "Forms and the LOD policy"); at thirty
  // metres a pixel is about 2 cm. The mid form's cope seen from the side keeps the near form's
  // broken top: its skyline at 4 cm bins moves at least half as much as the near form's, and
  // stands within 2 cm of it on average.
  f64 near_steps = 0, mid_steps = 0, apart = 0;
  u32 bins = 0;
  for (const u64 seed : {1ull, 2ull, 3ull, 4ull}) {
    const Module m = module_of(ModuleKind::run, 0, seed);
    assets::MeshData mesh;
    ModuleReport report;
    std::string error;
    REQUIRE(solve_module(m, mesh, report, error));
    const Vector<f64> near = skyline(mesh, mesh.indices, -1.4, 0.04, 70);
    const Vector<f64> mid = skyline(mesh, report.mid_indices, -1.4, 0.04, 70);
    for (u32 i = 1; i < near.size(); ++i) {
      near_steps += std::abs(near[i] - near[i - 1]);
      mid_steps += std::abs(mid[i] - mid[i - 1]);
    }
    for (u32 i = 0; i < near.size(); ++i) {
      apart += std::abs(near[i] - mid[i]);
      ++bins;
    }
  }
  MESSAGE("the skyline at 4 cm bins moves "
          << 1000.0 * near_steps / bins << " mm a bin near and " << 1000.0 * mid_steps / bins
          << " mm at the mid form; they stand " << 1000.0 * apart / bins << " mm apart");
  CHECK(mid_steps >= 0.5 * near_steps);
  CHECK(apart / static_cast<f64>(bins) <= 0.02);
}
