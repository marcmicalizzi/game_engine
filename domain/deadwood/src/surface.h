#pragma once

#include <domain/deadwood/deadwood.h>

namespace engine::deadwood::detail {
// Continuous, elongated bark-loss field in source UV metres, shared at triangle edges.
f32 bark_field(Vec2 uv, u64 seed);
// Native maps only for exposed roles lacking an authored set; no runtime work. `height`, when
// named, stands in for the wood's own relief as a function of the map's (u, v), so a case can bake
// a known slope and hold which way the normal map's red and green point (R202).
bool bake_surface(const DeadwoodFile& file, u32 role, u32 material, f32 end_span,
                  geometry::ClusterFileData& out, std::string& error,
                  f64 (*height)(f64 u, f64 v) = nullptr);
}  // namespace engine::deadwood::detail
