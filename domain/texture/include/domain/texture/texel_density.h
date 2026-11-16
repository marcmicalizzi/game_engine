#pragma once

// **How many texels a metre an image puts on its mesh, and how many a view can use**
// (docs/subsystems/texture.md, "Sized by the asset"; roadmap G14). A static prop's textures are
// paid whole in memory however small the prop is: a 2,048-texel image on a 20 cm box is about ten
// thousand texels a metre, several times what a 3840 x 2160 frame resolves at the nearest a
// walker's eye comes to it, so its top levels are never read and cost three quarters of the
// texture. This measures the density from the asset's own geometry — never from which generator
// made it — and turns a stated density into the build option that leaves those levels out
// (`TextureBuildOptions::needed_side`).
//
// Everything here feeds a cache key, so it is held to the texture build's determinism rule
// (texture.md, "Determinism"): IEEE arithmetic and `sqrt` in one order, f64 throughout, ties in the
// quantile broken by triangle index, and no C library function in a decision.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_file.h>

#include <span>

namespace engine::geometry {
struct ClusterLodMesh;
}

namespace engine::texture {

// The share of an image's surface (by area) allowed to sit below the density the measure
// reports: all but 1% of the surface has at least `ImageDensity::uv_per_m`. A strict minimum over
// triangles would let one sliver an exporter squeezed in its atlas hold a whole prop's texture at
// full size; 1% is a band of a 20 cm box's face 2 mm wide.
inline constexpr f64 k_density_surface_share = 0.01;

// One image of a mesh, measured over the level-0 triangles of every material that samples it in
// any slot (base colour, metallic-roughness, normal, occlusion, emissive), through the
// `KHR_texture_transform` scale the renderer draws the material with — the base colour's, or the
// first textured slot's, for every slot (texture.md, "Samplers and texture transforms"); a rotation
// and an offset change no density.
struct ImageDensity {
  u32 triangles = 0;     // level-0 triangles that sample the image
  f64 surface_m2 = 0.0;  // their area, in the mesh's own units (metres for a glTF)
  f64 uv_area = 0.0;     // their area in the slot's UV space, after the transform's scale
  // UV units a metre along the surface's **least** stretched direction — the smaller singular
  // value of the map from the triangle's plane to its UVs, which is what an anisotropic sampler's
  // level follows — at the area-weighted `k_density_surface_share` quantile. Times the image's
  // shorter side, it is the texels a metre level 0 has. 0: nothing samples the image, or that much
  // of its surface has no UV extent along some direction.
  f64 uv_per_m = 0.0;
  // sqrt(uv_area / surface_m2), the mean, for a report.
  f64 uv_per_m_mean = 0.0;
};

// Over an indexed triangle list: `positions` and `uvs` parallel, three `indices` a triangle, and
// `triangle_material` one material index a triangle (an index past `materials` samples nothing).
// `sampling` is parallel to `materials` or empty (every slot untransformed). `out` gets
// `image_count` entries.
void measure_image_density(std::span<const Vec3> positions, std::span<const Vec2> uvs,
                           std::span<const u32> indices, std::span<const u32> triangle_material,
                           std::span<const geometry::ClusterFileMaterial> materials,
                           std::span<const geometry::ClusterFileMaterialSampling> sampling,
                           u32 image_count, Vector<ImageDensity>& out);

// The same over a cluster DAG's level-0 clusters (`ClusterLodDesc::level` 0: the source's own
// triangles, in whatever order the clusters are stored), with its UVs as stored (half floats) and
// `cluster_material` one material index a cluster (empty: material 0).
void measure_cluster_density(const geometry::ClusterLodMesh& lod,
                             std::span<const u32> cluster_material,
                             std::span<const geometry::ClusterFileMaterial> materials,
                             std::span<const geometry::ClusterFileMaterialSampling> sampling,
                             u32 image_count, Vector<ImageDensity>& out);

// The `needed_side` an image measured `uv_per_m` asks for when the mesh is drawn at `scale` (its
// largest instance's largest axis scale, a fit included) and the closest view resolves
// `texels_per_m` on its surface: the texels along the image's shorter side at which level 0 is
// still at least that dense, `texels_per_m * scale / uv_per_m`, as the next power of two up. 0 (no
// cap) when either density is not positive or the answer passes `k_max_needed_side`.
u32 needed_side(f64 uv_per_m, f64 scale, f64 texels_per_m) noexcept;

}  // namespace engine::texture
