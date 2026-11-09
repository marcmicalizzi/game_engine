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
inline constexpr u32 k_sward_version = 6;
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

// G137, the stature rule (flora.md, "Growth forms by site"). What a cell's site gives it: the
// wetness (0..1), the slope in degrees, the shade (0..1) and two values of the mosaic noise (0..1).
struct SwardSite {
  f64 wet = 0, slope_deg = 0, shade = 0;
  f64 noise_tall = .5, noise_tussock = .5;
};
// The form the rule picks for a site: never ny. A pure function of its arguments.
SwardForm sward_site_form(const SwardStature& rule, const SwardSite& site) noexcept;
// Value noise in [0, 1] at (x_m, z_m) over a lattice of scale_m, its nodes' values hashes of
// seed and the node: smoothstep-bilinear, so it is continuous and its patches are the scale's.
f64 sward_mosaic_noise(u64 seed, f64 x_m, f64 z_m, f64 scale_m) noexcept;
}  // namespace engine::flora
