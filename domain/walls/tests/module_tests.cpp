// The module solver against the ranges docs/experiments/dry-stone-walls.md pins.

#include <domain/walls/walls.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::walls;

namespace {

bool in_face(const Stone& s) {
  return s.role == StoneRole::face || s.role == StoneRole::footing ||
         s.role == StoneRole::through || s.role == StoneRole::tie;
}

// The lowest face, through or tie stone of the face covering x whose bed is at or above `level`
// (the stone laid on what is below at x). Its bed and the level are read at the joint `xj` (its
// bed clamped to its own extent), not at x: a bed laid along a sloping top differs by the slope
// over the 4 cm between them (G177; for level stones the measure is G145's).
const Stone* on_top(const Vector<Stone>& stones, i8 side, f64 x, f64 xj, f64 level) {
  const Stone* best = nullptr;
  for (const Stone& s : stones) {
    if (!in_face(s)) continue;
    if (s.side != 0 && s.side != side) continue;
    if (x < static_cast<f64>(s.x0) || x > static_cast<f64>(s.x1)) continue;
    if (bed_at(s, xj) < level - 0.006) continue;
    if (best == nullptr || bed_at(s, xj) < bed_at(*best, xj)) best = &s;
  }
  return best;
}

// How many courses above `s` the joint at its end runs on: at each level the stones laid either
// side of the joint (4 cm off it) are different stones meeting within 3 cm of it.
u32 runs_above(const Vector<Stone>& stones, const Stone& s) {
  const f64 x = static_cast<f64>(s.x1), tol = 0.03;
  f64 level = top_at(s, x);
  u32 levels = 0;
  for (u32 k = 0; k < 3; ++k) {
    const Stone* l = on_top(stones, s.side, x - 0.04, x, level);
    const Stone* r = on_top(stones, s.side, x + 0.04, x, level);
    if (l == nullptr || r == nullptr || l == r) break;
    if (std::abs(static_cast<f64>(l->x1) - x) >= tol &&
        std::abs(static_cast<f64>(r->x0) - x) >= tol)
      break;
    ++levels;
    level = std::min(top_at(*l, x), top_at(*r, x));
  }
  return levels;
}
Module module_of(ModuleKind kind, i32 grade, u64 seed) {
  Module m;
  m.kind = kind;
  m.grade_permille = grade;
  m.seed = seed;
  return m;
}

}  // namespace

TEST_CASE("walls: vertical joints are crossed, one on two and two on one") {
  // TCV, Building: "one on two and two on one"; a running joint is two or more uncrossed joints.
  // For every joint at a face stone's end inside the module, the stone above it may end there too
  // (two courses) but the one above that may not.
  u32 joints = 0, two = 0, more = 0, four = 0, crossed = 0, left = 0;
  for (const i32 grade : {0, 250, 500})
    for (const u64 seed : {1ull, 2ull, 3ull, 4ull, 5ull, 6ull}) {
      for (const ModuleKind kind : {ModuleKind::run, ModuleKind::head}) {
        assets::MeshData mesh;
        ModuleReport report;
        std::string error;
        REQUIRE(solve_module(module_of(kind, grade, seed), mesh, report, error));
        crossed += report.seams_crossed;
        left += report.seams_left;
        for (const Stone& s : report.stones) {
          if (s.role != StoneRole::face && s.role != StoneRole::footing) continue;
          const f64 x = static_cast<f64>(s.x1);
          if (x > 1.4 || x < -1.4) continue;  // the module's own ends meet the next module
          ++joints;
          const u32 levels = runs_above(report.stones, s);
          two += levels >= 1 ? 1u : 0u;
          more += levels >= 2 ? 1u : 0u;
          four += levels >= 3 ? 1u : 0u;
        }
      }
    }
  MESSAGE(joints << " joints, " << two << " through two courses, " << more << " through three, "
                 << four << " through four; the solver crossed " << crossed << " seams and left "
                 << left);
  CHECK(joints > 500);
  // The solver keeps every end off the joint below and crosses what seams it can before the faces
  // are drawn (walls.md, "The module"); what is left is where a course ends against a step too
  // tall to bring level in one stone, beside a through-stone's packing, or a footing stepping up
  // a slope. Held over 36 modules (six seeds, three grades, a stretch and a head) at what the
  // solver reaches, stated as such and not from a source (TCV: none through three): G145c
  // measured 70 of 5,465 joints through three courses (1.28 %) and 34 through four (0.62 %).
  // G145b's bounds (2 % and 0.8 %) held on its own kept draws only; on the version's draws they
  // failed at 2.09 %. G177's layout (a stone that starts on another level beside a jumper starts
  // off the joint below) measures 46 of 4,560 through three (1.01 %) and 19 through four
  // (0.42 %); G178 holds that, tighter than G145c's 1.4 % and 0.7 %. What stops zero, of the 46:
  // 13 meet a through-stone or a tie (neither can move), 4 lie at the module's own ends, 3 beside
  // a jumper, 2 beside a lump, 2 at the footing, and 21 where a stone laid over a pack of pinnings
  // ends within 3 cm of a joint two stones down, which the end's check, reading the skyline just
  // below, does not see (walls.md, "Not yet").
  CHECK(static_cast<f64>(more) < 0.011 * static_cast<f64>(joints));
  CHECK(static_cast<f64>(four) < 0.005 * static_cast<f64>(joints));
}

TEST_CASE("walls: the face is not coursed boxes") {
  // G177 (walls.md, "The module"; dry-stone-walls.md, "A face that is not coursed"): where a wall
  // is not strictly coursed its stones differ in height inside a layer, jumpers rise through two,
  // tops are not level and a rounded top is bedded with small stones either side (TCV, Building;
  // Specialist building techniques). No source gives a share, so the bounds are design values,
  // each run against the base (G145c, k_wall_version 3) to see it fail: there every top was
  // level (share 0: a stone carried one top), no face was a pentagon (0), heights within a sweep
  // spread by a coefficient of variation of 0.140 and 1.11 % of stones stood 1.6 times their
  // sweep's median or more (2,251 face stones in 219 sweeps, measured on the base build).
  // Measured over six seeds, three grades, the standing face stones of a stretch.
  u32 stones = 0, sloped = 0, lumps = 0, tall = 0, groups = 0;
  f64 cv_sum = 0;
  for (const i32 grade : {0, 250, 500})
    for (const u64 seed : {1ull, 2ull, 3ull, 4ull, 5ull, 6ull}) {
      assets::MeshData mesh;
      ModuleReport report;
      std::string error;
      REQUIRE(solve_module(module_of(ModuleKind::run, grade, seed), mesh, report, error));
      for (const i8 side : {i8{1}, i8{-1}})
        for (u16 sweep = 1; sweep < 48; ++sweep) {
          Vector<f64> h;
          for (const Stone& s : report.stones) {
            if (s.role != StoneRole::face || s.side != side || s.sweep != sweep) continue;
            if (s.x0 < -1.4f || s.x1 > 1.4f) continue;
            const f64 xm = 0.5 * static_cast<f64>(s.x0 + s.x1);
            h.push_back(top_at(s, xm) - bed_at(s, xm));
            ++stones;
            sloped +=
                std::abs(static_cast<f64>(s.top1) - static_cast<f64>(s.top0)) >= 0.01 || is_lump(s)
                    ? 1u
                    : 0u;
            lumps += is_lump(s) ? 1u : 0u;
          }
          if (h.size() < 3) continue;
          Vector<f64> sorted = h;
          std::sort(sorted.begin(), sorted.end());
          const f64 median = sorted[sorted.size() / 2];
          f64 mean = 0;
          for (const f64 v : h)
            mean += v;
          mean /= static_cast<f64>(h.size());
          f64 var = 0;
          for (const f64 v : h) {
            var += (v - mean) * (v - mean);
            tall += v >= 1.6 * median ? 1u : 0u;
          }
          cv_sum += std::sqrt(var / static_cast<f64>(h.size())) / mean;
          ++groups;
        }
    }
  REQUIRE(stones > 500);
  REQUIRE(groups > 50);
  const f64 n = static_cast<f64>(stones);
  MESSAGE(stones << " face stones in " << groups << " sweeps: tops not level " << 100.0 * sloped / n
                 << " %, lumps " << 100.0 * lumps / n
                 << " %, 1.6 times the sweep's median or taller " << 100.0 * tall / n
                 << " %; heights within a sweep spread by " << cv_sum / groups);
  CHECK(static_cast<f64>(sloped) >= 0.5 * n);
  CHECK(static_cast<f64>(lumps) >= 0.08 * n);
  CHECK(static_cast<f64>(tall) >= 0.08 * n);
  CHECK(cv_sum / static_cast<f64>(groups) >= 0.25);
}

TEST_CASE("walls: the height, batter, through-stones and cope are inside the pinned ranges") {
  for (const i32 grade : {0, 375}) {
    const Module m = module_of(ModuleKind::run, grade, 7);
    std::string why;
    REQUIRE(module_in_range(m, why));
    const f64 b = batter(m);
    CHECK(b >= 0.1);        // 1:10
    CHECK(b <= 1.0 / 6.0);  // 1:6
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(solve_module(m, mesh, report, why));
    const f64 g = static_cast<f64>(grade) / 1000.0;
    const f64 H = static_cast<f64>(m.height_cm) / 100.0;
    f64 cope_sum = 0, top_lo = 1e9, top_hi = -1e9;
    u32 copes = 0;
    Vector<f64> throughs;
    for (const Stone& s : report.stones) {
      const f64 xm = 0.5 * static_cast<f64>(s.x0 + s.x1);
      if (s.role == StoneRole::cope) {
        cope_sum += static_cast<f64>(s.top - s.bed);
        ++copes;
        const f64 above_ground = static_cast<f64>(s.top) - g * xm;
        top_lo = std::min(top_lo, above_ground);
        top_hi = std::max(top_hi, above_ground);
        // On edge: taller than it is thick along the wall.
        CHECK(s.top - s.bed > s.x1 - s.x0);
      }
      if (s.role == StoneRole::through) {
        const f64 bed = static_cast<f64>(s.bed) - g * xm;
        CHECK(bed >= 0.45);
        CHECK(bed <= 0.75);
        // Through the whole wall and proud of both faces by 25 to 150 mm (TCV).
        const f64 half = 0.5 *
                         (static_cast<f64>(m.base_width_cm) -
                          (static_cast<f64>(m.base_width_cm) - static_cast<f64>(m.top_width_cm)) *
                              std::min(bed / body_height(m), 1.0)) /
                         100.0;
        CHECK(static_cast<f64>(s.z1) - half >= 0.025);
        CHECK(static_cast<f64>(s.z1) - half <= 0.15);
        throughs.push_back(xm);
      }
    }
    REQUIRE(copes > 10);
    const f64 mean_cope = cope_sum / static_cast<f64>(copes);
    INFO("mean cope " << mean_cope << ", top " << top_lo << ".." << top_hi);
    CHECK(mean_cope >= 0.25 * 0.97);
    CHECK(mean_cope <= 0.30 * 1.03);
    CHECK(top_lo >= H - 0.06);
    CHECK(top_hi <= H + 0.07);
    // A row at no more than 1 m centres (TCV), across the module.
    REQUIRE(throughs.size() >= 3);
    std::sort(throughs.begin(), throughs.end());
    for (u32 i = 1; i < throughs.size(); ++i)
      CHECK(throughs[i] - throughs[i - 1] <= 1.0);
    CHECK(throughs.front() + 1.5 <= 0.75);
    CHECK(1.5 - throughs.back() <= 0.75);
  }
}

TEST_CASE(
    "walls: courses are graded and level-bedded; a head is flush and tied; a fallen one lies") {
  std::string why;
  {
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(solve_module(module_of(ModuleKind::run, 0, 3), mesh, report, why));
    f64 low = 0, high = 0;
    u32 nl = 0, nh = 0;
    for (const Stone& s : report.stones) {
      if (s.role != StoneRole::face) continue;
      // A course stone to 190 mm, a jumper through two to 280 mm (dry-stone-walls.md); G177 made
      // jumpers the rule rather than a stone brought up against a step, so the bound is the
      // jumper's (it was 0.19 for every face stone of this one module).
      const f64 xm = 0.5 * static_cast<f64>(s.x0 + s.x1);
      const f64 h = top_at(s, xm) - bed_at(s, xm);
      CHECK(h <= 0.28 + 1e-6);
      if (s.bed < 0.3f) {
        low += h;
        ++nl;
      } else if (s.bed > 0.7f) {
        high += h;
        ++nh;
      }
    }
    REQUIRE(nl > 0);
    REQUIRE(nh > 0);
    CHECK(low / nl > 1.4 * (high / nh));  // large at the foot, small under the cover
    CHECK(report.near_triangles > report.mid_triangles * 3);
    CHECK(report.mid_triangles > report.far_triangles * 4);
  }
  {
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(solve_module(module_of(ModuleKind::head, 125, 3), mesh, report, why));
    u32 ties = 0;
    for (const Stone& s : report.stones) {
      if (s.role == StoneRole::tie) ++ties;
      if (in_face(s) || s.role == StoneRole::cope || s.role == StoneRole::cover)
        CHECK(s.x0 >= -1.5f - 1e-4f);
    }
    CHECK(ties >= 2);
  }
  {
    assets::MeshData mesh;
    ModuleReport report;
    REQUIRE(solve_module(module_of(ModuleKind::fallen, 0, 3), mesh, report, why));
    u32 rubble[2] = {0, 0};
    for (const Stone& s : report.stones) {
      if (s.role == StoneRole::rubble)
        ++rubble[s.side > 0 ? 0 : 1];
      else if (in_face(s))
        CHECK(s.top < 0.40f);
      CHECK(s.role != StoneRole::cope);
    }
    CHECK(rubble[0] > 20);
    CHECK(rubble[1] > 20);
  }
  {
    Module m = module_of(ModuleKind::run, 0, 1);
    m.top_width_cm = 30;  // a batter of 1:4.8, steeper than the sources' 1:6
    CHECK_FALSE(module_in_range(m, why));
  }
}

TEST_CASE("walls: a module's container carries the stones, the mid form and the outline") {
  // The mesh generator `wall` as a host calls it: the recipe's bytes to a container whose DAG
  // ends in the authored levels, and whose image is the stone palette.
  for (const ModuleKind kind : {ModuleKind::run, ModuleKind::strip}) {
    Module m = module_of(kind, kind == ModuleKind::strip ? 0 : 250, 5);
    if (kind == ModuleKind::strip) m.length_cm = 100;
    const std::string text = canonical_json(m);
    const auto* desc = assets::MeshGeneratorRegistry::global().find("wall");
    REQUIRE(desc != nullptr);
    u64 hash = 0;
    std::string error;
    REQUIRE(
        desc->source_hash({reinterpret_cast<const u8*>(text.data()), text.size()}, hash, error));
    CHECK(hash == source_hash(m));
    geometry::ClusterFileData data;
    const geometry::ClusterLodOptions lod;
    REQUIRE(desc->build({reinterpret_cast<const u8*>(text.data()), text.size()}, {}, lod, 0, data,
                        error));
    CHECK(data.generator == "wall");
    CHECK(data.materials.size() == 1);
    REQUIRE(data.images.size() == 1);
    CHECK(data.images[0].bytes.size() > 1000);
    CHECK(data.mesh.mesh.clusters.size() > (kind == ModuleKind::strip ? 0u : 20u));
  }
}
