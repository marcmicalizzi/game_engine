// The species solvers' shared shapes (kit.h): a grown axis, a tube along it, a spray of cards.
#include "kit.h"

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

}  // namespace engine::flora::detail
