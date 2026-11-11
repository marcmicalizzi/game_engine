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
// (the stone laid on what is below at x).
const Stone* on_top(const Vector<Stone>& stones, i8 side, f64 x, f64 level) {
  const Stone* best = nullptr;
  for (const Stone& s : stones) {
    if (!in_face(s)) continue;
    if (s.side != 0 && s.side != side) continue;
    if (static_cast<f64>(s.bed) < level - 0.006) continue;
    if (x < static_cast<f64>(s.x0) || x > static_cast<f64>(s.x1)) continue;
    if (best == nullptr || s.bed < best->bed) best = &s;
  }
  return best;
}

// How many courses above `s` the joint at its end runs on: at each level the stones laid either
// side of the joint (4 cm off it) are different stones meeting within 3 cm of it.
u32 runs_above(const Vector<Stone>& stones, const Stone& s) {
  const f64 x = static_cast<f64>(s.x1), tol = 0.03;
  f64 level = static_cast<f64>(s.top);
  u32 levels = 0;
  for (u32 k = 0; k < 3; ++k) {
    const Stone* l = on_top(stones, s.side, x - 0.04, level);
    const Stone* r = on_top(stones, s.side, x + 0.04, level);
    if (l == nullptr || r == nullptr || l == r) break;
    if (std::abs(static_cast<f64>(l->x1) - x) >= tol &&
        std::abs(static_cast<f64>(r->x0) - x) >= tol)
      break;
    ++levels;
    level = std::min(static_cast<f64>(l->top), static_cast<f64>(r->top));
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
  // failed at 2.09 %.
  CHECK(static_cast<f64>(more) < 0.014 * static_cast<f64>(joints));
  CHECK(static_cast<f64>(four) < 0.007 * static_cast<f64>(joints));
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
      const f64 h = static_cast<f64>(s.top - s.bed);
      CHECK(h <= 0.19);
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
