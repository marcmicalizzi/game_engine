#pragma once

// What the walls tests share: a planar ground of a stated grade along x and across z, a layer
// round a field with a gateway, and a placement's point in the world.

#include <core/hash/hash.h>
#include <domain/walls/walls.h>

namespace engine::walls::test {

// A TestPlane: y = gx * x + gz * z (+ a bump of `bump` metres every 7 m along x, for curved
// ground).
struct TestPlane {
  f64 gx = 0, gz = 0, bump = 0;
};
inline f64 plane_height(const void* c, i64 x_mm, i64 z_mm) noexcept {
  const TestPlane& p = *static_cast<const TestPlane*>(c);
  const f64 x = static_cast<f64>(x_mm) * 0.001, z = static_cast<f64>(z_mm) * 0.001;
  f64 y = p.gx * x + p.gz * z;
  if (p.bump != 0) {
    const f64 f = x / 7.0 - static_cast<f64>(static_cast<i64>(x / 7.0 + 1000.0) - 1000);
    y += p.bump * 4.0 * f * (1.0 - f);
  }
  return y;
}
inline scene_gen::Ground ground_of(const TestPlane& p) {
  scene_gen::Ground g;
  g.surface_fn = plane_height;
  g.context = &p;
  return g;
}

// A field 60 m by 40 m whose corner is at (x0, z0) metres, closed, with a gateway on its south
// side, and a lane wall beside it 50 m long.
inline Layer field_layer(i64 x0_m = 0, i64 z0_m = 0, f32 ruined = 0.0f) {
  Layer layer;
  WallLine field;
  const i64 x = x0_m * 1000, z = z0_m * 1000;
  field.points_mm = {x, z, x + 60000, z, x + 60000, z + 40000, x, z + 40000};
  field.closed = true;
  field.gateways_cm = {3000};
  layer.lines.push_back(field);
  WallLine lane;
  lane.points_mm = {x - 5000, z - 3000, x + 45000, z - 3000};
  layer.lines.push_back(lane);
  layer.ruined = ruined;
  return layer;
}

// A placement's fields, never its padding.
inline u64 placement_hash(const scene_gen::Placement& p) {
  u64 h = hash_bytes(&p.position, sizeof(p.position));
  h = hash_combine(h, hash_bytes(&p.rotation, sizeof(p.rotation)));
  h = hash_combine(h, hash_bytes(&p.scale, sizeof(p.scale)));
  h = hash_combine(h, (static_cast<u64>(p.mesh) << 32) | (static_cast<u64>(p.tag) << 24) |
                          (static_cast<u64>(p.handoff) << 16));
  return hash_combine(h, p.id);
}

// Where a point of a placed module's own frame lands in the world.
inline WorldPos world_of(const scene_gen::Placement& p, Vec3 local) {
  const Vec3 v =
      rotate(p.rotation, Vec3{local.x * p.scale.x, local.y * p.scale.y, local.z * p.scale.z});
  return {p.position.x + static_cast<f64>(v.x), p.position.y + static_cast<f64>(v.y),
          p.position.z + static_cast<f64>(v.z)};
}

}  // namespace engine::walls::test
