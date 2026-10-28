#pragma once

// Species identifiers and motion traits carried by generated geometry. This contract remains
// readable without the optional flora solver (like ClusterFileMotionPart); growth stays in flora.
#include <core/base/types.h>

#include <string_view>

namespace engine::geometry {
enum class FloraSpecies : u8 {
  date_palm,
  broadleaf,
  conifer,
  birch,
  willow,
  orchard,
  ancient,
  shrub,
  fern,
  tuft,
  herb,
  maple,
  oak,
  elm,
  walnut,
  beech,
  ash,
  pine,
  spruce,
  cedar,
  juniper,
  aspen,
  fir,
  hemlock,
  alder,
  sward,
  cover,
  ivy,
  roots
};
// **How a species moves** (renderer.md, "The flora stage"; R95). `parts`: posed per part on the
// CPU, parent-first, each instance a run of the frame's part-pose table (the trees). `field`: posed
// on the GPU by the pool pass from the wind at the vertex, with no CPU state and no pose table —
// one parentless part per blade, its root the part's pivot (the sward, grass-sward.md's contract).
// `none`: a contract that does not move at all — the cover capability's moss and lichen mats
// (domain/cover), whose part 0 is a geometry-less root only so collision leaves them out. The
// renderer draws such a mesh rigid without a warning, rather than refusing it as a tree.
enum class FloraMotionKind : u8 { parts, field, none };
// **How a species casts** (renderer.md, "The caster policy"; R95). `traced`: in the ray tracing
// chain's records at all; `cascades`: how many of the sun's nearest cascades it is drawn into (0
// none, `k_flora_all_cascades` every one); `rest_pose`: a cascade draws it from the grid, never
// from the deformed-vertex pool. Three bytes, alignment 1.
inline constexpr u8 k_flora_all_cascades = 0xFF;
struct FloraCasterPolicy {
  bool traced = true;
  u8 cascades = k_flora_all_cascades;
  bool rest_pose = false;
};
struct FloraSpeciesInfo {
  FloraSpecies id;
  std::string_view name;
  bool profile;
  u32 triangle_budget;
  f32 compliance[4];
  // A tree's defaults: posed by parts, traced, in every cascade, casting its swaying pose.
  FloraMotionKind motion = FloraMotionKind::parts;
  FloraCasterPolicy casters{};
};
// Named profiles own their budgets here; legacy solvers retain their schema defaults (zero here).
inline constexpr FloraSpeciesInfo k_flora_species[] = {
    {FloraSpecies::date_palm, "date_palm", false, 0, {.012f, .10f, .12f, .14f}},
    {FloraSpecies::broadleaf, "broadleaf", false, 0, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::conifer, "conifer", false, 0, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::birch, "birch", false, 0, {.015f, .05f, .09f, .14f}},
    {FloraSpecies::willow, "willow", false, 0, {.006f, .03f, .08f, .16f}},
    {FloraSpecies::orchard, "orchard", false, 0, {.006f, .025f, .06f, .10f}},
    {FloraSpecies::ancient, "ancient", false, 0, {.0015f, .008f, .04f, .10f}},
    {FloraSpecies::shrub, "shrub", false, 0, {.01f, .07f, .10f, .14f}},
    {FloraSpecies::fern, "fern", false, 0, {.01f, .18f, .20f, .22f}},
    {FloraSpecies::tuft, "tuft", false, 0, {.01f, .20f, .22f, .26f}},
    {FloraSpecies::herb, "herb", false, 0, {.01f, .16f, .18f, .20f}},
    {FloraSpecies::maple, "maple", true, 200000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::oak, "oak", true, 400000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::elm, "elm", true, 1000000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::walnut, "walnut", true, 1000000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::beech, "beech", true, 900000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::ash, "ash", true, 400000, {.006f, .03f, .07f, .12f}},
    {FloraSpecies::pine, "pine", true, 60000, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::spruce, "spruce", true, 60000, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::cedar, "cedar", true, 80000, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::juniper, "juniper", true, 30000, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::aspen, "aspen", true, 300000, {.012f, .05f, .09f, .14f}},
    {FloraSpecies::fir, "fir", true, 60000, {.008f, .035f, .06f, .08f}},
    {FloraSpecies::hemlock, "hemlock", true, 80000, {.008f, .04f, .08f, .12f}},
    {FloraSpecies::alder, "alder", true, 400000, {.01f, .04f, .08f, .12f}},
    // Posed as a field on the GPU (R95), with Tuft's blade compliance (the leaf class, read once
    // per mesh). It casts into the nearest cascade alone, from its rest pose, and is never traced:
    // grass-sward.md's "Confirmed wire contract", the owner's policy of 2026-10-25.
    {FloraSpecies::sward,
     "sward",
     false,
     2400,
     {.20f, .20f, .20f, .20f},
     FloraMotionKind::field,
     {false, 1, true}},
    // The cover capability's mats (cover.md): they stand still, so no compliance and no sway; they
    // cast as anything rigid does.
    {FloraSpecies::cover, "cover", false, 0, {0.0f, 0.0f, 0.0f, 0.0f}, FloraMotionKind::none},
    // Hanging ivy stems use a bounded young-climber analogue: 2.8 times oak's compliance,
    // from 8600 / 3074 MPa (ivy-stem-mechanics.md). This is not a measured Hedera modulus.
    {FloraSpecies::ivy, "ivy", false, 0, {.0168f, .084f, .196f, .336f}},
    // Terrain-fitted tree roots are rigid, with no per-instance wind state.
    {FloraSpecies::roots, "roots", false, 12000, {0, 0, 0, 0}, FloraMotionKind::none},
};
constexpr bool valid_flora_catalogue() {
  for (u32 i = 0; i < sizeof(k_flora_species) / sizeof(k_flora_species[0]); ++i) {
    const auto& info = k_flora_species[i];
    if (static_cast<u32>(info.id) != i || info.name.empty()) return false;
    if (info.profile && info.triangle_budget < 8000) return false;
    for (const f32 value : info.compliance) {
      if (info.motion == FloraMotionKind::none ? value != 0.0f : !(value > 0.0f && value <= 1.0f))
        return false;
    }
    // A field species casts from a bounded set of cascades: one that cast its swaying pose into
    // every cascade would pay the pool pass once a cascade for every blade of grass.
    if (info.motion == FloraMotionKind::field && info.casters.cascades == k_flora_all_cascades)
      return false;
    for (u32 j = 0; j < i; ++j)
      if (k_flora_species[j].name == info.name) return false;
  }
  return true;
}
static_assert(valid_flora_catalogue(),
              "every species needs a unique name, budget and motion traits");
constexpr const FloraSpeciesInfo* flora_species_info(std::string_view name) {
  for (const auto& info : k_flora_species)
    if (info.name == name) return &info;
  return nullptr;
}
constexpr const FloraSpeciesInfo& flora_species_info(FloraSpecies id) {
  return k_flora_species[static_cast<u32>(id)];
}
}  // namespace engine::geometry
