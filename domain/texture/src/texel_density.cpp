// How many texels a metre an image puts on its mesh (texel_density.h).
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/material_sampling.h>
#include <domain/texture/texel_density.h>
#include <domain/texture/texture_build.h>

#include <algorithm>
#include <cmath>

namespace engine::texture {

namespace {

// One triangle's share of one image: the smaller singular value of the surface-to-UV map, its
// area on the surface and in UV, and its index (the quantile's tie-break).
struct Sample {
  f64 sigma = 0.0;
  f64 area = 0.0;
  u32 order = 0;
};

// The images a material samples, with each slot's transform scale, image -1 for an empty slot.
struct MaterialSlots {
  i32 image[geometry::k_material_slots] = {-1, -1, -1, -1, -1};
  f64 scale_u[geometry::k_material_slots] = {1.0, 1.0, 1.0, 1.0, 1.0};
  f64 scale_v[geometry::k_material_slots] = {1.0, 1.0, 1.0, 1.0, 1.0};
};

void material_slots(std::span<const geometry::ClusterFileMaterial> materials,
                    std::span<const geometry::ClusterFileMaterialSampling> sampling,
                    Vector<MaterialSlots>& out) {
  out.assign(static_cast<u32>(materials.size()), MaterialSlots{});
  for (u32 m = 0; m < materials.size(); ++m) {
    const geometry::ClusterFileMaterial& material = materials[m];
    const geometry::ClusterFileMaterialSampling record =
        m < sampling.size() ? sampling[m] : geometry::ClusterFileMaterialSampling{};
    geometry::TextureSlotSampling slots[geometry::k_material_slots];
    f32 strength = 1.0f;
    geometry::decode_material_sampling(record, slots, strength);
    // The slot order of `k_slot_*` (material_sampling.h), as `image_edges` reads it.
    const i32 images[geometry::k_material_slots] = {
        material.base_color_image,
        geometry::decode_optional_image(material.metallic_roughness_image), material.normal_image,
        geometry::decode_optional_image(material.occlusion_image),
        geometry::decode_optional_image(material.emissive_image)};
    // The renderer keeps **one transform a material**, the base colour's or the first textured
    // slot's (texture.md, "Samplers and texture transforms"), and every slot draws with it, so that
    // is the one measured.
    u32 kept = 0;
    while (kept < geometry::k_material_slots && images[kept] < 0)
      ++kept;
    if (kept == geometry::k_material_slots) kept = 0;
    for (u32 s = 0; s < geometry::k_material_slots; ++s) {
      out[m].image[s] = images[s];
      out[m].scale_u[s] = std::fabs(static_cast<f64>(slots[kept].transform.scale.x));
      out[m].scale_v[s] = std::fabs(static_cast<f64>(slots[kept].transform.scale.y));
    }
  }
}

struct Accumulator {
  Vector<MaterialSlots> slots;
  Vector<Vector<Sample>> samples;  // per image
  Vector<ImageDensity> density;    // per image
  u32 next = 0;                    // triangle order
};

// One triangle of material `material`: for each image its slots name, the smaller singular value
// of the map from the triangle's plane to its (scaled) UVs. With the world edges a, b and the UV
// edges du, dv, the squared singular values are the roots of det(H - l G) = 0 for the Gram matrices
// G = [a.a a.b; a.b b.b] and H likewise of du, dv; the smaller root is taken as
// 2 det H / (B + sqrt(B^2 - 4 det G det H)), which does not cancel when det H is small.
void add_triangle(Accumulator& acc, Vec3 p0, Vec3 p1, Vec3 p2, Vec2 t0, Vec2 t1, Vec2 t2,
                  u32 material) {
  const u32 order = acc.next++;
  if (material >= acc.slots.size()) return;
  const f64 ax = static_cast<f64>(p1.x) - static_cast<f64>(p0.x);
  const f64 ay = static_cast<f64>(p1.y) - static_cast<f64>(p0.y);
  const f64 az = static_cast<f64>(p1.z) - static_cast<f64>(p0.z);
  const f64 bx = static_cast<f64>(p2.x) - static_cast<f64>(p0.x);
  const f64 by = static_cast<f64>(p2.y) - static_cast<f64>(p0.y);
  const f64 bz = static_cast<f64>(p2.z) - static_cast<f64>(p0.z);
  const f64 g11 = ax * ax + ay * ay + az * az;
  const f64 g12 = ax * bx + ay * by + az * bz;
  const f64 g22 = bx * bx + by * by + bz * bz;
  const f64 det_g = g11 * g22 - g12 * g12;
  if (!(det_g > 0.0)) return;  // no area on the surface: nothing to sample
  const f64 area = 0.5 * std::sqrt(det_g);
  const f64 du0 = static_cast<f64>(t1.x) - static_cast<f64>(t0.x);
  const f64 dv0 = static_cast<f64>(t1.y) - static_cast<f64>(t0.y);
  const f64 du1 = static_cast<f64>(t2.x) - static_cast<f64>(t0.x);
  const f64 dv1 = static_cast<f64>(t2.y) - static_cast<f64>(t0.y);
  const MaterialSlots& slots = acc.slots[material];
  // An image two slots of one material name counts the triangle once, at the smaller density.
  i32 seen_image[geometry::k_material_slots];
  f64 seen_sigma[geometry::k_material_slots];
  f64 seen_uv[geometry::k_material_slots];
  u32 seen = 0;
  for (u32 s = 0; s < geometry::k_material_slots; ++s) {
    const i32 image = slots.image[s];
    if (image < 0 || static_cast<u32>(image) >= acc.density.size()) continue;
    const f64 su = slots.scale_u[s];
    const f64 sv = slots.scale_v[s];
    const f64 eu0 = du0 * su, ev0 = dv0 * sv, eu1 = du1 * su, ev1 = dv1 * sv;
    const f64 h11 = eu0 * eu0 + ev0 * ev0;
    const f64 h12 = eu0 * eu1 + ev0 * ev1;
    const f64 h22 = eu1 * eu1 + ev1 * ev1;
    f64 det_h = h11 * h22 - h12 * h12;
    det_h = det_h > 0.0 ? det_h : 0.0;
    const f64 b = h11 * g22 + h22 * g11 - 2.0 * h12 * g12;
    f64 disc = b * b - 4.0 * det_g * det_h;
    disc = disc > 0.0 ? disc : 0.0;
    const f64 denominator = b + std::sqrt(disc);
    const f64 lambda = denominator > 0.0 ? 2.0 * det_h / denominator : 0.0;
    const f64 sigma = std::sqrt(lambda > 0.0 ? lambda : 0.0);
    const f64 uv_area = 0.5 * std::sqrt(det_h);
    u32 k = 0;
    while (k < seen && seen_image[k] != image)
      ++k;
    if (k == seen) {
      seen_image[k] = image;
      seen_sigma[k] = sigma;
      seen_uv[k] = uv_area;
      ++seen;
    } else if (sigma < seen_sigma[k]) {
      seen_sigma[k] = sigma;
      seen_uv[k] = uv_area;
    }
  }
  for (u32 k = 0; k < seen; ++k) {
    const u32 i = static_cast<u32>(seen_image[k]);
    ImageDensity& d = acc.density[i];
    ++d.triangles;
    d.surface_m2 += area;
    d.uv_area += seen_uv[k];
    acc.samples[i].push_back(Sample{seen_sigma[k], area, order});
  }
}

void finish(Accumulator& acc, Vector<ImageDensity>& out) {
  for (u32 i = 0; i < acc.density.size(); ++i) {
    ImageDensity& d = acc.density[i];
    Vector<Sample>& samples = acc.samples[i];
    if (samples.empty() || !(d.surface_m2 > 0.0)) continue;
    d.uv_per_m_mean = std::sqrt(d.uv_area / d.surface_m2);
    // A total order: by density, then by the triangle's place, so equal densities sort the same on
    // every toolchain and the quantile does not depend on the sort's algorithm.
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
      return a.sigma < b.sigma || (a.sigma == b.sigma && a.order < b.order);
    });
    const f64 floor_area = d.surface_m2 * k_density_surface_share;
    f64 below = 0.0;
    u32 at = 0;
    // The first sample at which the area below it reaches the share: everything from it on is at
    // least its density, and what is under it is less than the share.
    while (at + 1 < samples.size() && below + samples[at].area <= floor_area) {
      below += samples[at].area;
      ++at;
    }
    d.uv_per_m = samples[at].sigma;
  }
  out = std::move(acc.density);
}

void begin(Accumulator& acc, std::span<const geometry::ClusterFileMaterial> materials,
           std::span<const geometry::ClusterFileMaterialSampling> sampling, u32 image_count) {
  material_slots(materials, sampling, acc.slots);
  acc.samples.clear();
  acc.samples.resize(image_count);
  acc.density.assign(image_count, ImageDensity{});
}

}  // namespace

void measure_image_density(std::span<const Vec3> positions, std::span<const Vec2> uvs,
                           std::span<const u32> indices, std::span<const u32> triangle_material,
                           std::span<const geometry::ClusterFileMaterial> materials,
                           std::span<const geometry::ClusterFileMaterialSampling> sampling,
                           u32 image_count, Vector<ImageDensity>& out) {
  Accumulator acc;
  begin(acc, materials, sampling, image_count);
  const usize triangles = indices.size() / 3;
  for (usize t = 0; t < triangles; ++t) {
    const u32 i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
    if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size() ||
        i0 >= uvs.size() || i1 >= uvs.size() || i2 >= uvs.size()) {
      ++acc.next;
      continue;
    }
    const u32 material = t < triangle_material.size() ? triangle_material[t] : 0u;
    add_triangle(acc, positions[i0], positions[i1], positions[i2], uvs[i0], uvs[i1], uvs[i2],
                 material);
  }
  finish(acc, out);
}

void measure_cluster_density(const geometry::ClusterLodMesh& lod,
                             std::span<const u32> cluster_material,
                             std::span<const geometry::ClusterFileMaterial> materials,
                             std::span<const geometry::ClusterFileMaterialSampling> sampling,
                             u32 image_count, Vector<ImageDensity>& out) {
  Accumulator acc;
  begin(acc, materials, sampling, image_count);
  const geometry::ClusterMesh& mesh = lod.mesh;
  const bool have_uvs = mesh.attributes.size() == mesh.vertices.size();
  for (u32 c = 0; c < mesh.clusters.size(); ++c) {
    if (c < lod.lod.size() && lod.lod[c].level != 0) continue;
    const geometry::ClusterDesc& cluster = mesh.clusters[c];
    const u32 material = c < cluster_material.size() ? cluster_material[c] : 0u;
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = mesh.triangles[cluster.triangle_offset + t];
      u32 v[3];
      bool inside = have_uvs;
      for (u32 k = 0; k < 3; ++k) {
        v[k] = cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k);
        inside = inside && v[k] < mesh.vertices.size();
      }
      if (!inside) {
        ++acc.next;
        continue;
      }
      add_triangle(acc, mesh.vertices[v[0]], mesh.vertices[v[1]], mesh.vertices[v[2]],
                   geometry::decode_half2(mesh.attributes[v[0]].uv_half2),
                   geometry::decode_half2(mesh.attributes[v[1]].uv_half2),
                   geometry::decode_half2(mesh.attributes[v[2]].uv_half2), material);
    }
  }
  finish(acc, out);
}

u32 needed_side(f64 uv_per_m, f64 scale, f64 texels_per_m) noexcept {
  if (!(uv_per_m > 0.0) || !(scale > 0.0) || !(texels_per_m > 0.0)) return 0;
  const f64 side = texels_per_m * scale / uv_per_m;
  u32 power = 1;
  while (static_cast<f64>(power) < side) {
    if (power >= k_max_needed_side) return 0;
    power <<= 1;
  }
  return power;
}

}  // namespace engine::texture
