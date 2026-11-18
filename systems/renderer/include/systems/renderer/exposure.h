#pragma once

// **The eye's adaptation: the frame exposed for what the eye is looking at** (docs/subsystems/
// renderer.md, "Exposure: the eye's adaptation"; docs/experiments/eye-adaptation.md; roadmap R168
// and R57's first leg). The sky's rule (sky.h, `gfx::sky_exposure_ev100`) meters the light that
// falls at the eye from the sky, whether the eye is out under it or in a room with one door, so a
// room at noon is exposed for noon and drawn nearly black. With adaptation on, a frame pass of the
// renderer's own (`FramePassStage::Exposure`, src/exposure_pass.h) meters the frame itself — a
// histogram of the luminance the resolve wrote, centre-weighted over the view the eye looks
// through — turns the reading into a target exposure through the eye's partial compensation, and
// moves the frame's exposure towards it at the eye's speeds; the encode multiplies the picture by
// the difference between that exposure and the one the resolve drew at.
//
// **Off unless a scene or a tunable turns it on** (`Sky.exposure: Adapted`, `renderer.exposure.
// adapt`), and a fixed exposure value — the scene's `Fixed`, a run's `--exposure-ev100`, a frame's
// `ExposureRequest::fixed` — pins it whatever the switch: so a capture, a test and a benchmark can
// hold the exposure, and every picture drawn without it is the bytes it was (no pass is
// registered).
//
// This header is the model on the CPU — the same arithmetic the pass's shader does
// (shaders/exposure.slang), so the tests and a host's summary can say what the GPU should have
// reached — and the options the tunables give.

#include <core/base/types.h>

namespace engine::renderer {

// ---- the histogram ---------------------------------------------------------------------------
//
// log2 of the luminance in cd/m^2, in quarter stops from 2^-10 (about 0.001 cd/m^2, a moonless
// night's ground) to 2^22 (4 million: the sun's disc and anything brighter is the last bin).
inline constexpr u32 k_exposure_bins = 128;
inline constexpr f32 k_exposure_log2_min = -10.0f;
inline constexpr f32 k_exposure_bins_per_stop = 4.0f;
// One pixel in a 2 x 2 tile is metered: a quarter of the reads, the same reading to the bin on any
// picture whose features are wider than a pixel.
inline constexpr u32 k_exposure_stride = 2;
// **Centre-weighted metering**: a camera's (Durand and Dorsey 2000, section 4.2, after Nikon's
// centre-weighted meter: three quarters of the weight in the 12 mm circle at the centre of a 24 x
// 36 mm frame). The disc is a third of the view's width across, and a pixel in it weighs
// `k_exposure_centre_weight` against 1 outside it: on a 16:9 view the disc is 15.5% of the area, so
// 16 puts 74.6% of the weight in it.
inline constexpr u32 k_exposure_centre_weight = 16;
inline constexpr f32 k_exposure_centre_diameter = 1.0f / 3.0f;
// A scene with no sky has no light in physical units: its radiances are the stand-in's, where a
// white surface under the sun is about 1. Its picture is taken as a sunlit scene's at the rule's
// noon, EV100 15, so the model below meters it in the same units.
inline constexpr f32 k_exposure_stand_in_ev100 = 15.0f;

// The options (the tunables `renderer.exposure.*`; renderer.md, "Exposure: the eye's
// adaptation"), read when a renderer is made.
struct ExposureOptions {
  bool adapt = false;
  // The share of the metered weight the reading averages over, lowest first: the darkest 10% and
  // the brightest 10% (a doorway to noon, the sun, a lamp) are left out of the average.
  f32 low = 0.10f;
  f32 high = 0.90f;
  // The exposure's floor and ceiling, EV100: how far up the darkest frame is lifted and how far
  // down the brightest is taken.
  f32 min_ev100 = -6.0f;
  f32 max_ev100 = 18.0f;
  // The time constants, seconds. Darkening the picture is the eye's light adaptation, as fast as
  // the eye's (Durand and Dorsey 2000, section 3.3: 0.1 s for the cones, 0.4 s for the rods,
  // blended by the rods' share of vision, Krawczyk 2007 eq. 4.12). Brightening it is dark
  // adaptation, which in the eye takes minutes for the cones and forty for the rods; a game cannot
  // keep a room black for minutes, so it is a choice, slower than the light case so a door still
  // reads dark first.
  f32 dark_tau_s = 1.5f;
  f32 cone_tau_s = 0.1f;
  f32 rod_tau_s = 0.4f;
};

// The options from the tunables.
ExposureOptions exposure_options_from_tunables();

// ---- the model (exposure.slang does the same) --------------------------------------------------

// **The key**: the display value the adapting luminance is shown at, from the luminance itself in
// cd/m^2 (Krawczyk, Myszkowski and Seidel 2005; Krawczyk 2007 eq. 4.11): 0.18 is a photograph's
// middle grey, and the eye's key falls as the light does — about 0.03 under a full moon, 0.42 in a
// room lit to 100 lux, 0.72 on sunlit sand — so a night stays dark and sand stays bright where a
// reflected-light meter would draw both grey.
f64 exposure_key(f64 luminance) noexcept;
// The rods' share of vision at a luminance, 0.04 / (0.04 + L) (Krawczyk 2007 eq. 4.7, after Hunt).
f64 exposure_rod_share(f64 luminance) noexcept;
// **The exposure the eye settles at**, EV100, for an adapting luminance: the one that shows it at
// its key. The renderer's exposure shows a luminance L at L pi / (2.5 2^EV100) (sky.h,
// `gfx::sky_exposure_scale`), so EV100 = log2(pi L / (2.5 key(L))). Then `compensation` stops are
// taken off (positive brighter) and the result is held within the options' floor and ceiling.
f64 exposure_target_ev100(f64 luminance, f64 compensation, const ExposureOptions& options) noexcept;
// The time constant towards `target` from `current` (EV100) when the adapting luminance is
// `luminance`: dark adaptation's when the picture brightens (the target is lower), light
// adaptation's blended by the rods' share when it darkens.
f64 exposure_tau_s(f64 current, f64 target, f64 luminance, const ExposureOptions& options) noexcept;
// One step of `dt_s` seconds: an exponential approach in stops (Pattanaik et al. 2000; Durand and
// Dorsey 2000; Krawczyk 2007 eq. 4.5), so a step is the same however the time is cut into frames.
f64 exposure_step(f64 current, f64 target, f64 luminance, f64 dt_s,
                  const ExposureOptions& options) noexcept;

// **Which view is metered**: the one the eye looks through, whose rectangle holds the target's
// centre — the only view of a single layout, the centre monitor of a surround (so the side
// monitors, the periphery, never pump the frame's exposure), the whole of a Panini view.
struct ExposureViewRect {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
};
u32 exposure_metered_view(const ExposureViewRect* views, u32 count, u32 target_width,
                          u32 target_height) noexcept;
// The weight of the pixel at `(px, py)` of the target in a metered view's rectangle: the centre's
// disc or the rest.
u32 exposure_pixel_weight(const ExposureViewRect& view, u32 px, u32 py) noexcept;

// ---- what a frame reached ----------------------------------------------------------------------

// The pass's state as the GPU left it (exposure.slang mirrors it; 32 bytes).
struct ExposureState {
  f32 ev100 = 0.0f;         // the exposure the frame was shown at
  f32 target_ev100 = 0.0f;  // where it is going
  f32 luminance = 0.0f;     // the metered adapting luminance, cd/m^2
  f32 base_ev100 = 0.0f;    // the exposure the resolve drew at (the sky's rule, or a fixed one)
  f32 scale = 1.0f;         // what the encode multiplied the picture by: 2^(base - ev100)
  u32 frames = 0;           // frames metered since the pass began (0: none yet)
  u32 weight = 0;           // the histogram's total weight that frame
  u32 pinned = 0;           // 1: a fixed exposure held the frame, nothing was adapted
};
static_assert(sizeof(ExposureState) == 32);

}  // namespace engine::renderer
