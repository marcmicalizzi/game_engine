#pragma once

#include <domain/cover/host.h>

namespace engine::cover {

// Recipe-owned ecology in the host frame. Last matching region wins. No material-name inference.
const HostHabitat& habitat_at(const CoverGrowth& params, P3 point);
bool habitat_in_range(const CoverGrowth& params, std::string& error);
// Hard eligibility, reapplied after smoothing and at emission; never a rendering roughness test.
bool substrate_allows(const HostHabitat& habitat, const CoverGrowth& params, u32 kind);
// A dimensionless establishment weight, not a predicted germination probability.
f64 nurse_log_suitability(const HostHabitat& habitat, const Exposure& exposure, f64 damp);

}  // namespace engine::cover
