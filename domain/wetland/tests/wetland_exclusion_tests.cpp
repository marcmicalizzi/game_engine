// The water garden stands on no hole (roadmap G135; docs/subsystems/scene_gen.md, "Exclusion";
// wetland.md): a hole takes the plants over it and no others, while a lake or a stream, which a
// reed and a lily are there to stand in, take none; and a scene with none places every plant.
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/wetland/wetland.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;

TEST_CASE("wetland exclusion: a hole takes its plants, water and a stream take none") {
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("wetland");
  REQUIRE(gen != nullptr);
  wetland::Patch patch;
  patch.meshes.push_back("reed.wetland.json");
  patch.density = 1;
  scene_gen::Context context;
  void* state = nullptr;
  std::string error;
  REQUIRE(gen->open(schema::to_json(patch), context, &state, &error));
  scene_gen::Placements base;
  REQUIRE(gen->expand(state, context, base, &error));
  REQUIRE(base.instances.size() > 20);

  // A hole over the middle of the patch.
  f64 cx = 0, cz = 0;
  for (const auto& p : base.instances) {
    cx += p.position.x;
    cz += p.position.z;
  }
  cx /= static_cast<f64>(base.instances.size());
  cz /= static_cast<f64>(base.instances.size());
  scene::TerrainHole hole;
  hole.center = WorldPos{cx, 0.0, cz};
  hole.radius_m = 3.0f;
  scene_gen::Exclusion holed;
  REQUIRE(holed.build({&hole, 1}, {}, {}));
  context.exclusion = &holed;
  scene_gen::Placements out;
  REQUIRE(gen->expand(state, context, out, &error));
  u32 inside = 0;
  for (const auto& p : base.instances) {
    const f64 dx = p.position.x - cx, dz = p.position.z - cz;
    const f64 r2 = dx * dx + dz * dz;
    inside += r2 < 9.0 - 0.05 ? 1u : 0u;
    if (r2 > 9.0 + 0.05) {
      bool kept = false;
      for (const auto& q : out.instances)
        kept = kept || (q.id == p.id && q.position == p.position);
      CHECK(kept);
    }
  }
  CHECK(inside > 0);
  CHECK(out.instances.size() < base.instances.size());
  for (const auto& q : out.instances) {
    const f64 dx = q.position.x - cx, dz = q.position.z - cz;
    CHECK(dx * dx + dz * dz >= 9.0 - 0.05);
  }

  // A lake over all of it (the ground is flat at 0, below the level) and a stream through it, and
  // a hole nowhere near: every plant.
  scene::WaterBody lake;
  lake.level_m = 1.0;
  scene::WaterCircle circle;
  circle.center = WorldPos{cx, 1.0, cz};
  circle.radius_m = 500.0f;
  lake.shape.circle = circle;
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{cx - 50, 0.0, cz});
  channel.points.push_back(WorldPos{cx + 50, 0.0, cz});
  channel.width_m.push_back(4.0f);
  channel.width_m.push_back(4.0f);
  scene::TerrainHole away;
  away.center = WorldPos{cx + 9000, 0.0, cz + 9000};
  away.radius_m = 3.0f;
  scene_gen::Exclusion rest;
  REQUIRE(rest.build({&away, 1}, {&lake, 1}, {&channel, 1}));
  context.exclusion = &rest;
  scene_gen::Placements same;
  REQUIRE(gen->expand(state, context, same, &error));
  REQUIRE(same.instances.size() == base.instances.size());
  for (u32 i = 0; i < base.instances.size(); ++i) {
    CHECK(same.instances[i].id == base.instances[i].id);
    CHECK(same.instances[i].position == base.instances[i].position);
  }
  gen->close(state);
}
