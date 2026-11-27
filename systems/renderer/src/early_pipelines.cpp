// The resolve's pipeline started when the scene file has been read (early_pipelines.h; R205's
// second leg; renderer.md, "The resolve compiles from the scene file").
#include "renderer_log.h"
#include "resolve_classes.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/display.h>
#include <domain/gfx/pipeline.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/gpu_scene.h>
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
  // a water material (`resolve_module`): a branch compiled into every resolve took 0.48 ms with no
  // water on screen (2026-10-18), though on the forest's standing frame (2026-11-24, a quarter of
  // it sea) the classes that cannot be water were cheaper through the water module
  // (docs/experiments/resolve-cold-compile.md, "What the water costs the forest's frame").
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

namespace {

// A container's own word on its materials (T150): its material count and section 33's flags,
// read from its header, its section table and that one section (`geometry::ClusterFileReader`),
// with nothing decoded; a container without the section is all plain. False when the file cannot
// be opened as a container, which the load reports.
bool peek_material_flags(const std::string& path, Vector<u32>& flags) {
  geometry::ClusterFileReader reader;
  if (!reader.open(path)) return false;
  const u64 materials = reader.element_count(geometry::ClusterSection::Materials);
  flags.resize(static_cast<u32>(materials), 0u);
  u64 offset = 0;
  u64 bytes = 0;
  if (materials == 0 ||
      !reader.range(geometry::ClusterSection::MaterialSampling,
                    static_cast<u32>(sizeof(geometry::ClusterFileMaterialSampling)), 0, materials,
                    offset, bytes))
    return true;
  Vector<geometry::ClusterFileMaterialSampling> records;
  records.resize(static_cast<u32>(materials));
  u64 read = 0;
  if (reader.file().read_at(offset, records.data(), bytes, read) != io::Status::Ok || read != bytes)
    return true;
  for (u32 k = 0; k < records.size(); ++k)
    flags[k] = records[k].flags;
  return true;
}

bool foliage_optics(const materials::MaterialSet& set) noexcept {
  return set.transmission > 0.0f || set.normal_spread > 0.0f;
}

// What a recipe's parsed JSON asks for: a `material` block whose `sampling` field is the
// `Stochastic` enumerator (a rock recipe's `RockMaterial::sampling`, R131), and every string that
// names one of the pack's roles, whose set then reaches the generated mesh.
void read_recipe(const JsonValue& value, const materials::Pack* pack, ResolveVariant& v) {
  if (value.is_object()) {
    for (auto&& [key, child] : value.as_object()) {
      if (key == "material" && child.is_object()) {
        const JsonValue* sampling = child.find("sampling");
        if (sampling != nullptr && sampling->is_string() && sampling->as_string() == "Stochastic")
          v.stochastic = true;
      }
      read_recipe(child, pack, v);
    }
  } else if (value.is_array()) {
    for (const JsonValue& element : value.as_array())
      read_recipe(element, pack, v);
  } else if (value.is_string() && pack != nullptr) {
    if (const materials::MaterialSet* set = materials::find_role(pack, value.as_string());
        set != nullptr) {
      if (set->stochastic) v.stochastic = true;
      if (foliage_optics(*set)) v.foliage = true;
    }
  }
}

}  // namespace

ResolveVariant predict_resolve_variant(const SceneDesc& desc, const ResolvedSettings& resolved,
                                       gfx::Format color_format) {
  ResolveVariant v;
  v.shadows = resolved.shadows;
  v.color_format = color_format;
  for (const SceneMeshInfo& info : desc.mesh_info) {
    if (info.water.has_value()) v.water = true;
  }
  // **A mesh's materials as the load will find them** (T150; renderer.md, "Stochastic mesh
  // materials"): a container's own section 33, peeked, and then the scene's `Mesh.sampling` by the
  // load's one rule, in order. A glTF's `extras.sampling` is not read here (that is its import),
  // so a glTF stands as plain materials as many as its entries name plus one no entry names.
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    const Vector<SceneMeshInfo::MaterialSampling>* asked =
        m < desc.mesh_info.size() && !desc.mesh_info[m].sampling.empty()
            ? &desc.mesh_info[m].sampling
            : nullptr;
    Vector<u32> flags;
    const bool container = io::extension(desc.meshes[m]) == ".clusters";
    if (container) {
      if (!peek_material_flags(desc.meshes[m], flags)) continue;  // the load will say why
    } else if (asked != nullptr) {
      i32 highest = -1;
      for (const SceneMeshInfo::MaterialSampling& entry : *asked)
        highest = entry.material > highest ? entry.material : highest;
      flags.resize(static_cast<u32>(highest + 2), 0u);
    }
    std::string ignored;
    if (asked != nullptr && !apply_mesh_sampling(*asked, flags, ignored)) continue;  // refused
    for (const u32 f : flags)
      if ((f & geometry::k_material_sampling_stochastic) != 0) v.stochastic = true;
  }
  v.terrain = desc.terrain.enabled && desc.terrain.materials.has_value();
  // The mesh room keeps the foliage lines whatever was declared (gpu_scene.cpp, R177's review
  // finding A7): the GPU scene's own rule, asked with what the file says. Whether a stream setting
  // finds the scene paged is the load's to say: a stream setting is read as paged here.
  bool tile_meshes = false;
  for (const StreamedPlacements& entry : desc.streamed)
    tile_meshes = tile_meshes || entry.tile_meshes;
  if (GpuScene::wants_mesh_room(world_is_dynamic(desc.world, !desc.streamed.empty()),
                                resolved.stream, tile_meshes))
    v.foliage = true;
  // What the file's recipes use: a role they name, and a rock recipe's own `material.sampling`.
  const std::string dir = desc.materials.empty()
                              ? materials::default_pack_dir(platform::executable_directory())
                              : desc.materials;
  const materials::Pack* pack = materials::pack_at(dir);
  for (const MeshRecipe& recipe : desc.recipes) {
    JsonValue root;
    if (parse_json(recipe.bytes, root).ok) read_recipe(root, pack, v);
  }
  // What only the generators know: a generated mesh that names no role takes the pack's default
  // ones (the rocks', the banks'), so a pack with a stochastic role is read as reaching a scene
  // that generates meshes. A guess, said on renderer.md with the cases it gets wrong.
  v.light_cap = scene_light_cap(desc.fires, desc.lamps, desc.props);  // R213
  const bool generates = !desc.recipes.empty() || !desc.streamed.empty() ||
                         desc.placed_instances > 0 || desc.placed_buildings > 0;
  if (generates && pack != nullptr) {
    for (const materials::PackRole& role : pack->roles) {
      if (role.set.stochastic) v.stochastic = true;
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
    stats_.reason = sentence;
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
  std::string why;
  if (!shaders_.create(&device, &why)) return fail(why);
  add_resolve_modules(shaders_);
  const std::string manifest = resolve_manifest_path(shader_manifest);
  if (!manifest.empty() && !shaders_.load_manifest(manifest, &why)) return fail(why);
  module_ = shaders_.get(
      resolve_module_name(variant.shadows, variant.water, variant.terrain, variant.stochastic),
      &why);
  if (module_ == nullptr) return fail(why);
  // The scene's own shape (`GpuScene::bindless_config`), and the key carries what the device
  // clamped it to, so the pipeline is only ever taken by a renderer whose set matches it.
  if (!bindless_.create(device, GpuScene::bindless_config(), &why)) return fail(why);
  stats_.key.module = module_->name;
  stats_.key.hash = module_->hash;
  stats_.key.color_format = variant.color_format;
  stats_.key.foliage = variant.foliage;
  stats_.key.light_cap = variant.light_cap;
  stats_.key.layout = bindless_.capacity();
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
    desc.specialization = resolve_specialization(stats_.key.foliage, stats_.key.light_cap);
    gfx::PipelineHandle made;
    std::string failure;
    const i64 begin = time::monotonic_ns();
    const bool ok = gfx::create_graphics_pipeline(*device_, desc, made, &failure);
    const f64 seconds = static_cast<f64>(time::monotonic_ns() - begin) * 1.0e-9;
    ENGINE_LOG_INFO(log_renderer, "early pipeline made", log::field("shader", module_->name),
                    log::field("ms", seconds * 1.0e3), log::field("ok", ok));
    const std::lock_guard<std::mutex> lock(mutex_);
    pipeline_ = ok ? made : gfx::PipelineHandle{};
    error_ = std::move(failure);
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
    // Nothing started, or taken already (a shader reload's re-create): not a wrong guess.
    if (!stats_.started || stats_.taken) return false;
    if (!(stats_.key == key)) {
      ++stats_.mismatched;
      std::string differs;
      const auto add = [&differs](const char* what) {
        differs += differs.empty() ? "" : ", ";
        differs += what;
      };
      if (key.module != stats_.key.module) add("module");
      if (key.hash != stats_.key.hash) add("hash");
      if (key.color_format != stats_.key.color_format) add("format");
      if (key.foliage != stats_.key.foliage) add("foliage");
      if (key.light_cap != stats_.key.light_cap) add("light_cap");
      if (!(key.layout == stats_.key.layout)) add("layout");
      stats_.reason =
          "asked for " + key.module + ", started " + stats_.key.module + "; differs in " + differs;
      ENGINE_LOG_INFO(log_renderer, "early pipeline not taken", log::field("asked", key.module),
                      log::field("started", stats_.key.module), log::field("differs", differs));
      return false;
    }
  }
  join();  // the one compile; nothing else of it runs on the caller's thread
  const std::lock_guard<std::mutex> lock(mutex_);
  // The worker has ended: its module and layout are no longer needed by anything (a pipeline
  // keeps neither), so the set's pool and the sixteen modules go now rather than at `destroy`.
  bindless_.destroy();
  shaders_.destroy();
  module_ = nullptr;
  if (!pipeline_.valid()) {
    stats_.reason = "the early compile failed: " + error_;
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

JsonValue early_summary(const EarlyPipelines* early) {
  JsonValue out = JsonValue::object();
  if (early == nullptr) {
    out.set("started", JsonValue(false));
    return out;
  }
  const EarlyPipelines::Stats s = early->stats();
  out.set("started", JsonValue(s.started));
  out.set("done", JsonValue(s.done));
  out.set("ok", JsonValue(s.ok));
  out.set("taken", JsonValue(s.taken));
  out.set("mismatched", JsonValue(s.mismatched));
  out.set("seconds", JsonValue(s.seconds));
  if (s.started) out.set("shader", JsonValue(s.key.module));
  if (!s.reason.empty()) out.set("reason", JsonValue(s.reason));
  return out;
}

}  // namespace engine::renderer
