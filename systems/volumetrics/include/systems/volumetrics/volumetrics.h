#pragma once

// **The participating medium's mid tier** (docs/subsystems/volumetrics.md; plan 04 §4.2, the
// 2026-09-24 direction note "one field rendered three ways"; roadmap R47, W45, R83, P7): the dust
// the wind holds over the ground, the sand blowing off the dune crests, and the far-field clouds,
// drawn by one frame pass registered at the renderer's `AfterResolve` (frame_pass.h) — view-aligned
// froxels filled from the sources, integrated front to back and applied to the frame's linear
// colour, and a ray-marched cloud layer composited into the sky before them.
//
// **The sources are functions of the frame's state** — the scene's `medium` block, the wind
// record at the world's clock, the eye — and never of a previous frame: no history, no
// reprojection, so the same frame is the same bytes. The CPU half of each is here, with no device,
// so the tests hold the functions themselves; the shader (shaders/medium.slang) is the same
// arithmetic.
//
// **A capability** (ADR-0027): `ENGINE_WITH_VOLUMETRICS`, off with the terrain capability it reads
// the wind and the crests from. The renderer names nothing of it; a host registers the pass.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json.h>
#include <core/math/math.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>
#include <systems/renderer/frame_pass.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::gfx {
class Device;
struct ComputePipeline;
}  // namespace engine::gfx

namespace engine::terrain {
class DuneField;
struct CrestLine;
struct TileWind;
}  // namespace engine::terrain

namespace engine::renderer {
class SceneRenderer;
struct FrameFire;
struct SceneData;
class TerrainSampler;
}  // namespace engine::renderer

namespace engine::volumetrics {

// ---- the scene's block (`engine.scene.Medium`, schemas/volumetrics.schema) ---------------------

struct MediumDesc {
  // The far-field cloud layer (R83): between two altitudes above the world's y = 0, its coverage
  // (0 clear, 1 overcast), its type (0 a thin flat layer, 1 tall heaped cloud) and its seed.
  bool clouds = false;
  f32 cloud_base_m = 1800.0f;
  f32 cloud_top_m = 4200.0f;
  f32 cloud_coverage = 0.3f;
  f32 cloud_type = 0.5f;
  u32 cloud_seed = 1;
  // The dust haze: a multiplier on its density and its scale height at the record's mean wind.
  bool dust = true;
  f32 dust_scale = 1.0f;
  f32 dust_height_m = 15.0f;
  // The crest plumes (W45): on, and a multiplier on their density.
  bool plumes = true;
  f32 plume_strength = 1.0f;
};

// Reads a scene's `medium` block (`SceneData::medium`, carried by the renderer uninterpreted):
// false with a sentence naming the field for anything `engine.scene.Medium` refuses. A null block
// is no medium: `present` false and true returned.
bool read_medium(const JsonValue& block, MediumDesc& out, bool& present, std::string* error);

// `--medium-debug`: one source alone, for a look.
enum class MediumDebug : u8 { none = 0, dust, plumes, clouds, fire };
bool parse_medium_debug(std::string_view text, MediumDebug& out) noexcept;
const char* medium_debug_name(MediumDebug debug) noexcept;

// ---- the wind --------------------------------------------------------------------------------

// The wind in the hour holding a time, as the medium reads it: `terrain::WindRecord::wind_at`, the
// one wind (terrain.md, "Storms"), turned into the numbers the sources take.
struct MediumWind {
  Vec2 direction{1.0f, 0.0f};  // unit, horizontal: where the sand moves to
  f32 strength = 1.0f;         // the hour's wind over the record's mean (`speed_q16 / 2^16`)
  f32 speed_mps = 8.0f;        // `strength * terrain::k_mean_wind_mps`
  f32 flux_m2_per_day = 0.0f;  // the hour's sand flux per metre of width, as a daily rate
  bool storm = false;
};
// `field` null (no dunes: the waves, or no terrain): the record's mean wind towards +x.
MediumWind wind_now(const terrain::DuneField* field, f64 time_s) noexcept;

// ---- the dust haze -----------------------------------------------------------------------------

// Its numbers, named: what the functions below are built from (volumetrics.md, "The dust haze").
inline constexpr f32 k_dust_clear_per_m = 3.9e-5f;  // a calm's floor: 100 km of visibility
inline constexpr f32 k_dust_haze_per_m = 4.0e-4f;   // times strength^2: the day's wind's haze
inline constexpr f32 k_dust_wall_per_m = 0.08f;     // a storm hour's wall at its peak strength
inline constexpr f32 k_dust_storm_peak = 2.5f;      // the strength the wall is quoted at
inline constexpr f32 k_dust_phase_g = 0.75f;        // sand scatters strongly forward
inline constexpr f32 k_dust_multiple = 0.35f;       // the constant share scattered more than once
// The transmittance that defines visibility (the meteorological contrast threshold).
inline constexpr f32 k_visibility_contrast = 0.02f;
inline constexpr f32 k_max_visibility_m = 1.0e6f;

struct DustState {
  f32 extinction_per_m = 0.0f;  // at the ground
  f32 height_m = 1.0f;          // the scale height it falls off over
};
// The haze's density at the ground and its scale height for a wind: calm nearly clear, the day's
// wind a faint haze, a storm hour a wall; the height a few metres in a calm and tens in a storm,
// since saltation stays low and suspension rises. `desc.dust` off is no dust.
DustState dust_state(const MediumDesc& desc, const MediumWind& wind) noexcept;
// The extinction `height_m` above the dust's ground: the ground's below it.
f32 dust_extinction(const DustState& dust, f32 height_m) noexcept;
// The dust's optical depth along a ray from a point `eye_height_m` above the dust's ground, of
// vertical direction component `dy`, between distances `a` and `b` (shaders/medium.slang's
// `dust_optical_depth`, the same closed form).
f32 dust_optical_depth(const DustState& dust, f32 eye_height_m, f32 dy, f32 a, f32 b) noexcept;
// The distance at which a horizontal ray's transmittance falls to `k_visibility_contrast` through
// an extinction: -ln(0.02) / sigma, never past `k_max_visibility_m`.
f32 visibility_m(f32 extinction_per_m) noexcept;

// ---- the crest plumes (W45) ---------------------------------------------------------------------

// The sand blowing off a brink (volumetrics.md, "The crest plumes"): for each crest line of each
// dune tile within `k_plume_radius_m` of the eye, a plume leaving each of its four segments
// downwind along the tile's wind, its density the tile's saltation flux times the crest's
// sharpness, spread as a half-Gaussian ribbon that grows downwind and decays over a length tied to
// the wind's speed.
inline constexpr f32 k_plume_radius_m = 160.0f;       // plumes reaching within this of the eye
inline constexpr f32 k_plume_min_sharpness = 0.3f;    // a rounded crest sheds no plume
inline constexpr f32 k_plume_min_height_m = 0.5f;     // nor does a ripple-sized one
inline constexpr f32 k_plume_peak_per_m = 0.03f;      // at the brink: mean saltation, sharpness 1
inline constexpr f32 k_plume_mean_saltation = 0.55f;  // m^2 a day: the record's mean (5,479 cm^2)
inline constexpr f32 k_plume_decay_s = 2.5f;          // the decay length: this times the wind speed
inline constexpr f32 k_plume_spread_m = 0.4f;         // the ribbon's spread at the brink
inline constexpr f32 k_plume_spread_per_m = 0.12f;    // and its growth per metre downwind
inline constexpr f32 k_plume_lift = 0.04f;            // its centre rises this per metre downwind
inline constexpr f32 k_plume_floor = 0.35f;           // its spread below the centre, of above
inline constexpr u32 k_plume_cells = 64;              // the binning grid round the eye, a side
inline constexpr f32 k_plume_cell_m = 6.0f;
inline constexpr u32 k_plume_max_segments = 4096;
inline constexpr u32 k_plume_max_refs = 131072;
inline constexpr i64 k_plume_tile_mm = 32000;  // the world's tiles, the crest query's unit

// One segment of a brink and its plume, in the frame's space (GPU-mirrored, 48 bytes).
struct PlumeSegment {
  Vec4 a;      // xyz the segment's first end, w the plume's peak extinction /m
  Vec4 b;      // xyz its second end, w its decay length downwind, m
  Vec4 shape;  // xy downwind (unit, horizontal), z the spread at the brink, w per metre downwind
};

// The peak extinction of a plume leaving a brink of `sharpness` in a saltation flux: zero in a
// calm, `k_plume_peak_per_m` at the record's mean and a sharp brink, growing as the flux's square
// root.
f32 plume_peak(f32 saltation_m2_per_day, f32 sharpness, f32 strength) noexcept;
// A crest line's four segments in the frame of the eye `eye` (the crest's world metres less the
// eye, in f64), under a tile wind: at most `capacity` written, how many returned; none for a crest
// too rounded or too low, or in a calm.
u32 crest_plume_segments(const terrain::CrestLine& crest, const terrain::TileWind& wind,
                         const WorldPos& eye, f32 strength, PlumeSegment* out,
                         u32 capacity) noexcept;
// A segment's plume's extinction at a point of the frame's space (shaders/medium.slang's
// `plume_density`, the same arithmetic).
f32 plume_density(const PlumeSegment& segment, const Vec3& x) noexcept;

// ---- the far-field clouds (R83, P7's far half) --------------------------------------------------

// A procedural layer between the block's two altitudes (volumetrics.md, "The far-field clouds"):
// value-noise fbm in Perlin's role remapped by inverted Worley cells, a height profile by the type,
// the coverage's threshold and a finer Worley erosion, moved by the wind record's wind.
inline constexpr f32 k_cloud_extinction_per_m = 0.04f;  // a cloud's core
inline constexpr f32 k_cloud_speed_factor = 2.0f;       // the wind aloft over the surface's
inline constexpr f32 k_cloud_period_m = 131072.0f;      // the field repeats over this, on x and z
inline constexpr f32 k_cloud_shape_m = 4096.0f;         // the fbm's coarsest cell
inline constexpr f32 k_cloud_cell_m = 2048.0f;          // the Worley cells it is carved by
inline constexpr f32 k_cloud_detail_m = 512.0f;         // the erosion's cells
inline constexpr f32 k_cloud_phase_forward = 0.8f;      // the two-lobe phase: forward g
inline constexpr f32 k_cloud_phase_back = -0.3f;        // back g
inline constexpr f32 k_cloud_phase_mix = 0.7f;          // the forward lobe's share
inline constexpr f32 k_cloud_multiple = 0.3f;  // one octave more: this share at a quarter depth
inline constexpr u32 k_cloud_steps = 48;       // the march's samples through the slab
inline constexpr u32 k_cloud_scale = 4;        // the march's resolution: a quarter of the view's
inline constexpr u32 k_cloud_shadow_texels = 128;  // the clouds' map towards the sun, a side

struct CloudParams {
  f32 base_m = 1800.0f;
  f32 top_m = 4200.0f;
  f32 coverage = 0.3f;
  f32 type = 0.5f;
  u32 seed = 1;
};
CloudParams cloud_params(const MediumDesc& desc) noexcept;
// The layer's extinction at a noise coordinate (metres over the ground, wrapped to the period by
// the caller) and an altitude above the world's y = 0 (shaders/medium.slang's `cloud_density`).
f32 cloud_density(const CloudParams& params, f32 qx, f32 altitude_m, f32 qz) noexcept;

// **How far the wind has carried the clouds by a time**, closed form like the record's own flux
// integral: the wind's velocity aloft summed hour by hour over the record's period once, so the
// offset at t is the periods' whole sums, the prefix to t's hour and t's hour's velocity times the
// time into it — continuous, a function of the seed and the time, no history. Without a record, a
// steady wind of the mean speed aloft towards +x.
class CloudDrift {
 public:
  void build(const terrain::DuneField* field);
  // The drift in metres (x, z), reduced to the field's period in f64.
  Vec2 offset_m(f64 time_s) const noexcept;

 private:
  Vector<f64> prefix_;  // x, z per hour of the period, and the period's total last
  u32 hours_ = 0;
  const terrain::DuneField* field_ = nullptr;
};

// ---- fire (R85) ---------------------------------------------------------------------------------

// A fire in the medium (volumetrics.md, "Fire"): the renderer's `FrameFire` — its light already in
// the frame's light array — as a flame, its light scattered round it and its smoke.
//
// **The flame** is an emissive, weakly absorbing volume of seeded noise rising through an envelope
// over the base radius and height. Its emission is tied to the light, so the flame is as bright as
// the light it casts: the intensity over the flame's projected area (`k_flame_fill` of the
// rectangle 2r x h) is its luminance, and the emission per metre is that over the mean path through
// it (`k_flame_fill` of 2r), in the blackbody colour of the block's temperature.
inline constexpr f32 k_flame_absorption_per_m = 0.8f;  // at density 1: a flame is mostly clear
inline constexpr f32 k_flame_fill = 0.6f;
// The share of that luminance the flame's own emission carries at its densest, chosen by eye on the
// campfire's captures: at the whole of it every flame is one saturated blob at any exposure that
// shows the sand it lights, where a real flame's core and its tongues read apart.
inline constexpr f32 k_flame_brightness = 0.3f;
// **The puffing** (docs/experiments/campfire-flame.md): a buoyant diffusion flame of base diameter
// D sheds a toroidal vortex at f = 1.5 / sqrt(D) Hz (Cetegen & Ahmed 1993, Combust. Flame 93:157;
// the same D^-1/2 law as Hamins, Yang & Kashiwagi 1992 and Malalasekera et al. 1996's 1.68). The
// noise's large features are a diameter apart vertically (two cells of r), and they rise at f D,
// so one passes a point of the flame once a puff: 1.2 m/s and 1.9 Hz for the campfire's 0.64 m
// base, where the eye's 1.4 m/s through cells of 0.45 r had tongues pass at 9.7 Hz.
inline constexpr f32 k_flame_puff_hz_m = 1.5f;  // f sqrt(D), Hz m^1/2
inline constexpr f32 k_flame_cell = 0.45f;      // the noise's cell across, of the flame's radius
inline constexpr f32 k_flame_cell_up = 1.0f;    // and up: two cells a diameter
inline constexpr f32 k_flame_noise_period = 64.0f;  // its lattice wraps on y, in cells
// The puffing frequency and the speed the flame's features rise at, for a base radius.
f32 flame_puff_hz(f32 radius_m) noexcept;
f32 flame_rise_m_s(f32 radius_m) noexcept;
// **The marched flame** (R112): a fire is marched per pixel in the apply, with this many steps,
// while its distance is under `k_fire_march_widths` of its bounds' width (2 x 1.6 r): where it is
// wider than about two froxel columns of the default grid at a 60-degree view. Past it the froxels
// hold it as a smooth blob of the same light, and over the last `k_fire_march_blend` of that
// distance the two are blended. Before R112 the switch was a step at 8 m and the froxels sampled
// the flame's noise once a cell, which a slice 13% of the distance deep missed or caught whole.
inline constexpr f32 k_fire_march_widths = 60.0f;
inline constexpr f32 k_fire_march_blend = 0.25f;
inline constexpr u32 k_flame_steps = 40;
// The share of the marched flame a fire at `distance_m` from the eye draws, 1 near, 0 past the
// march distance, smooth across the blend.
f32 fire_march_weight(f64 distance_m, f32 radius_m) noexcept;
// **The far flame**: a Gaussian blob of the marched flame's light, across `k_flame_blob_across` of
// its radius and up `k_flame_blob_up` of its height about `k_flame_blob_centre` of it, holding
// `k_flame_blob_fill` of the emission at density 1 over the cylinder r^2 pi h. The four are the
// marched flame's own moments, the noise left out: its emission (density squared, hotter at the
// base) integrates to 0.129 of the cylinder's; its centroid is 0.26 h up, its spread 0.17 h up and
// about 0.35 r across. The fill was then measured against the marched flame itself at 30 m (the
// campfire, smoke off, nothing clipped): 0.15 drew 1.8 times its light — the noise carves more
// than it adds — so it is 0.08 (docs/experiments/campfire-flame.md).
inline constexpr f32 k_flame_blob_across = 0.35f;
inline constexpr f32 k_flame_blob_up = 0.17f;
inline constexpr f32 k_flame_blob_centre = 0.26f;
inline constexpr f32 k_flame_blob_fill = 0.08f;
// **The smoke**: at density 1 this extinction at the flame's top, an albedo darker than the sand's
// and the dust's phase; the column fades by e over `k_smoke_fade_m` of its path, starts at
// `k_smoke_start` of the flame's radius, and bends with `k_smoke_wind` of the record's wind (the
// wind at a fire's height, under the log profile's 10 m).
inline constexpr f32 k_smoke_extinction_per_m = 0.6f;
inline constexpr f32 k_smoke_albedo = 0.42f;
inline constexpr f32 k_smoke_fade_m = 25.0f;
inline constexpr f32 k_smoke_start = 0.6f;
inline constexpr f32 k_smoke_wind = 0.5f;

// One fire as the shader reads it (GPU-mirrored, 128 bytes; shaders/medium.slang's `MediumFire`).
struct MediumFire {
  Vec4 base;   // xyz the flame's base (frame space), w its radius
  Vec4 flame;  // x its height, y absorption /m, z the noise's rise (cells, mod the period), w cell
  Vec4 emission;    // rgb the flame's emission /m at density 1 (raw), w the march's share
  Vec4 light;       // xyz the light (frame space), w its reach
  Vec4 light_rgb;   // rgb its colour times its intensity at this clock (raw), w 0
  Vec4 smoke;       // x extinction /m at the top, y rise m/s, z spread per metre, w fade length m
  Vec4 wind;        // xz the wind at the fire (m/s), y the smoke's start radius, w the cell up
  u32 ids[4] = {};  // x the seed, y 1 when the apply marches this flame (the share is over 0)
};

// The flame's emission per metre at density 1, raw: its luminance (`intensity` over `k_flame_fill`
// of 2 r h) over its mean path (`k_flame_fill` of 2 r).
f32 flame_emission_per_m(f32 intensity, f32 radius_m, f32 height_m) noexcept;
// A frame's fire in the frame of the eye at the world's clock, under the hour's wind: the flame's
// noise offset reduced to its period in f64, the smoke's numbers, the light where the resolve has
// it, and the share the apply marches (`fire_march_weight` at the eye's distance).
MediumFire medium_fire(const renderer::FrameFire& fire, const WorldPos& eye, const MediumWind& wind,
                       f64 time_s) noexcept;
// The smoke's extinction at a point of the frame's space (shaders/medium.slang's
// `smoke_density`, the same arithmetic).
f32 smoke_density(const MediumFire& fire, const Vec3& x) noexcept;
// **The smoke a froxel holds** (shaders/medium.slang's `smoke_cell`, the same arithmetic): the mean
// extinction along the eye's ray `dir` between distances t0 and t1, in closed form for the column
// as it stands at the point `at` (its centre's line, its radius widened by half of `width`, the
// cell's width across, its fade), so a slice deeper than the column holds its share of it wherever
// the slice's boundaries fall rather than all of it or none. R112: the point sample a cell took
// before missed a column narrower than the slice or caught it whole.
f32 smoke_cell(const MediumFire& fire, const Vec3& dir, f32 t0, f32 t1, const Vec3& at,
               f32 width) noexcept;

// ---- the pass
// ------------------------------------------------------------------------------------

struct MediumOptions {
  MediumDebug debug = MediumDebug::none;
  // Draw the frame's fires (R85) in the medium. Off, the medium draws as if the scene had none (the
  // fires' lights stay in the resolve's array: they are the renderer's).
  bool fires = true;
  // The froxel grid and its slices; zero takes the tunables (`volumetrics.froxels.*`).
  u32 froxels_x = 0;
  u32 froxels_y = 0;
  u32 froxels_z = 0;
  f32 near_m = 0.0f;
  f32 far_m = 0.0f;
};

// What the last recorded frame drew, for a summary.
struct MediumStats {
  bool active = false;
  f32 visibility_m = -1.0f;
  f32 dust_extinction_per_m = 0.0f;  // at the eye's height
  f32 dust_height_m = 0.0f;
  f32 eye_height_m = 0.0f;  // above the dust's ground
  MediumWind wind;
  u32 froxels_x = 0;
  u32 froxels_y = 0;
  u32 froxels_z = 0;
  f32 near_m = 0.0f;
  f32 far_m = 0.0f;
  u32 plume_segments = 0;  // the brink segments the frame drew plumes from
  u32 plume_tiles = 0;     // the dune tiles round the eye they came from
  u32 cloud_width = 0;     // the cloud march's texels a view (a quarter of its pixels a side)
  u32 cloud_height = 0;
  Vec2 cloud_drift_m;  // how far the wind has carried the layer, reduced to its period
  u32 fires = 0;       // the frame's fires the medium drew (R85)
  u32 near_fires = 0;  // of them, the flames the apply marched
};

// The GPU-mirrored blocks (shaders/medium.slang), pinned by tests/size_table.cpp.
struct MediumParams {
  Mat4 clip_to_eye;
  Mat4 clip_to_ray;
  u32 grid[4] = {};  // froxels x, y, z, the sources
  Vec4 slices;
  Vec4 sun;
  Vec4 sun_light;
  Vec4 ambient;
  Vec4 dust;
  Vec4 dust_albedo;
  Vec4 terms;
  u32 rect[4] = {};
  u32 source[4] = {};
  Vec4 plume_grid;
  Vec4 cloud;
  Vec4 cloud_origin;
  Vec4 cloud_size;
  Vec4 cloud_shadow;
  u64 visibility = 0;
  u64 froxels = 0;
  u64 sky = 0;
  u64 shadow_maps = 0;
  u64 plumes = 0;
  u64 plume_cells = 0;
  u64 clouds = 0;
  u64 cloud_shadow_map = 0;
  u64 fires = 0;  // MediumFire[fire_count] (R85)
  u32 fire_count = 0;
  u32 fire_near = 0;  // how many of them the apply marches as a near flame
  Vec4 fire_terms;    // x the smoke's albedo, y the near flames' steps
  u32 pad[12] = {};
};

inline constexpr u32 k_source_dust = 1u;
inline constexpr u32 k_source_plumes = 2u;
inline constexpr u32 k_source_clouds = 4u;
inline constexpr u32 k_source_shadows = 8u;
inline constexpr u32 k_source_fire = 16u;

// The medium of one scene, drawn by one renderer: `create` registers the frame pass, `destroy`
// removes it. Both between frames. The pass draws only while the renderer draws shaded frames.
class MediumPass {
 public:
  MediumPass() noexcept;
  ~MediumPass();
  ENGINE_NON_COPYABLE(MediumPass);

  bool create(const gfx::Device& device, renderer::SceneRenderer& renderer,
              const renderer::SceneData& scene, const MediumDesc& desc,
              const MediumOptions& options = {}, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return renderer_ != nullptr; }
  const MediumDesc& desc() const noexcept { return desc_; }
  const MediumStats& stats() const noexcept { return stats_; }

 private:
  static bool record_thunk(const renderer::FramePassContext& context, void* user,
                           std::string* error);
  bool record(const renderer::FramePassContext& context, std::string* error);
  bool ensure_buffers(const renderer::FramePassContext& context, std::string* error);

  const gfx::Device* device_ = nullptr;
  renderer::SceneRenderer* renderer_ = nullptr;
  MediumDesc desc_;
  MediumOptions options_;
  MediumStats stats_;
  u32 sources_ = 0;
  struct Pipelines;
  std::unique_ptr<Pipelines> pipelines_;
  Vector<gfx::BufferResource> params_;  // per frame slot: one MediumParams per view
  gfx::BufferResource froxels_;         // every view's grid, four halves a cell
  u32 views_ = 0;
  std::unique_ptr<renderer::TerrainSampler> ground_;
  const terrain::DuneField* field_ = nullptr;
  // The dust's ground: the terrain's height at the corners of the 32 m lattice round the eye, kept
  // so a frame asks the provider (whose query allocates) only for corners it has not seen.
  static constexpr u32 k_corners = 256;
  i64 corner_x_[k_corners] = {};
  i64 corner_z_[k_corners] = {};
  f64 corner_h_[k_corners] = {};
  u64 corner_used_[k_corners] = {};
  u64 corner_clock_ = 0;
  f64 corner_height(i64 x, i64 z);
  // The crest plumes: the tiles' crest lines by tile and hour, the query's scratch, and per frame
  // slot the segments and their binning grid (host-visible, read by the fill).
  // The far clouds: the drift's hourly sums, and per view the march's texels, and the map of their
  // transmittance towards the sun the froxels read.
  CloudDrift drift_;
  gfx::BufferResource clouds_;
  gfx::BufferResource cloud_shadow_;
  u32 cloud_w_ = 0;
  u32 cloud_h_ = 0;
  // The fires (R85): per frame slot, the frame's `MediumFire`s (host-visible, read by every pass).
  Vector<gfx::BufferResource> fire_buffers_;
  u32 build_fires(const renderer::FramePassContext& context, const MediumWind& wind,
                  u32& near_count);
  struct PlumeCache;
  std::unique_ptr<PlumeCache> plumes_;
  u32 build_plumes(const renderer::FramePassContext& context, const MediumWind& wind, Vec4& grid);
};

}  // namespace engine::volumetrics
