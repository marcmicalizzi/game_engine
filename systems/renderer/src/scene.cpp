#include "scene_flora.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/image_identities.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/flora_species.h>
#include <domain/geometry/stress_mesh.h>
#include <domain/materials/materials.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_materials.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <renderer_log.h>
#include <scene_fire.h>
#include <scene_water.h>
#include <schemas/scene.h>
#include <utility>

namespace engine::renderer {

namespace {

void prune_form_parts(SourceMesh& form);  // below, beside the forms that take it first

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a valid scene") : text;
}

// splitmix64 to a float in [0, 1): the scatters' generator. Seeded per scatter, so adding a
// scatter to a file moves nothing another one placed.
f32 seeded_unit(u64& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<f32>(z >> 40) / static_cast<f32>(1u << 24);
}

// An (n x n) heightfield over [-extent, extent]^2 in XZ, dunes-and-ridges in Y. The scene that
// needs no content at all: it is what a bring-up on a new machine draws, and what the
// renderer's own tests fall back to when there is no fixture.
void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.reserve(n * n);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f) +
                    0.12f * std::cos(fx * 4.3f - fz * 3.7f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  indices.reserve((n - 1) * (n - 1) * 6);
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

// ---- The load's textures (renderer.md, "What the load reads"; roadmap W75) ---------------------
//
// One pass for the whole load, after every mesh is in, where until 2026-10-24 each mesh ran the
// content build's texture step on its own — reading and hashing every image its materials name to
// find the `.tex` the image's bytes address, and making and joining a job pool to do it — so a
// forest whose 42 trees share a pack's bark and leaves read the pack 42 times, and the GPU upload
// read and hashed every one again (2.7 GB of the v7 forest's 4.4 GB read before ready). Now:
//
//   1. Every (mesh, image) a material samples, in mesh order. An image the container carries has
//      the key its record says (the container's identity covers those bytes); one named by path is
//      found by its file, **one identity per distinct file for the whole load**, through the load's
//      table (`content_build::ImageIdentities`): a material pack's file from the pack's index under
//      the derived-data root without a read, any other file read and hashed once. The distinct
//      files are identified on the job system, each its own job.
//   2. Every distinct key's `.tex` is **trusted from its header** — it exists, it is the size its
//      header says, and its recorded identity is this key's (`texture::probe_texture_file`): a few
//      hundred bytes of derived data, not the texture and not its source.
//   3. Only a key whose `.tex` is missing or refused is built, all of them in one run of the
//      content build's step over one pool, from the bytes the container carries or the file read
//      now — and a pack file whose bytes no longer give its index's hash is built under the key
//      its bytes give.
//   4. The keys go back into each mesh's texture records, so the GPU upload finds every built
//      texture by key and never hashes a source again (`find_built_textures`, gpu_scene.cpp).
//
// Without a derived-data root nothing is built or looked up, as before, and the upload decodes.

struct TextureRef {
  u32 mesh = 0;
  u32 image = 0;
  u32 file = ~0u;  // into the distinct files; ~0u for an image the container carries
  u32 options = 0;
  u64 source_hash = 0;
  u64 key = 0;
};

struct FileWork {
  std::string path;
  content_build::ImageIdentities* identities = nullptr;
  u64 hash = 0;
  u64 size = 0;
  bool ok = false;
  std::string error;
};

void identify_file(void* data) {
  FileWork& work = *static_cast<FileWork*>(data);
  work.ok = work.identities->identify(work.path, work.hash, work.size, work.error);
}

struct KeyWork {
  u64 source_hash = 0;
  u64 key = 0;
  u32 first_ref = 0;  // the reference whose bytes build it when it has to be built
  std::string path;   // its `.tex`
  bool trusted = false;
  u64 bytes = 0;
  std::string why;
};

void probe_key(void* data) {
  KeyWork& work = *static_cast<KeyWork*>(data);
  u64 recorded_hash = 0;
  u64 recorded_key = 0;
  if (!texture::probe_texture_file(work.path, recorded_hash, recorded_key, work.bytes, &work.why))
    return;
  work.trusted = recorded_hash == work.source_hash && recorded_key == work.key;
  if (!work.trusted) work.why = "built from other bytes or options";
}

// One job of `fn` per item on the performance pool, the calling thread helping; returns when all
// are done.
template <typename T>
void run_jobs(jobs::JobSystem& pool, Vector<T>& items, void (*fn)(void*)) {
  if (items.empty()) return;
  Vector<jobs::Job> list;
  list.reserve(items.size());
  jobs::Counter counter;
  for (T& item : items)
    list.push_back(jobs::Job{fn, &item, &counter});
  pool.schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                counter);
  pool.wait(counter);
}

void prepare_scene_textures(const SceneDesc& desc, Vector<SourceMesh>& sources,
                            content_build::ImageIdentities& identities, jobs::JobSystem& pool,
                            SceneLoadReport& report) {
  if (!desc.cache || desc.ddc.empty()) return;
  const i64 start_ns = time::monotonic_ns();
  ENGINE_STARTUP_STAGE("cache textures");
  // 1. The references, and the distinct files among them.
  Vector<TextureRef> refs;
  Vector<FileWork> files;
  HashMap<u64, u32> file_of;
  for (u32 m = 0; m < sources.size(); ++m) {
    const SourceMesh& mesh = sources[m];
    if (mesh.textures.size() != mesh.data.images.size()) continue;
    for (u32 i = 0; i < mesh.textures.size(); ++i) {
      const geometry::ClusterFileTexture& record = mesh.textures[i];
      texture::TextureBuildOptions options;
      if (record.options == 0 || !texture::unpack_texture_options(record.options, options))
        continue;
      TextureRef ref;
      ref.mesh = m;
      ref.image = i;
      ref.options = record.options;
      const assets::ImageRef& image = mesh.data.images[i];
      if (!image.bytes.empty()) {
        ref.source_hash = record.key != 0 ? record.source_hash
                                          : hash_bytes(image.bytes.data(), image.bytes.size());
        // A cached mesh can outlive the texture builder version. Its embedded source identity
        // stays valid, but reusing its old derived key would prevent new texture sections from
        // ever being built (R97's terrain normal variance). The record is updated below, in memory.
        ref.key = texture::texture_cache_key(ref.source_hash, options);
      } else if (!image.uri.empty()) {
        std::string path = io::normalize_path(
            mesh.image_dir.empty() ? image.uri : io::join_path(mesh.image_dir, image.uri));
        const u64 path_hash = hash_bytes(path.data(), path.size());
        const u32* found = file_of.find_value(path_hash);
        if (found != nullptr && files[*found].path == path) {
          ref.file = *found;
        } else {
          ref.file = files.size();
          if (found == nullptr) file_of.try_emplace(path_hash, ref.file);
          FileWork work;
          work.path = std::move(path);
          work.identities = &identities;
          files.push_back(std::move(work));
        }
      } else {
        continue;  // neither bytes nor a path: the upload says so
      }
      refs.push_back(ref);
    }
  }
  report.references = refs.size();
  {
    ENGINE_STARTUP_STAGE("texture identities");
    profiling::trace_items(files.size());
    run_jobs(pool, files, identify_file);
  }
  for (TextureRef& ref : refs) {
    if (ref.file == ~0u) continue;
    const FileWork& file = files[ref.file];
    if (!file.ok) continue;
    texture::TextureBuildOptions options;
    (void)texture::unpack_texture_options(ref.options, options);
    ref.source_hash = file.hash;
    ref.key = texture::texture_cache_key(file.hash, options);
  }
  // 2. The distinct keys, each `.tex` trusted from its header.
  Vector<KeyWork> keys;
  HashMap<u64, u32> key_of;
  Vector<u32> key_of_ref(refs.size(), ~0u);
  for (u32 r = 0; r < refs.size(); ++r) {
    const TextureRef& ref = refs[r];
    if (ref.key == 0) continue;
    if (const u32* found = key_of.find_value(ref.key);
        found != nullptr && keys[*found].source_hash == ref.source_hash) {
      key_of_ref[r] = *found;
      continue;
    }
    key_of_ref[r] = keys.size();
    key_of.try_emplace(ref.key, keys.size());
    KeyWork work;
    work.source_hash = ref.source_hash;
    work.key = ref.key;
    work.first_ref = r;
    work.path = texture::texture_cache_path(desc.ddc, ref.key);
    keys.push_back(std::move(work));
  }
  report.textures = keys.size();
  {
    ENGINE_STARTUP_STAGE("texture probe");
    profiling::trace_items(keys.size());
    run_jobs(pool, keys, probe_key);
  }
  // 3. What has to be built, from the bytes it was keyed by, in one run of the step.
  Vector<content_build::TextureSource> build;
  Vector<u32> build_key;  // the key each build is for
  for (u32 k = 0; k < keys.size(); ++k) {
    KeyWork& work = keys[k];
    if (work.trusted) {
      ++report.trusted;
      continue;
    }
    const TextureRef& ref = refs[work.first_ref];
    content_build::TextureSource source;
    source.image = ref.image;
    source.options = ref.options;
    if (ref.file == ~0u) {
      const Vector<u8>& bytes = sources[ref.mesh].data.images[ref.image].bytes;
      source.view = std::span<const u8>(bytes.data(), bytes.size());
      source.source_hash = ref.source_hash;
    } else {
      std::string bytes;
      u64 hash = 0;
      std::string why;
      if (!identities.read(files[ref.file].path, bytes, hash, why)) {
        ENGINE_LOG_WARN(log_renderer, "texture not built",
                        log::field("image", files[ref.file].path), log::field("error", why));
        ++report.unbuilt;
        continue;
      }
      if (hash != work.source_hash) {
        // The file is not the bytes its identity said (a pack edited without its manifest): every
        // reference to it takes the key its bytes give, and that is what is built.
        texture::TextureBuildOptions options;
        for (u32 r = 0; r < refs.size(); ++r) {
          TextureRef& other = refs[r];
          if (other.file != ref.file) continue;
          (void)texture::unpack_texture_options(other.options, options);
          other.source_hash = hash;
          other.key = texture::texture_cache_key(hash, options);
        }
      }
      source.bytes.assign(reinterpret_cast<const u8*>(bytes.data()),
                          reinterpret_cast<const u8*>(bytes.data()) + bytes.size());
      source.source_hash = hash;
    }
    build.push_back(std::move(source));
    build_key.push_back(k);
  }
  if (!build.empty()) {
    ENGINE_STARTUP_STAGE("texture build");
    profiling::trace_items(build.size());
    Vector<const content_build::TextureSource*> pointers;
    Vector<u32> mesh_of;
    for (u32 b = 0; b < build.size(); ++b) {
      pointers.push_back(&build[b]);
      mesh_of.push_back(refs[keys[build_key[b]].first_ref].mesh);
    }
    Vector<content_build::TextureResult> results;
    content_build::run_texture_step(
        std::span<const content_build::TextureSource* const>(pointers.data(), pointers.size()),
        std::span<const u32>(mesh_of.data(), mesh_of.size()), desc.ddc, pool, results);
    for (const content_build::TextureResult& result : results) {
      switch (result.state) {
        case content_build::TextureState::Built: ++report.built; break;
        case content_build::TextureState::Skipped:
        case content_build::TextureState::Shared: ++report.trusted; break;
        case content_build::TextureState::Missing:
        case content_build::TextureState::Failed:
        case content_build::TextureState::Unreadable:
          ++report.unbuilt;
          ENGINE_LOG_WARN(log_renderer, "texture not built", log::field("image", result.image),
                          log::field("state", content_build::texture_state_name(result.state)),
                          log::field("error", result.error));
          break;
      }
    }
  }
  // 4. Every reference's key into its mesh's records, which the upload reads.
  for (const TextureRef& ref : refs) {
    if (ref.key == 0) continue;
    geometry::ClusterFileTexture& record = sources[ref.mesh].textures[ref.image];
    record.source_hash = ref.source_hash;
    record.key = ref.key;
  }
  identities.save();
  const content_build::ImageIdentities::Counts counts = identities.counts();
  report.images_from_index = counts.from_index;
  report.images_hashed = counts.hashed;
  report.source_image_reads = counts.reads;
  report.source_image_bytes = counts.read_bytes;
  report.textures_ms = static_cast<f64>(time::monotonic_ns() - start_ns) / 1.0e6;
  char detail[96];
  std::snprintf(detail, sizeof(detail), "trusted=%u built=%u hashed=%u from_index=%u",
                report.trusted, report.built, report.images_hashed, report.images_from_index);
  profiling::trace_end(profiling::trace_begin("textures trusted", detail));
  ENGINE_LOG_INFO(
      log_renderer, "scene textures checked", log::field("references", report.references),
      log::field("textures", report.textures), log::field("trusted", report.trusted),
      log::field("built", report.built), log::field("unbuilt", report.unbuilt),
      log::field("from_index", report.images_from_index),
      log::field("hashed", report.images_hashed),
      log::field("source_reads", report.source_image_reads),
      log::field("source_bytes", report.source_image_bytes), log::field("ms", report.textures_ms));
}

// Writes a mesh that was just imported and clustered into the derived-data cache, byte for byte
// the container `engine-content build` writes from the same source: the DAG, the materials as
// GPU records, a material index per cluster (a primitive that names none gets one appended
// default, shared), the image paths as the glTF gave them, and the source path they are
// relative to, plus the streaming page table the clusters were laid out in. `lod` and `pages`
// are moved into the container and back out again, so neither is ever copied. A cache that
// cannot be written is a warning and nothing more: the picture does not depend on it.
// True when the entry landed, which is what tells the caller its pages may be read back out of it.
// The textures its records name are built by the load's texture pass once every mesh is in
// (`prepare_scene_textures`), landed or not, because that pass reads the records this function
// hands back, not the file.
bool write_cluster_cache(const std::string& path, const std::string& source,
                         const assets::MeshData& mesh_data, const Vector<i32>& part_material,
                         const Vector<u32>& part_of_cluster, geometry::ClusterLodMesh& lod,
                         geometry::ClusterPages& pages,
                         Vector<geometry::ClusterFileTexture>& textures) {
  geometry::ClusterFileData data;
  data.mesh = std::move(lod);
  data.pages = std::move(pages);
  data.source_path = source;
  data.materials.reserve(mesh_data.materials.size() + 1);
  for (const assets::Material& source_material : mesh_data.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color = source_material.base_color;
    material.metallic = source_material.metallic;
    material.roughness = source_material.roughness;
    material.base_color_image = source_material.base_color_image;
    material.normal_image = source_material.normal_image;
    material.metallic_roughness_image =
        geometry::encode_optional_image(source_material.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(source_material.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(source_material.emissive_image);
    material.emissive = source_material.emissive;
    material.normal_scale = source_material.normal_scale;
    material.alpha = geometry::encode_alpha_word(
        source_material.alpha_mode, source_material.double_sided, source_material.alpha_cutoff);
    data.materials.push_back(material);
    data.material_sampling.push_back(geometry::encode_material_sampling(
        source_material.sampling, source_material.occlusion_strength));
  }
  constexpr u32 k_no_default = ~u32{0};
  u32 default_material = k_no_default;
  data.cluster_material.reserve(part_of_cluster.size());
  for (const u32 part : part_of_cluster) {
    const i32 index = part_material[part];
    if (index >= 0 && static_cast<u32>(index) < mesh_data.materials.size()) {
      data.cluster_material.push_back(static_cast<u32>(index));
      continue;
    }
    if (default_material == k_no_default) {
      default_material = data.materials.size();
      data.materials.push_back(geometry::ClusterFileMaterial{});
      data.material_sampling.push_back(geometry::ClusterFileMaterialSampling{});
    }
    data.cluster_material.push_back(default_material);
  }
  // The image paths as the glTF gave them, and beside them the bytes of the images it embedded,
  // so that what this container draws is what the glTF draws. Writing only the (empty) paths is
  // what made a cache hit a different picture from a cache miss ([geometry](geometry.md)).
  data.image_paths.reserve(mesh_data.images.size());
  data.images.reserve(mesh_data.images.size());
  for (const assets::ImageRef& image : mesh_data.images) {
    data.image_paths.push_back(image.uri);
    geometry::ClusterImage carried;
    carried.mime_type = image.mime_type;
    carried.bytes = image.bytes;
    data.images.push_back(std::move(carried));
  }
  // Where each image's built texture is, by the same function engine-content build calls, so the
  // two apps' entries stay the same bytes; the records come back to the caller whether or not the
  // entry lands, because the texture lookup does not depend on it (texture.md).
  texture::fill_cluster_texture_records(data);

  std::string error;
  const io::Status status = io::make_directories(io::parent_path(path));
  if (status != io::Status::Ok) {
    error = std::string("cannot create the directory: ") + io::status_name(status);
  } else if (geometry::write_cluster_file(path, data, &error)) {
    ENGINE_LOG_INFO(log_renderer, "cluster cache written", log::field("path", path),
                    log::field("clusters", data.mesh.mesh.clusters.size()));
  }
  if (!error.empty()) {
    ENGINE_LOG_WARN(log_renderer, "cluster cache not written", log::field("path", path),
                    log::field("error", error));
  }
  lod = std::move(data.mesh);
  pages = std::move(data.pages);
  textures = std::move(data.textures);
  return error.empty();
}

// The page layout renumbers a mesh's clusters, so it is only ever run where the result is
// already the renumbered one — the glTF, terrain and container paths below, whose layout is part
// of the derived-data cache key — or where nothing has been built yet and the caller asked to
// stream. Paging the classic heightfield unconditionally would change every id and every cut of
// the scene the renderer's own tests draw, for a table nobody would read.
void lay_out_pages(const SceneDesc& desc, const char* what, u32 page_bytes, SourceMesh& out,
                   geometry::ClusterLodMesh& lod, geometry::ClusterPages& pages) {
  if (!desc.stream) return;
  Vector<u32> source_of_cluster;
  std::string page_error;
  geometry::ClusterPagesOptions options;
  if (page_bytes > 0) options.page_bytes = page_bytes;
  if (geometry::build_cluster_pages(lod, options, pages, &page_error, &source_of_cluster)) {
    geometry::permute_cluster_array(source_of_cluster, out.part_of_cluster);
    if (!out.lod_coverage.empty())
      geometry::permute_cluster_array(source_of_cluster, out.lod_coverage);
    return;
  }
  pages = geometry::ClusterPages{};
  ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("mesh", what),
                  log::field("error", page_error));
}

// A container's materials, images, material map and (when streaming) page table, in the shape the
// glTF path leaves behind, so that everything downstream is the same code for both: one part per
// material, and the file's own map from cluster to part. `container` is where it was read from.
void adopt_container(geometry::ClusterFileData& container_data, const std::string& container,
                     const SceneDesc& desc, SourceMesh& out, geometry::ClusterLodMesh& lod,
                     geometry::ClusterPages& pages) {
  lod = std::move(container_data.mesh);
  out.data.materials.reserve(container_data.materials.size());
  out.part_material.reserve(container_data.materials.size());
  for (u32 m = 0; m < container_data.materials.size(); ++m) {
    const geometry::ClusterFileMaterial& source = container_data.materials[m];
    assets::Material material;
    // How its slots sample their images, or the glTF defaults for a container written before
    // the records existed (geometry.md, "How a material samples its images").
    geometry::decode_material_sampling(geometry::cluster_material_sampling(container_data, m),
                                       material.sampling, material.occlusion_strength);
    material.base_color = source.base_color;
    material.metallic = source.metallic;
    material.roughness = source.roughness;
    material.base_color_image = source.base_color_image;
    material.normal_image = source.normal_image;
    material.metallic_roughness_image =
        geometry::decode_optional_image(source.metallic_roughness_image);
    material.occlusion_image = geometry::decode_optional_image(source.occlusion_image);
    material.emissive_image = geometry::decode_optional_image(source.emissive_image);
    material.emissive = source.emissive;
    material.normal_scale = source.normal_scale;
    material.alpha_mode = geometry::alpha_word_mode(source.alpha);
    material.double_sided = geometry::alpha_word_double_sided(source.alpha);
    material.alpha_cutoff = geometry::alpha_word_cutoff(source.alpha);
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(static_cast<i32>(out.part_material.size()));
  }
  // The images: a path names a file beside the source mesh, and an image the source embedded
  // travels inside the container, so everything below is the same code for both and a container
  // draws what its source draws. A container written before the bytes were carried has the
  // paths alone, and an embedded image of one still draws untextured with a warning.
  out.data.images.reserve(container_data.image_paths.size());
  for (u32 i = 0; i < container_data.image_paths.size(); ++i) {
    assets::ImageRef image;
    image.uri = container_data.image_paths[i];
    if (i < container_data.images.size()) {
      image.mime_type = std::move(container_data.images[i].mime_type);
      image.bytes = std::move(container_data.images[i].bytes);
    }
    out.data.images.push_back(std::move(image));
  }
  out.textures = std::move(container_data.textures);  // where the built textures are, if any
  // A generated tree's motion contract, for the flora stage and the collision (renderer.md, "The
  // flora stage"): kept only when its per-vertex records cover the mesh, which is what a reader may
  // index them by.
  if (!container_data.motion_parts.empty() &&
      container_data.motion_vertices.size() == lod.mesh.vertices.size()) {
    out.motion_parts = std::move(container_data.motion_parts);
    out.motion_vertices = std::move(container_data.motion_vertices);
    out.structural_parts = std::move(container_data.structural_parts);
    out.species = container_data.generator_kind;
    // **A cooked grass container carries only the blades its vertices use** (R95): a sward's card
    // container (`distance.clusters`) was cooked with its patch's whole table — 1,040 blades for
    // three clusters' 192 vertices. Grass alone, because a field's blades have no collider a
    // vertexless part could stand for; a tree's table is kept whole for the collision.
    const geometry::FloraSpeciesInfo* info = geometry::flora_species_info(out.species);
    if (info != nullptr && info->motion == geometry::FloraMotionKind::field) {
      const u32 before = out.motion_parts.size();
      prune_form_parts(out);
      if (out.motion_parts.size() < before) {
        ENGINE_LOG_INFO(log_renderer, "a grass container's blades pruned to the ones it draws",
                        log::field("before", before),
                        log::field("after", static_cast<u32>(out.motion_parts.size())));
      }
    }
  }
  out.part_of_cluster = std::move(container_data.cluster_material);
  if (container_data.lod_coverage.size() == lod.mesh.clusters.size())
    out.lod_coverage = std::move(container_data.lod_coverage);
  if (out.part_of_cluster.empty()) {  // a container with no material map: one default for all
    out.part_of_cluster = Vector<u32>(lod.mesh.clusters.size(), out.part_material.size());
    out.part_material.push_back(-1);
  }
  // Relative image paths belong to the mesh the container was built from; a container that
  // does not name one resolves them beside itself.
  out.image_dir = std::string(io::parent_path(container_data.source_path.empty()
                                                  ? std::string_view(container)
                                                  : std::string_view(container_data.source_path)));
  out.primitives = 0;         // a container does not record how many were merged into it
  out.container = container;  // where its pages can be read from by range
  // The container's own page table, which is the one its clusters were renumbered into. A
  // container written with `--page-bytes 0` carries none, and the layout is run here instead so
  // that a streamed run never depends on which build wrote the cache entry.
  if (desc.stream) {
    pages = std::move(container_data.pages);
    if (pages.pages.empty()) {
      lay_out_pages(desc, container.c_str(), 0, out, lod, pages);
      // The layout renumbers the vertices, and the motion records are the container's order.
      out.motion_parts.clear();
      out.motion_vertices.clear();
      out.structural_parts.clear();
    }
  }
  ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("path", container),
                  log::field("from", "cluster file"),
                  log::field("source", container_data.source_path), log::field("cache", out.cache),
                  log::field("clusters", lod.mesh.clusters.size()),
                  log::field("vertices", lod.mesh.vertices.size()),
                  log::field("triangles", lod.leaf_triangle_count),
                  log::field("lod_levels", lod.level_cluster_counts.size()),
                  log::field("materials", out.data.materials.size()),
                  log::field("images", out.data.images.size()));
}

// The scene file's terrain (terrain.h): read from the derived-data cache when an entry for these
// fields and these LOD options exists, built and written there otherwise — so a 2049 x 2049
// terrain costs its clustering once per machine, like a glTF. It is always laid out in pages,
// exactly as a glTF's cache entry is, because the layout renumbers the clusters and a cache hit
// has to be the same scene as the build that wrote it.
//
// Its materials are the desert's four — sand, ridge rock, basin sand and the basin's floor —
// chosen per cluster from the features under the cluster's centre, and they are ordinary file
// materials: the terrain is a mesh like any other from here on, which is what lets it share a
// scene with glTF meshes (the classic heightfield's height-banded palette is the GPU scene's own
// and only exists when the heightfield is the whole scene).
bool load_terrain(const SceneDesc& desc, SourceMesh& out, geometry::ClusterLodMesh& lod,
                  geometry::ClusterPages& pages, std::string& error) {
  if (desc.terrain.materials.has_value() && desc.terrain.has_detail) {
    error = "terrain.materials and terrain.detail are alternative surface-detail paths; name one";
    return false;
  }
  if (desc.terrain.materials.has_value() &&
      !validate_terrain_materials(*desc.terrain.materials, &error, desc.terrain.masks.has_value()))
    return false;
  const u64 hash = terrain_hash(desc.terrain);
  out.source_hash = hash;
  // The grid is a heightfield over the ground plane, so its levels are built never to fold over it
  // (geometry.md, "Heightfield levels never fold"); `up` is part of the cache key, so an entry
  // built before the guard is never read for it.
  geometry::ClusterLodOptions lod_options = desc.lod;
  lod_options.up = Vec3{0.0f, 1.0f, 0.0f};
  std::string cache_path;
  if (desc.cache) {
    const u64 key = geometry::cluster_cache_key(hash, lod_options, false,
                                                geometry::ClusterPagesOptions{}.page_bytes);
    cache_path = geometry::cluster_cache_path(desc.ddc, key);
    out.cache = "miss";
    if (io::exists(cache_path)) {
      geometry::ClusterFileData container_data;
      std::string read_error;
      if (geometry::read_cluster_file(cache_path, container_data, &read_error)) {
        out.cache = "hit";
        adopt_container(container_data, cache_path, desc, out, lod, pages);
        return true;
      }
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", cache_path),
                      log::field("error", read_error));
    }
  }
  Vector<Vec3> positions;
  Vector<u32> indices;
  Vector<Vec2> uvs;
  if (!build_terrain_mesh(desc.terrain, positions, indices, uvs, &error)) return false;
  // The normals are the grid's own, by central differences, and they go to the builder as an
  // attribute rather than being recomputed per level. An edge collapse keeps source vertices, so a
  // coarse cluster then shades with the fine surface's normals; recomputed from the coarse
  // triangles they made every LOD boundary a visible change of shade — a darker patch of sand the
  // shape of a cluster, a few hundred metres out, which is what the first pictures showed.
  const u32 n = desc.terrain.size;
  Vector<Vec3> normals(positions.size());
  for (u32 zi = 0; zi < n; ++zi) {
    for (u32 xi = 0; xi < n; ++xi) {
      const u32 x0 = xi > 0 ? xi - 1 : xi;
      const u32 x1 = xi + 1 < n ? xi + 1 : xi;
      const u32 z0 = zi > 0 ? zi - 1 : zi;
      const u32 z1 = zi + 1 < n ? zi + 1 : zi;
      const Vec3 dx = positions[zi * n + x1] - positions[zi * n + x0];
      const Vec3 dz = positions[z1 * n + xi] - positions[z0 * n + xi];
      normals[zi * n + xi] = normalize(cross(dz, dx));  // +y for a flat grid
    }
  }
  geometry::AttributeSource attribute_source;
  attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  attribute_source.normals = std::span<const Vec3>(normals.data(), normals.size());
  if (!geometry::build_cluster_lod(positions, indices, lod_options, lod, &error, attribute_source))
    return false;
  geometry::ClusterPages built;
  std::string page_error;
  if (!geometry::build_cluster_pages(lod, geometry::ClusterPagesOptions{}, built, &page_error)) {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("mesh", "terrain"),
                    log::field("error", page_error));
  }
  // One material for the whole ground, its colour and roughness in two maps over the terrain's
  // UVs (`bake_terrain_maps`). Every LOD level keeps source vertices and their UVs, so the colour
  // at a point of the ground is the same whichever cluster draws it; until 2026-09-25 the terrain
  // had four materials voted per cluster, and a coarse cluster's vote over a larger patch moved
  // the sand/rock/basin borders by whole clusters whenever the cut changed (terrain.h,
  // `terrain_surface`). The maps travel as PNG bytes in the mesh's images — the road a GLB's
  // embedded textures take — so the cache entry carries them and the texture step builds them
  // into mipmapped block-compressed textures beside it like any other image.
  const i64 maps_start_ns = time::monotonic_ns();
  {
    const u32 side = terrain_map_side(desc.terrain);
    Vector<u8> base_color;
    Vector<u8> metallic_roughness;
    bake_terrain_maps(desc.terrain, side, base_color, metallic_roughness);
    const char* names[2] = {"terrain-base-color", "terrain-metallic-roughness"};
    const Vector<u8>* maps[2] = {&base_color, &metallic_roughness};
    for (u32 i = 0; i < 2; ++i) {
      assets::ImageRef image;
      image.name = names[i];
      image.mime_type = "image/png";
      if (!image::encode_png(side, side, 4, std::span<const u8>(maps[i]->data(), maps[i]->size()),
                             image.bytes)) {
        error = std::string("the terrain's ") + names[i] + " map could not be encoded";
        return false;
      }
      out.data.images.push_back(std::move(image));
    }
    assets::Material material;
    material.name = "ground";
    material.base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    material.roughness = 1.0f;
    material.metallic = 0.0f;
    material.base_color_image = 0;
    material.metallic_roughness_image = 1;
    // The maps end at the grid's edge; repeating would blend the far edge's colour into the near.
    for (const u32 slot : {geometry::k_slot_base_color, geometry::k_slot_metallic_roughness}) {
      material.sampling[slot].sampler.wrap_s = geometry::TextureWrap::clamp_to_edge;
      material.sampling[slot].sampler.wrap_t = geometry::TextureWrap::clamp_to_edge;
    }
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(0);
  }
  if (!append_terrain_materials(desc.terrain, out.data, &error)) return false;
  const f64 maps_ms = static_cast<f64>(time::monotonic_ns() - maps_start_ns) / 1.0e6;
  out.part_of_cluster.assign(lod.mesh.clusters.size(), 0u);
  out.primitives = 1;
  ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("from", "terrain"),
                  log::field("size", desc.terrain.size), log::field("extent", desc.terrain.extent),
                  log::field("triangles", lod.leaf_triangle_count),
                  log::field("clusters", lod.mesh.clusters.size()),
                  log::field("lod_levels", lod.level_cluster_counts.size()),
                  log::field("maps_ms", maps_ms), log::field("fold_groups", lod.fold_guard.groups),
                  log::field("folded", lod.fold_guard.regular_folded),
                  log::field("fold_retries", lod.fold_guard.retries),
                  log::field("fold_unsimplified", lod.fold_guard.unsimplified),
                  log::field("sloppy", lod.fold_guard.sloppy),
                  log::field("sloppy_folded", lod.fold_guard.sloppy_folded));
  if (!cache_path.empty() && write_cluster_cache(cache_path, "", out.data, out.part_material,
                                                 out.part_of_cluster, lod, built, out.textures)) {
    out.container = cache_path;
  }
  if (desc.stream) pages = std::move(built);
  return true;
}

// Reads one mesh: a `.clusters` container named outright, the derived-data cache entry this
// glTF and these options address, or the glTF itself (imported, welded, clustered per primitive
// so every cluster has a single material, merged, and written into the cache for the next run).
// An empty `path` builds the scene's terrain when it has one and the procedural scene otherwise.
// `expected_hash`, when not zero, is the source hash the file must have (a scene file's `hash`).
// `identities` is the load's table of its source images, when it has one (load_scene's); its
// textures are not this function's — the load's pass builds or trusts them after every mesh is in.
bool load_source_mesh(const std::string& path, const SceneDesc& desc, u64 expected_hash,
                      SourceMesh& out, geometry::ClusterLodMesh& lod, geometry::ClusterPages& pages,
                      std::string& error, content_build::ImageIdentities* identities = nullptr) {
  auto page_layout = [&](const char* what, u32 page_bytes) {
    lay_out_pages(desc, what, page_bytes, out, lod, pages);
  };
  // Built textures live in the same derived-data root as built meshes, and a load that reads no
  // cache reads neither (texture.md, "In the renderer").
  out.texture_ddc = desc.cache ? desc.ddc : std::string();
  if (path.empty() && desc.terrain.enabled) return load_terrain(desc, out, lod, pages, error);
  if (path.empty() && desc.procedural == Procedural::shredded_atlas) {
    // The atlas stress fixture, built here rather than committed as a file so that the scene
    // corpus can guard the LOD seam defect on a fresh clone and without a third-party model
    // ([geometry](geometry.md), "The shredded atlas"). It comes with a material and a probe
    // texture whose colour encodes atlas position, because the whole point of the scene is that a
    // UV a collapse dragged into the wrong island reads the wrong colour — a picture comparison
    // of this mesh measures exactly that and nothing else. The texture travels as an encoded PNG
    // in the mesh's images, which is the same road a GLB's embedded textures take, so nothing
    // downstream needs to know the mesh was procedural.
    geometry::ShreddedAtlasMesh fixture;
    geometry::ShreddedAtlasOptions fixture_options;
    // Denser than the geometry module's own unit fixture on purpose: a picture comparison needs a
    // DAG deep enough that a coarse threshold reaches a *much* smaller cut than the finest, and
    // 192 x 96 quads (36,864 triangles, 1,152 islands) is where that happens while the scene still
    // loads in well under a second.
    fixture_options.segments = 192;
    fixture_options.rings = 96;
    geometry::build_shredded_atlas_torus(fixture_options, fixture);
    geometry::AttributeSource attribute_source;
    attribute_source.normals =
        std::span<const Vec3>(fixture.normals.data(), fixture.normals.size());
    attribute_source.uvs = std::span<const Vec2>(fixture.uvs.data(), fixture.uvs.size());
    if (!geometry::build_cluster_lod(fixture.positions, fixture.indices, desc.lod, lod, &error,
                                     attribute_source)) {
      return false;
    }
    Vector<u8> texels;
    geometry::build_atlas_probe_texture(1024, fixture.atlas_cells, texels);
    assets::ImageRef probe;
    probe.name = "atlas-probe";
    probe.mime_type = "image/png";
    if (!image::encode_png(1024, 1024, 4, std::span<const u8>(texels.data(), texels.size()),
                           probe.bytes)) {
      error = "the atlas probe texture could not be encoded";
      return false;
    }
    out.data.images.push_back(std::move(probe));
    assets::Material material;
    material.name = "atlas-probe";
    material.base_color_image = 0;
    material.metallic = 0.0f;
    material.roughness = 0.6f;
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(0);
    out.part_of_cluster.resize(lod.mesh.clusters.size(), 0u);
    out.primitives = 1;
    ENGINE_LOG_INFO(log_renderer, "mesh loaded", log::field("from", "shredded atlas"),
                    log::field("islands", fixture.island_count),
                    log::field("atlas_cells", fixture.atlas_cells),
                    log::field("seam_vertices", fixture.seam_vertices),
                    log::field("triangles", lod.leaf_triangle_count),
                    log::field("clusters", lod.mesh.clusters.size()),
                    log::field("lod_levels", lod.level_cluster_counts.size()));
    page_layout("shredded-atlas", desc.page_bytes);
    return true;
  }
  if (path.empty()) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(desc.heightfield_grid, 10.0f, positions, indices);
    Vector<Vec2> uvs;
    uvs.reserve(positions.size());
    for (const Vec3& p : positions)
      uvs.push_back(Vec2{(p.x + 10.0f) / 20.0f, (p.z + 10.0f) / 20.0f});
    geometry::AttributeSource attribute_source;
    attribute_source.uvs = std::span<const Vec2>(uvs.data(), uvs.size());  // normals: computed
    if (!geometry::build_cluster_lod(positions, indices, desc.lod, lod, &error, attribute_source)) {
      return false;
    }
    page_layout("heightfield", desc.page_bytes);
    return true;
  }
  // A mesh a generator makes (`<name>.flora.json`): built into its container through the cache by
  // the generator its name gives, then read as any container is (scene_flora.cpp).
  if (is_generated_mesh(path)) {
    geometry::ClusterFileData generated;
    std::string where;
    if (!load_generated_mesh(path, desc, expected_hash, generated, where, out.cache,
                             out.source_hash, error, identities)) {
      return false;
    }
    // The images its parts sample — a recipe's sets, a material pack's — are the load's texture
    // pass's, as a glTF's are (`prepare_scene_textures`; renderer.md, "Built textures").
    adopt_container(generated, where, desc, out, lod, pages);
    return true;
  }
  // Where a built mesh may already be: a container named outright, or the cache entry this
  // source and these options address.
  std::string container;
  std::string cache_path;
  if (io::extension(path) == ".clusters") {
    container = path;
    out.cache = "file";
  } else if (desc.cache || expected_hash != 0) {
    u64 source_hash = 0;
    profiling::TraceStage hash_stage("source hash");
    if (!assets::source_mesh_hash(path, source_hash, &error)) return false;
    hash_stage.end();
    out.source_hash = source_hash;
    // A scene that names a mesh by its bytes gets those bytes or nothing: a number measured on a
    // different file would still carry this scene's name (plan 09 §9.4, "content-addressed").
    if (expected_hash != 0 && source_hash != expected_hash) {
      error = path + " hashes to " + hash_hex(source_hash) + ", and the scene names " +
              hash_hex(expected_hash);
      return false;
    }
    if (desc.cache) {
      // The page target is part of the key, so this has to be the one the build below uses —
      // and the one engine-content uses by default, or the two apps would stop sharing entries.
      const u64 key = geometry::cluster_cache_key(source_hash, desc.lod, true,
                                                  geometry::ClusterPagesOptions{}.page_bytes);
      cache_path = geometry::cluster_cache_path(desc.ddc, key);
      out.cache = "miss";
      if (io::exists(cache_path)) container = cache_path;
    }
  }
  geometry::ClusterFileData container_data;
  bool from_container = false;
  if (!container.empty()) {
    std::string read_error;
    profiling::TraceStage read_stage("cluster read");
    const bool read = geometry::read_cluster_file(container, container_data, &read_error);
    read_stage.end();
    if (read) {
      from_container = true;
      if (!cache_path.empty()) out.cache = "hit";
    } else if (cache_path.empty()) {
      error = read_error;  // a file named on the command line must load
      return false;
    } else {
      // A cache entry this build cannot read is not an error; it is a miss with a warning.
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", container),
                      log::field("error", read_error));
      profiling::trace_end(profiling::trace_begin("cache entry ignored", read_error));
    }
  }
  if (from_container) {
    // A container found in or named beside the cache may have been written before its textures
    // were built (an older engine-view, a cache that lost them, `engine-content build` without
    // `--cache`): the load's texture pass builds whatever of them is missing and trusts the rest.
    adopt_container(container_data, container, desc, out, lod, pages);
    return true;
  }
  ENGINE_STARTUP_STAGE("mesh build");  // a miss: the import, the repairs, the DAG, the write
  if (!assets::load_gltf(path, out.data, &error)) return false;
  out.image_dir = std::string(io::parent_path(path));
  // The two repairs, exactly where `engine-content build` runs them — after the load, before the
  // weld, the zero-area repair first — because the two write and read the same cache entries and
  // have to build the same bytes ([geometry](geometry.md), "Zero-area triangles: the repair" and
  // "UV-degenerate triangles: the repair"). A mesh a repair refuses (primitives out of order or an
  // index out of range, which the importer never produces) is drawn unrepaired, as before; and
  // where engine-content refuses what the zero-area repair does not cover (a repeated index, a
  // primitive with no area at all), this path draws it, as it always did.
  geometry::ZeroAreaRepairReport zero_area_repair;
  std::string repair_error;
  if (!assets::repair_zero_area_triangles(out.data, zero_area_repair, &repair_error)) {
    ENGINE_LOG_WARN(log_renderer, "zero-area repair refused", log::field("path", path),
                    log::field("error", repair_error));
  } else if (zero_area_repair.triangles != 0) {
    ENGINE_LOG_INFO(log_renderer, "zero-area repair", log::field("path", path),
                    log::field("rule", "geometry.zero_area"),
                    log::field("triangles", zero_area_repair.triangles),
                    log::field("collinear", zero_area_repair.collinear),
                    log::field("coincident", zero_area_repair.coincident),
                    log::field("dropped", zero_area_repair.dropped),
                    log::field("unrepaired", zero_area_repair.unrepaired));
  }
  geometry::UvRepairReport uv_repair;
  if (!assets::repair_uv_degenerate_triangles(out.data, uv_repair, &repair_error)) {
    ENGINE_LOG_WARN(log_renderer, "uv repair refused", log::field("path", path),
                    log::field("error", repair_error));
  } else if (uv_repair.triangles != 0) {
    ENGINE_LOG_INFO(
        log_renderer, "uv repair", log::field("path", path),
        log::field("rule", "geometry.uv_degenerate"), log::field("islands", uv_repair.islands),
        log::field("triangles", uv_repair.triangles), log::field("refolded", uv_repair.refolded),
        log::field("dropped", uv_repair.dropped), log::field("unrepaired", uv_repair.unrepaired));
  }
  // Exporters duplicate vertices freely; welding the identical ones gives the cluster builder
  // shared vertices to fill clusters with and the LOD builder edges to collapse. The weld is
  // `assets::weld_vertices`, the one `engine-content build` calls, because the two write and read
  // the same cache entries: it hands the weld **every per-vertex stream the mesh owns**, so two
  // duplicates that disagree on skin weights or morph deltas stay two vertices and the morph
  // channels are renumbered with the vertices rather than left in the old numbering
  // ([geometry](geometry.md), "Skinned meshes" and "Morph channels").
  const u32 loaded_vertices = out.data.positions.size();
  const u32 welded_vertices = assets::weld_vertices(out.data);
  ENGINE_LOG_INFO(
      log_renderer, "mesh loaded", log::field("path", path), log::field("from", "gltf"),
      log::field("cache", out.cache), log::field("vertices", loaded_vertices),
      log::field("welded", welded_vertices), log::field("triangles", out.data.indices.size() / 3),
      log::field("primitives", out.data.primitives.size()),
      log::field("materials", out.data.materials.size()),
      log::field("skins", out.data.skins.size()), log::field("images", out.data.images.size()));
  const geometry::AttributeSource attribute_source = assets::attribute_source(out.data);
  Vector<geometry::ClusterLodMesh> parts;
  for (const assets::Primitive& primitive : out.data.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(out.data.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(out.data.positions, range, desc.lod, part, &error,
                                     attribute_source)) {
      return false;
    }
    parts.push_back(std::move(part));
    out.part_material.push_back(primitive.material);
  }
  if (!geometry::merge_cluster_lod(parts, lod, &out.part_of_cluster, &error)) return false;
  out.primitives = parts.size();
  // Laid out in streaming pages, exactly as engine-content lays it out, because the page target
  // is part of the cache key and the entry either app writes has to be the same container. The
  // layout renumbers the clusters, so the map from cluster to primitive comes along. It runs
  // whether or not anyone will stream from the table, because the *renumbering* is what the cache
  // key promises; `pages` is only kept when a caller asked for it.
  geometry::ClusterPages built;
  Vector<u32> source_of_cluster;
  std::string page_error;
  if (geometry::build_cluster_pages(lod, geometry::ClusterPagesOptions{}, built, &page_error,
                                    &source_of_cluster)) {
    geometry::permute_cluster_array(source_of_cluster, out.part_of_cluster);
  } else {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("path", path),
                    log::field("error", page_error));
  }
  // Into the cache for the next run, as the same container engine-content build writes: either
  // app fills the cache, either app finds it.
  if (!cache_path.empty()) {
    if (write_cluster_cache(cache_path, path, out.data, out.part_material, out.part_of_cluster, lod,
                            built, out.textures)) {
      // The entry this load just wrote holds the same bytes in the same order this mesh is now in,
      // so a streamed run may read its pages back out of it rather than out of host memory.
      out.container = cache_path;
    }
  }
  if (desc.stream) pages = std::move(built);
  return true;
}

// n x n copies of mesh 0 on a grid, with a rotation and a scale that vary per copy and a
// non-uniform scale on every third, so one flag exercises the uniform and the non-uniform path,
// the cone test's precondition, and the LOD scaling all at once.
void make_instance_grid(u32 n, f32 mesh_radius, u32 joints, f32 bounds_padding,
                        Vector<SceneInstance>& out) {
  const f32 spacing = 3.0f * mesh_radius;
  const f32 center = 0.5f * static_cast<f32>(n - 1);
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const u32 k = z * n + x;
      SceneInstance instance;
      instance.mesh = 0;
      instance.joints = joints;
      instance.bounds_padding = bounds_padding;
      instance.transform.position = Vec3{(static_cast<f32>(x) - center) * spacing, 0.0f,
                                         (static_cast<f32>(z) - center) * spacing};
      instance.transform.rotation = quat_from_euler(radians(static_cast<f32>(k * 37 % 360)),
                                                    radians(static_cast<f32>(k * 11 % 25)), 0.0f);
      const f32 s = 0.6f + 0.2f * static_cast<f32>(k % 4);
      instance.transform.scale =
          k % 3 == 2 ? Vec3{s, s * 1.4f, s * 0.7f} : Vec3{s, s, s};  // every third non-uniform
      out.push_back(instance);
    }
  }
}

// A scene's placement entries, in the order the reader walks them (scene_gen.md, "The order
// rule"): the `ruins` entries first — each the placement generator "ruins" with the entry itself as
// its parameters, so every file written before the generic list reads as it did — then
// `placements`, each as it names itself.
struct PlacementRef {
  std::string generator;
  JsonValue params;
  std::string where;
  // In a streamed world, whether the world streams it (`PlacementEntry::stream`); a `ruins` entry
  // always is.
  bool stream = true;
};

// The job system's performance pool, made the first time a generator asks for it: a read of a few
// tiles never starts one, and a read of many shares one, as the load's meshes and textures share
// one.
struct LazyPool {
  std::optional<jobs::JobSystem> pool;
  static jobs::JobSystem* get(void* context) {
    auto* self = static_cast<LazyPool*>(context);
    if (!self->pool.has_value()) self->pool.emplace(content_build::job_config(0));
    return &*self->pool;
  }
};

// Appends a generator's meshes after everything the scene has so far, each once: a mesh an earlier
// entry appended — the same file under the same name, two entries of one kit — is that one, which
// is what reading each kit once did. Returns each of the generator's meshes as a scene mesh.
struct MeshKey {
  std::string path;
  std::string name;
  u64 hash = 0;
};
Vector<u32> append_placement_meshes(std::span<const scene_gen::PlacementMesh> meshes,
                                    Vector<std::pair<MeshKey, u32>>& seen, SceneDesc& out) {
  Vector<u32> index;
  index.reserve(static_cast<u32>(meshes.size()));
  for (const scene_gen::PlacementMesh& mesh : meshes) {
    u32 found = ~0u;
    for (const auto& [key, scene_mesh] : seen) {
      if (key.path == mesh.path && key.name == mesh.name && key.hash == mesh.hash) {
        found = scene_mesh;
        break;
      }
    }
    if (found == ~0u) {
      found = out.meshes.size();
      out.meshes.push_back(mesh.path);
      SceneMeshInfo info;
      info.name = mesh.name;
      info.hash = mesh.hash;
      out.mesh_info.push_back(std::move(info));
      seen.push_back({MeshKey{mesh.path, mesh.name, mesh.hash}, found});
    }
    index.push_back(found);
  }
  return index;
}

// The scene's placement entries through the scene-generator registry (docs/subsystems/scene_gen.md,
// ADR-0046): each generator found by the name the entry gives, opened on the entry's parameters,
// and either expanded whole — its instances standing on the terrain's ground (the provider's
// floor), its meshes appended after the file's own and every earlier entry's, so every index the
// file used still names what it named and the terrain, pushed after this, stays the last mesh — or,
// in a streamed world, asked for the meshes it holds resident and left for the world's placements
// consumer to expand a tile at a time. The renderer sees plain instances of meshes and nothing
// else about a generator, and names none.
bool expand_placements(const scene::Scene& file, const std::string& path, const std::string& dir,
                       const TerrainSampler& ground, SceneDesc& out, std::string& error) {
  Vector<PlacementRef> entries;
  for (u32 r = 0; r < file.ruins.size(); ++r)
    entries.push_back(PlacementRef{"ruins", schema::to_json(file.ruins[r]),
                                   path + ": ruins " + std::to_string(r)});
  for (u32 p = 0; p < file.placements.size(); ++p) {
    entries.push_back(PlacementRef{file.placements[p].generator, file.placements[p].params,
                                   path + ": placements " + std::to_string(p),
                                   file.placements[p].stream});
  }
  if (entries.empty()) return true;
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  // The world block a streamed entry is opened against: the file's, or the capability's defaults.
  const scene::WorldRings world = file.world.has_value() ? *file.world : scene::WorldRings{};
  LazyPool pool;
  scene_gen::Context context;
  context.world_seed = out.terrain.enabled ? out.terrain.seed : 0;
  context.tile_size = out.world.enabled ? out.world.tile_size : 32.0f;
  context.dir = dir;
  // The host's derived-data root, which both hosts set on the description before they read the
  // file (`SceneDesc::ddc`); empty when none was named, and the generator finds one itself.
  context.ddc = out.ddc;
  if (out.terrain.enabled) context.ground = ground.provider().view();
  context.jobs_fn = &LazyPool::get;
  context.jobs_context = &pool;
  Vector<std::pair<MeshKey, u32>> seen;
  // **Earlier placements** (ADR-0046 decision 4, as built 2026-10-12; scene_gen.md): every entry
  // read whole is handed the footprints of the entries read whole before it, in the scene's order —
  // where each placement stands and how far its mesh reaches on the ground, measured from the
  // mesh as loaded (`ground_radius`), once a mesh and only when a later entry is there to read it.
  Vector<scene_gen::Footprint> earlier;
  Vector<f32> radius_of;  // by scene mesh; < 0 not measured yet
  const auto measure = [&](u32 mesh, f32& radius, std::string& why) {
    if (mesh < radius_of.size() && radius_of[mesh] >= 0.0f) {
      radius = radius_of[mesh];
      return true;
    }
    SourceMesh source;
    geometry::ClusterLodMesh lod;
    geometry::ClusterPages pages;
    const u64 hash = mesh < out.mesh_info.size() ? out.mesh_info[mesh].hash : 0;
    // A path relative to the scene is the scene directory's, as the load resolves it. A mesh that
    // cannot be read here is not this read's to refuse — the load says why, with its own sentence —
    // so its footprint is its point alone until then.
    std::string mesh_path = out.meshes[mesh];
    if (!mesh_path.empty() && !dir.empty() && !std::filesystem::path(mesh_path).is_absolute())
      mesh_path = dir + "/" + mesh_path;
    radius = 0.0f;
    if (load_source_mesh(mesh_path, out, hash, source, lod, pages, why)) {
      radius = ground_radius(lod, 0, lod.mesh.clusters.size());
    } else {
      ENGINE_LOG_WARN(log_renderer, "a placement's mesh was not measured for the entries after it",
                      log::field("mesh", mesh_path), log::field("error", why));
    }
    if (radius_of.size() <= mesh) radius_of.resize(mesh + 1, -1.0f);
    radius_of[mesh] = radius;
    return true;
  };
  for (u32 e = 0; e < entries.size(); ++e) {
    const PlacementRef& entry = entries[e];
    context.earlier = std::span<const scene_gen::Footprint>(earlier.data(), earlier.size());
    const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement(entry.generator);
    if (generator == nullptr) {
      error = entry.where + " " + registry.unknown_placement(entry.generator);
      return false;
    }
    context.where = entry.where;
    // An entry the world does not stream (`PlacementEntry::stream` false) is a whole read, opened
    // as one outside a world is: its instances are the scene's own.
    const bool world_streams = out.world.enabled && entry.stream;
    context.world = world_streams ? &world : nullptr;
    void* state = nullptr;
    if (!generator->open(entry.params, context, &state, &error)) return false;
    // Closed however this entry ends.
    struct Close {
      const scene_gen::PlacementGeneratorDesc* generator;
      void* state;
      ~Close() { generator->close(state); }
    } close{generator, state};
    if (world_streams) {
      if (generator->meshes == nullptr || generator->tile == nullptr) {
        error = entry.where + ": the placement generator \"" + entry.generator +
                "\" has no tiles, so a streamed world cannot hold it";
        return false;
      }
      Vector<scene_gen::PlacementMesh> resident;
      if (!generator->meshes(state, context, resident, &error)) return false;
      StreamedPlacements streamed;
      streamed.generator = entry.generator;
      streamed.params = entry.params;
      streamed.dir = dir;
      streamed.where = entry.where;
      streamed.meshes = append_placement_meshes(
          std::span<const scene_gen::PlacementMesh>(resident.data(), resident.size()), seen, out);
      out.streamed.push_back(std::move(streamed));
      continue;
    }
    scene_gen::Placements placed;
    if (!generator->expand(state, context, placed, &error)) return false;
    const Vector<u32> meshes = append_placement_meshes(
        std::span<const scene_gen::PlacementMesh>(placed.meshes.data(), placed.meshes.size()), seen,
        out);
    out.instances.reserve(out.instances.size() + placed.instances.size());
    for (const scene_gen::Placement& p : placed.instances) {
      if (p.mesh >= meshes.size()) {
        error = entry.where + ": the placement generator \"" + entry.generator +
                "\" placed a mesh it did not name";
        return false;
      }
      // Where the generator put it, in f64 (ADR-0053): the instance stands at its `origin`, its
      // transform the placement's rotation and scale alone, so `instance_translation` adds the
      // mesh's fit to the placement's `WorldPos` in f64 and no absolute float32 is formed.
      SceneInstance instance;
      instance.mesh = meshes[p.mesh];
      instance.transform = p.turn();
      instance.origin = p.position;
      out.instances.push_back(instance);
    }
    // What the entries after this one are handed: only when there is one to hand it to.
    if (e + 1 < entries.size()) {
      earlier.reserve(earlier.size() + placed.instances.size());
      for (const scene_gen::Placement& p : placed.instances) {
        f32 radius = 0.0f;
        std::string why;
        if (!measure(meshes[p.mesh], radius, why)) {
          error = entry.where +
                  ": a placement's mesh could not be measured for the entries after "
                  "it: " +
                  why;
          return false;
        }
        const f32 scale = std::max(std::fabs(p.scale.x), std::fabs(p.scale.z));
        earlier.push_back(scene_gen::Footprint{scene_gen::nearest_mm(p.position.x),
                                               scene_gen::nearest_mm(p.position.z),
                                               static_cast<f64>(radius * scale)});
      }
    }
    out.placed_buildings += placed.things;
    out.placed_instances += placed.instances.size();
  }
  if (out.world.enabled) {
    ENGINE_LOG_INFO(log_renderer, "placements streamed by the world", log::field("scene", path),
                    log::field("entries", entries.size()), log::field("meshes", seen.size()));
  }
  return true;
}

// A mesh's fit (`MeshFit`) from the box of its vertices — every level's, which for an edge-collapse
// DAG are a subset of the source's, so the box is the source's box: the uniform scale and the
// point that goes to the origin. The one place the load computes it, for the instances' placement
// and for the authored instances' reach.
struct FitParts {
  f32 scale = 1.0f;
  Vec3 anchor{};
};
FitParts fit_parts(const MeshFit& fit, std::span<const Vec3> vertices) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (const Vec3 p : vertices) {
    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const Vec3 size = hi - lo;
  const f32 longest = std::max(size.x, std::max(size.y, size.z));
  FitParts out;
  out.scale = fit.height > 0.0f ? (size.y > 0.0f ? fit.height / size.y : 1.0f)
                                : (longest > 0.0f ? fit.extent / longest : 1.0f);
  out.anchor =
      Vec3{0.5f * (lo.x + hi.x), fit.ground ? lo.y : 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
  return out;
}

// ---- a streamed world's own instances (renderer.md, "Authored instances per tile") -------------

// The nearest an observer can be to an origin filed on a tile of ring `r` (r >= 1), metres: the
// ring's tiles have their centres at least ring r - 1's radius away once the ring is updated (it
// bands by the centre, `sim::TierAssignment::tier_of`), an origin is at most half a diagonal from
// its tile's centre, and a quarter tile more is the camera's way between two updates — a tile
// coming in is promoted at the first update past the boundary, and until then still draws the form
// it had (8 m at 32 m tiles: 480 m/s at 60 updates a second). Zero or less: no bound.
f32 ring_floor_m(const WorldDesc& world, u32 r) noexcept {
  const f32* radius = world.ring_count > 0 ? world.radius : k_default_world_ring_radius;
  return (radius[r - 1] - 0.70710678f - 0.25f) * world.tile_size;
}

u32 world_ring_count(const WorldDesc& world) noexcept {
  return world.ring_count > 0 ? world.ring_count : k_default_world_ring_count;
}

// **A distance form carries only the parts its vertices use** (renderer.md, "A form carries only
// the parts it uses"; R95). The copy kept its source's whole table, so a three-cluster sward card
// carried its patch's 1,040 blades and, swaying in a tail, took the 2,048-part class of the pose
// table: about 480 KiB a card over the three slots. The parts its motion records name are kept,
// with their ancestors (a part's frame is its parent chain's), in their order, so parents still
// come first; the records, the parents and the structural table's ids are remapped, and each kept
// part's source id is kept for the sway's phase (`SourceMesh::motion_part_ids`).
void prune_form_parts(SourceMesh& form) {
  const u32 n = form.motion_parts.size();
  if (n == 0) return;
  Vector<u8> keep(n, u8{0});
  for (const geometry::ClusterFileMotionVertex& v : form.motion_vertices) {
    u32 p = v.part;
    for (u32 guard = 0; p < n && keep[p] == 0 && guard <= n; ++guard) {
      keep[p] = 1;
      p = form.motion_parts[p].parent;
    }
  }
  Vector<u32> remap(n, ~0u);
  Vector<geometry::ClusterFileMotionPart> parts;
  Vector<u32> ids;
  const bool had_ids = form.motion_part_ids.size() == n;
  for (u32 p = 0; p < n; ++p) {
    if (keep[p] == 0) continue;
    remap[p] = parts.size();
    geometry::ClusterFileMotionPart part = form.motion_parts[p];
    if (part.parent != geometry::k_motion_no_parent)
      part.parent = part.parent < n ? remap[part.parent] : geometry::k_motion_no_parent;
    parts.push_back(part);
    ids.push_back(had_ids ? form.motion_part_ids[p] : p);
  }
  if (form.structural_parts.size() == n) {
    Vector<geometry::ClusterFileStructuralPart> structure;
    for (u32 p = 0; p < n; ++p) {
      if (keep[p] == 0) continue;
      geometry::ClusterFileStructuralPart record = form.structural_parts[p];
      record.part = remap[p];
      structure.push_back(record);
    }
    form.structural_parts = std::move(structure);
  }
  for (geometry::ClusterFileMotionVertex& v : form.motion_vertices) {
    if (v.part < n) v.part = static_cast<u16>(remap[v.part]);
  }
  form.motion_parts = std::move(parts);
  form.motion_part_ids = std::move(ids);
}

// Hands the file's own rigid instances to the "authored" generator (authored.md) and makes each
// ring's distance form of their meshes (geometry.md, "Distance forms"): appended after every mesh
// the scene has, each a mesh of its own with its source's fit, materials and grid. Nothing happens
// unless the world streams, the rules allow it, the registry has the generator and the scene is not
// paged; what stays in the prefix is counted by why.
bool stream_authored(SceneDesc& resolved, Vector<SourceMesh>& sources,
                     Vector<geometry::ClusterLodMesh>& dags, bool paged, SceneData& out,
                     std::string& error) {
  AuthoredStats& stats = out.authored;
  const AuthoredStreaming& rules = resolved.authored;
  const u32 first = resolved.authored_first;
  const u32 count = resolved.authored_count;
  out.loaded_of_read.clear();  // a scene read whole: the whole read's index is the loaded one
  if (!resolved.world.enabled || !rules.enabled || count == 0) return true;
  if (scene_gen::GeneratorRegistry::global().find_placement("authored") == nullptr) return true;
  if (resolved.mesh_info.size() != resolved.meshes.size() ||
      u64{first} + count > resolved.instances.size()) {
    return true;
  }
  if (paged) {
    stats.kept_paged = count;
    return true;
  }
  const u32 meshes = resolved.meshes.size();
  const f32 tile = resolved.world.tile_size;
  const f32 max_reach = rules.max_reach_m > 0.0f ? rules.max_reach_m : tile;
  // Per mesh, measured on first use: the fit, and how far the fitted mesh reaches from its origin.
  Vector<FitParts> fit(meshes);
  Vector<f32> reach(meshes, -1.0f);
  Vector<f32> scale_max(meshes, 0.0f);
  Vector<u8> take(count, 0);
  // **An actor is never streamed** (renderer.md, "An actor is never streamed"): what a host names
  // in `keep` stays in the prefix, so the game finds its entity, its joints and its instance row
  // on every frame, wherever the ring is.
  Vector<u8> actor(count, 0);
  for (const u32 k : rules.keep) {
    if (k >= first && k - first < count) actor[k - first] = 1;
  }
  u32 taken = 0;
  for (u32 i = 0; i < count; ++i) {
    const SceneInstance& instance = resolved.instances[first + i];
    const u32 m = instance.mesh;
    const geometry::ClusterMesh& mesh = dags[m].mesh;
    if (actor[i] != 0) {
      ++stats.kept_actors;
      continue;
    }
    if (instance.animation.play || instance.joints > 0 || !mesh.skin.empty() ||
        !mesh.morph_channels.empty() || resolved.meshes[m].empty()) {
      ++stats.kept_deformed;
      continue;
    }
    if (reach[m] < 0.0f) {
      const MeshFit& mf = resolved.mesh_info[m].fit;
      if (mf.active())
        fit[m] = fit_parts(mf, std::span<const Vec3>(mesh.vertices.data(), mesh.vertices.size()));
      f32 most = 0.0f;
      for (const geometry::ClusterDesc& c : mesh.clusters)
        most = std::max(most, length(c.center - fit[m].anchor) + c.radius);
      reach[m] = most * fit[m].scale;
    }
    const Vec3 s = instance.transform.scale;
    const f32 scale = std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
    if (reach[m] * scale > max_reach) {
      ++stats.kept_reach;
      continue;
    }
    take[i] = 1;
    ++taken;
    scale_max[m] = std::max(scale_max[m], scale);
  }
  if (taken == 0) return true;

  // Each used mesh's form set: the scene mesh each ring draws, the full mesh innermost.
  const u32 rings = world_ring_count(resolved.world);
  const f32 proj = rules.proj_scale > 0.0f ? rules.proj_scale : k_default_form_proj_scale;
  out.form_source.resize(meshes);
  for (u32 m = 0; m < meshes; ++m)
    out.form_source[m] = m;
  out.form_rigid.assign(meshes, 0);
  // **A swaying form is cut for the sway's padded spheres** (renderer.md, "Streamed trees sway"):
  // the cull pads a swaying instance's spheres by `flora_bounds_padding`, which makes its cut
  // finer, so a form that sways is cut from that much nearer, and the clusters the cull can choose
  // over it from its ring are the full mesh's swaying. Measured in the mesh's own units, as the cut
  // is.
  const f32 sway_m = rules.sway_m >= 0.0f                             ? rules.sway_m
                     : flora_motion_tunable() && flora_tail_tunable() ? flora_sway_m_tunable()
                                                                      : 0.0f;
  Vector<f32> sway_padding(meshes, -1.0f);  // -1: the mesh does not sway
  if (sway_m > 0.0f) {
    for (u32 m = 0; m < meshes; ++m) {
      const SourceMesh& source = sources[m];
      if (!(scale_max[m] > 0.0f) || !source.has_motion() ||
          source.motion_vertices.size() != dags[m].mesh.vertices.size()) {
        continue;
      }
      FloraMesh tree;
      if (!flora_mesh(std::span<const geometry::ClusterFileMotionPart>(source.motion_parts.data(),
                                                                       source.motion_parts.size()),
                      std::span<const geometry::ClusterFileStructuralPart>(
                          source.structural_parts.data(), source.structural_parts.size()),
                      source.species, tree)) {
        continue;
      }
      sway_padding[m] = flora_bounds_padding(
          tree, std::span<const Vec3>(dags[m].mesh.vertices.data(), dags[m].mesh.vertices.size()),
          std::span<const geometry::ClusterFileMotionVertex>(source.motion_vertices.data(),
                                                             source.motion_vertices.size()));
    }
  }
  scene::AuthoredSet set;
  set.tile_size = tile;
  Vector<u32> set_of(meshes, ~0u);
  Vector<u32> resident;  // the entry's resident meshes, as scene meshes
  Vector<u32> slot_of;   // per scene mesh, its place in `resident`; grown as meshes are made
  slot_of.resize(meshes, ~0u);
  const auto slot = [&](u32 scene_mesh) {
    if (slot_of.size() <= scene_mesh) slot_of.resize(scene_mesh + 1, ~0u);
    if (slot_of[scene_mesh] == ~0u) {
      slot_of[scene_mesh] = resident.size();
      resident.push_back(scene_mesh);
    }
    return slot_of[scene_mesh];
  };
  for (u32 m = 0; m < meshes; ++m) {
    if (!(scale_max[m] > 0.0f)) continue;
    scene::AuthoredForms forms;
    forms.rings.push_back(slot(m));
    u32 drawn = m;
    u32 kept = dags[m].mesh.clusters.size();
    for (u32 r = 1; r < rings; ++r) {
      const f32 floor_m = ring_floor_m(resolved.world, r);
      if (floor_m > 0.0f) {
        const bool sways = sway_padding[m] >= 0.0f && floor_m < sway_m;
        geometry::LodFormCut cut;
        cut.origin = fit[m].anchor;
        cut.distance = floor_m / (fit[m].scale * scale_max[m]);
        if (sways) cut.distance = std::max(0.0f, cut.distance - sway_padding[m]);
        cut.proj_scale = proj;
        cut.threshold_px = rules.threshold_px;
        cut.foliage_px = rules.foliage_px;
        cut.fade = rules.fade;
        geometry::ClusterLodMesh form;
        Vector<u32> coverage;
        geometry::LodFormMap map;
        const SourceMesh& source = sources[m];
        if (!geometry::extract_lod_form(
                dags[m],
                std::span<const u32>(source.lod_coverage.data(), source.lod_coverage.size()), cut,
                form, coverage, map, &error)) {
          error =
              resolved.mesh_info[m].name + ": its ring " + std::to_string(r) + " form: " + error;
          return false;
        }
        // A form that keeps what the ring inside it drew is that one: the cut only ever drops
        // more as the distance grows, so the same count is the same clusters.
        if (map.clusters.size() < kept) {
          SourceMesh copy = source;
          copy.part_of_cluster.clear();
          for (const u32 c : map.clusters)
            copy.part_of_cluster.push_back(source.part_of_cluster[c]);
          copy.lod_coverage = std::move(coverage);
          if (source.motion_vertices.size() == dags[m].mesh.vertices.size()) {
            copy.motion_vertices.clear();
            for (const u32 v : map.vertices)
              copy.motion_vertices.push_back(source.motion_vertices[v]);
            const u32 before = copy.motion_parts.size();
            prune_form_parts(copy);
            stats.form_parts_before += before;
            stats.form_parts_after += copy.motion_parts.size();
          } else {
            copy.motion_vertices.clear();
            copy.motion_parts.clear();
          }
          drawn = resolved.meshes.size();
          kept = map.clusters.size();
          std::string path = resolved.meshes[m];  // a copy: never push a Vector's own element
          resolved.meshes.push_back(std::move(path));
          SceneMeshInfo info = resolved.mesh_info[m];
          info.name += " (ring " + std::to_string(r) + ")";
          info.origin = "form";
          resolved.mesh_info.push_back(std::move(info));
          sources.push_back(std::move(copy));
          dags.push_back(std::move(form));
          out.form_source.push_back(m);
          out.form_rigid.push_back(sways ? 0 : 1);
          ++stats.forms;
          stats.form_clusters += kept;
        }
      }
      forms.rings.push_back(slot(drawn));
    }
    set_of[m] = set.forms.size();
    set.forms.push_back(std::move(forms));
  }

  // The set, in the file's order with each instance's place in it as its id, and the prefix
  // without it. The floor: the most pairs one tile's instances hold drawn whole.
  Vector<SceneInstance> kept_instances;
  kept_instances.reserve(resolved.instances.size() - taken);
  Vector<std::pair<u64, u64>> tile_pairs;  // (tile key, pairs), sorted afterwards
  tile_pairs.reserve(taken);
  set.instances.reserve(taken);
  // The whole read's index to the loaded one (`SceneData::loaded_of_read`), the tail's none.
  out.loaded_of_read.assign(resolved.instances.size(), k_not_loaded);
  for (u32 i = 0; i < resolved.instances.size(); ++i) {
    const SceneInstance& instance = resolved.instances[i];
    if (i < first || i >= first + count || take[i - first] == 0) {
      // Kept, at the place the whole read would have given it (`SceneInstance::motion_key`), so a
      // swaying landmark has the whole read's phase.
      out.loaded_of_read[i] = kept_instances.size();
      kept_instances.push_back(instance);
      if (kept_instances.back().motion_key == k_motion_key_load)
        kept_instances.back().motion_key = i;
      continue;
    }
    scene::AuthoredInstance a;
    a.forms = set_of[instance.mesh];
    a.translation = WorldPos{instance.origin.x + static_cast<f64>(instance.transform.position.x),
                             instance.origin.y + static_cast<f64>(instance.transform.position.y),
                             instance.origin.z + static_cast<f64>(instance.transform.position.z)};
    a.rotation = instance.transform.rotation;
    a.scale = instance.transform.scale;
    a.id = i - first + 1;
    set.instances.push_back(a);
    const u64 pairs = dags[instance.mesh].mesh.clusters.size();
    stats.whole_pairs += pairs;
    const i64 tx = static_cast<i64>(std::floor(a.translation.x / static_cast<f64>(tile)));
    const i64 tz = static_cast<i64>(std::floor(a.translation.z / static_cast<f64>(tile)));
    tile_pairs.push_back(
        {(static_cast<u64>(static_cast<u32>(tx)) << 32) | static_cast<u64>(static_cast<u32>(tz)),
         pairs});
  }
  std::sort(tile_pairs.begin(), tile_pairs.end());
  Vector<StreamedPlacements::TileInstances> tile_instances;
  u64 floor_pairs = 0;
  for (u32 i = 0; i < tile_pairs.size();) {
    u64 sum = 0;
    u32 j = i;
    for (; j < tile_pairs.size() && tile_pairs[j].first == tile_pairs[i].first; ++j)
      sum += tile_pairs[j].second;
    floor_pairs = std::max(floor_pairs, sum);
    tile_instances.push_back(StreamedPlacements::TileInstances{
        static_cast<i32>(static_cast<u32>(tile_pairs[i].first >> 32)),
        static_cast<i32>(static_cast<u32>(tile_pairs[i].first)), j - i});
    i = j;
  }
  out.tail_floor_pairs = static_cast<u32>(std::min<u64>(floor_pairs, 0xFFFFFFFFu));
  resolved.instances = std::move(kept_instances);
  StreamedPlacements entry;
  entry.generator = "authored";
  entry.params = schema::to_json(set);
  entry.where = (resolved.name.empty() ? std::string("scene") : resolved.name) + ": instances";
  entry.meshes = std::move(resident);
  entry.motion_key_base = first;  // id n is instance first + n - 1 of the whole read
  entry.tile_instances = std::move(tile_instances);
  resolved.streamed.push_back(std::move(entry));
  stats.streamed = taken;
  ENGINE_LOG_INFO(log_renderer, "instances streamed by the world", log::field("streamed", taken),
                  log::field("kept_actors", stats.kept_actors),
                  log::field("kept_deformed", stats.kept_deformed),
                  log::field("kept_reach", stats.kept_reach), log::field("forms", stats.forms),
                  log::field("form_clusters", stats.form_clusters),
                  log::field("form_parts_before", stats.form_parts_before),
                  log::field("form_parts_after", stats.form_parts_after),
                  log::field("whole_pairs", stats.whole_pairs),
                  log::field("tile_floor_pairs", out.tail_floor_pairs));
  return true;
}

}  // namespace

u32 loaded_instance(const SceneData& scene, u32 read) noexcept {
  if (scene.loaded_of_read.empty()) return read < scene.instances.size() ? read : k_not_loaded;
  return read < scene.loaded_of_read.size() ? scene.loaded_of_read[read] : k_not_loaded;
}

u32 read_instance(const SceneData& scene, u32 loaded) noexcept {
  // The load gives every kept instance the whole read's place as its `motion_key`; checked
  // against the map, so a key from anywhere else can never pass for one.
  if (scene.loaded_of_read.empty() || loaded >= scene.motion_keys.size()) return loaded;
  const u32 read = scene.motion_keys[loaded];
  return read < scene.loaded_of_read.size() && scene.loaded_of_read[read] == loaded ? read : loaded;
}

u32 read_instance_count(const SceneData& scene) noexcept {
  return scene.loaded_of_read.empty() ? scene.instances.size() : scene.loaded_of_read.size();
}

void mesh_bounds(const geometry::ClusterLodMesh& lod, u32 first, u32 count, Vec3& center,
                 f32& radius) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    lo = Vec3{std::min(lo.x, c.center.x - c.radius), std::min(lo.y, c.center.y - c.radius),
              std::min(lo.z, c.center.z - c.radius)};
    hi = Vec3{std::max(hi.x, c.center.x + c.radius), std::max(hi.y, c.center.y + c.radius),
              std::max(hi.z, c.center.z + c.radius)};
  }
  center = (lo + hi) * 0.5f;
  radius = 1e-6f;  // tighter than the box diagonal for elongated meshes
  for (u32 i = first; i < first + count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius = std::max(radius, length(c.center - center) + c.radius);
  }
}

f32 ground_radius(const geometry::ClusterLodMesh& lod, u32 first, u32 count) noexcept {
  f32 radius = 0.0f;
  for (u32 i = first; i < first + count && i < lod.mesh.clusters.size(); ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    radius =
        std::max(radius, std::sqrt(c.center.x * c.center.x + c.center.z * c.center.z) + c.radius);
  }
  return radius;
}

std::string hash_hex(u64 hash) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (u32 i = 0; i < 16; ++i)
    text[15 - i] = k_digits[(hash >> (4 * i)) & 0xfu];
  return text;
}

bool parse_hash_hex(std::string_view text, u64& out) noexcept {
  if (text.size() != 16) return false;
  u64 value = 0;
  for (const char c : text) {
    u64 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u64>(c - 'a' + 10);
    } else {
      return false;
    }
    value = value << 4 | digit;
  }
  out = value;
  return true;
}

bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error) {
  return read_scene_file(path, SceneFileOptions{}, out, error);
}

bool read_scene_props(const std::string& path, Vector<SceneProp>& out, std::string& error) {
  out.clear();
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = std::string("cannot read ") + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  scene::Scene file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    error = path + ": " + schema_errors(ctx);
    return false;
  }
  for (const scene::Prop& prop : file.props)
    out.push_back(SceneProp{prop.name, 0});
  return true;
}

bool read_scene_file(const std::string& path, const SceneFileOptions& options, SceneDesc& out,
                     std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = std::string("cannot read ") + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  profiling::TraceStage parse_stage("json parse");
  const JsonParseResult parsed = parse_json(text, root);
  parse_stage.end();
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  // One grammar, and it is the schema's: an unknown field is refused with its path rather than
  // skipped, because a misspelt `"ground"` that quietly did nothing would float a building.
  scene::Scene file;
  schema::ReadContext ctx;
  profiling::TraceStage schema_stage("schema read");
  const bool schema_read = schema::from_json(file, root, ctx);
  schema_stage.end();
  if (!schema_read || !ctx.ok()) {
    error = path + ": " + schema_errors(ctx);
    return false;
  }
  ENGINE_STARTUP_STAGE("scene expand");  // the providers, the placements, the instances
  if (!file.format.empty() && file.format != "engine.scene.v1") {
    error = path + ": format is '" + file.format + "', not engine.scene.v1";
    return false;
  }
  if (file.meshes.empty() && !file.terrain.has_value() && file.ruins.empty() &&
      file.placements.empty()) {
    error = path +
            ": no \"meshes\", no \"terrain\", no \"ruins\" and no \"placements\": a scene needs at "
            "least one";
    return false;
  }
  out.name = file.name;
  out.terrain_tiles = options.terrain_tiles;
  out.file_hash = hash_bytes(text.data(), text.size());
  // The sky (sky.h, ADR-0048): its provider is found and made here, as a terrain's ground provider
  // is below, so a name this executable does not carry, or an entry the provider refuses, refuses
  // the file with a sentence rather than drawing the stand-in the scene did not ask for.
  if (file.sky.has_value()) {
    const std::string_view sky_name = scene_gen::sky_provider_name(*file.sky);
    const scene_gen::SkyProviderDesc* sky_desc =
        scene_gen::GeneratorRegistry::global().find_sky(sky_name);
    if (sky_desc == nullptr) {
      error = path + ": the sky " + scene_gen::GeneratorRegistry::global().unknown_sky(sky_name);
      return false;
    }
    scene_gen::SkyProvider check;
    std::string why;
    if (!sky_desc->make(*file.sky, check, &why)) {
      error = path + ": " + why;
      return false;
    }
    out.sky = *file.sky;
  }
  // The participating medium (`engine.scene.Medium`): carried as the file gave it, read by the
  // capability that draws it (systems/volumetrics), as `world` is carried for the world's.
  out.medium = file.medium;
  // The fires (`engine.scene.Fire`, fire.h): lights the renderer burns, and flames the medium
  // draws.
  if (!read_scene_fires(file, path, out, error)) return false;

  // The overlay: a map from content hash to a local file, read before the meshes so that a mesh
  // it names is loaded from there and checked against the hash the scene gave for it.
  Vector<std::pair<u64, std::string>> overlay;  // hash, path
  if (!options.overlay.empty()) {
    std::string overlay_text;
    const io::Status read = io::read_file(options.overlay, overlay_text);
    if (read != io::Status::Ok) {
      error = "cannot read the overlay " + options.overlay + ": " + io::status_name(read);
      return false;
    }
    JsonValue overlay_json;
    const JsonParseResult overlay_parsed = parse_json(overlay_text, overlay_json);
    scene::Overlay manifest;
    schema::ReadContext overlay_ctx;
    if (!overlay_parsed.ok) {
      error = options.overlay + ":" + std::to_string(overlay_parsed.line) + ": " +
              overlay_parsed.message;
      return false;
    }
    if (!schema::from_json(manifest, overlay_json, overlay_ctx) || !overlay_ctx.ok()) {
      error = options.overlay + ": " + schema_errors(overlay_ctx);
      return false;
    }
    const std::string overlay_dir(io::parent_path(options.overlay));
    for (const scene::OverlayEntry& entry : manifest.entries) {
      u64 hash = 0;
      if (!parse_hash_hex(entry.hash, hash)) {
        error = options.overlay + ": '" + entry.hash + "' is not 16 lower-case hex digits";
        return false;
      }
      overlay.push_back({hash, io::is_absolute_path(entry.path) || overlay_dir.empty()
                                   ? entry.path
                                   : io::join_path(overlay_dir, entry.path)});
    }
  }

  const std::string dir(io::parent_path(path));
  out.camera_path =
      file.camera_path.empty() || io::is_absolute_path(file.camera_path) || dir.empty()
          ? file.camera_path
          : io::join_path(dir, file.camera_path);
  for (u32 i = 0; i < file.meshes.size(); ++i) {
    const scene::Mesh& mesh = file.meshes[i];
    const std::string where = path + ": mesh " + std::to_string(i);
    if (mesh.path.empty()) {
      error = where + " has no \"path\"";
      return false;
    }
    SceneMeshInfo info;
    info.name = mesh.name;
    if (!mesh.hash.empty() && !parse_hash_hex(mesh.hash, info.hash)) {
      error = where + ": hash '" + mesh.hash + "' is not 16 lower-case hex digits";
      return false;
    }
    if (mesh.fit.has_value()) {
      info.fit.height = mesh.fit->height;
      info.fit.extent = mesh.fit->extent;
      info.fit.ground = mesh.fit->ground;
    }
    std::string resolved =
        io::is_absolute_path(mesh.path) || dir.empty() ? mesh.path : io::join_path(dir, mesh.path);
    if (!mesh.overlay.empty()) {
      u64 wanted = 0;
      if (!parse_hash_hex(mesh.overlay, wanted)) {
        error = where + ": overlay '" + mesh.overlay + "' is not 16 lower-case hex digits";
        return false;
      }
      for (const auto& [hash, overlay_path] : overlay) {
        if (hash != wanted) continue;
        resolved = overlay_path;
        info.hash = hash;  // the load hashes the replacement and refuses other bytes
        info.origin = "overlay";
        break;
      }
      if (!overlay.empty() && std::string_view(info.origin) != "overlay") {
        ENGINE_LOG_WARN(log_renderer, "the overlay does not name this mesh; the scene's file stays",
                        log::field("mesh", mesh.name), log::field("overlay", mesh.overlay));
      }
    }
    out.meshes.push_back(std::move(resolved));
    out.mesh_info.push_back(std::move(info));
  }
  // The props (`engine.scene.Prop`): meshes a game module places, loaded like the file's own and
  // placed by nothing here. After the file's meshes, so every index the file's instances use still
  // names what it named, and before the placement generators' and the terrain's, which count the
  // meshes before them.
  for (u32 i = 0; i < file.props.size(); ++i) {
    const scene::Prop& prop = file.props[i];
    const std::string where = path + ": props " + std::to_string(i);
    if (prop.name.empty()) {
      error = where + " has no \"name\"";
      return false;
    }
    if (prop.mesh.empty()) {
      error = where + " (\"" + prop.name + "\") has no \"mesh\"";
      return false;
    }
    for (const SceneProp& earlier : out.props) {
      if (earlier.name == prop.name) {
        error = where + ": a second prop named \"" + prop.name + "\"";
        return false;
      }
    }
    SceneMeshInfo info;
    info.name = prop.name;
    info.origin = "prop";
    if (!prop.hash.empty() && !parse_hash_hex(prop.hash, info.hash)) {
      error = where + ": hash '" + prop.hash + "' is not 16 lower-case hex digits";
      return false;
    }
    if (prop.fit.has_value()) {
      info.fit.height = prop.fit->height;
      info.fit.extent = prop.fit->extent;
      info.fit.ground = prop.fit->ground;
    }
    out.props.push_back(SceneProp{prop.name, out.meshes.size()});
    if (prop.fire.has_value()) {  // fire.h: every placed instance carries it
      FireDesc fire;
      std::string why;
      if (!read_fire(*prop.fire, fire, &why)) {
        error = where + " (\"" + prop.name + "\"): fire: " + why;
        return false;
      }
      out.props.back().fire = std::move(fire);
    }
    out.meshes.push_back(
        io::is_absolute_path(prop.mesh) || dir.empty() ? prop.mesh : io::join_path(dir, prop.mesh));
    out.mesh_info.push_back(std::move(info));
  }
  // A ground time asked of a file with no terrain is a question with nothing to answer it, and
  // reading the scene anyway would hand back a picture that silently ignored it.
  if (options.ground_time_s.has_value() && !file.terrain.has_value()) {
    error = path + ": a ground time was asked for, and the scene has no terrain";
    return false;
  }
  if (file.terrain.has_value()) {
    const scene::Terrain& t = *file.terrain;
    out.terrain.enabled = true;
    out.terrain.size = t.size;
    out.terrain.extent = t.extent;
    out.terrain.seed = t.seed;
    out.terrain.dune_height = t.dune_height;
    out.terrain.dune_wavelength = t.dune_wavelength;
    for (const scene::Ridge& r : t.ridges)
      out.terrain.ridges.push_back(TerrainRidge{r.from, r.to, r.height, r.width, r.roughness});
    for (const scene::Basin& b : t.basins)
      out.terrain.basins.push_back(TerrainBasin{b.center, b.radius, b.depth});
    // The ground provider (scene_gen.md): `provider` by name, or the one `generator` names; a
    // provider beside `generator: Dunes` other than the dunes is two answers to one question.
    if (t.generator == scene::TerrainGenerator::Dunes && !t.provider.empty() &&
        t.provider != "dunes") {
      error = path + ": the terrain names the ground provider \"" + t.provider +
              "\" and the generator Dunes; name one";
      return false;
    }
    out.terrain.provider = t.provider;
    out.terrain.materials = t.materials;
    if (out.terrain.materials.has_value()) {
      for (auto& layer : out.terrain.materials->layers) {
        for (auto* p :
             {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask, &layer.height})
          if (!p->empty() && !dir.empty() && !io::is_absolute_path(*p)) *p = io::join_path(dir, *p);
      }
      if (!validate_terrain_materials(*out.terrain.materials, &error, t.masks.has_value())) {
        error = path + ": " + error;
        return false;
      }
    }
    // An authored tile set (`Terrain.tiles`, docs/subsystems/tileset.md): its paths resolved
    // against the file's directory, and the provider it implies; beside another provider it is two
    // answers to one question.
    if (t.tiles.has_value()) {
      if (!t.provider.empty() && t.provider != "tiles") {
        error = path + ": the terrain carries a tile set and names the ground provider \"" +
                t.provider + "\"; a tile set is the tiles provider's";
        return false;
      }
      scene::TerrainTileSet tiles = *t.tiles;
      const auto resolve_path = [&](std::string& p) {
        if (!p.empty() && !dir.empty() && !io::is_absolute_path(p)) p = io::join_path(dir, p);
      };
      resolve_path(tiles.manifest);
      resolve_path(tiles.heightfield);
      resolve_path(tiles.ddc);
      out.terrain.tiles = std::move(tiles);
      if (out.terrain.provider.empty()) out.terrain.provider = "tiles";
    }
    // Ground materials by place (`Terrain.masks`, renderer.md): weights of the materials' palette,
    // built over an authored tile set by the `tiles` provider; its images resolved against the
    // file's directory and its regions' water bodies against the file's `water`, so the build and
    // its key see shapes and paths, never names.
    if (t.masks.has_value()) {
      if (!t.materials.has_value()) {
        error = path +
                ": terrain.masks weigh the palette of terrain.materials, which the terrain "
                "does not name";
        return false;
      }
      if (!t.tiles.has_value()) {
        error = path +
                ": terrain.masks are built over an authored tile set; the terrain names no "
                "\"tiles\" block";
        return false;
      }
      scene::TerrainMasks masks = *t.masks;
      const auto resolve_image = [&](std::string& p) {
        if (!p.empty() && !dir.empty() && !io::is_absolute_path(p)) p = io::join_path(dir, p);
      };
      for (auto& paint : masks.paint)
        resolve_image(paint.image);
      if (masks.index.has_value()) resolve_image(masks.index->image);
      if (!scene_gen::resolve_mask_water({file.water.data(), file.water.size()}, masks, &error)) {
        error = path + ": " + error;
        return false;
      }
      out.terrain.masks = std::move(masks);
    }
    out.terrain.generator = t.generator == scene::TerrainGenerator::Dunes || t.provider == "dunes"
                                ? TerrainGenerator::dunes
                                : TerrainGenerator::waves;
    // The caller's ground time replaces the file's here, before anything below reads it: the
    // placement entries stand on the ground at this time, the mesh is built at it, and the sky's
    // one clock starts from it (renderer.md, "One clock").
    out.terrain.time_s = options.ground_time_s.has_value() ? *options.ground_time_s : t.time;
    out.terrain.sand_flux = t.sand_flux;
    out.terrain.storms_per_year = t.storms_per_year;
    out.terrain.storm_strength = t.storm_strength;
    out.terrain.diurnal_strength = t.diurnal_strength;
    out.terrain.diurnal_peak_hour = t.diurnal_peak_hour;
    out.terrain.diurnal_veer_deg = t.diurnal_veer_deg;
    out.terrain.diurnal_veer_hour = t.diurnal_veer_hour;
    out.terrain.storm_gain = t.storm_gain;
    out.terrain.storm_gains.clear();
    for (const scene::StormGain& g : t.storm_gains)
      out.terrain.storm_gains.push_back(TerrainDesc::StormGain{g.storm, g.gain});
    out.terrain.has_bands = false;
    out.terrain.bands.clear();
    if (t.bands.has_value()) {
      out.terrain.has_bands = true;
      for (const scene::TerrainBand& b : *t.bands) {
        TerrainBand band;
        band.name = b.name;
        band.kind = b.kind == scene::DuneKind::Barchan ? 1 : 0;
        band.height_min = b.height_min;
        band.height_max = b.height_max;
        band.cell = b.cell;
        band.share = b.share;
        band.length_min = b.length_min;
        band.length_max = b.length_max;
        band.stoss = b.stoss;
        band.bend = b.bend;
        band.sinuosity = b.sinuosity;
        band.spread_deg = b.spread_deg;
        band.sharpness = b.sharpness;
        band.side_days = b.side_days;
        band.sharp_days = b.sharp_days;
        band.couple = static_cast<u8>(b.couple);
        band.couple_width = b.couple_width;
        band.far = b.far;
        if (!(b.celerity_scale > 0.0f && b.celerity_scale <= 1000.0f)) {
          error = path + ": a dune band's celerity_scale must be within (0, 1000]";
          return false;
        }
        band.celerity_scale = b.celerity_scale;
        out.terrain.bands.push_back(std::move(band));
      }
    }
    // The sand's detail (renderer.md, "The sand close up"): the scene's numbers, checked here so a
    // number the mechanism cannot draw is refused with the file rather than drawn wrong.
    out.terrain.has_detail = t.detail.has_value();
    if (out.terrain.has_detail && out.terrain.materials.has_value()) {
      error =
          path +
          ": terrain.materials and terrain.detail are alternative surface-detail paths; name one";
      return false;
    }
    out.terrain.detail = t.detail.has_value() ? *t.detail : scene::TerrainDetail{};
    if (out.terrain.has_detail) {
      std::string why;
      if (!validate_terrain_detail(out.terrain.detail, &why)) {
        error = path + ": " + why;
        return false;
      }
    }
  }
  const u32 file_meshes = file.meshes.size();
  // One sampler for every height this read asks the terrain for — each grounded instance, each
  // scatter's, and each ruin's dozens — because `terrain_height` draws the dune field again per
  // call, and that was most of what reading a thousand ruins cost (ruins.md, "Performance
  // notes"). The heights are the direct function's, bit for bit. It is also where the terrain's
  // ground provider is found and made (scene_gen.md): a name this executable does not carry, or an
  // entry the provider refuses — a dune time out of range, a band table the field cannot be built
  // from — refuses the file with the provider's sentence.
  profiling::TraceStage sampler_stage("ground sampler");
  const TerrainSampler ground(out.terrain);
  sampler_stage.end();
  if (out.terrain.enabled) {
    if (!ground.ok()) {
      error = path + ": " + ground.error();
      return false;
    }
    const scene::Terrain& t = *file.terrain;
    if (t.size < 2 || t.size > k_terrain_max_size || !(t.extent > 0.0f)) {
      error = path + ": terrain size must be within 2.." + std::to_string(k_terrain_max_size) +
              " and extent positive";
      return false;
    }
  }
  // The surface under a world position (ADR-0053): asked at its whole millimetre, which never
  // passes through a float32 metre, at the terrain's own time — the heights the sampler's float
  // `height` gives at a point a float holds to the millimetre (`GroundOps::height_mm`), and the
  // right ones far out. A file's instances and its scatters' stand on it alike.
  auto ground_at_world = [&](WorldPos p) {
    return out.terrain.enabled
               ? static_cast<f64>(ground.provider().height_mm(
                     out.terrain.time_s, scene_gen::nearest_mm(p.x), scene_gen::nearest_mm(p.z)))
               : 0.0;
  };
  // A grounded fire stands where a grounded instance would (fire.h).
  for (u32 i = 0; i < file.fires.size() && i < out.fires.size(); ++i) {
    if (file.fires[i].ground) out.fires[i].position.y += ground_at_world(file.fires[i].position);
  }

  // The file's own instances and its scatters', the run a streamed world may hand to the
  // "authored" generator (`load_scene`; renderer.md, "Authored instances per tile").
  out.authored_first = out.instances.size();
  out.authored.enabled = options.world_instances;
  for (u32 i = 0; i < file.instances.size(); ++i) {
    const scene::Instance& entry = file.instances[i];
    if (entry.mesh >= file_meshes) {
      error = path + ": instance " + std::to_string(i) + " names no known mesh";
      return false;
    }
    if (entry.ground && !out.terrain.enabled) {
      error = path + ": instance " + std::to_string(i) + " stands on a terrain the scene has not";
      return false;
    }
    SceneInstance instance;
    instance.mesh = entry.mesh;
    // The file's translation is a `worldpos` (Instance version 3): it goes whole into the
    // instance's f64 `origin`, and `transform.position` stays zero, so nothing rounds it to a float
    // on the way to the instance's cell. A version 2 file's numbers read as they are written.
    instance.origin = entry.translation;
    if (entry.ground) instance.origin.y += ground_at_world(entry.translation);
    instance.transform.rotation =
        normalize(quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(entry.yaw_deg)) *
                  normalize(entry.rotation));
    if (entry.scale.has_value()) instance.transform.scale = *entry.scale;
    // {"animation":{"clip":"Run","speed":1.5,"phase":0.4}} — read whole and handed on unread.
    // An empty object is a legal block and means "the skin's first clip at speed 1".
    if (entry.animation.has_value()) {
      instance.animation.play = true;
      instance.animation.clip = entry.animation->clip;
      instance.animation.speed = entry.animation->speed;
      instance.animation.phase = entry.animation->phase;
    }
    out.instances.push_back(instance);
  }
  // Scatters, expanded here from their seeds, so the renderer sees plain instances and two
  // machines place the same ones. **The centre is a `worldpos`** (Scatter version 2, ADR-0053):
  // what the seed draws — the angle, the radius, the scale, the yaw — stays float32, the
  // expansion's content and the size of the annulus, and an instance's place is the centre plus
  // that offset in f64, into `origin` as a file's instance's is, with its ground asked at the whole
  // millimetre
  // (`ground_at_world`). Until 2026-10-07 the centre was a float32 `vec2` and the place float32
  // world metres, a 3.1 cm grid 420 km out, and the ground asked at them.
  for (u32 s = 0; s < file.scatters.size(); ++s) {
    const scene::Scatter& scatter = file.scatters[s];
    if (scatter.mesh >= file_meshes) {
      error = path + ": scatter " + std::to_string(s) + " names no known mesh";
      return false;
    }
    if (scatter.ground && !out.terrain.enabled) {
      error = path + ": scatter " + std::to_string(s) + " stands on a terrain the scene has not";
      return false;
    }
    u64 state = static_cast<u64>(scatter.seed) * 0x9E3779B97F4A7C15ull + s;
    for (u32 k = 0; k < scatter.count; ++k) {
      // Uniform over the annulus's area, not its radius, so the middle is not crowded.
      const f32 angle = seeded_unit(state) * 2.0f * k_pi;
      const f32 r0 = scatter.radius_min * scatter.radius_min;
      const f32 r1 = scatter.radius_max * scatter.radius_max;
      const f32 radius = std::sqrt(r0 + (r1 - r0) * seeded_unit(state));
      const f32 scale =
          scatter.scale_min + (scatter.scale_max - scatter.scale_min) * seeded_unit(state);
      const f32 yaw = seeded_unit(state) * 2.0f * k_pi;
      SceneInstance instance;
      instance.mesh = scatter.mesh;
      instance.origin = WorldPos{scatter.center.x, 0.0, scatter.center.z} +
                        DVec3{static_cast<f64>(std::cos(angle) * radius), 0.0,
                              static_cast<f64>(std::sin(angle) * radius)};
      instance.origin.y =
          static_cast<f64>(scatter.y) + (scatter.ground ? ground_at_world(instance.origin) : 0.0);
      instance.transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, yaw);
      instance.transform.scale = Vec3{scale, scale, scale};
      out.instances.push_back(instance);
    }
  }
  // A streamed world (the file's `world` block, or the caller's `SceneFileOptions::world`): the
  // ring's parameters, carried for the world capability, and the ruins entries left for it to
  // assemble a tile at a time.
  if (file.world.has_value() || options.world) {
    const scene::WorldRings rings = file.world.has_value() ? *file.world : scene::WorldRings{};
    out.world.enabled = true;
    out.world.tile_size = rings.tile_size;
    out.world.hysteresis = rings.hysteresis;
    out.world.simulated = rings.simulated;
    if (rings.rings.size() > 7) {
      error = path + ": a world has at most 7 rings";
      return false;
    }
    out.world.ring_count = rings.rings.size();
    out.world.ground = rings.ground;
    for (u32 r = 0; r < rings.rings.size(); ++r) {
      out.world.radius[r] = rings.rings[r].radius;
      out.world.ruins[r] = static_cast<u8>(rings.rings[r].ruins);
      out.world.ground_cells[r] = rings.rings[r].ground_cells;
    }
    if (!(out.world.tile_size > 0.0f)) {
      error = path + ": the world's tile_size must be positive";
      return false;
    }
  }
  // The file's own instances and its scatters' are the run a streamed world may stream.
  out.authored_count = out.instances.size() - out.authored_first;
  // The placement entries — the ruins, and any generator by name — through the scene-generator
  // registry, after the ground they stand on: expanded here, or in a streamed world left for the
  // world to expand tile by tile.
  // Ground tiles beside streamed placements were refused here until 2026-10-12: the tiles are
  // terrain levels drawn through the deformed-vertex pool, and a scene whose instances come and go
  // had no pool. It has one now, and the ray tracing chain, laid out over the tail's blocks
  // (renderer.md, "Instances that come and go").
  profiling::TraceStage placements_stage("placements");
  if (!expand_placements(file, path, dir, ground, out, error)) return false;
  placements_stage.end();
  profiling::TraceStage water_stage("water");
  if (!read_scene_water(file, path, dir, ground, out, error)) return false;  // water.md
  water_stage.end();
  if (file.instances.empty() && file.scatters.empty() && file.ruins.empty() &&
      file.placements.empty()) {
    for (u32 i = 0; i < file_meshes; ++i)
      out.instances.push_back(SceneInstance{i, Transform3::identity()});
  }
  // The terrain is the last mesh, with one identity instance after everything the file placed,
  // so every index the file used still names what it named.
  if (out.terrain.enabled) {
    // After the file's meshes and every placement generator's, so its index is the count so far.
    const u32 terrain_mesh = out.meshes.size();
    out.meshes.push_back(std::string());
    SceneMeshInfo info;
    info.name = "terrain";
    info.origin = "terrain";
    out.mesh_info.push_back(std::move(info));
    out.instances.push_back(SceneInstance{terrain_mesh, Transform3::identity()});
  }
  return true;
}

bool load_scene(const SceneDesc& desc, SceneData& out, std::string& error) {
  if (desc.meshes.empty()) {
    error = "a scene needs at least one mesh";
    return false;
  }
  SceneDesc resolved = desc;
  // The grid is hidden under terrain tiles; retaining an authored multi-kilometre source grid
  // here defeats streaming before the first frame. Its extent remains the material's UV frame.
  if ((resolved.terrain_tiles || (resolved.world.enabled && resolved.world.ground)) &&
      resolved.terrain.enabled && terrain_has_millimetres(resolved.terrain) &&
      !terrain_moves(resolved.terrain) && !resolved.stream)
    resolved.terrain.size = 17;
  // The derived-data cache lives at <repo>/ddc, found by walking up from the executable to the
  // directory that holds AGENTS.md, unless a root was named. With no root there is nowhere to
  // keep a built mesh, so the cache is simply off.
  if (resolved.cache && resolved.ddc.empty()) {
    resolved.ddc = geometry::find_ddc_root(platform::executable_directory());
    if (resolved.materials.empty())
      resolved.materials = materials::default_pack_dir(platform::executable_directory());
    if (resolved.ddc.empty()) {
      resolved.cache = false;
      ENGINE_LOG_WARN(log_renderer, "no derived-data root above the executable",
                      log::field("from", platform::executable_directory()));
    }
  }

  const i64 build_start = time::monotonic_ns();
  // `heightfield` is what makes the GPU scene synthesize the ripple texture and the height-banded
  // materials, so it is the *terrain* and not "procedural": the shredded atlas brings its own
  // material and its own image and goes down the ordinary path.
  out.heightfield = resolved.meshes.size() == 1 && resolved.meshes[0].empty() &&
                    resolved.procedural == Procedural::heightfield && !resolved.terrain.enabled;
  const bool described = resolved.mesh_info.size() == resolved.meshes.size();
  if (!resolved.mesh_info.empty() && !described) {
    error = "SceneDesc::mesh_info must be empty or parallel to meshes";
    return false;
  }
  out.sources.resize(resolved.meshes.size());
  Vector<geometry::ClusterLodMesh> dags(resolved.meshes.size());
  Vector<geometry::ClusterPages> tables(resolved.meshes.size());
  // **The meshes load on the job system**, one job each, the calling thread helping (renderer.md,
  // "What the load reads"; roadmap W75): every mesh's read, hash, container and build is its own
  // and lands in its own slot, so nothing about the result depends on the thread count or on which
  // finished first, and the first failure reported is the lowest mesh index's, as the serial loop
  // reported it. A path a later mesh repeats loads in a second wave, after the first has written
  // the entry it shares, so it is a cache hit as it was serially rather than a second build of the
  // same entry. One pool for the whole load: these jobs, then the texture pass's.
  const i64 meshes_start_ns = time::monotonic_ns();
  std::optional<jobs::JobSystem> pool;
  pool.emplace(content_build::job_config(resolved.load_jobs));
  out.load.meshes = resolved.meshes.size();
  out.load.threads = pool->total_worker_count() + 1;
  content_build::ImageIdentities identities(resolved.cache ? resolved.ddc : std::string(),
                                            materials::pack_at(resolved.materials));
  {
    struct MeshJob {
      const SceneDesc* desc = nullptr;
      const std::string* path = nullptr;
      u64 expected = 0;
      SourceMesh* source = nullptr;
      geometry::ClusterLodMesh* lod = nullptr;
      geometry::ClusterPages* pages = nullptr;
      content_build::ImageIdentities* identities = nullptr;
      bool ok = false;
      std::string error;
    };
    Vector<MeshJob> mesh_jobs(resolved.meshes.size());
    Vector<u32> wave(resolved.meshes.size(), 0u);
    for (u32 m = 0; m < resolved.meshes.size(); ++m) {
      MeshJob& job = mesh_jobs[m];
      job.desc = &resolved;
      job.path = &resolved.meshes[m];
      job.expected = described ? resolved.mesh_info[m].hash : 0;
      job.source = &out.sources[m];
      job.lod = &dags[m];
      job.pages = &tables[m];
      job.identities = &identities;
      for (u32 e = 0; e < m; ++e) {
        if (resolved.meshes[e] == resolved.meshes[m] && !resolved.meshes[m].empty()) {
          wave[m] = 1;
          break;
        }
      }
    }
    const auto run = [](void* data) {
      MeshJob& job = *static_cast<MeshJob*>(data);
      ENGINE_STARTUP_STAGE_DETAIL("mesh", *job.path);
      job.ok = load_source_mesh(*job.path, *job.desc, job.expected, *job.source, *job.lod,
                                *job.pages, job.error, job.identities);
    };
    ENGINE_STARTUP_STAGE("meshes");
    profiling::trace_items(resolved.meshes.size());
    for (u32 w = 0; w < 2; ++w) {
      Vector<jobs::Job> list;
      jobs::Counter counter;
      for (u32 m = 0; m < mesh_jobs.size(); ++m)
        if (wave[m] == w) list.push_back(jobs::Job{run, &mesh_jobs[m], &counter});
      if (list.empty()) continue;
      pool->schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                     counter);
      pool->wait(counter);
    }
    for (u32 m = 0; m < mesh_jobs.size(); ++m) {
      if (mesh_jobs[m].ok) continue;
      error = std::move(mesh_jobs[m].error);
      if (described && !resolved.mesh_info[m].name.empty())
        error = resolved.mesh_info[m].name + ": " + error;
      return false;
    }
  }
  out.load.meshes_ms = static_cast<f64>(time::monotonic_ns() - meshes_start_ns) / 1.0e6;
  // Every mesh's textures at once: one identity per distinct image, a built one trusted from its
  // header, the rest built on the same pool (`prepare_scene_textures`). Before the authored forms
  // below copy the records, so a form carries its mesh's keys.
  prepare_scene_textures(resolved, out.sources, identities, *pool, out.load);
  // Each mesh's path as the scene named it, for a sentence about it (a form copies its source's).
  for (u32 m = 0; m < out.sources.size() && m < resolved.meshes.size(); ++m)
    out.sources[m].path = resolved.meshes[m];
  // A streamed world's own instances, before the merge: their forms are meshes of their own
  // (renderer.md, "Authored instances per tile").
  {
    bool paged_scene = resolved.stream;
    for (const geometry::ClusterPages& table : tables)
      paged_scene = paged_scene && !table.pages.empty();
    if (!stream_authored(resolved, out.sources, dags, paged_scene, out, error)) return false;
    tables.resize(dags.size());
  }
  out.terrain = resolved.terrain;
  out.terrain_mesh = ~0u;
  if (resolved.terrain.enabled) {
    for (u32 m = 0; m < resolved.meshes.size(); ++m) {
      if (resolved.meshes[m].empty()) {
        out.terrain_mesh = m;
        break;
      }
    }
  }
  // The ground the hemisphere ambient's lower half carries: a terrain's sand, which is most of
  // any terrain, or the neutral grey (renderer.md, "The sky above, the ground below").
  const f32 grey = gfx::k_neutral_ground_albedo;
  out.ground_albedo = out.terrain.enabled ? terrain_sand_albedo() : Vec3{grey, grey, grey};
  out.mesh_info = resolved.mesh_info;
  out.name = resolved.name;
  out.file_hash = resolved.file_hash;
  out.mesh_primitives = out.sources[0].primitives;
  out.mesh_cache = out.sources[0].cache;
  // Streaming needs one page table over the whole scene, and it is a different merge: a paged mesh
  // is ordered coarse to fine, so pulling its leaves to the front — which the default merge does,
  // to make a direct draw of the leaves one range — would leave every one of its pages naming
  // clusters that are no longer beside each other. `merge_paged_cluster_meshes` keeps the order and
  // shifts the table ([geometry](geometry.md), "A scene of paged meshes"). A mesh whose table could
  // not be built takes the whole scene off the streaming path rather than half of it.
  bool paged = resolved.stream;
  for (const geometry::ClusterPages& table : tables)
    paged = paged && !table.pages.empty();
  if (paged) {
    if (!geometry::merge_paged_cluster_meshes(
            std::span<const geometry::ClusterLodMesh>(dags.data(), dags.size()),
            std::span<const geometry::ClusterPages>(tables.data(), tables.size()), out.lod,
            out.parts, out.pages, &error)) {
      return false;
    }
  } else if (dags.size() == 1) {
    out.lod = std::move(dags[0]);
    geometry::ClusterMeshPart part;
    part.cluster_count = out.lod.mesh.clusters.size();
    part.leaf_cluster_count = out.lod.level_cluster_counts[0];
    // **Where the leaves are has to be found, not assumed.** This branch takes a DAG as it
    // arrived, and a DAG read from a `.clusters` container arrived in **page order** — coarse
    // first, leaves last ([geometry](geometry.md), "Pages and streaming") — whether or not anyone
    // asked to stream from it, because the page layout is part of the derived-data cache key and
    // every container is written that way. Leaving `first_leaf_cluster` at zero therefore pointed
    // every consumer of it at the *coarsest* clusters of the mesh. The camera is the one that
    // showed it: it frames the leaves' bounds, a coarse cluster's sphere is looser than the
    // surface it stands for, and the looseness depends on how far the simplifier got — so two
    // builds of one mesh that differ only in their coarse levels drew it at two different sizes.
    // Found comparing a seam-aware LOD build against a position-only one, whose pictures
    // disagreed by 0.23 FLIP at the *finest* threshold, where they draw the same triangles.
    part.first_leaf_cluster = 0;
    for (u32 i = 0; i < out.lod.lod.size(); ++i) {
      if (out.lod.lod[i].level == 0) {
        part.first_leaf_cluster = i;
        break;
      }
    }
    part.quant_origin = out.lod.mesh.quant_origin;
    part.quant_scale = out.lod.mesh.quant_scale;
    // **The mesh's channels are the scene's channels**, and this branch has to say so because
    // nothing else will: `merge_cluster_meshes` fills `first_morph_channel`/`morph_channel_count`
    // for every part it concatenates, and a single mesh never goes through it. Left at zero, the
    // GPU scene gave the instance no morph stages and no deform record while `resolve_settings`
    // (which reads the stream, not the part) had turned the deform pass on — so it sized the pool
    // for zero vertices and every single morphed mesh failed to load with `vmaCreateBuffer:
    // VK_ERROR_INITIALIZATION_FAILED`. Found by the first real files through it, the Khronos morph
    // samples; the renderer's own morph test builds its `SceneData` by hand, sets the part's
    // channel run itself, and so had never been through `load_scene` at all.
    part.first_morph_channel = 0;
    part.morph_channel_count = out.lod.mesh.morph_channels.size();
    // And the same for the id space, for the same reason: only `merge_cluster_meshes` fills it.
    part.vertex_id_source = out.lod.mesh.vertex_ids.empty() ? geometry::VertexIdSource::none
                                                            : out.lod.mesh.vertex_id_source;
    out.parts.push_back(part);
  } else {
    // **The merge reorders a mesh, and its material map has to follow.** The scene convention
    // puts each mesh's leaves first, and every mesh that came through the cache arrived in page
    // order, leaves last — so the merge moves nearly every one of its clusters, and a
    // `part_of_cluster` left in the mesh's own order gives each merged cluster the material of
    // whichever cluster used to sit where it now sits. That drew rock on the dunes and metal on
    // the FlightHelmet's leather in every scene of more than one mesh from 2026-09-17 until the
    // first interactive session found it
    // (docs/experiments/first-interactive-session-2026-09-24.md): patches that came and went as the
    // LOD cut moved, because each cut drew other clusters.
    Vector<u32> source_of_cluster;
    if (!geometry::merge_cluster_meshes(dags, out.lod, out.parts, &error,
                                        geometry::ClusterOrder::leaves_first, &source_of_cluster)) {
      return false;
    }
    for (u32 m = 0; m < out.parts.size(); ++m) {
      const geometry::ClusterMeshPart& part = out.parts[m];
      geometry::permute_cluster_array(
          std::span<const u32>(source_of_cluster.data() + part.first_cluster, part.cluster_count),
          out.sources[m].part_of_cluster);
      if (!out.sources[m].lod_coverage.empty()) {
        geometry::permute_cluster_array(
            std::span<const u32>(source_of_cluster.data() + part.first_cluster, part.cluster_count),
            out.sources[m].lod_coverage);
      }
    }
  }

  // Instances: the caller's, the grid's, or one identity instance of the one mesh.
  Vector<SceneInstance> instances = resolved.instances;
  if (resolved.grid_instances > 1) {
    instances.clear();
    Vec3 mesh_center{};
    f32 mesh_radius = 1.0f;
    mesh_bounds(out.lod, out.parts[0].first_cluster + out.parts[0].first_leaf_cluster,
                out.parts[0].leaf_cluster_count, mesh_center, mesh_radius);
    make_instance_grid(resolved.grid_instances, mesh_radius, resolved.grid_joints, 0.0f, instances);
  } else if (instances.empty()) {
    instances.push_back(SceneInstance{0, Transform3::identity()});
  }
  // Each fitted mesh's own transform, from the box of its vertices — every level's vertices, which
  // for an edge-collapse DAG are a subset of the source's, so the box is the source's box. It is
  // read here, while the merged float positions still exist (a streamed scene releases them once
  // its page source is attached).
  Vector<Mat4> fit_of_mesh(out.parts.size(), Mat4::identity());
  if (described) {
    for (u32 m = 0; m < out.parts.size(); ++m) {
      const MeshFit& fit = resolved.mesh_info[m].fit;
      if (!fit.active()) continue;
      // A distance form is placed exactly as the mesh it was cut from: its own vertices are only
      // some of that mesh's, so their box is not the one the fit was measured on.
      if (m < out.form_source.size() && out.form_source[m] != m) {
        fit_of_mesh[m] = fit_of_mesh[out.form_source[m]];
        continue;
      }
      const u32 first = out.parts[m].first_vertex;
      const u32 end =
          m + 1 < out.parts.size() ? out.parts[m + 1].first_vertex : out.lod.mesh.vertices.size();
      const FitParts parts =
          fit_parts(fit, std::span<const Vec3>(out.lod.mesh.vertices.data() + first, end - first));
      fit_of_mesh[m] =
          scaling(Vec3{parts.scale, parts.scale, parts.scale}) * translation(-parts.anchor);
    }
  }
  u32 pair_count = 0;
  const u32 palette = out.lod.mesh.skin_joint_count;
  ENGINE_STARTUP_STAGE("instances");
  profiling::trace_items(instances.size());
  for (const SceneInstance& source : instances) {
    if (source.mesh >= out.parts.size()) {
      error = "an instance names mesh " + std::to_string(source.mesh) + ", which the scene has no";
      return false;
    }
    gfx::InstanceDesc instance{};
    gfx::set_instance_placement(instance,
                                mat4_from_transform(source.transform) * fit_of_mesh[source.mesh],
                                instance_translation(source, fit_of_mesh[source.mesh]));
    instance.mesh = source.mesh;
    instance.first_pair = pair_count;
    // The cull pass inflates every sphere of a deformed instance by this, in the instance's own
    // space (gfx::InstanceDesc::bounds_padding). It is the caller's number: only the caller knows
    // which clips this instance will play.
    instance.bounds_padding = source.bounds_padding;
    pair_count += out.parts[source.mesh].cluster_count;
    // An instance may only be skinned by a mesh that carries a binding stream, and never by more
    // joints than that stream's palette: a binding names a palette slot with one byte, so a
    // longer array would be addressed by indices that cannot exist. Both are stated rather than
    // silently clamped, because either one means the caller and the content disagree.
    u32 joints = source.joints;
    if (joints > 0 && out.lod.mesh.skin.empty()) {
      error = "an instance is skinned, but its mesh carries no skin binding stream";
      return false;
    }
    if (joints > palette) {
      error = "an instance names " + std::to_string(joints) +
              " joints, but the mesh's palette is " + std::to_string(palette);
      return false;
    }
    out.instance_joints.push_back(joints);
    // The key a swaying instance's seed is hashed from: its place here unless the source named
    // the place a whole read gives it (renderer.md, "Streamed trees sway").
    out.motion_keys.push_back(source.motion_key != k_motion_key_load ? source.motion_key
                                                                     : out.motion_keys.size());
    out.skinned_instances += joints > 0 ? 1u : 0u;
    out.instances.push_back(instance);
  }
  out.pair_count = pair_count;
  out.max_joints = resolved.max_joints;
  if (out.skinned_instances == 0) out.instance_joints.clear();
  out.mesh_fit = std::move(fit_of_mesh);
  out.world = resolved.world;
  out.streamed = resolved.streamed;
  // A streamed world's instances come and go — unless all it streams is its ground (the world's
  // tiles, which are terrain levels in fixed slots, not instances of the tail).
  out.dynamic = resolved.world.enabled && !(resolved.world.ground && resolved.streamed.empty());
  out.sky = resolved.sky;
  out.medium = resolved.medium;
  out.props = resolved.props;
  out.fires = resolved.fires;

  update_scene_bounds(out);
  out.build_ns = time::monotonic_ns() - build_start;
  // What the load cost and what it made: an offscreen run has no other line that says it, and a
  // scene of hundreds of thousands of ruin blocks spends its time here, in the instance table.
  ENGINE_LOG_INFO(
      log_renderer, "scene loaded", log::field("ms", static_cast<f64>(out.build_ns) / 1.0e6),
      log::field("meshes", out.parts.size()), log::field("instances", out.instances.size()),
      log::field("pairs", out.pair_count), log::field("clusters", out.cluster_count()));
  return true;
}

bool make_instance(const SceneData& scene, const SceneInstance& source, u32 first_pair,
                   gfx::InstanceDesc& out, std::string* error) {
  if (source.mesh >= scene.parts.size()) {
    if (error != nullptr) {
      *error = "an instance names mesh " + std::to_string(source.mesh) + ", which the scene has no";
    }
    return false;
  }
  if (source.joints != 0) {
    if (error != nullptr) {
      *error =
          "an instance added after the load is rigid: a skinned one needs a deform entry the "
          "scene lays out once";
    }
    return false;
  }
  out = gfx::InstanceDesc{};
  const Mat4 fit =
      source.mesh < scene.mesh_fit.size() ? scene.mesh_fit[source.mesh] : Mat4::identity();
  gfx::set_instance_placement(out, mat4_from_transform(source.transform) * fit,
                              instance_translation(source, fit));
  out.mesh = source.mesh;
  out.first_pair = first_pair;
  out.bounds_padding = source.bounds_padding;
  return true;
}

bool pairs_after(const SceneData& scene, u32 first_pair, std::span<const SceneInstance> instances,
                 u32 max_pairs, u32& end_pair, std::string* error) {
  u64 pairs = first_pair;
  for (const SceneInstance& source : instances) {
    if (source.mesh >= scene.parts.size()) {
      if (error != nullptr) {
        *error =
            "an instance names mesh " + std::to_string(source.mesh) + ", which the scene has no";
      }
      return false;
    }
    pairs += scene.parts[source.mesh].cluster_count;
    if (pairs > max_pairs) {
      if (error != nullptr) {
        *error = "the instances would take the scene past " + std::to_string(max_pairs) +
                 " (instance, cluster) pairs";
      }
      return false;
    }
  }
  end_pair = static_cast<u32>(pairs);
  return true;
}

std::span<const u32> mesh_vertex_ids(const SceneData& scene, u32 mesh) noexcept {
  if (mesh >= scene.parts.size()) return {};
  const geometry::ClusterMeshPart& part = scene.parts[mesh];
  const Vector<u32>& ids = scene.lod.mesh.vertex_ids;
  if (part.vertex_id_source == geometry::VertexIdSource::none || ids.empty()) return {};
  // A part records where its vertices start, not how many it has: the next part's start is where
  // they end. The id stream's own length stands in for the last part's end, because a streamed
  // scene has released `vertices` and kept the ids.
  const u32 first = part.first_vertex;
  const u32 end = mesh + 1 < scene.parts.size() ? scene.parts[mesh + 1].first_vertex : ids.size();
  if (first > end || end > ids.size()) return {};
  return std::span<const u32>(ids.data() + first, end - first);
}

WorldPos instance_translation(const SceneInstance& source, const Mat4& fit) noexcept {
  // The fit's translation through the instance's rotation and scale, in the instance's own frame:
  // a float32 the size of the mesh. Then the world position in f64 (ADR-0053), where the old
  // float32 product rounded the sum at the size of the world coordinate.
  Transform3 local = source.transform;
  local.position = Vec3{};
  const Mat4 placed = mat4_from_transform(local) * fit;
  return source.origin + DVec3{source.transform.position} + DVec3{placed.c[3].xyz()};
}

void update_scene_bounds(SceneData& out) {
  // The plain heightfield keeps the half extent it has always used, so a run with no mesh gets
  // exactly the camera it always did.
  if (out.heightfield && out.instances.size() == 1) {
    out.center = WorldPos{};
    out.radius = 10.0f;
  } else {
    // **In a local frame, then placed in f64** (ADR-0053): the instances' centres measured from
    // the corner of the first instance's cell, so the box and the sphere are float32 numbers the
    // size of the scene wherever in the world it is, and a scene moved by whole cells gets the
    // same radius and a centre moved by exactly as much. A scene whose first instance is in the
    // cell at the origin is measured from the origin, as it always was.
    const WorldCell corner{out.instances.empty() ? Vec3i{} : out.instances[0].cell, Vec3{}};
    const WorldEye frame = to_eye(to_world(corner));
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    Vector<Vec3> centers(out.parts.size());
    Vector<f32> radii(out.parts.size(), 0.0f);
    for (u32 m = 0; m < out.parts.size(); ++m) {
      // `first_leaf_cluster` is not decoration: a **paged** mesh is ordered coarse to fine, so its
      // leaves are at the *end* of its run and `first_cluster` alone names the coarsest clusters
      // instead. Their spheres cover the same surface but not the same volume — a coarse cluster's
      // sphere is loose — so framing them gave a camera distance that depended on the shape of the
      // DAG. Two builds of one mesh that differ only in how far they could simplify then drew it
      // at two different sizes, which is how this was found: comparing a seam-aware LOD build
      // against a position-only one, the pictures disagreed by 0.23 FLIP at the *finest* threshold,
      // where they draw exactly the same triangles.
      mesh_bounds(out.lod, out.parts[m].first_cluster + out.parts[m].first_leaf_cluster,
                  out.parts[m].leaf_cluster_count, centers[m], radii[m]);
    }
    // A skinned instance's bounds are its bind pose's plus the displacement bound, for the same
    // reason the cull pass inflates its spheres: the camera has to frame the character wherever
    // the clip puts it, not only where it rests.
    auto instance_radius = [&](const gfx::InstanceDesc& instance) {
      return (radii[instance.mesh] + instance.bounds_padding) * instance.scale_max;
    };
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(gfx::instance_matrix(instance, frame), centers[instance.mesh]);
      const f32 r = instance_radius(instance);
      lo = Vec3{std::min(lo.x, c.x - r), std::min(lo.y, c.y - r), std::min(lo.z, c.z - r)};
      hi = Vec3{std::max(hi.x, c.x + r), std::max(hi.y, c.y + r), std::max(hi.z, c.z + r)};
    }
    const Vec3 center = (lo + hi) * 0.5f;
    out.radius = 1e-6f;
    for (const gfx::InstanceDesc& instance : out.instances) {
      const Vec3 c = transform_point(gfx::instance_matrix(instance, frame), centers[instance.mesh]);
      out.radius = std::max(out.radius, length(c - center) + instance_radius(instance));
    }
    out.center = absolute(to_world(frame), center);
  }
}

}  // namespace engine::renderer
