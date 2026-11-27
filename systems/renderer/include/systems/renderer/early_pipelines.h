#pragma once

// **The resolve's pipeline is started when the scene file has been read** (roadmap R205, second
// leg; docs/subsystems/renderer.md, "The resolve compiles from the scene file"; the measurements,
// docs/experiments/resolve-cold-compile.md).
//
// On a driver cache that has not met the binary's resolve — a player's first launch, every
// driver update, every install of a binary whose shaders changed — the forest's resolve pipeline
// is the driver compiling one function of 326,000 instructions for about five minutes, with the
// process's commit up by about 6 GiB while it does (W156). `SceneRenderer::create` used to ask for
// it after the scene's upload, so those minutes came after everything else the load did. The
// variant is a function of the settings and of a few scene flags a scene file names before any
// mesh is read, so a host starts it on a worker as soon as the file is read, and the renderer's own
// creation takes the result: it waits for the one compile and never starts a second.
//
//     EarlyPipelines early;   // outlives the renderer that takes from it
//     early.start(device, predict_resolve_variant(desc, early_resolved, color_format), manifest);
//     ... load_scene, GpuScene::create ...
//     SceneRenderer::Desc rd;  rd.early = &early;  renderer.create(device, scene, ..., rd);
//
// **A wrong guess costs a compile, never a picture.** The early compile and the renderer's creation
// share one key (`ResolveKey`): the module's name and its SPIR-V's hash, the colour format, the
// foliage specialization and the bindless set's clamped capacity (`GpuScene::bindless_config`). A
// renderer whose key differs — the content had a flag the file did not name — makes its own
// pipeline as it always did, and the early one is destroyed untaken.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>
#include <domain/gfx/shader_library.h>
#include <systems/renderer/scene.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace engine::renderer {

struct RenderSettings;
struct ResolvedSettings;

// What selects the resolve's pipeline: the module (`resolve_module`, scene_renderer.cpp) by the
// shadow rays and the scene's three material flags, the foliage specialization (R177) and the
// target's colour format.
struct ResolveVariant {
  bool shadows = false;     // `ResolvedSettings::shadows`: the module with the shadow rays
  bool water = false;       // a water material (`GpuScene::has_water`)
  bool terrain = false;     // a layered ground's material (`GpuScene::has_terrain_materials`)
  bool stochastic = false;  // a stochastic mesh material (`GpuScene::has_stochastic_materials`)
  bool foliage = false;     // a foliage material (`GpuScene::has_foliage_materials`)
  bool light_cap = false;   // the cap on the shadow rays compiled in (R213, `scene_light_cap`)
  bool specular = false;    // a declared reflectance (R234, `GpuScene::has_specular_materials`)
  gfx::Format color_format = gfx::Format::R8G8B8A8Unorm;
  bool operator==(const ResolveVariant&) const noexcept = default;
};

// The variant a scene file names, read from its description before any mesh is loaded, under the
// settings resolved without the scene (`resolve_settings` with no `SceneData`):
//   - water: a mesh the file's water blocks made (`SceneMeshInfo::water`);
//   - terrain: the file's terrain with a `materials` block;
//   - foliage: the GPU scene's own mesh-room rule (`GpuScene::wants_mesh_room`, with
//     `world_is_dynamic`), or a pack role a recipe names whose set has foliage optics;
//   - stochastic: a recipe's `material.sampling` field naming `Stochastic`, a stochastic role a
//     recipe names, or — the one guess — a pack with a stochastic role where the file generates
//     meshes, since a generator that names no role takes the pack's defaults; and the scene's
//     `Mesh.sampling` (T150) over a mesh no file of which is read (`apply_mesh_sampling` over plain
//     materials). Over a mesh file, the scene's word is the worker's to apply, after its read;
//   - light_cap: the file's fires, lamps and props (`shadow_light_cap_compiled`), which the load
//     keeps as they are;
//   - specular: a generator's `tile_meshes_specular` under the mesh room (R234). What the mesh
//     files' own materials declare is not read here: `EarlyPipelines`' worker reads the files
//     (`mesh_files_to_read`, `read_mesh_declarations`) before it compiles, so the load starts at
//     once and the reads overlap it.
// What only the load can say (renderer.md, "The resolve compiles from the scene file", lists the
// cases) is a wrong guess at worst, and a wrong guess is one more compile, never a wrong pipeline.
// Touches no file.
ResolveVariant predict_resolve_variant(const SceneDesc& desc, const ResolvedSettings& resolved,
                                       gfx::Format color_format);

// **What a mesh file's own materials declare that selects the resolve** (R234; renderer.md, "The
// resolve compiles from the scene file"). One field a fact; a fact added is a field here, its lines
// in `read_mesh_declarations`, and a term in the worker's `known` test: the same files, the same
// one loop, never a second pass.
struct MeshDeclarations {
  bool specular = false;    // a material packs a reflectance other than the stand-in's (R234)
  bool stochastic = false;  // a material the load samples stochastically (T150; the module)
  bool operator==(const MeshDeclarations&) const noexcept = default;
};

// One mesh file the worker reads, with the scene's word over its materials (T150): the mesh's
// `SceneMeshInfo::sampling`, applied in order over what the file asks, as the load applies it.
struct MeshFileToRead {
  std::string path;
  Vector<SceneMeshInfo::MaterialSampling> sampling;
};

// Reads one mesh file only as far as its materials' declarations and ORs them into `out`: a
// `.gltf`'s JSON, a `.glb`'s JSON chunk, a `.clusters` container's sections 55 (the reflectance)
// and 33 (the sampling flags) with its material count, chosen by suffix without regard to case
// before the disk is touched. `stochastic` is the file's own ask — section 33's flag, or a glTF
// material's `extras.sampling` of "stochastic" in any case, as the import reads it (any other value
// is no request, and nothing refuses) — with `sampling` applied over it in order by
// `apply_mesh_sampling`; an entry the load would refuse declares nothing, and a file whose material
// count could not be learned has the scene's word applied over plain materials as many as its
// entries name plus one. True when the file was opened (what the startup trace counts). A file that
// is unreadable, truncated, of another container version, or whose lengths claim more than it holds
// declares nothing of its own, and nothing is allocated past its size: a wrong guess is one more
// compile.
bool read_mesh_declarations(const std::string& path, MeshDeclarations& out,
                            const Vector<SceneMeshInfo::MaterialSampling>& sampling = {});

// The files of `desc.meshes` the worker reads, each with its mesh's `sampling`: a `.gltf`, `.glb`
// or `.clusters` suffix in any case, less the procedural empty path and every path `desc.recipes`
// hands as bytes. Touches no file.
Vector<MeshFileToRead> mesh_files_to_read(const SceneDesc& desc);

// How many files `read_mesh_declarations` has opened in this process: for the cases' counts.
u64 mesh_declaration_reads() noexcept;

// The key the early compile and the renderer's creation share.
struct ResolveKey {
  std::string module;  // its name in the shader library
  u64 hash = 0;        // of its SPIR-V (`gfx::Shader::hash`): a `--shaders` manifest's bytes too
  gfx::Format color_format = gfx::Format::R8G8B8A8Unorm;
  bool foliage = false;
  bool light_cap = false;  // R213's specialization, which the file says exactly
  bool specular = false;   // R234's
  // The bindless set's capacity as the device clamped it: the pipeline layout is a function of it,
  // so a renderer whose scene's set has another shape never takes a pipeline made for this one.
  gfx::BindlessConfig layout;
  bool operator==(const ResolveKey&) const noexcept = default;
};

// The worker is a `std::thread` of its own, not a job on the job system's pools: it is one
// blocking driver call that lasts minutes cold, which would hold a pool's worker (and whatever the
// load queued behind it) for all of them. It touches nothing the caller's thread writes.
class EarlyPipelines {
 public:
  struct Stats {
    bool started = false;     // a compile was started
    bool done = false;        // it finished (made or failed)
    bool ok = false;          // it made a pipeline
    bool taken = false;       // a renderer took it
    u32 mismatched = 0;       // renderers that asked with another key, and made their own
    bool read = false;        // the worker's mesh-file reads are done: key and module are final
    u32 mesh_files = 0;       // files it was handed to read (`mesh_files_to_read`)
    u32 mesh_files_read = 0;  // files it opened before every fact was known
    f64 seconds = 0.0;        // the compile's wall time, on its worker
    ResolveKey key;
    // Why nothing was started, or what the last mismatched key differed in ("module", "hash",
    // "format", "foliage", "layout"), or why the compile failed; empty otherwise.
    std::string reason;
  };

  EarlyPipelines() noexcept = default;
  ~EarlyPipelines();
  ENGINE_NON_COPYABLE(EarlyPipelines);

  // Starts the resolve's one-draw pipeline for `variant` on a worker thread, from the module the
  // renderer would load (the embedded SPIR-V, or `shader_manifest`'s, which it reads the same way,
  // `SceneRenderer::Desc::shader_manifest`), with the bindless set's layout. False with `error`
  // when nothing was started: the device cannot draw the variant, or `renderer.resolve.classes` is
  // not `off` (the split's pipelines are other keys, made at creation). Once per object.
  // `mesh_files` (`mesh_files_to_read`) are read on the worker before it compiles, and what they
  // declare joins the key (`specular`, and `stochastic`, which picks the module: the worker takes
  // it from the library after the reads); a refused start reads none of them.
  bool start(const gfx::Device& device, const ResolveVariant& variant,
             const std::string& shader_manifest, std::string* error = nullptr,
             Vector<MeshFileToRead> mesh_files = {});

  // For `SceneRenderer`: whether a pipeline of `key` was started and not taken. Waits for the
  // worker's mesh-file reads (the key is not final before them), never for the compile.
  bool has(const ResolveKey& key) const;
  // Waits for the compile and hands its pipeline over (the caller owns it) when `key` is the one
  // started, then frees the early set and modules; false otherwise, and the caller makes its own.
  // Only another key is counted as a mismatch: nothing started, or taken already, is not one.
  bool take(const ResolveKey& key, gfx::PipelineHandle& out);

  // Waits for the worker and destroys a pipeline nobody took. The device must still be valid.
  void destroy() noexcept;

  Stats stats() const;

 private:
  void join() noexcept;
  // Under `mutex_`: until the worker's reads are done (at once when nothing was started).
  void wait_for_reads(std::unique_lock<std::mutex>& lock) const;

  const gfx::Device* device_ = nullptr;
  gfx::ShaderLibrary shaders_;
  // The default set's pipeline layout, which every `GpuScene`'s set is created identically to: a
  // pipeline made with it is used with the scene's (Vulkan's layout compatibility).
  gfx::BindlessSet bindless_;
  const gfx::Shader* module_ = nullptr;
  std::thread worker_;
  mutable std::mutex mutex_;
  mutable std::condition_variable read_cv_;  // `Stats::read`
  gfx::PipelineHandle pipeline_;
  std::string error_;
  Stats stats_;
};

// What both hosts call the moment the scene file is read: the settings resolved without the scene
// (`resolve_settings` with no `SceneData`), the variant the file names, and `start`. Never fails a
// load: what was not started is said in the log, and the renderer then makes its own.
void start_early_resolve(EarlyPipelines& early, const gfx::Device& device, const SceneDesc& desc,
                         const RenderSettings& settings, gfx::Format color_format,
                         const std::string& shader_manifest);

// The load summary's `early` block, engine-view's and `render.load`'s alike (apps.md): `started`,
// `done`, `ok`, `taken`, `mismatched`, `seconds` (the compile's, on its worker; 0 until done),
// `shader` (the module started) and `reason` (`Stats::reason`, when there is one). A null `early`
// (a mesh or a procedural scene, which starts nothing) is `{"started": false}`.
JsonValue early_summary(const EarlyPipelines* early);

}  // namespace engine::renderer
