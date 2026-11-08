// Litter lies on no hole, in no stream's bed and under no water (roadmap G135; docs/subsystems/
// scene_gen.md, "Exclusion"; litter.md), and a scene with none of the three places what it always
// did.
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;

namespace {

struct Run {
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement("litter");
  void* state = nullptr;
  scene_gen::Context c;
  Run() {
    litter::Layer p;
    p.meshes.push_back("leaf.litter.json");
    p.background = 1;
    p.max_x = 1;
    c.tile_size = 16;
    std::string error;
    REQUIRE(g != nullptr);
    REQUIRE_MESSAGE(g->open(schema::to_json(p), c, &state, &error), error);
  }
  ~Run() {
    if (state != nullptr) g->close(state);
  }
  scene_gen::Placements expand(const scene_gen::Exclusion* ex) {
    c.exclusion = ex;
    scene_gen::Placements out;
    std::string error;
    REQUIRE_MESSAGE(g->expand(state, c, out, &error), error);
    return out;
  }
  scene_gen::Placements tile(scene_gen::TileCoord t, const scene_gen::Exclusion* ex) {
    c.exclusion = ex;
    scene_gen::Placements out;
    std::string error;
    REQUIRE_MESSAGE(g->tile(state, t, c, out, &error), error);
    return out;
  }
};

const scene_gen::Placement* find(const scene_gen::Placements& in, u32 id) {
  for (const auto& p : in.instances)
    if (p.id == id) return &p;
  return nullptr;
}

}  // namespace

TEST_CASE("litter exclusion: nothing across a 3 m channel, the same everywhere else") {
  Run run;
  const auto base = run.expand(nullptr);
  REQUIRE(base.instances.size() > 20);

  scene_gen::Exclusion ex;
  scene::WaterChannel channel;
  channel.points.push_back(WorldPos{-5.0, 0.0, 8.0});
  channel.points.push_back(WorldPos{40.0, 0.0, 8.0});
  channel.width_m.push_back(3.0f);
  channel.width_m.push_back(3.0f);
  REQUIRE(ex.build({}, {}, {&channel, 1}));
  const auto out = run.expand(&ex);

  // The bed and banks: within 1.5 + 0.75 + 0.4 = 2.65 m of the line.
  u32 removed = 0;
  for (const auto& p : base.instances) {
    const f64 d = std::fabs(p.position.z - 8.0);
    if (std::fabs(d - 2.65) < 0.01) continue;
    const auto* kept = find(out, p.id);
    if (d < 2.65) {
      CHECK(kept == nullptr);
      ++removed;
    } else {
      REQUIRE(kept != nullptr);
      CHECK(kept->position == p.position);
      CHECK(kept->mesh == p.mesh);
    }
  }
  CHECK(removed > 3);
  for (const auto& p : out.instances)
    CHECK(std::fabs(p.position.z - 8.0) >= 2.65 - 0.01);

  // A tile read agrees with the whole read: the streamed world and the scene file place alike.
  const auto t0 = run.tile({0, 0}, &ex);
  const auto t1 = run.tile({1, 0}, &ex);
  CHECK(t0.instances.size() + t1.instances.size() == out.instances.size());
}

TEST_CASE("litter exclusion: none over a hole or under a lake, and nothing moves without them") {
  Run run;
  const auto base = run.expand(nullptr);

  scene_gen::Exclusion ex;
  scene::TerrainHole hole;
  hole.center = WorldPos{8.0, 0.0, 8.0};
  hole.radius_m = 4.0f;
  scene::WaterBody lake;
  lake.level_m = 1.0;  // the ground here is flat at 0
  scene::WaterCircle circle;
  circle.center = WorldPos{24.0, 1.0, 8.0};
  circle.radius_m = 5.0f;
  lake.shape.circle = circle;
  REQUIRE(ex.build({&hole, 1}, {&lake, 1}, {}));
  const auto out = run.expand(&ex);
  REQUIRE(out.instances.size() < base.instances.size());
  for (const auto& p : out.instances) {
    const f64 hx = p.position.x - 8.0, hz = p.position.z - 8.0;
    const f64 lx = p.position.x - 24.0, lz = p.position.z - 8.0;
    CHECK(hx * hx + hz * hz >= 16.0 - 0.05);
    CHECK(lx * lx + lz * lz >= 25.0 - 0.05);
    const auto* was = find(base, p.id);
    REQUIRE(was != nullptr);
    CHECK(was->position == p.position);
  }

  // An exclusion holding nothing near the layer leaves every litter where it was.
  scene_gen::Exclusion far;
  scene::TerrainHole away;
  away.center = WorldPos{900.0, 0.0, 900.0};
  away.radius_m = 3.0f;
  REQUIRE(far.build({&away, 1}, {}, {}));
  const auto same = run.expand(&far);
  REQUIRE(same.instances.size() == base.instances.size());
  for (u32 i = 0; i < base.instances.size(); ++i) {
    CHECK(same.instances[i].id == base.instances[i].id);
    CHECK(same.instances[i].position == base.instances[i].position);
  }
}
