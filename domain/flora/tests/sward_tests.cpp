#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/flora.h>
#include <domain/flora/sward.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;

TEST_CASE("sward: source dimensions, finite cell bounds, budget and parentless motion") {
  for (const auto& profile : flora::sward_profiles()) {
    flora::Sward p;
    p.species = profile.name;
    p.fit_lods = false;  // dimensions/bounds are independent of the separate LOD-fit checks
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    flora::TreeLevels levels;
    Vector<geometry::ClusterFileSwardVertex> attrs;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_sward(p, mesh, &report, &error, &motion, &levels, &attrs), error);
    CHECK(report.triangles <= 2400);
    CHECK(report.sprays == 256);
    REQUIRE(levels.levels.size() == 2);
    REQUIRE(attrs.size() == mesh.positions.size());
    REQUIRE(motion.vertex_part.size() == mesh.positions.size());
    for (u32 i = 0; i < report.sprays; ++i) {
      CHECK(motion.parts[i].parent == geometry::k_motion_no_parent);
      CHECK(motion.parts[i].length >= profile.length_min_m);
      CHECK(motion.parts[i].length <= profile.length_max_m);
      CHECK(std::abs(motion.parts[i].pivot.x) <= .500001f);
      CHECK(std::abs(motion.parts[i].pivot.z) <= .500001f);
      // Each ribbon has seven vertices; its base span is the botanical width.
      const f32 width = length(mesh.positions[i * 7] - mesh.positions[i * 7 + 1]);
      CHECK(width >= profile.width_min_m - 1e-7f);
      CHECK(width <= profile.width_max_m + 1e-7f);
      CHECK(motion.vertex_weight[i * 7] == 0);
      CHECK(motion.vertex_weight[i * 7 + 6] == doctest::Approx(1));
    }
    for (const auto& v : mesh.positions) {
      CHECK(std::isfinite(v.x));
      CHECK(std::isfinite(v.y));
      CHECK(std::isfinite(v.z));
      const f32 bound = .500001f + profile.length_max_m + profile.width_max_m;
      CHECK(std::abs(v.x) <= bound);
      CHECK(std::abs(v.z) <= bound);
    }
    for (const auto& part : motion.structure)
      CHECK(part.flags == 0);
  }
}

TEST_CASE("sward: invalid external recipes and insufficient total budgets are refused") {
  flora::Sward p;
  std::string error;
  p.density = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(flora::sward_in_range(p, error));
  p = {};
  p.leaves_per_tiller = 0;
  CHECK_FALSE(flora::sward_in_range(p, error));
  p = {};
  p.growth_min = .9f;
  p.growth = .5f;
  CHECK_FALSE(flora::sward_in_range(p, error));
  p = {};
  p.cut_height_m = std::numeric_limits<f32>::infinity();
  CHECK_FALSE(flora::sward_in_range(p, error));
  p = {};
  p.species = "not_a_grass";
  CHECK_FALSE(flora::sward_in_range(p, error));
  p = {};
  p.triangle_budget = 20;
  assets::MeshData mesh;
  CHECK_FALSE(flora::solve_sward(p, mesh, nullptr, &error, nullptr));
  CHECK(mesh.positions.empty());
}

TEST_CASE("sward: dense cover groups leaves into shoots and clips rather than shrinks them") {
  flora::Sward p;
  p.species = "tall_fescue";
  p.fit_lods = false;
  p.blades_m2 = 1024;
  p.growth_min = .45f;
  p.triangle_budget = 11000;
  assets::MeshData natural, clipped;
  flora::Motion natural_motion, cut_motion;
  flora::TreeReport report;
  flora::TreeLevels levels;
  std::string error;
  REQUIRE(flora::solve_sward(p, natural, &report, &error, &natural_motion, &levels));
  CHECK(report.leaf_area_m2 > 2);
  CHECK(report.triangles <= p.triangle_budget);
  u32 overhang[4]{};
  for (const auto& v : natural.positions) {
    overhang[0] += v.x < -.5f ? 1u : 0u;
    overhang[1] += v.x > .5f ? 1u : 0u;
    overhang[2] += v.z < -.5f ? 1u : 0u;
    overhang[3] += v.z > .5f ? 1u : 0u;
  }
  for (u32 vertices : overhang)
    CHECK(vertices > 16);  // no bald cell-edge confinement
  for (u32 i = 0; i < p.blades_m2; ++i) {
    const auto& root = natural_motion.parts[i].pivot;
    const auto& shared = natural_motion.parts[i - i % p.leaves_per_tiller].pivot;
    CHECK(root.x == shared.x);
    CHECK(root.y == shared.y);
    CHECK(root.z == shared.z);
  }
  p.cut_height_m = .05f;
  REQUIRE(flora::solve_sward(p, clipped, &report, &error, &cut_motion, &levels));
  CHECK(report.triangles <= p.triangle_budget);
  REQUIRE(clipped.primitives[0].index_count == 1024 * 6 * 3);
  for (u32 i = 0; i < p.blades_m2; ++i) {
    // Eight vertices per clipped ribbon: the end keeps two distinct vertices at the cut plane.
    CHECK(clipped.positions[i * 8 + 6].y == doctest::Approx(.05));
    CHECK(clipped.positions[i * 8 + 7].y == doctest::Approx(.05));
    CHECK(length(clipped.positions[i * 8 + 6] - clipped.positions[i * 8 + 7]) > .0029f);
    CHECK(length(clipped.positions[i * 8] - clipped.positions[i * 8 + 1]) ==
          doctest::Approx(length(natural.positions[i * 7] - natural.positions[i * 7 + 1]))
              .epsilon(.0001));
    CHECK(cut_motion.vertex_weight[i * 8 + 6] == doctest::Approx(1));
    CHECK(cut_motion.parts[i].length < natural_motion.parts[i].length);
  }
  p.cut_variation = .5f;
  REQUIRE(flora::solve_sward(p, clipped, &report, &error, &cut_motion, &levels));
  f32 min_height = 1, max_height = 0;
  for (u32 i = 0; i < p.blades_m2; ++i) {
    const f32 height = (clipped.positions[i * 8 + 6].y + clipped.positions[i * 8 + 7].y) * .5f;
    min_height = std::min(min_height, height);
    max_height = std::max(max_height, height);
  }
  CHECK(min_height >= .025f - 1e-6f);
  CHECK(max_height <= .075f + 1e-6f);
  CHECK(max_height - min_height > .045f);
}

TEST_CASE("sward: full-density reference patch holds four-view projected coverage") {
  flora::Sward p;
  p.seed = 8801;
  assets::MeshData mesh;
  flora::TreeLevels levels;
  std::string error;
  REQUIRE(flora::solve_sward(p, mesh, nullptr, &error, nullptr, &levels));
  const auto coverage = flora::measure_sward_coverage(mesh, levels);
  // This pins the measured reference, not acceptance of every species/density/variant.
  for (u32 view = 0; view < 4; ++view) {
    REQUIRE(coverage.fraction[0][view] > 0);
    CHECK(std::abs(coverage.fraction[1][view] / coverage.fraction[0][view] - 1) < .01f);
    CHECK(std::abs(coverage.fraction[2][view] / coverage.fraction[0][view] - 1) < .05f);
  }
}

TEST_CASE("sward: section 41 survives container roundtrip in cluster vertex order") {
  flora::FloraFile file;
  file.species = "sward";
  file.sward.blades_m2 = 32;
  file.sward.card_px = 32;
  geometry::ClusterFileData data, back;
  std::string error;
  REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 0, data, nullptr, error), error);
  REQUIRE(data.sward_vertices.size() == data.mesh.mesh.vertices.size());
  REQUIRE(!data.sward_vertices.empty());
  CHECK_FALSE(flora::flora_collides(file));
  CHECK(data.impostors.size() == 1);
  test::TempDir temp("sward");
  REQUIRE(geometry::write_cluster_file(temp.file("patch.clusters"), data, &error));
  REQUIRE_MESSAGE(geometry::read_cluster_file(temp.file("patch.clusters"), back, &error), error);
  REQUIRE(back.sward_vertices.size() == data.sward_vertices.size());
  CHECK(std::memcmp(back.sward_vertices.data(), data.sward_vertices.data(),
                    data.sward_vertices.size() * sizeof(geometry::ClusterFileSwardVertex)) == 0);
  const u64 pin = hash_bytes(data.sward_vertices.data(), data.sward_vertices.size() * 4);
  // G90 v5: tillered arched blades, coarse curve rung and remapped card vertices.
  // v4: 17839441404926445449; new arc stations change the flex/AO bytes and cluster ordering.
  CHECK(pin == 8707601967473198806ull);
}

namespace {
u64 family_pin() {
  u64 pin = k_hash_seed;
  for (u32 variant = 0; variant < 8; ++variant) {
    flora::Sward p;
    p.variant = variant;
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeLevels levels;
    Vector<geometry::ClusterFileSwardVertex> attrs;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_sward(p, mesh, nullptr, &error, &motion, &levels, &attrs), error);
    pin =
        hash_combine(pin, hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3)));
    pin = hash_combine(pin, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
    pin = hash_combine(pin, hash_bytes(attrs.data(), attrs.size() * 4));
    for (const auto& level : levels.levels) {
      pin = hash_combine(pin, hash_bytes(level.indices.data(), level.indices.size() * sizeof(u32)));
      pin = hash_combine(pin, hash_bytes(&level.alpha_cutoff, sizeof(level.alpha_cutoff)));
    }
  }
  return pin;
}
struct MaskGround {
  scene_gen::GroundMaskInfo info;
  Vector<u32> words;
};
const scene_gen::GroundMaskOps k_masks{
    .present = [](const void*, i32, i32) noexcept { return true; },
    .info =
        [](const void* state, scene_gen::GroundMaskInfo& out) noexcept {
          out = static_cast<const MaskGround*>(state)->info;
          return true;
        },
    .tile =
        [](const void* state, i32, i32, std::span<u32> out) noexcept {
          const auto& g = *static_cast<const MaskGround*>(state);
          for (u32 i = 0; i < g.words.size(); ++i)
            out[i] = g.words[i];
          return true;
        }};
}  // namespace
TEST_CASE("sward: eight-variant geometry and attribute family is deterministic") {
  const u64 pin = family_pin();
  // V5 adds fitted card cutoff to positions, all three index streams and source attributes.
  // v4 before the tiller/fan model and LOD fit: 3702055350956951526.
  CHECK(pin == 2792296357357641801ull);
}
TEST_CASE("sward: palette density, stable IDs across tile order and empty outer rings") {
  MaskGround ground;
  ground.info.cells = 4;
  ground.info.spacing_mm = 1000;
  ground.info.palette = 2;
  ground.info.tile_min_x = -1;
  ground.info.tile_max_x = 1;
  ground.info.tile_max_z = 0;
  ground.words.resize(scene_gen::mask_tile_words(4), 0);
  ground.words[0] = 0xffff0001u;  // grass is palette 1, channel 0, not channel 1
  for (u32 i = 0; i < scene_gen::mask_points(4); ++i)
    ground.words[i + 1] = 255;
  scene_gen::Context ctx;
  ctx.tile_size = 4;
  ctx.ground.context = &ground;
  ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
    out = {&k_masks, s};
    return true;
  };
  flora::SwardPlacements p;
  p.layer = 1;
  p.min_x = -1;
  p.max_x = 1;
  flora::SwardFamily family;
  family.near.push_back("near.clusters");
  family.cards.push_back("cards.clusters");
  p.families.push_back(family);
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
  REQUIRE(gen != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(gen->open(schema::to_json(p), ctx, &state, &error), error);
  scene_gen::Placements first, other, again;
  REQUIRE(gen->tile(state, {-1, 0}, ctx, first, &error));
  REQUIRE(gen->tile(state, {1, 0}, ctx, other, &error));
  REQUIRE(gen->tile(state, {-1, 0}, ctx, again, &error));
  REQUIRE(first.instances.size() == 16);
  REQUIRE(again.instances.size() == 16);
  for (u32 i = 0; i < 16; ++i) {
    CHECK(first.instances[i].id == i + 1);
    CHECK(first.instances[i].id == again.instances[i].id);
    CHECK(first.instances[i].position.x == again.instances[i].position.x);
    CHECK(first.instances[i].id != other.instances[i].id);
  }
  ctx.ring = 1;
  REQUIRE(gen->tile(state, {-1, 0}, ctx, again, &error));
  REQUIRE(again.instances.size() == 16);
  CHECK(again.instances[0].mesh == 1);
  CHECK(again.instances[0].id == first.instances[0].id);
  ctx.ring = 2;
  REQUIRE(gen->tile(state, {-1, 0}, ctx, again, &error));
  CHECK(again.instances.empty());
  ctx.ring = 0;
  for (u32 i = 0; i < scene_gen::mask_points(4); ++i)
    ground.words[i + 1] = 255u << 8;
  REQUIRE(gen->tile(state, {-1, 0}, ctx, again, &error));
  CHECK(again.instances.empty());
  gen->close(state);
}

namespace {
constexpr f64 k_tan10 = 0.17632698070846498;
constexpr f64 k_tan20 = 0.36397023426620234;
f64 rising_20(const void*, i64 x_mm, i64) noexcept {
  return k_tan20 * static_cast<f64>(x_mm) * .001;
}
f64 rising_10(const void*, i64 x_mm, i64) noexcept {
  return k_tan10 * static_cast<f64>(x_mm) * .001;
}
f64 level(const void*, i64, i64) noexcept { return 3.0; }

// One 64 m tile of 1 m patches, all grass, over `surface`, placed with `max_tilt_deg`.
struct Meadow {
  MaskGround ground;
  scene_gen::Context ctx;
  flora::SwardPlacements p;
  void* state = nullptr;
  const scene_gen::PlacementGeneratorDesc* gen = nullptr;
  Meadow(scene_gen::HeightFn surface, f32 max_tilt_deg) {
    ground.info.cells = 64;
    ground.info.spacing_mm = 1000;
    ground.info.palette = 2;
    ground.info.tile_max_x = 0;
    ground.info.tile_max_z = 0;
    ground.words.resize(scene_gen::mask_tile_words(64), 0);
    ground.words[0] = 0xffff0001u;
    for (u32 i = 0; i < scene_gen::mask_points(64); ++i)
      ground.words[i + 1] = 255;
    ctx.tile_size = 64;
    ctx.ground.context = &ground;
    ctx.ground.surface_fn = surface;
    ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
      out = {&k_masks, s};
      return true;
    };
    p.layer = 1;
    p.max_tilt_deg = max_tilt_deg;
    flora::SwardFamily family;
    family.near.push_back("near.clusters");
    family.cards.push_back("cards.clusters");
    p.families.push_back(family);
    gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
    std::string error;
    REQUIRE(gen != nullptr);
    REQUIRE_MESSAGE(gen->open(schema::to_json(p), ctx, &state, &error), error);
  }
  ~Meadow() {
    if (gen != nullptr && state != nullptr) gen->close(state);
  }
  scene_gen::Placements tile(u8 ring = 0) {
    ctx.ring = ring;
    scene_gen::Placements out;
    std::string error;
    REQUIRE_MESSAGE(gen->tile(state, {0, 0}, ctx, out, &error), error);
    return out;
  }
};
// The angle between a patch's up and the world's, degrees.
f64 tilt_deg(const scene_gen::Placement& placed) {
  const Vec3 up = rotate(placed.rotation, Vec3::unit_y());
  return std::acos(std::min(1.0, static_cast<f64>(up.y))) * 57.29577951308232;
}
}  // namespace

TEST_CASE("sward: a patch's place and turn are its hash's, in every ring and request order") {
  Meadow first(&rising_20, 15.0f), second(&rising_20, 15.0f);
  const auto a0 = first.tile(0);
  const auto a1 = first.tile(1);
  const auto b1 = second.tile(1);  // the other order, on a fresh state
  const auto b0 = second.tile(0);
  const auto a0_again = first.tile(0);
  REQUIRE(a0.instances.size() == 64 * 64);
  REQUIRE(a1.instances.size() == a0.instances.size());
  for (u32 i = 0; i < a0.instances.size(); ++i) {
    const auto same = [&](const scene_gen::Placement& l, const scene_gen::Placement& r) {
      return l.id == r.id && std::memcmp(&l.position, &r.position, sizeof(l.position)) == 0 &&
             l.rotation == r.rotation && l.scale == r.scale;
    };
    CHECK(same(a0.instances[i], a0_again.instances[i]));
    CHECK(same(a0.instances[i], a1.instances[i]));
    CHECK(same(a0.instances[i], b0.instances[i]));
    CHECK(same(a0.instances[i], b1.instances[i]));
  }
}

TEST_CASE("sward: patches turn uniformly about their axes and stay inside a quarter cell") {
  Meadow meadow(&level, 15.0f);
  const auto out = meadow.tile();
  REQUIRE(out.instances.size() == 64 * 64);
  u32 bins[8] = {};
  f64 cos_sum = 0, sin_sum = 0, worst = 0;
  bool all_equal = true;
  for (const auto& placed : out.instances) {
    // Level ground: no tilt, so the turn is the yaw alone, 2 atan2(q.y, q.w).
    CHECK(std::fabs(placed.rotation.x) < 1e-6f);
    CHECK(std::fabs(placed.rotation.z) < 1e-6f);
    f64 yaw =
        2.0 * std::atan2(static_cast<f64>(placed.rotation.y), static_cast<f64>(placed.rotation.w));
    if (yaw < 0) yaw += 6.283185307179586;
    all_equal = all_equal && placed.rotation == out.instances[0].rotation;
    ++bins[std::min(7u, static_cast<u32>(yaw / 6.283185307179586 * 8))];
    cos_sum += std::cos(yaw);
    sin_sum += std::sin(yaw);
    CHECK(placed.scale == Vec3::one());
    // Cell (x, z) is id - 1 = z * 64 + x; its centre is at (x + .5, z + .5).
    const u32 cell = placed.id - 1;
    worst = std::max(worst, std::fabs(placed.position.x - (cell % 64 + .5)));
    worst = std::max(worst, std::fabs(placed.position.z - (cell / 64 + .5)));
    CHECK(placed.position.y == 3.0);
  }
  CHECK_FALSE(all_equal);
  // 4096 turns: each eighth of the circle holds 512 (sigma 21), the mean direction ~0 (sigma
  // 0.011).
  for (u32 count : bins) {
    CHECK(count > 512 - 90);
    CHECK(count < 512 + 90);
  }
  CHECK(std::fabs(cos_sum) / 4096 < 0.05);
  CHECK(std::fabs(sin_sum) / 4096 < 0.05);
  // Moved, and never more than a quarter cell a side.
  CHECK(worst <= 0.25 + 1e-9);
  CHECK(worst > 0.24);
}

TEST_CASE("sward: a patch tips onto the slope, capped, and a cap of zero keeps it upright") {
  {
    Meadow steep(&rising_20, 15.0f);  // 20 degrees of slope, a 15 degree cap
    for (const auto& placed : steep.tile().instances) {
      CHECK(tilt_deg(placed) == doctest::Approx(15.0).epsilon(0.001));
      // The ground rises with +x, so its normal and the patch's up lean toward -x, not along z.
      const Vec3 up = rotate(placed.rotation, Vec3::unit_y());
      CHECK(up.x < 0);
      CHECK(std::fabs(up.z) < 1e-4f);
    }
  }
  {
    Meadow gentle(&rising_10, 15.0f);  // under the cap: the full slope
    for (const auto& placed : gentle.tile().instances)
      CHECK(tilt_deg(placed) == doctest::Approx(10.0).epsilon(0.001));
  }
  {
    Meadow wide(&rising_20, 45.0f);  // the cap above the slope: the slope, 20 degrees
    for (const auto& placed : wide.tile().instances)
      CHECK(tilt_deg(placed) == doctest::Approx(20.0).epsilon(0.001));
  }
  {
    Meadow upright(&rising_20, 0.0f);  // today's flat patch, still turned and moved
    for (const auto& placed : upright.tile().instances) {
      const Vec3 up = rotate(placed.rotation, Vec3::unit_y());
      CHECK(up.x == 0.0f);
      CHECK(up.z == 0.0f);
      CHECK(up.y == doctest::Approx(1.0));
    }
  }
}

TEST_CASE("sward: a tilt cap out of range is refused") {
  scene_gen::Context ctx;
  ctx.tile_size = 4;
  flora::SwardPlacements p;
  flora::SwardFamily family;
  family.near.push_back("n.clusters");
  family.cards.push_back("c.clusters");
  p.families.push_back(family);
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
  REQUIRE(gen != nullptr);
  for (const f32 bad : {-1.0f, 45.5f}) {
    p.max_tilt_deg = bad;
    void* state = nullptr;
    std::string error;
    CHECK_FALSE(gen->open(schema::to_json(p), ctx, &state, &error));
    CHECK(error.find("max_tilt_deg") != std::string::npos);
  }
}
