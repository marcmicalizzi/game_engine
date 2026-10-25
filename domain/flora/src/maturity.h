#pragma once

#include "kit.h"

#include <domain/flora/flora.h>

namespace engine::flora::detail {
// Content-build state, not a per-frame type. Heights and radii are nominal woody dimensions.
struct MaturityState {
  f32 height = 0;
  f32 radius = 0;
  f32 mature = 0;
  f32 old = 0;
  f32 space = .5f;
  f32 vitality = 1;
};
bool maturity_in_range(std::string_view species, const Tree& tree, std::string& error);
MaturityState maturity_state(std::string_view species, const Tree& tree, f32 reference_height = 0,
                             f32 reference_radius = 0);
void mature_broadleaf(Broadleaf& tree, GrowthRules& rules, const Tree& control,
                      const MaturityState& age);
void mature_conifer(Conifer& tree, GrowthRules& rules, const Tree& control,
                    const MaturityState& age);
// A copy with native legacy parameters resolved; age zero is a no-op.
bool resolve_legacy_maturity(const FloraFile& source, FloraFile& out, std::string& error,
                             GrowthRules* resolved_rules = nullptr);
}  // namespace engine::flora::detail
