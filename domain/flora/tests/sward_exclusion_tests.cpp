// The sward leaves a scene's holes, channels and water empty and thins towards a bank (roadmap
// G135; docs/subsystems/scene_gen.md, "Exclusion"; flora.md, "Where the sward grows"). A scene with
// none of the three places what it always did, patch for patch.
#include <core/schema/json_reflect.h>
#include <domain/flora/sward.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;

namespace {

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

f64 flat(const void*, i64, i64) noexcept { return 1.0; }

// One 64 m tile of 1 m patches, all grass, on flat ground at 1 m; one density family.
struct Meadow {
  MaskGround ground;
  scene_gen::Context ctx;
  void* state = nullptr;
  const scene_gen::PlacementGeneratorDesc* gen = nullptr;
  Meadow() {
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
    ctx.ground.surface_fn = &flat;
    ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
      out = {&k_masks, s};
      return true;
    };
    flora::SwardPlacements p;
    p.layer = 1;
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
  scene_gen::Placements tile(const scene_gen::Exclusion* exclusion) {
    ctx.exclusion = exclusion;
    scene_gen::Placements out;
    std::string error;
    REQUIRE_MESSAGE(gen->tile(state, {0, 0}, ctx, out, &error), error);
    return out;
  }
};

bool same(const scene_gen::Placement& a, const scene_gen::Placement& b) {
  return a.id == b.id && a.mesh == b.mesh &&
         std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
         std::memcmp(&a.rotation, &b.rotation, sizeof(a.rotation)) == 0;
}

const scene_gen::Placement* find(const scene_gen::Placements& in, u32 id) {
  for (const auto& p : in.instances)
    if (p.id == id) return &p;
  return nullptr;
}

}  // namespace

TEST_CASE("sward exclusion: a 6 m hole takes its patches and no others") {
  Meadow meadow;
  const auto base = meadow.tile(nullptr);
  REQUIRE(base.instances.size() == 4096);

  scene_gen::Exclusion ex;
  scene::TerrainHole hole;
  hole.center = WorldPos{32.0, 0.0, 32.0};
  hole.radius_m = 3.0f;
  REQUIRE(ex.build({&hole, 1}, {}, {}));
  const auto holed = meadow.tile(&ex);

  // A patch within a hair of the rim is rounded to a millimetre before it is asked, so it may go
  // either way; the rest are decided.
  u32 expected_inside = 0, rim = 0;
  for (const auto& p : base.instances) {
    const f64 dx = p.position.x - 32.0, dz = p.position.z - 32.0;
    const f64 r2 = dx * dx + dz * dz;
    const bool inside = r2 < 9.0 - 0.05;
    expected_inside += inside ? 1u : 0u;
    rim += std::fabs(r2 - 9.0) <= 0.05 ? 1u : 0u;
    const auto* kept = find(holed, p.id);
    if (inside) {
      CHECK(kept == nullptr);
    } else if (r2 > 9.0 + 0.05) {
      // Outside the hole, the patch is the one it was, to the bit.
      REQUIRE(kept != nullptr);
      CHECK(same(p, *kept));
    }
  }
  CHECK(expected_inside > 20);  // about pi 3^2
  CHECK(holed.instances.size() <= 4096 - expected_inside);
  CHECK(holed.instances.size() >= 4096 - expected_inside - rim);
  for (const auto& p : holed.instances) {
    const f64 dx = p.position.x - 32.0, dz = p.position.z - 32.0;
    CHECK(dx * dx + dz * dz >= 9.0 - 0.05);
  }
}

TEST_CASE("sward exclusion: a scene with none places what it always did, patch for patch") {
  Meadow meadow;
  const auto base = meadow.tile(nullptr);
  // An exclusion with nothing in it, and one whose hole, channel and lake are nowhere near.
  scene_gen::Exclusion empty;
  REQUIRE(empty.build({}, {}, {}));
  scene_gen::Exclusion far;
  scene::TerrainHole hole;
  hole.center = WorldPos{900.0, 0.0, 900.0};
  hole.radius_m = 3.0f;
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{-500.0, 0.0, -500.0});
  channel.points.push_back(WorldPos{-400.0, 0.0, -500.0});
  channel.width_m.push_back(3.0f);
  channel.width_m.push_back(3.0f);
  scene::WaterBody lake;
  lake.level_m = 50.0;
  scene::WaterCircle circle;
  circle.center = WorldPos{2000.0, 50.0, 2000.0};
  circle.radius_m = 20.0f;
  lake.shape.circle = circle;
  REQUIRE(far.build({&hole, 1}, {&lake, 1}, {&channel, 1}));
  for (const scene_gen::Exclusion* ex : {&empty, &far}) {
    const auto out = meadow.tile(ex);
    REQUIRE(out.instances.size() == base.instances.size());
    for (u32 i = 0; i < base.instances.size(); ++i)
      CHECK(same(base.instances[i], out.instances[i]));
  }
}

TEST_CASE("sward exclusion: a stream's bed and banks are bare and its margin is sparser") {
  Meadow meadow;
  const auto base = meadow.tile(nullptr);
  scene_gen::Exclusion ex;
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{-10.0, 0.0, 32.0});
  channel.points.push_back(WorldPos{80.0, 0.0, 32.0});
  channel.width_m.push_back(3.0f);
  channel.width_m.push_back(3.0f);
  REQUIRE(ex.build({}, {}, {&channel, 1}));
  const auto out = meadow.tile(&ex);
  // Bank edge: 1.5 + 0.25 * 3 + 0.4 = 2.65 m from the line.
  u32 margin_base = 0, margin_kept = 0, far_base = 0, far_kept = 0;
  for (const auto& p : base.instances) {
    const f64 d = std::fabs(p.position.z - 32.0);
    const auto* kept = find(out, p.id);
    if (std::fabs(d - 2.65) < 0.01 || std::fabs(d - 5.65) < 0.01) continue;
    if (d < 2.65) {
      CHECK(kept == nullptr);
    } else if (d < 2.65 + 3.0) {
      ++margin_base;
      margin_kept += kept != nullptr ? 1u : 0u;
      if (kept != nullptr) CHECK(same(p, *kept));
    } else {
      ++far_base;
      far_kept += kept != nullptr ? 1u : 0u;
      REQUIRE(kept != nullptr);
      CHECK(same(p, *kept));
    }
  }
  CHECK(far_kept == far_base);
  REQUIRE(margin_base > 300);
  // The factor runs from 0.2 at the edge to 1 at 3 m on a smoothstep: its mean is about 0.625.
  const f64 kept_fraction = static_cast<f64>(margin_kept) / static_cast<f64>(margin_base);
  CHECK(kept_fraction > 0.5);
  CHECK(kept_fraction < 0.75);
}

TEST_CASE("sward exclusion: nothing grows under a lake's water") {
  Meadow meadow;  // the ground is 1 m
  scene::WaterBody lake;
  lake.level_m = 2.0;
  scene::WaterCircle circle;
  circle.center = WorldPos{32.0, 2.0, 32.0};
  circle.radius_m = 10.0f;
  lake.shape.circle = circle;
  scene_gen::Exclusion ex;
  REQUIRE(ex.build({}, {&lake, 1}, {}));
  const auto out = meadow.tile(&ex);
  REQUIRE(out.instances.size() < 4096);
  u32 inside = 0;
  for (const auto& p : out.instances) {
    const f64 dx = p.position.x - 32.0, dz = p.position.z - 32.0;
    CHECK(dx * dx + dz * dz >= 100.0 - 0.05);
    inside += dx * dx + dz * dz < 100.0 - 0.05 ? 1u : 0u;
  }
  CHECK(inside == 0);
  CHECK(out.instances.size() > 4096 - 340);  // pi 10^2 = 314 patches gone, no more
}
