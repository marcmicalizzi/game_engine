#pragma once

// G90: pure build-time sward geometry and the baked source of cards and ground.
#include <domain/flora/impostor.h>
#include <domain/flora/motion.h>
#include <domain/flora/trees.h>

#include <schemas/flora.h>
#include <span>
#include <string>
#include <string_view>

namespace engine::flora {
// 6 (G137): the tussock layout, the raised crown base, the between-crown turf and two profiles.
// 7 (G138): the stature rule's transition band, and the slope's arctangent the sward's own.
// 8 (G141): the blade's albedo a live grass leaf's measured visible one.
inline constexpr u32 k_sward_version = 8;
struct SwardProfile {
  std::string_view name;
  f32 length_min_m, length_max_m, width_min_m, width_max_m;
  // Zero means no species-specific field measurement found, not zero biological density.
  u32 tillers_min_m2, tillers_max_m2;
  std::string_view seed_head;
};
std::span<const SwardProfile> sward_profiles() noexcept;
const SwardProfile* sward_profile(std::string_view name) noexcept;
bool sward_in_range(const Sward& params, std::string& error);
bool solve_sward(const Sward& params, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion, TreeLevels* levels = nullptr,
                 Vector<geometry::ClusterFileSwardVertex>* attributes = nullptr);
// Orthographic top view of the same rest mesh. Uncovered pixels retain alpha zero;
// the cook combines them with its explicitly chosen soil, rather than inventing blade coverage.
void bake_sward_ground(const assets::MeshData& mesh, const TreeLevels& levels, f32 cell_m,
                       u32 pixels, BakedAtlas& out, Vector<f32>* height = nullptr);
// Fraction of a common orthographic frame covered from four azimuths at 30 degrees elevation.
// Rows: near, coarse, cards; columns: 0,90,180,270 degrees. Cards use their authored cutoff.
struct SwardCoverage {
  f32 fraction[3][4]{};
};
SwardCoverage measure_sward_coverage(const assets::MeshData& mesh, const TreeLevels& levels);
// The same at another elevation of the view, degrees (G136: the card against the near form as the
// view grazes; grass-sward.md, "Coverage against elevation").
SwardCoverage measure_sward_coverage_at(const assets::MeshData& mesh, const TreeLevels& levels,
                                        f64 elevation_deg);

// G137, the stature rule (flora.md, "Growth forms by site"). What a cell's site gives it: the
// wetness (0..1), the slope in degrees, the shade (0..1) and two values of the mosaic noise (0..1).
// G138 (flora.md, "Where two forms meet"): the two noises' gradient magnitudes there, per metre,
// which turn the band's metres into score, and two uniforms in [0, 1) from the cell's hash that
// draw its form inside the band. A gradient of zero leaves the score's sign to decide, as G137 did.
struct SwardSite {
  f64 wet = 0, slope_deg = 0, shade = 0;
  f64 noise_tall = .5, noise_tussock = .5;
  f64 gradient_tall = 0, gradient_tussock = 0;
  f64 draw_tall = .5, draw_tussock = .5;
};
// The arctangent the stature rule's slope and the patch's lean are measured with: fdlibm's, in
// plain IEEE arithmetic, so a form decided on a slope is the same bits on every toolchain (a
// builder asks the C library for no decision). Within 2 ulp of `std::atan` (the forms tests hold
// it).
f64 sward_atan(f64 x) noexcept;
// The form the rule picks for a site: never `any`. A pure function of its arguments.
SwardForm sward_site_form(const SwardStature& rule, const SwardSite& site) noexcept;
// Value noise in [0, 1] at (x_m, z_m) over a lattice of scale_m, its nodes' values hashes of
// seed and the node: smoothstep-bilinear, so it is continuous and its patches are the scale's.
f64 sward_mosaic_noise(u64 seed, f64 x_m, f64 z_m, f64 scale_m) noexcept;
// The same value, to the bit, and its gradient (per metre) from the same four nodes: the derivative
// of the smoothstep-bilinear interpolation, no further hash and no further sample (G138).
f64 sward_mosaic_noise_gradient(u64 seed, f64 x_m, f64 z_m, f64 scale_m, f64& d_x,
                                f64& d_z) noexcept;
// G138: the share of a smoothstep's unit over which it rises from 5 % to 95 %, 2u with
// u^3 - 0.75 u + 0.225 = 0 (smoothstep(1/2 + u) = 0.95): how the band's metres become the ramp's.
inline constexpr f64 k_sward_band_span = 0.7292992756568325;

// ---- G141, the ground under the sward (flora.md, "The ground under the sward") -----------------
// What lies under a sward between its blades, as shares of the floor that sum to one: standing and
// fallen dead leaves (thatch), bryophytes and bare soil (grass-sward.md, "The floor under a
// sward").
struct SwardFloorMix {
  f64 thatch = 0, moss = 0, soil = 0;
};
// The floor a form stands in: a tussock in thatch and soil, a sod on a closed mat of thatch and
// moss. `wet` and `shade` (0..1, the stature rule's own inputs) move share from soil and thatch to
// moss. A pure function; the shares sum to one.
SwardFloorMix sward_floor_mix(SwardForm form, f64 wet = 0, f64 shade = 0) noexcept;
// The floor under a cell, from the stature rule's own inputs: the form `sward_site_form` picks
// there, and the same site's wetness and shade.
SwardFloorMix sward_site_floor(const SwardStature& rule, const SwardSite& site) noexcept;
// The form a recipe draws, for a bake that is not told: a tussock where it has a crown spacing,
// tall where its leaves' grown length is 0.2 m or more (the stature rule's tall forms' range).
SwardForm sward_recipe_form(const Sward& params) noexcept;
// One part of the floor: its linear albedo and roughness, or a pack role's images in their place
// (sRGB colour; glTF metallic-roughness, G the roughness), repeating every `tile_m`.
struct SwardFloorPart {
  Vec3 colour{};
  f32 roughness = 0.9f;
  const image::Image* colour_image = nullptr;
  const image::Image* roughness_image = nullptr;
  f32 tile_m = 1.0f;
};
struct SwardFloor {
  SwardFloorMix mix;
  SwardFloorPart thatch, moss, soil;
  u64 seed = 1;
  // The leaf area index of the real canopy the drawn blades stand for: it shades the floor, its
  // undrawn leaves lie baked over the floor, and it hides the floor from afar (grass-sward.md). The
  // drawn swards carry a tenth to a hundredth of it.
  f64 canopy_lai = 2;
  // The share of the canopy that is standing dead leaf (a tussock's straw), which tints its far
  // colour.
  f64 standing_dead = 0;
};
// The procedural floor's parts at their stated albedos and roughness (grass-sward.md), in `form`'s
// shares, with the form's canopy leaf area and standing dead.
SwardFloor sward_default_floor(SwardForm form);
// Visible albedos, linear, as band means (grass-sward.md, "The canopy's colour"): a single live
// grass leaf (the drawn blade's material), a closed green grass canopy, and a canopy of standing
// dead leaf.
inline constexpr f32 k_sward_leaf_albedo[3] = {.055f, .12f, .035f};
inline constexpr f64 k_sward_canopy_albedo[3] = {.04, .085, .025};
inline constexpr f64 k_sward_dead_canopy_albedo[3] = {.16, .135, .085};
// The ground's two colours (flora.md, "Near and far"). Near the eye the image: the undrawn live
// leaves, darker with depth, over the floor in the canopy's shade, under the drawn blades' top
// view. Past the sward's reach `far_colour`, the real canopy seen at a low elevation, which the
// cook writes into the scene's layer as its `far_color`.
struct SwardGround {
  u32 pixels = 0;
  Vector<u8> colour;     // RGBA8, RGB sRGB, opaque
  Vector<u8> normal;     // RGBA8, OpenGL tangent space
  Vector<u8> roughness;  // R8, linear
  Vector<u8> height;     // R8, linear, of the profile's longest leaf
  // R8, what the texel is: 0 thatch, 1 moss, 2 soil, 3 drawn blade (bake cover >= 1/2), 4 a baked
  // live leaf (its cover >= 1/2).
  Vector<u8> part;
  Vec3 near_mean{};      // linear, of the image
  Vec3 far_colour{};     // linear, the canopy's colour seen at k_sward_far_elevation_deg
  f64 blade_cover = 0;   // the drawn blades' mean cover from above
  f64 live_target = 0;   // the gap law's cover for the undrawn leaf area, 1 - exp(-0.5 L)
  f64 live_cover = 0;    // the baked live leaves' achieved mean cover from above
  f64 transmission = 1;  // the canopy's diffuse transmission to the floor
  f64 view_cover = 0;    // the real canopy's cover at k_sward_far_elevation_deg
  f64 floor_share[3]{};  // the parts' achieved shares of the floor texels
};
// The canopy's transmission to the floor: exp(-k LAI), k = 0.5, floored at 0.25. The bake takes the
// larger of the drawn sward's leaf area index and the floor's `canopy_lai`.
inline constexpr f64 k_sward_extinction = 0.5;
inline constexpr f64 k_sward_transmission_floor = 0.25;
// The elevation the far colour sees the canopy at: the lowest the card is held to the near form at
// (G136), and steeper than a walker's view of the ground past the sward's reach (under 4 degrees).
inline constexpr f64 k_sward_far_elevation_deg = 10;
f64 sward_canopy_transmission(f64 leaf_area_index) noexcept;
// The sward's top view composed over `floor` at `pixels` a side (one cell): colour, normal,
// roughness and height are the floor's between blades and the blade's on a blade, by the bake's
// cover. Deterministic: hashes, IEEE arithmetic, a sort, and the engine's own exp, log, sin, cos.
bool bake_sward_ground_over(const Sward& params, const assets::MeshData& mesh,
                            const TreeLevels& levels, f64 leaf_area_index, const SwardFloor& floor,
                            u32 pixels, SwardGround& out, std::string* error);
}  // namespace engine::flora
