#include <domain/cover/growth.h>
#include <domain/cover/habitat.h>

#include <cmath>

namespace engine::cover {
namespace {
f64 ramp(f64 lo, f64 hi, f64 x) {
  const f64 t = x <= lo ? 0.0 : (x >= hi ? 1.0 : (x - lo) / (hi - lo));
  return t * t * (3.0 - 2.0 * t);
}
bool valid(const HostHabitat& h) {
  return static_cast<u8>(h.substrate) <= static_cast<u8>(Substrate::sealed) && h.decay >= 0.0f &&
         h.decay <= 1.0f && h.rooting_depth_m >= 0.0f && h.rooting_depth_m <= 1.0f &&
         h.bryophyte_depth_m >= 0.0f && h.bryophyte_depth_m <= 1.0f;
}
}  // namespace

const HostHabitat& habitat_at(const CoverGrowth& p, P3 point) {
  for (u32 i = p.regions.size(); i > 0; --i) {
    const HabitatRegion& r = p.regions[i - 1];
    if (point.x >= static_cast<f64>(r.min.x) && point.x <= static_cast<f64>(r.max.x) &&
        point.y >= static_cast<f64>(r.min.y) && point.y <= static_cast<f64>(r.max.y) &&
        point.z >= static_cast<f64>(r.min.z) && point.z <= static_cast<f64>(r.max.z))
      return r.habitat;
  }
  return p.habitat;
}

bool habitat_in_range(const CoverGrowth& p, std::string& error) {
  if (!valid(p.habitat)) {
    error = "cover.habitat has an invalid substrate, decay or depth";
    return false;
  }
  if (static_cast<u8>(p.lichen_substrate) > static_cast<u8>(LichenSubstrate::wood)) {
    error = "cover.lichen_substrate is invalid";
    return false;
  }
  for (u32 i = 0; i < p.regions.size(); ++i) {
    const HabitatRegion& r = p.regions[i];
    if (!valid(r.habitat) || !std::isfinite(r.min.x) || !std::isfinite(r.min.y) ||
        !std::isfinite(r.min.z) || !std::isfinite(r.max.x) || !std::isfinite(r.max.y) ||
        !std::isfinite(r.max.z) || r.min.x >= r.max.x || r.min.y >= r.max.y || r.min.z >= r.max.z) {
      error = "cover.regions[" + std::to_string(i) + "] has invalid bounds or habitat";
      return false;
    }
  }
  return true;
}

bool substrate_allows(const HostHabitat& h, const CoverGrowth& p, u32 kind) {
  const Substrate s = h.substrate;
  if (s == Substrate::sealed) return false;
  if (kind == k_fungus)
    return s == Substrate::legacy ? p.dead_wood : s == Substrate::dead_wood && h.decay > 0.0f;
  if (kind != k_lichen) return true;
  // This is the crustose guild carried by cover, not all lichen species (including soil lichens).
  if (s == Substrate::soil) return false;
  switch (p.lichen_substrate) {
    case LichenSubstrate::any: return true;
    case LichenSubstrate::siliceous: return s == Substrate::siliceous_rock;
    case LichenSubstrate::calcareous: return s == Substrate::calcareous_rock;
    case LichenSubstrate::bark: return s == Substrate::living_bark;
    case LichenSubstrate::wood: return s == Substrate::dead_wood;
  }
  return false;
}

f64 nurse_log_suitability(const HostHabitat& h, const Exposure& e, f64 damp) {
  if (h.substrate != Substrate::dead_wood) return 0.0;
  // Grounding and authored response widths: experiments/surface-colonization.md.
  return ramp(0.0, 0.25, static_cast<f64>(h.decay)) *
         ramp(0.0, 0.03, static_cast<f64>(h.rooting_depth_m)) *
         (1.0 - ramp(0.04, 0.08, static_cast<f64>(h.bryophyte_depth_m))) *
         ramp(0.0, 1.0, static_cast<f64>(e.up)) * damp;
}
}  // namespace engine::cover
