// The resolve's pipeline started when the scene file has been read (roadmap R205, second leg;
// docs/subsystems/renderer.md, "The resolve compiles from the scene file"). What must hold, and is
// held here by counts, never by a clock: the pipeline a scene file names is compiled once, early,
// and the renderer's creation compiles none of it; a variant the file did not name costs one more
// compile and never a wrong pipeline; and the renderer that took the early pipeline draws the bytes
// one that made its own draws. Each device has a pipeline cache of the case's own, so the counts
// are the device's creations and not another run's cache.

#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline_batch.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 256;
constexpr u32 k_height = 144;

struct Gpu {
  test::TempDir tmp{"renderer_early_pipelines"};
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    gfx::DeviceOptions options = renderer_test::device_options();
    options.pipeline_cache_dir = tmp.file("pipeline-cache");
    std::string error;
    if (!device.create(options, &error)) {
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

// The renderer's own heightfield: a scene file's description that names no water, no layered
// ground, no generated mesh and no world, so every flag of its variant is the file's to say.
SceneDesc hills() {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  return desc;
}

FrameDesc looking() {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.dt_s = 1.0 / 60.0;
  return frame;
}

// The device's creations of the resolve's one-draw pipeline from `module`: every compile, early
// (on the worker, outside any batch) or at the renderer's creation (in its batch).
struct Compiles {
  u32 early = 0;
  u32 at_creation = 0;
};
Compiles resolve_compiles(const gfx::Device& device, const std::string& module) {
  Vector<gfx::PipelineCreation> all;
  gfx::pipeline_creations(device, all);
  Compiles out;
  for (const gfx::PipelineCreation& c : all) {
    if (c.shader != module || c.entry != "vs_fullscreen/fs_resolve") continue;
    ++(c.batched ? out.at_creation : out.early);
  }
  return out;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  std::string error;

  bool load(const gfx::Device& device) {
    if (!load_scene(hills(), data, error)) return false;
    resolve_settings(RenderSettings{}, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    return scene.create(device, data, resolved, &error);
  }
  bool render(const gfx::Device& device, EarlyPipelines* early, CapturedFrame& out) {
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.early = early;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    return renderer.capture(looking(), channels, out, &error);
  }
};

// The variant the file names, under the settings resolved before the scene was read.
ResolveVariant named_variant(const gfx::Device& device) {
  ResolvedSettings early;
  resolve_settings(RenderSettings{}, device.features(), nullptr, early);
  return predict_resolve_variant(hills(), early, gfx::Format::R8G8B8A8Unorm);
}

}  // namespace

TEST_CASE("renderer: the resolve a scene file names compiles once, early, and is the one drawn") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const ResolveVariant variant = named_variant(gpu.device);
  CHECK_FALSE(variant.water);
  CHECK_FALSE(variant.terrain);
  CHECK_FALSE(variant.stochastic);
  CHECK_FALSE(variant.foliage);

  EarlyPipelines early;
  std::string error;
  REQUIRE_MESSAGE(early.start(gpu.device, variant, std::string(), &error), error);
  const std::string module = early.stats().key.module;
  CHECK(module == (variant.shadows ? "visibility_resolve_rt" : "visibility_resolve"));

  Rig rig;
  REQUIRE_MESSAGE(rig.load(gpu.device), rig.error);
  CapturedFrame taken;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, taken), rig.error);
  const EarlyPipelines::Stats stats = early.stats();
  CHECK(stats.started);
  CHECK(stats.done);
  CHECK(stats.ok);
  CHECK(stats.taken);
  CHECK(stats.mismatched == 0);
  // One compile for one key: the worker's, and none at the renderer's creation.
  const Compiles with_early = resolve_compiles(gpu.device, module);
  CHECK(with_early.early == 1);
  CHECK(with_early.at_creation == 0);
  // The prediction is what the GPU scene decided, flag for flag.
  const GpuScene& scene = rig.scene;
  CHECK(variant.water == scene.has_water());
  CHECK(variant.terrain == scene.has_terrain_materials());
  CHECK(variant.stochastic == scene.has_stochastic_materials());
  CHECK(variant.foliage == scene.has_foliage_materials());
  // One shape of the bindless set (M1): the early layout's capacity is the scene's, and it is in
  // the key, so a set of another shape is another key and never takes this pipeline.
  CHECK(stats.key.layout == scene.bindless().capacity());
  CHECK(stats.key.layout.sampled_images <= GpuScene::bindless_config().sampled_images);
  ResolveKey other = stats.key;
  other.layout.sampled_images = stats.key.layout.sampled_images / 2;
  CHECK_FALSE(other == stats.key);

  // A second renderer handed the same object after the take (a shader reload's re-create) makes
  // its own, at creation, draws the same bytes, and is not counted as a wrong guess.
  CapturedFrame made;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, made), rig.error);
  CHECK(early.stats().mismatched == 0);
  const Compiles both = resolve_compiles(gpu.device, module);
  CHECK(both.early == 1);
  CHECK(both.at_creation == 1);
  REQUIRE(made.color.size() == taken.color.size());
  CHECK(std::memcmp(made.color.data(), taken.color.data(), made.color.size()) == 0);
  REQUIRE(made.ids.size() == taken.ids.size());
  CHECK(std::memcmp(made.ids.data(), taken.ids.data(), made.ids.size() * sizeof(u32)) == 0);
}

TEST_CASE("renderer: a variant the scene did not name costs one more compile, not a wrong one") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // The guess says foliage where the scene has none: another key (the specialization), so the
  // renderer makes its own pipeline and the early one is destroyed untaken.
  ResolveVariant wrong = named_variant(gpu.device);
  wrong.foliage = !wrong.foliage;
  EarlyPipelines early;
  std::string error;
  REQUIRE_MESSAGE(early.start(gpu.device, wrong, std::string(), &error), error);
  const std::string module = early.stats().key.module;

  Rig rig;
  REQUIRE_MESSAGE(rig.load(gpu.device), rig.error);
  CapturedFrame drawn;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, drawn), rig.error);
  // An untaken compile runs to its end on its worker (a driver's compile cannot be called off):
  // `destroy` waits for it and destroys its pipeline, the device still open, before it is counted.
  early.destroy();
  const EarlyPipelines::Stats stats = early.stats();
  CHECK(stats.started);
  CHECK(stats.done);
  CHECK_FALSE(stats.taken);
  CHECK(stats.mismatched == 1);
  CHECK(stats.reason.find("foliage") != std::string::npos);
  CHECK(stats.reason.find("module") == std::string::npos);
  const Compiles compiles = resolve_compiles(gpu.device, module);
  CHECK(compiles.early == 1);
  CHECK(compiles.at_creation == 1);

  // A start is refused where the split's pipelines would be made instead: those are other keys.
  EarlyPipelines refused;
  tunables::Tunable* classes = tunables::find("renderer.resolve.classes");
  REQUIRE(classes != nullptr);
  std::string previous;
  classes->append_value(previous);
  REQUIRE(classes->set_from_text("draws"));
  CHECK_FALSE(refused.start(gpu.device, named_variant(gpu.device), std::string(), &error));
  CHECK_FALSE(refused.stats().started);
  CHECK(refused.stats().reason == error);
  (void)classes->set_from_text(previous);
}

// The prediction's rules off any device: each flag the file can name, read as the GPU scene's own
// rule reads it (M2 of R205's second leg). Against the GPU scene's flags for the hills above; the
// scenes that set water, a layered ground, stochastic and foliage by content are owed a case of
// their own (renderer.md, "The resolve compiles from the scene file").
TEST_CASE("renderer: the resolve's prediction reads the file by the renderer's own rules") {
  ResolvedSettings resolved;
  const auto predict = [&resolved](const SceneDesc& desc) {
    return predict_resolve_variant(desc, resolved, gfx::Format::R8G8B8A8Unorm);
  };
  const test::TempDir tmp{"renderer_early_prediction"};
  SceneDesc base = hills();
  base.materials = tmp.file("no-pack");  // no pack: the recipes' own words only
  CHECK(predict(base) == ResolveVariant{});

  // A streamed world whose tiles hand meshes: the mesh room, by `GpuScene::wants_mesh_room`.
  SceneDesc world = base;
  world.world.enabled = true;
  world.streamed.emplace_back();
  world.streamed.back().tile_meshes = true;
  CHECK(predict(world).foliage ==
        GpuScene::wants_mesh_room(world_is_dynamic(world.world, true), false, true));
  CHECK(predict(world).foliage);
  resolved.stream = true;  // paged streaming has no mesh room
  CHECK_FALSE(predict(world).foliage);
  resolved.stream = false;
  world.streamed.back().tile_meshes = false;
  CHECK_FALSE(predict(world).foliage);

  // A recipe's `material.sampling` is read as the field it is, never matched as text.
  SceneDesc rock = base;
  rock.recipes.push_back(MeshRecipe{"rock", R"({"material": {"sampling": "Stochastic"}})"});
  CHECK(predict(rock).stochastic);
  rock.recipes.back().bytes = R"({"material": {"sampling": "Tiled"}, "stochastic": false})";
  CHECK_FALSE(predict(rock).stochastic);
  rock.recipes.back().bytes = R"({"parts": [{"material": {"sampling": "Stochastic"}}]})";
  CHECK(predict(rock).stochastic);
}
