// Walking on structures (docs/subsystems/scene_collision.md, "Structures"; roadmap S89; plan 14
// §14.4): a village's house is a placement whose mesh its tile hands as a recipe, so its floors,
// walls, stair and doorways reach the walker's collision as that mesh's proxy, with no code here
// that names a building. These cases hold what that gives a walker: in from the lane through the
// doorway, across the rooms, up the stair and to a window — and stopped where a wall is in the way.

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/village/flight.h>
#include <domain/village/village.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }

world::RingParams ring() {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  return p;
}

sim::ObserverSet at(f64 x, f64 z) {
  sim::ObserverSet set;
  set.add(WorldPos{x, 0.0, z}, 1.0f);
  return set;
}

physics::WorldOptions physics_options() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

// A flat floor at y = 0: the scene has no ground provider, so the village's pads and its lane are
// at zero and this is them.
void add_floor(physics::World& physics) {
  physics::ShapeId shape;
  REQUIRE(physics.create_box(Vec3{400.0f, 0.5f, 400.0f}, shape) == physics::Status::Ok);
  physics::BodyDesc slab;
  slab.shape = shape;
  slab.transform.position = place(Vec3{0.0f, -0.5f, 0.0f});
  slab.motion = physics::MotionType::Static;
  slab.layer = physics::Layer::Static;
  physics::BodyId body;
  REQUIRE(physics.create_body(slab, body) == physics::Status::Ok);
}

// A streamed scene of one village entry, two houses along the left of a street, and the plan the
// generator makes of it (the same function on the same flat ground).
struct Village {
  renderer::SceneData scene;
  village::VillageLayer layer;
  village::VillagePlan plan;
};
void village_scene(Village& v) {
  renderer::StreamedPlacements entry;
  entry.generator = "village";
  const std::string params =
      "{\"name\":\"walk\",\"seed\":7,\"houses\":2,\"storeys\":2,\"lanes\":[{\"name\":\"street\","
      "\"waypoints\":[[8,0,40],[88,0,40]],\"rows\":\"Left\"}]}";
  REQUIRE(parse_json(params, entry.params).ok);
  entry.where = "test.json: placements 0";
  entry.tile_meshes = true;
  schema::ReadContext reader;
  REQUIRE(schema::from_json(v.layer, entry.params, reader));
  REQUIRE(reader.ok());
  v.scene.streamed.push_back(entry);
  v.scene.tile_meshes = true;
  v.scene.mesh_options.cache = false;
  v.scene.terrain_mesh = ~0u;
  v.scene.world.enabled = true;
  v.scene.dynamic = true;
  scene_gen::Context context;
  std::string error;
  REQUIRE_MESSAGE(village::plan_scene_village(v.layer, context, v.plan, error), error);
  REQUIRE_MESSAGE(v.plan.built == 2, "refused " << v.plan.refused << ", no fit " << v.plan.no_fit
                                                << ": " << v.plan.first_refusal);
}

// Steers a walker at each point in turn. Returns how many it reached; `stuck` is where it stood
// when it gave up on the next.
u32 walk_along(physics::CharacterBody& walker, const Vector<village::WalkPoint>& points,
               WorldPos& stuck, f32& highest) {
  const f64 speed = static_cast<f64>(walker.config().walk_speed);
  const f64 hz = static_cast<f64>(walker.config().step_hz);
  u32 reached = 1;  // it starts on the first
  for (u32 k = 1; k < points.size(); ++k) {
    const WorldPos target = points[k].feet;
    const WorldPos from = walker.feet();
    const f64 apart = std::sqrt((target.x - from.x) * (target.x - from.x) +
                                (target.z - from.z) * (target.z - from.z));
    // Three times the walk at its own speed, and three seconds for the steps of a stair.
    const u32 budget = static_cast<u32>((3.0 * apart / speed + 3.0) * hz);
    bool there = false;
    for (u32 tick = 0; tick < budget && !there; ++tick) {
      const WorldPos feet = walker.feet();
      const f64 dx = target.x - feet.x, dz = target.z - feet.z;
      const f64 d = std::sqrt(dx * dx + dz * dz);
      if (d < 0.12 && std::fabs(target.y - feet.y) < 0.45) {
        there = true;
        break;
      }
      physics::CharacterInput input;
      if (d > 1e-6) input.move = Vec3{static_cast<f32>(dx / d), 0.0f, static_cast<f32>(dz / d)};
      // Slower over the last stretch, so a step does not carry it past the point.
      input.speed_scale = d < 0.3 ? 0.4f : 1.0f;
      REQUIRE(walker.step(input) == physics::Status::Ok);
      highest = std::max(highest, static_cast<f32>(walker.feet().y));
    }
    if (!there) {
      stuck = walker.feet();
      return reached;
    }
    ++reached;
  }
  stuck = walker.feet();
  return reached;
}

}  // namespace

TEST_CASE("scene_collision: a walker enters a house by its door, crosses it and climbs its stair") {
  Village v;
  village_scene(v);
  Vector<village::WalkPoint> points;
  WorldPos look;
  std::string error;
  REQUIRE_MESSAGE(village::walk_points(v.layer, v.plan, 0, points, look, error), error);
  REQUIRE(points.size() >= 7);
  const village::Plot* plot = nullptr;
  for (const village::Plot& p : v.plan.plots)
    if (p.state == village::PlotState::built && p.house == 0) plot = &p;
  REQUIRE(plot != nullptr);
  const village::House& house = v.plan.houses[0];

  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  {
    SceneCollision collision;
    REQUIRE_MESSAGE(collision.create(physics, v.scene, nullptr, Config{}, &error), error);
    CHECK(collision.streamed());
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(static_cast<f64>(plot->house_x_mm) / 1000.0,
                    static_cast<f64>(plot->house_z_mm) / 1000.0),
                 0, true);
    const Stats& s = collision.stats();
    CHECK(s.placements == 2);  // both houses stand within the ring
    CHECK(s.tile_proxies >= 1);
    MESSAGE("two houses: " << s.tile_proxies << " proxies of " << s.proxy_triangles
                           << " triangles, " << s.proxy_bytes << " bytes; compounds "
                           << s.compound_bytes << " bytes");

    physics::CharacterConfig c;
    c.step_hz = 120;
    physics::CharacterBody walker;
    const WorldPos start{points[0].feet.x, points[0].feet.y + 0.05, points[0].feet.z};
    REQUIRE(walker.create(physics, c, start) == physics::Status::Ok);
    WorldPos stuck;
    f32 highest = 0.0f;
    const u32 reached = walk_along(walker, points, stuck, highest);
    INFO("stopped at (" << stuck.x << ", " << stuck.y << ", " << stuck.z << ") on the way to '"
                        << std::string(points[std::min(reached, points.size() - 1)].what) << "' at ("
                        << points[std::min(reached, points.size() - 1)].feet.x << ", "
                        << points[std::min(reached, points.size() - 1)].feet.y << ", "
                        << points[std::min(reached, points.size() - 1)].feet.z << ")");
    CHECK(reached == points.size());
    // At the window: upstairs (a storey over the pad), standing, inside the footprint's box.
    const WorldPos end = walker.feet();
    const f64 pad = static_cast<f64>(plot->pad_mm) / 1000.0;
    CHECK(std::fabs(end.y - points[points.size() - 1].feet.y) < 0.1);
    CHECK(house.route.size() >= 2);
    CHECK(end.y > pad + 2.0);  // the entry asks two storeys: the window is an upper one
    CHECK(walker.state().ground == physics::Ground::OnGround);
    const f64 rx = end.x - static_cast<f64>(plot->front_x_mm) / 1000.0;
    const f64 rz = end.z - static_cast<f64>(plot->front_z_mm) / 1000.0;
    const f64 behind_front = rx * plot->inward_x + rz * plot->inward_z;
    CHECK(behind_front > static_cast<f64>(v.layer.setback_m));
    MESSAGE("walked " << points.size() << " points to the window at (" << end.x << ", " << end.y
                      << ", " << end.z << "), " << end.y - pad << " m over the pad");

    // **And a wall is a wall.** A second walker starts on the pad a step from the house's front,
    // beside the doorway and not in it, and walks at the house for five seconds: it stops at the
    // wall's face, a capsule's radius short of it.
    const f64 door_u = (static_cast<f64>(plot->door_x_mm - plot->front_x_mm) * plot->along_x +
                        static_cast<f64>(plot->door_z_mm - plot->front_z_mm) * plot->along_z) /
                       1000.0;
    // Toward the middle of the front, so the walker faces wall and not the house's corner.
    const f64 aside = (door_u > 0.0 ? -1.0 : 1.0) *
                      (static_cast<f64>(house.door_width_mm) / 2000.0 + 0.9);
    const f64 setback = static_cast<f64>(v.layer.setback_m);
    const auto front_point = [&](f64 u, f64 back) {
      return WorldPos{static_cast<f64>(plot->front_x_mm) / 1000.0 + plot->along_x * u +
                          plot->inward_x * back,
                      pad + 0.05,
                      static_cast<f64>(plot->front_z_mm) / 1000.0 + plot->along_z * u +
                          plot->inward_z * back};
    };
    physics::CharacterBody blocked;
    REQUIRE(blocked.create(physics, c, front_point(door_u + aside, setback - 1.2)) ==
            physics::Status::Ok);
    physics::CharacterInput in;
    in.move = Vec3{static_cast<f32>(plot->inward_x), 0.0f, static_cast<f32>(plot->inward_z)};
    for (u32 k = 0; k < 5 * 120; ++k)
      REQUIRE(blocked.step(in) == physics::Status::Ok);
    const WorldPos held = blocked.feet();
    const f64 held_back = (held.x - static_cast<f64>(plot->front_x_mm) / 1000.0) * plot->inward_x +
                          (held.z - static_cast<f64>(plot->front_z_mm) / 1000.0) * plot->inward_z;
    MESSAGE("walked at the wall beside the door: stopped " << setback - held_back
                                                           << " m short of its face");
    CHECK(held_back < setback - 0.25);  // five seconds would have carried it 7.5 m, through it
    CHECK(held_back > setback - 0.45);  // and it did reach the wall
  }
  CHECK(physics.built_shape_count() == 0);
}
