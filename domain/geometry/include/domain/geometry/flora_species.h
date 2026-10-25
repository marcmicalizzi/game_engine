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
  sward
};
struct FloraSpeciesInfo {
  FloraSpecies id;
  std::string_view name;
  bool profile;
  u32 triangle_budget;
  f32 compliance[4];
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
    // Temporary main's CPU pose: same blade compliance as Tuft; field posing is engine-side.
    {FloraSpecies::sward, "sward", false, 2400, {.20f, .20f, .20f, .20f}},
};
constexpr bool valid_flora_catalogue() {
  for (u32 i = 0; i < sizeof(k_flora_species) / sizeof(k_flora_species[0]); ++i) {
    const auto& info = k_flora_species[i];
    if (static_cast<u32>(info.id) != i || info.name.empty()) return false;
    if (info.profile && info.triangle_budget < 8000) return false;
    for (const f32 value : info.compliance)
      if (!(value > 0.0f && value <= 1.0f)) return false;
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
