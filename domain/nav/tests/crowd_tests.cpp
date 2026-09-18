// Agents: that they exist, that they walk, and that they arrive.

#include "nav_test_support.h"

#include <core/containers/vector.h>
#include <domain/nav/nav_mesh.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::nav;
using namespace engine::nav::testing;

namespace {

struct Fixture {
  NavBuildParams params = test_params();
  NavMesh mesh;
  Crowd crowd;

  Status init(f32 wall_length) {
    Status status = mesh.init(test_mesh_options(params));
    if (status != Status::Ok) return status;
    const Soup soup = floor_with_wall(wall_length);
    NavTileData tile;
    status = build_tile(params, TileCoord{0, 0}, soup.view(), tile, nullptr);
    if (status != Status::Ok) return status;
    status = mesh.add_tile(tile);
    if (status != Status::Ok) return status;
    CrowdOptions options;
    options.max_agents = 8;
    options.max_agent_radius = 0.6f;
    return crowd.init(mesh, options);
  }

  // Runs the crowd at 60 Hz for at most `seconds` and stops when every agent is within
  // `tolerance` of its target.
  bool run_until_arrived(std::span<const AgentId> ids, f32 tolerance, f32 seconds) {
    const f32 dt = 1.0f / 60.0f;
    const u32 steps = static_cast<u32>(seconds / dt);
    for (u32 i = 0; i < steps; ++i) {
      crowd.update(dt);
      bool all_there = true;
      for (const AgentId id : ids) {
        AgentState state;
        if (!crowd.agent_state(id, state)) return false;
        const Vec3 flat_position(state.position.x, 0.0f, state.position.z);
        const Vec3 flat_target(state.target.x, 0.0f, state.target.z);
        if (distance(flat_position, flat_target) > tolerance) all_there = false;
      }
      if (all_there) return true;
    }
    return false;
  }
};

}  // namespace

TEST_CASE("nav: a crowd agent walks to its target") {
  Fixture fixture;
  REQUIRE(fixture.init(0.0f) == Status::Ok);

  AgentDesc desc;
  desc.position = Vec3(8.0f, 0.0f, 24.0f);
  desc.radius = 0.5f;
  desc.max_speed = 6.0f;
  desc.user_data = 42;
  AgentId agent;
  REQUIRE(fixture.crowd.add_agent(desc, agent) == Status::Ok);
  CHECK(fixture.crowd.agent_count() == 1);
  CHECK(fixture.crowd.contains(agent));

  AgentState state;
  REQUIRE(fixture.crowd.agent_state(agent, state));
  CHECK(state.user_data == 42);
  CHECK_FALSE(state.has_target);
  CHECK(state.position.x == doctest::Approx(8.0f).epsilon(0.05));

  REQUIRE(fixture.crowd.request_move(agent, Vec3(40.0f, 0.0f, 24.0f)));
  REQUIRE(fixture.crowd.agent_state(agent, state));
  CHECK(state.has_target);

  const AgentId ids[1] = {agent};
  CHECK(fixture.run_until_arrived(std::span<const AgentId>(ids), 1.0f, 30.0f));

  Vec3 positions[1] = {Vec3(0.0f, 0.0f, 0.0f)};
  fixture.crowd.read_positions(std::span<const AgentId>(ids), std::span<Vec3>(positions));
  CHECK(positions[0].x == doctest::Approx(40.0f).epsilon(0.1));
}

TEST_CASE("nav: agents walk around a wall and still arrive") {
  Fixture fixture;
  REQUIRE(fixture.init(48.0f) == Status::Ok);

  AgentId ids[4];
  for (u32 i = 0; i < 4; ++i) {
    AgentDesc desc;
    desc.position = Vec3(8.0f, 0.0f, 8.0f + static_cast<f32>(i) * 2.0f);
    desc.radius = 0.5f;
    desc.max_speed = 8.0f;
    desc.separation_weight = 2.0f;
    desc.user_data = i;
    REQUIRE(fixture.crowd.add_agent(desc, ids[i]) == Status::Ok);
    REQUIRE(fixture.crowd.request_move(ids[i], Vec3(56.0f, 0.0f, 8.0f + static_cast<f32>(i))));
  }
  CHECK(fixture.crowd.agent_count() == 4);

  // The only way across is round the far end of the wall at z = 48, so this is a real path, not
  // a straight line: about 110 m at 8 m/s plus avoidance.
  CHECK(fixture.run_until_arrived(std::span<const AgentId>(ids), 2.0f, 60.0f));
}

TEST_CASE("nav: the agent cap is a cap and stale handles fail every call") {
  Fixture fixture;
  REQUIRE(fixture.init(0.0f) == Status::Ok);

  AgentDesc desc;
  desc.radius = 0.5f;
  Vector<AgentId> agents;
  for (u32 i = 0; i < 8; ++i) {
    desc.position = Vec3(4.0f + static_cast<f32>(i) * 3.0f, 0.0f, 4.0f);
    AgentId id;
    REQUIRE(fixture.crowd.add_agent(desc, id) == Status::Ok);
    agents.push_back(id);
  }
  CHECK(fixture.crowd.agent_count() == fixture.crowd.max_agents());

  AgentId overflow;
  desc.position = Vec3(40.0f, 0.0f, 40.0f);
  CHECK(fixture.crowd.add_agent(desc, overflow) == Status::LimitReached);
  CHECK(overflow.is_null());

  // An agent larger than the crowd was sized for is refused rather than silently clamped.
  AgentDesc oversized = desc;
  oversized.radius = 4.0f;
  CHECK(fixture.crowd.add_agent(oversized, overflow) == Status::InvalidArgument);

  const AgentId removed = agents[3];
  CHECK(fixture.crowd.remove_agent(removed));
  CHECK(fixture.crowd.agent_count() == 7);
  CHECK_FALSE(fixture.crowd.contains(removed));
  CHECK_FALSE(fixture.crowd.remove_agent(removed));
  AgentState state;
  CHECK_FALSE(fixture.crowd.agent_state(removed, state));
  CHECK_FALSE(fixture.crowd.request_move(removed, Vec3(8.0f, 0.0f, 8.0f)));

  // The slot comes back.
  AgentId replacement;
  CHECK(fixture.crowd.add_agent(desc, replacement) == Status::Ok);
  CHECK(fixture.crowd.agent_count() == 8);
}

TEST_CASE("nav: a target with no mesh near it is refused") {
  Fixture fixture;
  REQUIRE(fixture.init(0.0f) == Status::Ok);

  AgentDesc desc;
  desc.position = Vec3(8.0f, 0.0f, 8.0f);
  desc.radius = 0.5f;
  AgentId agent;
  REQUIRE(fixture.crowd.add_agent(desc, agent) == Status::Ok);
  CHECK_FALSE(fixture.crowd.request_move(agent, Vec3(1000.0f, 0.0f, 1000.0f)));

  REQUIRE(fixture.crowd.request_move(agent, Vec3(32.0f, 0.0f, 32.0f)));
  CHECK(fixture.crowd.stop(agent));
  AgentState state;
  REQUIRE(fixture.crowd.agent_state(agent, state));
  CHECK_FALSE(state.has_target);
}

TEST_CASE("nav: a crowd without a mesh refuses to start") {
  NavMesh empty;
  Crowd crowd;
  CrowdOptions options;
  CHECK(crowd.init(empty, options) == Status::NotFound);
  CHECK_FALSE(crowd.initialized());

  NavMesh mesh;
  NavBuildParams params = test_params();
  REQUIRE(mesh.init(test_mesh_options(params)) == Status::Ok);
  options.max_agents = 0;
  CHECK(crowd.init(mesh, options) == Status::InvalidArgument);
  options.max_agents = 4;
  CHECK(crowd.init(mesh, options) == Status::Ok);
  CHECK(crowd.initialized());
  crowd.shutdown();
  CHECK_FALSE(crowd.initialized());
}
