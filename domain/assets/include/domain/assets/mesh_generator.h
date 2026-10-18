#pragma once

// Mesh generators (docs/subsystems/assets.md, "Generated meshes"; docs/subsystems/flora.md): the
// registry a scene reader finds a generated mesh's builder in, by the name its file gives.
//
// **Why a registry, and why here.** A scene's mesh table names glTF files and containers, and a
// generator's file beside them — `content/flora/date-palm.flora.json` — is a mesh too: parameters
// a capability's solver turns into one, built through the derived-data cache exactly as a glTF is
// (flora.md, "In a scene"). The renderer's scene reader is not a capability and may not link one
// (ADR-0027), so a generator registers a constant-initialized descriptor from its own source, a
// host links the capabilities its configuration has, and the reader finds the descriptor here by
// the name in the file's name. It is the scene-generator registry's pattern (ADR-0046) for a
// third thing a scene names, one level down: `scene_gen` makes instances of meshes, this makes a
// mesh. It lives in `assets` because that is where every other source of a mesh is — the glTF
// importer — and because the generators (the flora capability) sit just above it.
//
// **The name is in the file's name.** `<anything>.<name>.json` is generator `<name>`'s file:
// `date-palm.flora.json` is "flora"'s. Nothing else about the path is read, and a path with no
// second extension before `.json` names no generator. A build without the generator refuses such a
// path with a sentence naming it and the capability it belongs to (`unknown`), never by reading it
// as a glTF.
//
// **One build, two hosts.** `build` makes the whole container, so engine-content (which links the
// capability and calls it) and the renderer (which finds it here) write the same bytes under the
// same key, and either finds what the other built.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

struct MeshGeneratorDesc {
  // The generator, as its files' names give it: "flora".
  const char* name = "";
  // The source hash of a file's bytes: over what it says — canonicalized, so two files that say
  // the same thing share a key — and the generator's version. It is what
  // `geometry::cluster_cache_key` takes, as a glTF's `source_mesh_hash` is. False, with a
  // sentence, for bytes that are not a file this generator reads.
  bool (*source_hash)(std::span<const u8> bytes, u64& out, std::string& error) = nullptr;
  // Builds the container for a file's bytes under these LOD options and this page target (0: no
  // page table), recording the source hash, `cluster_cache_key(source hash, lod, false,
  // page_bytes)` as the build key, and what made it (`ClusterFileData::generator`). `source_path`
  // is left to the caller. False, with a sentence, when the file or its parameters are refused.
  bool (*build)(std::span<const u8> bytes, const geometry::ClusterLodOptions& lod, u32 page_bytes,
                geometry::ClusterFileData& out, std::string& error) = nullptr;
};

// The generator a path names: "flora" for ".../date-palm.flora.json"; empty for any other path.
std::string_view mesh_generator_of(std::string_view path) noexcept;

// Every generator this executable carries: the descriptors themselves, constant-initialized in
// their capabilities' sources and never copied. Filled before `main` by registrars.
class MeshGeneratorRegistry {
 public:
  MeshGeneratorRegistry() = default;
  MeshGeneratorRegistry(const MeshGeneratorRegistry&) = delete;
  MeshGeneratorRegistry& operator=(const MeshGeneratorRegistry&) = delete;

  static MeshGeneratorRegistry& global();

  // False, and nothing changes, for an incomplete descriptor or a second one under a name another
  // holds; the same descriptor again is true (a library linked into two images registers twice).
  bool add(const MeshGeneratorDesc& desc) noexcept;
  const MeshGeneratorDesc* find(std::string_view name) const noexcept;
  // The names, sorted: the same list whatever order the registrars ran in.
  Vector<std::string_view> names() const;
  // The sentence a reader refuses `path`, which names generator `name`, with: the generator, the
  // capability switch it comes with (`ENGINE_WITH_<NAME>`), and the generators this build has.
  std::string unknown(std::string_view path, std::string_view name) const;

 private:
  Vector<const MeshGeneratorDesc*> generators_;
};

// A capability's registration, at namespace scope beside the descriptor it adds:
//
//   constexpr assets::MeshGeneratorDesc k_flora{.name = "flora", .source_hash = ..., .build = ...};
//   const assets::MeshGeneratorRegistrar k_flora_registrar{k_flora};
//
// A module holding one is `WHOLE_ARCHIVE`, or the linker drops the object nothing references and
// the registration with it. A second descriptor under one name stops the program with a message.
struct MeshGeneratorRegistrar {
  explicit MeshGeneratorRegistrar(const MeshGeneratorDesc& desc) noexcept;
};

}  // namespace engine::assets
