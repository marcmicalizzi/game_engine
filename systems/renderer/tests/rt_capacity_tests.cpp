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
//   lit;
// - the chain's **index space** — the geometry indices it names, the scene's pairs times the views,
//   three times that with the shadow casters — is bounded by the device and not by the budget, and
//   a scene past it starts anyway, with the casters dropped first and the chain second (the
//   settings resolution, on the CPU against a made-up limit).
//
// The scene is the procedural heightfield as a grid of instances, so it needs no content. The GPU
// cases skip on a device without cluster acceleration structures and ray queries.
#include <core/log/log.h>
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flythrough.h>
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

TEST_CASE("rt index space: compact casters share the drawn bound without refusing the chain") {
  CHECK(rt_max_geometry_index(1000, 1, false) == 999);
  CHECK(rt_max_geometry_index(1000, 3, true) == 2999);
  CHECK(rt_max_geometry_index(0, 3, true) == 0);
  CHECK(rt_max_geometry_index(3699274, 3, true) == 11097821);
  CHECK(rt_max_geometry_index(0xffffffffu, 3, true) == u64{3} * 0xffffffffu - 1);
  gfx::DeviceFeatures features;
  features.mesh_shader = features.buffer_int64_atomics = true;
  features.acceleration_structure = features.cluster_acceleration_structure = features.ray_query =
      true;
  SceneData scene;
  scene.pair_count = 1000;
  PairAccount account;
  account.prefix_pairs = 1000;
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  settings.views = ViewLayout::Surround3;
  ResolvedSettings resolved;
  features.cluster_max_geometry_index = 2999;
  resolve_settings(settings, features, &scene, resolved, &account);
  CHECK(resolved.casters);
  features.cluster_max_geometry_index = 2998;
  resolve_settings(settings, features, &scene, resolved, &account);
  CHECK_FALSE(resolved.casters);
  CHECK(resolved.caster_index_fallback);
  CHECK(resolved.shadows);
  CHECK(resolved.rt_chain);
  CHECK(check_availability(resolved, features) == RenderAvailability::Ok);
  settings.raster = RasterMode::RayTrace;
  resolve_settings(settings, features, &scene, resolved, &account);
  CHECK(resolved.ray_path);
  CHECK(resolved.shadows);
  settings.views = ViewLayout::Single;
  resolve_settings(settings, features, &scene, resolved, &account);
  CHECK(resolved.casters);
  settings.views = ViewLayout::Surround3;
  resolve_settings(settings, features, &scene, resolved);
  CHECK(resolved.casters);  // provisional resolution defers admission until terrain is known
}
TEST_CASE("rt reporting: final scene settings and typed creation differences under a tiny budget") {
  gfx::Device device;
  std::string error;
  if (!device.create({}, &error) || !traces(device)) {
    MESSAGE("cluster ray tracing unavailable: " << error);
    return;
  }
  SceneData data;
  REQUIRE_MESSAGE(load_scene(terrain_grid(), data, error), error);
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  settings.rt_budget_mib = 1;
  // Lower only this device object's advertised limit, as the CPU case does, so a small scene
  // exercises the real GpuScene admission fallback. Every build remains below the real device
  // limit; no large visibility buffers or driver mock are needed. Restore it before teardown.
  struct IndexLimit {
    gfx::DeviceFeatures& features;
    u32 previous;
    explicit IndexLimit(gfx::Device& dev, u32 limit)
        : features(const_cast<gfx::DeviceFeatures&>(dev.features())),
          previous(features.cluster_max_geometry_index) {
      features.cluster_max_geometry_index = limit;
    }
    ~IndexLimit() { features.cluster_max_geometry_index = previous; }
  } index_limit(device, data.pair_count - 2);
  ResolvedSettings requested;
  resolve_settings(settings, device.features(), &data, requested);
  REQUIRE(requested.casters);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, requested, &error), error);
  CHECK_FALSE(scene.resolved().casters);
  REQUIRE(scene.resolved().caster_index_fallback);
  // Check the other creation-difference fields on the same line as the actual admission change.
  requested.ray_path = !scene.resolved().ray_path;
  requested.rt_chain = !scene.resolved().rt_chain;
  requested.static_forms = !scene.resolved().static_forms;
  requested.caster_index_fallback = !scene.resolved().caster_index_fallback;
  struct CaptureLog {
    log::RingSink sink{512};
    CaptureLog() { log::add_sink(&sink); }
    ~CaptureLog() { log::remove_sink(&sink); }
  } capture;
  SceneRenderer renderer;
  SceneRenderer::Desc desc;
  desc.width = k_width;
  desc.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, requested, desc, &error), error);
  u32 lines = 0;
  capture.sink.for_each(0, [&](const log::RingSink::Entry& entry) {
    if (entry.message != "scene settings resolved") return;
    ++lines;
    CHECK(entry.level == log::Level::Info);
    REQUIRE(entry.fields.size() == 11);
    CHECK(entry.fields[0].kind == log::Field::Kind::String);
    CHECK(entry.fields[0].string() == "the pair account");
    const bool expected[] = {true,
                             false,
                             requested.ray_path,
                             scene.resolved().ray_path,
                             requested.rt_chain,
                             scene.resolved().rt_chain,
                             requested.static_forms,
                             scene.resolved().static_forms,
                             requested.caster_index_fallback,
                             scene.resolved().caster_index_fallback};
    const char* keys[] = {"requested_casters",
                          "resolved_casters",
                          "requested_ray_path",
                          "resolved_ray_path",
                          "requested_rt_chain",
                          "resolved_rt_chain",
                          "requested_static_forms",
                          "resolved_static_forms",
                          "requested_caster_index_fallback",
                          "resolved_caster_index_fallback"};
    for (u32 i = 0; i < 10; ++i) {
      bool found = false;
      for (const log::Field& field : entry.fields) {
        if (field.key != keys[i]) continue;
        found = true;
        CHECK(field.kind == log::Field::Kind::Bool);
        CHECK(field.value.b == expected[i]);
      }
      CHECK(found);
    }
  });
  CHECK(lines == 1);
  CHECK(renderer.stats().rt.caster_index_fallbacks == 1);
  CHECK(renderer.stats().rt.geometry_index_max == scene.rt_capacity() - 1);
  CHECK(renderer.stats().rt.geometry_index_required == data.pair_count - 1);
  CHECK(renderer.stats().rt.geometry_index_limit == device.features().cluster_max_geometry_index);
  scene::FlythroughRt report;
  summarize_rt(renderer.stats().rt, report);
  CHECK(report.geometry_index_need == report.geometry_index_max);
  CHECK(report.geometry_index_required == data.pair_count - 1);
  renderer.reset_stats();
  CHECK(renderer.stats().rt.caster_index_fallbacks == 1);
  renderer.destroy();
  capture.sink.clear();
  REQUIRE_MESSAGE(renderer.create(device, scene, scene.resolved(), desc, &error), error);
  capture.sink.for_each(0, [&](const log::RingSink::Entry& entry) {
    CHECK(entry.message != "scene settings resolved");
  });
}
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

TEST_CASE("rt capacity: removing the last untraced tile restores the frame estimate") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("tracing unavailable: " << error);
    return;
  }
  SceneData data;
  REQUIRE_MESSAGE(load_scene(terrain_grid(), data, error), error);
  data.dynamic = true;
  // A synthetic mesh with the field's policy exercises admission without a grass cook.
  data.sources.resize(data.parts.size());
  data.sources[0].species = "sward";
  RenderSettings settings;
  settings.shadows = ShadowMode::RayTraced;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneInstance placed;
  placed.mesh = 0;
  GpuScene::TileChange added;
  REQUIRE_MESSAGE(scene.set_dynamic_instances(std::span(&placed, 1), {}, added, nullptr, &error),
                  error);
  CHECK(scene.flora_stats().untraced == data.instances.size() + 1);
  GpuScene::TileChange removed;
  REQUIRE_MESSAGE(scene.set_dynamic_instances({}, {}, removed, nullptr, &error), error);
  CHECK(removed.layout.freed == 1);
  CHECK(removed.slots_written == 0);
  CHECK(scene.flora_stats().untraced == data.instances.size());
  CHECK(scene.rt_frame_estimate() == u64{scene.pair_stride()} - data.pair_count);
}

// R94: the first capacity is what the scene could want, clamped to the budget and the union, with
// `renderer.rt.initial_clusters` as a floor — where it used to be the union or 65,536, whichever
// was less, which a streamed forest's first frames outgrew one growth at a time.
TEST_CASE("rt capacity: the first capacity is the scene's estimate, the budget its ceiling") {
  // A scene inside the budget is allocated for every pair: it never drops anything.
  CHECK(first_rt_capacity(150000, 20000, 20000, 65536) == 20000);
  // A scene past it starts at the budget, not at 65,536 below it.
  CHECK(first_rt_capacity(118214, 7284526, 7284526, 65536) == 118214);
  // What the static forms take off the estimate is not allocated: 3.3 M pairs of forms in a union
  // of 3.4 M leave 80,000 a frame can build.
  CHECK(first_rt_capacity(150000, 3400000, 80000, 4096) == 80000);
  // The floor where the union is larger than the estimate, never past the union or the budget.
  CHECK(first_rt_capacity(150000, 3400000, 1000, 65536) == 65536);
  CHECK(first_rt_capacity(150000, 30000, 1000, 65536) == 30000);
  CHECK(first_rt_capacity(40000, 3400000, 1000, 65536) == 40000);
  CHECK(first_rt_capacity(150000, 0, 0, 0) == 1);
}

// R94: past the capacity a frame spends it nearest first — the prefix, then the rings inside out,
// one ring's blocks by tile key, a block's slots in order — whatever order the blocks were laid
// out in, so the same instances are dropped every run.
TEST_CASE("rt capacity: the drop order is the prefix, then the rings inside out by tile key") {
  RtDropBlock blocks[] = {{.key = 9, .rank = 3, .first_slot = 10, .slots = 2},
                          {.key = 4, .rank = 2, .first_slot = 12, .slots = 1},
                          {.key = 2, .rank = 3, .first_slot = 13, .slots = 3},
                          {.key = 7, .rank = 1, .first_slot = 16, .slots = 2}};
  Vector<u32> order;
  rt_drop_order(3, std::span<RtDropBlock>(blocks, 4), order);
  const u32 expected[] = {11, 0,                               // count, pad
                          0,  0, 1,  0, 2,  0,                 // the prefix
                          16, 1, 17, 1,                        // ring 0: key 7
                          12, 2,                               // ring 1: key 4
                          13, 3, 14, 3, 15, 3, 10, 3, 11, 3};  // ring 2: key 2, then 9
  REQUIRE(order.size() == std::size(expected));
  for (u32 i = 0; i < order.size(); ++i)
    CHECK(order[i] == expected[i]);
  // The blocks handed over in another order lay out the same order.
  RtDropBlock shuffled[] = {blocks[3], blocks[0], blocks[2], blocks[1]};
  Vector<u32> again;
  rt_drop_order(3, std::span<RtDropBlock>(shuffled, 4), again);
  REQUIRE(again.size() == order.size());
  for (u32 i = 0; i < order.size(); ++i)
    CHECK(again[i] == order[i]);
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
