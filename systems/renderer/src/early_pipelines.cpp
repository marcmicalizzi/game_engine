// The resolve's pipeline started when the scene file has been read (early_pipelines.h; R205's
// second leg; renderer.md, "The resolve compiles from the scene file").
#include "renderer_log.h"
#include "resolve_classes.h"

#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/gfx/display.h>
#include <domain/gfx/pipeline.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <shaders/visibility_resolve.spv.h>
#include <shaders/visibility_resolve_rt.spv.h>
#include <shaders/visibility_resolve_rt_stochastic.spv.h>
#include <shaders/visibility_resolve_rt_terrain.spv.h>
#include <shaders/visibility_resolve_rt_terrain_stochastic.spv.h>
#include <shaders/visibility_resolve_rt_water.spv.h>
#include <shaders/visibility_resolve_rt_water_stochastic.spv.h>
#include <shaders/visibility_resolve_rt_water_terrain.spv.h>
#include <shaders/visibility_resolve_rt_water_terrain_stochastic.spv.h>
#include <shaders/visibility_resolve_stochastic.spv.h>
#include <shaders/visibility_resolve_terrain.spv.h>
#include <shaders/visibility_resolve_terrain_stochastic.spv.h>
#include <shaders/visibility_resolve_water.spv.h>
#include <shaders/visibility_resolve_water_stochastic.spv.h>
#include <shaders/visibility_resolve_water_terrain.spv.h>
#include <shaders/visibility_resolve_water_terrain_stochastic.spv.h>
#include <string_view>
#include <utility>

namespace engine::renderer {

// The sixteen resolve modules, embedded: the renderer's library and the early compile's are given
// the same bytes, so the two keys agree on a module's hash.
void add_resolve_modules(gfx::ShaderLibrary& library) {
#define ENGINE_RESOLVE_MODULE(name) \
  library.add_embedded(#name, shaders::k_##name##_spirv, shaders::k_##name##_spirv_size)
  ENGINE_RESOLVE_MODULE(visibility_resolve);
  // The same resolve with the shadow rays in; only a device with acceleration structures may draw
  // with it, because its `g_scenes[]` is binding 3 of the bindless set.
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt);
  // The same two with the water branch (water.md, "Shading"), drawn with only by a scene that has
  // a water material (`resolve_module`): a branch in every resolve costs every pixel registers.
  ENGINE_RESOLVE_MODULE(visibility_resolve_water);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_water);
  ENGINE_RESOLVE_MODULE(visibility_resolve_terrain);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_terrain);
  ENGINE_RESOLVE_MODULE(visibility_resolve_water_terrain);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_water_terrain);
  // The eight with stochastic mesh materials (R131), drawn only by a scene that has one.
  ENGINE_RESOLVE_MODULE(visibility_resolve_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_water_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_water_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_terrain_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_terrain_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_water_terrain_stochastic);
  ENGINE_RESOLVE_MODULE(visibility_resolve_rt_water_terrain_stochastic);
#undef ENGINE_RESOLVE_MODULE
}

std::string resolve_manifest_path(const std::string& named) {
  if (!named.empty()) return named;
  const std::string candidate = platform::executable_directory() + "/../shaders/manifest.json";
  return io::exists(candidate) ? candidate : std::string();
}

ResolveVariant predict_resolve_variant(const SceneDesc& desc, const ResolvedSettings& resolved,
                                       gfx::Format color_format) {
  ResolveVariant v;
  v.shadows = resolved.shadows;
  v.color_format = color_format;
  for (const SceneMeshInfo& info : desc.mesh_info) {
    if (info.water.has_value()) v.water = true;
  }
  v.terrain = desc.terrain.enabled && desc.terrain.materials.has_value();
  // A streamed world whose tiles hand meshes has a mesh room, and the GPU scene keeps the foliage
  // lines for it whatever was declared (gpu_scene.cpp, R177's review finding A7).
  if (desc.world.enabled && !resolved.stream) {
    for (const StreamedPlacements& entry : desc.streamed) {
      if (entry.tile_meshes) v.foliage = true;
    }
  }
  // What only the content says: the pack's sets reach a mesh the file's entries generate, and a
  // rock recipe may ask for stochastic sampling itself (`material.sampling`).
  const bool generates = !desc.recipes.empty() || !desc.streamed.empty() ||
                         desc.placed_instances > 0 || desc.placed_buildings > 0;
  if (generates) {
    const std::string dir = desc.materials.empty()
                                ? materials::default_pack_dir(platform::executable_directory())
                                : desc.materials;
    if (const materials::Pack* pack = materials::pack_at(dir); pack != nullptr) {
      for (const materials::PackRole& role : pack->roles) {
        if (role.set.stochastic) v.stochastic = true;
        if (role.set.transmission > 0.0f || role.set.normal_spread > 0.0f) v.foliage = true;
      }
    }
    for (const MeshRecipe& recipe : desc.recipes) {
      if (recipe.bytes.find("\"stochastic\"") != std::string::npos) v.stochastic = true;
    }
  }
  return v;
}

EarlyPipelines::~EarlyPipelines() { destroy(); }

void EarlyPipelines::join() noexcept {
  if (worker_.joinable()) worker_.join();
}

bool EarlyPipelines::start(const gfx::Device& device, const ResolveVariant& variant,
                           const std::string& shader_manifest, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (device_ != nullptr) return fail("the early pipelines were started already");
  if (resolve_classes_tunable() != ResolveClassMode::Off) {
    return fail("renderer.resolve.classes is not off: the split's pipelines are made at creation");
  }
  if (variant.shadows && !device.features().acceleration_structure)
    return fail("the device has no acceleration structures for the resolve's shadow rays");
  device_ = &device;
  if (!shaders_.create(&device, error)) return false;
  add_resolve_modules(shaders_);
  const std::string manifest = resolve_manifest_path(shader_manifest);
  if (!manifest.empty() && !shaders_.load_manifest(manifest, error)) return false;
  module_ = shaders_.get(
      resolve_module_name(variant.shadows, variant.water, variant.terrain, variant.stochastic),
      error);
  if (module_ == nullptr) return false;
  if (!bindless_.create(device, gfx::BindlessConfig{}, error)) return false;
  stats_.key.module = module_->name;
  stats_.key.hash = module_->hash;
  stats_.key.color_format = variant.color_format;
  stats_.key.foliage = variant.foliage;
  stats_.started = true;
  ENGINE_LOG_INFO(log_renderer, "early pipeline started", log::field("shader", module_->name),
                  log::field("format", gfx::format_name(variant.color_format)),
                  log::field("foliage", variant.foliage));
  // The worker owns nothing: it reads the module and the layout made above and writes the handle
  // under the lock. Creating a pipeline on another thread is the device's normal use (the
  // pipeline cache is not externally synchronized; pipeline_batch.h).
  worker_ = std::thread([this] {
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = desc.fragment = module_->module;
    desc.vertex_entry = "vs_fullscreen";
    desc.fragment_entry = "fs_resolve";
    desc.layout = bindless_.pipeline_layout();
    desc.color_format = stats_.key.color_format;
    desc.specialization = resolve_specialization(stats_.key.foliage);
    gfx::PipelineHandle made;
    std::string why;
    const i64 begin = time::monotonic_ns();
    const bool ok = gfx::create_graphics_pipeline(*device_, desc, made, &why);
    const f64 seconds = static_cast<f64>(time::monotonic_ns() - begin) * 1.0e-9;
    ENGINE_LOG_INFO(log_renderer, "early pipeline made", log::field("shader", module_->name),
                    log::field("ms", seconds * 1.0e3), log::field("ok", ok));
    const std::lock_guard<std::mutex> lock(mutex_);
    pipeline_ = ok ? made : gfx::PipelineHandle{};
    error_ = std::move(why);
    stats_.done = true;
    stats_.ok = ok;
    stats_.seconds = seconds;
  });
  return true;
}

bool EarlyPipelines::has(const ResolveKey& key) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_.started && !stats_.taken && stats_.key == key;
}

bool EarlyPipelines::take(const ResolveKey& key, gfx::PipelineHandle& out) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!stats_.started || stats_.taken || !(stats_.key == key)) {
      ++stats_.mismatched;
      ENGINE_LOG_INFO(log_renderer, "early pipeline not taken", log::field("asked", key.module),
                      log::field("started", stats_.key.module),
                      log::field("same_hash", key.hash == stats_.key.hash),
                      log::field("same_format", key.color_format == stats_.key.color_format),
                      log::field("same_foliage", key.foliage == stats_.key.foliage),
                      log::field("taken", stats_.taken));
      return false;
    }
  }
  join();  // the one compile; nothing else of it runs on the caller's thread
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!pipeline_.valid()) {
    ENGINE_LOG_WARN(log_renderer, "early pipeline failed; made at creation",
                    log::field("reason", error_));
    return false;
  }
  out = pipeline_;
  pipeline_ = {};
  stats_.taken = true;
  return true;
}

void EarlyPipelines::destroy() noexcept {
  join();
  if (device_ != nullptr && pipeline_.valid()) gfx::destroy_pipeline(*device_, pipeline_);
  pipeline_ = {};
  bindless_.destroy();
  shaders_.destroy();
  module_ = nullptr;
  device_ = nullptr;
}

EarlyPipelines::Stats EarlyPipelines::stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void start_early_resolve(EarlyPipelines& early, const gfx::Device& device, const SceneDesc& desc,
                         const RenderSettings& settings, gfx::Format color_format,
                         const std::string& shader_manifest) {
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), nullptr, resolved);
  std::string why;
  if (!early.start(device, predict_resolve_variant(desc, resolved, color_format), shader_manifest,
                   &why)) {
    ENGINE_LOG_INFO(log_renderer, "early pipeline not started", log::field("reason", why));
  }
}

}  // namespace engine::renderer
