#pragma once

// What a frame is drawn with (docs/subsystems/renderer.md). `RenderSettings` is the whole knob
// set the renderer has: the rasterization path, the view mode, the culling switches, the
// lights, the deformation mode, and the ray tracing options. It is *what the caller asked for*;
// `resolve_settings` turns it into `ResolvedSettings`, *what this device and this scene can
// actually do*, in one place, so engine-view and engine-host make the same choices and log the
// same reasons. Every choice that a device or a scene can override is made there and nowhere
// else: a path that probes `DeviceFeatures` inside a frame would drift between the two hosts.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>

#include <optional>
#include <string>
#include <string_view>

namespace engine::renderer {

struct SceneData;

// How primary visibility is produced. `Direct` draws mesh shaders straight to color with a
// depth buffer and has no visibility buffer, so it has no id or depth capture either.
enum class RasterMode : u8 { Direct, Hardware, Software, Auto, Vertex, RayTrace };

// How the sun (and, traced, the point lights) cast shadows (docs/subsystems/renderer.md,
// "Shadows"). `RayTraced` is a ray query per light against the frame's own acceleration
// structures; `Cascaded` is the sun's cascaded shadow maps, drawn by the cull pass and the
// rasterizers from the light and filtered in the resolve. `Auto` is ray-traced wherever the device
// has ray queries and cluster acceleration structures, cascaded maps where it has not, and off on
// a path that can have neither (direct, sw, auto). Changing the RTX-class default to maps is a
// decision the flythrough measurements inform, not one made here.
enum class ShadowMode : u8 { Auto, Off, RayTraced, Cascaded };

// The cascaded shadow maps' defaults (docs/subsystems/renderer.md, "Shadows"). Four cascades of
// 2048 x 2048 texels, side by side in one D32 atlas: 64 MiB, and the cascades split the camera's
// depth logarithmically from the nearest geometry to the farthest (`fit_shadow_cascades`).
inline constexpr u32 k_default_shadow_cascades = 4;
inline constexpr u32 k_default_shadow_map = 2048;
inline constexpr u32 k_max_shadow_map = 4096;

// How many views the frame has and how they are laid out over the target (04 §4.6,
// systems/renderer/view_set.h). `Single` is one rectilinear view over the whole target and is
// what every existing caller gets. `Surround3` is three views with per-monitor off-axis frusta,
// the arrangement the project owner plays on. `Panini` is one wide view rendered rectilinear into
// an oversampled source and resampled in the resolve.
enum class ViewLayout : u8 { Single, Surround3, Panini };

// The deformed-vertex pool's default budget, in kibibytes: 8 MiB, or 699,050 vertices — more than
// twice the largest cut E25 measured (4,831 clusters at about 65 vertices each, 3.8 MB) and about
// seven times what a 1,024-fox crowd's cut actually deforms. It is a *budget* — `GpuScene` clamps
// it down to what a block per instance's whole mesh would have taken, so a scene of one character
// still allocates exactly what it always did and can never overflow — and `--deform-pool-mib`
// raises it for a frame whose cut is larger than anything measured here.
//
// Kibibytes rather than mebibytes because a *test* has to be able to ask for a pool too small for
// its own cut: overflow is a stated behaviour (the rest pose, counted) and a behaviour nothing
// exercises is a behaviour nobody knows works.
inline constexpr u32 k_default_deform_pool_kib = 8 * 1024;
inline constexpr u32 k_min_deform_pool_kib = 1;  // a pool of nothing is not a pool
// 16 bytes a mesh vertex: 4 MiB is a 260,000-vertex character, or twenty 13,000-vertex control
// meshes. It is a default and not a limit; `--static-shape-kib` moves it.
inline constexpr u32 k_default_static_shape_kib = 4 * 1024;

struct RenderSettings {
  RasterMode raster = RasterMode::Hardware;
  ShadowMode shadows = ShadowMode::Auto;
  // gfx::ResolveMode: cluster ids, triangle shading, depth, shaded, normals, uvs, the sun's shadow,
  // the textured albedo unlit, and the material's ambient occlusion as data.
  u32 view_mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  f32 lod_px = 1.0f;      // screen-space error threshold for LOD selection
  f32 sw_px = 32.0f;      // Auto: clusters narrower than this go to the software rasterizer
  bool cull = true;       // GPU culling and LOD selection
  bool occlusion = true;  // two-pass occlusion culling against a Hi-Z
  bool cone = true;       // backface culling of clusters by their normal cones
  // The clusters the cone test keeps out of the picture still cast ray-traced shadows: the cull
  // pass hands them to the acceleration structures as a run of their own, at the picture's LOD
  // (docs/subsystems/renderer.md, "Shadows"). Off is the old behaviour, in which a card seen from
  // behind throws no shadow; it exists to measure what the casters cost and what they restore.
  bool shadow_casters = true;
  // The cascaded shadow maps (`ShadowMode::Cascaded`): how many cascades (1..4), how many texels a
  // side each, and how far from the camera they reach — 0 is the far side of the scene's bounds.
  // They size the depth atlas, which is why they are here where a change forces a rebuild.
  u32 shadow_cascades = k_default_shadow_cascades;
  u32 shadow_map = k_default_shadow_map;
  f32 shadow_distance = 0.0f;
  // The cascades' light-view cull tests each pair against the cascade's box. Off draws every pair
  // of the camera's cut into every cascade, which must cast exactly the same shadow — the test of
  // "culling never changes the picture" for the maps; it is a switch for that test, not a mode.
  bool shadow_frustum = true;
  // Multiplies the LOD threshold of the cascades' cull only. 1 — the only value anything but a
  // measurement uses — draws each cascade from exactly the picture's cut; larger is a coarser cut
  // in the maps than in the picture, which is the question "should a shadow's LOD be the light's
  // own" asked in numbers (docs/subsystems/renderer.md, "Shadows", has the answer).
  f32 shadow_lod_scale = 1.0f;
  // Texels the cascaded maps' lookup leaves the surface along its geometric normal at a grazing
  // surface (gfx::ShadowMapParams::normal_offset); negative is k_shadow_normal_offset_texels.
  f32 shadow_normal_offset = -1.0f;
  bool lights = true;  // the two point lights beside the sun
  // They orbit as the frame index advances (lighting.h); off, the default since 2026-09-27, they
  // stand where frame 0 puts them, so a time-lapse's sand is lit the same from frame to frame.
  bool orbit_lights = false;
  // The sun, degrees (lighting.h, `sun_direction`); unset takes the `renderer.sun.*` tunables.
  // Not read with a sky, whose sun its provider places (sky.h).
  std::optional<f32> sun_azimuth_deg;
  std::optional<f32> sun_elevation_deg;
  // A sky's exposure for the whole run (renderer.md, "Exposure"; engine-view's `--exposure` and
  // `--exposure-ev100`): stops added to the rule's, and a fixed exposure value that overrides it.
  // A frame's `ExposureRequest` adds to both. Nothing without a sky.
  f32 exposure_ev = 0.0f;
  std::optional<f32> exposure_ev100;
  bool deform = false;  // every instance reads the per-frame deformed-vertex pool
  u32 deform_kind = gfx::k_deform_identity;
  f32 deform_amplitude = 0.02f;
  // The deformed-vertex pool's budget in kibibytes; 0 takes `k_default_deform_pool_kib`. It sizes
  // a scene buffer, which is why it lives here beside the other things that force a rebuild.
  u32 deform_pool_kib = 0;
  // The **static shape** stage's weights, one per morph channel of the scene, and the budget for
  // the per-instance caches that keep its result. Both size scene buffers, which is why they are
  // here rather than on a frame: a weights change is cheap (one dispatch over the mesh) but
  // adding a channel is not.
  //
  // An entry left at zero is a channel the static stage does not play. The array may be shorter
  // than the scene's channel array, in which case the rest are zero; a scene with no channels
  // ignores it entirely, which is what keeps every existing picture byte-identical.
  Vector<f32> morph_static_weights;
  // Kibibytes of `gfx::DeformCacheVertex` (16 bytes a mesh vertex) the scene may spend on static
  // shape caches, handed out in instance order until it runs out. An instance that gets none runs
  // its static stage every frame over the cut — the same answer for more work, which is the same
  // graceful degradation `k_no_pool_slot` gives the pool. 0 takes the default.
  u32 static_shape_kib = 0;
  bool rt_templates = false;  // instantiate prebuilt cluster templates instead of rebuilding
  // The most device memory the per-frame cluster acceleration structures may take, in mebibytes;
  // 0 takes the `renderer.rt.budget_mib` tunable (1024 by default). The structures are sized by
  // what the frames build and only ever reach this under a cut that large; past it a frame drops
  // whole instances' structures, shadow casters first (docs/subsystems/renderer.md, "The ray
  // tracing chain's memory"). It bounds a scene buffer, which is why it lives here.
  u32 rt_budget_mib = 0;
  // One upload per distinct image per scene (docs/subsystems/renderer.md, "One upload per distinct
  // image"): every (mesh, image) a material samples is keyed by its content — the `.tex` build key,
  // or the hash of the image's encoded bytes when it is decoded — and every mesh that samples the
  // same content shares one texture and one bindless slot. Off uploads each mesh's own copy, which
  // is what every build before 2026-09-25 did; it exists to measure what sharing saves and to hold
  // the picture to the same bytes, not as a mode. It decides what the scene's textures are, which
  // is why it lives here, where a change forces a rebuild.
  bool share_textures = true;

  // Game seconds per real second for the dune field's time-lapse (terrain_time.h,
  // docs/subsystems/renderer.md, "The dunes in time-lapse"); 0, the default, is the scene's own
  // `time` and nothing re-evaluated. It sizes nothing, but it is a question about the scene, and
  // both hosts read their questions from here.
  f64 time_rate = 0.0;
  // The rate may change while the scene is drawn (`TerrainMotion::set_rate`; engine-view's `,` and
  // `.` in an interactive window), so a dune terrain is drawn as terrain levels even at a
  // `time_rate` of zero: the motion has to exist, standing still, for a key to set it going. It
  // costs what a still terrain's levels cost — the pool's `renderer.terrain.pool_mib` floor, the
  // rest pose evaluated once at load, no cone test on the terrain — and draws the same picture
  // (the rest pose is the mesh's heights to the bit). Nothing else asks for it.
  bool time_rate_live = false;
  // The dune generator's ground near the camera at the terrain rings' finer grids, drawn beside the
  // scene's own (terrain_rings.h, docs/subsystems/renderer.md, "The rings in the scene"): an inner
  // ring at 50 cm and a middle one at a metre by default (`terrain.rings.*`), rebuilt round the
  // camera as it moves. Off by default: they are built at load, a few seconds on the erg, and hold
  // their slots on the device whether the camera is near the ground or not.
  bool terrain_rings = false;
  // The ground drawn from the world's tiles (terrain_tiles.h; docs/subsystems/renderer.md, "The
  // ground from the world's tiles"; ADR-0050): the dune generator's terrain as the world's tile
  // grid, streamed round the camera with no end, in place of the scene's grid. A scene asks for it
  // with its `world` block's `ground`; this asks for it on any dune terrain (engine-view's
  // `--terrain-tiles`), with the world's default rings where the scene has none. It takes the place
  // of the rings.
  bool terrain_tiles = false;

  // Geometry streaming (04 §4.3 step 3, §4.9). The scene's clusters are laid out in fixed-size
  // pages, the GPU holds a budgeted subset of them in a page pool, the cull pass draws whatever is
  // resident and asks for what it is missing, and the picture converges. These size the scene's
  // page pool and its staging ring, which is why they live here beside the view count rather than
  // in the frame: changing one means rebuilding the `GpuScene`.
  bool stream = false;
  // The residency manager's budget, over the page table's own byte counts
  // (`geometry::ClusterPageDesc::bytes`). Zero is every page: streaming with nothing to evict,
  // which is the configuration the "all resident equals today's cut" test renders.
  u64 page_budget_bytes = 0;
  // How many bytes of page payload one frame may copy into the pool. Zero is
  // `k_default_upload_budget`; a value under the largest page's payload is raised to it, because a
  // budget no page fits in would never converge.
  u32 upload_budget_bytes = 0;

  // Multi-view (04 §4.6). These size the scene's per-frame working set — the visible list, the
  // argument blocks, the flags, the acceleration structures all carry a slice per view — which is
  // why they live here, where a change forces a rebuild, and not in the frame.
  ViewLayout views = ViewLayout::Single;
  f32 side_yaw = 0.0f;        // surround3: radians the side monitors are turned inward
  f32 panini_d = 1.0f;        // panini: 0 is rectilinear, 1 the classic Pannini
  f32 peripheral_lod = 1.0f;  // LOD threshold multiplier outside the attention region

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
  // The frame draws the sun's cascaded shadow maps and the resolve filters them: `Cascaded`, or
  // `Auto` on a device without ray queries, on a path that rasterizes the picture into the
  // visibility buffer in one list per pass (hw, vertex). Never together with `shadows`.
  bool csm = false;
  u32 shadow_cascades = 0;  // the cascades drawn, 1..4 with `csm`, else 0
  bool occlusion = false;   // two-pass occlusion culling runs
  bool rt_chain = false;    // the frame builds acceleration structures from its visible list
  // The cull pass keeps what its cone test rejects as shadow casters, in the visible list's run
  // `gfx::k_caster_run`, and the chain builds them beside the drawn clusters. True when shadows
  // are traced, the cone test is on, and the records are build records rather than template
  // instantiations (which cannot mark a caster non-opaque) — see `resolve_settings`.
  bool casters = false;
  // The vertex path draws a culled cut with one indexed draw per run (cluster_vertex_indexed.slang)
  // rather than every cluster's triangle capacity: `vertex_path`, the cull pass on, and a device
  // with geometryShader, which the draw's SV_PrimitiveID needs, and fullDrawIndexUint32, which
  // its `slot << 8 | local` indices need past 65,536 clusters in a run.
  bool vertex_indexed = false;
  // The deformed-vertex pool pass runs. True when the settings deform every instance and also
  // when the scene has a skinned instance, which `settings.deform` alone does not say: skinning
  // is one of `deform.slang`'s kinds rather than a second pass, so a scene with a character in it
  // fills the pool whether or not anyone asked for `--deform`.
  bool deform_pass = false;
  // The terrain is drawn as **terrain levels** (docs/subsystems/renderer.md, "The dunes in
  // time-lapse"): its instance is deformed, and the pool pass writes its heights blended between
  // two evaluated fields of the dune generator. True when the scene's terrain is the generator's
  // and the settings move it (`time_rate` above zero), may move it later (`time_rate_live`) or ask
  // for its rings; it turns `deform_pass` on with it.
  bool terrain_levels = false;
  // The terrain rings are drawn as levels beside the scene's grid (`RenderSettings::terrain_rings`
  // on a scene with terrain levels, read whole): the host builds a `TerrainRingSet` and hands it
  // to `GpuScene::create` and `TerrainMotion::start`.
  bool terrain_rings = false;
  // The terrain is drawn from the world's tiles (`RenderSettings::terrain_tiles`, or the scene's
  // `world.ground`, on a scene with terrain levels, read whole): the host builds a `TerrainTileSet`
  // and hands it to `GpuScene::create` and `TerrainMotion::start`, and the world's ring hands it
  // the tiles it holds. Never together with `terrain_rings`.
  bool terrain_tiles = false;
  // Geometry pages stream on demand. True only when the caller asked *and* the scene carries a
  // page table *and* nothing else in the frame contradicts it — see `resolve_settings` for the
  // three things that do and why each one is a refusal rather than a silent half-measure.
  bool stream = false;
  // How many views the layout has. The `GpuScene` is created before the `SceneRenderer` and so
  // before the view set, and every per-frame buffer it owns is sized by this, so the count is
  // resolved here rather than read off a `ViewSet` that does not exist yet.
  u32 view_count = 1;
};

// Why a device cannot render at all with these settings. `Ok` is the only value that lets
// `GpuScene::create` and `SceneRenderer::create` run; engine-view turns `Unavailable` into its
// exit code 3 and engine-host into protocol error 1007.
enum class RenderAvailability : u8 {
  Ok,
  NoVisibilityBuffer,  // no 64-bit buffer atomics: nothing can write the visibility buffer
  // `--raster rt`, or an explicit `--shadows rt`, on a device without cluster acceleration
  // structures and ray queries. `ShadowMode::Auto` never gets here: it turns shadows off.
  NoAccelerationStructures
};

// **The largest cluster geometry index the ray tracing chain names** for a scene of `pair_count`
// pairs in `views` views (docs/subsystems/renderer.md, "The ray tracing chain's index space"). A
// cluster's geometry index is its visible index, so a hit leads straight to the pair the
// rasterizer drew; the drawn clusters are the views' first runs of the visible list, run-major, so
// they end at `views * pair_count - 1`, and the shadow casters are run `gfx::k_caster_run`, so
// with them the range ends at `(k_caster_run + 1) * views * pair_count - 1`. It is a function of
// the scene and the view layout and **not** of the frame's size or the chain's capacity: every
// structure is created able to name any entry. The device bounds it
// (`gfx::DeviceFeatures::cluster_max_geometry_index`, 2^24 - 1 on the RTX 5090), and
// `resolve_settings` drops what does not fit. u64, because the product does not fit a u32 at the
// scale where it matters. 0 for an empty scene.
u64 rt_max_geometry_index(u32 pair_count, u32 views, bool casters) noexcept;

// Applies every device and scene override, in the order the renderer depends on: the mesh
// shader fallback first (it decides which path runs), then shadows (which decide whether
// occlusion culling can run at all), then the switches that must be forced on. **The ray tracing
// chain's index space** is one of them: a scene whose pairs in every view name more cluster
// geometry indices than the device has (`rt_max_geometry_index` against
// `gfx::DeviceFeatures::cluster_max_geometry_index`) loses the shadow casters first, and if the
// drawn clusters alone do not fit, the ray path draws with the rasterizer and traced shadows —
// `rt` or `auto` — become the cascaded maps. Never a refusal: the limit is the scene's times the
// layout's, and the same flags run on a smaller scene or with one view. Logs one record
// per override under the `renderer` category, because a picture that silently ignored a flag is
// the hardest kind of surprise to track down. `scene` may be null before a scene is loaded; the
// two scene-driven overrides are then skipped.
void resolve_settings(const RenderSettings& requested, const gfx::DeviceFeatures& features,
                      const SceneData* scene, ResolvedSettings& out);

// Whether a device can run the resolved settings at all. Presentation is deliberately not
// checked: that is the window's requirement, not the renderer's.
RenderAvailability check_availability(const ResolvedSettings& resolved,
                                      const gfx::DeviceFeatures& features) noexcept;
// The short phrase for a refusal ("has no 64-bit buffer atomics"), for a caller with no device.
const char* availability_message(RenderAvailability availability) noexcept;
// The refusal as a host prints it: the adapter's name and, for a ray-tracing refusal, the
// sentence the device's verdict carries (`gfx::ray_tracing_degradation`), which names exactly
// the extensions that are missing. engine-view's exit 3 and engine-host's error 1007 both say
// this, so what `engine-cli gpu.adapters` reported and what the refusal says cannot drift.
std::string unavailable_reason(RenderAvailability availability, const gfx::Device& device);

// Names as the command line and the protocol spell them, and the parsers for them. One spelling
// per concept, shared by engine-view's flags and engine-host's schema strings.
const char* raster_name(RasterMode mode) noexcept;
bool parse_raster_mode(std::string_view text, RasterMode& out) noexcept;
const char* shadow_name(ShadowMode mode) noexcept;
bool parse_shadow_mode(std::string_view text, ShadowMode& out) noexcept;
// What the frame actually shadows with, as a summary reports it: "rt", "csm", or "off".
const char* resolved_shadow_name(const ResolvedSettings& resolved) noexcept;
// "none", "identity", "wave", or "lattice" into `deform` and `deform_kind`.
const char* deform_name(const RenderSettings& settings) noexcept;
bool parse_deform_mode(std::string_view text, bool& deform, u32& kind) noexcept;
const char* view_mode_name(u32 mode) noexcept;
bool parse_view_mode(std::string_view text, u32& out) noexcept;
// "single", "surround3", or "panini".
const char* view_layout_name(ViewLayout layout) noexcept;
bool parse_view_layout(std::string_view text, ViewLayout& out) noexcept;

}  // namespace engine::renderer
