#pragma once

#include <domain/deadwood/deadwood.h>

namespace engine::deadwood::detail {
// Continuous, elongated bark-loss field in source UV metres, shared at triangle edges.
f32 bark_field(Vec2 uv, u64 seed);
// Native maps only for exposed roles lacking an authored set; no runtime work.
bool bake_surface(const DeadwoodFile& file, u32 role, u32 material, f32 end_span,
                  geometry::ClusterFileData& out, std::string& error);
}  // namespace engine::deadwood::detail
