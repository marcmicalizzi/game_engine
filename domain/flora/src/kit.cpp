// The species solvers' shared shapes (kit.h): a grown axis, a tube along it, a spray of cards.
#include "kit.h"

#include <domain/geometry/cluster.h>

namespace engine::flora::detail {

Axis grow_axis(D3 base, D3 direction, f64 length, u32 rows, D3 pull, f64 pull_per_m, f64 wiggle,
               Rng& rng) {
  Axis a;
  a.length = length;
  const f64 step = length / static_cast<f64>(rows);
  D3 t = normalized(direction);
  D3 n = perpendicular(t, (t.y < 0.9 && t.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
  D3 p = base;
  a.point.reserve(rows + 1);
  for (u32 r = 0; r <= rows; ++r) {
    a.point.push_back(p);
    a.tangent.push_back(t);
    a.normal.push_back(n);
    a.arc.push_back(step * static_cast<f64>(r));
    if (r == rows) break;
    // The turn towards the pull: about the axis both lie in, by an angle that shrinks with the
    // sine between them, so a shoot that has reached the light stops turning and never overshoots.
    D3 next = t;
    const D3 hinge = cross(t, pull);
    const f64 s = length_of(hinge);
    if (s > 1e-9) next = rotate(next, hinge * (1.0 / s), pull_per_m * step * s);
    if (wiggle > 0.0) {
      const D3 b = cross(next, n);
      next = rotate(next, n, wiggle * rng.symmetric());
      next = rotate(next, normalized(b), wiggle * rng.symmetric());
    }
    next = normalized(next);
    p = p + normalized(t + next) * step;  // the step's middle direction
    t = next;
    n = perpendicular(t, n);  // the frame transported, never twisted
  }
  return a;
}

namespace {
u32 segment_of(const Axis& a, f64 arc) {
  const u32 rows = a.point.size() - 1;
  const f64 step = a.length / static_cast<f64>(rows);
  f64 f = arc / step;
  if (f < 0.0) f = 0.0;
  u32 i = static_cast<u32>(f);
  return i >= rows ? rows - 1 : i;
}
}  // namespace

D3 axis_point(const Axis& a, f64 arc) {
  const u32 i = segment_of(a, arc);
  const f64 t = clamp01((arc - a.arc[i]) / (a.arc[i + 1] - a.arc[i]));
  return a.point[i] + (a.point[i + 1] - a.point[i]) * t;
}

D3 axis_tangent(const Axis& a, f64 arc) {
  const u32 i = segment_of(a, arc);
  const f64 t = clamp01((arc - a.arc[i]) / (a.arc[i + 1] - a.arc[i]));
  return normalized(a.tangent[i] * (1.0 - t) + a.tangent[i + 1] * t);
}

Axis resample_axis(const Axis& a, u32 rows) {
  Axis r;
  r.length = a.length;
  D3 n = a.normal[0];
  for (u32 i = 0; i <= rows; ++i) {
    const f64 s = a.length * static_cast<f64>(i) / static_cast<f64>(rows);
    const D3 t = axis_tangent(a, s);
    n = perpendicular(t, n);
    r.point.push_back(i == rows ? a.point.back() : axis_point(a, s));
    r.tangent.push_back(t);
    r.normal.push_back(n);
    r.arc.push_back(s);
  }
  return r;
}

void build_tube(const Axis& axis, f64 r0, f64 r1, u32 sides, Vec2 uv_offset, Emitter& out) {
  const u32 rows = axis.point.size();
  const f64 round = 2.0 * k_pi * 0.5 * (r0 + r1);  // metres once round, at the mean radius
  const f64 u0 = static_cast<f64>(uv_offset.x);
  const f64 v0 = static_cast<f64>(uv_offset.y);
  const u32 first = out.mesh.positions.size();
  for (u32 r = 0; r < rows; ++r) {
    const f64 s = axis.arc[r] / axis.length;
    out.weight = s;
    const f64 radius = lerp(r0, r1, s);
    const D3 t = axis.tangent[r];
    const D3 n = axis.normal[r];
    const D3 b = cross(t, n);
    for (u32 i = 0; i <= sides; ++i) {
      const f64 angle = 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(sides);
      const D3 radial = n * det::cos(angle) + b * det::sin(angle);
      out.vertex(axis.point[r] + radial * radius, radial,
                 u0 + round * static_cast<f64>(i) / static_cast<f64>(sides), v0 + axis.arc[r]);
    }
  }
  for (u32 r = 0; r + 1 < rows; ++r) {
    for (u32 i = 0; i < sides; ++i) {
      const u32 a = first + r * (sides + 1) + i;
      const u32 b = a + 1;
      const u32 c = a + sides + 1;
      const u32 d = c + 1;
      const D3 hint = from_f32(out.mesh.normals[a]) + from_f32(out.mesh.normals[d]);
      out.face(a, b, d, hint);
      out.face(a, d, c, hint);
    }
  }
  // The tip: a cone to a point one tip radius past the last row.
  const D3 tip_dir = axis.tangent[rows - 1];
  out.weight = 1.0;
  const u32 tip = out.vertex(axis.point[rows - 1] + tip_dir * r1, tip_dir, u0 + 0.5 * round,
                             v0 + axis.length + r1);
  const u32 last = first + (rows - 1) * (sides + 1);
  for (u32 i = 0; i < sides; ++i) {
    const Vec3 nv = out.mesh.normals[last + i];
    const D3 hint = from_f32(nv) + tip_dir;
    out.face(last + i, last + i + 1, tip, hint);
  }
}

void build_spray(const Axis& axis, f64 from, u32 cards, f64 card_length, f64 angle, f64 lift,
                 f64 droop, D3 side, Emitter& out) {
  // The centre line's stations, shared by the cards either side of them.
  const f64 span = axis.length - from;
  Vector<D3> centre(cards + 1);
  Vector<D3> tangent(cards + 1);
  Vector<u32> mid(cards + 1);
  Vector<u32> tips[2] = {Vector<u32>(cards + 1), Vector<u32>(cards + 1)};
  const f64 ca = det::cos(angle);
  const f64 sa = det::sin(angle);
  const f64 cl = det::cos(lift);
  const f64 sl = det::sin(lift);
  Vector<D3> lateral(cards + 1);
  Vector<D3> upper(cards + 1);
  for (u32 k = 0; k <= cards; ++k) {
    const f64 s = from + span * static_cast<f64>(k) / static_cast<f64>(cards);
    centre[k] = axis_point(axis, s);
    tangent[k] = axis_tangent(axis, s);
    lateral[k] = perpendicular(tangent[k], side);
    upper[k] = normalized(cross(lateral[k], tangent[k]));
    out.weight = s / axis.length;
    mid[k] = out.vertex(centre[k], upper[k], static_cast<f64>(k), 0.0);
  }
  for (u32 k = 0; k <= cards; ++k) {
    const f64 s = from + span * static_cast<f64>(k) / static_cast<f64>(cards);
    out.weight = s / axis.length;
    for (u32 j = 0; j < 2; ++j) {
      const f64 sigma = j == 0 ? 1.0 : -1.0;
      const D3 out_dir = lateral[k] * (sigma * cl) + upper[k] * sl;
      const D3 d = normalized(tangent[k] * ca + out_dir * sa);
      const D3 tip = centre[k] + d * card_length - k_up * (droop * card_length);
      tips[j][k] = out.vertex(tip, upper[k], static_cast<f64>(k), sigma);
    }
  }
  for (u32 k = 0; k < cards; ++k) {
    for (u32 j = 0; j < 2; ++j) {
      out.face(mid[k], mid[k + 1], tips[j][k + 1], upper[k]);
      out.face(mid[k], tips[j][k + 1], tips[j][k], upper[k]);
    }
  }
}

// ---- the second batch of species (kit.h)
// ---------------------------------------------------------

Axis grow_axis_after(D3 base, D3 direction, f64 length, u32 rows, D3 pull, f64 pull_per_m,
                     f64 pull_from, f64 wiggle, Rng& rng) {
  Axis a;
  a.length = length;
  const f64 step = length / static_cast<f64>(rows);
  D3 t = normalized(direction);
  D3 n = perpendicular(t, (t.y < 0.9 && t.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
  D3 p = base;
  a.point.reserve(rows + 1);
  for (u32 r = 0; r <= rows; ++r) {
    a.point.push_back(p);
    a.tangent.push_back(t);
    a.normal.push_back(n);
    a.arc.push_back(step * static_cast<f64>(r));
    if (r == rows) break;
    D3 next = t;
    if (step * static_cast<f64>(r) >= pull_from) {
      const D3 hinge = cross(t, pull);
      const f64 s = length_of(hinge);
      if (s > 1e-9) next = rotate(next, hinge * (1.0 / s), pull_per_m * step * s);
    }
    if (wiggle > 0.0) {
      const D3 b = cross(next, n);
      const f64 first = wiggle * rng.symmetric();
      next = rotate(next, n, first);
      const f64 second = wiggle * rng.symmetric();
      next = rotate(next, normalized(b), second);
    }
    next = normalized(next);
    p = p + normalized(t + next) * step;
    t = next;
    n = perpendicular(t, n);
  }
  return a;
}

Axis truncate_axis(const Axis& axis, u32 rows) {
  Axis r;
  for (u32 i = 0; i <= rows; ++i) {
    r.point.push_back(axis.point[i]);
    r.tangent.push_back(axis.tangent[i]);
    r.normal.push_back(axis.normal[i]);
    r.arc.push_back(axis.arc[i]);
  }
  r.length = axis.arc[rows];
  return r;
}

D3 branch_direction(const Axis& parent, f64 arc, f64 azimuth, f64 angle, D3* radial_out) {
  const u32 rows = parent.point.size() - 1;
  const f64 f = arc / parent.length * static_cast<f64>(rows);
  u32 i = static_cast<u32>(f);
  if (i > rows) i = rows;
  const D3 t = axis_tangent(parent, arc);
  const D3 n = perpendicular(t, parent.normal[i]);
  const D3 b = cross(t, n);
  const D3 radial = n * det::cos(azimuth) + b * det::sin(azimuth);
  if (radial_out != nullptr) *radial_out = radial;
  return normalized(t * det::cos(angle) + radial * det::sin(angle));
}

f64 pipe_radius_of(f64 root_radius, f64 tips, f64 total) {
  return root_radius * det::exp(det::log(tips / total) / 2.5);
}

u32 begin_structural_part(Emitter& out, D3 pivot, D3 direction, f64 length, u32 parent, f64 r0,
                          f64 r1, u32 flags) {
  const u32 part = out.begin_part(pivot, direction, length, parent);
  geometry::ClusterFileStructuralPart record;
  record.part = part;
  record.base_radius = static_cast<f32>(r0);
  record.tip_radius = static_cast<f32>(r1);
  record.flags = flags;
  out.motion.structure.push_back(record);
  return part;
}

u32 emit_tube_part(Emitter& out, const Axis& axis, f64 r0, f64 r1, u32 rows, u32 sides, u32 parent,
                   u32 flags) {
  const u32 part = begin_structural_part(out, axis.point[0], axis.tangent[0], axis.length, parent,
                                         r0, r1, flags);
  build_tube(resample_axis(axis, rows), r0, r1, sides, Vec2{0.0f, 0.0f}, out);
  return part;
}

u32 emit_spray_part(Emitter& out, const Axis& axis, u32 parent, u32 cards, f64 card_length,
                    f64 angle, f64 lift, f64 droop) {
  const u32 part =
      begin_structural_part(out, axis.point[0], axis.tangent[0], axis.length, parent, 0.0, 0.0, 0u);
  D3 side = cross(axis.tangent[0], k_up);
  if (dot(side, side) < 1e-6) side = D3{1.0, 0.0, 0.0};
  build_spray(axis, 0.0, cards, card_length, angle, lift, droop, normalized(side), out);
  return part;
}

void smooth_normals(assets::MeshData& mesh, u32 first_vertex, u32 first_index) {
  Vector<Vec3> normals;
  const std::span<const u32> range(mesh.indices.data() + first_index,
                                   mesh.indices.size() - first_index);
  geometry::compute_vertex_normals(mesh.positions, range, normals);
  for (u32 v = first_vertex; v < mesh.positions.size(); ++v)
    mesh.normals[v] = normals[v];
}

}  // namespace engine::flora::detail
