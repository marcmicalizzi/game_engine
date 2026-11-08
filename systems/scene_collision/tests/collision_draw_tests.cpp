// The collision debug draw (collision_draw.h; docs/subsystems/scene_collision.md, "Seeing the
// collision"; roadmap P62): the lines are the held tiles' proxy triangles' edges and their ground
// lattices', the budget drops the farthest tile first, and on a device the pass draws lines where
// the proxy is and, removed, leaves the frame the bytes it was.

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/device.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/scene_collision/collision_draw.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

// A flat ground at y = 0, as a provider of the test's own.
void flat_destroy(void*) noexcept {}
f32 flat_height(const void*, f32, f32) noexcept { return 0.0f; }
constexpr scene_gen::GroundOps k_flat_ops{.destroy = &flat_destroy, .height = &flat_height};

// Boxes standing on y = 0, one mesh, one instance a box.
void box_scene(std::span<const Vec3> centres, Vec3 half, renderer::SceneData& scene) {
  Vector<Vec3> positions;
  Vector<u32> indices;
  for (u32 k = 0; k < 8; ++k) {
    positions.push_back(Vec3{(k & 1) ? half.x : -half.x, (k & 2) ? 2.0f * half.y : 0.0f,
                             (k & 4) ? half.z : -half.z});
  }
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      indices.push_back(v);
  }
  geometry::ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                  std::span<const u32>(indices.data(), indices.size()),
                                  geometry::ClusterLodOptions{}, lod, &error),
      error);
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(&lod, 1),
                                                 scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (const Vec3& c : centres) {
    renderer::SceneInstance placed;
    placed.mesh = 0;
    placed.transform.position = c;
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, placed, first, desc, &error));
    first += scene.parts[0].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
}

WorldPos wp(f64 x, f64 z) { return WorldPos{x, 0.0, z}; }

// Whether a point is in one of the four corner tiles round tile (0, 0) of a 32 m grid, past the
// metre and a half a tile's lattice reaches over its edges (its window runs from the lattice point
// at or west of the edge to the one at or east of the other).
bool in_corner_tile(Vec3 p, WorldPos eye) {
  const f64 x = eye.x + static_cast<f64>(p.x);
  const f64 z = eye.z + static_cast<f64>(p.z);
  const bool west = x < -1.5, east = x > 33.5, south = z < -1.5, north = z > 33.5;
  return (west || east) && (south || north);
}

}  // namespace

TEST_CASE("scene_collision: the draw's lines are the proxies' edges and the lattices'") {
  const Vec3 centres[] = {{10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 20.0f}, {40.0f, 0.0f, 5.0f}};
  renderer::SceneData scene;
  box_scene(centres, Vec3{0.5f, 1.0f, 0.5f}, scene);
  scene_gen::GroundProvider ground(&k_flat_ops, nullptr);
  CollisionView view;
  Config config;
  std::string error;
  REQUIRE_MESSAGE(view.create(scene, &ground, config, 1.5f, &error), error);
  view.update(wp(16.0, 16.0));
  const SceneCollision& collision = view.collision();
  REQUIRE(collision.held_tiles() == 9);

  // The held pieces' triangles and the lattices' segments, counted from what is held.
  u64 triangles = 0;
  u64 lattice = 0;
  for (u32 t = 0; t < collision.held_tiles(); ++t) {
    HeldTile tile;
    REQUIRE(collision.held_tile(t, tile));
    CHECK(tile.n == 34);
    lattice += 2ull * tile.n * (tile.n - 1);
    for (u32 p = 0; p < tile.pieces; ++p) {
      HeldPiece piece;
      REQUIRE(collision.held_piece(t, p, piece));
      CHECK(piece.instance < 3);
      triangles += piece.indices.size() / 3;
    }
  }
  CHECK(triangles == 3 * 12);  // three boxes of twelve, each within one tile
  CHECK(collision.stats().proxy_triangles == 12);

  Vector<DrawLine> lines;
  DrawStats stats;
  DrawCapsule capsule;
  capsule.feet = wp(16.0, 16.0);
  const WorldPos eye{16.0, 1.6, 10.0};
  collision_lines(collision, eye, capsule.feet, &capsule, DrawOptions{}, lines, stats);
  MESSAGE("lines: " << stats.lines << " (" << stats.proxy_lines << " of proxies, "
                    << stats.ground_lines << " of the lattice, " << stats.capsule_lines
                    << " of the capsule)");
  CHECK(stats.tiles_drawn == 9);
  CHECK(stats.tiles_dropped == 0);
  CHECK(stats.proxy_lines == 3 * triangles);
  CHECK(stats.ground_lines == lattice);
  CHECK(stats.capsule_lines == k_capsule_lines);
  CHECK(lines.size() == 3 * triangles + lattice + k_capsule_lines);
  // A box's top edge is 2 m up, in the eye's frame.
  f32 highest = -1e9f;
  for (const DrawLine& l : lines) {
    if (l.color == k_capsule_color) continue;
    highest = std::max(highest, std::max(l.a.y, l.b.y));
  }
  CHECK(highest == doctest::Approx(2.0f - 1.6f).epsilon(1e-4));

  // A consumer that keeps no shapes holds the same bodies, and draws its ground alone.
  CHECK(collision.config().keep_shapes);
}

TEST_CASE("scene_collision: the draw's budget drops the farthest tile first") {
  renderer::SceneData scene;
  box_scene(std::span<const Vec3>(), Vec3{0.5f, 1.0f, 0.5f}, scene);
  scene.instances.clear();
  scene_gen::GroundProvider ground(&k_flat_ops, nullptr);
  CollisionView view;
  std::string error;
  REQUIRE_MESSAGE(view.create(scene, &ground, Config{}, 1.5f, &error), error);
  view.update(wp(16.0, 16.0));
  const SceneCollision& collision = view.collision();
  REQUIRE(collision.held_tiles() == 9);
  const u32 per_tile = 2 * 34 * 33;
  // Room for five tiles: the walker's and its four edge neighbours, 32 m off; the four corners,
  // 45 m off, are dropped.
  DrawOptions options;
  options.max_lines = 5 * per_tile + per_tile / 2;
  Vector<DrawLine> lines;
  DrawStats stats;
  const WorldPos eye{16.0, 1.6, 16.0};
  collision_lines(collision, eye, wp(16.0, 16.0), nullptr, options, lines, stats);
  CHECK(stats.tiles_drawn == 5);
  CHECK(stats.tiles_dropped == 4);
  CHECK(stats.dropped_lines == 4ull * per_tile);
  CHECK(stats.lines == 5 * per_tile);
  u32 in_corners = 0;
  for (const DrawLine& l : lines) {
    const Vec3 mid = (l.a + l.b) * 0.5f;
    if (in_corner_tile(mid, eye)) ++in_corners;
  }
  CHECK(in_corners == 0);
  // And a radius: only the tiles within 10 m of the centre's tile, which is its own.
  options = DrawOptions{};
  options.radius_m = 10.0f;
  collision_lines(collision, eye, wp(16.0, 16.0), nullptr, options, lines, stats);
  CHECK(stats.tiles_drawn == 1);
  CHECK(stats.lines == per_tile);
}

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;

}  // namespace

TEST_CASE("scene_collision: the draw's pass draws lines where the proxy is, and off is no pass") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // The picture: the procedural hills, as the renderer's frame-pass test draws them.
  renderer::SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  renderer::SceneData data;
  std::string error;
  REQUIRE_MESSAGE(renderer::load_scene(desc, data, error), error);
  renderer::RenderSettings settings;
  settings.shadows = renderer::ShadowMode::Off;
  renderer::ResolvedSettings resolved;
  renderer::resolve_settings(settings, gpu.device.features(), &data, resolved);
  if (renderer::check_availability(resolved, gpu.device.features()) !=
      renderer::RenderAvailability::Ok) {
    MESSAGE("skipped: the renderer is unavailable on this device");
    return;
  }
  renderer::GpuScene gpu_scene;
  REQUIRE_MESSAGE(gpu_scene.create(gpu.device, data, resolved, &error), error);
  renderer::SceneRenderer view;
  renderer::SceneRenderer::Desc rd;
  rd.width = k_width;
  rd.height = k_height;
  REQUIRE_MESSAGE(view.create(gpu.device, gpu_scene, resolved, rd, &error), error);

  // The collision: one box 4 m tall standing 12 m in front of the eye and 10 m to its right, high
  // over the hills, no ground lattice (no provider), so every line drawn is the box's.
  renderer::SceneData boxes;
  const Vec3 centre{10.0f, 4.0f, -12.0f};  // within tile (0, -1), so held once
  box_scene(std::span<const Vec3>(&centre, 1), Vec3{1.0f, 2.0f, 1.0f}, boxes);
  CollisionView collision;
  REQUIRE_MESSAGE(collision.create(boxes, nullptr, Config{}, 1.5f, &error), error);
  collision.update(wp(10.0, -12.0));

  renderer::FrameDesc frame;
  const Vec3 eye{0.0f, 6.0f, 0.0f};
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 6.0f, -100.0f});
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  renderer::CaptureChannels channels;
  channels.color = true;
  renderer::CapturedFrame before;
  REQUIRE_MESSAGE(view.capture(frame, channels, before, &error), error);

  CollisionDraw draw;
  REQUIRE_MESSAGE(draw.create(gpu.device, view, DrawOptions{}, &error), error);
  draw.set(&collision.collision(), wp(10.0, -12.0), nullptr);
  renderer::CapturedFrame with;
  REQUIRE_MESSAGE(view.capture(frame, channels, with, &error), error);
  CHECK(draw.stats().lines == 3 * 12);
  CHECK(draw.stats().ground_lines == 0);

  // The box's outline on the screen: its corners projected through the camera's own frame.
  const f32 t = std::tan(0.5f * frame.camera.fov_y);
  const f32 aspect = static_cast<f32>(k_width) / static_cast<f32>(k_height);
  f32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (u32 k = 0; k < 8; ++k) {
    const Vec3 p{centre.x + ((k & 1) ? 1.0f : -1.0f), centre.y + ((k & 2) ? 4.0f : 0.0f),
                 centre.z + ((k & 4) ? 1.0f : -1.0f)};
    const Vec3 r = p - eye;
    const f32 sx = (r.x / (-r.z * t * aspect) * 0.5f + 0.5f) * static_cast<f32>(k_width);
    const f32 sy = (0.5f - r.y / (-r.z * t) * 0.5f) * static_cast<f32>(k_height);
    x0 = std::min(x0, sx);
    x1 = std::max(x1, sx);
    y0 = std::min(y0, sy);
    y1 = std::max(y1, sy);
  }
  REQUIRE(with.color.size() == before.color.size());
  const u32 channels_per_pixel = static_cast<u32>(before.color.size() / (k_width * k_height));
  u32 inside = 0;
  u32 outside = 0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 at = (y * k_width + x) * channels_per_pixel;
      bool changed = false;
      for (u32 c = 0; c < 3; ++c) {
        const i32 d =
            std::abs(static_cast<i32>(with.color[at + c]) - static_cast<i32>(before.color[at + c]));
        changed = changed || d > 1;  // past the linear image's rounding (frame_pass_tests.cpp)
      }
      if (!changed) continue;
      const f32 px = static_cast<f32>(x) + 0.5f;
      const f32 py = static_cast<f32>(y) + 0.5f;
      if (px >= x0 - 1.5f && px <= x1 + 1.5f && py >= y0 - 1.5f && py <= y1 + 1.5f) {
        ++inside;
      } else {
        ++outside;
      }
    }
  }
  MESSAGE("the box's outline (" << x0 << ", " << y0 << ")-(" << x1 << ", " << y1 << "): " << inside
                                << " pixels coloured inside it, " << outside << " outside");
  // The box's twelve edges and its faces' diagonals, a few pixels apart: well over a hundred.
  CHECK(inside > 100);
  CHECK(outside == 0);

  // Off — the pass removed — the frame is the bytes it was.
  draw.destroy();
  CHECK(view.frame_pass_count() == 0);
  renderer::CapturedFrame after;
  REQUIRE_MESSAGE(view.capture(frame, channels, after, &error), error);
  CHECK(after.color == before.color);
}
