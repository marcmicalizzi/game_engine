#pragma once

// The hosts a generated mesh grows over (`assets::MeshGeneratorHosts`; docs/subsystems/cover.md,
// "The host"): the one implementation of the seam both hosts of the generators use —
// engine-content's builds and the renderer's scene reader — so a vine built by either reads its
// host the same way and the two keep sharing cache entries.
//
// A dependant names its host by a path relative to its own file. Three kinds of host are read:
//
//   a generated mesh   `<name>.<generator>.json`: built in memory through its generator, under the
//                      dependant's own LOD options, page target and pack, and its finest level
//                      taken (`assets::host_mesh_of`); its identity its generator's source hash
//   a container        `.clusters`: read, and its finest level taken; its identity the hash it
//                      records
//   a glTF or GLB      imported (`assets::load_gltf`, the flattened default scene), every triangle
//                      holding, each its primitive's material roughness; its identity
//                      `assets::source_mesh_hash`
//
// A host that is itself a dependant (a cover over a vine) reaches its own host the same way, up to
// `k_host_depth` levels, so a loop of recipes is refused rather than followed.

#include <core/base/types.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_lod.h>

#include <string>
#include <string_view>

namespace engine::materials {
struct Pack;
}

namespace engine::content_build {

inline constexpr u32 k_host_depth = 4;

class GeneratorHosts {
 public:
  // `dependant` is the path of the file whose host paths are resolved against its directory.
  GeneratorHosts(std::string dependant, const geometry::ClusterLodOptions& lod, u32 page_bytes,
                 const materials::Pack* pack, u32 depth = 0);
  GeneratorHosts(const GeneratorHosts&) = delete;
  GeneratorHosts& operator=(const GeneratorHosts&) = delete;

  const assets::MeshGeneratorHosts* hooks() const noexcept { return &hooks_; }
  // The inputs a generator is called with: the pack and these hosts.
  assets::MeshGeneratorInputs inputs() const noexcept {
    return assets::MeshGeneratorInputs{pack_, &hooks_};
  }

  // The host path as a native path: absolute as written, else beside the dependant.
  std::string resolve(std::string_view path) const;

  bool identify(std::string_view path, u64& out, std::string& error) const;
  bool load(std::string_view path, assets::GeneratorHostMesh& out, std::string& error) const;

 private:
  std::string dependant_;
  geometry::ClusterLodOptions lod_;
  u32 page_bytes_ = 0;
  const materials::Pack* pack_ = nullptr;
  u32 depth_ = 0;
  assets::MeshGeneratorHosts hooks_;
};

}  // namespace engine::content_build
