#pragma once

#include <domain/flora/trees.h>

#include <string_view>

namespace engine::flora::detail {
bool tree_root_dimensions(std::string_view species, const Tree& tree, f32& radius, f32& reach,
                          std::string& error);
bool is_tree_species(std::string_view species);
bool is_evergreen_species(std::string_view species);
bool tree_in_range(std::string_view species, const Tree& tree, std::string& error);
bool solve_tree(std::string_view species, const Tree& tree, assets::MeshData& mesh,
                TreeReport* report, std::string& error, Motion& motion, TreeLevels* levels);
}  // namespace engine::flora::detail
