// kinematics capability (docs/subsystems/kinematics.md): the integration, the system's
// registration, the same bytes under either executor, and `Mover` records through materialization.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/materialize.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/materialize.h>
#include <systems/kinematics/kinematics.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::kinematics;

namespace {

constexpr f32 k_step = 1.0f / 60.0f;

flecs::entity spawn(ecs::SimWorld& sim, u64 n, WorldPos position, Vec3 linear, Vec3 angular) {
  const flecs::entity e = ecs::create_entity(sim.world(), Id128::from_parts(0x6B, n));
  world::Transform transform;
  transform.position = position;
  e.set<world::Transform>(transform);
  Velocity velocity;
  velocity.linear = linear;
  velocity.angular = angular;
  e.set<Velocity>(velocity);
  return e;
}

}  // namespace

TEST_CASE("kinematics: integrate moves by the velocity and turns about the entity's origin") {
  world::Transform t;
  Velocity v;
  v.linear = Vec3{2.0f, 0.0f, -1.0f};
  integrate(t, v, 0.5f);
  CHECK(t.position == WorldPos{1.0, 0.0, -0.5});
  // Not turning: the orientation is left alone to the bit, so it is never written back.
  CHECK(t.orientation == Quat::identity());

  // A quarter turn a second about y, in 60 steps of a sixtieth: about a quarter turn.
  v.linear = Vec3{};
  v.angular = Vec3{0.0f, k_pi * 0.5f, 0.0f};
  world::Transform spun;
  for (int i = 0; i < 60; ++i)
    integrate(spun, v, k_step);
  const Quat expected = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, k_pi * 0.5f);
  CHECK(std::fabs(dot(spun.orientation, expected)) == doctest::Approx(1.0f).epsilon(1e-4));
  CHECK(length(spun.orientation) == doctest::Approx(1.0f));
}

// ADR-0053: a cart walking pace along x and half that along z, stepped one second at 60 Hz, moves
// as far 420 km, 10,000 km and 1e8 m out as it does by the origin. Each of the 60 additions rounds
// at the position's own f64 step, so the far displacement is within 60 half-steps of the origin's,
// bounded here by 60 × site × 2^-53: 6.7e-8 m at 1e7 m, 6.7e-7 m at 1e8 m.
TEST_CASE("kinematics far: a second's motion is the same distance far from the origin") {
  Velocity v;
  v.linear = Vec3{1.5f, 0.0f, 0.75f};
  const auto walk = [&](WorldPos start) {
    world::Transform t;
    t.position = start;
    for (int i = 0; i < 60; ++i)
      integrate(t, v, k_step);
    return t.position - start;
  };
  const DVec3 by_origin = walk(WorldPos::origin());
  CHECK(by_origin.x == doctest::Approx(1.5).epsilon(1e-6));
  CHECK(by_origin.z == doctest::Approx(0.75).epsilon(1e-6));
  for (const f64 site : {419072.0, 10000000.0, 100000000.0}) {
    CAPTURE(site);
    const DVec3 far = walk(WorldPos{site, 12.0, -site});
    const f64 bound = 60.0 * site * 0x1p-53;  // 60 roundings of half the step at `site`
    CHECK(std::fabs(far.x - by_origin.x) <= bound);
    CHECK(std::fabs(far.z - by_origin.z) <= bound);
    CHECK(far.y == 0.0);
  }
  // The float32 arithmetic this replaced, for the record: 420 km out each 2.5 cm step rounds up to
  // the float's 3.125 cm, and the cart goes 1.875 m in the second it should go 1.5.
  f32 x = 419072.0f;
  for (int i = 0; i < 60; ++i)
    x += v.linear.x * k_step;
  CHECK(x == 419072.0f + 1.875f);
}

TEST_CASE("kinematics: the system registers through the engine's descriptor and moves entities") {
  ecs::SimWorld sim;
  KinematicsSystem system;
  system.install(sim);
  const sim::SystemDesc& desc = system.descriptor();
  CHECK(std::string(desc.name) == "kinematics.integrate");
  CHECK(desc.phase == sim::TickPhase::Systems);
  CHECK(desc.reads.test(ecs::component_index<Velocity>(sim.world())));
  CHECK(desc.writes.test(ecs::component_index<world::Transform>(sim.world())));
  CHECK(ecs::systems(sim.world()).count_in(sim::TickPhase::Systems) == 1);

  const flecs::entity moving = spawn(sim, 1, WorldPos{}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{});
  const flecs::entity still = spawn(sim, 2, WorldPos{5.0, 0.0, 0.0}, Vec3{}, Vec3{});
  for (int i = 0; i < 30; ++i)
    sim.step();
  CHECK(moving.get<world::Transform>().position.x == doctest::Approx(0.5));
  CHECK(still.get<world::Transform>().position.x == 5.0);
  CHECK(system.stats().moved == 2);
}

TEST_CASE("kinematics: the scheduler as executor ticks it with the same step and the same bytes") {
  const auto run = [](bool scheduled) {
    ecs::SimWorld sim;
    KinematicsSystem system;
    system.install(sim);
    for (u64 i = 0; i < 64; ++i) {
      spawn(sim, i + 1, WorldPos{static_cast<f64>(i), 0.0, 0.0},
            Vec3{0.25f * static_cast<f32>(i), 1.0f, -0.5f},
            Vec3{0.0f, 0.1f * static_cast<f32>(i % 7), 0.0f});
    }
    sim::SimScheduler scheduler;
    if (scheduled) {
      ecs::ScheduledTick tick(sim, scheduler);
      for (int t = 0; t < 45; ++t)
        tick.step();
      CHECK(sim.tick().value == 45);
    } else {
      for (int t = 0; t < 45; ++t)
        sim.step();
    }
    Vector<world::Transform> out;
    sim.world().query_builder<const world::Transform>().build().each(
        [&out](const world::Transform& t) { out.push_back(t); });
    return out;
  };
  const Vector<world::Transform> pipeline = run(false);
  const Vector<world::Transform> scheduled = run(true);
  REQUIRE(pipeline.size() == 64);
  REQUIRE(scheduled.size() == pipeline.size());
  CHECK(std::memcmp(pipeline.data(), scheduled.data(),
                    pipeline.size() * sizeof(world::Transform)) == 0);
}

TEST_CASE("kinematics: a Mover record materializes with its spin converted to radians") {
  const schema::MaterializeInfo* mover =
      schema::MaterializeRegistry::global().find("engine.kinematics.Mover");
  REQUIRE(mover != nullptr);
  CHECK(mover->parent == schema::MaterializeParent::ChildOf);

  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  JsonValue props = JsonValue::object();
  JsonValue velocity = JsonValue::array();
  velocity.push_back(JsonValue(3.0));
  velocity.push_back(JsonValue(0.0));
  velocity.push_back(JsonValue(0.0));
  props.set("velocity", std::move(velocity));
  JsonValue spin = JsonValue::array();
  spin.push_back(JsonValue(0.0));
  spin.push_back(JsonValue(90.0));
  spin.push_back(JsonValue(0.0));
  props.set("spin", std::move(spin));
  const Id128 id = Id128::from_parts(0x6B, 99);
  REQUIRE(d.apply(doc::cmd_create(id, "engine.kinematics.Mover", Id128{}, std::move(props)),
                  nullptr, nullptr));

  ecs::SimWorld sim;
  KinematicsSystem system;
  system.install(sim);
  sim::SimScheduler scheduler;
  ecs::RecordMaterializer records(sim.world());
  scheduler.add_hooks(records.hooks());
  sim::MaterializeConfig config;
  config.tier = 0;
  sim::Materializer driver(scheduler, config);
  driver.set_target(records.target());
  const sim::MaterializeReport report = driver.materialize(d);
  CHECK(report.created == 1);

  const flecs::entity e = ecs::entity_for(sim.world(), id);
  REQUIRE(e.is_valid());
  const Velocity& v = e.get<Velocity>();
  CHECK(v.linear == Vec3{3.0f, 0.0f, 0.0f});
  CHECK(v.angular.y == doctest::Approx(k_pi * 0.5f));  // 90 deg/s is a quarter turn a second
  CHECK(e.get<world::Transform>().orientation == Quat::identity());

  ecs::ScheduledTick tick(sim, scheduler);
  for (int i = 0; i < 60; ++i)
    tick.step();
  CHECK(e.get<world::Transform>().position.x == doctest::Approx(3.0));
}
