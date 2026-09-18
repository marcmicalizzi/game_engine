#pragma once

// What a frame is drawn with (docs/subsystems/renderer.md). `RenderSettings` is the whole knob
// set the renderer has: the rasterization path, the view mode, the culling switches, the
// lights, the deformation mode, and the ray tracing options. It is *what the caller asked for*;
// `resolve_settings` turns it into `ResolvedSettings`, *what this device and this scene can
// actually do*, in one place, so engine-view and engine-host make the same choices and log the
// same reasons. Every choice that a device or a scene can override is made there and nowhere
// else: a path that probes `DeviceFeatures` inside a frame would drift between the two hosts.

#include <core/base/types.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>

#include <string_view>

namespace engine::renderer {

struct SceneData;

// How primary visibility is produced. `Direct` draws mesh shaders straight to color with a
// depth buffer and has no visibility buffer, so it has no id or depth capture either.
enum class RasterMode : u8 { Direct, Hardware, Software, Auto, Vertex, RayTrace };

// `Auto`: ray-traced shadows wherever the device can build the structures, off where it cannot.
enum class ShadowMode : u8 { Auto, Off, RayTraced };

struct RenderSettings {
  RasterMode raster = RasterMode::Hardware;
  ShadowMode shadows = ShadowMode::Auto;
  // gfx::ResolveMode: cluster ids, triangle shading, depth, shaded, normals, uvs.
  u32 view_mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  f32 lod_px = 1.0f;      // screen-space error threshold for LOD selection
  f32 sw_px = 32.0f;      // Auto: clusters narrower than this go to the software rasterizer
  bool cull = true;       // GPU culling and LOD selection
  bool occlusion = true;  // two-pass occlusion culling against a Hi-Z
  bool cone = true;       // backface culling of clusters by their normal cones
  bool lights = true;     // the two orbiting point lights beside the sun
  bool deform = false;    // every instance reads the per-frame deformed-vertex pool
  u32 deform_kind = gfx::k_deform_identity;
  f32 deform_amplitude = 0.02f;
  bool rt_templates = false;  // instantiate prebuilt cluster templates instead of rebuilding

  // Whether two requests would build the same scene and the same pipelines. A host that keeps a
  // loaded scene across calls uses this to decide whether it may reuse the GPU side: the
  // deformed-vertex pool and the ray tracing chain are sized by these, and the resolve's
  // pipeline is chosen by them, so anything that differs means a rebuild.
  bool operator==(const RenderSettings&) const = default;
};

// What the device and the scene made of a `RenderSettings`. The booleans are the frame's
// actual shape; `settings` is the request with the overrides applied (`raster` may have fallen
// back to the vertex path, `cull` may have been forced on, `rt_templates` may have been
// dropped), and it is what a summary reports, because reporting the request would be a lie.
struct ResolvedSettings {
  RenderSettings settings;
  bool direct = false;       // raster == Direct
  bool vertex_path = false;  // raster == Vertex
  bool ray_path = false;     // raster == RayTrace
  bool shadows = false;      // the resolve traces shadow rays
  bool occlusion = false;    // two-pass occlusion culling runs
  bool rt_chain = false;     // the frame builds acceleration structures from its visible list
};

// Why a device cannot render at all with these settings. `Ok` is the only value that lets
// `GpuScene::create` and `SceneRenderer::create` run; engine-view turns `Unavailable` into its
// exit code 3 and engine-host into protocol error 1007.
enum class RenderAvailability : u8 {
  Ok,
  NoVisibilityBuffer,       // no 64-bit buffer atomics: nothing can write the visibility buffer
  NoAccelerationStructures  // --raster rt without cluster acceleration structures or ray queries
};

// Applies every device and scene override, in the order the renderer depends on: the mesh
// shader fallback first (it decides which path runs), then shadows (which decide whether
// occlusion culling can run at all), then the switches that must be forced on. Logs one record
// per override under the `renderer` category, because a picture that silently ignored a flag is
// the hardest kind of surprise to track down. `scene` may be null before a scene is loaded; the
// two scene-driven overrides are then skipped.
void resolve_settings(const RenderSettings& requested, const gfx::DeviceFeatures& features,
                      const SceneData* scene, ResolvedSettings& out);

// Whether a device can run the resolved settings at all. Presentation is deliberately not
// checked: that is the window's requirement, not the renderer's.
RenderAvailability check_availability(const ResolvedSettings& resolved,
                                      const gfx::DeviceFeatures& features) noexcept;
const char* availability_message(RenderAvailability availability) noexcept;

// Names as the command line and the protocol spell them, and the parsers for them. One spelling
// per concept, shared by engine-view's flags and engine-host's schema strings.
const char* raster_name(RasterMode mode) noexcept;
bool parse_raster_mode(std::string_view text, RasterMode& out) noexcept;
const char* shadow_name(ShadowMode mode) noexcept;
bool parse_shadow_mode(std::string_view text, ShadowMode& out) noexcept;
// "none", "identity", "wave", or "lattice" into `deform` and `deform_kind`.
const char* deform_name(const RenderSettings& settings) noexcept;
bool parse_deform_mode(std::string_view text, bool& deform, u32& kind) noexcept;
const char* view_mode_name(u32 mode) noexcept;
bool parse_view_mode(std::string_view text, u32& out) noexcept;

}  // namespace engine::renderer
