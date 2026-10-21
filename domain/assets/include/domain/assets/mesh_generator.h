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

namespace engine::materials {
struct Pack;
}

namespace engine::assets {

// What a host hands a generator besides the file's bytes: the material pack whose roles a recipe
// that names no set for a part falls back to (domain/materials, docs/subsystems/materials.md), or
// none. The host finds the pack — `<repo>/content/materials`, or a directory it was told — and
// reads it, so a generator stays a function of what it is given.
struct MeshGeneratorHosts;
struct MeshGeneratorInputs {
  const materials::Pack* materials = nullptr;
  // How a generator whose file names another mesh — a vine's or a cover's host
  // (docs/subsystems/cover.md) — reaches it; null where the host offers none, and such a
  // generator then refuses with a sentence. Every other generator ignores it.
  const MeshGeneratorHosts* hosts = nullptr;
};

// **A mesh another generated mesh grows over** (docs/subsystems/cover.md, "The host"): what a vine
// or a cover solver reads of its host — every triangle at full detail in the host's own mesh
// space, each with what its surface is — and nothing it would have to know the host's format for.
// A container's is its finest DAG level (`host_mesh_of`), a glTF's its flattened import; the host
// itself is never edited, and the dependant's mesh is placed with it (the same instance transform).
struct GeneratorHostMesh {
  Vector<Vec3> positions;  // the host's vertices: a container's cluster-ordered `Vertices`
  Vector<u32> indices;     // three per triangle
  // Per triangle: its material's roughness, 0 (polished) to 1, which is what says whether a root
  // climber's rootlets hold (cover.md, "Attachment").
  Vector<f32> roughness;
  // Per triangle: 1 where a climber may hold — all of a rigid mesh; of a generated tree, only the
  // triangles of the parts that stay colliders (its trunk, its load-bearing limbs; flora.md, "The
  // structural table"), so a vine never grows over a leaf card.
  Vector<u8> holds;
  // The host's source hash (a generator's canonical parameters, a glTF's bytes, a container's
  // recorded source), which a dependant's own source hash covers so a changed host is a new key.
  u64 identity = 0;
};

// The host's side of the seam: a context and two functions, so `assets` holds no file reader and
// the dependant names its host by the path its file wrote, relative to that file.
struct MeshGeneratorHosts {
  void* context = nullptr;
  // The host's identity alone (`GeneratorHostMesh::identity`), cheaply: what a source hash needs.
  bool (*identify)(void* context, std::string_view path, u64& out, std::string& error) = nullptr;
  // The host's triangles. False, with a sentence, for a path that names no mesh this host reads.
  bool (*load)(void* context, std::string_view path, GeneratorHostMesh& out,
               std::string& error) = nullptr;
};

// A container's finest level as a host (`GeneratorHostMesh`): its leaf clusters' triangles over
// its cluster-ordered vertices, each triangle's roughness its cluster material's, and `holds` from
// its motion contract and structural table where it carries them (a corner of part 0 or of a part
// flagged load-bearing or a collider holds; a triangle holds when all three do), else every
// triangle. `identity` is the container's recorded source hash, or its build key, or zero.
void host_mesh_of(const geometry::ClusterFileData& container, GeneratorHostMesh& out);

// The key a generated mesh's container is addressed by, which every host computes the same way:
// `geometry::cluster_cache_key(source_hash, lod, false, page_bytes)` — no weld, since a generator's
// vertices are shared where it means them — combined with the pack's key when the inputs carry a
// pack with roles, so a mesh built with a pack's defaults and one built without never share an
// entry, and a build with no pack keys exactly as it did before packs existed.
u64 generated_mesh_build_key(u64 source_hash, const MeshGeneratorInputs& inputs,
                             const geometry::ClusterLodOptions& lod, u32 page_bytes) noexcept;

struct MeshGeneratorDesc {
  // The generator, as its files' names give it: "flora".
  const char* name = "";
  // The source hash of a file's bytes: over what it says — canonicalized, so two files that say
  // the same thing share a key — and the generator's version. It is what
  // `geometry::cluster_cache_key` takes, as a glTF's `source_mesh_hash` is. False, with a
  // sentence, for bytes that are not a file this generator reads.
  bool (*source_hash)(std::span<const u8> bytes, u64& out, std::string& error) = nullptr;
  // Builds the container for a file's bytes and the host's inputs under these LOD options and this
  // page target (0: no page table), recording the source hash, `generated_mesh_build_key` as the
  // build key, and what made it (`ClusterFileData::generator`). `source_path` is left to the
  // caller; image paths a recipe names are relative to it, a pack's are absolute. False, with a
  // sentence, when the file or its parameters are refused.
  bool (*build)(std::span<const u8> bytes, const MeshGeneratorInputs& inputs,
                const geometry::ClusterLodOptions& lod, u32 page_bytes,
                geometry::ClusterFileData& out, std::string& error) = nullptr;
  // Optional: the source hash of a file whose mesh depends on another's (a vine's on its host):
  // over what `source_hash` covers **and** the host's identity through `inputs.hosts`, so a changed
  // host is a new key and never a stale vine. A host calls `generated_source_hash`, which takes
  // this when it is set and `source_hash` otherwise; `source_hash` alone answers without a host.
  bool (*source_hash_with)(std::span<const u8> bytes, const MeshGeneratorInputs& inputs, u64& out,
                           std::string& error) = nullptr;
};

// The source hash a host keys a generated mesh by: `desc.source_hash_with` with the inputs where
// the generator has it, `desc.source_hash` otherwise.
bool generated_source_hash(const MeshGeneratorDesc& desc, std::span<const u8> bytes,
                           const MeshGeneratorInputs& inputs, u64& out, std::string& error);

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
