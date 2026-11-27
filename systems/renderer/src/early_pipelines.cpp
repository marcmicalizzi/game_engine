// The resolve's pipeline started when the scene file has been read (early_pipelines.h; R205's
// second leg; renderer.md, "The resolve compiles from the scene file").
#include "renderer_log.h"
#include "resolve_classes.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/profiling/startup_trace.h>  // a stage for the mesh-file reads: timing, never logic
#include <core/time/time.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/material_sampling.h>
#include <domain/gfx/display.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <algorithm>
#include <atomic>
#include <cstring>
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

// The whole range or nothing.
bool read_range(const std::string& path, u64 offset, void* dst, u64 bytes) {
  u64 read = 0;
  return io::read_file_range(path, offset, dst, bytes, read) == io::Status::Ok && read == bytes;
}

// R234: whether a reflectance packs to other than the stand-in's, by the record's own packer.
bool packs_specular(f32 ior, f32 specular, Vec3 colour) {
  gfx::ResolveMaterial m;
  gfx::set_dielectric_specular(m, ior, specular, colour);
  return m.specular.x != 0.0f || m.specular.y != 0.0f || m.specular.z != 0.0f ||
         m.specular.w != 0.0f;
}

f32 json_number(const JsonValue* value, f32 fallback) {
  f64 v = 0.0;
  return value != nullptr && value->get_f64(v) ? static_cast<f32>(v) : fallback;
}

// What one file says of its own materials, before the scene's word is applied over it.
struct FileFacts {
  bool specular = false;
  bool counted = false;  // its own material count was learned: `flags` holds one word a material
  Vector<u32> flags;     // `geometry::k_material_sampling_stochastic` where a material asks (T150)
};

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (usize i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// A glTF material's `extras.sampling` by the import's rule (gltf.cpp, `sampling_word`): the string
// "stochastic" in any case asks; anything else, a value that is not a string included, is no
// request.
bool asks_stochastic(const JsonValue& material) {
  const JsonValue* extras = material.is_object() ? material.find("extras") : nullptr;
  const JsonValue* sampling =
      extras != nullptr && extras->is_object() ? extras->find("sampling") : nullptr;
  return sampling != nullptr && sampling->is_string() &&
         equal_ignoring_case(sampling->as_string(), "stochastic");
}

// A glTF's JSON, parsed once for both facts: does a material state KHR_materials_ior or _specular
// at other than the defaults, and which materials ask for stochastic sampling.
void read_gltf_json(std::string_view text, FileFacts& out) {
  JsonValue root;
  if (!parse_json(text, root).ok) return;
  out.counted = true;  // a parsed file with no `materials` array has none of its own
  const JsonValue* materials = root.find("materials");
  if (materials == nullptr || !materials->is_array()) return;
  out.flags.resize(static_cast<u32>(materials->as_array().size()), 0u);
  u32 index = 0;
  for (const JsonValue& material : materials->as_array()) {
    if (asks_stochastic(material)) out.flags[index] = geometry::k_material_sampling_stochastic;
    ++index;
    if (out.specular) continue;
    const JsonValue* ext = material.is_object() ? material.find("extensions") : nullptr;
    if (ext == nullptr || !ext->is_object()) continue;
    const JsonValue* ior = ext->find("KHR_materials_ior");
    const JsonValue* spec = ext->find("KHR_materials_specular");
    if (ior == nullptr && spec == nullptr) continue;
    const bool ior_obj = ior != nullptr && ior->is_object();
    const bool spec_obj = spec != nullptr && spec->is_object();
    Vec3 colour{1.0f, 1.0f, 1.0f};
    const JsonValue* c = spec_obj ? spec->find("specularColorFactor") : nullptr;
    if (c != nullptr && c->is_array() && c->as_array().size() == 3) {
      colour = Vec3{json_number(&c->as_array()[0], 1.0f), json_number(&c->as_array()[1], 1.0f),
                    json_number(&c->as_array()[2], 1.0f)};
    }
    if (packs_specular(ior_obj ? json_number(ior->find("ior"), 1.5f) : 1.5f,
                       spec_obj ? json_number(spec->find("specularFactor"), 1.0f) : 1.0f, colour))
      out.specular = true;
  }
}

enum class MeshFileKind : u8 { none, gltf, glb, clusters };

// By suffix, without regard to case: what a file is before anything reads it.
MeshFileKind mesh_file_kind(std::string_view path) noexcept {
  const auto ends = [path](std::string_view suffix) {
    if (path.size() < suffix.size()) return false;
    const std::string_view tail = path.substr(path.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
      char c = tail[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != suffix[i]) return false;
    }
    return true;
  };
  if (ends(".gltf")) return MeshFileKind::gltf;
  if (ends(".glb")) return MeshFileKind::glb;
  if (ends(".clusters")) return MeshFileKind::clusters;
  return MeshFileKind::none;
}

std::atomic<u64> g_mesh_declaration_reads{0};

// A `.glb`'s JSON chunk, every length checked against the file's size before anything is allocated.
void read_glb(const std::string& path, u64 size, FileFacts& out) {
  u32 head[5] = {};  // magic, version, length; the first chunk's length and type
  if (size < sizeof(head) || !read_range(path, 0, head, sizeof(head))) return;
  if (head[0] != 0x46546C67u || head[1] != 2u || head[4] != 0x4E4F534Au) return;
  if (head[2] > size || u64{head[3]} > size - sizeof(head) || head[3] > (64u << 20)) return;
  std::string text(head[3], '\0');
  if (!read_range(path, sizeof(head), text.data(), head[3])) return;
  read_gltf_json(text, out);
}

// A container's sections, trusted only at this build's version and only within the file's size:
// section 55's reflectance (R234), and the material count with section 33's flags (T150), as
// `geometry::ClusterFileReader` would range them; a container without section 33 is all plain.
void read_clusters(const std::string& path, u64 size, FileFacts& out) {
  geometry::ClusterFileHeader header;
  if (size < sizeof(header) || !read_range(path, 0, &header, sizeof(header)) ||
      std::memcmp(header.magic, "CLST", 4) != 0 ||
      header.version != geometry::k_cluster_file_version || header.section_count > 4096 ||
      u64{header.section_count} * sizeof(geometry::ClusterFileSection) > size - sizeof(header))
    return;
  Vector<geometry::ClusterFileSection> sections(header.section_count);
  if (!read_range(path, sizeof(header), sections.data(),
                  u64{header.section_count} * sizeof(geometry::ClusterFileSection)))
    return;
  const geometry::ClusterFileSection* sampling = nullptr;
  u64 materials = 0;
  for (const geometry::ClusterFileSection& section : sections) {
    if (section.kind == static_cast<u32>(geometry::ClusterSection::Materials)) {
      materials = section.element_count;
      continue;
    }
    if (section.kind == static_cast<u32>(geometry::ClusterSection::MaterialSampling)) {
      sampling = &section;
      continue;
    }
    if (out.specular ||
        section.kind != static_cast<u32>(geometry::ClusterSection::MaterialSpecular) ||
        section.element_size != sizeof(geometry::ClusterFileMaterialSpecular) ||
        section.element_count > 65536)
      continue;
    const u64 bytes = section.element_count * sizeof(geometry::ClusterFileMaterialSpecular);
    if (section.offset > size || bytes > size - section.offset) return;
    Vector<geometry::ClusterFileMaterialSpecular> records(static_cast<u32>(section.element_count));
    if (!read_range(path, section.offset, records.data(), bytes)) return;
    for (const geometry::ClusterFileMaterialSpecular& r : records) {
      if ((r.extensions & (assets::k_ext_ior | assets::k_ext_specular)) != 0 &&
          packs_specular(r.ior, r.specular, r.color)) {
        out.specular = true;
        break;
      }
    }
  }
  if (materials > 65536) return;  // past what any record section is trusted with: not counted
  out.counted = true;
  out.flags.resize(static_cast<u32>(materials), 0u);
  if (materials == 0 || sampling == nullptr ||
      sampling->element_size != sizeof(geometry::ClusterFileMaterialSampling) ||
      sampling->element_count < materials)
    return;
  const u64 bytes = materials * sizeof(geometry::ClusterFileMaterialSampling);
  if (sampling->offset > size || bytes > size - sampling->offset) return;
  Vector<geometry::ClusterFileMaterialSampling> records(static_cast<u32>(materials));
  if (!read_range(path, sampling->offset, records.data(), bytes)) return;
  for (u32 k = 0; k < records.size(); ++k)
    out.flags[k] = records[k].flags;
}

// The scene's word over a file whose material count is not known: plain materials as many as its
// entries name plus one no entry names (T150's stand-in), so no entry is refused for want of one.
void plain_stand_in(const Vector<SceneMeshInfo::MaterialSampling>& sampling, Vector<u32>& flags) {
  if (sampling.empty()) return;
  i32 highest = -1;
  for (const SceneMeshInfo::MaterialSampling& entry : sampling)
    highest = entry.material > highest ? entry.material : highest;
  flags.assign(static_cast<u32>(highest + 2), 0u);
}

// The load's rule over one mesh's materials (`apply_mesh_sampling`): any stochastic at the end.
bool samples_stochastic(const Vector<SceneMeshInfo::MaterialSampling>& sampling,
                        Vector<u32>& flags) {
  std::string ignored;
  if (!sampling.empty() && !apply_mesh_sampling(sampling, flags, ignored)) return false;  // refused
  for (const u32 f : flags)
    if ((f & geometry::k_material_sampling_stochastic) != 0) return true;
  return false;
}

// Whether the worker reads `mesh` (`mesh_files_to_read`): by suffix, and never a recipe's path
// (`handed`, sorted).
bool read_on_worker(const std::string& mesh, const Vector<std::string>& handed) {
  return mesh_file_kind(mesh) != MeshFileKind::none &&
         !std::binary_search(handed.begin(), handed.end(), mesh);
}

Vector<std::string> handed_paths(const SceneDesc& desc) {
  Vector<std::string> handed;  // the recipes' paths, sorted: never read from the disk
  for (const MeshRecipe& recipe : desc.recipes)
    handed.push_back(recipe.path);
  std::sort(handed.begin(), handed.end());
  return handed;
}

}  // namespace

u64 mesh_declaration_reads() noexcept {
  return g_mesh_declaration_reads.load(std::memory_order_relaxed);
}

bool read_mesh_declarations(const std::string& path, MeshDeclarations& out,
                            const Vector<SceneMeshInfo::MaterialSampling>& sampling) {
  const MeshFileKind kind = mesh_file_kind(path);
  if (kind == MeshFileKind::none) return false;
  io::FileInfo info;
  if (io::stat_file(path, info) != io::Status::Ok) return false;
  g_mesh_declaration_reads.fetch_add(1, std::memory_order_relaxed);
  FileFacts facts;
  switch (kind) {
    case MeshFileKind::gltf: {
      std::string text;
      if (info.size <= (u64{64} << 20) && io::read_file(path, text) == io::Status::Ok)
        read_gltf_json(text, facts);
      break;
    }
    case MeshFileKind::glb: read_glb(path, info.size, facts); break;
    case MeshFileKind::clusters: read_clusters(path, info.size, facts); break;
    case MeshFileKind::none: break;
  }
  out.specular = out.specular || facts.specular;
  // The file's own ask, then the scene's word over it in order (T150): the load's one rule.
  if (!facts.counted) plain_stand_in(sampling, facts.flags);
  out.stochastic = out.stochastic || samples_stochastic(sampling, facts.flags);
  return true;
}

Vector<MeshFileToRead> mesh_files_to_read(const SceneDesc& desc) {
  const Vector<std::string> handed = handed_paths(desc);
  Vector<MeshFileToRead> out;
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    if (!read_on_worker(desc.meshes[m], handed)) continue;
    MeshFileToRead file;
    file.path = desc.meshes[m];
    if (m < desc.mesh_info.size()) file.sampling = desc.mesh_info[m].sampling;
    out.push_back(std::move(file));
  }
  return out;
}

ResolveVariant predict_resolve_variant(const SceneDesc& desc, const ResolvedSettings& resolved,
                                       gfx::Format color_format) {
  ResolveVariant v;
  v.shadows = resolved.shadows;
  v.color_format = color_format;
  for (const SceneMeshInfo& info : desc.mesh_info) {
    if (info.water.has_value()) v.water = true;
  }
  // **The scene's `Mesh.sampling` over a mesh no file of which is read** (T150; renderer.md,
  // "Stochastic mesh materials"): plain materials as many as its entries name plus one, by the
  // load's one rule, in order. A mesh file's own word (a container's section 33, a glTF's
  // `extras.sampling`) and the scene's over it are the early compile's worker's to read
  // (`read_mesh_declarations`), so nothing here opens a file.
  const Vector<std::string> handed = handed_paths(desc);
  for (u32 m = 0; m < desc.meshes.size() && m < desc.mesh_info.size(); ++m) {
    if (desc.mesh_info[m].sampling.empty() || read_on_worker(desc.meshes[m], handed)) continue;
    Vector<u32> flags;
    plain_stand_in(desc.mesh_info[m].sampling, flags);
    if (samples_stochastic(desc.mesh_info[m].sampling, flags)) v.stochastic = true;
  }
  v.terrain = desc.terrain.enabled && desc.terrain.materials.has_value();
  // The mesh room keeps the foliage lines whatever was declared (gpu_scene.cpp, R177's review
  // finding A7): the GPU scene's own rule, asked with what the file says. Whether a stream setting
  // finds the scene paged is the load's to say: a stream setting is read as paged here.
  bool tile_meshes = false;
  for (const StreamedPlacements& entry : desc.streamed)
    tile_meshes = tile_meshes || entry.tile_meshes;
  const bool room = GpuScene::wants_mesh_room(world_is_dynamic(desc.world, !desc.streamed.empty()),
                                              resolved.stream, tile_meshes);
  if (room) v.foliage = true;
  // A declared reflectance (R234): the masked rule, the GPU scene's. The files' own words are read
  // on the early compile's worker (`EarlyPipelines::start`), never here on the load's thread.
  for (const StreamedPlacements& entry : desc.streamed)
    v.specular = v.specular || (room && entry.tile_meshes_specular);
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

void EarlyPipelines::wait_for_reads(std::unique_lock<std::mutex>& lock) const {
  read_cv_.wait(lock, [this] { return !stats_.started || stats_.read; });
}

bool EarlyPipelines::start(const gfx::Device& device, const ResolveVariant& variant,
                           const std::string& shader_manifest, std::string* error,
                           Vector<MeshFileToRead> mesh_files) {
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
  // The scene's own shape (`GpuScene::bindless_config`), and the key carries what the device
  // clamped it to, so the pipeline is only ever taken by a renderer whose set matches it.
  if (!bindless_.create(device, GpuScene::bindless_config(), &why)) return fail(why);
  // The module is the worker's to take, after its reads: a mesh file's stochastic request (T150)
  // picks it, and stochastic sampling is a module, not a specialization.
  stats_.key.color_format = variant.color_format;
  stats_.key.foliage = variant.foliage;
  stats_.key.light_cap = variant.light_cap;
  stats_.key.specular = variant.specular;
  stats_.key.layout = bindless_.capacity();
  stats_.mesh_files = mesh_files.size();
  stats_.started = true;
  ENGINE_LOG_INFO(log_renderer, "early pipeline started",
                  log::field("format", gfx::format_name(variant.color_format)),
                  log::field("foliage", variant.foliage), log::field("specular", variant.specular),
                  log::field("stochastic", variant.stochastic),
                  log::field("mesh_files", mesh_files.size()));
  // The worker owns nothing: it reads the files it was handed, takes the module their facts pick,
  // uses the layout made above, and writes the key's last facts and the handle under the lock.
  // Creating a pipeline on another thread is the device's normal use (the pipeline cache is not
  // externally synchronized; pipeline_batch.h); the library is the worker's until `take` or
  // `destroy` joins it.
  worker_ = std::thread([this, files = std::move(mesh_files), predicted = variant] {
    // **The mesh files' declarations, here and not on the load's thread** (renderer.md, "The
    // resolve compiles from the scene file"): one read a file, in the scene's order, until every
    // fact the key takes from them is known. A fact added to `MeshDeclarations` adds its term to
    // `known` and its field to the key below; the loop stays the one pass.
    MeshDeclarations declared;
    declared.specular = predicted.specular;
    declared.stochastic = predicted.stochastic;
    const auto known = [](const MeshDeclarations& d) { return d.specular && d.stochastic; };
    u32 opened = 0;
    {
      ENGINE_STARTUP_STAGE("early resolve mesh reads");
      for (const MeshFileToRead& file : files) {
        if (known(declared)) break;
        opened += read_mesh_declarations(file.path, declared, file.sampling) ? 1u : 0u;
      }
      profiling::trace_items(opened);
    }
    std::string missing;
    const gfx::Shader* module =
        shaders_.get(resolve_module_name(predicted.shadows, predicted.water, predicted.terrain,
                                         declared.stochastic),
                     &missing);
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      module_ = module;
      if (module != nullptr) {
        stats_.key.module = module->name;
        stats_.key.hash = module->hash;
      } else {
        // No module, no compile: the renderer makes its own, and `take` says why.
        error_ = missing;
        stats_.reason = "the early compile found no module: " + missing;
        stats_.done = true;
      }
      stats_.key.specular = declared.specular;
      stats_.mesh_files_read = opened;
      stats_.read = true;
    }
    read_cv_.notify_all();
    ENGINE_LOG_INFO(log_renderer, "early pipeline read its mesh files",
                    log::field("files", files.size()), log::field("read", opened),
                    log::field("specular", declared.specular),
                    log::field("stochastic", declared.stochastic),
                    log::field("shader", module != nullptr ? std::string_view(module->name)
                                                           : std::string_view()));
    if (module == nullptr) return;
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = desc.fragment = module_->module;
    desc.vertex_entry = "vs_fullscreen";
    desc.fragment_entry = "fs_resolve";
    desc.layout = bindless_.pipeline_layout();
    desc.color_format = stats_.key.color_format;
    desc.specialization =
        resolve_specialization(stats_.key.foliage, stats_.key.light_cap, declared.specular);
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
  std::unique_lock<std::mutex> lock(mutex_);
  wait_for_reads(lock);
  return stats_.started && !stats_.taken && stats_.key == key;
}

bool EarlyPipelines::take(const ResolveKey& key, gfx::PipelineHandle& out) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_reads(lock);
    // Nothing started, or taken already (a shader reload's re-create): not a wrong guess.
    if (!stats_.started || stats_.taken) return false;
    // The worker found no module for what the files said (`reason` says why): nothing to take,
    // and not a wrong guess.
    if (stats_.key.module.empty()) return false;
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
      if (key.specular != stats_.key.specular) add("specular");
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
  // Neither argument touches a file: the mesh files are read on the worker, after `start`'s
  // refusals, so a refused start reads none of them, and the load's thread reads none at all.
  if (!early.start(device, predict_resolve_variant(desc, resolved, color_format), shader_manifest,
                   &why, mesh_files_to_read(desc))) {
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
