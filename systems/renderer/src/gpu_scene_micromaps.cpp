// The opacity micromaps' bake at load, and its read back from the derived-data root (roadmap R120,
// R207; docs/subsystems/renderer.md, "Opacity micromaps" and "The bake is read back";
// systems/renderer/micromap_cache.h says what the key covers and why it names the device).

#include <core/log/log.h>
#include <core/profiling/startup_trace.h>
#include <core/time/time.h>
#include <domain/gfx/opacity_micromap.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/micromap_cache.h>

#include <algorithm>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// Where a bake's sections go: the file beside the clusters and the textures, written before the
// bake frees what it read them from. A failed write is a warning; the bake stands.
struct BakeStore {
  std::string path;
  u64 key = 0;
  bool stored = false;
  std::string error;
  static void take(void* context, const gfx::MicromapSections& sections) {
    auto& self = *static_cast<BakeStore*>(context);
    self.stored = write_micromap_file(self.path, self.key, sections, &self.error);
  }
};

}  // namespace

// **The opacity micromaps' bake** (gfx/opacity_micromap.h; renderer.md, "Opacity micromaps";
// R120): once, over the alpha-tested clusters `upload_materials` listed, blocking as every upload
// of a scene's creation is — or, since R207, read back from `<ddc>/micromaps/<key>.omm` when a
// load before this one on this device baked the same inputs. Then the rigid forms' static
// structures, built at create before the shaders existed, are built again naming the micromaps,
// before any frame references them.
bool GpuScene::bake_micromaps(gfx::ShaderLibrary& shaders, std::string* error) {
  if (micromaps_.address != 0 || micromap_list_.empty()) return true;
  ENGINE_STARTUP_STAGE("opacity micromaps");
  const i64 start = time::monotonic_ns();
  const gfx::Device& device = *device_;
  gfx::MicromapBakeInput in;
  in.clusters = clusters.address;
  in.triangles = triangles.address;
  in.attributes = attributes.address;
  in.materials = materials.address;
  in.items = std::span<const u32>(micromap_list_.data(), micromap_list_.size());
  in.triangle_counts = std::span<const u32>(micromap_triangles_.data(), micromap_triangles_.size());
  in.cluster_count = cluster_count_;
  u32 limit_2 = 0;
  u32 limit_4 = 0;
  gfx::opacity_micromap_limits(device, limit_2, limit_4);
  in.max_level = std::min(micromap_key_.level, limit_4);
  in.budget_bytes = micromap_key_.budget_bytes;
  in.target_texels = micromap_key_.texels;

  // The key: the device, the shader, the parameters, the list, the materials and the geometry.
  MicromapCacheReport& report = micromap_key_.report;
  const std::string& root = micromap_key_.root;
  report.unkeyed = micromap_key_.unkeyed;
  const gfx::Shader* shader = root.empty() ? nullptr : shaders.get("opacity_micromap_bake");
  if (!root.empty() && shader == nullptr) report.unkeyed = "the bake shader did not load";
  if (shader != nullptr) {
    MicromapCacheKeyInput key;
    std::string why;
    if (!gfx::micromap_device_identity(device, key.device) ||
        !gfx::micromap_geometry_digest(device, shaders, bindless_, in, key.geometry, &why)) {
      report.unkeyed = "the geometry digest failed: " + why;
    } else {
      key.shader = shader->hash;
      key.level_cap = in.max_level;
      key.device_limit = limit_4;
      key.budget_bytes = in.budget_bytes;
      key.target_texels = in.target_texels;
      key.cluster_count = in.cluster_count;
      key.items = in.items;
      key.triangle_counts = in.triangle_counts;
      key.materials = micromap_key_.materials;
      report.materials = key.materials;
      report.geometry[0] = key.geometry[0];
      report.geometry[1] = key.geometry[1];
      report.key = micromap_cache_key(key);
      report.path = micromap_cache_path(root, report.key);
    }
  }
  report.key_ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;

  if (report.key != 0) {
    const MicromapSceneShape shape{in.cluster_count, in.items, in.triangle_counts};
    const MicromapFileRead found =
        read_micromap_file(device, report.path, report.key, shape, micromaps_);
    report.file_bytes = found.file_bytes;
    if (found.used) {
      report.read = 1;
    } else if (found.found) {
      report.refused = found.refused;
      ENGINE_LOG_WARN(log_renderer, "stored opacity micromaps refused",
                      log::field("path", std::string_view{report.path}),
                      log::field("reason", std::string_view{found.refused}));
    }
  }
  if (report.read == 0) {
    BakeStore store;
    store.path = report.path;
    store.key = report.key;
    gfx::MicromapSink sink;
    if (report.key != 0) {
      sink.take = &BakeStore::take;
      sink.context = &store;
    }
    if (!gfx::bake_opacity_micromaps(device, shaders, bindless_, in, sink, micromaps_, error)) {
      return false;
    }
    report.baked = 1;
    report.stored = store.stored;
    if (report.key != 0 && !store.stored) {
      ENGINE_LOG_WARN(log_renderer, "opacity micromaps not stored",
                      log::field("path", std::string_view{report.path}),
                      log::field("reason", std::string_view{store.error}));
    } else if (store.stored) {
      io::FileInfo info;
      if (io::stat_file(report.path, info) == io::Status::Ok) report.file_bytes = info.size;
    }
  }
  report.ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  micromap_list_ = Vector<u32>{};
  micromap_triangles_ = Vector<u32>{};
  const gfx::MicromapStats& s = micromaps_.stats;
  u32 entries = 0;
  // Per level, `level:triangles wanting it`, `level:triangles/stored entries`, and
  // `level:unknown/micro-triangles` (R120 stage 3's histogram), as text for the log line.
  std::string wanted;
  std::string stored;
  std::string unknown;
  for (u32 l = 0; l <= gfx::k_micromap_max_level; ++l) {
    entries += s.entries_per_level[l];
    if (s.wanted_per_level[l] != 0)
      wanted += std::to_string(l) + ":" + std::to_string(s.wanted_per_level[l]) + " ";
    if (s.entries_per_level[l] != 0)
      stored += std::to_string(l) + ":" + std::to_string(s.entries_per_level[l]) + "/" +
                std::to_string(s.unique_per_level[l]) + " ";
    if (s.micro_per_level[l] != 0)
      unknown += std::to_string(l) + ":" + std::to_string(s.unknown_per_level[l]) + "/" +
                 std::to_string(s.micro_per_level[l]) + " ";
  }
  ENGINE_LOG_INFO(
      log_renderer, report.read != 0 ? "opacity micromaps read" : "opacity micromaps baked",
      log::field("triangles", s.triangles), log::field("with_data", entries),
      log::field("unique_entries", s.unique_entries),
      log::field("dedup_mismatches", s.dedup_mismatches),
      log::field("fully_opaque", s.special_opaque),
      log::field("fully_transparent", s.special_transparent),
      log::field("fully_unknown", s.special_unknown), log::field("micro_unknown", s.micro_unknown),
      log::field("micro_unknown_filter", s.micro_unknown_filter),
      log::field("micro_known", s.micro_opaque + s.micro_transparent),
      log::field("level_cap", s.level_cap), log::field("levels_dropped", s.levels_dropped),
      log::field("wanted", std::string_view{wanted}),
      log::field("stored", std::string_view{stored}),
      log::field("unknown", std::string_view{unknown}), log::field("data_bytes", s.data_bytes),
      log::field("resident_bytes", micromap_bytes()), log::field("bake_ms", s.bake_ms),
      log::field("stage_ms", report.ms), log::field("key_ms", report.key_ms),
      log::field("key", report.key), log::field("key_materials", report.materials),
      log::field("key_geometry", report.geometry[0] ^ report.geometry[1]),
      log::field("file_bytes", report.file_bytes), log::field("written", report.stored),
      log::field("unkeyed", std::string_view{report.unkeyed}));
  if (micromaps_.address != 0 && static_build_ && static_clusters_ > 0) {
    gfx::destroy_cluster_set(device, static_clas_);
    gfx::destroy_cluster_blas_set(device, static_blas_);
    if (!build_static_forms(error)) return false;
  }
  return true;
}

}  // namespace engine::renderer
