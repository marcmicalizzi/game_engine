#pragma once

// **The meadow community** (roadmap G140, with G139; flora.md, "The meadow community"; the
// grounding in docs/experiments/meadow-community.md). A montane hay meadow is not one forb: its
// species sort along the moisture gradient from a stream's wet margin to a dry sunny slope, and
// along the light gradient to a woodland's shaded edge. This header is the rule, pure and testable:
//
// - **The niche** (`meadow_niche`): a cell's site (`scene_gen::SiteSample`: the distances to water,
//   the slope, the heat load, the curvature) and the paint's wetness and shade become a moisture F
//   and a light L on Ellenberg's scales as recalibrated for Britain (Hill et al. 1999), so the
//   species' published indicator values are their optima as read.
// - **The covers** (`meadow_cover`): each species' cover is its greatest cover (a Domin class's
//   midpoint) times a Gaussian response on F and on L (ter Braak and Looman 1986: an optimum, a
//   tolerance, a maximum), quantized to 1/65536 and, where the sum passes one, scaled down in
//   integers so it never does.
// - **The stature rule's wetness** (`meadow_wetness`): the sward's form reads the same F, so the
//   tussock stands where the rushes and the marsh marigold do.
//
// **Determinism.** The response's exponential is `det::exp` (core/math/deterministic.h); everything
// else is IEEE `+ - * /` with contraction off, then integers, so a cover is the same on every
// toolchain and from every tile (a cell's site is a function of its point, never of its tile).

#include <core/base/types.h>
#include <domain/scene_gen/site.h>

#include <schemas/flora.h>
#include <span>
#include <string_view>

namespace engine::flora {

// What a species is to the sward: a forb stands among the grass and flowers; a sedge, a rush and a
// dwarf shrub are the community's other layers, drawn by the same rule from their own mesh.
enum class MeadowRole : u8 { forb, sedge, rush, dwarf_shrub };

// One species of the table. `moisture` and `light` are Hill et al. (1999)'s final F and L, read in
// the text; `cover` is a Domin class's midpoint, chosen from the constancy the grounding page
// records; the height, the flowers and their colour are chosen (the page says which) and only
// drive the stand-in herb recipe.
struct MeadowSpeciesTraits {
  std::string_view name;
  MeadowRole role;
  f64 moisture;
  f64 light;
  f64 cover;
  f64 height_m;
  u32 flowers;  // heads a plant (0: a sedge's, a rush's or a shrub's, drawn as none)
  u32 petals;   // petal cards a head (a clover's florets, an umbel's flowers)
  f64 petal_m;  // a petal card's length
  f64 petal_angle_deg;
  f32 flower_rgb[3];  // linear
  // G167: the radius of one plant's crown, metres (chosen from the floras' leaf and branch spans,
  // the grounding page says which): what a drift spaces its plants by, so a cover becomes a count.
  f64 crown_m;
  // G167: the months it flowers, first and last, 1..12 (the British flora's; 0 and 0 for a species
  // drawn with no heads).
  u32 bloom_from;
  u32 bloom_to;
};
std::span<const MeadowSpeciesTraits> meadow_species() noexcept;
const MeadowSpeciesTraits* meadow_species(std::string_view name) noexcept;

// The Gaussian's tolerances, in the scales' own units (chosen: the grounding page says why).
inline constexpr f64 k_meadow_moisture_tolerance = 1.0;
inline constexpr f64 k_meadow_light_tolerance = 1.5;
// How far from its optimum a species stands at all, in tolerances (the niche's two axes together):
// the Gaussian holds 99.7 % of a species' occurrences within three, and past them it is cut to 0.
inline constexpr f64 k_meadow_reach = 3.0;
// The wet margin's F: Ellenberg's "wet-site indicator, often on water-saturated, badly aerated
// soils", where the stature rule's wetness is 1.
inline constexpr f64 k_meadow_wet_margin = 9.0;
// A cover of one, in the integers covers are kept in.
inline constexpr u32 k_meadow_cover_one = 65536u;

struct MeadowNiche {
  f64 moisture = 5.0;  // F
  f64 light = 7.5;     // L
};
// The niche at a site. `wet` is the paint's wetness or a stream's nearness, 0..1 (the stature
// rule's G137 input, kept as a floor where the scene has a cooked wet band but no water entry);
// `shade` the shade layer's weight, 0..1. A pure function.
MeadowNiche meadow_niche(const MeadowRule& rule, const scene_gen::SiteSample& site, f64 wet = 0,
                         f64 shade = 0) noexcept;
// The stature rule's wetness, 0..1, from the niche's F: 0 at or below `moisture_base`, 1 at the wet
// margin's 9.
f64 meadow_wetness(const MeadowRule& rule, const MeadowNiche& niche) noexcept;
// One species' response, 0..1: exp(-(F - u_F)^2 / 2 t_F^2 - (L - u_L)^2 / 2 t_L^2), and 0 past
// `k_meadow_reach` tolerances.
f64 meadow_response(const MeadowSpeciesTraits& species, const MeadowNiche& niche) noexcept;
// The covers of `species` at the niche, in 1/65536, into `out` (the same length); the sum, at most
// `k_meadow_cover_one`, is returned.
u32 meadow_cover(std::span<const MeadowSpeciesTraits* const> species, const MeadowNiche& niche,
                 std::span<u32> out) noexcept;
// The species' stand-in mesh recipe: a herb of the table's height with its flowers at their colour
// (flat-coloured until leaf and flower sets exist), or, for a sedge, a rush or a dwarf shrub, a
// herb of leaves alone. `seed` varies the plant. G167: a forb out of its flowering months
// (`meadow_in_bloom`) stands in leaf, its heads left off.
Herb meadow_herb(const MeadowSpeciesTraits& species, u64 seed = 1, u32 day_of_year = 182);
// G167: whether the species carries its heads on `day_of_year` (1..366): within its flowering
// months, first day of the first to last day of the last, in a common year.
bool meadow_in_bloom(const MeadowSpeciesTraits& species, u32 day_of_year) noexcept;

// **The drift** (G167; flora.md, "The meadow at whole density"). A cover is the share of the ground
// the species' crowns cover, so a cell the species holds is a drift of its plants at its crown's
// spacing: `meadow_drift_count` plants, `round(cell area / (pi crown^2))` held to
// 1..`k_meadow_drift_max`, on a disc of `crown sqrt(count)`. A cell draws the drift of species i
// with probability cover_i over the drift's own covered share of the cell, `meadow_drift_scale`,
// in 1/65536 (the sum scaled down in integers where it passes one), so a species still covers its
// cover's share of the ground and stands at its crown's density there.
inline constexpr u32 k_meadow_drift_max = 8;
u32 meadow_drift_count(const MeadowSpeciesTraits& species, f64 cell_m) noexcept;
f64 meadow_drift_radius(const MeadowSpeciesTraits& species, f64 cell_m) noexcept;
// The factor, in 1/65536, a cover is multiplied by to give the probability of the drift.
u32 meadow_drift_scale(const MeadowSpeciesTraits& species, f64 cell_m) noexcept;
// The probabilities of each species' drift at a niche, in 1/65536, into `out`; their sum, at most
// `k_meadow_cover_one`, is returned. `scales` are `meadow_drift_scale`'s, one a species.
u32 meadow_drift_odds(std::span<const MeadowSpeciesTraits* const> species,
                      std::span<const u32> scales, const MeadowNiche& niche,
                      std::span<u32> out) noexcept;
// The near drift's recipe (species `meadow`): `meadow_drift_count` of the stand-in herb at the
// drift's radius, each head's petal cards held to `k_meadow_drift_petals` with its coloured area
// kept, and the coarsest stem that reads.
inline constexpr u32 k_meadow_drift_petals = 8;
MeadowPlant meadow_drift(const MeadowSpeciesTraits& species, f64 cell_m, u64 seed = 1,
                         u32 day_of_year = 182);

// **The far form** (flora.md, "The meadow community"): past the near ring one plant in
// `meadow_far_thin` of a flowering species' stands, as a leafless stem carrying the heads of that
// many plants, each head with fewer, larger petal cards of the same coloured area, so a tile's
// flowers keep their colour and their density at a fraction of the instances and triangles. The
// most heads a herb carries, and the most plants a far form stands for:
inline constexpr u32 k_meadow_far_heads = 40;
inline constexpr u32 k_meadow_far_thin = 4;
// How many cells' drifts a species' far form stands for: 0 for a species with no flowers or out of
// bloom (it has none), else 40 over its heads a plant, held to 1..4.
u32 meadow_far_thin(const MeadowSpeciesTraits& species, u32 day_of_year = 182) noexcept;
// G167: the far form carries the heads of `thin` cells' drifts of `count` plants each: the heads of
// `thin` plants on one stem, each head's coloured area `count` times a plant's, its petal cards
// lengthened to at most the herb's 8 cm and then more of them, so the area is kept.
Herb meadow_far_herb(const MeadowSpeciesTraits& species, u64 seed = 1, u32 count = 1,
                     u32 day_of_year = 182);
// The far form as a field (species `meadow`): one plant, the far herb.
MeadowPlant meadow_far_drift(const MeadowSpeciesTraits& species, f64 cell_m, u64 seed = 1,
                             u32 day_of_year = 182);

}  // namespace engine::flora
