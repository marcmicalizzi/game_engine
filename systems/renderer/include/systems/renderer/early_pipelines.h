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
// share one key (`ResolveKey`): the module's name and its SPIR-V's hash, the colour format and the
// foliage specialization. A renderer whose key differs — the content had a flag the file did not
// name — makes its own pipeline as it always did, and the early one is destroyed untaken.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>
#include <domain/gfx/shader_library.h>

#include <mutex>
#include <string>
#include <thread>

namespace engine::renderer {

struct SceneDesc;
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
  gfx::Format color_format = gfx::Format::R8G8B8A8Unorm;
  bool operator==(const ResolveVariant&) const noexcept = default;
};

// The variant a scene file names, read from its description before any mesh is loaded, under the
// settings resolved without the scene (`resolve_settings` with no `SceneData`):
//   - water: a mesh the file's water blocks made (`SceneMeshInfo::water`);
//   - terrain: the file's terrain with a `materials` block;
//   - foliage: a streamed world with tile meshes (the GPU scene keeps the foliage lines for a mesh
//     room whatever was declared), or a pack role with foliage optics where the file generates
//     meshes;
//   - stochastic: a pack role sampled stochastically where the file generates meshes.
// The last two are the content's to say and are a guess here; a wrong guess is one more compile.
ResolveVariant predict_resolve_variant(const SceneDesc& desc, const ResolvedSettings& resolved,
                                       gfx::Format color_format);

// The key the early compile and the renderer's creation share.
struct ResolveKey {
  std::string module;  // its name in the shader library
  u64 hash = 0;        // of its SPIR-V (`gfx::Shader::hash`): a `--shaders` manifest's bytes too
  gfx::Format color_format = gfx::Format::R8G8B8A8Unorm;
  bool foliage = false;
  bool operator==(const ResolveKey&) const noexcept = default;
};

class EarlyPipelines {
 public:
  struct Stats {
    bool started = false;  // a compile was started
    bool done = false;     // it finished (made or failed)
    bool ok = false;       // it made a pipeline
    bool taken = false;    // a renderer took it
    u32 mismatched = 0;    // renderers that asked with another key, and made their own
    f64 seconds = 0.0;     // the compile's wall time, on its worker
    ResolveKey key;
  };

  EarlyPipelines() noexcept = default;
  ~EarlyPipelines();
  ENGINE_NON_COPYABLE(EarlyPipelines);

  // Starts the resolve's one-draw pipeline for `variant` on a worker thread, from the module the
  // renderer would load (the embedded SPIR-V, or `shader_manifest`'s, which it reads the same way,
  // `SceneRenderer::Desc::shader_manifest`), with the bindless set's layout. False with `error`
  // when nothing was started: the device cannot draw the variant, or `renderer.resolve.classes` is
  // not `off` (the split's pipelines are other keys, made at creation). Once per object.
  bool start(const gfx::Device& device, const ResolveVariant& variant,
             const std::string& shader_manifest, std::string* error = nullptr);

  // For `SceneRenderer`: whether a pipeline of `key` was started and not taken. Does not wait.
  bool has(const ResolveKey& key) const;
  // Waits for the compile and hands its pipeline over (the caller owns it) when `key` is the one
  // started; false otherwise — counted as a mismatch, and the caller makes its own.
  bool take(const ResolveKey& key, gfx::PipelineHandle& out);

  // Waits for the worker and destroys a pipeline nobody took. The device must still be valid.
  void destroy() noexcept;

  Stats stats() const;

 private:
  void join() noexcept;

  const gfx::Device* device_ = nullptr;
  gfx::ShaderLibrary shaders_;
  // The default set's pipeline layout, which every `GpuScene`'s set is created identically to: a
  // pipeline made with it is used with the scene's (Vulkan's layout compatibility).
  gfx::BindlessSet bindless_;
  const gfx::Shader* module_ = nullptr;
  std::thread worker_;
  mutable std::mutex mutex_;
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

}  // namespace engine::renderer
