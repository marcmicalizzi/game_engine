// A scene mesh a generator makes (scene_flora.h): the flora capability's palms first.
//
// The renderer names no generator: it finds one by the name in the file's name in the mesh
// generators' registry (domain/assets/mesh_generator.h), which a capability registers into from its
// own source and a host links (ADR-0027, the pattern ADR-0046 set for scene generators). So a
// build without the flora capability compiles this file, reads every other mesh as it always did,
// and refuses a `.flora.json` with the registry's sentence, which names the capability.
#include "scene_flora.h"

#include <core/log/log.h>
#include <core/profiling/startup_trace.h>
#include <domain/assets/mesh_generator.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>

#include <cstdio>
#include <renderer_log.h>
#include <span>

namespace engine::renderer {

bool is_generated_mesh(const std::string& path) { return !assets::mesh_generator_of(path).empty(); }

bool load_generated_mesh(const std::string& path, const SceneDesc& desc, u64 expected_hash,
                         geometry::ClusterFileData& data, std::string& container,
                         const char*& cache, u64& source_hash, std::string& error) {
  container.clear();
  cache = "none";
  const std::string_view name = assets::mesh_generator_of(path);
  const assets::MeshGeneratorRegistry& registry = assets::MeshGeneratorRegistry::global();
  const assets::MeshGeneratorDesc* generator = registry.find(name);
  if (generator == nullptr) {
    error = registry.unknown(path, name);
    return false;
  }
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + io::status_name(status);
    return false;
  }
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()), text.size());
  // A vine's or a cover's host (docs/subsystems/cover.md) is read through content_build's seam, the
  // one engine-content uses, under this scene's LOD options and pack: the two build one container.
  const u32 page_bytes = geometry::ClusterPagesOptions{}.page_bytes;
  profiling::TraceStage inputs_stage("generator inputs");
  const content_build::GeneratorHosts hosts(path, desc.lod, page_bytes,
                                            materials::pack_at(desc.materials));
  const assets::MeshGeneratorInputs inputs = hosts.inputs();
  std::string generator_error;
  if (!assets::generated_source_hash(*generator, bytes, inputs, source_hash, generator_error)) {
    error = path + ": " + generator_error;
    return false;
  }
  inputs_stage.end();
  // A scene that names a mesh by its bytes gets those bytes or nothing, as a glTF's does; for a
  // generated mesh the bytes are the parameters, canonicalized.
  if (expected_hash != 0 && source_hash != expected_hash) {
    char got[24];
    char want[24];
    std::snprintf(got, sizeof(got), "%016llx", static_cast<unsigned long long>(source_hash));
    std::snprintf(want, sizeof(want), "%016llx", static_cast<unsigned long long>(expected_hash));
    error = path + " hashes to " + got + ", and the scene names " + want;
    return false;
  }
  // The key `engine-content flora` writes under too, so either app finds what the other built —
  // with the material pack the parts fall back to in it, when there is one (materials.md).
  const u64 key = assets::generated_mesh_build_key(source_hash, inputs, desc.lod, page_bytes);
  std::string cache_path;
  if (desc.cache) {
    cache_path = geometry::cluster_cache_path(desc.ddc, key);
    cache = "miss";
    if (io::exists(cache_path)) {
      std::string read_error;
      profiling::TraceStage read_stage("cluster read");
      const bool read = geometry::read_cluster_file(cache_path, data, &read_error);
      read_stage.end();
      if (read && data.source_hash == source_hash && data.build_key == key) {
        cache = "hit";
        container = cache_path;
        // A recipe's image paths are relative to the recipe, which is this path whatever path the
        // host that wrote the entry knew it by.
        data.source_path = path;
        return true;
      }
      ENGINE_LOG_WARN(log_renderer, "cluster cache entry ignored", log::field("path", cache_path),
                      log::field("error", read_error.empty() ? "a different build" : read_error));
      // An accidental rebuild: an entry under this key that this build will not use (startup
      // trace, "accidental rebuilds"); the stage's detail says why.
      profiling::trace_end(profiling::trace_begin(
          "cache entry ignored", read_error.empty() ? std::string_view("a different build")
                                                    : std::string_view(read_error)));
    }
  }
  ENGINE_STARTUP_STAGE_DETAIL("generate", path);
  if (!generator->build(bytes, inputs, desc.lod, page_bytes, data, generator_error)) {
    error = path + ": " + generator_error;
    return false;
  }
  data.source_path = path;
  ENGINE_LOG_INFO(log_renderer, "mesh generated", log::field("path", path),
                  log::field("generator", generator->name), log::field("kind", data.generator_kind),
                  log::field("triangles", data.mesh.leaf_triangle_count),
                  log::field("clusters", data.mesh.mesh.clusters.size()));
  if (!cache_path.empty()) {
    std::string write_error;
    const io::Status made = io::make_directories(io::parent_path(cache_path));
    if (made != io::Status::Ok) {
      write_error = std::string("cannot create the directory: ") + io::status_name(made);
    } else if (geometry::write_cluster_file(cache_path, data, &write_error)) {
      container = cache_path;
    }
    if (container.empty()) {
      // A cache that cannot be written costs the next run a build and nothing else.
      ENGINE_LOG_WARN(log_renderer, "cluster cache not written", log::field("path", cache_path),
                      log::field("error", write_error));
    }
  }
  return true;
}

}  // namespace engine::renderer
