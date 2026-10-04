#pragma once

// The ground's detail (shaders/ground_detail.slang; docs/subsystems/gfx.md, "The ground's detail";
// docs/subsystems/renderer.md, "The sand close up"): wind ripples and grain on a terrain's sand,
// **a function and not an image**. It is evaluated per pixel from the surface point's world (x, z),
// the scene's seed and the wind, with no texture coordinates, no tiles and nothing that meets at an
// edge, so it has no seam anywhere a function of position has none — across a cluster, an LOD cut,
// a ring of the terrain or a chunk of one — and it does not repeat.
//
// **The ripples are phasor noise** (Tricard, Efremov, Zanni, Neyret, Martínez, Lefebvre,
// "Procedural Phasor Noise", SIGGRAPH 2019): a sum of complex Gabor kernels on a jittered lattice,
// each a sinusoid oriented along the wind (within `spread` of it) under a smooth bell, whose
// **argument** is the pattern's phase. Chosen over a sum of real Gabor kernels because the sum's
// contrast wanders wherever kernels cancel, and its profile can only be a sinusoid; the phase is a
// field of constant contrast that any periodic profile can be laid on — here an asymmetric one, a
// long gentle windward slope and a short steep lee — and its **singularities**, where the kernels
// cancel, are exactly the defects real ripples have: a crest that ends, two crests joining one. How
// often that happens is the kernels' size in wavelengths (a narrower spectrum is longer, straighter
// crests), which is what `GroundDetailDesc::ripple_defects` sets. The amplitude tapers where the
// sum is small, so a defect is a crest fading out rather than a point where every phase meets.
//
// **The filter** makes it look natural at a distance: the ripples fade by how many of their
// wavelengths one pixel spans along the wind, measured from the reconstruction's own derivatives
// per screen axis (a grazing footprint is long and thin, and only its extent across the crests
// aliases), in two stages — the profile's harmonics first, the asymmetric profile easing to a
// plain sinusoid between eight pixels a wavelength and four, since its steep lee is a quarter of a
// wavelength wide; then the sinusoid, between four pixels and two (`ground_fade`) — and the slope
// variance each stage takes moves into the GGX roughness (`alpha^2 += sigma^2 - drawn sigma^2`),
// so distant sand is rougher, not a mirror. The grain fades the same way by its cells.
//
// The block below is what the resolve and the path tracer read through an address
// (`ResolveParams::ground_detail`, `PathTraceParams::ground_detail`); a material draws the detail
// when it has `k_material_ground_detail` and the address is not zero. `ground_detail_block` turns a
// scene's numbers into it, and `domain/gfx/tests/ground_detail_reference.h` evaluates the same
// function in double precision on the CPU.

#include <core/base/types.h>
#include <core/math/math.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::gfx {

// GroundDetailParams::flags.
inline constexpr u32 k_ground_ripples = 1u;  // the phasor ripples perturb the shading normal
inline constexpr u32 k_ground_grain = 2u;    // the grain varies the albedo and the roughness
// The ripples fade on ground that faces away from the wind (`lee_tan_start`/`end`): exposure.
inline constexpr u32 k_ground_exposure = 4u;
// The grain is gradient noise in `grain_octaves` octaves (and may lean the normal, `grain_normal`)
// rather than the first pass's two octaves of value noise.
inline constexpr u32 k_ground_gradient_grain = 8u;
// Grainflow streaks down the fall line where the ground falls away from the wind near the angle of
// repose (`streak_*`).
inline constexpr u32 k_ground_streaks = 16u;
// The ripples' wavelength follows the ground's climb into the wind (`spacing_*`).
inline constexpr u32 k_ground_spacing = 32u;
// Third pass. Grainflow lanes down a slip face (`flow_*`), long and seen by their relief.
inline constexpr u32 k_ground_grainflow = 64u;
// The ripples' wavelength, height and defects vary in patches across the world (`patch_*`).
inline constexpr u32 k_ground_patches = 128u;
// The ripples' direction is the wind turned along the contours of a slope oblique to it.
inline constexpr u32 k_ground_steering = 256u;
// The ripples travel with the transport, fade by their travel per frame, and flatten in a storm.
inline constexpr u32 k_ground_motion = 512u;

// Gradient noise in one dimension as the grainflow lanes draw it (a lattice point's gradient
// uniform in [-1, 1], the quintic fade, each lane's gradient windowed along the fall line by its
// own start and length) has a value of root-mean-square `k_ground_flow_rms` and a derivative of
// `k_ground_flow_slope_rms` per lane width, after the four cells' blend: measured over the mirror
// (ground_detail_tests.cpp, "grainflow lanes …"), and the block scales by them.
inline constexpr f32 k_ground_flow_rms = 0.213f;
inline constexpr f32 k_ground_flow_slope_rms = 0.723f;
// The tongues fade by the pixel's footprint against `k_ground_flow_body` lane widths, whole at four
// pixels a period and gone at two, as every term of the detail is (fifth pass,
// docs/experiments/sand-fifth-pass-2026-10-04.md). A tongue's relief is its lobe's two flanks,
// which span the lobe's width: 0.4 of a lane width for a split lobe, up to 1.3 at a wide toe. The
// number is the instrument's (ground_detail_tests.cpp, "a slip face draws …"): at a whole lane
// width (the fourth pass) the pixels drew up to a fifth more variation than 8 x 8 samples of each
// hold, at 15 cm, and with the levees a third more; at half a lane width the tongues were gone at
// 15 cm where the samples still held them at 3% of contrast; at 0.75 the drawn variation stays
// within a tenth of the held at every size from 3 to 40 cm.
inline constexpr f32 k_ground_flow_body = 0.75f;
// The ripples' travel reaches the shader reduced modulo this many base wavelengths, in double
// precision on the CPU, so a float carries it to a ten-thousandth of a wavelength; where the
// spacing or the patches scale the wavelength, the phase steps once as the reduction wraps, which
// at a centimetre a minute is once in about three game days (renderer.md, "Ripples that move").
inline constexpr f64 k_ground_travel_period = 256.0;

// Streak kernels per lattice cell of `streak_length`: three cover about four fifths of a slip face
// at the default width, so tongues touch and cross but leave sand between them.
inline constexpr u32 k_ground_streak_impulses = 3;

// The most octaves the grain takes: a 2 cm grain down to 0.2 mm is seven.
inline constexpr u32 k_ground_max_grain_octaves = 8;
// Gradient noise as the grain draws it (a corner's gradient five bits a component in [-1, 1], the
// quintic fade) has a value of root-mean-square 0.182 and a gradient of 0.743 per cell over
// (x, z), measured over the mirror (ground_detail_tests.cpp, "the grain reads as sand"). The
// grain's albedo and roughness channels are scaled to `k_ground_grain_rms` over all its octaves,
// which is what the first pass's two octaves of value noise had, so `grain_albedo` means what it
// meant; its height to a slope of `grain_normal` root-mean-square.
inline constexpr f32 k_ground_noise_rms = 0.182f;
inline constexpr f32 k_ground_noise_slope_rms = 0.743f;
inline constexpr f32 k_ground_grain_rms = 0.28f;

// Kernels per lattice cell, mirrored in the shader. A point sums the 3 x 3 cells round it, eighteen
// kernels, of which about a third reach it (a kernel's disc is pi R^2 of the 9 R^2 searched): with
// one a cell the sum leans on the lattice, a cell's single kernel being most of the field at its
// centre, and every further one is six more kernels a pixel.
inline constexpr u32 k_ground_impulses = 2;

// The taper's floor, as a share of the sum's root-mean-square magnitude: `m = |S|^2 / (|S|^2 +
// (k |S|_rms)^2)`. Where the kernels cancel, `m` takes the ripple down with them, so a defect is a
// crest fading out; above about twice `k`, the ripple stands at nearly its full height.
inline constexpr f32 k_ground_taper = 0.35f;

// The share of the profile's slope variance the phasor pattern has, measured over the pattern
// itself (domain/gfx/tests/ground_detail_tests.cpp, the first case: E|grad h|^2 over 160,000
// points is 1.00 of this at the default numbers, 1.00 at the sinusoid the filter eases to, 1.05
// with no defects and 1.08 with the most): the taper lowers the ripples near every defect, and the
// phase's gradient is longer than the wavelength says near them. The roughness transfer moves this
// much, so the rule moves the variance the pattern has, not the variance a perfect ripple would.
inline constexpr f32 k_ground_slope_share = 0.65f;

// **The frame the pattern is evaluated in** (2026-10-04; docs/subsystems/gfx.md, "Far from the
// origin"). The detail is a function of the world's (x, z), but a float32 world coordinate 420 km
// out has a step of 3.1 cm — four to a 12 cm ripple — and every term's lattice, hash and phase
// would be taken there. So the shader never sees an absolute coordinate: a point reaches it as its
// offset from the frame's **origin**, a corner of a `k_ground_frame_cell` grid near the eye that
// a float holds exactly, and every lattice the pattern draws from comes with where it stands at
// that origin — the cell the origin falls in, as the integers the hashes take, and the origin's
// offset into it — worked out here in double, where 10,000 km is still a nanometre. The shader adds
// the integers to the cell its small local coordinate falls in, so a lattice point is the world's
// own whichever origin the frame has, and the offset from a kernel's centre (all a phase is ever
// taken of) is a difference of two small numbers.
inline constexpr f64 k_ground_frame_cell = 1024.0;
// The grainflow's sixteen fixed lane directions (`ground_lane_set`).
inline constexpr u32 k_ground_flow_directions = 16;

// Where a lattice of side `size` stands at the frame's origin A: the cell A falls in,
// `floor(A / size)` as 32-bit integers (wrapped, as the shader's integers and the hashes take
// them), and A's offset into it, in [0, size). A lattice whose side is this one's halved n times
// stands at the same offset, in the cell the integers times 2^n, which is how the grain's octaves,
// the value grain's quarter and the patches' half share one.
struct GroundLattice {
  i32 base_x = 0;
  i32 base_z = 0;
  f32 rem_x = 0.0f;
  f32 rem_z = 0.0f;
};
static_assert(sizeof(GroundLattice) == 16);

// Where one of the grainflow's directions stands at the frame's origin A: `lane`, the lane whose
// line is at or before A across the direction (`floor(dot(A, across) / spacing)`), and `across`,
// A's distance past that line; `segment`, the segment A falls in along the direction
// (`floor(dot(A, dir) / flow_length)`), and `along`, A's distance into it. The directions are
// irrational, so these are not a lattice's integers times anything; each is its own.
struct GroundLane {
  i32 lane = 0;
  i32 segment = 0;
  f32 across = 0.0f;
  f32 along = 0.0f;
};
static_assert(sizeof(GroundLane) == 16);

// Mirrors GroundDetail in shaders/ground_detail.slang, with the lane table behind it. 560 bytes:
// the first pass's 80, the second's grain, streaks and spacing (128), the third's grainflow and its
// episodes, patches, steering and motion (224), and the frame (304, then the sixteen lanes' 256).
// The shader's struct ends at `lanes`; it reads the table through the block's address
// (`k_ground_lanes_offset`), where the one lane a pixel needs is one load.
struct GroundDetailParams {
  // xy: the direction the sand moves, (x, z), unit: the ripples' phase grows along it, so a crest's
  // gentle side faces up the wind and its lee down it. z: sin of the kernels' orientation spread.
  Vec4 wind{1.0f, 0.0f, 0.0f, 0.0f};
  f32 wavelength = 0.12f;  // metres, crest to crest
  f32 amplitude = 0.004f;  // metres: half the crest-to-trough height
  f32 asymmetry = 0.75f;   // the windward share of a wavelength, (0, 1)
  f32 cell = 0.5f;  // metres: a kernel's radius, and the side of the lattice cells they sit in
  // `(k_ground_taper |S|_rms)^2`, the taper's floor, with `|S|_rms^2 = k_ground_impulses pi / 7`
  // for the kernel `(1 - r^2/R^2)^3` at `k_ground_impulses` a cell of side R.
  f32 magnitude_floor = 0.11f;
  // The ripples' slope variance at full height, `E|grad h|^2`, is `slope_scale (1/a + 1/(1 - a))`
  // at asymmetry a (`ground_slope_scale`): what the filter moves into the roughness as the profile
  // eases to a sinusoid and then fades.
  f32 slope_scale = 0.0f;
  // The ripples' fade by the ground's slope, as the shading normal's y: all of them at or above
  // `slope_cos_start`, none at or below `slope_cos_end` (a slip face at the angle of repose is
  // avalanched smooth).
  f32 slope_cos_start = 1.0f;
  f32 slope_cos_end = 0.0f;
  f32 grain_size = 0.02f;      // metres: the coarse octave's cell; the fine one's is a quarter
  f32 grain_albedo = 0.0f;     // the albedo's variation, a share of it either way
  f32 grain_roughness = 0.0f;  // the perceptual roughness's, either way
  u32 seed = 0;                // the scene's: what every kernel and lattice value is drawn from
  u32 flags = 0;               // k_ground_ripples | k_ground_grain | k_ground_exposure
  // The exposure (k_ground_exposure): the ground's fall along the wind, `dot(n.xz, wind) / n.y`
  // (the tangent of its slope down the wind: negative climbing into it, positive falling away),
  // at which the ripples start to go, and at and past which there are none.
  f32 lee_tan_start = 0.0f;
  f32 lee_tan_end = 0.0f;
  // The gradient grain (k_ground_gradient_grain): octaves from `grain_size` halving down to the
  // scene's finest, and the root-mean-square slope its height leans the normal by.
  u32 grain_octaves = 0;
  f32 grain_normal = 0.0f;
  // The streaks (k_ground_streaks): the ground's fall along the wind (as `lee_tan_*`) at which they
  // start and at which they are whole; a tongue's width and length, metres; and what the sum of the
  // tongues, scaled to unit root-mean-square here, moves: the albedo (a share), the roughness, and
  // the normal (`streak_slope`, the sum's gradient to a root-mean-square slope of `streak_normal`).
  f32 streak_tan_start = 0.0f;
  f32 streak_tan_full = 0.0f;
  f32 streak_width = 0.4f;
  f32 streak_length = 2.0f;
  f32 streak_albedo = 0.0f;
  f32 streak_roughness = 0.0f;
  f32 streak_slope = 0.0f;
  f32 streak_normal = 0.0f;
  // The spacing (k_ground_spacing): the wavelength and the height are scaled by `clamp(1 + gain
  // climb, min, max)`, climb being the ground's rise into the wind (minus its fall along it).
  f32 spacing_gain = 0.0f;
  f32 spacing_min = 1.0f;
  f32 spacing_max = 1.0f;
  // Third pass. Grainflow (k_ground_grainflow): the fall along the wind where the lanes start and
  // are whole (as the streaks'), a lane's segment (the longest a tongue runs) and a tongue's mean
  // width, metres, the normal's lean per unit of the tongues' gradient (to a root-mean-square
  // slope of `flow_normal`), and the albedo's share per unit of their height. (The word after
  // `flow_albedo` was the third pass's widening, retired by the fourth.)
  f32 flow_tan_start = 0.0f;
  f32 flow_tan_full = 0.0f;
  f32 flow_length = 24.0f;
  f32 flow_width = 0.6f;
  f32 flow_slope = 0.0f;
  f32 flow_normal = 0.0f;
  f32 flow_albedo = 0.0f;
  f32 flow_retired = 0.0f;
  // Patches (k_ground_patches): the noise's coarse cell, metres; the wavelength's scale is
  // `patch_mid × 2^(v patch_half_log2)` for v in [-1, 1], and the kernels' spread `1 + v'
  // patch_defects` of the scene's.
  f32 patch_size = 30.0f;
  f32 patch_mid = 1.0f;
  f32 patch_half_log2 = 0.0f;
  f32 patch_defects = 0.0f;
  // Steering (k_ground_steering): the share of the wind's component along the fall line the slope
  // turns aside per unit of its tangent, and the turn's clamp as its cosine and sine. Motion
  // (k_ground_motion): the ripples' height kept (1, falling to 0 in a storm), their travel along
  // the wind, metres reduced modulo `k_ground_travel_period` wavelengths, and their travel in one
  // frame.
  f32 steer_gain = 0.0f;
  f32 steer_cos_max = 1.0f;
  f32 steer_sin_max = 0.0f;
  f32 ripple_live = 1.0f;
  f32 travel = 0.0f;
  f32 travel_per_frame = 0.0f;
  u32 pad6 = 0;
  u32 pad7 = 0;
  // The grainflow's episodes: the share of lanes present at once (1: every lane, always), the
  // avalanche clock — the ground's transport path length times the scene's turnover, reduced to a
  // fraction of a cycle in double — and its advance in one frame.
  f32 flow_share = 1.0f;
  f32 flow_clock = 0.0f;
  f32 flow_clock_step = 0.0f;
  u32 pad8 = 0;
  // Fourth: the frame (`ground_detail_frame`; above). `origin_x`, `origin_z`: its origin, metres, a
  // multiple of `k_ground_frame_cell` (exact in a float to 2^34 m). The lattices of the ripples'
  // kernels (`cell`), the grain (`grain_size`), the patches (`patch_size`) and the streaks
  // (`streak_length`) at it, and the grainflow's sixteen directions. All zero is the frame at the
  // world's origin, which is right for any eye and exact for one near the origin.
  f32 origin_x = 0.0f;
  f32 origin_z = 0.0f;
  u32 pad9 = 0;
  u32 pad10 = 0;
  GroundLattice ripple_lattice;
  GroundLattice grain_lattice;
  GroundLattice patch_lattice;
  GroundLattice streak_lattice;
  GroundLane lanes[k_ground_flow_directions];
};
static_assert(sizeof(GroundDetailParams) == 560);
static_assert(sizeof(GroundDetailParams) % 16 == 0, "the block is read as float4 rows on the GPU");
// Where the shader finds the lane table behind its `GroundDetail` (shaders/ground_detail.slang's
// `k_ground_lanes_offset`).
inline constexpr u64 k_ground_lanes_offset = 304;
static_assert(offsetof(GroundDetailParams, lanes) == k_ground_lanes_offset);

// The scene's numbers (the renderer's `engine.scene.TerrainDetail`): what a scene says, in metres
// and degrees, before the mechanism's own constants are derived from them.
struct GroundDetailDesc {
  f32 ripple_wavelength = 0.12f;  // metres crest to crest
  f32 ripple_height = 0.008f;     // metres crest to trough; 0 draws no ripples
  f32 ripple_asymmetry = 0.75f;   // the windward share of a wavelength
  // How often crests end and join, [0, 1]: it sets the kernels' radius from six wavelengths at 0
  // (long straight crests) to one and a half at 1, and their spread about the wind from 2 to 20
  // degrees.
  f32 ripple_defects = 0.35f;
  f32 slope_start_deg = 22.0f;  // the ground's slope at which the ripples start to fade
  f32 slope_end_deg = 30.0f;    // and past which there are none
  // The ripples' exposure to the wind: on ground falling away from it, they start to go at a lee
  // slope of `lee_start_deg` along the wind and are gone at `lee_end_deg`. Both 0 (the default):
  // no exposure, the first pass's ripples, which fade by steepness alone.
  f32 lee_start_deg = 0.0f;
  f32 lee_end_deg = 0.0f;
  f32 grain_size = 0.02f;
  f32 grain_albedo = 0.08f;
  f32 grain_roughness = 0.05f;
  // The grain's finest octave, metres: gradient noise in octaves from `grain_size` halving down to
  // it. 0 (the default) is the first pass's two octaves of value noise, `grain_size` and a quarter.
  f32 grain_finest = 0.0f;
  // The root-mean-square slope the grain's height leans the shading normal by; 0 none.
  f32 grain_normal = 0.0f;
  // Grainflow streaks on a slip face: on ground falling away from the wind they start at a lee
  // slope of `streak_start_deg` along it and are whole at `streak_full_deg`; both 0 (the default)
  // is none. Tongues `streak_width` across and `streak_length` long down the fall line, moving the
  // albedo by `streak_albedo` (a share, root-mean-square), the roughness by `streak_roughness` and
  // the normal by a root-mean-square slope of `streak_normal`.
  f32 streak_start_deg = 0.0f;
  f32 streak_full_deg = 0.0f;
  f32 streak_width = 0.4f;
  f32 streak_length = 2.0f;
  f32 streak_albedo = 0.06f;
  f32 streak_roughness = 0.04f;
  f32 streak_normal = 0.012f;
  // The ripples' spacing follows the wind: the wavelength (and the height with it, so a ripple
  // keeps its shape) is scaled by `1 + spacing_gain × climb`, the ground's rise into the wind as a
  // tangent, clamped to [spacing_min, spacing_max] — longer where the flow speeds up climbing a
  // windward slope. 0 (the default) is the scene's wavelength everywhere.
  f32 spacing_gain = 0.0f;
  f32 spacing_min = 0.8f;
  f32 spacing_max = 1.6f;
  // Third pass; every term is off by default, which draws the second pass to the bit.
  // Grainflow on a slip face, in place of the streaks: on ground falling away from the wind the
  // lanes start at a lee slope of `flow_start_deg` and are whole at `flow_full_deg` (both 0: none).
  // Lanes `flow_width` across in sixteen fixed directions, the fall line choosing between them
  // (fourth pass; `flow_cell` and `flow_widening` are retired, kept so a scene naming them reads);
  // seen by their relief, a root-mean-square slope of `flow_normal`, and barely by colour,
  // `flow_albedo` of the albedo.
  f32 flow_start_deg = 0.0f;
  f32 flow_full_deg = 0.0f;
  f32 flow_cell = 10.0f;
  f32 flow_width = 0.6f;
  f32 flow_normal = 0.05f;
  f32 flow_albedo = 0.015f;
  f32 flow_widening = 0.3f;
  // The tongues (version 4): each lane is a chain of grainflow tongues along its direction, each
  // running up to `flow_length` metres — a chute with levees at its head, a lobe widening to a
  // rounded toe — with gaps between, so some reach the toe of a face and some stop part-way.
  f32 flow_length = 24.0f;
  // The lanes are episodes, not a pattern: a lane avalanches when sand has piled at the brink and
  // is buried again by grainfall, so only `flow_share` of them are present at once (1, the
  // default: all, always), and each runs through `flow_turnover` cycles for every square metre of
  // sand the ground's wind carries across a metre of width (0: they never change). With no wind
  // the face stands as it is.
  f32 flow_share = 1.0f;
  f32 flow_turnover = 0.0f;
  // Patches: a slow noise of position in two octaves, the coarse one `patch_size` metres (0: none),
  // scaling the ripples' wavelength and height together within [patch_min, patch_max] and their
  // kernels' spread about the wind by up to `patch_defects` of it either way.
  f32 patch_size = 0.0f;
  f32 patch_min = 0.7f;
  f32 patch_max = 1.4f;
  f32 patch_defects = 0.5f;
  // Steering: the ripples' direction is the wind with `min(steer_gain tan(slope), 1/2)` of its
  // component along the ground's fall line taken out, turned at most `steer_max_deg` (0: none).
  f32 steer_max_deg = 0.0f;
  f32 steer_gain = 2.0f;
  // Motion (the renderer fills the travel; `ground_detail_motion`): the ripples travel
  // `ripple_celerity` metres for every square metre of sand the ground's wind moves across a metre
  // of width (0: they stand), and flatten between `flatten_start` and `flatten_end` of the wind
  // record's mean strength (0 and 0: never).
  f32 ripple_celerity = 0.0f;
  f32 flatten_start = 0.0f;
  f32 flatten_end = 0.0f;
};

// The gradient grain's octaves for a coarsest cell and a finest: `grain_size` halving until the
// next would be under `grain_finest` (a thousandth of slack, so 2 cm to 1.25 mm is five), at most
// k_ground_max_grain_octaves; 0 when there is no finest, the first pass's grain.
inline u32 ground_grain_octaves(f32 grain_size, f32 grain_finest) noexcept {
  if (!(grain_finest > 0.0f) || !(grain_size > 0.0f)) return 0;
  u32 n = 1;
  f32 size = grain_size;
  while (n < k_ground_max_grain_octaves && size * 0.5f >= grain_finest * 0.999f) {
    size *= 0.5f;
    ++n;
  }
  return n;
}

// The kernels' radius and spread for a defect density, and the ripples' slope variance: the
// arithmetic the block carries so the shader does not repeat it per pixel.
inline f32 ground_cell(const GroundDetailDesc& desc) noexcept {
  const f32 d = std::clamp(desc.ripple_defects, 0.0f, 1.0f);
  return desc.ripple_wavelength * (6.0f + (1.5f - 6.0f) * d);
}
inline f32 ground_spread_rad(const GroundDetailDesc& desc) noexcept {
  const f32 d = std::clamp(desc.ripple_defects, 0.0f, 1.0f);
  return radians(2.0f + (20.0f - 2.0f) * d);
}
// The profile's `E|dh/ds|^2` along the wind for a ripple of half-height `a` and wavelength `l`:
// the warped cosine rises over the windward share `w` and falls over the rest, so its slope is
// `a 2 pi sin(2 pi u) / (2 w l)` on the one and `/ (2 (1 - w) l)` on the other, and sin^2 averages
// a half over each: `(pi a / l)^2 (1/w + 1/(1 - w)) / 2`. The pattern has `k_ground_slope_share`
// of it. This is the factor in front of `(1/w + 1/(1 - w))`, which the shader completes for the
// asymmetry it draws.
inline f32 ground_slope_scale(f32 half_height, f32 wavelength) noexcept {
  const f64 s =
      3.14159265358979323846 * static_cast<f64>(half_height) / static_cast<f64>(wavelength);
  return static_cast<f32>(s * s * 0.5 * static_cast<f64>(k_ground_slope_share));
}
inline f32 ground_slope_variance(const GroundDetailParams& d, f32 asymmetry) noexcept {
  return d.slope_scale * (1.0f / asymmetry + 1.0f / (1.0f - asymmetry));
}

// The block for a scene's numbers, the wind's direction over (x, z) — any length; normalized here,
// and +x when it is zero — and the scene's seed.
inline GroundDetailParams ground_detail_block(const GroundDetailDesc& desc, Vec2 wind,
                                              u32 seed) noexcept {
  GroundDetailParams out;
  const f32 length = std::sqrt(wind.x * wind.x + wind.y * wind.y);
  const Vec2 w = length > 1e-12f ? Vec2{wind.x / length, wind.y / length} : Vec2{1.0f, 0.0f};
  out.wind = Vec4{w.x, w.y, std::sin(ground_spread_rad(desc)), 0.0f};
  out.wavelength = desc.ripple_wavelength;
  out.amplitude = 0.5f * desc.ripple_height;
  out.asymmetry = std::clamp(desc.ripple_asymmetry, 0.05f, 0.95f);
  out.cell = ground_cell(desc);
  const f32 rms2 = static_cast<f32>(k_ground_impulses) * k_pi / 7.0f;
  out.magnitude_floor = k_ground_taper * k_ground_taper * rms2;
  out.slope_scale = ground_slope_scale(out.amplitude, out.wavelength);
  out.slope_cos_start = std::cos(radians(desc.slope_start_deg));
  out.slope_cos_end = std::cos(radians(desc.slope_end_deg));
  out.grain_size = desc.grain_size;
  out.grain_albedo = desc.grain_albedo;
  out.grain_roughness = desc.grain_roughness;
  out.seed = seed;
  out.flags = (desc.ripple_height > 0.0f ? k_ground_ripples : 0u) |
              (desc.grain_albedo > 0.0f || desc.grain_roughness > 0.0f ? k_ground_grain : 0u);
  out.grain_octaves = ground_grain_octaves(desc.grain_size, desc.grain_finest);
  if (out.grain_octaves > 0) {
    out.grain_normal = desc.grain_normal;
    if (desc.grain_albedo > 0.0f || desc.grain_roughness > 0.0f || desc.grain_normal > 0.0f)
      out.flags |= k_ground_grain | k_ground_gradient_grain;
  }
  if (desc.streak_full_deg > desc.streak_start_deg && desc.streak_width > 0.0f &&
      desc.streak_length > 0.0f) {
    out.flags |= k_ground_streaks;
    out.streak_tan_start = std::tan(radians(desc.streak_start_deg));
    out.streak_tan_full = std::tan(radians(desc.streak_full_deg));
    out.streak_width = desc.streak_width;
    out.streak_length = desc.streak_length;
    // The tongues' sum has `E S^2 = k / (3 L^2) pi a b / 7` for k kernels a cell of side L, each an
    // amplitude uniform in [-1, 1] under `(1 - r^2)^3` over an ellipse of half-axes a = L/2 and
    // b = W/2, and `E |grad S|^2 = k / (3 L^2) 0.6 pi a b (1/a^2 + 1/b^2)`; the block scales the
    // sum by the first's root and its gradient by the second's (ground_detail_tests.cpp measures
    // both).
    const f64 a = 0.5 * static_cast<f64>(desc.streak_length);
    const f64 b = 0.5 * static_cast<f64>(desc.streak_width);
    const f64 density =
        static_cast<f64>(k_ground_streak_impulses) /
        (3.0 * static_cast<f64>(desc.streak_length) * static_cast<f64>(desc.streak_length));
    const f64 pi = 3.14159265358979323846;
    const f64 rms = std::sqrt(density * pi * a * b / 7.0);
    const f64 slope_rms = std::sqrt(density * 0.6 * pi * a * b * (1.0 / (a * a) + 1.0 / (b * b)));
    out.streak_albedo = static_cast<f32>(static_cast<f64>(desc.streak_albedo) / rms);
    out.streak_roughness = static_cast<f32>(static_cast<f64>(desc.streak_roughness) / rms);
    out.streak_slope = static_cast<f32>(static_cast<f64>(desc.streak_normal) / slope_rms);
    out.streak_normal = desc.streak_normal;
  }
  if (desc.flow_full_deg > desc.flow_start_deg && desc.flow_width > 0.0f &&
      desc.flow_length > 0.0f) {
    out.flags |= k_ground_grainflow;
    out.flow_tan_start = std::tan(radians(desc.flow_start_deg));
    out.flow_tan_full = std::tan(radians(desc.flow_full_deg));
    out.flow_length = desc.flow_length;
    out.flow_width = desc.flow_width;
    out.flow_slope = desc.flow_normal / k_ground_flow_slope_rms;
    out.flow_normal = desc.flow_normal;
    out.flow_albedo = desc.flow_albedo / k_ground_flow_rms;
    out.flow_share = std::clamp(desc.flow_share, 0.0f, 1.0f);
  }
  if (desc.patch_size > 0.0f && desc.patch_max >= desc.patch_min && desc.patch_min > 0.0f) {
    out.flags |= k_ground_patches;
    out.patch_size = desc.patch_size;
    out.patch_mid = std::sqrt(desc.patch_min * desc.patch_max);
    out.patch_half_log2 = 0.5f * std::log2(desc.patch_max / desc.patch_min);
    out.patch_defects = desc.patch_defects;
  }
  if (desc.steer_max_deg > 0.0f && desc.steer_gain > 0.0f) {
    out.flags |= k_ground_steering;
    out.steer_gain = desc.steer_gain;
    out.steer_cos_max = std::cos(radians(desc.steer_max_deg));
    out.steer_sin_max = std::sin(radians(desc.steer_max_deg));
  }
  if (desc.spacing_gain != 0.0f && desc.spacing_max >= desc.spacing_min) {
    out.flags |= k_ground_spacing;
    out.spacing_gain = desc.spacing_gain;
    out.spacing_min = desc.spacing_min;
    out.spacing_max = desc.spacing_max;
  }
  if (desc.lee_end_deg > desc.lee_start_deg) {
    out.flags |= k_ground_exposure;
    out.lee_tan_start = std::tan(radians(desc.lee_start_deg));
    out.lee_tan_end = std::tan(radians(desc.lee_end_deg));
  }
  return out;
}

// The ripples' motion for a frame: `travel_m`, how far they have moved along the wind since the
// ground's epoch (double: it grows without bound), `per_frame_m`, how far in this frame, and the
// wind's strength over the record's mean, which flattens them between the scene's two numbers.
// Sets k_ground_motion; the travel is reduced modulo `k_ground_travel_period` base wavelengths
// here, in double, so the float the shader reads keeps a ten-thousandth of a wavelength.
// `moved_m2` and `moved_per_frame_m2` are the ground's transport path length and its step this
// frame, which drive the grainflow's episodes (reduced to a fraction of a cycle here, in double).
inline void ground_detail_motion(GroundDetailParams& d, const GroundDetailDesc& desc, f64 travel_m,
                                 f64 per_frame_m, f32 strength, f64 moved_m2 = 0.0,
                                 f64 moved_per_frame_m2 = 0.0) noexcept {
  if (desc.flow_turnover > 0.0f) {
    const f64 cycles = moved_m2 * static_cast<f64>(desc.flow_turnover);
    d.flow_clock = static_cast<f32>(cycles - std::floor(cycles));
    d.flow_clock_step =
        static_cast<f32>(std::fabs(moved_per_frame_m2 * static_cast<f64>(desc.flow_turnover)));
  }
  if (!(desc.ripple_celerity > 0.0f) && !(desc.flatten_end > desc.flatten_start)) return;
  d.flags |= k_ground_motion;
  const f64 period = k_ground_travel_period * static_cast<f64>(d.wavelength);
  f64 t = std::fmod(travel_m, period);
  if (t < 0.0) t += period;
  d.travel = static_cast<f32>(t);
  d.travel_per_frame = static_cast<f32>(std::fabs(per_frame_m));
  d.ripple_live = 1.0f;
  if (desc.flatten_end > desc.flatten_start) {
    const f32 x = std::clamp(
        (strength - desc.flatten_start) / (desc.flatten_end - desc.flatten_start), 0.0f, 1.0f);
    d.ripple_live = 1.0f - x * x * (3.0f - 2.0f * x);
  }
}

// `floor(v / step)` and `v` less that many steps, in [0, step), for a `v` up to 2^53 steps: the
// remainder by a fused multiply-add, which is `v - n step` rounded once — exact here, since the two
// agree in their leading bits (std::fma, ADR-0035: the one rounding is the point) — and moved into
// the range where a quotient rounded across an integer would leave it outside.
inline void ground_reduce(f64 v, f64 step, f64& n, f64& rem) noexcept {
  n = std::floor(v / step);
  rem = std::fma(-n, step, v);
  if (rem < 0.0) {
    n -= 1.0;
    rem += step;
  } else if (rem >= step) {
    n += 1.0;
    rem -= step;
  }
}

// A cell's index as the shader's 32-bit integers carry it: the low 32 bits of the integer, which is
// what a hash of it reads (two's complement, as the GPU's integer arithmetic wraps).
inline i32 ground_wrap(f64 n) noexcept {
  return static_cast<i32>(static_cast<u32>(static_cast<u64>(static_cast<i64>(n))));
}

// A lattice of side `size` at the frame's origin (ax, az); all zero for a lattice of no size.
inline GroundLattice ground_lattice_at(f64 ax, f64 az, f32 size) noexcept {
  GroundLattice out;
  if (!(size > 0.0f)) return out;
  const f64 s = static_cast<f64>(size);
  f64 nx = 0.0, nz = 0.0, rx = 0.0, rz = 0.0;
  ground_reduce(ax, s, nx, rx);
  ground_reduce(az, s, nz, rz);
  out.base_x = ground_wrap(nx);
  out.base_z = ground_wrap(nz);
  // A remainder that rounds up to the side itself is the next cell's zero.
  out.rem_x = static_cast<f32>(rx);
  out.rem_z = static_cast<f32>(rz);
  if (!(out.rem_x < size)) {
    out.rem_x = 0.0f;
    out.base_x = ground_wrap(nx + 1.0);
  }
  if (!(out.rem_z < size)) {
    out.rem_z = 0.0f;
    out.base_z = ground_wrap(nz + 1.0);
  }
  return out;
}

// The frame for an eye at world (eye_x, eye_z): its origin, the corner of the `k_ground_frame_cell`
// grid nearest the eye — so a point near the eye is within 512 m of it, where a float's step is
// 61 µm — and every lattice and lane direction at that origin, in double. A function of the
// origin alone, so it changes only when the eye crosses into another cell, and then the pattern
// does not move: the integers and offsets name the same world lattice from the new corner (the
// renderer's "no seam where the frame's cell changes" test). Called once a frame with the eye the
// resolve's `camera` is, after the block is made; the reference path tracer's likewise. The eye is
// the camera's own float position, so it is exact in double and so is everything here.
inline void ground_detail_frame(GroundDetailParams& d, f64 eye_x, f64 eye_z) noexcept {
  const f64 g = k_ground_frame_cell;
  const f64 ax = std::floor(eye_x / g + 0.5) * g;
  const f64 az = std::floor(eye_z / g + 0.5) * g;
  d.origin_x = static_cast<f32>(ax);
  d.origin_z = static_cast<f32>(az);
  d.ripple_lattice = ground_lattice_at(ax, az, d.cell);
  d.grain_lattice = ground_lattice_at(ax, az, d.grain_size);
  d.patch_lattice = ground_lattice_at(ax, az, d.patch_size);
  d.streak_lattice = ground_lattice_at(ax, az, d.streak_length);
  const f64 spacing = 2.0 * static_cast<f64>(d.flow_width);
  const f64 seg = static_cast<f64>(d.flow_length);
  for (u32 k = 0; k < k_ground_flow_directions; ++k) {
    GroundLane& lane = d.lanes[k];
    lane = GroundLane{};
    if (!(spacing > 0.0) || !(seg > 0.0)) continue;
    // The direction as the mirror takes it, in double (the shader's float one differs by 1e-8,
    // which it only ever multiplies by a local coordinate).
    const f64 angle = static_cast<f64>(k) * (6.28318530717958647692 / 16.0);
    const f64 dx = std::cos(angle);
    const f64 dz = std::sin(angle);
    f64 m = 0.0, j = 0.0, across = 0.0, along = 0.0;
    ground_reduce(ax * -dz + az * dx, spacing, m, across);
    ground_reduce(ax * dx + az * dz, seg, j, along);
    lane.lane = ground_wrap(m);
    lane.segment = ground_wrap(j);
    lane.across = static_cast<f32>(across);
    lane.along = static_cast<f32>(along);
  }
}

}  // namespace engine::gfx
