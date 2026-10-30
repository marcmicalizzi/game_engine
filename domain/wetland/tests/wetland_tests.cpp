#include <core/schema/json_reflect.h>
#include <domain/materials/materials.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/wetland/wetland.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
using namespace engine;

TEST_CASE("wetland: grounded dimensions and catalogue policies") {
  CHECK(wetland::profile("sedge")->leaf_width_m == doctest::Approx(.005));
  CHECK(wetland::profile("rush")->stem_radius_m * 2 == doctest::Approx(.003));
  CHECK(wetland::profile("reed")->leaf_length_m == doctest::Approx(.30));
  CHECK(wetland::profile("reed")->leaf_width_m == doctest::Approx(.03));
  CHECK(wetland::profile("cattail")->leaf_width_m == doctest::Approx(.018));
  CHECK(wetland::profile("arrowhead")->leaf_length_m == doctest::Approx(.22));
  CHECK(wetland::profile("water_lily")->leaf_width_m == doctest::Approx(.25));
  CHECK(wetland::profile("pond_lily")->leaf_length_m == doctest::Approx(.28));
  for (const auto& p : wetland::profiles()) {
    const auto& info = geometry::flora_species_info(p.species);
    CHECK(info.casters.cascades == 1);
    CHECK_FALSE(info.casters.traced);
  }
}
TEST_CASE("wetland: all forms have finite nondegenerate geometry within budget") {
  for (const auto& p : wetland::profiles()) {
    wetland::Plant recipe;
    recipe.species = geometry::flora_species_info(p.species).name;
    CAPTURE(recipe.species);
    assets::MeshData mesh;
    flora::Motion motion;
    wetland::Report report;
    std::string error;
    REQUIRE_MESSAGE(wetland::solve(recipe, mesh, motion, report, error), error);
    CHECK(report.triangles + 6 <= report.budget);
    CHECK(report.live_shoots + report.dead_shoots == p.shoots);
    REQUIRE(mesh.positions.size() == mesh.normals.size());
    REQUIRE(mesh.positions.size() == mesh.uvs.size());
    REQUIRE(mesh.positions.size() == motion.vertex_part.size());
    REQUIRE(motion.parts.size() == motion.structure.size());
    for (u32 i = 0; i < motion.parts.size(); ++i)
      CHECK(motion.structure[i].flags == 0);
    for (const auto& v : mesh.positions)
      CHECK(std::isfinite(v.x + v.y + v.z));
    for (const auto& n : mesh.normals)
      CHECK(length(n) == doctest::Approx(1).epsilon(.001));
    for (u32 i = 0; i < mesh.indices.size(); i += 3) {
      const Vec3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]],
                 c = mesh.positions[mesh.indices[i + 2]];
      CHECK(length(cross(b - a, c - a)) > 0);
    }
    const auto parts = motion.parts;
    recipe.triangle_budget = report.triangles;
    if (wetland::solve(recipe, mesh, motion, report, error)) {
      REQUIRE(parts.size() == motion.parts.size());
      CHECK(std::memcmp(parts.data(), motion.parts.data(),
                        parts.size() * sizeof(flora::MotionPart)) == 0);
    }
  }
}
TEST_CASE("wetland: invalid input and impossible budgets are explicit errors") {
  wetland::Plant p;
  assets::MeshData mesh;
  flora::Motion motion;
  wetland::Report report;
  std::string e;
  p.triangle_budget = 64;
  CHECK_FALSE(wetland::solve(p, mesh, motion, report, e));
  CHECK(mesh.indices.empty());
  p = {};
  p.dead_share = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(wetland::in_range(p, e));
  p = {};
  p.species = "grass";
  CHECK_FALSE(wetland::in_range(p, e));
  CHECK_FALSE(wetland::read_file("{", p, e));
}
TEST_CASE("wetland: containers carry far cards and motion without colliders") {
  for (const auto* name : {"sedge", "reed", "water_lily"}) {
    wetland::Plant p;
    p.species = name;
    geometry::ClusterFileData data;
    wetland::Report report;
    std::string error;
    REQUIRE_MESSAGE(wetland::build_container(p, {}, {}, 65536, data, report, error), error);
    CHECK(data.generator == "wetland");
    CHECK(data.generator_kind == name);
    CHECK(data.materials.size() == 3);
    CHECK(data.impostors.size() == 1);
    CHECK(data.images.size() == 2);
    CHECK(data.lod_coverage.size() == data.mesh.lod.size());
    REQUIRE(data.motion_vertices.size() == data.mesh.mesh.vertices.size());
    CHECK(report.triangles <= report.budget);
    Vector<u32> selected;
    geometry::select_lod_raw(data.mesh, .0001f, selected);
    u32 triangles = 0;
    for (const u32 cluster : selected)
      triangles += data.mesh.mesh.clusters[cluster].triangle_count;
    CHECK(triangles == report.triangles - 6);
    selected.clear();
    geometry::select_lod_raw(data.mesh, 100, selected);
    REQUIRE(selected.size() == 1);
    CHECK(data.mesh.mesh.clusters[selected[0]].triangle_count == 6);
  }
}
TEST_CASE("wetland: tiles agree with expansion and preserve IDs far from origin") {
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("wetland");
  REQUIRE(gen);
  wetland::Patch p;
  p.meshes.push_back("sedge.wetland.json");
  p.far_meshes.push_back("sedge-far.clusters");
  p.x_m = 10000000.125;
  p.z_m = -10000000.25;
  scene_gen::Context c;
  c.tile_size = 4;
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(gen->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements all;
  REQUIRE(gen->expand(state, c, all, &error));
  REQUIRE(!all.instances.empty());
  Vector<u32> ids;
  const i32 tx = static_cast<i32>(std::floor(p.x_m / 4)),
            tz = static_cast<i32>(std::floor(p.z_m / 4));
  for (i32 z = tz + 2; z >= tz - 2; --z)
    for (i32 x = tx + 3; x >= tx - 3; --x) {
      scene_gen::Placements near, far;
      REQUIRE(gen->tile(state, {x, z}, c, near, &error));
      c.ring = 1;
      REQUIRE(gen->tile(state, {x, z}, c, far, &error));
      c.ring = scene_gen::k_no_ring;
      REQUIRE(near.instances.size() == far.instances.size());
      for (u32 i = 0; i < near.instances.size(); ++i) {
        CHECK(near.instances[i].id == far.instances[i].id);
        CHECK(far.instances[i].mesh == near.instances[i].mesh + 1);
        ids.push_back(near.instances[i].id);
        bool found = false;
        for (const auto& whole : all.instances)
          if (whole.id == near.instances[i].id) {
            CHECK(whole.position.x == near.instances[i].position.x);
            CHECK(whole.position.z == near.instances[i].position.z);
            found = true;
          }
        CHECK(found);
      }
    }
  CHECK(ids.size() == all.instances.size());
  gen->close(state);
  p.candidate_budget = 1;
  CHECK_FALSE(gen->open(schema::to_json(p), c, &state, &error));
  p.candidate_budget = 100000;
  p.mask_layer = 0;
  CHECK_FALSE(gen->open(schema::to_json(p), c, &state, &error));
}

TEST_CASE("wetland: depth, slope, exclusions, mortality and static floating leaves") {
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("wetland");
  REQUIRE(gen);
  wetland::Patch patch;
  patch.meshes.push_back("reed.wetland.json");
  patch.density = 1;
  scene_gen::Context context;
  void* state = nullptr;
  std::string error;
  REQUIRE(gen->open(schema::to_json(patch), context, &state, &error));
  scene_gen::Placements before, after;
  REQUIRE(gen->expand(state, context, before, &error));
  REQUIRE(!before.instances.empty());
  context.ground.surface_fn = [](const void*, i64, i64) noexcept -> f64 { return -1; };
  REQUIRE(gen->expand(state, context, after, &error));
  CHECK(after.instances.empty());
  context.ground.surface_fn = [](const void*, i64 x, i64) noexcept -> f64 {
    return static_cast<f64>(x) * .002;
  };
  REQUIRE(gen->expand(state, context, after, &error));
  CHECK(after.instances.empty());
  context.ground = {};
  const scene_gen::Footprint obstacle{0, 0, 20};
  context.earlier = {&obstacle, 1};
  REQUIRE(gen->expand(state, context, after, &error));
  CHECK(after.instances.empty());
  context.earlier = {};
  gen->close(state);
  patch.species = "water_lily";
  patch.water_y_m = .1;
  REQUIRE(gen->open(schema::to_json(patch), context, &state, &error));
  REQUIRE(gen->expand(state, context, after, &error));
  REQUIRE(!after.instances.empty());
  CHECK(after.instances[0].position.y == doctest::Approx(-.4));
  gen->close(state);
  wetland::Plant p;
  p.species = "water_lily";
  assets::MeshData mesh;
  flora::Motion motion;
  wetland::Report report;
  REQUIRE(wetland::solve(p, mesh, motion, report, error));
  for (auto w : motion.vertex_weight)
    CHECK(w == 0);
  p.species = "reed";
  p.dead_share = 1;
  p.broken_share = 0;
  REQUIRE(wetland::solve(p, mesh, motion, report, error));
  CHECK(report.dead_shoots == 8);
  f32 intact = 0;
  for (const auto v : mesh.positions)
    intact = std::max(intact, v.y);
  p.broken_share = 1;
  REQUIRE(wetland::solve(p, mesh, motion, report, error));
  f32 broken = 0;
  for (const auto v : mesh.positions)
    broken = std::max(broken, v.y);
  CHECK(broken < intact);
  p.collider = true;
  CHECK_FALSE(wetland::in_range(p, error));
}
TEST_CASE("wetland: masks weight occupancy and do not wrap outside their extent") {
  struct Mask {
    u32 weight = 255;
    bool available = true;
  } mask;
  const scene_gen::GroundMaskOps ops{
      .info =
          [](const void*, scene_gen::GroundMaskInfo& info) noexcept {
            info = {};
            info.cells = 8;
            info.spacing_mm = 1000;
            info.palette = 1;
            info.tile_min_x = info.tile_min_z = -1;
            info.tile_max_x = info.tile_max_z = 1;
            return true;
          },
      .tile =
          [](const void* state, i32, i32, std::span<u32> words) noexcept {
            if (!static_cast<const Mask*>(state)->available) return false;
            std::fill(words.begin(), words.end(), static_cast<const Mask*>(state)->weight);
            words[0] = 0xffffff00u;
            return true;
          }};
  struct Source {
    const scene_gen::GroundMaskOps* ops;
    Mask* mask;
  } source{&ops, &mask};
  scene_gen::Context c;
  c.ground.context = &source;
  c.ground.masks_fn = [](const void* state, scene_gen::GroundMasks& out, std::string*) {
    const auto& s = *static_cast<const Source*>(state);
    out = {s.ops, s.mask};
    return true;
  };
  wetland::Patch p;
  p.meshes.push_back("sedge.wetland.json");
  p.mask_layer = 0;
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("wetland");
  REQUIRE(gen);
  void* entry = nullptr;
  std::string error;
  REQUIRE_MESSAGE(gen->open(schema::to_json(p), c, &entry, &error), error);
  scene_gen::Placements a, b;
  REQUIRE(gen->expand(entry, c, a, &error));
  CHECK(!a.instances.empty());
  mask.weight = 0;
  REQUIRE(gen->expand(entry, c, b, &error));
  CHECK(b.instances.empty());
  mask.available = false;
  CHECK_FALSE(gen->expand(entry, c, b, &error));
  mask.available = true;
  gen->close(entry);
  p.x_m = 10000000;
  mask.weight = 255;
  REQUIRE(gen->open(schema::to_json(p), c, &entry, &error));
  REQUIRE(gen->expand(entry, c, b, &error));
  CHECK(b.instances.empty());
  gen->close(entry);
}

TEST_CASE("wetland: pack pixels affect source identity and the far bake") {
  wetland::Plant p;
  p.species = "arrowhead";
  materials::Pack pack;
  materials::PackRole role;
  role.role = "arrowhead/leaf";
  role.set.base_color = "leaf.png";
  pack.roles.push_back(role);
  pack.hash = 19;
  struct Pixels {
    Vector<u8> png;
  } pixels;
  const u8 rgba[] = {80, 150, 35, 255, 80, 150, 35, 255, 80, 150, 35, 255, 80, 150, 35, 255};
  REQUIRE(image::encode_png(2, 2, 4, rgba, pixels.png));
  assets::MeshGeneratorHosts hosts;
  hosts.context = &pixels;
  hosts.read_image = [](void* ctx, std::string_view, Vector<u8>& out, std::string&) {
    out = static_cast<Pixels*>(ctx)->png;
    return true;
  };
  geometry::ClusterFileData a, b;
  wetland::Report report;
  std::string error;
  REQUIRE_MESSAGE(wetland::build_container(p, {&pack, &hosts}, {}, 0, a, report, error), error);
  const u8 brown[] = {130, 70, 20, 255, 130, 70, 20, 255, 130, 70, 20, 255, 130, 70, 20, 255};
  REQUIRE(image::encode_png(2, 2, 4, brown, pixels.png));
  REQUIRE_MESSAGE(wetland::build_container(p, {&pack, &hosts}, {}, 0, b, report, error), error);
  CHECK(a.source_hash != b.source_hash);
  CHECK(a.build_key != b.build_key);
  REQUIRE(a.images.size() == b.images.size());
  CHECK(a.images[1].bytes != b.images[1].bytes);
  CHECK_FALSE(wetland::build_container(p, {&pack, nullptr}, {}, 0, b, report, error));
  CHECK(b.mesh.mesh.vertices.empty());
}

TEST_CASE("wetland: tussock roots stay inside the crown on every budget rung") {
  for (const u32 budget : {8000u, 4000u, 2500u}) {
    wetland::Plant p;
    p.species = "tussock";
    p.triangle_budget = budget;
    p.dead_share = 0;
    assets::MeshData mesh;
    flora::Motion motion;
    wetland::Report report;
    std::string error;
    REQUIRE_MESSAGE(wetland::solve(p, mesh, motion, report, error), error);
    const u32 crown = motion.parts.size() - 1;
    for (u32 part = 0; part < crown; ++part) {
      const Vec3 root = motion.parts[part].pivot;
      f32 surface = -1;
      for (u32 i = 0; i < mesh.indices.size(); i += 3) {
        const u32 ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        if (motion.vertex_part[ia] != crown || motion.vertex_part[ib] != crown ||
            motion.vertex_part[ic] != crown)
          continue;
        const Vec3 a = mesh.positions[ia], b = mesh.positions[ib], c = mesh.positions[ic];
        const f64 ax = static_cast<f64>(a.x - c.x), az = static_cast<f64>(a.z - c.z),
                  bx = static_cast<f64>(b.x - c.x), bz = static_cast<f64>(b.z - c.z);
        const f64 dx = static_cast<f64>(root.x - c.x), dz = static_cast<f64>(root.z - c.z),
                  det = ax * bz - bx * az;
        if (std::abs(det) < 1e-12) continue;
        const f64 u = (dx * bz - bx * dz) / det, v = (ax * dz - dx * az) / det;
        if (u >= -1e-6 && v >= -1e-6 && u + v <= 1.000001)
          surface = std::max(
              surface, static_cast<f32>(u * static_cast<f64>(a.y) + v * static_cast<f64>(b.y) +
                                        (1 - u - v) * static_cast<f64>(c.y)));
      }
      REQUIRE(surface >= 0);
      CHECK(root.y <= surface + 1e-5f);
    }
  }
}
