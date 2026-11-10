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
inline constexpr u32 k_sward_version = 7;
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
}  // namespace engine::flora
