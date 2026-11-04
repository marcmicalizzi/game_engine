#pragma once

// Alpine growth forms, cooked through flora's existing mesh registry. Botanical dimensions,
// authored sampling choices and planting support: docs/experiments/alpine-growth-forms.md.
#include <domain/flora/understory.h>
#include <domain/geometry/flora_species.h>

#include <span>
#include <string_view>

namespace engine::flora {
inline constexpr u32 k_alpine_version = 2;
enum class AlpineForm : u8 { cushion, rosette, dwarf_scrub };
// Cold profile metadata; no new per-frame or GPU record.
struct AlpineProfile {
  geometry::FloraSpecies species;
  AlpineForm form;
  f32 radius_m;
  f32 height_m;
  f32 leaf_length_m;
  f32 leaf_width_m;
  u32 shoots;
  Vec4 leaf;
};
std::span<const AlpineProfile> alpine_profiles() noexcept;
const AlpineProfile* alpine_profile(std::string_view species) noexcept;
bool alpine_in_range(std::string_view species, const Alpine& params, std::string& error);
bool solve_alpine(std::string_view species, const Alpine& params, assets::MeshData& mesh,
                  Motion& motion, TreeReport* report, std::string& error,
                  TreeLevels* levels = nullptr);
}  // namespace engine::flora
