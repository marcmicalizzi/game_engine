#pragma once

// Static juvenile growth and damage; sources and authored constants: flora-regeneration.md.
#include <domain/flora/trees.h>
#include <domain/geometry/flora_species.h>

#include <span>
#include <string_view>

namespace engine::flora {
inline constexpr u32 k_regeneration_version = 1;
// Cold profile metadata, never stored per scene instance.
struct RegenerationProfile {
  geometry::FloraSpecies species;
  f32 leaf_length_m;
  f32 leaf_width_m;
  f32 radius_per_height;
  f32 lateral_reach;
  f32 lateral_angle_deg;
  u32 branches_per_node;
  u32 basal_stems;
  u32 triangle_budget;
  bool opposite;
  bool evergreen;
  bool basal_recovery;
};
std::span<const RegenerationProfile> regeneration_profiles() noexcept;
const RegenerationProfile* regeneration_profile(std::string_view species) noexcept;
bool regeneration_in_range(std::string_view species, const Regeneration& params,
                           std::string& error);
bool regeneration_collides(std::string_view species, const Regeneration& params) noexcept;
bool solve_regeneration(std::string_view species, const Regeneration& params,
                        assets::MeshData& mesh, Motion& motion, TreeReport* report,
                        std::string& error, TreeLevels* levels = nullptr);
}  // namespace engine::flora
