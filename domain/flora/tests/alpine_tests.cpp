#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/alpine.h>
#include <domain/flora/flora.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/flora_species.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;
namespace {
u64 mesh_hash(const assets::MeshData& mesh) {
  return hash_combine(hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3)),
                      hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
}
bool solve(const char* species, const flora::Alpine& p, assets::MeshData& mesh,
           flora::Motion& motion, flora::TreeReport& report, flora::TreeLevels* levels = nullptr) {
  std::string error;
  const bool ok = flora::solve_alpine(species, p, mesh, motion, &report, error, levels);
  INFO(error);
  CHECK(ok);
  return ok;
}
}  // namespace

TEST_CASE("alpine: botanical scale, distinct forms and explicit budgets") {
  u64 previous = 0;
  for (const auto& profile : flora::alpine_profiles()) {
    const auto& species = geometry::flora_species_info(profile.species);
    assets::MeshData mesh, repeat;
    flora::Motion motion, again;
    flora::TreeReport report, second;
    flora::TreeLevels levels;
    REQUIRE(solve(species.name.data(), {}, mesh, motion, report, &levels));
    CHECK(report.triangles == mesh.indices.size() / 3);
    CHECK(report.triangles <= species.triangle_budget);
    CHECK(report.triangles > 0);
    CHECK(report.parts == report.shoots + 1);
    CHECK(report.card_length == profile.leaf_length_m);
    CHECK(mesh_hash(mesh) != previous);
    previous = mesh_hash(mesh);
    REQUIRE(solve(species.name.data(), {}, repeat, again, second, &levels));
    CHECK(mesh_hash(mesh) == mesh_hash(repeat));
    CHECK(motion.vertex_part.size() == mesh.positions.size());
    CHECK(motion.structure.size() == motion.parts.size());
    for (u32 i = 0; i < motion.parts.size(); ++i) {
      CHECK(motion.structure[i].flags == 0u);
      if (i) CHECK(motion.parts[i].parent < i);
    }
    for (Vec3 v : mesh.positions) {
      CHECK(std::isfinite(v.x));
      CHECK(std::isfinite(v.y));
      CHECK(std::isfinite(v.z));
      CHECK(v.y < .06f);
    }
    CHECK(species.motion == geometry::FloraMotionKind::none);
    CHECK_FALSE(species.casters.traced);
    CHECK(species.casters.cascades == 1);
    REQUIRE(levels.levels.size() == 1);
    CHECK(levels.levels[0].indices.size() == 18);
    REQUIRE(levels.levels[0].jobs.size() == 3);
    CHECK(levels.levels[0].jobs[2].view.normal.y == 1);
  }
}

TEST_CASE("alpine: budget rungs preserve roots, parts and botanical leaves") {
  for (const auto& profile : flora::alpine_profiles()) {
    const auto name = geometry::flora_species_info(profile.species).name;
    flora::Alpine p;
    p.impostor_px = 0;
    p.triangle_budget = 200000;
    assets::MeshData near, coarse;
    flora::Motion motion, low;
    flora::TreeReport report, lower;
    REQUIRE(solve(name.data(), p, near, motion, report));
    p.triangle_budget = report.triangles - 1;
    REQUIRE(solve(name.data(), p, coarse, low, lower));
    CHECK(lower.steps == 1);
    CHECK(lower.triangles <= p.triangle_budget);
    CHECK(lower.leaf_cards == report.leaf_cards);
    CHECK(lower.card_length == report.card_length);
    REQUIRE(low.parts.size() == motion.parts.size());
    CHECK(std::memcmp(low.parts.data(), motion.parts.data(),
                      motion.parts.size() * sizeof(motion.parts[0])) == 0);
    CHECK(std::memcmp(low.structure.data(), motion.structure.data(),
                      motion.structure.size() * sizeof(motion.structure[0])) == 0);
    p.triangle_budget = 64;
    std::string error;
    CHECK_FALSE(flora::solve_alpine(name, p, coarse, low, &lower, error));
    CHECK(error.find("triangle_budget") != std::string::npos);
    CHECK(coarse.indices.empty());
  }
}

TEST_CASE("alpine: individual organs stay within botanical dimensions at either patch radius") {
  for (const auto& profile : flora::alpine_profiles()) {
    for (f32 radius : {.04f, .2f}) {
      flora::Alpine p;
      p.radius_m = radius;
      p.shoots = 8;
      p.impostor_px = 0;
      assets::MeshData mesh;
      flora::Motion motion;
      flora::TreeReport report;
      const auto name = geometry::flora_species_info(profile.species).name;
      REQUIRE(solve(name.data(), p, mesh, motion, report));
      const auto& primitive = mesh.primitives.back();
      const u32 first = mesh.indices[primitive.first_index];
      // Four blade intervals: paired edge vertices then one tip. Test the emitted geometry,
      // not the report's nominal size. Curvature may raise the tip; horizontal extent is fixed.
      REQUIRE((mesh.positions.size() - first) % 9 == 0);
      for (u32 i = first; i < mesh.positions.size(); i += 9) {
        const Vec3 base = (mesh.positions[i] + mesh.positions[i + 1]) * .5f;
        const Vec3 tip = mesh.positions[i + 8];
        const f32 dx = tip.x - base.x, dz = tip.z - base.z;
        const f32 extent = std::sqrt(dx * dx + dz * dz);
        const f32 minimum = profile.form == flora::AlpineForm::rosette ? .35f : 1.f;
        CHECK(extent >= profile.leaf_length_m * minimum * .899f);
        CHECK(extent <= profile.leaf_length_m * 1.001f);
        const Vec3 shoulder = mesh.positions[i + 5] - mesh.positions[i + 4];
        CHECK(length(shoulder) <= profile.leaf_width_m * 1.01f);
      }
      for (u32 i = 0; i < mesh.indices.size(); i += 3) {
        const Vec3 a = mesh.positions[mesh.indices[i]];
        const Vec3 b = mesh.positions[mesh.indices[i + 1]];
        const Vec3 c = mesh.positions[mesh.indices[i + 2]];
        CHECK(length(cross(b - a, c - a)) > 1e-12f);
      }
      for (Vec3 n : mesh.normals)
        CHECK(length(n) == doctest::Approx(1).epsilon(1e-5));
    }
  }
}

TEST_CASE("alpine: authored rock pockets root every plant on its support") {
  flora::Alpine p;
  p.shoots = 24;
  p.occupancy = .15f;
  p.slope_x = .3f;
  p.slope_z = -.2f;
  p.pockets.push_back({-.2f, .1f, .08f, .025f});
  p.pockets.push_back({.3f, -.1f, .15f, .04f});
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  REQUIRE(solve("saxifraga_paniculata", p, mesh, motion, report));
  CHECK(report.shoots >= 2);
  CHECK(report.shoots < p.shoots);
  u32 occupied[2]{};
  for (u32 i = 1; i < motion.parts.size(); ++i) {
    const auto foot = motion.parts[i].pivot;
    bool supported = false;
    for (u32 k = 0; k < p.pockets.size(); ++k) {
      const auto& pocket = p.pockets[k];
      const f32 dx = foot.x - pocket.x, dz = foot.z - pocket.z;
      if (dx * dx + dz * dz <= pocket.radius_m * pocket.radius_m + 1e-7f) {
        CHECK(foot.y ==
              doctest::Approx(pocket.y + p.slope_x * foot.x + p.slope_z * foot.z).epsilon(1e-6));
        supported = true;
        ++occupied[k];
      }
    }
    CHECK(supported);
  }
  CHECK(occupied[0] > 0);
  CHECK(occupied[1] > 0);
}

TEST_CASE("alpine: planting slope rotates leaves without stretching them") {
  for (const auto& profile : flora::alpine_profiles()) {
    const auto name = geometry::flora_species_info(profile.species).name;
    flora::Alpine p;
    p.shoots = 1;
    p.impostor_px = 0;
    assets::MeshData flat, slope;
    flora::Motion motion;
    flora::TreeReport report;
    REQUIRE(solve(name.data(), p, flat, motion, report));
    p.slope_x = 1;
    p.slope_z = -1;
    REQUIRE(solve(name.data(), p, slope, motion, report));
    REQUIRE(flat.positions.size() == slope.positions.size());
    const auto& leaves = flat.primitives.back();
    const u32 first = flat.indices[leaves.first_index];
    for (u32 blade = first; blade < flat.positions.size(); blade += 9) {
      for (u32 v = 0; v < 8; ++v) {
        const f32 a = length(flat.positions[blade + v + 1] - flat.positions[blade + v]);
        const f32 b = length(slope.positions[blade + v + 1] - slope.positions[blade + v]);
        CHECK(a == doctest::Approx(b).epsilon(2e-5));
      }
    }
  }
}

TEST_CASE("alpine: schema identity and invalid external input") {
  std::string error;
  flora::FloraFile file, back;
  REQUIRE(
      flora::read_flora_file(R"({"species":"silene_acaulis","alpine":{"seed":17}})", file, error));
  const auto canonical = flora::canonical_flora_json(file);
  REQUIRE(flora::read_flora_file(canonical, back, error));
  CHECK(flora::flora_source_hash(file) == flora::flora_source_hash(back));
  CHECK(canonical.find("shrub_growth") == std::string::npos);
  CHECK_FALSE(flora::flora_collides(file));
  CHECK(flora::flora_detail_tier(file) == flora::DetailTier::rich);
  const u64 key = flora::flora_source_hash(file);
  file.alpine.occupancy = .5f;
  CHECK(flora::flora_source_hash(file) != key);
  for (const f32 invalid :
       {-1.0f, 2.0f, std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity()}) {
    file.alpine.occupancy = invalid;
    CHECK_FALSE(flora::alpine_in_range(file.species, file.alpine, error));
  }
  CHECK_FALSE(flora::read_flora_file(R"({"species":"silene_acaulis","alpine":{"unknown":1}})", back,
                                     error));
  flora::Alpine p;
  p.pockets.resize(17);
  CHECK_FALSE(flora::alpine_in_range("silene_acaulis", p, error));
  p = {};
  p.impostor_px = 127;
  CHECK_FALSE(flora::alpine_in_range("silene_acaulis", p, error));
}

TEST_CASE("alpine: containers, far cuts and deterministic section hashes") {
  constexpr u64 pins[] = {15342167867693253831ull, 14353891800973444185ull,
                          15090448681600777050ull};
  u32 profile_index = 0;
  for (const auto& profile : flora::alpine_profiles()) {
    flora::FloraFile file;
    file.species = geometry::flora_species_info(profile.species).name;
    file.alpine.shoots = 8;
    file.alpine.impostor_px = 64;
    geometry::ClusterFileData data, back;
    std::string error;
    REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 65536, data, nullptr, error), error);
    test::TempDir temp("alpine_sections");
    const auto path = temp.file("alpine.clusters");
    REQUIRE(geometry::write_cluster_file(path, data, &error));
    REQUIRE(geometry::read_cluster_file(path, back, &error));
    CHECK(geometry::cluster_file_hash(data) == geometry::cluster_file_hash(back));
    CHECK(data.motion_vertices.size() == data.mesh.mesh.vertices.size());
    // Complete serialized section payloads, including baked images and authored distance levels.
    std::string bytes;
    REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
    geometry::ClusterFileHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    u64 folded = 0;
    for (u32 i = 0; i < header.section_count; ++i) {
      geometry::ClusterFileSection section;
      std::memcpy(&section, bytes.data() + sizeof(header) + i * sizeof(section), sizeof(section));
      folded = hash_combine(folded, hash_bytes(bytes.data() + section.offset,
                                               u64{section.element_size} * section.element_count));
    }
    MESSAGE(file.species, " sections ", folded);
    CHECK(folded == pins[profile_index++]);
    for (const f32 distance : {5.0f, 20.0f, 60.0f, 240.0f}) {
      geometry::LodFormCut cut;
      cut.distance = distance;
      cut.proj_scale = 1080;
      cut.foliage_px = geometry::k_default_foliage_px;
      geometry::ClusterLodMesh form;
      geometry::LodFormMap map;
      Vector<u32> materials;
      REQUIRE_MESSAGE(geometry::extract_lod_form(data.mesh, data.lod_coverage, cut, form, materials,
                                                 map, &error),
                      error);
      CHECK_FALSE(form.mesh.clusters.empty());
      for (const u32 v : map.vertices)
        CHECK(v < data.motion_vertices.size());
    }
  }
}

TEST_CASE("alpine: baked material image edits invalidate only enabled bakes") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(
      R"({"species":"silene_acaulis","materials":{"leaf":{"base_color":"leaf.png"}}})", file,
      error));
  Vector<u8> bytes{1, 2, 3, 4};
  assets::MeshGeneratorHosts hosts;
  hosts.context = &bytes;
  hosts.read_image = [](void* context, std::string_view, Vector<u8>& out, std::string&) {
    out = *static_cast<Vector<u8>*>(context);
    return true;
  };
  const assets::MeshGeneratorInputs inputs{nullptr, &hosts};
  u64 before = 0, after = 0;
  REQUIRE(flora::flora_source_hash_with(file, inputs, before, error));
  bytes[0]++;
  REQUIRE(flora::flora_source_hash_with(file, inputs, after, error));
  CHECK(before != after);
  file.alpine.impostor_px = 0;
  REQUIRE(flora::flora_source_hash_with(file, inputs, before, error));
  bytes[0]++;
  REQUIRE(flora::flora_source_hash_with(file, inputs, after, error));
  CHECK(before == after);
}

TEST_CASE("alpine: default patches reach their six-triangle far forms") {
  for (const auto& profile : flora::alpine_profiles()) {
    flora::FloraFile file;
    file.species = geometry::flora_species_info(profile.species).name;
    geometry::ClusterFileData data;
    std::string error;
    REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 65536, data, nullptr, error), error);
    for (const f32 distance : {5.f, 20.f, 60.f, 240.f}) {
      geometry::LodFormCut cut;
      cut.distance = distance;
      cut.proj_scale = 1080;
      cut.foliage_px = geometry::k_default_foliage_px;
      geometry::ClusterLodMesh form;
      geometry::LodFormMap map;
      Vector<u32> coverage;
      REQUIRE_MESSAGE(geometry::extract_lod_form(data.mesh, data.lod_coverage, cut, form, coverage,
                                                 map, &error),
                      error);
      u32 triangles = 0;
      for (const auto& cluster : form.mesh.clusters)
        triangles += cluster.triangle_count;
      MESSAGE(file.species, " at ", distance, " m: ", triangles, " retained triangles");
      CHECK(triangles > 0);
      if (distance == 5 && profile.form != flora::AlpineForm::dwarf_scrub) {
        // A resident form includes its finer frontier and the six coarser triangles which a
        // still more distant camera may select. They are not all drawn in the same frame.
        CHECK(triangles == data.mesh.leaf_triangle_count + 6);
      }
      if (distance == 240) CHECK(triangles == 6);
    }
  }
}
