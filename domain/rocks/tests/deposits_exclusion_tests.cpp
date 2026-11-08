// Deposits leave a hole bare and nothing else (roadmap G135; docs/subsystems/scene_gen.md,
// "Exclusion"; rocks.md): gravel and rubble in a stream's bed and under water are what a stream and
// a lake are, so only a patch wholly over a hole is dropped; a scene with none places every patch.
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

using namespace engine;
using namespace engine::rocks;

namespace {

f64 cliff(const void*, i64 x, i64) noexcept {
  const f64 local = static_cast<f64>(x) / 1000.0;
  return local < 16 ? 0 : (local > 24 ? 16 : (local - 16) * 2);
}

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("deposits");
  void* state = nullptr;
  ~Opened() {
    if (state != nullptr) desc->close(state);
  }
};

}  // namespace

TEST_CASE(
    "deposits exclusion: a hole over the region takes the patches, water and a stream do not") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &cliff;
  c.tile_size = 16;
  DepositLayer layer;
  layer.region_max = {32, 32};
  layer.patch.fragments = 8;
  layer.patch.triangle_budget = 10000;
  Opened opened;
  REQUIRE(opened.desc != nullptr);
  std::string error;
  REQUIRE_MESSAGE(opened.desc->open(schema::to_json(layer), c, &opened.state, &error), error);
  scene_gen::Placements base;
  REQUIRE(opened.desc->expand(opened.state, c, base, &error));
  REQUIRE_FALSE(base.instances.empty());

  const auto expand = [&](const scene_gen::Exclusion& ex) {
    c.exclusion = &ex;
    scene_gen::Placements out;
    REQUIRE_MESSAGE(opened.desc->expand(opened.state, c, out, &error), error);
    return out;
  };

  // A hole round the whole region leaves no patch.
  scene::TerrainHole big;
  big.center = WorldPos{16.0, 0.0, 16.0};
  big.radius_m = 60.0f;
  scene_gen::Exclusion all_hole;
  REQUIRE(all_hole.build({&big, 1}, {}, {}));
  CHECK(expand(all_hole).instances.empty());

  // A hole nowhere near, a lake over all of it with the ground below its level, and a stream
  // across it: every patch, at the same place.
  scene::TerrainHole away;
  away.center = WorldPos{900.0, 0.0, 900.0};
  away.radius_m = 3.0f;
  scene::WaterBody lake;
  lake.level_m = 100.0;
  scene::WaterCircle circle;
  circle.center = WorldPos{16.0, 100.0, 16.0};
  circle.radius_m = 80.0f;
  lake.shape.circle = circle;
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{-10.0, 0.0, 16.0});
  channel.points.push_back(WorldPos{50.0, 0.0, 16.0});
  channel.width_m.push_back(4.0f);
  channel.width_m.push_back(4.0f);
  scene_gen::Exclusion rest;
  REQUIRE(rest.build({&away, 1}, {&lake, 1}, {&channel, 1}));
  const auto same = expand(rest);
  REQUIRE(same.instances.size() == base.instances.size());
  for (u32 i = 0; i < base.instances.size(); ++i) {
    CHECK(same.instances[i].id == base.instances[i].id);
    CHECK(same.instances[i].position == base.instances[i].position);
  }
}
