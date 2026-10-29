#pragma once

// Named shrub architectures, cooked without per-frame state. See shrub-growth-forms.md.
#include <domain/flora/understory.h>
#include <domain/geometry/flora_species.h>

#include <span>
#include <string_view>

namespace engine::flora {
inline constexpr u32 k_shrub_growth_version = 1;
enum class ShrubForm : u8 { upright, spreading, arching, thicket, sparse };
// Cold botanical/profile metadata, never a per-instance or GPU record.
struct ShrubProfile {
  geometry::FloraSpecies species;
  ShrubForm form;
  f32 height_m;
  f32 height_min_m;
  f32 height_max_m;
  f32 reach_m;
  f32 stem_radius_m;
  f32 leaf_length_m;
  f32 leaf_width_m;
  f32 leaf_min_m;
  f32 leaf_max_m;
  f32 dead_fraction;
  u32 stems;
  u32 colonies;
  u32 branches;
  u32 twigs;
  u32 leaf_nodes;
  f32 twig_length_m;
  Vec4 bark;
  Vec4 leaf;
};
std::span<const ShrubProfile> shrub_profiles() noexcept;
const ShrubProfile* shrub_profile(std::string_view species) noexcept;
bool shrub_growth_in_range(std::string_view species, const ShrubGrowth& params, std::string& error);
bool shrub_growth_collides(std::string_view species, const ShrubGrowth& params) noexcept;
bool solve_shrub_growth(std::string_view species, const ShrubGrowth& params, assets::MeshData& mesh,
                        Motion& motion, TreeReport* report, std::string& error,
                        TreeLevels* levels = nullptr);
}  // namespace engine::flora
