#pragma once

// The deform chain on the CPU, in double precision where it matters, as `brdf_reference.h` is for
// the BSDF: the same four stages in the same order, written against the same data the shader
// reads, so a GPU test compares against an implementation rather than against last week's
// numbers. It lives here rather than in `domain/anim` or `domain/geometry` because two of the
// four stages — `wave` and `lattice` — are `domain/gfx`'s own procedural stand-ins for a cage
// solver and have no meaning below the RHI; the other two are thin wrappers over
// `geometry::morph_delta_at` and `anim`'s skinning, which *are* the modules' own references.
//
// Change `deform.slang` and change this in the same commit, then say by how much the two still
// agree — the same rule the BSDF reference carries.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_cull.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace engine::gfx::test_reference {

// Everything one vertex of one instance needs. The spans are the instance's own: `static_weights`
// and `pose_weights` are its two halves of `DeformDesc::weights`, indexed by the *mesh's* channel
// index (so channel `first_channel + k` of the scene is `weights[k]` here).
struct DeformChain {
  const geometry::ClusterMesh* mesh = nullptr;
  std::span<const f32> static_weights;
  std::span<const f32> pose_weights;
  std::span<const anim::JointMatrix> joints;
  u32 stages = 0;
  f32 time = 0.0f;
  f32 amplitude = 1.0f;
  // The mesh's quantization box, which the two procedural stand-ins are defined over. Filled
  // from the mesh when left at zero.
  Vec3 grid_origin{};
  f32 grid_side = 0.0f;
};

inline f32 chain_grid_side(const DeformChain& chain) noexcept {
  if (chain.grid_side > 0.0f) return chain.grid_side;
  return chain.mesh != nullptr ? chain.mesh->quant_scale * 65535.0f : 1.0f;
}

inline Vec3 chain_grid_origin(const DeformChain& chain) noexcept {
  if (chain.grid_side > 0.0f) return chain.grid_origin;
  return chain.mesh != nullptr ? chain.mesh->quant_origin : Vec3{};
}

// Stage 1 and 2 are the same arithmetic over different weights: sum over the channels that touch
// this cluster of weight x delta. `morph_delta_at` is geometry's own accessor, so this reference
// measures the *shader's* walk of the directory rather than re-deriving the format.
inline void morph_stage(const DeformChain& chain, u32 cluster, u32 local,
                        std::span<const f32> weights, Vec3& position, Vec3& normal) {
  if (chain.mesh == nullptr || weights.empty()) return;
  bool moved = false;
  for (u32 c = 0; c < chain.mesh->morph_channels.size() && c < weights.size(); ++c) {
    const f32 weight = weights[c];
    if (weight == 0.0f) continue;
    Vec3 position_delta;
    Vec3 normal_delta;
    if (!geometry::morph_delta_at(*chain.mesh, cluster, c, local, position_delta, normal_delta))
      continue;
    position = position + position_delta * weight;
    normal = normal + normal_delta * weight;
    moved = true;
  }
  if (moved && length_squared(normal) > 1.0e-20f) normal = normalize(normal);
}

// Stage 3. The position half is `anim::skin_positions` for one vertex; the normal half is the
// same blend applied to a direction, which is what the shader does when the frame carries a
// normal pool.
inline void skin_stage(const DeformChain& chain, const geometry::SkinBinding& binding,
                       Vec3& position, Vec3& normal) {
  if (chain.joints.empty()) return;
  Vec3 point{};
  Vec3 direction{};
  for (u32 k = 0; k < 4; ++k) {
    const u32 weight = binding.weights[k];
    if (weight == 0) continue;
    const u32 joint = binding.joints[k];
    if (joint >= chain.joints.size()) continue;
    const anim::JointMatrix& m = chain.joints[joint];
    const f32 w = static_cast<f32>(weight) * (1.0f / 255.0f);
    point = point + anim::transform_point(m, position) * w;
    const Vec3 turned{m.rows[0].x * normal.x + m.rows[0].y * normal.y + m.rows[0].z * normal.z,
                      m.rows[1].x * normal.x + m.rows[1].y * normal.y + m.rows[1].z * normal.z,
                      m.rows[2].x * normal.x + m.rows[2].y * normal.y + m.rows[2].z * normal.z};
    direction = direction + turned * w;
  }
  position = point;
  if (length_squared(direction) > 1.0e-20f) normal = normalize(direction);
}

// Stage 4, the two procedural stand-ins, copied from `deform.slang` term for term.
inline Vec3 wave_stage(const DeformChain& chain, Vec3 p, Vec3 normal) {
  const f32 side = std::max(chain_grid_side(chain), 1.0e-9f);
  const Vec3 u = (p - chain_grid_origin(chain)) * (1.0f / side);
  const f32 tau = 6.2831853071795864769f;
  const f32 phase = (u.x * 6.0f + u.y * 2.5f + u.z * 4.0f) * tau + chain.time * 3.0f;
  return p + normal * (std::sin(phase) * chain.amplitude * side);
}

inline Vec3 lattice_control_point(Vec3 origin, f32 side, u32 i, u32 j, u32 k, f32 time,
                                  f32 amplitude) {
  const Vec3 t{static_cast<f32>(i) * 0.5f, static_cast<f32>(j) * 0.5f, static_cast<f32>(k) * 0.5f};
  const Vec3 centred{t.x - 0.5f, t.y - 0.5f, t.z - 0.5f};
  const f32 radius = std::sqrt(centred.x * centred.x + centred.z * centred.z);
  const f32 angle = std::sin(time * 1.7f) * amplitude * 4.0f * radius;
  const f32 s = std::sin(angle);
  const f32 c = std::cos(angle);
  const Vec3 swirl{centred.x * c - centred.z * s, centred.y, centred.x * s + centred.z * c};
  const f32 breathe = 1.0f + std::sin(time * 2.3f + centred.y * 4.0f) * amplitude * 0.5f;
  return origin + (swirl * breathe + Vec3{0.5f, 0.5f, 0.5f}) * side;
}

inline Vec3 lattice_axis_weights(f32 a) {
  if (a < 0.5f) {
    const f32 t = a * 2.0f;
    return Vec3{1.0f - t, t, 0.0f};
  }
  const f32 t = (a - 0.5f) * 2.0f;
  return Vec3{0.0f, 1.0f - t, t};
}

inline Vec3 lattice_stage(const DeformChain& chain, Vec3 p) {
  const f32 side = std::max(chain_grid_side(chain), 1.0e-9f);
  const Vec3 origin = chain_grid_origin(chain);
  auto clamp01 = [](f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
  const Vec3 u{clamp01((p.x - origin.x) / side), clamp01((p.y - origin.y) / side),
               clamp01((p.z - origin.z) / side)};
  const Vec3 wx = lattice_axis_weights(u.x);
  const Vec3 wy = lattice_axis_weights(u.y);
  const Vec3 wz = lattice_axis_weights(u.z);
  auto pick = [](const Vec3& w, u32 i) { return i == 0 ? w.x : (i == 1 ? w.y : w.z); };
  Vec3 moved{};
  for (u32 i = 0; i < 3; ++i) {
    const f32 ax = pick(wx, i);
    if (ax == 0.0f) continue;
    for (u32 j = 0; j < 3; ++j) {
      const f32 ay = pick(wy, j);
      if (ay == 0.0f) continue;
      for (u32 k = 0; k < 3; ++k) {
        const f32 az = pick(wz, k);
        if (az == 0.0f) continue;
        moved = moved + lattice_control_point(origin, side, i, j, k, chain.time, chain.amplitude) *
                            (ax * ay * az);
      }
    }
  }
  return moved;
}

// The whole chain over one vertex of one cluster, in the one order the pass runs it: static
// shape, pose morphs, skinning, procedural. `position` and `normal` come back as the pool and the
// normal pool would hold them.
inline void deform_vertex(const DeformChain& chain, u32 cluster, u32 local, Vec3& position,
                          Vec3& normal) {
  const geometry::ClusterMesh& mesh = *chain.mesh;
  const geometry::ClusterDesc& desc = mesh.clusters[cluster];
  const u32 vertex = desc.vertex_offset + local;
  position = geometry::dequantize_position(mesh, vertex);
  normal = mesh.attributes.size() == mesh.vertices.size()
               ? geometry::decode_normal_oct(mesh.attributes[vertex].normal_oct)
               : Vec3{0.0f, 1.0f, 0.0f};
  if ((chain.stages & k_deform_stage_static) != 0)
    morph_stage(chain, cluster, local, chain.static_weights, position, normal);
  if ((chain.stages & k_deform_stage_pose) != 0)
    morph_stage(chain, cluster, local, chain.pose_weights, position, normal);
  if ((chain.stages & k_deform_stage_skin) != 0 && mesh.skin.size() == mesh.vertices.size())
    skin_stage(chain, mesh.skin[vertex], position, normal);
  if ((chain.stages & k_deform_stage_procedural) != 0) {
    const u32 kind = chain.stages & k_deform_kind_mask;
    if (kind == k_deform_wave) {
      position = wave_stage(chain, position, normal);
    } else if (kind == k_deform_lattice) {
      position = lattice_stage(chain, position);
    }
  }
}

}  // namespace engine::gfx::test_reference
