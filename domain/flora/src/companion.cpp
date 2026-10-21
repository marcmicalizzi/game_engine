// The kit's shapes for a companion solver (companion.h): thin public wrappers over the species'
// kit (kit.h), so a capability that grows over a host emits through the same emitter, motion
// contract and structural rows as the species, and nothing in the kit itself changes.
#include "kit.h"

#include <core/math/deterministic.h>
#include <domain/flora/companion.h>

namespace engine::flora {

namespace {

using detail::D3;

D3 d3(KitPoint p) { return D3{p.x, p.y, p.z}; }

detail::Emitter emitter_of(KitWriter& out) {
  detail::Emitter e{out.mesh, out.motion};
  e.part = out.part;
  e.weight = out.weight;
  return e;
}

}  // namespace

u32 kit_begin_part(KitWriter& out, KitPoint pivot, KitPoint direction, f64 length, u32 parent,
                   f64 base_radius, f64 tip_radius, u32 flags) {
  detail::Emitter e = emitter_of(out);
  out.part = detail::begin_structural_part(e, d3(pivot), d3(direction), length, parent, base_radius,
                                           tip_radius, flags);
  out.weight = 0.0;
  return out.part;
}

void kit_polyline_tube(KitWriter& out, std::span<const KitPoint> points, std::span<const f64> radii,
                       std::span<const f64> weights, u32 sides) {
  const u32 n = static_cast<u32>(points.size());
  if (n < 2 || radii.size() < n || weights.size() < n || sides < 3) return;
  detail::Emitter e = emitter_of(out);
  // Tangents by central differences, a frame transported along them (no twist).
  Vector<D3> tangent;
  tangent.resize(n);
  for (u32 i = 0; i < n; ++i) {
    const D3 a = d3(points[i == 0 ? 0 : i - 1]);
    const D3 b = d3(points[i + 1 < n ? i + 1 : n - 1]);
    tangent[i] = detail::normalized(b - a);
  }
  const D3 hint = (tangent[0].y > 0.9 || tangent[0].y < -0.9) ? D3{1.0, 0.0, 0.0} : detail::k_up;
  D3 normal = detail::perpendicular(tangent[0], hint);
  f64 arc = 0.0;
  Vector<u32> previous;
  Vector<u32> ring;
  for (u32 i = 0; i < n; ++i) {
    const D3 t = tangent[i];
    if (i > 0) {
      arc += detail::length_of(d3(points[i]) - d3(points[i - 1]));
      const D3 carried = normal - t * detail::dot(normal, t);
      normal = detail::length_of(carried) > 1e-9
                   ? detail::normalized(carried)
                   : detail::perpendicular(
                         t, (t.y > 0.9 || t.y < -0.9) ? D3{1.0, 0.0, 0.0} : detail::k_up);
    }
    const D3 binormal = detail::cross(t, normal);
    const f64 r = radii[i];
    e.weight = weights[i];
    ring.clear();
    for (u32 j = 0; j <= sides; ++j) {
      const f64 angle = 2.0 * detail::k_pi * static_cast<f64>(j) / static_cast<f64>(sides);
      const D3 dir = normal * det::cos(angle) + binormal * det::sin(angle);
      const f64 u = 2.0 * detail::k_pi * r * static_cast<f64>(j) / static_cast<f64>(sides);
      ring.push_back(e.vertex(d3(points[i]) + dir * r, dir, u, arc));
    }
    if (i > 0) {
      for (u32 j = 0; j < sides; ++j) {
        const f64 mid = 2.0 * detail::k_pi * (static_cast<f64>(j) + 0.5) / static_cast<f64>(sides);
        const D3 out_dir = normal * det::cos(mid) + binormal * det::sin(mid);
        e.face(previous[j], ring[j], previous[j + 1], out_dir);
        e.face(previous[j + 1], ring[j], ring[j + 1], out_dir);
      }
    }
    previous = ring;
  }
  // The tip: a cone to a point one radius past the last ring.
  const D3 last = d3(points[n - 1]);
  const D3 t = tangent[n - 1];
  e.weight = weights[n - 1];
  const u32 tip = e.vertex(last + t * radii[n - 1], t, 0.0, arc + radii[n - 1]);
  for (u32 j = 0; j < sides; ++j)
    e.face(previous[j], tip, previous[j + 1], t);
}

void kit_leaf_card(KitWriter& out, KitPoint base, KitPoint direction, KitPoint side, f64 length,
                   f64 width, f64 droop) {
  detail::Emitter e = emitter_of(out);
  detail::build_leaf_card(d3(base), d3(direction), d3(side), length, width, droop, e);
}

void kit_dome(KitWriter& out, KitPoint centre, KitPoint up, KitPoint along, f64 radius, f64 height,
              f64 squash, u32 sides, u32 rings) {
  if (sides < 3 || rings < 1 || radius <= 0.0 || height <= 0.0) return;
  detail::Emitter e = emitter_of(out);
  const D3 c = d3(centre);
  const D3 u = detail::normalized(d3(up));
  const D3 a = detail::perpendicular(u, d3(along));
  const D3 b = detail::cross(u, a);
  const f64 minor = radius * (squash > 0.0 ? squash : 1.0);
  Vector<u32> previous;
  Vector<u32> ring;
  for (u32 k = 0; k < rings; ++k) {
    const f64 theta = 0.5 * detail::k_pi * static_cast<f64>(k) / static_cast<f64>(rings);
    const f64 ct = det::cos(theta);
    const f64 st = det::sin(theta);
    ring.clear();
    for (u32 j = 0; j <= sides; ++j) {
      const f64 phi = 2.0 * detail::k_pi * static_cast<f64>(j) / static_cast<f64>(sides);
      const f64 cp = det::cos(phi);
      const f64 sp = det::sin(phi);
      const D3 p = c + a * (radius * cp * ct) + b * (minor * sp * ct) + u * (height * st);
      const D3 nrm = a * (cp * ct / radius) + b * (sp * ct / minor) + u * (st / height);
      ring.push_back(e.vertex(p, nrm, static_cast<f64>(j) / static_cast<f64>(sides),
                              static_cast<f64>(k) / static_cast<f64>(rings)));
    }
    if (k > 0) {
      for (u32 j = 0; j < sides; ++j) {
        const D3 outward = detail::normalized(
            e.mesh.positions.size() > 0
                ? detail::from_f32(e.mesh.positions[previous[j]]) - c + u * (0.25 * height)
                : u);
        e.face(previous[j], previous[j + 1], ring[j], outward);
        e.face(previous[j + 1], ring[j + 1], ring[j], outward);
      }
    }
    previous = ring;
  }
  const u32 pole = e.vertex(c + u * height, u, 0.5, 1.0);
  for (u32 j = 0; j < sides; ++j)
    e.face(previous[j], previous[j + 1], pole, u);
}

}  // namespace engine::flora
