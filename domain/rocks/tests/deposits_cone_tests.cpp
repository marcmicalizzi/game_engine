// G143: cones under gullies, blocks shed below faces, and the formation's lithology
// (docs/subsystems/rocks.md, "Cones and shed blocks"; docs/experiments/rockfall-deposits.md, G143).
#include "rock_checks.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace engine;
using namespace engine::rocks;
namespace {
// A 24 m face at 63 degrees from x = 24 to 36 m above a meadow falling gently toward -x, a plateau
// above it, and three gullies cut into the face at z = 12, 24 and 36 m, each 6 m across and 5 m
// deep, their floors still steeper than 50 degrees. `data` pointing at a zero is the same face
// with no gullies: straight contours everywhere.
f64 gullied(const void* data, i64 x_mm, i64 z_mm) noexcept {
  const bool gullies = data == nullptr || *static_cast<const int*>(data) != 0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  f64 h = x < 24 ? .08 * (x - 24) : (x > 36 ? 24 : (x - 24) * 2);
  if (!gullies) return h;
  for (f64 c : {12.0, 24.0, 36.0}) {
    const f64 u = (z - c) / 3.0;
    if (u * u >= 1) continue;
    const f64 across = (1 - u * u) * (1 - u * u);
    const f64 along = std::clamp((x - 24) / 8.0, 0.0, 1.0) * std::clamp((40 - x) / 4.0, 0.0, 1.0);
    h -= 5.0 * across * along;
  }
  return h;
}
scene_gen::Ground ground(const int* gullies = nullptr) {
  scene_gen::Ground g;
  g.surface_fn = &gullied;
  g.context = gullies;
  return g;
}
DepositLayer layer() {
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {48, 48};
  p.spacing_m = .5f;
  p.patch.fragments = 8;
  p.patch.fragment_m = .2f;
  p.patch.triangle_budget = 20000;
  p.sediment_depth_m = 0;
  return p;
}
DepositLayer cones() {
  DepositLayer p = layer();
  p.talus_depth_m = 0;  // the cones alone, so every surface tested is a cone's
  p.cone_supply_m = 8;
  return p;
}
f64 top(const DepositPlan& plan, u32 i) {
  return plan.ground[i] + static_cast<f64>(plan.thickness[i]);
}
u32 at(const DepositPlan& plan, f64 x, f64 z) {
  return static_cast<u32>(std::llround(z / .5)) * plan.width +
         static_cast<u32>(std::llround(x / .5));
}
constexpr f64 k_repose = 0.70020753820970977;  // tan 35 degrees
}  // namespace

TEST_CASE("deposits: G128's envelope makes an apron, not a cone, below three gullies") {
  // Stage 1 of G143: what the existing talus does at a gully's foot. The numbers go to
  // docs/experiments/rockfall-deposits.md ("What G128 already did at a gully's foot").
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(layer(), ground(), plan, error), error);
  CHECK(plan.cones.empty());
  CHECK(plan.blocks.empty());
  CHECK(plan.rock.empty());
  f64 under = 0, between = 0, reach_under = 0, reach_between = 0;
  for (f64 z : {12.0, 24.0, 36.0}) {
    under += static_cast<f64>(plan.thickness[at(plan, 23.5, z)]) / 3;
    for (f64 x = 23.5; x >= 0 && plan.thickness[at(plan, x, z)] > .001f; x -= .5)
      reach_under = std::max(reach_under, 24 - x);
  }
  for (f64 z : {18.0, 30.0}) {
    between += static_cast<f64>(plan.thickness[at(plan, 23.5, z)]) / 2;
    for (f64 x = 23.5; x >= 0 && plan.thickness[at(plan, x, z)] > .001f; x -= .5)
      reach_between = std::max(reach_between, 24 - x);
  }
  MESSAGE("G128 at the foot: depth under a gully ", under, " m, between ", between,
          " m; reach under ", reach_under, " m, between ", reach_between, " m");
  // Even depth, even reach: the talus' outline follows the foot line, not the gullies.
  CHECK(std::abs(under - between) < .25);
  CHECK(std::abs(reach_under - reach_between) <= 1.0);
}

TEST_CASE("deposits: a cone stands at each gully's mouth and nowhere on a planar face") {
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(cones(), ground(), plan, error), error);
  REQUIRE(plan.cones.size() == 3);
  f64 expected[] = {12, 24, 36};
  for (u32 c = 0; c < 3; ++c) {
    const DepositCone& cone = plan.cones[c];
    const f64 x = static_cast<f64>(cone.x_mm) / 1000.0, z = static_cast<f64>(cone.z_mm) / 1000.0;
    MESSAGE("cone at ", x, ", ", z, ": ", cone.height_m, " m over its mouth, catchment ",
            cone.catchment_m2, " m2, reach ", cone.reach_m, " m");
    // In the gully's mouth: its lower reach, where the floor stands at the apex's height, on its
    // axis; its mouth is where the floor leaves the face, at the foot.
    CHECK(x >= 24.0);
    CHECK(x <= 30.0);
    const f64 mouth_x = static_cast<f64>(cone.mouth % plan.width) * .5;
    CHECK(mouth_x >= 23.0);
    CHECK(mouth_x <= 24.5);
    bool on_axis = false;
    for (f64 e : expected)
      on_axis = on_axis || std::abs(z - e) <= 1.0;
    CHECK(on_axis);
    CHECK(cone.height_m > 2.0);
    CHECK(cone.height_m <= 12.0);
    // Nothing of it near its apex stands higher than the apex, and it fills its mouth.
    CHECK(plan.thickness[cone.mouth] > 1.0f);
    for (u32 i = 0; i < plan.ground.size(); ++i) {
      const f64 dx = static_cast<f64>(i % plan.width) * .5 - x;
      const f64 dz = static_cast<f64>(i / plan.width) * .5 - z;
      if (plan.thickness[i] > .001f && dx * dx + dz * dz < 9.0)
        CHECK(top(plan, i) <= cone.apex_m + 1e-4);
    }
  }
  // A planar face: straight contours converge nowhere, so it has no gully and no cone.
  const int none = 0;
  REQUIRE(plan_deposits(cones(), ground(&none), plan, error));
  CHECK(plan.cones.empty());
  for (u32 i = 0; i < plan.ground.size(); ++i)
    CHECK(deposit_convergence(plan, i, 4) == 0.0);
}

TEST_CASE("deposits: a cone holds the angle of repose above and eases concave to its toe") {
  DepositPlan plan;
  std::string error;
  const DepositLayer p = cones();
  REQUIRE(plan_deposits(p, ground(), plan, error));
  REQUIRE_FALSE(plan.cones.empty());
  // No part of any cone is steeper than the repose angle, between any two neighbouring samples.
  const u32 w = plan.width;
  u32 pairs = 0;
  for (u32 i = 0; i < plan.ground.size(); ++i) {
    if (plan.thickness[i] <= .001f) continue;
    for (u32 j : {i + 1, i + w, i + w + 1}) {
      if (j >= plan.ground.size() || (j % w) == 0 || plan.thickness[j] <= .001f) continue;
      const f64 run = j == i + w + 1 ? .5 * std::sqrt(2.0) : .5;
      CHECK(std::abs(top(plan, i) - top(plan, j)) <= k_repose * run + 1e-4);
      ++pairs;
    }
  }
  CHECK(pairs > 100);
  // Down the axis: the upper half of its nominal length at 35 degrees (to 0.02 in slope), the
  // lower part gentler, and the slope never rising again toward the toe.
  const DepositCone& cone = plan.cones[0];
  const f64 cz = static_cast<f64>(cone.z_mm) / 1000.0, ax = static_cast<f64>(cone.x_mm) / 1000.0;
  const f64 cx = static_cast<f64>(cone.mouth % plan.width) * .5;  // down the axis from the mouth
  const f64 upper = .75 * cone.height_m / k_repose;
  f64 last = k_repose + 1;
  u32 upper_steps = 0, lower_steps = 0;
  for (f64 x = cx; x - .5 >= 0; x -= .5) {
    const u32 a = at(plan, x, cz), b = at(plan, x - .5, cz);
    if (plan.thickness[a] <= .001f || plan.thickness[b] <= .001f) break;
    const f64 slope = (top(plan, a) - top(plan, b)) / .5;
    CHECK(slope <= k_repose + 1e-4);
    CHECK(slope <= last + 1e-4);
    last = slope;
    if (ax - x + .5 <= upper) {
      CHECK(slope >= k_repose - .02);
      ++upper_steps;
    } else {
      ++lower_steps;
    }
  }
  CHECK(upper_steps >= 2);
  CHECK(lower_steps >= 2);
  CHECK(last < k_repose - .02);
  // Fall sorting on the bed: the coarse share rises from the apex to the toe.
  CHECK(plan.coarse[at(plan, cx - 1, cz)] < plan.coarse[at(plan, cx - 4, cz)]);
}

TEST_CASE("deposits: shed blocks thin with distance, the largest farthest, none past the shadow") {
  DepositPlan plan;
  std::string error;
  DepositLayer p = cones();
  p.talus_depth_m = 3;
  p.blocks_per_100m2 = 60;
  REQUIRE_MESSAGE(plan_deposits(p, ground(), plan, error), error);
  MESSAGE("shed blocks: ", plan.blocks.size());
  REQUIRE(plan.blocks.size() > 30);
  const f64 shadow = std::tan(27.5 * 3.14159265358979323846 / 180.0);
  f64 farthest = 0;
  for (const auto& b : plan.blocks) {
    const f64 dx = static_cast<f64>(b.x_mm - b.top_x_mm) / 1000.0;
    const f64 dz = static_cast<f64>(b.z_mm - b.top_z_mm) / 1000.0;
    const f64 d = std::sqrt(dx * dx + dz * dz);
    // Evans and Hungr's line from the top of its talus: never past it.
    CHECK(b.top_m - b.y_m >= shadow * d - 1e-9);
    // Rests: never on a top steeper than the repose angle.
    CHECK(b.slope[0] * b.slope[0] + b.slope[1] * b.slope[1] <= 1.0609 * k_repose * k_repose);
    // Partly buried by its age, in the stated range.
    CHECK(b.buried >= .15 - 1e-9);
    CHECK(b.buried <= .5 + 1e-9);
    CHECK(b.half[1] <= std::max(b.half[0], b.half[2]));
    farthest = std::max(farthest, b.distance_m);
  }
  u32 count[3] = {};
  f64 size[3] = {};
  for (const auto& b : plan.blocks) {
    const u32 bin = std::min(2u, static_cast<u32>(3 * b.distance_m / farthest));
    ++count[bin];
    size[bin] += b.half[0] + b.half[2];
  }
  for (u32 k = 0; k < 3; ++k)
    if (count[k]) size[k] /= count[k];
  MESSAGE("blocks ", plan.blocks.size(), " out to ", farthest, " m; by thirds: ", count[0], " ",
          count[1], " ", count[2], ", mean size ", size[0], " ", size[1], " ", size[2]);
  CHECK(count[0] > count[1]);
  CHECK(count[1] > count[2]);
  CHECK(size[0] < size[1]);
  CHECK(size[1] < size[2]);
  // Mean block size rises from a cone's apex to its toe.
  const DepositCone& cone = plan.cones[0];
  const i64 mouth_mm = static_cast<i64>(cone.mouth % plan.width) * 500;
  f64 inner = 0, outer = 0, far_r = 0;
  u32 inner_n = 0, outer_n = 0;
  for (const auto& b : plan.blocks)
    if (std::abs(b.z_mm - cone.z_mm) <= 4000 && b.x_mm <= mouth_mm)
      far_r = std::max(far_r, static_cast<f64>(mouth_mm - b.x_mm) / 1000.0);
  for (const auto& b : plan.blocks) {
    if (std::abs(b.z_mm - cone.z_mm) > 4000 || b.x_mm > mouth_mm) continue;
    const f64 r = static_cast<f64>(mouth_mm - b.x_mm) / 1000.0;
    if (r < .5 * far_r) {
      inner += b.half[0];
      ++inner_n;
    } else {
      outer += b.half[0];
      ++outer_n;
    }
  }
  REQUIRE(inner_n > 0);
  REQUIRE(outer_n > 0);
  CHECK(inner / inner_n < outer / outer_n);
}

TEST_CASE(
    "deposits: a deposit under a limestone face is limestone, its blocks no thicker than a bed") {
  DepositLayer p = cones();
  p.talus_depth_m = 3;
  p.blocks_per_100m2 = 60;
  scene::Formation f;
  f.dip_deg = 0;
  scene::FormationBed cap;
  cap.name = "cap";
  cap.thickness_m = 1000;
  f.beds.push_back(cap);
  p.formation = f;
  DepositLithology row;
  row.bed = "cap";
  row.type = RockType::Limestone;
  p.lithology.push_back(row);
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(p, ground(), plan, error), error);
  REQUIRE(plan.rock.size() == plan.ground.size());
  for (u32 i = 0; i < plan.ground.size(); ++i)
    if (plan.thickness[i] > .001f) CHECK(plan.rock[i] == RockType::Limestone);
  for (const auto& b : plan.blocks)
    CHECK(b.type == RockType::Limestone);
  // Thin beds (0.4 m) of shale, named as the type: the blocks are slabs no thicker than a bed.
  f.beds[0].name = "shale";
  f.beds[0].thickness_m = .4f;
  p.formation = f;
  p.lithology.clear();
  REQUIRE(plan_deposits(p, ground(), plan, error));
  REQUIRE_FALSE(plan.blocks.empty());
  for (const auto& b : plan.blocks) {
    CHECK(b.type == RockType::Shale);
    CHECK(2 * b.half[1] <= .4 + 1e-6);
  }
  // Without a formation nothing is read: no rock per sample, and the patch's own type.
  p.formation.reset();
  REQUIRE(plan_deposits(p, ground(), plan, error));
  CHECK(plan.rock.empty());
  for (const auto& b : plan.blocks)
    CHECK(b.type == p.patch.type);
  // A field out of the scene's ranges is refused with its sentence.
  f.dip_deg = 120;
  p.formation = f;
  CHECK_FALSE(plan_deposits(p, ground(), plan, error));
  CHECK(error.find("formation") != std::string::npos);
}

TEST_CASE("deposits: a cone across patch and tile borders meets itself to the millimetre") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  DepositLayer p = cones();
  p.talus_depth_m = 3;
  p.blocks_per_100m2 = 60;
  scene::Formation f;
  f.dip_deg = 0;
  scene::FormationBed cap;
  cap.name = "Limestone";
  cap.thickness_m = 1000;
  f.beds.push_back(cap);
  p.formation = f;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, c, whole, &error));
  desc->close(state);
  struct Read {
    DepositPatch patch;
    WorldPos at;
  };
  Vector<Read> patches;
  u32 blocks = 0;
  for (const auto& instance : whole.instances) {
    std::string text;
    REQUIRE(io::read_file(whole.meshes[instance.mesh].path, text) == io::Status::Ok);
    JsonValue json;
    REQUIRE(parse_json(text, json).ok);
    Read r;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(r.patch, json, ctx));
    r.at = instance.position;
    blocks += r.patch.blocks.size();
    CHECK(r.patch.type == RockType::Limestone);
    patches.push_back(r);
  }
  CHECK(blocks > 30);
  // Every pair of patches side by side along x: the shared column's top, in world metres, as the
  // mesh computes it (f32 local surface plus the f64 corner).
  u32 compared = 0;
  for (const auto& a : patches)
    for (const auto& b : patches) {
      const f64 span = .5 * static_cast<f64>(a.patch.width - 1);
      if (std::abs(b.at.x - (a.at.x + span)) > 1e-9 || b.at.z != a.at.z) continue;
      for (u32 z = 0; z < a.patch.depth && z < b.patch.depth; ++z) {
        const u32 ia = z * a.patch.width + a.patch.width - 1, ib = z * b.patch.width;
        if (a.patch.thickness[ia] <= .001f) continue;
        const f64 ya =
            a.at.y + static_cast<f64>(a.patch.ground[ia] + a.patch.thickness[ia] - .001f);
        const f64 yb =
            b.at.y + static_cast<f64>(b.patch.ground[ib] + b.patch.thickness[ib] - .001f);
        CHECK(std::abs(ya - yb) < 1e-3);
        ++compared;
      }
    }
  CHECK(compared > 20);
  // And a patch with blocks solves to a closed mesh: its bed and every fragment and block.
  for (const auto& r : patches)
    if (!r.patch.blocks.empty() && r.patch.thickness[0] >= 0) {
      assets::MeshData mesh;
      REQUIRE_MESSAGE(solve_deposit(r.patch, mesh, error), error);
      bool bed = false;
      for (f32 t : r.patch.thickness)
        bed = bed || t > .001f;
      if (bed) {
        checks::check_closed(mesh, 1 + r.patch.fragments + r.patch.blocks.size());
        break;
      }
    }
}

TEST_CASE("deposits: G143's plan and a patch with blocks are their pinned bytes") {
  DepositLayer p = cones();
  p.talus_depth_m = 3;
  p.blocks_per_100m2 = 60;
  DepositPlan plan;
  std::string error;
  REQUIRE(plan_deposits(p, ground(), plan, error));
  u64 h = hash_bytes(plan.thickness.data(), plan.thickness.size() * sizeof(f32));
  h = hash_combine(h, hash_bytes(plan.coarse.data(), plan.coarse.size() * sizeof(f32)));
  for (const auto& b : plan.blocks) {
    const f64 v[] = {static_cast<f64>(b.x_mm),
                     static_cast<f64>(b.z_mm),
                     b.y_m,
                     b.half[0],
                     b.half[1],
                     b.half[2],
                     b.slope[0],
                     b.slope[1],
                     b.yaw,
                     b.buried};
    h = hash_combine(h, hash_bytes(v, sizeof(v)));
  }
  for (const auto& cone : plan.cones)
    h = hash_combine(h, hash_bytes(&cone.height_m, sizeof(f64)));
  MESSAGE("G143 plan pin: ", h);
  // G143 initial MSVC pin (2026-11-10), reproduced by Clang and GCC in the containers.
  CHECK(h == 11798949088187083648ull);
  DepositPatch patch;
  patch.width = 5;
  patch.depth = 5;
  patch.fragments = 4;
  patch.triangle_budget = 4000;
  for (u32 i = 0; i < 25; ++i) {
    patch.ground.push_back(static_cast<f32>(i % 5) * .125f);
    patch.thickness.push_back(.1f);
    patch.coarse.push_back(.5f);
  }
  DepositBlock block;
  block.x_m = 2;
  block.z_m = 2;
  block.y_m = .35f;
  block.half = {.6f, .3f, .45f};
  block.slope = {.125f, 0};
  block.yaw = .7f;
  block.buried = .3f;
  block.seed = 143;
  patch.blocks.push_back(block);
  assets::MeshData mesh;
  REQUIRE_MESSAGE(solve_deposit(patch, mesh, error), error);
  checks::check_closed(mesh, 1 + patch.fragments + 1);
  MESSAGE("G143 patch mesh pin: ", checks::mesh_hash(mesh));
  CHECK(checks::mesh_hash(mesh) == 6681331591728775090ull);
  // A patch with no blocks keeps its version-1 key: the canonical bytes say nothing of them.
  DepositPatch plain = patch;
  plain.blocks.clear();
  plain.block_triangles = 999;
  DepositPatch other = plain;
  other.block_triangles = 432;
  CHECK(deposit_source_hash(plain) == deposit_source_hash(other));
}

TEST_CASE("deposits: what a tile of cones and shed blocks costs (measured, printed)") {
  // The numbers rocks.md and the grounding page quote: instances and merged meshes a 16 m tile,
  // triangles a square metre of bed at the finest and coarsest forms, and the milliseconds a
  // tile's patches take to build (the activation's cost: `tile` itself is a lookup).
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  DepositLayer p = cones();
  p.talus_depth_m = 3;
  p.blocks_per_100m2 = 30;
  p.patch.fragments = 64;
  p.patch.triangle_budget = 40000;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  void* state = nullptr;
  std::string error;
  const auto opened = std::chrono::steady_clock::now();
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  const f64 open_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - opened).count();
  scene_gen::Placements tile;
  REQUIRE(desc->tile(state, {1, 1}, c, tile, &error));  // the tile holding the middle cone's foot
  Vector<scene_gen::PlacementMesh> meshes;
  REQUIRE(desc->meshes(state, c, meshes, &error));
  desc->close(state);
  REQUIRE_FALSE(tile.instances.empty());
  u64 leaf = 0, root = 0, blocks = 0;
  f64 area = 0, build_ms = 0;
  for (const auto& instance : tile.instances) {
    std::string text;
    REQUIRE(io::read_file(meshes[instance.mesh].path, text) == io::Status::Ok);
    JsonValue json;
    REQUIRE(parse_json(text, json).ok);
    DepositPatch patch;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(patch, json, ctx));
    blocks += patch.blocks.size();
    for (u32 z = 0; z + 1 < patch.depth; ++z)
      for (u32 x = 0; x + 1 < patch.width; ++x) {
        const u32 a = z * patch.width + x;
        if (std::max({patch.thickness[a], patch.thickness[a + 1], patch.thickness[a + patch.width],
                      patch.thickness[a + patch.width + 1]}) > .001f)
          area += static_cast<f64>(patch.spacing_m) * static_cast<f64>(patch.spacing_m);
      }
    geometry::ClusterFileData data;
    const auto built = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(build_deposit_container(patch, {}, 0, data, error), error);
    build_ms +=
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - built).count();
    leaf += data.mesh.leaf_triangle_count;
    for (u32 k = 0; k < data.mesh.lod.size(); ++k)
      if (data.mesh.lod[k].parent_error == geometry::k_lod_terminal_error)
        root += data.mesh.mesh.clusters[k].triangle_count;
  }
  MESSAGE("tile (1,1): ", tile.instances.size(), " instances (merged meshes), ", blocks,
          " blocks as geometry, ", area, " m2 of bed; finest ", leaf, " triangles (",
          static_cast<f64>(leaf) / area, " a m2), coarsest ", root, " (",
          static_cast<f64>(root) / area, " a m2); region open ", open_ms,
          " ms, the tile's patches ", build_ms, " ms");
  CHECK(tile.instances.size() <= 4);  // a 16 m tile of 8 m patches: four merged meshes at most
  CHECK(root < leaf);
}
