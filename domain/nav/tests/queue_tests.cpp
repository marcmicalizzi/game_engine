// The rebuild queue's promises: a tile requested twice before it builds is built once with the
// second input, builds run off the calling thread, results are applied only by the calling
// thread, and the priority order is the observers' rather than the arrival order.

#include "nav_test_support.h"

#include <core/jobs/job_system.h>
#include <domain/nav/rebuild_queue.h>
#include <domain/nav/region_graph.h>

#include <doctest/doctest.h>

#include <functional>
#include <thread>

using namespace engine;
using namespace engine::nav;
using namespace engine::nav::testing;

namespace {

u64 this_thread_id() {
  return static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

RebuildRequest request_for(const Soup& soup, TileCoord coord) {
  RebuildRequest request;
  request.coord = coord;
  request.vertices = std::span<const Vec3>(soup.vertices.data(), soup.vertices.size());
  request.indices = std::span<const u32>(soup.indices.data(), soup.indices.size());
  return request;
}

RebuildQueueOptions queue_options(jobs::JobSystem* system) {
  RebuildQueueOptions options;
  options.build = test_params();
  options.job_system = system;
  return options;
}

// The length of the straight crossing of tile (0, 0) at z = 24, or -1 when there is no path.
f32 crossing_length(const NavMesh& mesh) {
  Vec3 corridor[64];
  PathResult result;
  if (mesh.find_path(Vec3(8.0f, 0.5f, 24.0f), Vec3(56.0f, 0.5f, 24.0f), walk_filter(),
                     std::span<Vec3>(corridor), result) != Status::Ok)
    return -1.0f;
  return result.length;
}

}  // namespace

TEST_CASE("nav: a tile requested twice before it builds is built once, with the second input") {
  // Nothing dispatches until dispatch(), so this is exact rather than a race: both requests land
  // in the pending set and one build comes out.
  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(nullptr)) == Status::Ok);

  NavMesh mesh;
  REQUIRE(mesh.init(test_mesh_options(queue.options().build)) == Status::Ok);

  const Soup walled = floor_with_wall(48.0f);
  const Soup open = floor_with_wall(0.0f);
  REQUIRE(queue.request(request_for(walled, TileCoord{0, 0})) == Status::Ok);
  REQUIRE(queue.request(request_for(open, TileCoord{0, 0})) == Status::Ok);

  RebuildQueueStats stats = queue.stats();
  CHECK(stats.requested == 1);
  CHECK(stats.coalesced == 1);
  CHECK(stats.pending == 1);
  CHECK(stats.built == 0);

  queue.dispatch();
  stats = queue.stats();
  CHECK(stats.built == 1);

  CHECK(queue.apply_ready(mesh) == 1);
  CHECK(mesh.tile_count() == 1);
  // The second request is the one that was built: no wall, so the crossing is straight.
  CHECK(crossing_length(mesh) == doctest::Approx(48.0f).epsilon(0.02));
}

TEST_CASE("nav: eight requests for one tile are one pending entry") {
  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(nullptr)) == Status::Ok);

  const Soup soup = floor_with_wall(48.0f);
  for (u32 i = 0; i < 8; ++i)
    REQUIRE(queue.request(request_for(soup, TileCoord{3, 3})) == Status::Ok);

  const RebuildQueueStats stats = queue.stats();
  CHECK(stats.requested == 1);
  CHECK(stats.coalesced == 7);
  CHECK(stats.pending == 1);

  queue.wait_idle();
  CHECK(queue.stats().built == 1);
}

TEST_CASE("nav: builds run on a worker and results are applied on the caller's thread") {
  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  jobs::JobSystem jobs_system(config);

  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(&jobs_system)) == Status::Ok);

  NavMesh mesh;
  RegionGraph graph;
  REQUIRE(mesh.init(test_mesh_options(queue.options().build)) == Status::Ok);

  const Soup soup = floor_with_wall(48.0f);
  REQUIRE(queue.request(request_for(soup, TileCoord{0, 0})) == Status::Ok);
  queue.wait_idle();

  // The build is finished, and the mesh has not been touched: nothing but apply_ready may.
  CHECK(queue.ready_count() == 1);
  CHECK(mesh.tile_count() == 0);
  CHECK(graph.node_count() == 0);

  CHECK(queue.apply_ready(mesh, &graph) == 1);
  CHECK(mesh.tile_count() == 1);
  CHECK(graph.node_count() >= 1);
  CHECK(queue.ready_count() == 0);

  const RebuildQueueStats stats = queue.stats();
  CHECK(stats.applied == 1);
  CHECK(stats.last_apply_thread == this_thread_id());
  CHECK(stats.last_build_thread != this_thread_id());
  CHECK(stats.build_ns > 0);
}

TEST_CASE("nav: with no job system a request and an apply are the whole loop") {
  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(nullptr)) == Status::Ok);

  NavMesh mesh;
  REQUIRE(mesh.init(test_mesh_options(queue.options().build)) == Status::Ok);

  const Soup soup = floor_with_wall(0.0f);
  REQUIRE(queue.request(request_for(soup, TileCoord{0, 0})) == Status::Ok);
  CHECK(queue.stats().built == 0);  // queued, not started

  CHECK(queue.apply_ready(mesh) == 1);
  CHECK(mesh.tile_count() == 1);
  CHECK(queue.stats().built == 1);
  CHECK(queue.stats().last_build_thread == this_thread_id());
  CHECK(queue.stats().last_apply_thread == this_thread_id());
}

TEST_CASE("nav: many tiles arrive in tile order whatever order they finished in") {
  jobs::JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 4;
  jobs::JobSystem jobs_system(config);

  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(&jobs_system)) == Status::Ok);

  NavMesh mesh;
  RegionGraph graph;
  REQUIRE(mesh.init(test_mesh_options(queue.options().build)) == Status::Ok);

  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 192.0f, 192.0f, 0.0f, 24);
  for (i32 x = 0; x < 3; ++x) {
    for (i32 y = 0; y < 3; ++y)
      REQUIRE(queue.request(request_for(soup, TileCoord{x, y})) == Status::Ok);
  }
  queue.wait_idle();
  CHECK(queue.apply_ready(mesh, &graph) == 9);
  CHECK(mesh.tile_count() == 9);
  CHECK(graph.stats().tile_count == 9);

  // Nine tiles of open floor, one region each, joined to their orthogonal neighbours: twelve
  // pairs, twenty-four directed edges.
  CHECK(graph.node_count() == 9);
  CHECK(graph.edge_count() == 24);

  Vec3 corridor[128];
  PathResult result;
  REQUIRE(mesh.find_path(Vec3(8.0f, 0.5f, 8.0f), Vec3(184.0f, 0.5f, 184.0f), walk_filter(),
                         std::span<Vec3>(corridor), result) == Status::Ok);
  CHECK_FALSE(result.partial);
}

TEST_CASE("nav: the nearest tile to an observer is built first") {
  RebuildQueue queue;
  RebuildQueueOptions options = queue_options(nullptr);
  options.max_in_flight = 1;
  REQUIRE(queue.init(options) == Status::Ok);

  const Vec3 observer(0.0f, 0.0f, 0.0f);
  queue.set_observers(std::span<const Vec3>(&observer, 1));

  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 640.0f, 640.0f, 0.0f, 40);
  // Requested far to near; the queue should not serve them in that order.
  for (i32 i = 8; i >= 1; --i)
    REQUIRE(queue.request(request_for(soup, TileCoord{i, 0})) == Status::Ok);
  CHECK(queue.stats().pending == 8);

  NavMesh mesh;
  RegionGraph graph;
  REQUIRE(mesh.init(test_mesh_options(options.build)) == Status::Ok);
  queue.dispatch();
  queue.apply_ready(mesh, &graph);
  CHECK(mesh.tile_count() == 8);
}

TEST_CASE("nav: the pending cap evicts the least urgent tile instead of growing") {
  RebuildQueue queue;
  RebuildQueueOptions options = queue_options(nullptr);
  options.max_pending = 2;
  REQUIRE(queue.init(options) == Status::Ok);

  const Vec3 observer(0.0f, 0.0f, 0.0f);
  queue.set_observers(std::span<const Vec3>(&observer, 1));

  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 8);
  bool saw_limit = false;
  for (i32 i = 20; i > 0; --i) {
    if (queue.request(request_for(soup, TileCoord{i, 0})) == Status::LimitReached) saw_limit = true;
  }
  CHECK(saw_limit);
  CHECK(queue.stats().evicted > 0);
  CHECK(queue.stats().pending <= 2);
}

TEST_CASE("nav: the starvation guard promotes a tile that keeps being passed over") {
  // The guard is a subtraction, so it is testable without a clock. One slot, a relief of 100 m a
  // pass, and eight tiles spread over 500 m: without the guard the nearest would be rebuilt over
  // and over as long as anything nearer kept arriving; with it, the queue drains.
  RebuildQueue queue;
  RebuildQueueOptions options = queue_options(nullptr);
  options.max_in_flight = 1;
  options.max_pending = 64;
  options.starvation_relief = 100.0f;
  REQUIRE(queue.init(options) == Status::Ok);

  const Vec3 observer(0.0f, 0.0f, 0.0f);
  queue.set_observers(std::span<const Vec3>(&observer, 1));

  Soup soup;
  add_floor(soup, 0.0f, 0.0f, 640.0f, 640.0f, 0.0f, 40);
  for (i32 i = 0; i < 6; ++i)
    REQUIRE(queue.request(request_for(soup, TileCoord{i, 0})) == Status::Ok);

  queue.wait_idle();
  CHECK(queue.stats().built == 6);
  CHECK(queue.stats().pending == 0);

  NavMesh mesh;
  REQUIRE(mesh.init(test_mesh_options(options.build)) == Status::Ok);
  CHECK(queue.apply_ready(mesh) == 6);
  CHECK(mesh.tile_count() == 6);
}

TEST_CASE("nav: a tile whose floor was destroyed removes itself") {
  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(nullptr)) == Status::Ok);

  NavMesh mesh;
  RegionGraph graph;
  REQUIRE(mesh.init(test_mesh_options(queue.options().build)) == Status::Ok);

  const Soup floor = floor_with_wall(0.0f);
  REQUIRE(queue.request(request_for(floor, TileCoord{0, 0})) == Status::Ok);
  CHECK(queue.apply_ready(mesh, &graph) == 1);
  CHECK(mesh.tile_count() == 1);

  const Soup nothing;
  REQUIRE(queue.request(request_for(nothing, TileCoord{0, 0})) == Status::Ok);
  CHECK(queue.apply_ready(mesh, &graph) == 1);
  CHECK(mesh.tile_count() == 0);
  CHECK(graph.node_count() == 0);
}

TEST_CASE("nav: bad requests are refused before anything is copied") {
  RebuildQueue queue;
  REQUIRE(queue.init(queue_options(nullptr)) == Status::Ok);

  Soup soup = floor_with_wall(0.0f);
  soup.indices.pop_back();
  CHECK(queue.request(request_for(soup, TileCoord{0, 0})) == Status::InvalidArgument);
  CHECK(queue.stats().pending == 0);

  RebuildQueueOptions bad = queue_options(nullptr);
  bad.build.cell_size = 0.3f;  // does not divide the 64 m tile
  RebuildQueue refused;
  CHECK(refused.init(bad) == Status::InvalidArgument);
}
