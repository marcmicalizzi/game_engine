#include "far_forest_build.h"
#include "scene_admission.h"
#include "scene_far.h"
#include "scene_flora.h"
#include "scene_ground_plan.h"
#include "source_images.h"
#include "texture_bound_state.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <core/schema/json_reflect.h>
#include <core/schema/json_text.h>
#include <core/time/time.h>
#include <domain/assets/mesh_generator.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/image_identities.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/flora_species.h>
#include <domain/geometry/stress_mesh.h>
#include <domain/materials/materials.h>
#include <domain/scene_gen/derived_root.h>
#include <domain/scene_gen/far_tile.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <domain/scene_gen/water_channel.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texel_density.h>
#include <domain/texture/texture_build.h>
#include <domain/texture/texture_file.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/far_tile.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/gpu_scene.h>  // k_max_pairs: the bound a scatter's count is held to
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_text.h>
#include <systems/renderer/scene_wind.h>
#include <systems/renderer/sway_forms.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/terrain_materials.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <renderer_log.h>
#include <scene_fire.h>
#include <scene_water.h>
#include <schemas/scene.h>
#include <thread>
#include <utility>

namespace engine::renderer {

namespace {

// How far off the ground an authored instance with `ground: false` may stand before the load
// counts it (T74): a tree's roots and a rock's burial are within a metre; past it the author
// either means it or the ground moved under an absolute `y`.
constexpr f64 k_off_ground_m = 1.0;

// **An authored instance over a metre above the ground** (renderer.md, "Instances off the
// ground"; need 153 (f)): floating unless a building stands under it. `held` is set when a
// placement entry says it occupies a disc or a polygon covering (x, z) — a house's footprint, whose
// floors a chest stands on and whose joists a flitch hangs from — and the read counts it in
// `instances_in_structures` instead. A band (a lane, a tread) holds nothing up.
struct Lifted {
  u32 instance = 0;
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 off_m = 0.0;
  bool held = false;
};

// Marks every lifted instance a disc or polygon of `shapes` covers; the count still unheld.
u32 hold_lifted(std::span<const scene_gen::Footprint> shapes, Vector<Lifted>& lifted) noexcept {
  u32 left = 0;
  for (Lifted& l : lifted) {
    for (u32 s = 0; s < shapes.size() && !l.held; ++s)
      l.held =
          shapes[s].shape != scene_gen::FootprintShape::band && shapes[s].covers_mm(l.x_mm, l.z_mm);
    left += l.held ? 0u : 1u;
  }
  return left;
}

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
  // The image's class hash (C64; its sidecar's, by name), which its key holds when it is not zero
  // (`content_class::classified_key`, C62's rule): zero for every image that declares nothing.
  u64 class_hash = 0;
};

// The key a reference's `.tex` is found by: its bytes and options, moved by its class only when it
// has one, so an unclassified image keeps the key and the entry it had.
u64 reference_key(u64 source_hash, const texture::TextureBuildOptions& options, u64 class_hash) {
  return content_class::classified_key(texture::texture_cache_key(source_hash, options),
                                       class_hash);
}

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

// `fn` once per item on the performance pool, the calling thread helping; returns when all are
// done. **One runner job per thread, each taking the next item by an atomic index**, never one job
// per item (T73, docs/experiments/load-v35-2026-11-08.md): the pool's queues hold 4,096 jobs each
// (`JobSystemConfig::queue_capacity`, the caller's guest deque and the inbox), and `schedule` runs
// a job that fits neither on the submitter, at once and before it wakes a sleeping worker — so the
// v35 forest's 16,351 meshes ran 8,159 of them one after another on the load thread, 27 ms each,
// 220 s, while every worker slept, and only then the 8,192 queued ones in parallel. Runners never
// overflow a queue whatever the item count, and an item still lands in its own slot, so nothing
// depends on the thread count or on which runner took which item (cooked_load_tests.cpp).
template <typename T>
void run_jobs(jobs::JobSystem& pool, std::span<T* const> items, void (*fn)(void*)) {
  if (items.empty()) return;
  struct Shared {
    std::span<T* const> items;
    void (*fn)(void*) = nullptr;
    std::atomic<u32> next{0};
  } shared;
  shared.items = items;
  shared.fn = fn;
  const auto runner = [](void* data) {
    Shared& s = *static_cast<Shared*>(data);
    for (u32 i = s.next.fetch_add(1, std::memory_order_relaxed); i < s.items.size();
         i = s.next.fetch_add(1, std::memory_order_relaxed))
      s.fn(s.items[i]);
  };
  const u32 runners = std::min<u32>(static_cast<u32>(items.size()), pool.total_worker_count() + 1);
  Vector<jobs::Job> list;
  list.reserve(runners);
  jobs::Counter counter;
  for (u32 r = 0; r < runners; ++r)
    list.push_back(jobs::Job{runner, &shared, &counter});
  pool.schedule(jobs::Pool::Performance, std::span<const jobs::Job>(list.data(), list.size()),
                counter);
  pool.wait(counter);
}
template <typename T>
void run_jobs(jobs::JobSystem& pool, Vector<T>& items, void (*fn)(void*)) {
  Vector<T*> pointers;
  pointers.reserve(items.size());
  for (T& item : items)
    pointers.push_back(&item);
  run_jobs<T>(pool, std::span<T* const>(pointers.data(), pointers.size()), fn);
}
// The same with no pool: every item on the calling thread, in order.
template <typename T>
void run_jobs(jobs::JobSystem* pool, Vector<T>& items, void (*fn)(void*)) {
  if (pool != nullptr) {
    run_jobs(*pool, items, fn);
    return;
  }
  for (T& item : items)
    fn(&item);
}

// `pool` null: a pass with no pool of its own (a tile's mesh, `load_placement_textures`) looks up
// and probes on the calling thread and makes a pool only when something has to be built.
void prepare_scene_textures(const SceneDesc& desc, Vector<SourceMesh>& sources,
                            content_build::ImageIdentities& identities, jobs::JobSystem* pool,
                            SceneLoadReport& report,
                            const Vector<Vector<u64>>* image_hash = nullptr,
                            const content_class::Categories* categories = nullptr) {
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
        if (image_hash != nullptr && m < image_hash->size() && i < (*image_hash)[m].size())
          ref.class_hash = (*image_hash)[m][i];
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
    ref.key = reference_key(file.hash, options, ref.class_hash);
  }
  // 1b. **The caps** (G14; texture.md, "Sized by the asset"): every reference to one texture — the
  // same bytes at the same options, so the same key — takes the largest need of the meshes that
  // name it, and a mesh that asks no cap needs the whole image. Nothing here runs for a scene that
  // states no density, so its keys are the ones they always were.
  bool any_cap = false;
  for (const SourceMesh& mesh : sources)
    any_cap = any_cap || !mesh.texture_caps.empty();
  if (any_cap) {
    HashMap<u64, u32> group_of;
    Vector<u32> need;  // per group: the largest needed side, 0 for the whole image
    Vector<u32> group_of_ref(refs.size(), ~0u);
    for (u32 r = 0; r < refs.size(); ++r) {
      const TextureRef& ref = refs[r];
      if (ref.key == 0) continue;
      const SourceMesh& mesh = sources[ref.mesh];
      const u32 cap = ref.image < mesh.texture_caps.size() ? mesh.texture_caps[ref.image] : 0u;
      const u32* found = group_of.find_value(ref.key);
      if (found == nullptr) {
        group_of.try_emplace(ref.key, need.size());
        group_of_ref[r] = need.size();
        need.push_back(cap);
        continue;
      }
      group_of_ref[r] = *found;
      u32& wanted = need[*found];
      wanted = (wanted == 0 || cap == 0) ? 0u : std::max(wanted, cap);
    }
    for (u32 r = 0; r < refs.size(); ++r) {
      if (group_of_ref[r] == ~0u || need[group_of_ref[r]] == 0) continue;
      TextureRef& ref = refs[r];
      texture::TextureBuildOptions options;
      (void)texture::unpack_texture_options(ref.options, options);
      options.needed_side = need[group_of_ref[r]];
      ref.options = texture::pack_texture_options(options);
      ref.key = reference_key(ref.source_hash, options, ref.class_hash);
      ++report.capped;
    }
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
          other.key = reference_key(hash, options, other.class_hash);
        }
      }
      if (ref.class_hash != 0) {
        // A classified image is built from its file, so the step reads its sidecar beside it and
        // writes the class into the texture's section 6 under the key the class moved (C62).
        source.file = files[ref.file].path;
      } else {
        source.bytes.assign(reinterpret_cast<const u8*>(bytes.data()),
                            reinterpret_cast<const u8*>(bytes.data()) + bytes.size());
        source.source_hash = hash;
      }
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
    // A build is the one step that wants threads; with no pool of the caller's, one of two for the
    // length of the step (a cold cache only: a warm one never gets here).
    std::optional<jobs::JobSystem> own;
    if (pool == nullptr) {
      jobs::JobSystemConfig config;
      config.performance_workers = 1;
      config.efficiency_workers = 1;
      own.emplace(config);
    }
    content_build::run_texture_step(
        std::span<const content_build::TextureSource* const>(pointers.data(), pointers.size()),
        std::span<const u32>(mesh_of.data(), mesh_of.size()), desc.ddc,
        pool != nullptr ? *pool : *own, results, categories);
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
    record.options = ref.options;  // a cap's, when 1b gave one
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
      log::field("capped", report.capped), log::field("from_index", report.images_from_index),
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
//
// **It records what it was built from** (T78): `source_hash` and `build_key` are the ones the entry
// is addressed by, which for a glTF at the default options are what `content_build::source_hash_of`
// and `mesh_build_key` give, so `engine-content build-all` finds the entry already the answer
// instead of rebuilding it (content_build.h, `run_mesh_task`). Until T78 it recorded neither.
bool write_cluster_cache(const std::string& path, const std::string& source,
                         const assets::MeshData& mesh_data, const Vector<i32>& part_material,
                         const Vector<u32>& part_of_cluster, geometry::ClusterLodMesh& lod,
                         geometry::ClusterPages& pages,
                         Vector<geometry::ClusterFileTexture>& textures, u64 source_hash,
                         u64 build_key, const Vector<u8>* classes = nullptr) {
  geometry::ClusterFileData data;
  // Section 54 (C62): the source's own class, as engine-content build writes it; none when null.
  if (classes != nullptr) data.content_classes = *classes;
  data.mesh = std::move(lod);
  // Section 39: an alpha-tested import's coverage errors, which the build left on the mesh in the
  // clusters' final order (`ClusterLodMesh::coverage`) and the caller keeps a copy of.
  data.lod_coverage = std::move(data.mesh.coverage);
  data.mesh.coverage = Vector<u32>();
  data.pages = std::move(pages);
  data.source_path = source;
  data.source_hash = source_hash;
  data.build_key = build_key;
  data.materials.reserve(mesh_data.materials.size() + 1);
  for (const assets::Material& source_material : mesh_data.materials) {
    // engine-content build's own conversion (G2), so the two write one record.
    data.materials.push_back(assets::cluster_file_material(source_material));
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
  // The second-UV slots, lights and cameras (G2), as engine-content build writes them.
  assets::write_import_sections(mesh_data, data);
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
  data.perches = mesh_data.perches;  // section 43 (A43): the perches the glTF declares, if any

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
    material.transmission = geometry::alpha_word_transmit(source.alpha);  // R164, foliage optics
    material.normal_spread = geometry::alpha_word_spread(source.alpha);
    out.data.materials.push_back(std::move(material));
    out.part_material.push_back(static_cast<i32>(out.part_material.size()));
    // How it places its texels (R131): stochastic or plain, which the upload reads.
    out.material_sampling_flags.push_back(
        geometry::cluster_material_sampling(container_data, m).flags);
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
  // What a host seats an instance of it by (G201 stage 2; section 42): none but a tree's.
  out.seat = container_data.seat;
  out.perches = std::move(container_data.perches);  // section 43 (A43): declared perches, if any
  out.doors = std::move(container_data.doors);      // section 53 (S91): declared leaves, if any
  // Its far form (G198; sections 44 to 48): none but a rigid mesh's whose build baked one.
  out.far_form = std::move(container_data.far_form);
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
  // A palette with a wind-sand layer draws the detail over that layer's share (R99, renderer.md
  // "Sand and snow"); any other palette and the detail stay alternatives.
  if (desc.terrain.materials.has_value() && desc.terrain.has_detail &&
      !terrain_materials_wind_sand(*desc.terrain.materials)) {
    error =
        "terrain.materials and terrain.detail are alternative surface-detail paths; name one, or "
        "give the palette a layer with \"response\": \"Sand\" of kind Wind";
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
  u64 key = 0;
  if (desc.cache) {
    key = geometry::cluster_cache_key(hash, lod_options, false,
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
  // **A hole's rim is locked** (terrain.md, "Holes"): every corner of a hole cell, so every level
  // of the DAG keeps the opening's edge where the finest draws it and a cave's collar meets it at
  // every cut. Only for a terrain with holes: every other builds as it did, to the byte.
  Vector<u8> locked;
  if (!desc.terrain.holes.empty()) {
    locked.assign(positions.size(), u8{0});
    const std::span<const Vec3> grid(positions.data(), positions.size());
    for (u32 zi = 0; zi + 1 < n; ++zi) {
      for (u32 xi = 0; xi + 1 < n; ++xi) {
        if (!terrain_grid_cell_is_hole(desc.terrain, grid, n, xi, zi)) continue;
        locked[zi * n + xi] = locked[zi * n + xi + 1] = 1;
        locked[(zi + 1) * n + xi] = locked[(zi + 1) * n + xi + 1] = 1;
      }
    }
    attribute_source.locked = std::span<const u8>(locked.data(), locked.size());
  }
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
  if (!cache_path.empty() &&
      write_cluster_cache(cache_path, "", out.data, out.part_material, out.part_of_cluster, lod,
                          built, out.textures, hash, key)) {
    out.container = cache_path;
  }
  if (desc.stream) pages = std::move(built);
  return true;
}

// Reads one mesh: a `.clusters` container named outright, the derived-data cache entry this
// glTF and these options address, or the glTF itself (imported, welded, clustered per primitive
// so every cluster has a single material, merged, and written into the cache for the next run).
// An empty `path` builds the scene's terrain when it has one and the procedural scene otherwise.
// `expected_hash`, when not zero, is the hash the file's bytes must have (a scene file's `hash`,
// `assets::source_bytes_hash`; for a generated mesh the generator's source hash).
// `identities` is the load's table of its source images, when it has one (load_scene's); its
// textures are not this function's — the load's pass builds or trusts them after every mesh is in.
bool load_source_mesh(const std::string& path, const SceneDesc& desc, u64 expected_hash,
                      SourceMesh& out, geometry::ClusterLodMesh& lod, geometry::ClusterPages& pages,
                      std::string& error, content_build::ImageIdentities* identities = nullptr,
                      u64 class_hash = 0, const Vector<u8>* class_record = nullptr) {
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
  u64 source_hash = 0;  // what a cache entry written below records it was built from (T78)
  u64 cache_key = 0;
  if (io::extension(path) == ".clusters") {
    container = path;
    out.cache = "file";
  } else if (desc.cache || expected_hash != 0) {
    profiling::TraceStage hash_stage("source hash");
    u64 bytes_hash = 0;  // what a pin names; `source_hash` is the cache's key, importer tags in
    if (!assets::source_mesh_hash(path, source_hash, bytes_hash, &error)) return false;
    hash_stage.end();
    out.source_hash = source_hash;
    // A scene that names a mesh by its bytes gets those bytes or nothing: a number measured on a
    // different file would still carry this scene's name (plan 09 §9.4, "content-addressed").
    // The bytes, not the key: an importer change moves the key and must not refuse the same file.
    if (expected_hash != 0 && bytes_hash != expected_hash) {
      error = path + " hashes to " + hash_hex(bytes_hash) + ", and the scene names " +
              hash_hex(expected_hash);
      return false;
    }
    if (desc.cache) {
      // The page target is part of the key, so this has to be the one the build below uses —
      // and the one engine-content uses by default, or the two apps would stop sharing entries.
      // An import's levels are built with the loose-island rule, here and in engine-content alike
      // (`geometry::imported_lod_options`), and the key says so.
      cache_key = geometry::cluster_cache_key(source_hash, geometry::imported_lod_options(desc.lod),
                                              true, geometry::ClusterPagesOptions{}.page_bytes);
      // A classified source's entry is its own (C62's rule, C64 at the load): the class by name,
      // only when there is one, so an unclassified glTF keeps the key it had.
      cache_key = content_class::classified_key(cache_key, class_hash);
      cache_path = geometry::cluster_cache_path(desc.ddc, cache_key);
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
  out.perches = out.data.perches;  // A43: the perches its nodes declare, as a container's 43
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
  // What each alpha-tested primitive's mask keeps, measured where `engine-content build` measures
  // it and by the same function, because the two build the same container (geometry.md, "Coverage
  // errors for an imported alpha-tested part").
  Vector<f32> mask_covers;
  content_build::measure_mask_covers(out.data, out.image_dir, mask_covers);
  Vector<geometry::ClusterLodMesh> parts;
  for (const assets::Primitive& primitive : out.data.primitives) {
    if (primitive.index_count < 3) continue;
    const std::span<const u32> range(out.data.indices.data() + primitive.first_index,
                                     primitive.index_count);
    geometry::ClusterLodOptions part_options = geometry::imported_lod_options(desc.lod);
    part_options.mask_cover =
        mask_covers[static_cast<u32>(&primitive - out.data.primitives.data())];
    geometry::ClusterLodMesh part;
    if (!geometry::build_cluster_lod(out.data.positions, range, part_options, part, &error,
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
    if (!lod.coverage.empty()) geometry::permute_cluster_array(source_of_cluster, lod.coverage);
  } else {
    ENGINE_LOG_WARN(log_renderer, "cluster pages not built", log::field("path", path),
                    log::field("error", page_error));
  }
  // The coverage errors this mesh draws by, as a container's section 39 would hand them over; the
  // cache write below takes the mesh's own copy.
  out.lod_coverage = lod.coverage;
  if (cache_path.empty()) lod.coverage = Vector<u32>();
  // Into the cache for the next run, as the same container engine-content build writes, the
  // source's identity included (T78): either app fills the cache, either app finds it, and
  // `build-all` takes the entry as current.
  if (!cache_path.empty()) {
    if (write_cluster_cache(cache_path, path, out.data, out.part_material, out.part_of_cluster, lod,
                            built, out.textures, source_hash, cache_key, class_record)) {
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
// rule"), are `placement_entries` (scene_ground_plan.h): the `ruins` entries first — each the
// placement generator "ruins" with the entry itself as its parameters, so every file written before
// the generic list reads as it did — then `placements`, each as it names itself.

// Appends a generator's meshes after everything the scene has so far, each once: a mesh an earlier
// entry appended — the same file under the same name, two entries of one kit — is that one, which
// is what reading each kit once did. Returns each of the generator's meshes as a scene mesh.
struct MeshKey {
  std::string path;
  std::string name;
  u64 hash = 0;
  // A mesh handed as its recipe (scene_gen.md, "A tile's meshes"): what it is, whatever its path.
  u64 identity = 0;
};
Vector<u32> append_placement_meshes(std::span<const scene_gen::PlacementMesh> meshes,
                                    Vector<std::pair<MeshKey, u32>>& seen, SceneDesc& out,
                                    std::string* clash = nullptr) {
  Vector<u32> index;
  index.reserve(static_cast<u32>(meshes.size()));
  for (const scene_gen::PlacementMesh& mesh : meshes) {
    u32 found = ~0u;
    // A recipe is one mesh however many placements, tiles or entries hand it, as it is one
    // resident mesh in a streamed world (`placement_mesh_identity`); a file is its path and name.
    const u64 identity = mesh.recipe.empty() ? 0 : placement_mesh_identity(mesh);
    for (const auto& [key, scene_mesh] : seen) {
      if (identity != 0 ? key.identity == identity
                        : key.identity == 0 && key.path == mesh.path && key.name == mesh.name &&
                              key.hash == mesh.hash) {
        found = scene_mesh;
        break;
      }
      // Two recipes under one path: the load finds a recipe by its path, so the second would be
      // built from the first's bytes. Said, not drawn wrong.
      if (identity != 0 && key.identity != 0 && key.path == mesh.path && clash != nullptr &&
          clash->empty()) {
        *clash = "two different recipes were handed as " + mesh.path +
                 ": a recipe's path names it, so each needs its own";
      }
    }
    if (found == ~0u) {
      found = out.meshes.size();
      out.meshes.push_back(mesh.path);
      if (identity != 0) out.recipes.push_back(MeshRecipe{mesh.path, mesh.recipe});
      SceneMeshInfo info;
      info.name = mesh.name;
      info.hash = mesh.hash;
      out.mesh_info.push_back(std::move(info));
      seen.push_back({MeshKey{mesh.path, mesh.name, mesh.hash, identity}, found});
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
                       std::span<const PlacementRef> entries, const TerrainSampler& ground,
                       SceneDesc& out, Vector<Lifted>& lifted, std::string& error) {
  if (entries.empty()) return true;
  u32 unheld = lifted.size();
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  // The world block a streamed entry is opened against: the file's, or the capability's defaults.
  const scene::WorldRings world = file.world.has_value() ? *file.world : scene::WorldRings{};
  LazyPool pool;
  scene_gen::Context context;
  context.world_seed = out.terrain.enabled ? out.terrain.seed : 0;
  context.tile_size = out.world.enabled ? out.world.tile_size : 32.0f;
  context.dir = dir;
  // **The generators' root, by the one rule** (T119; scene_gen.md, "The derived-data root"): the
  // host's (`SceneDesc::ddc`, set by both hosts before they read the file), else the scene's own;
  // and the same root carried on each streamed entry for every consumer that opens it again.
  const std::string root = scene_gen::host_derived_root(out.ddc, dir, true);
  context.ddc = root;
  if (out.terrain.enabled) context.ground = ground.provider().view();
  if (!out.terrain.exclusion.empty()) context.exclusion = &out.terrain.exclusion;
  context.site = &out.terrain.site;
  // The scene's formation, and the ground before its cuts when it has any (scene_gen.md, "The
  // formation and the ground before its cuts").
  if (out.terrain.formation.has_value()) context.formation = &*out.terrain.formation;
  UncutGround uncut(out.terrain);
  if (uncut.present()) context.uncut_ground = uncut.hook();
  context.jobs_fn = &LazyPool::get;
  context.jobs_context = &pool;
  Vector<std::pair<MeshKey, u32>> seen;
  // **Earlier placements** (ADR-0046 decision 4, as built 2026-10-12; scene_gen.md): every entry
  // read whole is handed the footprints of the entries read whole before it, in the scene's order —
  // where each placement stands and how far its mesh reaches on the ground, measured from the
  // mesh as loaded (`ground_radius`), once a mesh and only when a later entry is there to read it —
  // and, after them, the shapes the entry said it occupies (`hand_shapes_on`).
  scene_gen::FootprintList earlier;
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
    // **The path is the load's as it stands** (G210, 2026-11-17): every path in `out.meshes` is
    // resolved when it is put there — the file's own meshes and props against the scene's
    // directory as they are read, a generator's as its entry resolved them — so joining the
    // directory again named `<scene dir>/content/flora/...` for the village scene's cover and
    // every one of them went unmeasured. A mesh that cannot be read here is not this read's to
    // refuse — the load says why, with its own sentence — so its footprint is its point alone
    // until then.
    const std::string& mesh_path = out.meshes[mesh];
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
    context.earlier = earlier.span();
    // An earlier entry's shapes may have made an empty exclusion one to hand.
    if (!out.terrain.exclusion.empty()) context.exclusion = &out.terrain.exclusion;
    const scene_gen::PlacementGeneratorDesc* generator = registry.find_placement(entry.generator);
    if (generator == nullptr) {
      error = entry.where + " " + registry.unknown_placement(entry.generator);
      return false;
    }
    context.where = entry.where;
    // One stage an entry, its generator's name the detail, so a load's `placements` says which
    // generator its time went to (T73).
    ENGINE_STARTUP_STAGE_DETAIL("placement", entry.generator);
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
      if (generator->tile == nullptr) {
        error = entry.where + ": the placement generator \"" + entry.generator +
                "\" has no tiles, so a streamed world cannot hold it";
        return false;
      }
      // What the entry needs resident from the start, which may be nothing: a generator with no
      // `meshes` hands every mesh with the tile that names it (scene_gen.md, "A tile's meshes").
      Vector<scene_gen::PlacementMesh> resident;
      if (generator->meshes != nullptr && !generator->meshes(state, context, resident, &error))
        return false;
      StreamedPlacements streamed;
      streamed.generator = entry.generator;
      streamed.params = entry.params;
      streamed.dir = dir;
      streamed.where = entry.where;
      streamed.tile_meshes = generator->tile_meshes;
      streamed.tile_meshes_masked = generator->tile_meshes && generator->tile_meshes_masked;
      streamed.ddc = root;
      std::string clash;
      streamed.meshes = append_placement_meshes(
          std::span<const scene_gen::PlacementMesh>(resident.data(), resident.size()), seen, out,
          &clash);
      if (!clash.empty()) {
        error = entry.where + ": " + clash;
        return false;
      }
      // What it occupies under a lifted instance (`Lifted`): its tile asked only where one stands,
      // once a tile, so a scene with none asks nothing and the world builds every tile as before.
      const i64 tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
      Vector<scene_gen::TileCoord> asked;
      scene_gen::Placements here;
      for (u32 k = 0; k < lifted.size() && unheld > 0 && tile_mm > 0; ++k) {
        if (lifted[k].held) continue;
        const scene_gen::TileCoord at{
            static_cast<i32>(scene_gen::mask_floor_div(lifted[k].x_mm, tile_mm)),
            static_cast<i32>(scene_gen::mask_floor_div(lifted[k].z_mm, tile_mm))};
        bool seen_tile = false;
        for (const scene_gen::TileCoord& a : asked)
          seen_tile = seen_tile || (a.x == at.x && a.z == at.z);
        if (seen_tile) continue;
        asked.push_back(at);
        if (generator->occupies != nullptr && !generator->occupies(state, at)) continue;
        here.clear();
        std::string why;
        if (!generator->tile(state, at, context, here, &why)) continue;  // the world says why
        unheld = hold_lifted(here.shapes.span(), lifted);
      }
      out.streamed.push_back(std::move(streamed));
      continue;
    }
    scene_gen::Placements placed;
    if (!generator->expand(state, context, placed, &error)) return false;
    std::string clash;
    const Vector<u32> meshes = append_placement_meshes(
        std::span<const scene_gen::PlacementMesh>(placed.meshes.data(), placed.meshes.size()), seen,
        out, &clash);
    if (!clash.empty()) {
      error = entry.where + ": " + clash;
      return false;
    }
    // A recipe's reach on the ground is the generator's word (`PlacementMesh::reach`), as it is
    // for the same mesh handed with a tile, where nothing has been built when the next entry asks.
    for (u32 k = 0; k < placed.meshes.size(); ++k) {
      if (placed.meshes[k].recipe.empty()) continue;
      if (radius_of.size() <= meshes[k]) radius_of.resize(meshes[k] + 1, -1.0f);
      radius_of[meshes[k]] = placed.meshes[k].reach;
    }
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
      instance.handoff = p.handoff;  // the near and far forms' hand-off (G136)
      // A generator's placement stands on `context.ground`: the load seats it (G201 stage 2).
      instance.grounded = true;
      out.instances.push_back(instance);
    }
    // What the entries after this one are handed: only when there is one to hand it to.
    if (e + 1 < entries.size()) {
      ENGINE_STARTUP_STAGE("footprints");  // each placed mesh read once for its ground radius
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
        earlier.add_disc(scene_gen::nearest_mm(p.position.x), scene_gen::nearest_mm(p.position.z),
                         static_cast<f64>(radius * scale));
      }
    }
    // The shapes it occupies: into the exclusion, which the world's consumer and the collision are
    // handed too, and into what the entries after this one are handed.
    if (!hand_shapes_on(placed.shapes, entry.where, out, earlier, error)) return false;
    if (unheld > 0) unheld = hold_lifted(placed.shapes.span(), lifted);
    out.placed_buildings += placed.things;
    out.placed_instances += placed.instances.size();
  }
  if (out.world.enabled) {
    ENGINE_LOG_INFO(log_renderer, "placements streamed by the world", log::field("scene", path),
                    log::field("entries", entries.size()), log::field("meshes", seen.size()));
  }
  return true;
}

// A mesh's fit (`MeshFit`) from the box of its **source's surface**, which is its level 0
// clusters' vertices: the uniform scale and the point that goes to the origin. The one place the
// load computes it, for the instances' placement and for the authored instances' reach.
//
// It was the box of every level's vertices while those were a subset of the source's. They no
// longer are: a level that grows an island holds corners no source vertex has (geometry.md, "Loose
// islands: removed whole, the rest grown"), and fitted by that box the static world's lantern
// stood a tenth shorter than its 2.4 m the moment its parts were islands (T147: the first leg's
// pictures showed it and read it as a scene's difference). `first_cluster` and `cluster_count`
// name the mesh's run of `dag`.
struct FitParts {
  f32 scale = 1.0f;
  Vec3 anchor{};
};
FitParts fit_parts(const MeshFit& fit, const geometry::ClusterLodMesh& dag, u32 first_cluster,
                   u32 cluster_count) {
  Vec3 lo{1e30f, 1e30f, 1e30f};
  Vec3 hi{-1e30f, -1e30f, -1e30f};
  for (u32 c = first_cluster; c < first_cluster + cluster_count; ++c) {
    if (dag.lod[c].level != 0) continue;
    const geometry::ClusterDesc& cluster = dag.mesh.clusters[c];
    for (u32 v = 0; v < cluster.vertex_count; ++v) {
      const Vec3 p = dag.mesh.vertices[cluster.vertex_offset + v];
      lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
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

// ---- textures sized by the asset (G14; texture.md, "Sized by the asset") -----------------------

tunables::Float texture_texel_density{
    "renderer.textures.texel_density", 0.0, 0.0, 1'000'000.0,
    "Texels a metre the closest view of a scene's meshes resolves: a mesh's built textures that "
    "hold more than this at its largest instance leave their top levels out (0: no cap, every "
    "texture keeps its key; texture.md, \"Sized by the asset\")"};

// What the density measures read of a mesh: its materials as the container records them (with
// each slot's sampling, the transform among it) and every cluster's material, ~0 for none.
void density_inputs(const SourceMesh& mesh, const geometry::ClusterLodMesh& dag,
                    Vector<geometry::ClusterFileMaterial>& materials,
                    Vector<geometry::ClusterFileMaterialSampling>& sampling,
                    Vector<u32>& cluster_material) {
  materials.clear();
  sampling.clear();
  materials.reserve(mesh.data.materials.size());
  sampling.reserve(mesh.data.materials.size());
  for (const assets::Material& source : mesh.data.materials) {
    geometry::ClusterFileMaterial material;
    material.base_color_image = source.base_color_image;
    material.normal_image = source.normal_image;
    material.metallic_roughness_image =
        geometry::encode_optional_image(source.metallic_roughness_image);
    material.occlusion_image = geometry::encode_optional_image(source.occlusion_image);
    material.emissive_image = geometry::encode_optional_image(source.emissive_image);
    materials.push_back(material);
    sampling.push_back(
        geometry::encode_material_sampling(source.sampling, source.occlusion_strength));
  }
  cluster_material.assign(static_cast<u32>(dag.mesh.clusters.size()), ~0u);
  for (u32 c = 0; c < cluster_material.size(); ++c) {
    const u32 part = mesh.part_of_cluster[c];
    const i32 material = part < mesh.part_material.size() ? mesh.part_material[part] : -1;
    if (material >= 0) cluster_material[c] = static_cast<u32>(material);
  }
}

// The meshes something after the load may place (`cap_scene_textures`, below, says which and why):
// 1 in `open` for each, so neither the cap nor the bound (R172) sizes their textures from the
// load's instances.
void open_scene_meshes(const SceneDesc& desc, u32 count, Vector<u8>& open) {
  open.assign(count, u8{0});
  const auto open_mesh = [&](u32 m) {
    if (m < open.size()) open[m] = 1;
  };
  for (const StreamedPlacements& entry : desc.streamed)
    for (const u32 m : entry.meshes)
      open_mesh(m);
  for (const SceneProp& prop : desc.props)
    open_mesh(prop.mesh);
  for (const u32 i : desc.authored.keep)
    if (i < desc.instances.size()) open_mesh(desc.instances[i].mesh);
  for (u32 m = 0; m < desc.meshes.size(); ++m)
    if (desc.meshes[m].empty()) open_mesh(m);
}

// Each mesh's caps (`SourceMesh::texture_caps`) from the density the scene or the mesh states,
// the mesh's own measure (`texture::measure_cluster_density` over its level 0) and its largest
// instance's largest axis scale times its fit (texture.md, "Who states the density").
//
// **The scene's density caps only a mesh whose every placement the load knows**: one the scene's
// own instances place (its placement entries' expansions among them) and nothing after the load
// can place too. What the load is told can: a streamed placement entry's resident meshes
// (`SceneDesc::streamed`, placed by the world's tiles through the tail at whatever scale the
// generator picks), a prop (`SceneDesc::props`, a game module's `prop_place` takes a scale), an
// actor (`AuthoredStreaming::keep`, which `actor_scale` may grow up to four times), and a
// procedural mesh (an empty path: the terrain a world's ground tiles draw). Such a mesh is
// **open**: the scene's density does not cap it, since a placement at twice the scale would need a
// level the cap left out (review of G14, 2026-11-16). A mesh's own `Mesh.texel_density` is the
// author's word for that mesh and still caps it, read at its largest load-time instance's scale —
// for an open mesh never under 1, the mesh at its fitted size. A scene with no instances of its own
// (the load's default identity instances, or a grid) knows no placement at all and caps nothing.
void cap_scene_textures(const SceneDesc& desc, std::span<const geometry::ClusterLodMesh> dags,
                        Vector<SourceMesh>& sources) {
  const f64 scene_density = desc.texel_density >= 0.0f ? static_cast<f64>(desc.texel_density)
                                                       : texture_texel_density.get();
  const bool described = desc.mesh_info.size() == desc.meshes.size();
  bool any = scene_density > 0.0;
  for (u32 m = 0; described && m < desc.mesh_info.size(); ++m)
    any = any || desc.mesh_info[m].texel_density > 0.0f;
  if (!any || desc.instances.empty()) return;
  Vector<f64> scale(sources.size(), 0.0);
  for (const SceneInstance& instance : desc.instances) {
    if (instance.mesh >= scale.size()) continue;
    const Vec3 s = instance.transform.scale;
    const f64 largest =
        std::max(std::fabs(static_cast<f64>(s.x)),
                 std::max(std::fabs(static_cast<f64>(s.y)), std::fabs(static_cast<f64>(s.z))));
    scale[instance.mesh] = std::max(scale[instance.mesh], largest);
  }
  Vector<u8> open;
  open_scene_meshes(desc, sources.size(), open);
  for (u32 m = 0; m < sources.size() && m < dags.size(); ++m) {
    SourceMesh& mesh = sources[m];
    f64 density = open[m] != 0 ? 0.0 : scene_density;
    f64 at_scale = scale[m];
    if (described && desc.mesh_info[m].texel_density != 0.0f) {
      density = desc.mesh_info[m].texel_density > 0.0f
                    ? static_cast<f64>(desc.mesh_info[m].texel_density)
                    : 0.0;
      if (open[m] != 0) at_scale = std::max(at_scale, 1.0);
    }
    const geometry::ClusterLodMesh& dag = dags[m];
    if (!(density > 0.0) || !(at_scale > 0.0) || mesh.textures.empty() ||
        mesh.part_of_cluster.size() != dag.mesh.clusters.size()) {
      continue;
    }
    f64 fit = 1.0;
    if (described && desc.mesh_info[m].fit.active()) {
      fit = static_cast<f64>(
          fit_parts(desc.mesh_info[m].fit, dag, 0, static_cast<u32>(dag.mesh.clusters.size()))
              .scale);
    }
    Vector<geometry::ClusterFileMaterial> materials;
    Vector<geometry::ClusterFileMaterialSampling> sampling;
    Vector<u32> cluster_material;
    density_inputs(mesh, dag, materials, sampling, cluster_material);
    Vector<texture::ImageDensity> measured;
    texture::measure_cluster_density(dag, {cluster_material.data(), cluster_material.size()},
                                     {materials.data(), materials.size()},
                                     {sampling.data(), sampling.size()},
                                     static_cast<u32>(mesh.textures.size()), measured);
    mesh.texture_caps.assign(static_cast<u32>(mesh.textures.size()), 0u);
    for (u32 i = 0; i < measured.size(); ++i)
      mesh.texture_caps[i] = texture::needed_side(measured[i].uv_per_m, at_scale * fit, density);
  }
}

// **What the bound needs of each mesh** (R172 stage 1; texture.md, "Levels resident by what the
// frame samples"): its images' least densities over every cluster (`least_cluster_density`) and the
// sphere round its vertices. Only for a mesh with built textures that nothing after the load may
// place larger (`open_scene_meshes`): the bound is evaluated over the instances the scene holds,
// and a placement that arrives near an observer would want a level before a promotion could bring
// it. Such a mesh's textures, and any texture an open mesh shares, are uploaded whole.
void bound_scene_textures(const SceneDesc& desc, std::span<const geometry::ClusterLodMesh> dags,
                          Vector<SourceMesh>& sources) {
  if (!texture_bound_enabled()) return;
  Vector<u8> open;
  open_scene_meshes(desc, sources.size(), open);
  Vector<geometry::ClusterFileMaterial> materials;
  Vector<geometry::ClusterFileMaterialSampling> sampling;
  Vector<u32> cluster_material;
  for (u32 m = 0; m < sources.size() && m < dags.size(); ++m) {
    SourceMesh& mesh = sources[m];
    const geometry::ClusterLodMesh& dag = dags[m];
    if (open[m] != 0 || mesh.textures.empty() || mesh.texture_ddc.empty() ||
        mesh.part_of_cluster.size() != dag.mesh.clusters.size() || dag.mesh.vertices.empty()) {
      continue;
    }
    density_inputs(mesh, dag, materials, sampling, cluster_material);
    texture::least_cluster_density(
        dag, {cluster_material.data(), cluster_material.size()},
        {materials.data(), materials.size()}, {sampling.data(), sampling.size()},
        static_cast<u32>(mesh.textures.size()), mesh.texture_least_density);
    Vec3 lo = dag.mesh.vertices[0];
    Vec3 hi = lo;
    for (const Vec3& v : dag.mesh.vertices) {
      lo = Vec3{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
      hi = Vec3{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    const Vec3 centre{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
    f64 radius_sq = 0.0;
    for (const Vec3& v : dag.mesh.vertices) {
      const f64 dx = static_cast<f64>(v.x) - static_cast<f64>(centre.x);
      const f64 dy = static_cast<f64>(v.y) - static_cast<f64>(centre.y);
      const f64 dz = static_cast<f64>(v.z) - static_cast<f64>(centre.z);
      radius_sq = std::max(radius_sq, dx * dx + dy * dy + dz * dz);
    }
    // Rounded up by a part in a thousand: the sphere has to hold every vertex after f32 rounding.
    mesh.texture_sphere =
        Vec4{centre.x, centre.y, centre.z, static_cast<f32>(std::sqrt(radius_sq) * 1.001 + 1e-4)};
  }
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

// **Seats the instances standing on the ground by their meshes' seats** (G201 stage 2; renderer.md,
// "A tree seated by its footprint"; scene_gen/seat.h): every grounded instance whose mesh carries a
// seat and no fit, on the drawn ground — the scene's terrain with every edit planned, `terrain`,
// asked as the read asked it — with the scene's holes left out. After the meshes, whose containers
// say the seat, and before the authored entry and its forms take their instances, so a streamed
// tree is handed on seated and the world's tiles place it where a whole read does. A ground is
// made only when there is an instance to seat on it. Instances are independent of each other, so
// the order does not matter and nothing depends on the thread count: on a pool, blocks of a fixed
// size, each its own instances and its own counts, the counts summed in block order.

struct SeatBlock {
  std::span<SceneInstance> instances;
  std::span<const SourceMesh> sources;
  std::span<const SceneMeshInfo> info;
  scene_gen::Ground ground;
  const scene_gen::Exclusion* holes = nullptr;
  u32 seated = 0;
  u32 lowered = 0;
  f64 lowered_max_m = 0.0;
};

scene_gen::MeshSeat seat_of(const SceneInstance& instance, std::span<const SourceMesh> sources,
                            std::span<const SceneMeshInfo> info) {
  if (!instance.grounded || instance.mesh >= sources.size()) return scene_gen::MeshSeat{};
  if (instance.mesh < info.size() && info[instance.mesh].fit.active()) return scene_gen::MeshSeat{};
  return scene_gen::mesh_seat(sources[instance.mesh].seat);
}

void seat_block(void* data) {
  SeatBlock& b = *static_cast<SeatBlock*>(data);
  for (SceneInstance& instance : b.instances) {
    const scene_gen::MeshSeat seat = seat_of(instance, b.sources, b.info);
    if (!seat.valid()) continue;
    const scene_gen::SeatResult r = scene_gen::seat_on_ground(
        b.ground, b.holes, seat, instance.transform.scale, instance.origin);
    if (!r.seated) continue;
    instance.slope = r.slope;
    ++b.seated;
    if (r.lowered_m > 0.0) {
      ++b.lowered;
      b.lowered_max_m = std::max(b.lowered_max_m, r.lowered_m);
    }
  }
}

}  // namespace

void seat_scene_instances(const TerrainDesc& terrain, std::span<SceneInstance> instances,
                          std::span<const SourceMesh> sources, std::span<const SceneMeshInfo> info,
                          SceneLoadReport& report, jobs::JobSystem* pool) {
  if (!terrain.enabled) return;
  bool any = false;
  for (const SceneInstance& instance : instances)
    any = any || seat_of(instance, sources, info).valid();
  if (!any) return;
  ENGINE_STARTUP_STAGE("seats");
  const i64 start = time::monotonic_ns();
  const TerrainSampler ground(terrain);
  if (!ground.ok()) return;  // the read made this ground already, and said why it could not
  constexpr usize k_block = 1024;
  Vector<SeatBlock> blocks;
  blocks.reserve(static_cast<u32>((instances.size() + k_block - 1) / k_block));
  for (usize first = 0; first < instances.size(); first += k_block) {
    SeatBlock b;
    b.instances = instances.subspan(first, std::min(k_block, instances.size() - first));
    b.sources = sources;
    b.info = info;
    b.ground = ground.provider().view();
    b.holes = terrain.exclusion.empty() ? nullptr : &terrain.exclusion;
    blocks.push_back(b);
  }
  if (pool != nullptr && blocks.size() > 1) {
    run_jobs<SeatBlock>(*pool, blocks, &seat_block);
  } else {
    for (SeatBlock& b : blocks)
      seat_block(&b);
  }
  for (const SeatBlock& b : blocks) {
    report.seated += b.seated;
    report.lowered += b.lowered;
    report.lowered_max_m = std::max(report.lowered_max_m, b.lowered_max_m);
  }
  report.seat_ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  ENGINE_LOG_INFO(log_renderer, "instances seated by their footprints",
                  log::field("seated", report.seated), log::field("lowered", report.lowered),
                  log::field("lowered_max_m", report.lowered_max_m),
                  log::field("ms", report.seat_ms));
}

namespace {

// Hands the file's own rigid instances to the "authored" generator (authored.md) and makes each
// ring's distance form of their meshes (geometry.md, "Distance forms"): appended after every mesh
// the scene has, each a mesh of its own with its source's fit, materials and grid. Nothing happens
// unless the world streams, the rules allow it, the registry has the generator and the scene is not
// paged; what stays in the prefix is counted by why.
bool stream_authored(SceneDesc& resolved, Vector<SourceMesh>& sources,
                     Vector<geometry::ClusterLodMesh>& dags, bool paged, SceneData& out,
                     jobs::JobSystem* pool, std::string& error) {
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
      if (mf.active()) fit[m] = fit_parts(mf, dags[m], 0, dags[m].mesh.clusters.size());
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
  // The stages below name what "authored forms" spends before the far forest (W150: 29.5 s of the
  // forest staging's warm load in one unnamed run on the main thread).
  profiling::TraceStage sway_stage("authored sway");
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
  sway_stage.end();
  profiling::TraceStage rings_stage("authored rings");
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
          // Given back, not cleared (W51-3): a cleared Vector keeps its source's whole capacity.
          copy.part_of_cluster = Vector<u32>();
          copy.motion_vertices = Vector<geometry::ClusterFileMotionVertex>();
          for (const u32 c : map.clusters)
            copy.part_of_cluster.push_back(source.part_of_cluster[c]);
          copy.lod_coverage = std::move(coverage);
          if (source.motion_vertices.size() == dags[m].mesh.vertices.size()) {
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
  rings_stage.end();
  profiling::TraceStage set_stage("authored set");

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
  set_stage.end();
  // **The far forest** (renderer.md, "The far forest"; ADR-0059): the streamed instances' coarsest
  // forms merged per world tile, each tile a mesh after every other and an instance after the
  // prefix, which the renderer draws wherever the ring's tail does not draw that tile.
  if (rules.far_forest) {
    Vector<FarForestInstance> far;
    far.reserve(taken);
    for (u32 i = 0; i < count; ++i) {
      if (take[i] == 0) continue;
      const SceneInstance& instance = resolved.instances[first + i];
      far.push_back(FarForestInstance{instance.mesh, instance.transform, instance.origin});
    }
    Vector<FarForestFit> far_fits(meshes);
    for (u32 m = 0; m < meshes; ++m)
      far_fits[m] = FarForestFit{fit[m].scale, fit[m].anchor};
    FarForestOptions options;
    far_forest_tunables(options);
    options.tile_size = tile;
    options.ddc = resolved.ddc;
    options.cache = resolved.cache;
    options.pool = pool;
    ENGINE_STARTUP_STAGE("far forest");
    FarForestScene scene{resolved.meshes, resolved.mesh_info, sources,       dags,
                         out.form_source, out.form_rigid,     kept_instances};
    if (!build_far_forest(std::span<const FarForestInstance>(far.data(), far.size()),
                          std::span<const FarForestFit>(far_fits.data(), far_fits.size()), options,
                          scene, out.far_forest, error)) {
      return false;
    }
  }
  resolved.instances = std::move(kept_instances);
  StreamedPlacements entry;
  entry.generator = "authored";
  profiling::TraceStage params_stage("authored params");
  entry.params = schema::to_json(set);
  params_stage.end();
  entry.where = (resolved.name.empty() ? std::string("scene") : resolved.name) + ": instances";
  entry.meshes = std::move(resident);
  entry.motion_key_base = first;  // id n is instance first + n - 1 of the whole read
  entry.seated = true;     // the load seated every grounded one before handing it on (G201 stage 2)
  entry.own_forms = true;  // its meshes are its rings' forms already (G197: no table of them)
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

// **A placement's form by ring** (G197; ring_forms.h; renderer.md, "A placement's form by ring"):
// for every resident mesh of a streamed entry that does not pick its own forms, a cut of its DAG
// for each ring's floor — a mesh of its own after every other, with its source's fit, materials,
// textures and grid, as an authored form is — and the rule's choice per ring among the cuts, the
// forms the mesh offers (`offered`: a far form its container carries) and nothing. Only what any
// mesh has is read: its DAG, its size and what it offers. **A mesh that sways** takes a swaying
// cut in a ring where its motion is seen and a still one where it is not, by what its motion
// measures (G204; sway_forms.h; renderer.md, "A swaying mesh's form by ring"). A mesh that deforms
// any other way (a skin, morphs) keeps itself in every ring; so does every mesh of a paged scene,
// whose page table a form has none of.
bool make_placement_forms(SceneDesc& resolved, Vector<SourceMesh>& sources,
                          Vector<geometry::ClusterLodMesh>& dags, bool paged,
                          std::span<const OfferedForm> offered, SceneData& out,
                          std::string& error) {
  RingFormTable& table = out.ring_forms;
  table = RingFormTable{};
  if (!resolved.world.enabled || paged || resolved.streamed.empty()) return true;
  if (resolved.mesh_info.size() != resolved.meshes.size() ||
      dags.size() != resolved.meshes.size() || sources.size() != resolved.meshes.size()) {
    return true;
  }
  const AuthoredStreaming& lod_rules = resolved.authored;  // the host's projection and thresholds
  const f32 proj = lod_rules.proj_scale > 0.0f ? lod_rules.proj_scale : k_default_form_proj_scale;
  const RingFormRules rules = ring_form_rules(proj);
  if (!rules.enabled) return true;
  const u32 rings = std::min(world_ring_count(resolved.world), k_ring_forms_max);
  if (rings < 2) return true;
  f32 floor_m[k_ring_forms_max] = {};
  for (u32 r = 1; r < rings; ++r)
    floor_m[r] = ring_floor_m(resolved.world, r);
  // The subjects: every scene mesh an entry without forms of its own holds resident, once.
  const u32 meshes = resolved.meshes.size();
  Vector<u8> subject(meshes, u8{0});
  for (const StreamedPlacements& entry : resolved.streamed) {
    if (entry.own_forms) continue;
    for (const u32 m : entry.meshes)
      if (m < meshes) subject[m] = 1;
  }
  // `form_source` and `form_rigid` cover every mesh from here (the authored forms may have made
  // them already; the far forest grows them only when they are not empty).
  if (out.form_source.size() < meshes) {
    const u32 had = out.form_source.size();
    out.form_source.resize(meshes);
    for (u32 m = had; m < meshes; ++m)
      out.form_source[m] = m;
  }
  if (out.form_rigid.size() < meshes) out.form_rigid.resize(meshes, u8{0});
  // **A swaying mesh's forms** (G204; sway_forms.h): the rule's knobs, and the scene's largest
  // field root offset, mesh units, which the far field's rest takes every root of an instance to
  // lie within of its origin (`flora_field_rest_of`) — over every mesh a field instance may be of.
  const SwayFormRules sway_rules = sway_form_rules();
  f32 field_root = 0.0f;
  if (sway_rules.enabled && sway_rules.motion && sway_rules.field_rest_m > 0.0f) {
    for (u32 m = 0; m < meshes; ++m) {
      const SourceMesh& s = sources[m];
      if (!s.has_motion()) continue;
      FloraMesh field;
      if (!flora_mesh(std::span<const geometry::ClusterFileMotionPart>(s.motion_parts.data(),
                                                                       s.motion_parts.size()),
                      std::span<const geometry::ClusterFileStructuralPart>(
                          s.structural_parts.data(), s.structural_parts.size()),
                      s.species, field) ||
          !field.field) {
        continue;
      }
      for (const geometry::ClusterFileMotionPart& part : field.parts)
        field_root = std::max(field_root, length(part.pivot));
    }
  }
  Vector<MeshRingForms> chosen(meshes);
  for (u32 m = 0; m < meshes; ++m) {
    if (subject[m] == 0 || resolved.meshes[m].empty()) continue;
    const geometry::ClusterMesh& mesh = dags[m].mesh;
    if (mesh.clusters.empty() || !mesh.skin.empty() || !mesh.morph_channels.empty()) continue;
    // **A mesh moves when its motion contract makes a pose** (`flora_mesh`, which the flora stage
    // poses by): a sway or a field. A contract whose species never moves (the litter's, `none`)
    // is a rigid mesh, and is formed like one; so is every contract in a session that poses no
    // tail (`SwayFormRules::motion`). **One that sways is formed by what its motion measures**
    // (G204): with the rule off, or a contract that does not cover its vertices (the stage draws
    // that one rigid and says so), it keeps itself in every ring, as before.
    const SourceMesh& own = sources[m];
    FloraMesh pose;
    bool sways = false;
    if (own.has_motion()) {
      if (flora_mesh(std::span<const geometry::ClusterFileMotionPart>(own.motion_parts.data(),
                                                                      own.motion_parts.size()),
                     std::span<const geometry::ClusterFileStructuralPart>(
                         own.structural_parts.data(), own.structural_parts.size()),
                     own.species, pose)) {
        if (!sway_rules.enabled || own.motion_vertices.size() != mesh.vertices.size()) continue;
        sways = sway_rules.motion;
      }
    }
    // **A form is a static form only where a ray sees it** (renderer.md, "A placement's form by
    // ring"): a static form draws its frontier at every distance (the structures built once), so a
    // mesh drawn nearer as itself, at its own LOD, would change at the ring's edge. A mesh its
    // caster policy leaves untraced (the litter) gains nothing from a structure built once, and its
    // forms are cut forms the cull chooses over like the mesh, which hand over with no pixel.
    const geometry::FloraSpeciesInfo* policy = geometry::flora_species_info(own.species);
    const u8 static_form = policy == nullptr || policy->casters.traced ? 1u : 0u;
    // The mesh's frame as a placement places it (its fit), and how far it reaches from there.
    FitParts fit;
    const MeshFit& mf = resolved.mesh_info[m].fit;
    if (mf.active()) fit = fit_parts(mf, dags[m], 0, dags[m].mesh.clusters.size());
    f32 most = 0.0f;
    for (const geometry::ClusterDesc& c : mesh.clusters)
      most = std::max(most, length(c.center - fit.anchor) + c.radius);
    const f32 diameter_m = 2.0f * most * fit.scale;
    // What a swaying mesh's motion measures (G204): its reach at the rule's wind, the padding the
    // cull gives its instances, and per ring the largest placement scale that stands still there.
    SwayMeasure sway;
    f32 still_at[k_ring_forms_max] = {};
    u32 still_from = 0;
    bool was_still = false;
    const std::string own_name = resolved.mesh_info[m].name;
    if (sways) {
      sway = measure_sway(pose, std::span<const Vec3>(mesh.vertices.data(), mesh.vertices.size()),
                          std::span<const geometry::ClusterFileMotionVertex>(
                              own.motion_vertices.data(), own.motion_vertices.size()),
                          fit.scale, sway_rules);
    }
    // `mesh` and `own` are not read past here: a form pushed onto `dags` and `sources` may move
    // them.
    const u32 own_clusters = mesh.clusters.size();
    RingCut cuts[k_ring_forms_max];
    cuts[0] = RingCut{m, own_clusters};
    u32 drawn = m;
    u32 kept = own_clusters;
    for (u32 r = 1; r < rings; ++r) {
      if (floor_m[r] > 0.0f) {
        // **This ring's form stands still** when every placement its cut serves does, seen from
        // its floor (G204): the largest still scale, by the mesh's reach in pixels or a field's
        // rest distance, is at least the scale a cut is cut for. A ring has one form, so a ring
        // where only the smaller placements would stand still keeps the swaying cut for all of
        // them. The floors grow with the ring, so once still, still.
        if (sways)
          still_at[r] = still_scale(sway, floor_m[r], proj, field_root * fit.scale, sway_rules);
        const bool still = sways && still_at[r] >= rules.cut_scale;
        geometry::LodFormCut cut;
        cut.origin = fit.anchor;
        cut.distance = floor_m[r] / (fit.scale * rules.cut_scale);
        // A swaying mesh's cut is cut for the sway's padded spheres, as an authored tree's swaying
        // form is (`stream_authored`): its instances are padded in every form, the still one's
        // too (`SceneData::still_padding`), so the cull chooses over the form from this ring what
        // it chooses over the mesh.
        if (sways) cut.distance = std::max(0.0f, cut.distance - sway.padding);
        cut.proj_scale = proj;
        cut.threshold_px = lod_rules.threshold_px;
        cut.foliage_px = lod_rules.foliage_px;
        cut.fade = lod_rules.fade;
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
        // A cut that keeps what the ring inside it kept is that one (the cut only drops more as
        // the distance grows) — but the first ring that stands still, whose form drops the
        // contract whatever clusters it keeps (G204).
        const bool first_still = still && !was_still;
        if (map.clusters.size() < kept || first_still) {
          SourceMesh copy = source;
          // Given back, not cleared (W51-3): a cleared Vector keeps its source's whole capacity.
          copy.part_of_cluster = Vector<u32>();
          for (const u32 c : map.clusters)
            copy.part_of_cluster.push_back(source.part_of_cluster[c]);
          copy.lod_coverage = std::move(coverage);
          copy.motion_vertices = Vector<geometry::ClusterFileMotionVertex>();
          if (sways && !still) {
            // **A swaying cut** keeps its vertices' records and the parts they use, each with its
            // source's id, so it sways in its source's phase (`prune_form_parts`).
            for (const u32 v : map.vertices)
              copy.motion_vertices.push_back(source.motion_vertices[v]);
            prune_form_parts(copy);
          } else {
            copy.motion_parts.clear();
          }
          drawn = resolved.meshes.size();
          kept = map.clusters.size();
          std::string path = resolved.meshes[m];  // a copy: never push a Vector's own element
          resolved.meshes.push_back(std::move(path));
          SceneMeshInfo info = resolved.mesh_info[m];
          info.name += (still ? " (still form " : " (ring form ") + std::to_string(r) + ")";
          info.origin = "form";
          resolved.mesh_info.push_back(std::move(info));
          sources.push_back(std::move(copy));
          dags.push_back(std::move(form));
          out.form_source.push_back(m);
          // **A swaying mesh's form is never a static form** (G204): a static form draws its
          // frontier at every distance, which is not what the swaying ring beside it draws; the
          // resident cache gives a still instance its structures built once without it.
          out.form_rigid.push_back(sways ? u8{0} : static_form);
          ++table.cuts;
          table.cut_clusters += kept;
          if (still) {
            // The still form's instances keep the mesh's padding (`make_instance`).
            out.still_padding.resize(resolved.meshes.size(), 0.0f);
            out.still_padding[drawn] = sway.padding;
            out.still_lean.resize(resolved.meshes.size(), 0.0f);  // W154-2: how it leans
            out.still_lean[drawn] = sway.lean;
            out.still_wind = sway_rules.wind;
            ++table.stills;
          }
        }
        if (first_still) still_from = r;
        was_still = still;
      }
      cuts[r] = RingCut{drawn, kept};
    }
    chosen[m] = choose_ring_forms(m, own_clusters, diameter_m, std::span<const f32>(floor_m, rings),
                                  std::span<const RingCut>(cuts, rings), offered, rules);
    if (sways) {
      // A form the mesh offers is rigid: it is weighed only where the mesh stands still, and a
      // still entry serves only the scales that stand still there (a larger placement draws the
      // ring before's form, which sways).
      const MeshRingForms cut_only =
          choose_ring_forms(m, own_clusters, diameter_m, std::span<const f32>(floor_m, rings),
                            std::span<const RingCut>(cuts, rings), {}, rules);
      for (u32 r = 1; r < rings; ++r) {
        if (still_from == 0 || r < still_from) {
          chosen[m].ring[r] = cut_only.ring[r];
        } else if (chosen[m].ring[r].max_scale > still_at[r]) {
          chosen[m].ring[r].max_scale = still_at[r];
        }
      }
      chosen[m].still_from = static_cast<u8>(still_from);
      if (chosen[m].active) {
        ++table.swaying;
        ENGINE_LOG_INFO(log_renderer, "a swaying mesh's ring forms", log::field("name", own_name),
                        log::field("reach_m", static_cast<f64>(sway.reach_m)),
                        log::field("padding_m", static_cast<f64>(sway.padding * fit.scale)),
                        log::field("field", sway.field), log::field("still_from", still_from),
                        log::field("still_scale",
                                   static_cast<f64>(still_from != 0 ? still_at[still_from] : 0.0f)),
                        log::field("last_ring_scale", static_cast<f64>(still_at[rings - 1])));
      }
    }
    if (!chosen[m].active) continue;
    ++table.subjects;
    for (u32 r = 1; r < rings; ++r)
      table.offered += chosen[m].ring[r].kind == RingFormKind::offered ? 1u : 0u;
  }
  if (table.subjects == 0) return true;
  table.rings = rings;
  for (u32 r = 0; r < rings; ++r)
    table.floor_m[r] = floor_m[r];
  chosen.resize(resolved.meshes.size());  // the forms themselves draw as they are
  table.meshes = std::move(chosen);
  if (!out.still_padding.empty()) out.still_padding.resize(resolved.meshes.size(), 0.0f);
  if (!out.still_lean.empty()) out.still_lean.resize(resolved.meshes.size(), 0.0f);
  ENGINE_LOG_INFO(log_renderer, "placements' ring forms", log::field("meshes", table.subjects),
                  log::field("cuts", table.cuts), log::field("cut_clusters", table.cut_clusters),
                  log::field("offered", table.offered), log::field("rings", rings),
                  log::field("swaying", table.swaying), log::field("still_forms", table.stills),
                  log::field("still_px", static_cast<f64>(sway_rules.still_px)),
                  log::field("still_wind", static_cast<f64>(sway_rules.wind)),
                  log::field("proj_scale", static_cast<f64>(proj)),
                  log::field("min_px", static_cast<f64>(rules.min_px)),
                  log::field("cut_scale", static_cast<f64>(rules.cut_scale)));
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

namespace {

// The file's `water_channels`, read as a tree before it is mapped: dropped for the water's cook,
// which writes them (W120), and a block of versions 3 to 5 read through `WaterChannel`'s migration
// — a scene stores no version, so the block itself says which it is (water_channel.h).
bool prepare_water_channels(JsonValue& value, schema::ReadContext& ctx, void* without) {
  if (*static_cast<const bool*>(without)) value = JsonValue::array();
  JsonValue root = JsonValue::object();
  root.set("water_channels", std::move(value));
  const bool ok = scene_gen::migrate_scene_water_channels(root, ctx);
  value = std::move(*root.find("water_channels"));
  return ok;
}

}  // namespace

bool read_scene_text(std::string_view text, const std::string& path, bool without_water_channels,
                     scene::Scene& out, std::string& error) {
  // One grammar, and it is the schema's: an unknown field is refused with its path rather than
  // skipped, because a misspelt `"ground"` that quietly did nothing would float a building. Read
  // straight from the text, with no tree of the whole file (D40; core/schema/json_text.h): a forest
  // staging's file is 172 MB, and the tree path's verdicts are kept word for word.
  bool without = without_water_channels;
  const schema::TreeField channels{"water_channels", &prepare_water_channels, &without};
  schema::TextReadOptions options;
  options.tree_fields = std::span<const schema::TreeField>(&channels, 1);
  schema::ReadContext ctx;
  JsonParseResult parsed;
  profiling::TraceStage read_stage("schema read");
  const bool read = schema::read_json_text(out, text, ctx, parsed, options);
  read_stage.end();
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  if (!read || !ctx.ok()) {
    error = path + ": " + schema_errors(ctx);
    return false;
  }
  return true;
}

bool read_scene_props(const std::string& path, Vector<SceneProp>& out, std::string& error) {
  out.clear();
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = std::string("cannot read ") + path + ": " + io::status_name(status);
    return false;
  }
  // Straight from the text, with no tree of the whole file (D40).
  scene::Scene file;
  schema::ReadContext ctx;
  JsonParseResult parsed;
  const bool read = schema::read_json_text(file, text, ctx, parsed);
  if (!parsed.ok) {
    error = std::string(path) + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  if (!read || !ctx.ok()) {
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
  scene::Scene file;
  if (!read_scene_text(text, path, options.without_water_channels, file, error)) return false;
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
    // The scene's wind (gfx.md, "The wind a scene names"; W136), checked here so a wind nobody can
    // draw refuses the file.
    if (file.sky->wind.has_value() && !read_scene_wind(*file.sky->wind, path, out.wind, error))
      return false;
    // The swell arriving beside it (gfx.md, "A swell a scene names"; W136-3).
    if (file.sky->swell.has_value() &&
        !read_scene_swell(*file.sky->swell, path, out.wind.swell, error))
      return false;
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
    info.texel_density = mesh.texel_density;  // G14
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
    if (prop.collision.has_value()) {  // its body (P45; absent: its mesh)
      const scene::PropCollision& c = *prop.collision;
      PropCollider& collider = out.props.back().collider;
      collider.shape = c.shape == scene::PropCollisionShape::Box    ? PropShape::box
                       : c.shape == scene::PropCollisionShape::None ? PropShape::none
                                                                    : PropShape::mesh;
      collider.half_extents = c.half_extents;
      collider.center = c.center;
      const Vec3 h = c.half_extents;
      if (collider.shape == PropShape::box &&
          !(h.x > 0.0f && h.y > 0.0f && h.z > 0.0f && std::isfinite(h.x) && std::isfinite(h.y) &&
            std::isfinite(h.z) && std::isfinite(c.center.x) && std::isfinite(c.center.y) &&
            std::isfinite(c.center.z))) {
        error = where + " (\"" + prop.name +
                "\"): collision: a box's half_extents must each be "
                "above zero, and the box finite";
        return false;
      }
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
    out.terrain.formation = file.formation;
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
      if (!validate_terrain_formation(*out.terrain.materials, out.terrain.formation, &error)) {
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
      if (!out.ddc.empty()) tiles.ddc = out.ddc;  // the host's root wins (T90; tileset.md)
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
    // Holes in the ground (`Terrain.holes`, W87; terrain.md, "Holes"): compiled once here, read by
    // every level's mesh and by the collision.
    if (!out.terrain.holes.build({t.holes.data(), t.holes.size()}, &error)) {
      error = path + ": " + error;
      return false;
    }
    out.terrain.generator = t.generator == scene::TerrainGenerator::Dunes || t.provider == "dunes"
                                ? TerrainGenerator::dunes
                                : TerrainGenerator::waves;
    // The caller's ground time replaces the file's here, before anything below reads it: the
    // placement entries stand on the ground at this time, the mesh is built at it, and the sky's
    // one clock starts from it (renderer.md, "One world clock").
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
    if (out.terrain.has_detail && out.terrain.materials.has_value() &&
        !terrain_materials_wind_sand(*out.terrain.materials)) {
      error = path +
              ": terrain.materials and terrain.detail are alternative surface-detail paths; name "
              "one, or give the palette a layer with \"response\": \"Sand\" of kind Wind";
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
  // **Before it**, what every placement reads of the scene besides the ground (the exclusion, the
  // site), and what the placement entries plan for the ground — their edits and their openings,
  // added to the scene's own, so the ground made here is the edited one (scene_ground_plan.h).
  const Vector<PlacementRef> placements = placement_entries(file, path);
  if (!plan_scene_ground(file, path, dir, {placements.data(), placements.size()},
                         file.world.has_value() || options.world, out, error))
    return false;
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
  // The drawn ground and nothing on it (ground_sample.h; masks check). A placement entry this
  // executable does not carry planned nothing (`plan_scene_ground` asks only the ones it has), and
  // nothing expands here to refuse it: the caller names such entries (`SceneGround::not_carried`).
  if (options.ground_only) return true;
  // **The masks before the placements** (T122; tileset.md, "What a load refuses before it
  // builds"): the level cooked here, over the ground just made, so a tile naming more layers than
  // its budget or a level over `resident_mib` is refused now and not after every entry has been
  // expanded and the scene loaded. The upload asks again and finds it in the cache.
  if (out.terrain.enabled && !check_terrain_masks(out.terrain, ground.provider(), &error)) {
    error = path + ": " + error;
    return false;
  }
  // R185: the palette's share of the ground, which the ambient's lower half is weighed by
  // (`load_scene`; terrain_materials.h, `terrain_mask_cover`). A field the sampler never reads.
  if (out.terrain.enabled &&
      !terrain_mask_cover(out.terrain, ground.provider(), out.terrain.mask_cover, &error)) {
    error = path + ": " + error;
    return false;
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
  // **Every instance's ground, asked by place** (W150; renderer.md, "What the load reads"): the
  // same `ground_at_world` per instance, grounded or not, through `heights_by_place`, which asks in
  // the order of the places rather than the file's. The forest staging's 470,641 instances, in
  // their file's order, read 126,302 tiles of 18 KB through the `tiles` provider's cache of 3,737
  // (17.8 s of its warm load); by place they read the 53,086 the instances stand on. Each height is
  // the provider's answer to the bit; only the order of the questions changed.
  Vector<f32> instance_ground;
  if (out.terrain.enabled && !file.instances.empty()) {
    profiling::TraceStage heights_stage("instance heights");
    profiling::trace_items(file.instances.size());
    const u32 n = file.instances.size();
    Vector<i64> xs(n);
    Vector<i64> zs(n);
    for (u32 i = 0; i < n; ++i) {
      xs[i] = scene_gen::nearest_mm(file.instances[i].translation.x);
      zs[i] = scene_gen::nearest_mm(file.instances[i].translation.z);
    }
    instance_ground.resize(n);
    const std::span<const i64> x_mm(xs.data(), n);
    const std::span<const i64> z_mm(zs.data(), n);
    const std::span<f32> heights(instance_ground.data(), n);
    scene_gen::heights_by_place(ground.provider(), out.terrain.time_s, x_mm, z_mm, heights);
  }
  const auto instance_ground_at = [&](u32 i) {
    return out.terrain.enabled ? static_cast<f64>(instance_ground[i]) : 0.0;
  };
  // Grown once from the file's count: the array's growth was the read's peak (D40).
  out.instances.reserve(out.instances.size() + file.instances.size());
  Vector<Lifted> lifted;  // `ground: false` instances over a metre above the ground
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
    if (entry.ground) {
      instance.origin.y += instance_ground_at(i);
      instance.grounded = true;  // seated by the load when its mesh has a seat (G201 stage 2)
    } else if (out.terrain.enabled) {
      // T74: an instance placed at an absolute `y` over a ground that has moved since (the forest
      // proof of concept floated trees 26 m when its landform changed). Counted, not refused.
      const f64 off = entry.translation.y - instance_ground_at(i);
      if (off > k_off_ground_m) {
        // Floating only if no building stands under it: decided once the placements say what
        // they occupy (`expand_placements`; need 153 (f)).
        lifted.push_back(Lifted{i, scene_gen::nearest_mm(entry.translation.x),
                                scene_gen::nearest_mm(entry.translation.z), off, false});
      } else if (-off > k_off_ground_m) {
        if (out.instances_buried++ == 0) out.buried_first = i;
        if (-off > out.buried_max_m) out.buried_max_m = -off;
      }
    }
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
      // A factor on the animation tier's rule (A44; systems/animation/visibility.h), refused here
      // outside [0, 64] rather than clamped, so a typo is a sentence at the read.
      const f32 bias = entry.animation->lod_bias;
      if (!(bias >= 0.0f) || bias > 64.0f) {
        error = path + ": instance " + std::to_string(i) + ": animation.lod_bias " +
                std::to_string(bias) + " is outside [0, 64]";
        return false;
      }
      instance.animation.lod_bias = bias;
    }
    // A raise (Instance version 4; C64): kept by name and read at the load, once the scene's
    // containers have taught it their categories' ladders.
    if (entry.content_classes.has_value()) {
      if (out.raises.size() >= k_max_raises) {
        error = path + ": more than " + std::to_string(k_max_raises) + " instances raise a class";
        return false;
      }
      out.raises.push_back(*entry.content_classes);
      instance.content_classes = static_cast<u16>(out.raises.size());
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
    // A file's count sizes this read's instance list, so it is bounded before the first is placed
    // (T187): a scene names at most `k_max_pairs` (instance, cluster) pairs, the visibility id's
    // 2^24, and every instance is at least one, so instances past it could never be drawn.
    if (u64{out.instances.size()} + scatter.count > k_max_pairs) {
      error = path + ": scatter " + std::to_string(s) + " would place " +
              std::to_string(scatter.count) + " instances beside " +
              std::to_string(out.instances.size()) + "; a scene draws at most " +
              std::to_string(k_max_pairs) + " (instance, cluster) pairs";
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
      instance.grounded = scatter.ground;
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
  // The exclusion (G135) and the site (G144) they read were built before the ground was
  // (`plan_scene_ground`, above).
  profiling::TraceStage placements_stage("placements");
  if (!expand_placements(file, path, dir, {placements.data(), placements.size()}, ground, out,
                         lifted, error))
    return false;
  placements_stage.end();
  for (const Lifted& l : lifted) {
    if (l.held) {
      ++out.instances_in_structures;
      continue;
    }
    if (out.instances_floating++ == 0) out.floating_first = l.instance;
    if (l.off_m > out.floating_max_m) out.floating_max_m = l.off_m;
  }
  if (out.instances_floating + out.instances_buried > 0) {
    // One line a scene (T74): how many authored instances stand well off the ground they are
    // over, the worst of each and where the first is, so a landform that moved under a scene
    // whose instances are not relative is seen at the load and not in a picture. Those over a
    // building's footprint are said apart: they stand on its floors or hang from its timbers.
    const auto named = [&](u32 index) {
      return file.instances[index].name.empty() ? std::to_string(index)
                                                : file.instances[index].name;
    };
    ENGINE_LOG_WARN(
        log_renderer, "instances off the ground", log::field("floating", out.instances_floating),
        log::field("floating_max_m", out.floating_max_m),
        log::field("floating_first",
                   out.instances_floating > 0 ? named(out.floating_first) : std::string()),
        log::field("buried", out.instances_buried), log::field("buried_max_m", out.buried_max_m),
        log::field("buried_first",
                   out.instances_buried > 0 ? named(out.buried_first) : std::string()),
        log::field("in_structures", out.instances_in_structures),
        log::field("threshold_m", k_off_ground_m));
  }
  profiling::TraceStage water_stage("water");
  if (!read_scene_water(file, path, dir, ground, out, error)) return false;           // water.md
  if (!read_scene_water_channels(file, path, dir, ground, out, error)) return false;  // S96
  water_stage.end();
  if (file.instances.empty() && file.scatters.empty() && file.ruins.empty() &&
      file.placements.empty()) {
    for (u32 i = 0; i < file_meshes; ++i)
      out.instances.push_back(SceneInstance{i, Transform3::identity()});
  }
  // The leaves its meshes hang (S91; scene_collision.md, "What is drawn swings with what
  // collides"): after every instance the file and its entries placed, so theirs keep their places.
  if (!hang_scene_leaves(out, error)) {
    error = path + ": " + error;
    return false;
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
  // The default material pack is looked up whatever the root (materials.md, "Where the hosts find
  // it"; T103), and the load says which it used.
  if (resolved.materials.empty())
    resolved.materials = materials::default_pack_dir(platform::executable_directory());
  out.load.materials = resolved.materials;
  if (resolved.cache && resolved.ddc.empty()) {
    resolved.ddc = geometry::find_ddc_root(platform::executable_directory());
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
  // **Content classes, before any geometry is read** (C64; renderer.md, "Content classes at
  // load"): each mesh's own class, the scene's policy and table, and which meshes are read at all.
  LoadAdmission admission;
  if (!admission_before_meshes(resolved, out, admission, error)) return false;
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
  out.load.instances_floating = resolved.instances_floating;
  out.load.instances_in_structures = resolved.instances_in_structures;
  out.load.instances_buried = resolved.instances_buried;
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
      u64 class_hash = 0;                        // a classified glTF's, which its cache key holds
      const Vector<u8>* class_record = nullptr;  // and its section 54, if the load builds it
      bool ok = false;
      std::string error;
    };
    Vector<MeshJob> mesh_jobs(resolved.meshes.size());
    Vector<u32> wave(resolved.meshes.size(), 0u);
    // The first mesh of each path, by the path's hash (a collision compares the strings and falls
    // back to the scan): a scan of every earlier path was 134 million string compares for v35's
    // 16,351 meshes (T73).
    HashMap<u64, u32> first_of_path;
    for (u32 m = 0; m < resolved.meshes.size(); ++m) {
      MeshJob& job = mesh_jobs[m];
      job.desc = &resolved;
      job.path = &resolved.meshes[m];
      job.expected = described ? resolved.mesh_info[m].hash : 0;
      job.source = &out.sources[m];
      job.lod = &dags[m];
      job.pages = &tables[m];
      job.identities = &identities;
      job.class_hash = admission.mesh_hash[m];
      job.class_record = admission.record[m].empty() ? nullptr : &admission.record[m];
      // A mesh nothing under the policy draws is never read (its slot becomes a proxy box below).
      job.ok = admission.load[m] == 0;
      const std::string& path = resolved.meshes[m];
      if (path.empty()) continue;
      const u64 path_hash = hash_bytes(path.data(), path.size());
      const u32* first = first_of_path.find_value(path_hash);
      if (first == nullptr) {
        first_of_path.insert(path_hash, m);
      } else if (resolved.meshes[*first] == path) {
        wave[m] = 1;
      } else {
        for (u32 e = 0; e < m; ++e) {
          if (resolved.meshes[e] == path) {
            wave[m] = 1;
            break;
          }
        }
      }
    }
    const auto run = [](void* data) {
      MeshJob& job = *static_cast<MeshJob*>(data);
      ENGINE_STARTUP_STAGE_DETAIL("mesh", *job.path);
      job.ok =
          load_source_mesh(*job.path, *job.desc, job.expected, *job.source, *job.lod, *job.pages,
                           job.error, job.identities, job.class_hash, job.class_record);
    };
    ENGINE_STARTUP_STAGE("meshes");
    profiling::trace_items(resolved.meshes.size());
    for (u32 w = 0; w < 2; ++w) {
      // Through the runners (`run_jobs`, above), never a job a mesh: a world has more meshes than
      // the pool's queues hold.
      Vector<MeshJob*> list;
      for (u32 m = 0; m < mesh_jobs.size(); ++m)
        if (wave[m] == w && admission.load[m] != 0) list.push_back(&mesh_jobs[m]);
      run_jobs<MeshJob>(*pool, std::span<MeshJob* const>(list.data(), list.size()), run);
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
  // **Every instance through the one door** (C64), before a texture is read or a mesh merged: a
  // mesh nothing admitted draws leaves as its proxy box, its images with it. Free when nothing in
  // the scene is classified.
  admission_after_meshes(resolved, out, admission, dags, tables);
  // Every mesh's textures at once: one identity per distinct image, a built one trusted from its
  // header, the rest built on the same pool (`prepare_scene_textures`). Before the authored forms
  // below copy the records, so a form carries its mesh's keys. The caps a scene's density asks of
  // them first (G14), while the meshes' own levels are still at hand to be measured.
  cap_scene_textures(resolved, {dags.data(), dags.size()}, out.sources);
  bound_scene_textures(resolved, {dags.data(), dags.size()}, out.sources);  // R172
  prepare_scene_textures(resolved, out.sources, identities, &*pool, out.load, &admission.image_hash,
                         &out.categories);
  // The ground the hemisphere ambient's lower half carries: the mean of what the terrain draws
  // (R185; `terrain_ground_albedo`), or the neutral grey without one (renderer.md, "The ground's
  // albedo is a scene value"). Once, here, from what the scene already holds — **before the release
  // below**, which takes the terrain mesh's embedded coverage maps with every other built image's
  // bytes: measured after it, a cached load weighted each layer by its weight alone and a load
  // without the cache by its map too (the stack's review of R185 with W51-3).
  {
    const f32 grey = gfx::k_neutral_ground_albedo;
    out.ground_albedo = Vec3{grey, grey, grey};
    u32 terrain_mesh = ~0u;
    for (u32 m = 0; resolved.terrain.enabled && m < resolved.meshes.size(); ++m) {
      if (resolved.meshes[m].empty()) {
        terrain_mesh = m;
        break;
      }
    }
    if (resolved.terrain.enabled) {
      out.ground_albedo = terrain_ground_albedo(
          resolved.terrain,
          terrain_mesh < out.sources.size() ? &out.sources[terrain_mesh].data : nullptr);
    }
  }  // W51-3: what the built textures now stand in for goes, before the forms below copy the
     // meshes.
  const ImageBytesReleased released_images = release_built_image_bytes(out.sources);
  out.load.image_bytes_released_meshes = released_images.meshes;
  out.load.image_bytes_released = released_images.bytes;
  // Each mesh's path as the scene named it, for a sentence about it (a form copies its source's).
  for (u32 m = 0; m < out.sources.size() && m < resolved.meshes.size(); ++m)
    out.sources[m].path = resolved.meshes[m];
  // The trees standing on the ground, seated by their footprints (G201 stage 2), on the ground as
  // the file described it (`desc`: before a tile-drawn terrain's carrier grid is shrunk, above).
  seat_scene_instances(desc.terrain, {resolved.instances.data(), resolved.instances.size()},
                       {out.sources.data(), out.sources.size()},
                       {resolved.mesh_info.data(), resolved.mesh_info.size()}, out.load,
                       pool.has_value() ? &*pool : nullptr);
  // A streamed world's own instances, before the merge: their forms are meshes of their own
  // (renderer.md, "Authored instances per tile").
  {
    ENGINE_STARTUP_STAGE("authored forms");  // T73: the load's time between textures and merge
    bool paged_scene = resolved.stream;
    for (const geometry::ClusterPages& table : tables)
      paged_scene = paged_scene && !table.pages.empty();
    if (!stream_authored(resolved, out.sources, dags, paged_scene, out,
                         pool.has_value() ? &*pool : nullptr, error)) {
      return false;
    }
    // Every other streamed entry's meshes, a form a ring (G197). `offered` is where a form a mesh
    // carries for distance is handed to the rule (ring_forms.h, `OfferedForm`): each rigid resident
    // mesh's far form, a level a scene mesh of its own (G198; far_tile.h, renderer.md, "The far
    // tile").
    Vector<OfferedForm> offered;
    if (!offer_far_forms(resolved, out.sources, dags, paged_scene, out, offered, error))
      return false;
    if (!make_placement_forms(resolved, out.sources, dags, paged_scene,
                              std::span<const OfferedForm>(offered.data(), offered.size()), out,
                              error)) {
      return false;
    }
    tables.resize(dags.size());
  }
  out.load.inline_jobs = pool->stats().inline_runs;
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
  profiling::TraceStage merge_stage("merge");
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
  merge_stage.end();
  // **The per-mesh DAGs go as soon as the merge has copied them** (T73): nothing below reads them,
  // and held to the end of the load they were a second copy of every cluster, vertex and triangle
  // beside the merged one through the instance table — v35's working set peaked there, 4.3 GB above
  // what the load hands on.
  {
    ENGINE_STARTUP_STAGE("release meshes");
    dags = Vector<geometry::ClusterLodMesh>();
    tables = Vector<geometry::ClusterPages>();
  }

  // Instances: the caller's, the grid's, or one identity instance of the one mesh.
  Vector<SceneInstance> instances = std::move(resolved.instances);
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
  // The instances the load made itself are mesh 0's with no raise, decided once by the door (C64):
  // omitted, none is made; proxied, each draws mesh 0's proxy, which is mesh 0's own slot by then.
  if ((desc.instances.empty() || desc.grid_instances > 1) &&
      admission.mesh0 != content_class::Outcome::admit) {
    for (SceneInstance& made : instances) {
      made.joints = 0;
      if (admission.mesh0 == content_class::Outcome::omit)
        made.content_classes = k_instance_omitted;
    }
  }
  // Each fitted mesh's own transform, from the box of its source's surface: its level 0 clusters'
  // vertices (`fit_parts` has why not every level's). It is read here, while the merged float
  // positions still exist (a streamed scene releases them once its page source is attached).
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
      const FitParts parts =
          fit_parts(fit, out.lod, out.parts[m].first_cluster, out.parts[m].cluster_count);
      fit_of_mesh[m] =
          scaling(Vec3{parts.scale, parts.scale, parts.scale}) * translation(-parts.anchor);
    }
  }
  u32 pair_count = 0;
  const u32 palette = out.lod.mesh.skin_joint_count;
  const u32 handoff_band = sward_handoff_band_tunable();  // read once for the load (G136)
  ENGINE_STARTUP_STAGE("instances");
  profiling::trace_items(instances.size());
  // An omitted instance is left out of the table (C64): nothing of it, not its place, reaches the
  // scene. `kept_of` maps a prefix index to its loaded one when something was left out.
  Vector<u32> kept_of;
  for (u32 j = 0; j < instances.size(); ++j) {
    const SceneInstance& source = instances[j];
    if (source.content_classes == k_instance_omitted) {
      if (kept_of.empty()) {
        kept_of.reserve(instances.size());
        for (u32 k = 0; k < j; ++k)
          kept_of.push_back(k);
      }
      kept_of.push_back(k_not_loaded);
      continue;
    }
    if (!kept_of.empty()) kept_of.push_back(out.instances.size());
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
    apply_handoff(instance, source.handoff, handoff_band);
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
    // Its place among the prefix (`j`), not among the kept, so an omission elsewhere moves no other
    // instance's sway; the two are the same number when nothing was left out.
    out.motion_keys.push_back(source.motion_key != k_motion_key_load ? source.motion_key : j);
    out.ground_slopes.push_back(source.slope);  // the seat's plane (G201 stage 2)
    out.skinned_instances += joints > 0 ? 1u : 0u;
    out.instances.push_back(instance);
  }
  // The whole read's index follows the omissions (renderer.md, "The whole read's index"): a read
  // index whose instance was left out is not loaded, and every kept one's loaded index is its
  // place among the kept. A scene read whole gets the map only when something was left out.
  if (!kept_of.empty()) {
    if (out.loaded_of_read.empty()) {
      out.loaded_of_read = std::move(kept_of);
    } else {
      for (u32& loaded : out.loaded_of_read)
        if (loaded != k_not_loaded)
          loaded = loaded < kept_of.size() ? kept_of[loaded] : k_not_loaded;
    }
  }
  out.pair_count = pair_count;
  out.max_joints = resolved.max_joints;
  if (out.skinned_instances == 0) out.instance_joints.clear();
  out.mesh_fit = std::move(fit_of_mesh);
  // What a tile's mesh is built with after the load: the load's own options (R142).
  out.mesh_options =
      PlacementMeshOptions{resolved.lod, resolved.ddc, resolved.cache, resolved.materials};
  out.mesh_options.view_policy = out.instantiated_under;  // a tile's mesh is decided the same (C67)
  out.world = resolved.world;
  // Moved, not copied (W150): the authored entry's parameters are a tree of every instance it
  // streams (about 465,000 in the forest staging), and the copy and then the original's free at
  // the end of the load were 2 s of its "instances" stage and its tail.
  out.streamed = std::move(resolved.streamed);
  out.tile_meshes = false;
  out.tile_meshes_masked = false;
  for (const StreamedPlacements& entry : out.streamed) {
    out.tile_meshes = out.tile_meshes || entry.tile_meshes;
    out.tile_meshes_masked = out.tile_meshes_masked || entry.tile_meshes_masked;
  }
  // A streamed world's instances come and go — unless all it streams is its ground (the world's
  // tiles, which are terrain levels in fixed slots, not instances of the tail).
  out.dynamic = resolved.world.enabled && !(resolved.world.ground && out.streamed.empty());
  out.sky = resolved.sky;
  out.wind = resolved.wind;
  out.medium = resolved.medium;
  out.props = resolved.props;
  out.fires = resolved.fires;
  // The drawn leaves, by the loaded instances: a leaf whose house or own instance the load did not
  // keep is left out (S91).
  out.leaves.clear();
  for (const SceneLeaf& leaf : resolved.leaves) {
    SceneLeaf loaded = leaf;
    loaded.instance = loaded_instance(out, leaf.instance);
    loaded.house = loaded_instance(out, leaf.house);
    if (loaded.instance == k_not_loaded || loaded.house == k_not_loaded) continue;
    out.leaves.push_back(loaded);
  }

  update_scene_bounds(out);
  out.build_ns = time::monotonic_ns() - build_start;
  // What the load cost and what it made: an offscreen run has no other line that says it, and a
  // scene of hundreds of thousands of ruin blocks spends its time here, in the instance table.
  ENGINE_LOG_INFO(
      log_renderer, "scene loaded", log::field("ms", static_cast<f64>(out.build_ns) / 1.0e6),
      log::field("meshes", out.parts.size()), log::field("instances", out.instances.size()),
      log::field("pairs", out.pair_count), log::field("clusters", out.cluster_count()),
      log::field("ground_r", out.ground_albedo.x), log::field("ground_g", out.ground_albedo.y),
      log::field("ground_b", out.ground_albedo.z));
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
  const Mat4 fit =
      source.mesh < scene.mesh_fit.size() ? scene.mesh_fit[source.mesh] : Mat4::identity();
  place_instance(source, fit, first_pair, out);
  // A still form's instance is padded as its swaying source's are (G204;
  // `SceneData::still_padding`): the cull's choice over it is its choice over the mesh.
  if (source.mesh < scene.still_padding.size() &&
      scene.still_padding[source.mesh] > out.bounds_padding) {
    out.bounds_padding = scene.still_padding[source.mesh];
  }
  return true;
}

void place_instance(const SceneInstance& source, const Mat4& fit, u32 first_pair,
                    gfx::InstanceDesc& out) {
  out = gfx::InstanceDesc{};
  gfx::set_instance_placement(out, mat4_from_transform(source.transform) * fit,
                              instance_translation(source, fit));
  out.mesh = source.mesh;
  out.first_pair = first_pair;
  out.bounds_padding = source.bounds_padding;
  if (source.handoff != 0) apply_handoff(out, source.handoff, sward_handoff_band_tunable());
}

void apply_handoff(gfx::InstanceDesc& out, u8 handoff, u32 band) noexcept {
  const u32 role = handoff == scene_gen::k_handoff_near  ? gfx::k_instance_handoff_near
                   : handoff == scene_gen::k_handoff_far ? gfx::k_instance_handoff_far
                                                         : 0u;
  gfx::set_instance_handoff(out, role, band, 0);
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
    // **Each instance in its own cell, then everything in f64** (ADR-0053; T171): an instance's
    // centre is placed in float32 in the frame of its own cell's corner, which no other instance
    // decides, and the box, the centre and the sphere are taken in f64 from those. The centre and
    // the radius place the stand-in lights, so a rounding that depends on which instances a scene
    // holds moves the light: R164-3 framed the measure at the lowest cell the instances hold, which
    // made it the same in any order (impostor_view_tests.cpp, "the dither follows the
    // placement"), but a scene read whole then framed from another cell than the same scene
    // streamed in (its slab alone at the load), and was lit from a few micrometres away: one
    // pixel's colour on the Titan Xp (mesh_room_tests.cpp, "a streamed scene is the scene read
    // whole"). In f64 a min, a max and a sum of whole cells round nowhere a float can see, so
    // neither the order nor an instance inside the bounds moves them, and a scene moved by whole
    // cells gets the same radius and a centre moved by as much, to f64's precision.
    DVec3 lo{1e300, 1e300, 1e300};
    DVec3 hi{-1e300, -1e300, -1e300};
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
      return static_cast<f64>((radii[instance.mesh] + instance.bounds_padding) *
                              instance.scale_max);
    };
    auto instance_center = [&](const gfx::InstanceDesc& instance) {
      const WorldPos corner = to_world(WorldCell{instance.cell, Vec3{}});
      const Vec3 c =
          transform_point(gfx::instance_matrix(instance, to_eye(corner)), centers[instance.mesh]);
      return absolute(corner, c);
    };
    for (const gfx::InstanceDesc& instance : out.instances) {
      const WorldPos c = instance_center(instance);
      const f64 r = instance_radius(instance);
      lo = DVec3{std::min(lo.x, c.x - r), std::min(lo.y, c.y - r), std::min(lo.z, c.z - r)};
      hi = DVec3{std::max(hi.x, c.x + r), std::max(hi.y, c.y + r), std::max(hi.z, c.z + r)};
    }
    const WorldPos center{(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5};
    f64 radius = 1e-6;
    for (const gfx::InstanceDesc& instance : out.instances)
      radius =
          std::max(radius, length(instance_center(instance) - center) + instance_radius(instance));
    out.radius = static_cast<f32>(radius);
    out.center = center;
  }
}

// ---- a tile's mesh (renderer.md, "Meshes that come and go") -------------------------------------

// **A tile's mesh's textures, off the frame** (texture.md, "Textures that arrive after the load";
// R147): the load's own texture pass (`prepare_scene_textures`) run for the one mesh on the thread
// that built it — the same function over a list of one, so a tile's image gets the key, the trust
// and, on a cold cache, the build a whole read's would, and there is no second copy of the rule to
// drift — and then the `.tex` of every image a material samples, read whole unless the scene says
// it holds the key. All or none, as the upload's rule is: the first image without a built texture
// empties the list and says why.
void load_placement_textures(const SceneDesc& desc, const PlacementMeshOptions& options,
                             PlacementMeshData& out) {
  const u32 image_count = out.source.data.images.size();
  Vector<bool> sampled(image_count, false);
  u32 needed = 0;
  for (const assets::Material& material : out.source.data.materials) {
    for (const i32 image :
         {material.base_color_image, material.metallic_roughness_image, material.normal_image,
          material.occlusion_image, material.emissive_image}) {
      if (image >= 0 && static_cast<u32>(image) < image_count &&
          !sampled[static_cast<u32>(image)]) {
        sampled[static_cast<u32>(image)] = true;
        ++needed;
      }
    }
  }
  if (needed == 0) return;
  const auto refuse = [&](std::string why) {
    out.textures.clear();
    out.textures_why = std::move(why);
  };
  if (!desc.cache || desc.ddc.empty())
    return refuse("the load reads no derived-data cache, where built textures live");
  if (out.source.textures.size() != image_count)
    return refuse("the mesh records no built textures");
  {
    // **No pool of its own** (T129; docs/experiments/memory-commit-accounting.md, "Who made the
    // unnamed commit"): the look-up and the probes of a mesh's handful of images run on this thread
    // (it may be a worker of the host's pool, which must not wait on its own), and only a build on
    // a cold cache makes threads. A pool of two made and joined per mesh cost two threads a tile's
    // mesh, and every thread that names itself to the profiler keeps 4 MiB of commit until the
    // process ends: 6.2 GB on the forest's staging after its tiles came in twice.
    content_build::ImageIdentities identities(desc.ddc, materials::pack_at(desc.materials));
    Vector<SourceMesh> one;
    one.push_back(std::move(out.source));
    SceneLoadReport report;
    prepare_scene_textures(desc, one, identities, nullptr, report);
    out.source = std::move(one[0]);
  }
  for (u32 i = 0; i < image_count; ++i) {
    if (!sampled[i]) continue;
    const std::string name = "image " + std::to_string(i);
    const geometry::ClusterFileTexture& record = out.source.textures[i];
    if (record.options == 0 || record.key == 0)
      return refuse(name + " has no built texture (its record names none this build reads)");
    PlacementTexture texture;
    texture.image = i;
    texture.key = record.key;
    const bool held = options.texture_resident != nullptr &&
                      options.texture_resident(options.texture_context, record.key);
    if (!held) {
      std::string why;
      const std::string path = texture::texture_cache_path(desc.ddc, record.key);
      // Two tiles' meshes that name one new set are built at once on two workers, and both may
      // write the entry — the same bytes, each by a rename — so a read can meet the other's
      // rename on a file system that refuses one of the two. The entry is whole either way: ask
      // again, twice, before the mesh is left to draw its constants.
      bool read = texture::read_texture_file(path, texture.data, &why);
      for (u32 again = 0; !read && again < 2; ++again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        read = texture::read_texture_file(path, texture.data, &why);
      }
      if (!read) return refuse(name + ": " + why);
      if (texture.data.source_hash != record.source_hash || texture.data.build_key != record.key)
        return refuse(name + ": '" + path + "' was built from other bytes or options");
    }
    out.textures.push_back(std::move(texture));
  }
}

u64 PlacementTexture::staged_bytes() const noexcept {
  u64 bytes = 0;
  for (u32 l = 0; l < data.levels.size(); ++l)
    bytes += (data.level_bytes(l).size() + 15) / 16 * 16;
  if (!data.normal_variance.empty()) {
    for (u32 l = 0; l < data.levels.size(); ++l)
      bytes += (data.normal_variance_level_bytes(l).size() + 15) / 16 * 16;
  }
  return bytes;
}

bool read_placement_texture(const PlacementMeshOptions& options, u64 key, texture::TextureData& out,
                            std::string* error) {
  if (options.ddc.empty()) {
    if (error != nullptr) *error = "no derived-data root to read a texture from";
    return false;
  }
  if (!texture::read_texture_file(texture::texture_cache_path(options.ddc, key), out, error))
    return false;
  if (out.build_key != key) {
    if (error != nullptr) *error = "the texture file was built from other bytes or options";
    return false;
  }
  return true;
}

u64 PlacementMeshData::payload_bytes() const noexcept {
  const geometry::ClusterMesh& m = lod.mesh;
  return u64{m.clusters.size()} *
             (sizeof(geometry::ClusterDesc) + sizeof(geometry::ClusterLodDesc) + sizeof(u32)) +
         u64{m.vertices.size()} * (6 + sizeof(geometry::VertexAttributes) + sizeof(Vec3)) +
         u64{m.triangles.size()} * (sizeof(u32) + 3);
}

u64 placement_mesh_identity(const scene_gen::PlacementMesh& mesh) noexcept {
  u64 words[2];
  if (!mesh.recipe.empty()) {
    const std::string_view generator = assets::mesh_generator_of(mesh.path);
    words[0] = hash_bytes(generator.data(), generator.size());
    words[1] = hash_bytes(mesh.recipe.data(), mesh.recipe.size());
  } else {
    words[0] = hash_bytes(mesh.path.data(), mesh.path.size());
    words[1] = mesh.hash;
  }
  const u64 identity = hash_bytes(words, sizeof(words));
  return identity != 0 ? identity : 1;  // zero is "no recipe" to a reader
}

bool load_placement_mesh(const scene_gen::PlacementMesh& mesh, const PlacementMeshOptions& options,
                         PlacementMeshData& out, std::string& error) {
  out = PlacementMeshData{};
  if (mesh.path.empty()) {
    error = "a tile's mesh names no path";
    return false;
  }
  // A far tile (G198; far_tile.h) is its members' far forms, which only a host can build.
  if (scene_gen::is_far_tile(mesh)) return load_far_tile(mesh, options, out, error);
  // The load's own per-mesh pass (`load_source_mesh`), under the load's options: one cache entry
  // whichever of the two asked first, and the same bytes from either.
  SceneDesc desc;
  desc.lod = options.lod;
  desc.ddc = options.ddc;
  desc.cache = options.cache && !options.ddc.empty();
  desc.materials = options.materials;
  if (!mesh.recipe.empty()) desc.recipes.push_back(MeshRecipe{mesh.path, mesh.recipe});
  Vector<geometry::ClusterLodMesh> dags(1);
  geometry::ClusterPages pages;
  if (!load_source_mesh(mesh.path, desc, mesh.hash, out.source, dags[0], pages, error))
    return false;
  if (!dags[0].mesh.skin.empty() || !dags[0].mesh.morph_channels.empty()) {
    error = mesh.path + ": a mesh that arrives with a tile is drawn rigid, and this one is " +
            (dags[0].mesh.skin.empty() ? "morphed" : "skinned");
    return false;
  }
  // A whole read's merge puts each mesh's leaves first and reorders what is kept per cluster with
  // it (`load_scene`); the same here, so a cluster of this mesh is the cluster a whole read numbers
  // the same — which is what makes a streamed scene's ids the whole read's.
  Vector<geometry::ClusterMeshPart> parts;
  Vector<u32> source_of_cluster;
  if (!geometry::merge_cluster_meshes(
          std::span<const geometry::ClusterLodMesh>(dags.data(), dags.size()), out.lod, parts,
          &error, geometry::ClusterOrder::leaves_first, &source_of_cluster)) {
    error = mesh.path + ": " + error;
    return false;
  }
  out.part = parts[0];
  const std::span<const u32> order(source_of_cluster.data() + out.part.first_cluster,
                                   out.part.cluster_count);
  geometry::permute_cluster_array(order, out.source.part_of_cluster);
  if (!out.source.lod_coverage.empty())
    geometry::permute_cluster_array(order, out.source.lod_coverage);
  out.source.path = mesh.path;
  out.reach = ground_radius(out.lod, out.part.first_cluster, out.part.cluster_count);
  if (options.textures) load_placement_textures(desc, options, out);
  return true;
}

// A tile mesh from a container already in hand (G198; scene_far.h): the far tile's own
// derived-data entry, read back as `load_placement_mesh` reads any container.
bool placement_mesh_from_container(geometry::ClusterFileData& data, const std::string& where,
                                   const PlacementMeshOptions& options, PlacementMeshData& out,
                                   std::string& error) {
  out = PlacementMeshData{};
  SceneDesc desc;
  desc.lod = options.lod;
  desc.ddc = options.ddc;
  desc.cache = options.cache && !options.ddc.empty();
  desc.materials = options.materials;
  out.source.texture_ddc = desc.cache ? desc.ddc : std::string();
  Vector<geometry::ClusterLodMesh> dags(1);
  geometry::ClusterPages pages;
  adopt_container(data, where, desc, out.source, dags[0], pages);
  Vector<geometry::ClusterMeshPart> parts;
  Vector<u32> source_of_cluster;
  if (!geometry::merge_cluster_meshes(
          std::span<const geometry::ClusterLodMesh>(dags.data(), dags.size()), out.lod, parts,
          &error, geometry::ClusterOrder::leaves_first, &source_of_cluster)) {
    error = where + ": " + error;
    return false;
  }
  out.part = parts[0];
  geometry::permute_cluster_array(
      std::span<const u32>(source_of_cluster.data() + out.part.first_cluster,
                           out.part.cluster_count),
      out.source.part_of_cluster);
  out.source.path = where;
  out.source.cache = "hit";
  out.reach = ground_radius(out.lod, out.part.first_cluster, out.part.cluster_count);
  if (options.textures) load_placement_textures(desc, options, out);
  return true;
}

// ---- the ground before its cuts -----------------------------------------------------------------

struct UncutGround::State {
  TerrainDesc terrain;  // the scene's, less its cuts
  std::mutex mutex;
  std::unique_ptr<TerrainSampler> sampler;  // made on the first ask
};

UncutGround::UncutGround(const TerrainDesc& terrain) {
  present_ = terrain.enabled && terrain.tiles.has_value() && !terrain.tiles->edits.empty();
  if (!present_) return;
  state_ = std::make_unique<State>();
  state_->terrain = terrain;
  state_->terrain.tiles->edits.clear();
}

UncutGround::~UncutGround() = default;

scene_gen::Context::UncutGround UncutGround::hook() noexcept {
  scene_gen::Context::UncutGround out;
  if (present_) {
    out.fn = &UncutGround::get;
    out.context = state_.get();
  }
  return out;
}

bool UncutGround::get(void* context, scene_gen::Ground& out) {
  auto* state = static_cast<State*>(context);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->sampler == nullptr) state->sampler = std::make_unique<TerrainSampler>(state->terrain);
  if (!state->sampler->provider().valid()) return false;
  out = state->sampler->provider().view();
  return true;
}

}  // namespace engine::renderer
