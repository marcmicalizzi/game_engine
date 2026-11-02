// The ray tracing chain sized by the frame (docs/subsystems/renderer.md, "The ray tracing chain's
// memory"). Three things hold it:
//
// - the capacity policy's arithmetic, on the CPU: it grows ahead of the demand, shrinks only after
//   a whole window of wall time has peaked below half of what it holds, rounds to its step, and
//   never passes its limit; and since R116 a growth goes back to the largest capacity of the last
//   minute and a shrink keeps half of it, so a view that returns regrows once;
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
#include <domain/gfx/resources.h>
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
  // The pair count is a u64 all the way in (`PairAccount::frame_pairs()` is one): a count past
  // 2^32 must not wrap to a small bound. The layout holds it to k_max_pairs, so this is the guard.
  CHECK(rt_max_geometry_index(u64{1} << 32, 3, true) == (u64{3} << 32) - 1);
  CHECK(rt_max_geometry_index(u64{k_max_pairs}, 3, true) == u64{3} * k_max_pairs - 1);
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
  // R107's arithmetic alone: a window of frames with no seconds to it, and neither the peak's
  // memory nor the floor (R116; the cases below hold those).
  config.shrink_seconds = 0.0;
  config.peak_seconds = 0.0;
  config.peak_frames = 0;
  config.floor_pct = 0;
  RtCapacity policy;
  f64 t = 0.0;
  policy.reset(config, 65536, t);
  CHECK(policy.capacity() == 65536);
  CHECK(policy.target(1000) == 4096);     // 1,250 rounds up to one step
  CHECK(policy.target(4000) == 8192);     // 5,000 rounds up to two
  CHECK(policy.target(90000) == 100000);  // clamped to the limit

  // A window of frames that want little: nothing moves until the window is whole.
  for (u32 f = 0; f < 7; ++f)
    CHECK(policy.observe(1000, t += 1.0) == 65536);
  const u32 shrunk = policy.observe(1000, t += 1.0);
  CHECK(shrunk == 4096);
  policy.resized(shrunk, t);

  // Into the headroom but not past half of it: no growth. Past half of it: growth, to the demand
  // plus the whole headroom, in steps.
  CHECK(policy.observe(3600, t += 1.0) == 4096);     // 3,600 + 12.5% = 4,050 still fits
  const u32 grown = policy.observe(3700, t += 1.0);  // 4,162.5 does not
  CHECK(grown == 8192);
  policy.resized(grown, t);

  // A demand that swings between two cuts settles on the larger: the window's peak is what a
  // shrink is judged by, and 3,300 with its headroom still rounds up to the 8,192 it holds.
  for (u32 f = 0; f < 32; ++f)
    CHECK(policy.observe(f % 2 == 0 ? 500u : 3300u, t += 1.0) == 8192);

  // Past the limit there is nothing to grow into: the capacity stops there and stays.
  const u32 limited = policy.observe(250000, t += 1.0);
  CHECK(limited == 100000);
  policy.resized(limited, t);
  CHECK(policy.observe(250000, t += 1.0) == 100000);
  CHECK(policy.capacity() == 100000);
}

// R116's rules, from the owner's v25 walk (docs/experiments/rt-capacity-2026-11-02.md): the
// defaults, a limit of the RTX 5090's budget, and frames at 60 a second unless a case says.
namespace {

RtCapacityConfig v25_config() {
  RtCapacityConfig config;
  config.limit = 336348;
  return config;
}

// Feeds `frames` frames of `wanted`, `dt` seconds apart, from `t`, and resizes on every answer the
// way the renderer does; counts the growths and shrinks.
struct Driven {
  RtCapacity policy;
  f64 t = 0.0;
  u32 grows = 0;
  u32 shrinks = 0;
  u32 least = ~u32{0};
  void run(u32 wanted, u32 frames, f64 dt = 1.0 / 60.0) {
    for (u32 f = 0; f < frames; ++f) {
      t += dt;
      const u32 before = policy.capacity();
      const u32 next = policy.observe(wanted, t);
      if (next != before) {
        grows += next > before ? 1u : 0u;
        shrinks += next < before ? 1u : 0u;
        // A shrink never takes away what the frame that asked for it built, with half the headroom.
        if (next < before) CHECK(u64{next} * 200 >= u64{wanted} * 225);
        policy.resized(next, t);
      }
      least = std::min(least, policy.capacity());
    }
  }
};

}  // namespace

TEST_CASE("rt capacity: a demand that falls to a tenth and returns regrows once, to the peak") {
  Driven drive;
  drive.policy.reset(v25_config(), 65536, 0.0);
  // The forest: one growth, to 200,000 and its headroom.
  drive.run(200000, 30 * 60);
  const u32 forest = drive.policy.capacity();
  CHECK(forest == 253952);  // 250,000 rounded up to the step
  CHECK(drive.grows == 1);

  // A second at a tenth of it — a glance at the sky — frees nothing: the window is ten seconds.
  drive.run(20000, 60);
  drive.run(200000, 60);
  CHECK(drive.shrinks == 0);
  CHECK(drive.grows == 1);
  CHECK(drive.policy.capacity() == forest);

  // Twenty-five seconds at a tenth — a look at the sea — holds a whole window: one shrink, to the
  // floor.
  drive.run(20000, 25 * 60);
  CHECK(drive.shrinks == 1);
  CHECK(drive.policy.capacity() == 126976);  // half the forest's, rounded up to the step
  CHECK(drive.policy.capacity() == drive.policy.floor(drive.t));

  // And back: the demand climbs faster than the headroom (the walk's 47,773 → 58,308 → 205,282),
  // which grew in a staircase of resizes; now the first growth goes straight to the peak.
  const u32 grows = drive.grows;
  for (const u32 wanted : {40000u, 60000u, 120000u, 200000u, 210000u})
    drive.run(wanted, 1);
  drive.run(200000, 60);
  CHECK(drive.grows == grows + 1);
  CHECK(drive.policy.capacity() == forest);
}

TEST_CASE("rt capacity: a window of fast frames does not shrink") {
  // A streamed world's load draws frames that want little as fast as it can; 240 of them were a
  // whole window three seconds after the owner's v25 walk began (336,348 → 40,960).
  Driven drive;
  drive.policy.reset(v25_config(), 336348, 0.0);
  drive.run(31447, 2000, 0.001);  // two seconds of 1 ms frames
  CHECK(drive.shrinks == 0);
  CHECK(drive.policy.capacity() == 336348);
  // A whole window of wall time is: to half of what the chain held, not to the load's demand.
  drive.run(31447, 9000, 0.001);
  CHECK(drive.shrinks == 1);
  CHECK(drive.policy.capacity() == 172032);  // 168,174 rounded up to the step
  // And the world arriving grows it once, back to what it held.
  drive.run(54701, 1);
  drive.run(206379, 1);
  drive.run(206379, 60);
  CHECK(drive.grows == 1);
  CHECK(drive.policy.capacity() == 336348);

  // Nor does a handful of slow frames — a replay whose ground took a second a frame made a window
  // of ten seconds out of 14 frames — and a peak is kept for its frames as well as its minute.
  // 200 s of frames wanting little: the window that closes in them began in the forest's frames,
  // and the next is twenty frames old.
  drive.run(20000, 200, 1.0);
  CHECK(drive.shrinks == 1);
  drive.run(20000, 250, 1.0);  // a whole window of them now, to half of what it held
  CHECK(drive.shrinks == 2);
  CHECK(drive.policy.capacity() == 172032);
  drive.run(20000, 100, 1.0);  // 130 s on: the peak is past its minute but not its 3,600 frames
  drive.run(206379, 1, 1.0);
  CHECK(drive.grows == 2);
  CHECK(drive.policy.capacity() == 336348);
}

TEST_CASE("rt capacity: the floor holds half the last minute's peak, and halves once a minute") {
  Driven drive;
  drive.policy.reset(v25_config(), 65536, 0.0);
  drive.run(260000, 20 * 60);
  const u32 forest = drive.policy.capacity();
  CHECK(forest == 327680);  // 325,000 rounded up to the step
  // Nothing in view for ten minutes: the chain gives half back at the first window, and half of
  // what is left once the minute in which it held more is over, and so on down to the demand.
  Vector<u32> capacities;
  Vector<f64> times;
  for (u32 f = 0; f < 600 * 60; ++f) {
    const u32 before = drive.policy.capacity();
    drive.run(1000, 1);
    if (drive.policy.capacity() != before) {
      capacities.push_back(drive.policy.capacity());
      times.push_back(drive.t);
    }
  }
  REQUIRE(capacities.size() >= 2);
  CHECK(drive.grows == 1);
  CHECK(capacities[0] == forest / 2);        // 163,840: the floor, not the demand's 4,096
  CHECK(times[0] - 20.0 <= 2 * 10.0 + 0.1);  // the first whole window after the fall
  for (u32 i = 1; i < capacities.size(); ++i) {
    CAPTURE(i);
    CHECK(capacities[i] < capacities[i - 1]);
    CHECK(u64{capacities[i]} * 2 >= capacities[i - 1]);  // never below half of what it held
    CHECK(times[i] - times[i - 1] >= 60.0);              // and that held for a minute
  }
  CHECK(drive.policy.capacity() == drive.policy.target(1000));  // and in the end the demand
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
  // Both contracts (R110): with a rasterized picture an untraced pair is never built, so the
  // estimate leaves the grass out; on the ray path its drawn entries are the picture's, so the
  // estimate counts every pair, the tile's included while it stands.
  for (const bool ray_path : {false, true}) {
    CAPTURE(ray_path);
    RenderSettings settings;
    settings.shadows = ShadowMode::RayTraced;
    if (ray_path) settings.raster = RasterMode::RayTrace;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    REQUIRE(resolved.ray_path == ray_path);
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
    SceneInstance placed;
    placed.mesh = 0;
    GpuScene::TileChange added;
    REQUIRE_MESSAGE(scene.set_dynamic_instances(std::span(&placed, 1), {}, added, nullptr, &error),
                    error);
    CHECK(scene.flora_stats().untraced == data.instances.size() + 1);
    const u64 untraced_with_tile = u64{data.pair_count} + data.parts[0].cluster_count;
    CHECK(scene.rt_frame_estimate() ==
          u64{scene.pair_stride()} - (ray_path ? 0u : untraced_with_tile));
    GpuScene::TileChange removed;
    REQUIRE_MESSAGE(scene.set_dynamic_instances({}, {}, removed, nullptr, &error), error);
    CHECK(removed.layout.freed == 1);
    CHECK(removed.slots_written == 0);
    CHECK(scene.flora_stats().untraced == data.instances.size());
    CHECK(scene.rt_frame_estimate() ==
          u64{scene.pair_stride()} - (ray_path ? 0u : u64{data.pair_count}));
  }
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

// R107: the default budget follows the device — a sixteenth of its device-local memory, never
// under 512 MiB nor over 4,096 — and the setting, then the tunable, override it. Until 2026-10-27
// it was 1,024 MiB on every card, which the forest proof of concept's 243,604 structures a frame
// at 11520×2160 outgrew on a 32 GB card.
TEST_CASE("rt capacity: the default budget is a sixteenth of the device, 512 to 4096 MiB") {
  constexpr u64 k_mib = u64{1} << 20;
  // The card classes the runners and the owner have, by the heaps' size the driver reports.
  CHECK(rt_device_budget_mib(32404 * k_mib) == 2025);  // RTX 5090
  CHECK(rt_device_budget_mib(24 * 1024 * k_mib) == 1536);
  CHECK(rt_device_budget_mib(16 * 1024 * k_mib) == 1024);  // the old default
  CHECK(rt_device_budget_mib(12 * 1024 * k_mib) == 768);   // TITAN X, TITAN Xp
  CHECK(rt_device_budget_mib(8 * 1024 * k_mib) == 512);    // an 8 GB laptop card
  // The floor: a 6 GB card, and a device that reports no device-local memory at all.
  CHECK(rt_device_budget_mib(6 * 1024 * k_mib) == k_rt_budget_floor_mib);
  CHECK(rt_device_budget_mib(0) == k_rt_budget_floor_mib);
  // The ceiling: 64 GB and up.
  CHECK(rt_device_budget_mib(64 * 1024 * k_mib) == k_rt_budget_ceiling_mib);
  CHECK(rt_device_budget_mib(96 * 1024 * k_mib) == k_rt_budget_ceiling_mib);
  CHECK(rt_device_budget_mib(~u64{0}) == k_rt_budget_ceiling_mib);
  // Rounded down to whole MiB, never up: a budget is a ceiling.
  CHECK(rt_device_budget_mib(16 * 1024 * k_mib + 15 * k_mib) == 1024);

  // The setting first, then the tunable, then the device.
  const RtBudget named = resolve_rt_budget(1536, 2048, 32404 * k_mib);
  CHECK(named.mib == 1536);
  CHECK(named.source == RtBudgetSource::Setting);
  const RtBudget tuned = resolve_rt_budget(0, 2048, 32404 * k_mib);
  CHECK(tuned.mib == 2048);
  CHECK(tuned.source == RtBudgetSource::Tunable);
  const RtBudget device = resolve_rt_budget(0, 0, 12 * 1024 * k_mib);
  CHECK(device.mib == 768);
  CHECK(device.source == RtBudgetSource::Device);
  // An explicit budget is not clamped by the rule: a setting below the floor or above the ceiling
  // is what the caller asked for (the tests' 1 MiB budgets, a measurement's 8 GiB).
  CHECK(resolve_rt_budget(1, 0, 32404 * k_mib).mib == 1);
  CHECK(resolve_rt_budget(0, 8192, 32404 * k_mib).mib == 8192);
  CHECK(std::string(rt_budget_source_name(RtBudgetSource::Setting)) == "setting");
  CHECK(std::string(rt_budget_source_name(RtBudgetSource::Tunable)) == "tunable");
  CHECK(std::string(rt_budget_source_name(RtBudgetSource::Device)) == "device");
}

// The cluster cache's ceiling follows the device too (R114): a sixty-fourth of the heaps, never
// under the 256 MiB every card had before nor over 1,024; the tunable, when it is not zero, as
// given.
TEST_CASE("rt capacity: the cache's ceiling is a sixty-fourth of the device, 256 to 1024 MiB") {
  constexpr u64 k_mib = u64{1} << 20;
  CHECK(rt_device_cache_mib(32404 * k_mib) == 506);  // RTX 5090: 86,357 slots at 6,144 bytes
  CHECK(rt_device_cache_mib(24 * 1024 * k_mib) == 384);
  CHECK(rt_device_cache_mib(16 * 1024 * k_mib) == 256);  // the old ceiling, on every card
  CHECK(rt_device_cache_mib(12 * 1024 * k_mib) == k_rt_cache_floor_mib);
  CHECK(rt_device_cache_mib(8 * 1024 * k_mib) == k_rt_cache_floor_mib);
  CHECK(rt_device_cache_mib(0) == k_rt_cache_floor_mib);
  CHECK(rt_device_cache_mib(64 * 1024 * k_mib) == k_rt_cache_ceiling_mib);
  CHECK(rt_device_cache_mib(~u64{0}) == k_rt_cache_ceiling_mib);
  // v24's wanted set, 53,000 to 55,000 cached clusters a frame, fits the 5090's with room.
  CHECK(u64{rt_device_cache_mib(32404 * k_mib)} * k_mib / 6144 > 55000);
  const RtBudget device = resolve_rt_cache_mib(0, 32404 * k_mib);
  CHECK(device.mib == 506);
  CHECK(device.source == RtBudgetSource::Device);
  const RtBudget tuned = resolve_rt_cache_mib(64, 32404 * k_mib);
  CHECK(tuned.mib == 64);
  CHECK(tuned.source == RtBudgetSource::Tunable);
}

// The same rule on a real scene: with nothing named, the chain's limit is the device's budget in
// clusters, and the scene and its renderer say where it came from.
TEST_CASE("rt capacity: a scene with no budget named takes the device's, and says so") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("cluster ray tracing unavailable: " << error);
    return;
  }
  gfx::MemoryBudget memory;
  REQUIRE(device.memory_budget(memory));
  SceneData data;
  REQUIRE_MESSAGE(load_scene(terrain_grid(), data, error), error);
  const auto made = [&](u32 setting_mib, RtBudgetSource source, u32 mib) {
    RenderSettings settings;
    settings.shadows = ShadowMode::RayTraced;
    settings.rt_budget_mib = setting_mib;
    ResolvedSettings resolved;
    resolve_settings(settings, device.features(), &data, resolved);
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
    CAPTURE(setting_mib);
    CHECK(scene.rt_budget_source() == source);
    CHECK(scene.rt_budget_mib() == mib);
    REQUIRE(scene.rt_bytes_per_cluster() > 0);
    const u64 clusters = u64{mib} * 1024 * 1024 / scene.rt_bytes_per_cluster();
    CHECK(scene.rt_capacity_limit() == std::min<u64>(clusters, scene.rt_union_clusters()));
    SceneRenderer renderer;
    SceneRenderer::Desc desc;
    desc.width = k_width;
    desc.height = k_height;
    REQUIRE_MESSAGE(renderer.create(device, scene, resolved, desc, &error), error);
    REQUIRE(renderer.render_offscreen(FrameDesc{}, &error));
    CHECK(renderer.stats().rt.budget_mib == mib);
    CHECK(renderer.stats().rt.budget_source == source);
    renderer.destroy();
    scene.destroy();
  };
  const u32 rule = rt_device_budget_mib(memory.device_local_bytes);
  MESSAGE("device-local " << (memory.device_local_bytes >> 20) << " MiB: the default budget is "
                          << rule << " MiB");
  made(0, RtBudgetSource::Device, rule);
  made(64, RtBudgetSource::Setting, 64);
  {
    const ScopedTunable tuned("renderer.rt.budget_mib", "96");
    made(0, RtBudgetSource::Tunable, 96);
    made(64, RtBudgetSource::Setting, 64);
  }
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
  // The per-frame chain's policy: the grid is rigid, so with the cluster cache (renderer.md, "The
  // resident cluster structure cache") it would build nothing per frame and have nothing to size.
  ScopedTunable per_frame("renderer.rt.cache", "false");
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
    // shrinks onto its cut at once and has to grow for every cut larger than the last — with R116's
    // seconds, floor and memory of a peak switched off, which would hold it at the first cut's.
    Rig frame;
    {
      ScopedTunable soon("renderer.rt.shrink_frames", "4");
      ScopedTunable no_seconds("renderer.rt.shrink_seconds", "0");
      ScopedTunable no_floor("renderer.rt.floor_pct", "0");
      ScopedTunable no_peak("renderer.rt.peak_seconds", "0");
      ScopedTunable no_peak_frames("renderer.rt.peak_frames", "0");
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
  // The frame's own builds past their budget; the cache's references have a ceiling of their own
  // (`renderer.rt.reference_mib`), which the grid's cut is nowhere near.
  ScopedTunable per_frame("renderer.rt.cache", "false");
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

// ---- the resident cluster structure cache (renderer.md, "The resident cluster structure cache";
// R101) ----------------------------------------------------------------------------------------

namespace {

struct Pictures {
  u32 colour = 0;
  u32 ids = 0;
};

Pictures compare(const CapturedFrame& a, const CapturedFrame& b) {
  Pictures out;
  REQUIRE(a.color.size() == b.color.size());
  REQUIRE(a.ids.size() == b.ids.size());
  for (u32 i = 0; i < a.color.size(); ++i)
    out.colour += a.color[i] != b.color[i] ? 1u : 0u;
  for (u32 i = 0; i < a.ids.size(); ++i)
    out.ids += a.ids[i] != b.ids[i] ? 1u : 0u;
  return out;
}

}  // namespace

// The grid is nine instances of one rigid heightfield, so with the cache every structure a frame
// traces is a cached one: the frame builds no CLAS of its own, a cluster is built the first frame
// any instance draws it, and a second flight over the same cuts builds nothing at all. Against
// the per-frame build of the same cuts (`renderer.rt.cache=false`) the traced shadows of the mesh
// path and the ray path's picture are the same bytes, colour and ids, at every stop.
TEST_CASE("rt cache: rigid structures built once trace the per-frame build's bytes") {
  ScopedTunable on("renderer.rt.cache", "true");  // whatever the default (true since R106)
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("tracing unavailable: " << error);
    return;
  }
  for (const RasterMode raster : {RasterMode::Hardware, RasterMode::RayTrace}) {
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    Rig per_frame;
    {
      ScopedTunable off("renderer.rt.cache", "false");
      REQUIRE_MESSAGE(per_frame.build(device, settings, error), error);
    }
    Rig cached;
    REQUIRE_MESSAGE(cached.build(device, settings, error), error);
    REQUIRE(cached.scene.rt_cache());
    REQUIRE_FALSE(per_frame.scene.rt_cache());
    const f32 distances[] = {9.0f, 1.4f, 0.8f, 5.0f};
    u64 frame_index = 0;
    u64 built_first = 0;
    for (u32 flight = 0; flight < 2; ++flight) {
      const u64 before = cached.renderer.stats().rt.cache_built_total;
      Pictures differ;
      for (const f32 distance : distances) {
        for (u32 k = 0; k < 3; ++k, ++frame_index) {
          REQUIRE_MESSAGE(per_frame.renderer.render_offscreen(
                              frame_at(per_frame.data, distance, frame_index), &error),
                          error);
          REQUIRE_MESSAGE(cached.renderer.render_offscreen(
                              frame_at(cached.data, distance, frame_index), &error),
                          error);
          // No rigid cluster is built per frame: everything this grid traces is the cache's.
          CHECK(cached.renderer.stats().rt.built == 0);
          CHECK(cached.renderer.stats().rt.references > 0);
        }
        for (const u32 view : {~u32{0}, static_cast<u32>(gfx::ResolveMode::Shadow)}) {
          CapturedFrame a;
          CapturedFrame b;
          REQUIRE_MESSAGE(
              per_frame.renderer.capture(frame_at(per_frame.data, distance, frame_index, view),
                                         {.color = true, .ids = true}, a, &error),
              error);
          REQUIRE_MESSAGE(
              cached.renderer.capture(frame_at(cached.data, distance, frame_index, view),
                                      {.color = true, .ids = true}, b, &error),
              error);
          ++frame_index;
          const Pictures d = compare(a, b);
          differ.colour += d.colour;
          differ.ids += d.ids;
        }
      }
      const RtStats& rt = cached.renderer.stats().rt;
      const u64 built = rt.cache_built_total - before;
      if (flight == 0) built_first = built;
      MESSAGE(std::string(raster_name(raster))
              << " flight " << flight << ": " << built << " structures built into the cache ("
              << rt.cache_occupied << " of " << rt.cache_slots << " slots, " << rt.cache_clusters
              << " clusters of " << rt.cache_meshes << " meshes), " << rt.references
              << " references the last frame, " << rt.cache_evictions << " evictions; "
              << differ.colour << " colour bytes and " << differ.ids
              << " id words differ from the per-frame build, which built "
              << per_frame.renderer.stats().rt.built << " the last frame");
      CHECK(differ.colour == 0);
      CHECK(differ.ids == 0);
      CHECK(rt.cache_overflow == 0);
      CHECK(rt.cache_evictions == 0);
      if (flight == 0) {
        CHECK(built > 0);
      } else {
        CHECK(built == 0);  // nothing new entered the cut: the second flight builds nothing
      }
    }
    CHECK(built_first <= cached.scene.rt_cache_slots());
  }
  device.destroy();
}

// A cache too small for the flight but not for any one frame of it: the near cut's clusters and
// the far cut's do not fit together, so each move evicts the slots the other drew least recently
// — counted — and builds its own again, and the picture is the per-frame build's throughout. The
// cache's size comes from the cuts themselves: what each one occupies alone, and together.
TEST_CASE("rt cache: a full cache evicts the least recently drawn and the picture holds") {
  ScopedTunable on("renderer.rt.cache", "true");  // whatever the default (true since R106)
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("tracing unavailable: " << error);
    return;
  }
  {  // the scenes and renderers go before the device they were made on
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    const f32 near_at = 0.8f;
    const f32 far_at = 9.0f;
    // What a cut occupies: the slots a fresh cache holds after drawing it (and then the other).
    const auto occupied = [&](std::initializer_list<f32> stops) {
      Rig rig;
      REQUIRE_MESSAGE(rig.build(device, settings, error), error);
      u64 index = 0;
      for (const f32 distance : stops) {
        for (u32 k = 0; k < 2; ++k, ++index)
          REQUIRE_MESSAGE(
              rig.renderer.render_offscreen(frame_at(rig.data, distance, index), &error), error);
      }
      return rig.renderer.stats().rt.cache_occupied;
    };
    const u32 near_alone = occupied({near_at});
    const u32 far_alone = occupied({far_at});
    const u32 together = occupied({near_at, far_at});
    MESSAGE("the near cut occupies " << near_alone << " slots, the far one " << far_alone
                                     << ", the two together " << together);
    REQUIRE(together > std::max(near_alone, far_alone));
    Rig per_frame;
    {
      ScopedTunable off("renderer.rt.cache", "false");
      REQUIRE_MESSAGE(per_frame.build(device, settings, error), error);
    }
    // Room for either cut and the inactive slot 0, not for both.
    Rig small;
    {
      const std::string slots = std::to_string(std::max(near_alone, far_alone) + 1);
      ScopedTunable tight("renderer.rt.cache_slots", slots.c_str());
      REQUIRE_MESSAGE(small.build(device, settings, error), error);
    }
    REQUIRE(small.scene.rt_cache());
    REQUIRE(small.scene.rt_cache_slots() < together + 1);
    const f32 distances[] = {near_at, far_at, near_at, far_at, near_at};
    u64 frame_index = 0;
    Pictures differ;
    for (const f32 distance : distances) {
      for (u32 k = 0; k < 2; ++k, ++frame_index) {
        REQUIRE_MESSAGE(per_frame.renderer.render_offscreen(
                            frame_at(per_frame.data, distance, frame_index), &error),
                        error);
        REQUIRE_MESSAGE(
            small.renderer.render_offscreen(frame_at(small.data, distance, frame_index), &error),
            error);
      }
      CapturedFrame a;
      CapturedFrame b;
      const u32 shadow_view = static_cast<u32>(gfx::ResolveMode::Shadow);
      REQUIRE_MESSAGE(
          per_frame.renderer.capture(frame_at(per_frame.data, distance, frame_index, shadow_view),
                                     {.color = true, .ids = true}, a, &error),
          error);
      REQUIRE_MESSAGE(
          small.renderer.capture(frame_at(small.data, distance, frame_index, shadow_view),
                                 {.color = true, .ids = true}, b, &error),
          error);
      ++frame_index;
      const Pictures d = compare(a, b);
      differ.colour += d.colour;
      differ.ids += d.ids;
      const RtStats& rt = small.renderer.stats().rt;
      MESSAGE("at " << distance << " radii: " << rt.references << " references, "
                    << rt.cache_occupied << " of " << rt.cache_slots << " slots occupied, "
                    << rt.cache_evictions << " evictions and " << rt.cache_overflow
                    << " misses without room so far; " << d.colour << " colour bytes differ");
    }
    const RtStats& rt = small.renderer.stats().rt;
    CHECK(rt.cache_evictions > 0);  // the flight did not fit: the cache evicted, and counted it
    CHECK(rt.cache_overflow == 0);  // every frame on its own did
    CHECK(differ.colour == 0);
    CHECK(differ.ids == 0);
  }
  device.destroy();
}

// **A frame that wants more than the cache holds** (R114): the case the eviction test does not
// cover. One frame's cut has more distinct cached clusters than the cache has slots, so no eviction
// makes room — the forest proof of concept's v24 walk, whose 53,000 to 55,000 cached clusters a
// frame met 43,690 slots and flickered as the overflow pointed a different set at the inactive slot
// every frame. The clusters the cache cannot place are built in the frame's own set and referenced
// from there: the picture is the per-frame build's, colour and ids, frame after frame, on the ray
// path (where a missing structure is a hole in the ids) and under traced shadows, and
// `cache_overflow` counts what the cache kept out.
TEST_CASE("rt cache: a frame wanting more than the cache holds builds the rest itself") {
  ScopedTunable on("renderer.rt.cache", "true");  // whatever the default (true since R106)
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("tracing unavailable: " << error);
    return;
  }
  for (const RasterMode raster : {RasterMode::RayTrace, RasterMode::Hardware}) {
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::RayTraced;
    settings.lights = false;
    // A quarter-pixel threshold up close: a cut of several hundred distinct clusters (the starved
    // budget case's), against a cache of a few hundred slots.
    settings.lod_px = 0.25f;
    constexpr u32 k_slots = 200;
    Rig per_frame;
    {
      ScopedTunable off("renderer.rt.cache", "false");
      REQUIRE_MESSAGE(per_frame.build(device, settings, error), error);
    }
    Rig small;
    {
      ScopedTunable tight("renderer.rt.cache_slots", std::to_string(k_slots).c_str());
      REQUIRE_MESSAGE(small.build(device, settings, error), error);
    }
    REQUIRE(small.scene.rt_cache());
    REQUIRE(small.scene.rt_cache_slots() == k_slots);
    const f32 distance = 0.8f;
    const u32 shadow_view = static_cast<u32>(gfx::ResolveMode::Shadow);
    const u32 view = raster == RasterMode::RayTrace ? ~u32{0} : shadow_view;
    u64 frame_index = 0;
    REQUIRE_MESSAGE(per_frame.renderer.render_offscreen(
                        frame_at(per_frame.data, distance, frame_index), &error),
                    error);
    REQUIRE_MESSAGE(
        small.renderer.render_offscreen(frame_at(small.data, distance, frame_index), &error),
        error);
    ++frame_index;
    // Two frames running, each against the per-frame build of the same cut: an overflow that drew
    // a different set as nothing each frame differs in both.
    Pictures differ;
    for (u32 f = 0; f < 2; ++f, ++frame_index) {
      CapturedFrame a;
      CapturedFrame b;
      REQUIRE_MESSAGE(
          per_frame.renderer.capture(frame_at(per_frame.data, distance, frame_index, view),
                                     {.color = true, .ids = true}, a, &error),
          error);
      REQUIRE_MESSAGE(small.renderer.capture(frame_at(small.data, distance, frame_index, view),
                                             {.color = true, .ids = true}, b, &error),
                      error);
      const Pictures d = compare(a, b);
      differ.colour += d.colour;
      differ.ids += d.ids;
      const RtStats& rt = small.renderer.stats().rt;
      MESSAGE(std::string(raster_name(raster))
              << " frame " << f << ": " << rt.references << " references, " << rt.cache_misses
              << " misses, " << rt.cache_occupied << " of " << rt.cache_slots
              << " slots occupied, the frame built " << rt.built << " of " << rt.wanted
              << " itself; " << rt.cache_overflow << " misses without a slot so far; " << d.colour
              << " colour bytes and " << d.ids << " id words differ");
    }
    const RtStats& rt = small.renderer.stats().rt;
    CHECK(rt.cache_overflow > 0);  // the case: more new clusters than the cache has slots
    CHECK(rt.built > 0);           // ... which the frame built itself
    CHECK(rt.built == rt.wanted);  // ... all of them: the frame's own set had the room
    CHECK(differ.colour == 0);
    CHECK(differ.ids == 0);
  }
  device.destroy();
}

// ---- the resize swapped at a frame boundary (renderer.md, "The ray tracing chain's memory"; R116)

// A chain that resizes while frames are in flight — its new buffers made beside the old ones,
// swapped in at the frame boundary, the old ones freed once the frames that imported them have
// completed — draws exactly what a chain that never resizes draws, and never stops for the device
// to do it; the same chain resized by waiting (`renderer.rt.swap_resize=false`, every resize
// before R116) stops once a resize. Frames go in flight two at a time, as a window's do: the
// demand of a frame reaches the policy two frames later, so every resize here is made while the
// frames before it are still running.
TEST_CASE(
    "rt capacity: a resize swapped in while frames are in flight draws what a whole chain draws") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error) || !traces(device)) {
    MESSAGE("tracing unavailable: " << error);
    return;
  }
  // The per-frame chain: with the cache the rigid grid builds nothing per frame to resize.
  ScopedTunable per_frame("renderer.rt.cache", "false");
  for (const RasterMode raster : {RasterMode::RayTrace, RasterMode::Hardware}) {
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::RayTraced;
    Rig whole;
    {
      ScopedTunable never("renderer.rt.shrink_seconds", "86400");
      REQUIRE_MESSAGE(whole.build(device, settings, error), error);
    }
    for (const bool swap : {true, false}) {
      CAPTURE(swap);
      // Resizing on every cut, as the frame-sized case above: no window, floor, memory of a peak
      // or headroom, and a step of 64 clusters.
      Rig moving;
      {
        ScopedTunable soon("renderer.rt.shrink_seconds", "0");
        ScopedTunable one("renderer.rt.shrink_frames", "1");
        ScopedTunable no_floor("renderer.rt.floor_pct", "0");
        ScopedTunable no_peak("renderer.rt.peak_seconds", "0");
        ScopedTunable no_peak_frames("renderer.rt.peak_frames", "0");
        ScopedTunable tight("renderer.rt.headroom_pct", "0");
        ScopedTunable fine("renderer.rt.step_clusters", "64");
        REQUIRE_MESSAGE(moving.build(device, settings, error), error);
      }
      ScopedTunable swapping("renderer.rt.swap_resize", swap ? "true" : "false");
      const f32 distances[] = {9.0f, 1.4f, 9.0f, 0.8f, 5.0f, 1.4f};
      u64 frame_index = 0;
      u64 waits = 0;
      u32 resizes = 0;
      Pictures differ;
      for (const f32 distance : distances) {
        const u32 before = moving.renderer.stats().rt.grows + moving.renderer.stats().rt.shrinks;
        const u64 waited = gfx::device_waits_on_thread();
        for (u32 k = 0; k < 6; ++k, ++frame_index) {
          moving.renderer.begin_frame();
          REQUIRE_MESSAGE(moving.renderer.submit_frame(frame_at(moving.data, distance, frame_index),
                                                       &error) != 0,
                          error);
        }
        waits += gfx::device_waits_on_thread() - waited;
        resizes += moving.renderer.stats().rt.grows + moving.renderer.stats().rt.shrinks - before;
        // The next frame with the last six still running, and the whole chain's of the same pose.
        CapturedFrame a;
        CapturedFrame b;
        REQUIRE_MESSAGE(whole.renderer.capture(frame_at(whole.data, distance, frame_index),
                                               {.color = true, .ids = true}, a, &error),
                        error);
        REQUIRE_MESSAGE(moving.renderer.capture(frame_at(moving.data, distance, frame_index),
                                                {.color = true, .ids = true}, b, &error),
                        error);
        ++frame_index;
        const Pictures d = compare(a, b);
        differ.colour += d.colour;
        differ.ids += d.ids;
      }
      MESSAGE(std::string(raster_name(raster))
              << std::string(swap ? " swapped" : " waited") << ": " << resizes
              << " resizes in flight, " << waits << " stops for the device, " << differ.colour
              << " colour bytes and " << differ.ids << " id words differ from the whole chain");
      CHECK(resizes > 0);  // the case: the chain did resize between frames in flight
      CHECK(differ.colour == 0);
      CHECK(differ.ids == 0);
      if (swap) {
        CHECK(waits == 0);  // and never waited for the device to do it
      } else {
        CHECK(waits >= resizes);  // the comparison: every waited resize stops once
      }
    }
  }
  device.destroy();
}
