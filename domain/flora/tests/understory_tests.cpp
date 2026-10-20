// The understory — the shrub, the fern, the grass tuft and the herb — and the detail tier every
// flora kind declares (docs/subsystems/flora.md, "The understory", "Detail tiers").
#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/flora/understory.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

using namespace engine;

namespace {

f64 triangle_area(const assets::MeshData& mesh, u32 t) {
  const Vec3 a = mesh.positions[mesh.indices[t * 3 + 0]];
  const Vec3 b = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3 c = mesh.positions[mesh.indices[t * 3 + 2]];
  const Vec3 n = cross(b - a, c - a);
  return 0.5 * std::sqrt(static_cast<f64>(dot(n, n)));
}

// The mesh's every stream, its motion contract and its structural table, hashed in order (the
// trees' hash in species_tests.cpp).
u64 plant_hash(const assets::MeshData& mesh, const flora::Motion& motion) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  for (const assets::Primitive& p : mesh.primitives) {
    h = hash_combine(h, p.first_index);
    h = hash_combine(h, p.index_count);
    h = hash_combine(h, static_cast<u64>(static_cast<u32>(p.material)));
  }
  h = hash_combine(h, hash_bytes(motion.parts.data(),
                                 motion.parts.size() * sizeof(geometry::ClusterFileMotionPart)));
  h = hash_combine(h,
                   hash_bytes(motion.vertex_part.data(), motion.vertex_part.size() * sizeof(u32)));
  h = hash_combine(
      h, hash_bytes(motion.vertex_weight.data(), motion.vertex_weight.size() * sizeof(f32)));
  return hash_combine(h, hash_bytes(motion.structure.data(),
                                    motion.structure.size() * sizeof(flora::StructuralPart)));
}

// Valid, in budget, its primitives covering the index buffer in order and every material
// two-sided but a shrub's bark; the motion contract dense and parent-first, part 0 the root with
// no parent and no geometry, every other part reaching weight 0 at its root and 1 at its tip; the
// structural table one row a part, in order, no part load-bearing.
void check_plant(const assets::MeshData& mesh, const flora::Motion& motion,
                 const flora::TreeReport& r, u32 budget, bool bark) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  CHECK(mesh.indices.size() / 3 <= budget);
  CHECK(r.triangles == mesh.indices.size() / 3);
  CHECK(r.wood_triangles + r.leaf_triangles + r.fruit_triangles == r.triangles);
  u32 covered = 0;
  for (const assets::Primitive& prim : mesh.primitives) {
    CHECK(prim.first_index == covered);
    covered += prim.index_count;
    const bool two_sided = mesh.materials[static_cast<u32>(prim.material)].double_sided;
    CHECK(two_sided == !(bark && prim.material == 0));
  }
  CHECK(covered == mesh.indices.size());
  for (const Vec3& v : mesh.positions) {
    REQUIRE(std::isfinite(v.x));
    REQUIRE(std::isfinite(v.y));
    REQUIRE(std::isfinite(v.z));
  }
  u32 degenerate = 0;
  for (u32 t = 0; t < mesh.indices.size() / 3; ++t) {
    const u32 a = mesh.indices[t * 3];
    const u32 b = mesh.indices[t * 3 + 1];
    const u32 c = mesh.indices[t * 3 + 2];
    REQUIRE(a < mesh.positions.size());
    REQUIRE(b < mesh.positions.size());
    REQUIRE(c < mesh.positions.size());
    if (a == b || b == c || a == c || !(triangle_area(mesh, t) > 1e-12)) ++degenerate;
  }
  CHECK_MESSAGE(degenerate == 0, degenerate << " degenerate triangles");

  REQUIRE(motion.vertex_part.size() == mesh.positions.size());
  REQUIRE(motion.vertex_weight.size() == mesh.positions.size());
  REQUIRE(motion.parts.size() == r.parts);
  REQUIRE(motion.structure.size() == motion.parts.size());
  CHECK(motion.parts[0].parent == geometry::k_motion_no_parent);
  Vector<f32> lo(motion.parts.size(), 2.0f);
  Vector<f32> hi(motion.parts.size(), -1.0f);
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const u32 part = motion.vertex_part[v];
    REQUIRE(part < motion.parts.size());
    lo[part] = std::min(lo[part], motion.vertex_weight[v]);
    hi[part] = std::max(hi[part], motion.vertex_weight[v]);
  }
  CHECK(hi[0] < 0.0f);  // the root has no geometry: nothing of the plant is the kept trunk
  for (u32 p = 1; p < motion.parts.size(); ++p) {
    CAPTURE(p);
    CHECK(motion.parts[p].parent < p);
    CHECK(lo[p] <= 0.01f);
    CHECK(hi[p] >= 0.99f);
  }
  for (u32 p = 0; p < motion.structure.size(); ++p) {
    const flora::StructuralPart& s = motion.structure[p];
    CHECK(s.part == p);
    CHECK(s.tip_radius <= s.base_radius);
    CHECK((s.flags & flora::k_load_bearing) == 0u);
  }
}

template <typename Params, typename Solve>
u64 check_species(const std::string& species, const Params& params, Solve solve,
                  std::initializer_list<u32> budgets, bool bark, u64 golden,
                  flora::TreeReport* out_report = nullptr) {
  CAPTURE(species);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(solve(params, mesh, &report, &error, &motion), error);
  MESSAGE(species << ": " << report.triangles << " triangles (wood " << report.wood_triangles
                  << ", leaves " << report.leaf_triangles << ", flowers " << report.fruit_triangles
                  << "), " << report.parts << " parts, " << report.sprays << " sprays of "
                  << report.cards_per_side << " a side, " << report.steps << " budget steps, "
                  << report.colliders << " colliders");
  CHECK(report.steps == 0);
  check_plant(mesh, motion, report, params.triangle_budget, bark);
  // Every rung holds and names the same parts with the same structure.
  for (const u32 budget : budgets) {
    CAPTURE(budget);
    Params q = params;
    q.triangle_budget = budget;
    assets::MeshData small;
    flora::Motion small_motion;
    flora::TreeReport small_report;
    REQUIRE_MESSAGE(solve(q, small, &small_report, &error, &small_motion), error);
    MESSAGE(species << " on " << budget << ": " << small_report.triangles << " triangles after "
                    << small_report.steps << " steps");
    CHECK(small_report.steps > 0);
    check_plant(small, small_motion, small_report, budget, bark);
    REQUIRE(small_motion.parts.size() == motion.parts.size());
    CHECK(std::memcmp(small_motion.parts.data(), motion.parts.data(),
                      motion.parts.size() * sizeof(flora::MotionPart)) == 0);
    CHECK(std::memcmp(small_motion.structure.data(), motion.structure.data(),
                      motion.structure.size() * sizeof(flora::StructuralPart)) == 0);
  }
  // One seed one plant, the same bytes on every toolchain; another seed another plant.
  assets::MeshData again;
  flora::Motion again_motion;
  REQUIRE(solve(params, again, nullptr, &error, &again_motion));
  const u64 got = plant_hash(again, again_motion);
  CHECK(got == plant_hash(mesh, motion));
  char hex[32];
  std::snprintf(hex, sizeof(hex), "0x%016llxull", static_cast<unsigned long long>(got));
  CHECK_MESSAGE(got == golden, species << " hashes to " << hex);
  Params other = params;
  other.seed = 2;
  assets::MeshData other_mesh;
  flora::Motion other_motion;
  REQUIRE(solve(other, other_mesh, nullptr, &error, &other_motion));
  CHECK(plant_hash(other_mesh, other_motion) != got);
  // The mesh is the same bytes whether the contract is asked for or not.
  assets::MeshData bare;
  REQUIRE(solve(params, bare, nullptr, &error, nullptr));
  REQUIRE(bare.positions.size() == mesh.positions.size());
  CHECK(std::memcmp(bare.positions.data(), mesh.positions.data(),
                    mesh.positions.size() * sizeof(Vec3)) == 0);
  if (out_report != nullptr) *out_report = report;
  return got;
}

bool read(const std::string& text, flora::FloraFile& file, std::string& error) {
  return flora::read_flora_file(text, file, error);
}

}  // namespace

TEST_CASE("understory: the shrub grows the allometric recursion from its stems") {
  flora::TreeReport report;
  const flora::Shrub hazel;
  check_species("shrub", hazel, flora::solve_shrub, {12000u, 4000u}, true, 0x011d85ebe0232489ull,
                &report);
  CHECK(report.parts >= 1u + hazel.stems);
  CHECK(report.merge_level == 0u);
  CHECK(std::fabs(report.card_length - hazel.leaf_length_m) < 1e-6f);
  MESSAGE("hazel: " << report.twigs << " twigs over " << report.orders << " orders, "
                    << report.leaf_cards << " cards over " << report.footprint_m2 << " m2, LAI "
                    << report.leaf_area_index);
  CHECK(report.leaf_area_index > 0.8f * hazel.leaf_area_index);
  CHECK(report.leaf_area_index < 1.2f * hazel.leaf_area_index);

  SUBCASE("the leaf is the species' at 1 m and at 5 m") {
    for (const f32 height : {1.0f, 5.0f}) {
      CAPTURE(height);
      flora::Shrub p;
      p.height_m = height;
      p.triangle_budget = 400000;
      assets::MeshData mesh;
      flora::TreeReport r;
      std::string error;
      REQUIRE_MESSAGE(flora::solve_shrub(p, mesh, &r, &error, nullptr), error);
      CHECK(r.merge_level == 0u);
      CHECK(std::fabs(r.card_length - p.leaf_length_m) < 1e-6f);
      MESSAGE(height << " m: " << r.twigs << " twigs over " << r.orders << " orders, "
                     << r.triangles << " triangles");
    }
  }

  SUBCASE("a stem collides when it is at least 1.25 cm thick at the ground") {
    flora::Motion motion;
    assets::MeshData mesh;
    flora::TreeReport r;
    std::string error;
    REQUIRE(flora::solve_shrub(hazel, mesh, &r, &error, &motion));
    u32 colliders = 0;
    for (u32 p = 1; p < motion.structure.size(); ++p) {
      const flora::StructuralPart& s = motion.structure[p];
      const bool stem = motion.parts[p].parent == 0u;
      const bool flagged = (s.flags & flora::k_collider) != 0u;
      CHECK(flagged == (stem && s.base_radius >= flora::k_shrub_collider_radius_m));
      if (flagged) ++colliders;
    }
    CHECK(colliders == r.colliders);
    CHECK(colliders > 0u);
    CHECK((motion.structure[1].flags & flora::k_collider) != 0u);  // the stoutest stem
    flora::Shrub dogwood;
    dogwood.stem_radius_m = 0.009f;
    REQUIRE(flora::solve_shrub(dogwood, mesh, &r, &error, &motion));
    CHECK(r.colliders == 0u);
    CHECK_FALSE(flora::shrub_collides(dogwood));
    CHECK(flora::shrub_collides(hazel));
  }

  SUBCASE("arching canes bow back down") {
    flora::Shrub bramble;
    bramble.upturn = -1.0f;
    bramble.stem_spread_deg = 60.0f;
    bramble.stem_radius_m = 0.006f;
    flora::Motion motion;
    assets::MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_shrub(bramble, mesh, nullptr, &error, &motion), error);
    CHECK(motion.parts[2].direction.y > 0.0f);
  }
}

TEST_CASE("understory: a fern is its fronds, a stipe and pinnae as cards") {
  flora::TreeReport report;
  const flora::Fern fern;
  check_species("fern", fern, flora::solve_fern, {1500u, 600u}, false, 0x33e78d83c2e1a749ull,
                &report);
  CHECK(report.parts == fern.fronds + 1u);
  CHECK(report.sprays == fern.fronds);
  CHECK(report.triangles == fern.fronds * 4u * report.cards_per_side);
  CHECK(report.cards_per_side > fern.pinnae);  // and the stipe's
  for (const u32 fronds : {1u, 7u, 40u}) {
    flora::Fern p;
    p.fronds = fronds;
    p.triangle_budget = 100000;
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport r;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_fern(p, mesh, &r, &error, &motion), error);
    CHECK(motion.parts.size() == fronds + 1u);
  }
}

TEST_CASE("understory: a tuft is its blades, each a part") {
  flora::TreeReport report;
  const flora::Tuft tuft;
  check_species("tuft", tuft, flora::solve_tuft, {}, false, 0x93296f455ec0624cull, &report);
  CHECK(report.parts == tuft.blades + 1u);
  CHECK(report.triangles == tuft.blades * (2u * tuft.segments - 1u));
  flora::Tuft crossed;
  crossed.crossed = true;
  crossed.blades = 400;
  crossed.triangle_budget = 50000;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport r;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_tuft(crossed, mesh, &r, &error, &motion), error);
  CHECK(motion.parts.size() == 401u);
  CHECK(r.triangles == 400u * 2u * (2u * crossed.segments - 1u));
  crossed.triangle_budget = 1500;  // the floor holds the most blades at two segments
  REQUIRE_MESSAGE(flora::solve_tuft(crossed, mesh, &r, &error, &motion), error);
  CHECK(r.triangles <= 1500u);
}

TEST_CASE("understory: a herb is a stem, its leaves and its flowers") {
  flora::TreeReport report;
  const flora::Herb daisy;
  check_species("herb", daisy, flora::solve_herb, {}, false, 0x0ec557bd6cdc6f38ull, &report);
  CHECK(report.parts == 2u + daisy.leaves + daisy.flowers);
  CHECK(report.fruit_triangles == daisy.flowers * daisy.petals * 3u);
  flora::Herb spike;
  spike.flowers = 40;
  spike.petals = 40;
  spike.leaves = 40;
  spike.spike = 0.5f;
  spike.triangle_budget = 1000;  // the floor: the petals halved, never a flower dropped
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport r;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_herb(spike, mesh, &r, &error, &motion), error);
  CHECK(r.triangles <= 1000u);
  CHECK(motion.parts.size() == 2u + 40u + 40u);
  flora::Herb bare;
  bare.flowers = 0;
  REQUIRE(flora::solve_herb(bare, mesh, &r, &error, &motion));
  CHECK(mesh.materials.size() == 1u);
  CHECK(r.fruit_triangles == 0u);
}

TEST_CASE(
    "understory: every kind declares its detail tier, and what may be dropped never collides") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(read(R"({"species": "shrub"})", file, error));
  CHECK(flora::flora_detail_tier(file) == flora::DetailTier::essential);
  CHECK(flora::flora_collides(file));
  CHECK_FALSE(read(R"({"species": "shrub", "shrub": {"detail_tier": "rich"}})", file, error));
  CHECK(error.find("engine.flora.Shrub.detail_tier") != std::string::npos);
  MESSAGE(error);
  REQUIRE_MESSAGE(read(R"({"species": "shrub", "shrub": {"stem_radius_m": 0.009,
                         "detail_tier": "rich"}})",
                       file, error),
                  error);
  CHECK_FALSE(flora::flora_collides(file));
  CHECK(flora::flora_detail_tier(file) == flora::DetailTier::rich);
  for (const char* species : {"fern", "tuft", "herb"}) {
    CAPTURE(species);
    REQUIRE(read(std::string("{\"species\": \"") + species + "\"}", file, error));
    CHECK(flora::flora_detail_tier(file) == flora::DetailTier::rich);
    CHECK_FALSE(flora::flora_collides(file));
  }
  for (const char* species : {"date_palm", "broadleaf", "conifer", "ancient"}) {
    CAPTURE(species);
    REQUIRE(read(std::string("{\"species\": \"") + species + "\"}", file, error));
    CHECK(flora::flora_detail_tier(file) == flora::DetailTier::essential);
    CHECK(flora::flora_collides(file));
  }
  // The tier is in the container's generator section, which `engine-content info` prints.
  REQUIRE(read(R"({"species": "fern"})", file, error));
  geometry::ClusterFileData container;
  REQUIRE_MESSAGE(flora::build_flora_container(file, geometry::ClusterLodOptions{}, 0, container,
                                               nullptr, error),
                  error);
  CHECK(container.generator_kind == "fern");
  CHECK(container.generator_params.find("\"detail_tier\":\"rich\"") != std::string::npos);
  CHECK(container.motion_parts.size() == file.fern.fronds + 1u);
  CHECK(container.structural_parts.size() == container.motion_parts.size());
}

TEST_CASE("understory: the parts sample their roles") {
  CHECK(flora::flora_role("shrub", 0) == "shrub/bark");
  CHECK(flora::flora_role("shrub", 1) == "shrub/leaf");
  CHECK(flora::flora_role("fern", 0) == "fern/frond");
  CHECK(flora::flora_role("fern", 1).empty());
  CHECK(flora::flora_role("tuft", 0) == "grass/blade");
  CHECK(flora::flora_role("herb", 0) == "herb/leaf");
  CHECK(flora::flora_role("herb", 1) == "herb/flower");
}

TEST_CASE("understory: a parameter out of range is refused by name") {
  std::string error;
  flora::Shrub shrub;
  shrub.stems = 41;
  CHECK_FALSE(flora::shrub_in_range(shrub, error));
  CHECK(error.find("engine.flora.Shrub.stems") != std::string::npos);
  flora::Fern fern;
  fern.pinnae = 2;
  CHECK_FALSE(flora::fern_in_range(fern, error));
  CHECK(error.find("engine.flora.Fern.pinnae") != std::string::npos);
  flora::Tuft tuft;
  tuft.blade_width_m = 0.05f;
  CHECK_FALSE(flora::tuft_in_range(tuft, error));
  CHECK(error.find("engine.flora.Tuft.blade_width_m") != std::string::npos);
  flora::Herb herb;
  herb.flower_blue = 1.5f;
  CHECK_FALSE(flora::herb_in_range(herb, error));
  CHECK(error.find("engine.flora.Herb.flower_blue") != std::string::npos);
}
