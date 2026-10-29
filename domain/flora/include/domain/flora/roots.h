#pragma once

#include <domain/flora/flora.h>
#include <domain/scene_gen/scene_gen.h>

namespace engine::flora {
// Version 2 was used by the unaccepted G102 tessellation candidate in local DDCs.
inline constexpr u32 k_roots_version = 3;
// Cold cook diagnostics and consumer mask data. The same skeleton is used by both LODs.
struct RootSpan {
  Vec3 from{}, to{};
  f32 radius = 0;
};
struct RootReport {
  u32 primary = 0, secondary = 0, triangles = 0, collar_triangles = 0;
  f32 lod_error = 0;
  Vector<Vec3> buried_tips;
  Vector<RootSpan> exposed;
  Vector<u32> coarse_indices, fine_element, coarse_element;
  u32 elements = 0;
};
bool roots_in_range(const Roots& roots, std::string& error);
// Empty output is success for disabled roots, small stems or a site with no room.
// The consumer omits that placement; it must not cook an empty mesh container.
bool solve_roots(const Roots& roots, assets::MeshData& mesh, Motion& motion, RootReport& report,
                 std::string& error);
// Bind an already solved tree's lower trunk surface. Reuse this per shared tree recipe.
// Replaces only roots.stem; failure leaves it unchanged. No terrain query or runtime work.
// Ancient trunks retain their authored buttresses/hollows and reject this fitted sleeve.
bool fit_root_stem(const assets::MeshData& mesh, const Motion& motion, Roots& roots,
                   std::string& error);
// Resolve dimensions cheaply. Opt into binding the actual trunk with bind_stem=true, which
// solves the tree once at content-build time; the default preserves the inexpensive existing API.
// For repeated sites, call once and copy the resulting recipe before applying site controls.
// Retains caller site controls, replacing species, seed, tree dimensions and stem boundary.
bool roots_for_tree(const FloraFile& tree, Roots& roots, std::string& error,
                    bool bind_stem = false);
// Bake a tree-local terrain patch from the scene query. No global float32 coordinates.
// Uniform positive scale and yaw only; tree/roots placements must share this transform.
bool sample_root_ground(const scene_gen::Ground& ground, WorldPos origin, f32 yaw_radians,
                        f32 scale, f32 reach_m, f32 spacing_m, RootGround& out, std::string& error);
// A conservative exposed-wood footprint for a grass-mask cook; buried portions contribute none.
bool root_covers(const RootReport& report, f32 x_m, f32 z_m) noexcept;
}  // namespace engine::flora
