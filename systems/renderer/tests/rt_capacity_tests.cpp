// The ray tracing chain sized by the frame (docs/subsystems/renderer.md, "The ray tracing chain's
// memory"). Three things hold it:
//
// - the capacity policy's arithmetic, on the CPU: it grows ahead of the demand, shrinks only after
//   a whole window has peaked below half of what it holds, rounds to its step, and never passes
//   its limit;
// - a renderer whose chain shrinks to its frames and grows back **draws exactly what a renderer
//   whose chain holds the whole scene draws**, colour and ids, frame after frame, while the camera
//   moves between a cut that wants little and one that wants much — on the mesh path with traced
//   shadows and on the ray path, where a structure the chain did not build would be a hole;
// - past the budget, a frame drops whole instances' structures and says so, and what that costs is
//   shadows: the surfaces are the same surfaces, and no pixel is shadowed that the full chain left
//   lit.
//
// The scene is the procedural heightfield as a grid of instances, so it needs no content. The GPU
// cases skip on a device without cluster acceleration structures and ray queries.
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/rt_capacity.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 240;

// A tunable set for the length of a scope, and put back after it: the policy reads
// `renderer.rt.*` once, when a renderer is created, and the other cases in the binary must see
// the defaults.
struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

SceneDesc terrain_grid() {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 129;
  desc.grid_instances = 3;  // nine instances, so there is something to drop whole
  desc.cache = false;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  bool build(const gfx::Device& device, const RenderSettings& settings, std::string& error) {
    if (!load_scene(terrain_grid(), data, error)) return false;
    resolve_settings(settings, device.features(), &data, resolved);
    if (!resolved.rt_chain) {
      error = "the settings resolved to no ray tracing chain";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc desc;
    desc.width = k_width;
    desc.height = k_height;
    return renderer.create(device, scene, resolved, desc, &error);
  }
  ~Rig() {
    renderer.destroy();
    scene.destroy();
  }
};

bool traces(const gfx::Device& device) {
  return device.features().cluster_acceleration_structure && device.features().ray_query;
}

FrameDesc frame_at(const SceneData& data, f32 distance, u64 index, u32 view_mode = ~u32{0}) {
  FrameDesc frame;
  frame.camera = orbit_camera(data.center, data.radius, distance, 0);
  frame.frame_index = index;
  frame.view_mode = view_mode;
  return frame;
}

// The budget case's body, in a function of its own so that its scenes and renderers are gone
// before the device they were made on.
void run_starved(const gfx::Device& device);

}  // namespace

TEST_CASE("rt capacity: grows ahead, shrinks late, steps, and stops at the limit") {
  RtCapacityConfig config;
  config.limit = 100000;
  config.step = 4096;
  config.headroom_pct = 25;
  config.shrink_frames = 8;
  RtCapacity policy;
  policy.reset(config, 65536);
  CHECK(policy.capacity() == 65536);
  CHECK(policy.target(1000) == 4096);     // 1,250 rounds up to one step
  CHECK(policy.target(4000) == 8192);     // 5,000 rounds up to two
  CHECK(policy.target(90000) == 100000);  // clamped to the limit

  // A window of frames that want little: nothing moves until the window is whole.
  for (u32 f = 0; f < 7; ++f)
    CHECK(policy.observe(1000) == 65536);
  const u32 shrunk = policy.observe(1000);
  CHECK(shrunk == 4096);
  policy.resized(shrunk);

  // Into the headroom but not past half of it: no growth. Past half of it: growth, to the demand
  // plus the whole headroom, in steps.
  CHECK(policy.observe(3600) == 4096);     // 3,600 + 12.5% = 4,050 still fits
  const u32 grown = policy.observe(3700);  // 4,162.5 does not
  CHECK(grown == 8192);
  policy.resized(grown);

  // A demand that swings between two cuts settles on the larger: the window's peak is what a
  // shrink is judged by, and 3,300 with its headroom still rounds up to the 8,192 it holds.
  for (u32 f = 0; f < 32; ++f)
    CHECK(policy.observe(f % 2 == 0 ? 500u : 3300u) == 8192);

  // Past the limit there is nothing to grow into: the capacity stops there and stays.
  const u32 limited = policy.observe(250000);
  CHECK(limited == 100000);
  policy.resized(limited);
  CHECK(policy.observe(250000) == 100000);
  CHECK(policy.capacity() == 100000);
}

TEST_CASE("rt capacity: a chain sized by the frame draws what a chain sized by the scene draws") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!traces(device)) {
    MESSAGE("no cluster acceleration structures or ray queries: nothing to size");
    return;
  }
  for (const RasterMode raster : {RasterMode::Hardware, RasterMode::RayTrace}) {
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::RayTraced;
    // The whole-scene reference: a shrink window no run reaches, so its chain keeps the union
    // `GpuScene` gave a scene this small, exactly as every build before this one did.
    Rig whole;
    {
      ScopedTunable never("renderer.rt.shrink_frames", "1000000");
      REQUIRE_MESSAGE(whole.build(device, settings, error), error);
    }
    // The frame-sized one: a window of four frames, no headroom and a step of 64 clusters, so it
    // shrinks onto its cut at once and has to grow for every cut larger than the last.
    Rig frame;
    {
      ScopedTunable soon("renderer.rt.shrink_frames", "4");
      ScopedTunable tight("renderer.rt.headroom_pct", "0");
      ScopedTunable fine("renderer.rt.step_clusters", "64");
      REQUIRE_MESSAGE(frame.build(device, settings, error), error);
    }
    REQUIRE(whole.scene.rt_capacity() == whole.scene.rt_union_clusters());
    REQUIRE(frame.scene.rt_capacity() == frame.scene.rt_union_clusters());

    // Far, near, far, nearer: each near cut wants more than the far one left room for.
    const f32 distances[] = {9.0f, 1.4f, 9.0f, 0.8f, 5.0f};
    u32 smallest = frame.scene.rt_capacity();
    u32 frame_index = 0;
    u32 differing_colour = 0;
    u32 differing_ids = 0;
    for (const f32 distance : distances) {
      for (u32 k = 0; k < 8; ++k, ++frame_index) {
        REQUIRE_MESSAGE(
            whole.renderer.render_offscreen(frame_at(whole.data, distance, frame_index), &error),
            error);
        REQUIRE_MESSAGE(
            frame.renderer.render_offscreen(frame_at(frame.data, distance, frame_index), &error),
            error);
        smallest = std::min(smallest, frame.scene.rt_capacity());
      }
      CapturedFrame a;
      CapturedFrame b;
      REQUIRE_MESSAGE(whole.renderer.capture(frame_at(whole.data, distance, frame_index),
                                             {.color = true, .ids = true}, a, &error),
                      error);
      // The one frame that can outgrow a shrunk chain is a blocking one here, and a blocking
      // frame is drawn complete (`render_offscreen` grows and draws it again).
      REQUIRE_MESSAGE(frame.renderer.capture(frame_at(frame.data, distance, frame_index),
                                             {.color = true, .ids = true}, b, &error),
                      error);
      ++frame_index;
      REQUIRE(a.color.size() == b.color.size());
      REQUIRE(a.ids.size() == b.ids.size());
      u32 colour = 0;
      for (u32 i = 0; i < a.color.size(); ++i)
        colour += a.color[i] != b.color[i] ? 1u : 0u;
      u32 ids = 0;
      for (u32 i = 0; i < a.ids.size(); ++i)
        ids += a.ids[i] != b.ids[i] ? 1u : 0u;
      differing_colour += colour;
      differing_ids += ids;
      MESSAGE(std::string(raster_name(raster))
              << " at " << distance << " radii: wanted " << frame.renderer.stats().rt.wanted
              << ", capacity " << frame.scene.rt_capacity() << " of "
              << frame.scene.rt_union_clusters() << "; " << colour << " colour bytes and " << ids
              << " id words differ");
    }
    const RtStats& rt = frame.renderer.stats().rt;
    MESSAGE(std::string(raster_name(raster))
            << ": " << rt.grows << " grows, " << rt.shrinks << " shrinks, smallest capacity "
            << smallest << " of " << frame.scene.rt_union_clusters() << ", peak wanted "
            << rt.peak_wanted << ", bytes " << rt.bytes << " against " << whole.scene.rt_bytes());
    CHECK(differing_colour == 0);
    CHECK(differing_ids == 0);
    // And the frame-sized chain really was sized by the frames: it shrank under the union, grew
    // back for the near cuts, and ends holding less than the scene-sized one.
    CHECK(rt.shrinks > 0);
    CHECK(rt.grows > 0);
    CHECK(smallest < frame.scene.rt_union_clusters());
    CHECK(frame.scene.rt_capacity() <= frame.scene.rt_union_clusters());
  }
  device.destroy();
}

TEST_CASE("rt capacity: past the budget a frame drops whole instances and costs only shadows") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!traces(device)) {
    MESSAGE("no cluster acceleration structures or ray queries: nothing to size");
    return;
  }
  run_starved(device);
  device.destroy();
}

namespace {

void run_starved(const gfx::Device& device) {
  std::string error;
  RenderSettings settings;
  settings.raster = RasterMode::Hardware;
  settings.shadows = ShadowMode::RayTraced;
  settings.lights = false;  // the sun alone, which is what the shadow view shows
  // A quarter-pixel threshold and a camera among the instances: a cut of several hundred clusters,
  // which is several times what the budget below holds.
  settings.lod_px = 0.25f;
  Rig full;
  REQUIRE_MESSAGE(full.build(device, settings, error), error);
  // One mebibyte is about 165 clusters of the driver's worst case: well under what the grid wants
  // up close, so the frame keeps the instances that fit and drops the rest.
  settings.rt_budget_mib = 1;
  Rig starved;
  REQUIRE_MESSAGE(starved.build(device, settings, error), error);
  REQUIRE(starved.scene.rt_capacity_limit() < starved.scene.rt_union_clusters());

  const f32 distance = 0.8f;
  const u32 shadow_view = static_cast<u32>(gfx::ResolveMode::Shadow);
  CapturedFrame a;
  CapturedFrame b;
  for (u32 f = 0; f < 6; ++f) {
    REQUIRE_MESSAGE(full.renderer.render_offscreen(frame_at(full.data, distance, f), &error),
                    error);
    REQUIRE_MESSAGE(starved.renderer.render_offscreen(frame_at(starved.data, distance, f), &error),
                    error);
  }
  REQUIRE_MESSAGE(full.renderer.capture(frame_at(full.data, distance, 6, shadow_view),
                                        {.color = true, .ids = true}, a, &error),
                  error);
  REQUIRE_MESSAGE(starved.renderer.capture(frame_at(starved.data, distance, 6, shadow_view),
                                           {.color = true, .ids = true}, b, &error),
                  error);
  const RtStats& rt = starved.renderer.stats().rt;
  MESSAGE("budget of " << rt.limit << " clusters against " << rt.wanted << " wanted: built "
                       << rt.built << ", " << rt.overflow_frames << " frames dropped "
                       << rt.dropped_instances << " instances' drawn clusters and "
                       << rt.dropped_caster_instances << " instances' casters");
  REQUIRE(rt.wanted > rt.limit);  // the case is about a frame past its budget
  CHECK(rt.built <= rt.limit);
  CHECK(rt.capacity == rt.limit);
  CHECK(rt.overflow_frames > 0);
  CHECK(rt.dropped_instances + rt.dropped_caster_instances > 0);
  // The surfaces are the rasterizer's and did not change; the shadows lost casters, so no pixel
  // is in shadow that the full chain left lit.
  REQUIRE(a.ids.size() == b.ids.size());
  u32 id_differences = 0;
  for (u32 i = 0; i < a.ids.size(); ++i)
    id_differences += a.ids[i] != b.ids[i] ? 1u : 0u;
  CHECK(id_differences == 0);
  u32 shadowed_full = 0;
  u32 shadowed_starved = 0;
  u32 gained = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const bool sa = a.color[u64{p} * 4] == 0;
    const bool sb = b.color[u64{p} * 4] == 0;
    shadowed_full += sa ? 1u : 0u;
    shadowed_starved += sb ? 1u : 0u;
    gained += sb && !sa ? 1u : 0u;
  }
  MESSAGE("shadowed pixels: " << shadowed_full << " with the whole chain, " << shadowed_starved
                              << " past the budget");
  CHECK(gained == 0);
  CHECK(shadowed_starved <= shadowed_full);
}

}  // namespace
