// G198: a far form for any rigid mesh, rocks first (docs/subsystems/geometry.md, "The far form";
// rocks.md, "A deposit past the first ring"). What a tile of deposits at the forest's density costs
// in a ring past the first as the DAG's own distance form, and as the far form.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/rocks/deposits.h>
#include <domain/scene_gen/far_tile.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <tuple>
#include <vector>

using namespace engine;
using namespace engine::rocks;
namespace {
f64 massif(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 12.0 * std::sin(z / 300.0 * 6.283185307179586);
  const f64 d = std::abs(x);
  auto bump = [&](f64 h, f64 w) {
    return d < w ? h * (0.5 + 0.5 * std::cos(3.14159265358979323846 * d / w)) : 0.0;
  };
  return bump(60, 40) + bump(40, 70) + .02 * x;
}

// The forest's density over the massif's foot, one 32 m tile's meshes as a host builds them, and
// the build's wall time.
i64 build_ns = 0;
// Each mesh's bed rectangle, as the build locks it: x and z from 0 to these (deposits.cpp).
Vector<Vec2> spans;
void forest_tile(Vector<geometry::ClusterFileData>& out) {
  spans.clear();
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &massif;
  c.tile_size = 32;
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {96, 32};
  p.spacing_m = 1;
  p.patch_m = 16;
  p.patch.triangle_budget = 128000;
  p.patch.fragments = 96;
  p.sediment_depth_m = 0;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements tile;
  c.ring = 0;
  REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, tile, &error), error);
  desc->close(state);
  REQUIRE_FALSE(tile.meshes.empty());
  for (const auto& m : tile.meshes) {
    JsonValue json;
    REQUIRE(parse_json(m.recipe, json).ok);
    DepositPatch patch;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(patch, json, ctx));
    geometry::ClusterFileData data;
    const i64 start = time::monotonic_ns();
    REQUIRE_MESSAGE(build_deposit_container(patch, {}, 0, data, error), error);
    build_ns += time::monotonic_ns() - start;
    out.push_back(std::move(data));
    const i64 spacing_mm = std::llround(static_cast<f64>(patch.spacing_m) * 1000.0);
    spans.push_back(Vec2{static_cast<f32>(static_cast<f64>(spacing_mm * (patch.width - 1)) / 1000.0),
                         static_cast<f32>(static_cast<f64>(spacing_mm * (patch.depth - 1)) / 1000.0)});
  }
}

u64 file_bytes(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<u64>(size);
}
}  // namespace

TEST_CASE("deposits far: what the DAG's own distance form keeps of a dense tile, ring by ring") {
  // The measurement the far form's choice rests on (rocks.md, "A deposit past the first ring"):
  // every mesh of a tile, cut as a streamed world cuts an authored mesh's distance form for a ring
  // whose floor is 96, 256 and 512 m (the deposit scenes' rings of 1.5, 4 and 8 tiles of 64 m), at
  // the default camera's projection and one pixel.
  Vector<geometry::ClusterFileData> meshes;
  forest_tile(meshes);
  const f32 floors[] = {0.0f, 96.0f, 256.0f, 512.0f};
  u64 clusters[4] = {}, triangles[4] = {};
  for (const auto& data : meshes) {
    for (u32 r = 0; r < 4; ++r) {
      if (r == 0) {
        clusters[0] += data.mesh.mesh.clusters.size();
        for (const auto& cl : data.mesh.mesh.clusters)
          triangles[0] += cl.triangle_count;
        continue;
      }
      geometry::LodFormCut cut;
      Vec3 lo = data.mesh.mesh.vertices[0], hi = lo;
      for (const Vec3 v : data.mesh.mesh.vertices) {
        lo = Vec3{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = Vec3{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
      }
      cut.origin = (lo + hi) * 0.5f;
      cut.distance = floors[r];
      cut.proj_scale = 1037.33f;
      cut.threshold_px = 1.0f;
      geometry::ClusterLodMesh form;
      Vector<u32> coverage;
      geometry::LodFormMap map;
      std::string error;
      REQUIRE_MESSAGE(geometry::extract_lod_form(data.mesh, {}, cut, form, coverage, map, &error),
                      error);
      clusters[r] += form.mesh.clusters.size();
      // The triangles a camera at the floor draws: the frontier, every cluster of level 0.
      for (u32 k = 0; k < form.mesh.clusters.size(); ++k)
        if (form.lod[k].level == 0) triangles[r] += form.mesh.clusters[k].triangle_count;
    }
  }
  for (u32 r = 0; r < 4; ++r)
    MESSAGE("floor ", floors[r], " m: ", clusters[r], " clusters (pairs) over ", meshes.size(),
            " meshes, ", triangles[r], " triangles at the floor");
  CHECK(clusters[0] > 0);

  // The far form of the same meshes (geometry.md, "The far form"): its levels' triangles, and the
  // nearest each stands in from at the same projection.
  u64 level_triangles[geometry::k_far_form_levels] = {};
  f32 level_from[geometry::k_far_form_levels] = {};
  u64 vertices = 0;
  i64 bake_ns = 0;
  u64 bytes_without = 0, bytes_with = 0;
  test::TempDir files;
  for (auto& data : meshes) {
    std::string error;
    REQUIRE(geometry::far_form_applies(data));
    const std::string path = files.path() + "/mesh.clusters";
    REQUIRE(geometry::write_cluster_file(path, data, &error));
    bytes_without += file_bytes(path);
    const i64 start = time::monotonic_ns();
    REQUIRE_MESSAGE(geometry::ensure_far_form(data, &error), error);
    bake_ns += time::monotonic_ns() - start;
    REQUIRE(geometry::write_cluster_file(path, data, &error));
    bytes_with += file_bytes(path);
    vertices += data.far_form.vertices.size();
    for (u32 l = 0; l < data.far_form.levels.size(); ++l) {
      level_triangles[l] += data.far_form.levels[l].triangles;
      level_from[l] = std::max(
          level_from[l], geometry::far_form_distance(data.far_form.levels[l], 1037.33f, 1.0f));
    }
  }
  for (u32 l = 0; l < geometry::k_far_form_levels; ++l)
    MESSAGE("far form level ", l, ": ", level_triangles[l], " triangles over ", meshes.size(),
            " meshes, standing in from ", level_from[l], " m");
  MESSAGE("far form vertices, every level: ", vertices);
  // **The far forms meet where the near forms do** (geometry.md, "The far form meets its
  // neighbour"): every point of a piece's locked bed rectangle that its finest level draws is still
  // a vertex of every far form level, so two pieces' far forms share their cut and no wall shows
  // between them. A far form that ignored the build's locks parted them (the dark lines along the
  // pieces' cuts in G198's first captures).
  u64 border_points = 0, border_missing = 0;
  for (u32 k = 0; k < meshes.size(); ++k) {
    const geometry::ClusterLodMesh& lod = meshes[k].mesh;
    const geometry::FarForm& form = meshes[k].far_form;
    Vector<Vec3> border;
    for (u32 c = 0; c < lod.mesh.clusters.size(); ++c) {
      if (lod.lod[c].level != 0) continue;
      const auto& cl = lod.mesh.clusters[c];
      for (u32 v = 0; v < cl.vertex_count; ++v) {
        const Vec3 at = lod.mesh.vertices[cl.vertex_offset + v];
        if (at.x == 0.0f || at.z == 0.0f || at.x == spans[k].x || at.z == spans[k].y)
          border.push_back(at);
      }
    }
    for (u32 l = 0; l < form.levels.size(); ++l) {
      std::vector<std::tuple<f32, f32, f32>> used;
      const auto& level = form.levels[l];
      for (u32 p = level.first_part; p < level.first_part + level.part_count; ++p)
        for (u32 i = 0; i < form.parts[p].index_count; ++i) {
          const Vec3 at = form.vertices[form.indices[form.parts[p].first_index + i]].position;
          used.emplace_back(at.x, at.y, at.z);
        }
      std::sort(used.begin(), used.end());
      for (const Vec3 want : border) {
        ++border_points;
        if (!std::binary_search(used.begin(), used.end(), std::make_tuple(want.x, want.y, want.z)))
          ++border_missing;
      }
    }
  }
  MESSAGE("bed border points over every level: ", border_points, ", missing ", border_missing);
  CHECK(border_points > 0);
  CHECK(border_missing == 0);
  // What every rigid container build now pays (geometry.md, "The far form"): the bake's share of
  // the meshes' build, and the bytes it adds.
  MESSAGE("bake ", static_cast<f64>(bake_ns) / 1.0e6, " ms against the build's ",
          static_cast<f64>(build_ns) / 1.0e6, " ms (",
          100.0 * static_cast<f64>(bake_ns) / static_cast<f64>(build_ns), " %); containers ",
          bytes_without, " bytes, ", bytes_with, " with the far form (+",
          100.0 * static_cast<f64>(bytes_with - bytes_without) / static_cast<f64>(bytes_without),
          " %)");
  // The pin (determinism): every piece's far form hashed in order, the same bytes from every
  // toolchain and thread count.
  u64 pin = 0;
  for (const auto& data : meshes)
    pin = hash_combine(pin, geometry::far_form_hash(data.far_form));
  MESSAGE("far forms' hash ", pin);
  // Taken on MSVC 14.51, 2026-11-13 (k_far_form_version 2, k_deposit_version 11).
  CHECK(pin == 11231041873535240146ull);
  CHECK(level_triangles[0] * 4 < triangles[0]);
}

TEST_CASE("deposits far: a 64 m tile at the forest's budget hands a far tile a block of 2 x 2") {
  // rocks.md, "A far tile a block, under the cluster limit": a whole 64 m tile's far tile built
  // over 4,096 clusters on G191's streamed scene and was refused; its blocks are each a far tile,
  // each instance naming its own mesh (the first split named mesh 0 for every block, which drew
  // one block twice and the other not at all: half the band missing from 300 m).
  test::TempDir scratch;
  scene_gen::Context c;
  c.dir = scratch.path();
  c.ddc = scratch.path();
  c.ground.surface_fn = &massif;
  c.tile_size = 64;
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {128, 64};
  p.spacing_m = 1;
  p.patch_m = 16;
  p.patch.triangle_budget = 128000;
  p.sediment_depth_m = 0;
  p.far_rings = 2;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements near, far;
  c.ring = 0;
  REQUIRE_MESSAGE(desc->tile(state, {0, 0}, c, near, &error), error);
  c.ring = 1;
  REQUIRE_MESSAGE(desc->tile(state, {0, 0}, c, far, &error), error);
  desc->close(state);
  REQUIRE_FALSE(near.meshes.empty());
  MESSAGE(near.meshes.size(), " near meshes, ", far.meshes.size(), " far tiles");
  CHECK(far.meshes.size() > 1u);
  CHECK(far.meshes.size() <= 4u);
  REQUIRE(far.instances.size() == far.meshes.size());
  usize members = 0;
  for (u32 k = 0; k < far.instances.size(); ++k) {
    CHECK(far.instances[k].mesh == k);
    REQUIRE(scene_gen::is_far_tile(far.meshes[k]));
    scene_gen::FarTile tile;
    REQUIRE_MESSAGE(scene_gen::read_far_tile(far.meshes[k].recipe, tile, &error), error);
    members += tile.placements.size();
    for (u32 j = 0; j < k; ++j)
      CHECK(far.instances[j].id != far.instances[k].id);
  }
  CHECK(members == near.instances.size());
}

TEST_CASE("deposits far: a ring past near_rings hands one far tile of what the near ring hands") {
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &massif;
  c.tile_size = 32;
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {96, 32};
  p.spacing_m = 1;
  p.patch_m = 16;
  p.sediment_depth_m = 0;
  p.far_rings = 4;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements near;
  c.ring = 0;
  REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, near, &error), error);
  REQUIRE_FALSE(near.meshes.empty());
  CHECK(near.kind == 0);
  for (u8 ring = 1; ring < 4; ++ring) {
    c.ring = ring;
    scene_gen::Placements far;
    REQUIRE_MESSAGE(desc->tile(state, {1, 0}, c, far, &error), error);
    CHECK(far.kind == std::min(2 + ring - 1, 3));
    CHECK(desc->representation(state, c) == 2 + ring - 1);
    REQUIRE(far.meshes.size() == 1);
    REQUIRE(far.instances.size() == 1);
    REQUIRE(scene_gen::is_far_tile(far.meshes[0]));
    // At the tile's corner, standing for exactly the near ring's meshes, recipe for recipe.
    CHECK(far.instances[0].position.x == 32.0);
    CHECK(far.instances[0].position.z == 0.0);
    scene_gen::FarTile tile;
    REQUIRE_MESSAGE(scene_gen::read_far_tile(far.meshes[0].recipe, tile, &error), error);
    CHECK(tile.level == ring - 1u);
    REQUIRE(tile.meshes.size() == near.meshes.size());
    REQUIRE(tile.placements.size() == near.instances.size());
    for (u32 k = 0; k < tile.meshes.size(); ++k) {
      CHECK(tile.meshes[k].recipe == near.meshes[k].recipe);
      CHECK(tile.meshes[k].path == near.meshes[k].path);
      const auto& placed = near.instances[k];
      const auto& member = tile.placements[k];
      CHECK(member.mesh == placed.mesh);
      CHECK(static_cast<f64>(member.x_mm) / 1000.0 + far.instances[0].position.x ==
            doctest::Approx(placed.position.x));
      CHECK(static_cast<f64>(member.z_mm) / 1000.0 + far.instances[0].position.z ==
            doctest::Approx(placed.position.z));
      // Heights to the nearest millimetre, the far tile's lattice.
      CHECK(std::abs(static_cast<f64>(member.y_mm) / 1000.0 + far.instances[0].position.y -
                     placed.position.y) <= 0.0011);
    }
    // The same tile asked again is the same bytes.
    scene_gen::Placements again;
    REQUIRE(desc->tile(state, {1, 0}, c, again, &error));
    CHECK(again.meshes[0].recipe == far.meshes[0].recipe);
    CHECK(again.meshes[0].path == far.meshes[0].path);
  }
  // Past far_rings, nothing.
  c.ring = 4;
  scene_gen::Placements none;
  REQUIRE(desc->tile(state, {1, 0}, c, none, &error));
  CHECK(none.instances.empty());
  CHECK(none.kind == 1);
  desc->close(state);
}
