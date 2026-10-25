// The species solvers' shared shapes (kit.h): a grown axis, a tube along it, a spray of cards.
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/geometry/cluster.h>

#include <algorithm>

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
  if (!a.uniform_steps) {
    const auto upper = std::upper_bound(a.arc.begin(), a.arc.end(), arc);
    const u32 index = static_cast<u32>(upper - a.arc.begin());
    return index == 0 ? 0 : std::min(index - 1, rows - 1);
  }
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

f64 continuation_radius(f64 r0, f64 r1, f64 length, const StemContinuation& join, f64 arc) {
  const f64 slope = (join.reference_tip - r0) / join.reference_length;
  if (arc <= join.from) return lerp(r0, join.reference_tip, arc / join.reference_length);
  const f64 span = length - join.from;
  const f64 u = clamp01((arc - join.from) / span);
  const f64 u2 = u * u;
  const f64 u3 = u2 * u;
  const f64 start = r0 + slope * join.from;
  // Monotone endpoint slopes cannot overshoot the positive radius envelope.
  const f64 limit = 1.5 * (start - r1) / span;
  const f64 m0 = std::clamp(slope, -limit, 0.0) * span;
  const f64 m1 = std::clamp(join.end_slope, -limit, 0.0) * span;
  return (2 * u3 - 3 * u2 + 1) * start + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * r1 +
         (u3 - u2) * m1;
}

Axis resample_continuation(const Axis& a, u32 rows, const StemContinuation& join) {
  Axis r;
  r.length = a.length;
  r.uniform_steps = false;
  D3 n = a.normal[0];
  for (u32 i = 0; i <= rows + k_continuation_rows; ++i) {
    const f64 s = i == rows                         ? join.from
                  : i == rows + k_continuation_rows ? a.length
                  : i <= rows ? join.from * static_cast<f64>(i) / rows
                              : join.from + (a.length - join.from) * static_cast<f64>(i - rows) /
                                                k_continuation_rows;
    const D3 t = axis_tangent(a, s);
    n = perpendicular(t, n);
    r.point.push_back(i == rows + k_continuation_rows ? a.point.back() : axis_point(a, s));
    r.tangent.push_back(t);
    r.normal.push_back(n);
    r.arc.push_back(s);
  }
  return r;
}

Axis resample_continuing_child(const Axis& a, u32 rows, f64 station) {
  Axis r;
  r.length = a.length;
  r.uniform_steps = false;
  D3 n = a.normal[0];
  const u32 before =
      std::clamp(static_cast<u32>(static_cast<f64>(rows + 1) * station / a.length + .5), 1u, rows);
  for (u32 i = 0; i <= rows + 1; ++i) {
    const f64 s = i == before     ? station
                  : i == rows + 1 ? a.length
                  : i <= before   ? station * static_cast<f64>(i) / before
                                  : station + (a.length - station) * static_cast<f64>(i - before) /
                                                  (rows + 1 - before);
    const D3 t = axis_tangent(a, s);
    n = perpendicular(t, n);
    r.point.push_back(i == rows + 1 ? a.point.back() : axis_point(a, s));
    r.tangent.push_back(t);
    r.normal.push_back(n);
    r.arc.push_back(s);
  }
  return r;
}

void crown_continue_stem(Crown& crown, u32 stem, u32 child, f64 station) {
  CrownNode& node = crown.nodes[stem];
  const CrownNode& branch = crown.nodes[child];
  const Axis original = crown.axes[node.axis];
  const Axis& target = crown.axes[branch.axis];
  StemContinuation join;
  join.from = station;
  join.reference_length = original.length;
  join.reference_tip = node.r1;
  const f64 radius = lerp(node.r0, node.r1, station / original.length);
  // The shoulder has physical extent, but cannot consume more than the available scaffold.
  const f64 reach = std::min(.65 * target.length, std::max(4.0 * radius, .12 * target.length));
  const D3 start = axis_point(original, station);
  const D3 end = axis_point(target, reach);
  const D3 t0 = axis_tangent(original, station);
  const D3 t1 = axis_tangent(target, reach);
  const D3 delta = end - start;
  const f64 distance = length_of(delta);
  // A thick short stem can have much more lateral offset than rise. Bound the handles by
  // displacement along both tangents as well, so the shoulder cannot fold back on itself.
  const f64 handle =
      std::min(.7 * distance, std::max(1e-6 * distance, std::min(dot(delta, t0), dot(delta, t1))));
  const D3 m0 = t0 * handle;
  const D3 m1 = t1 * handle;
  Axis axis;
  axis.uniform_steps = false;
  for (u32 i = 0; i < original.point.size() && original.arc[i] < station; ++i) {
    axis.point.push_back(original.point[i]);
    axis.tangent.push_back(original.tangent[i]);
    axis.normal.push_back(original.normal[i]);
    axis.arc.push_back(original.arc[i]);
  }
  D3 n = perpendicular(t0, axis.normal.back());
  axis.point.push_back(start);
  axis.tangent.push_back(t0);
  axis.normal.push_back(n);
  axis.arc.push_back(station);
  constexpr u32 samples = 32;  // structure resolution; independent of the polygon ladder
  for (u32 i = 1; i <= samples; ++i) {
    const f64 u = static_cast<f64>(i) / samples;
    const f64 u2 = u * u;
    const f64 u3 = u2 * u;
    const D3 p = start * (2 * u3 - 3 * u2 + 1) + m0 * (u3 - 2 * u2 + u) + end * (-2 * u3 + 3 * u2) +
                 m1 * (u3 - u2);
    const D3 t = normalized(start * (6 * u2 - 6 * u) + m0 * (3 * u2 - 4 * u + 1) +
                            end * (-6 * u2 + 6 * u) + m1 * (3 * u2 - 2 * u));
    n = perpendicular(t, n);
    axis.arc.push_back(axis.arc.back() + length_of(p - axis.point.back()));
    axis.point.push_back(p);
    axis.tangent.push_back(t);
    axis.normal.push_back(n);
  }
  axis.length = axis.arc.back();
  // A flat cap is buried in the continuing branch, whose outer axis and pipe remain unchanged.
  node.r1 = std::min(.9 * radius, .25 * lerp(branch.r0, branch.r1, reach / target.length));
  join.end_slope = .25 * (branch.r1 - branch.r0) / target.length;
  join.child = child;
  join.child_arc = reach;
  node.tip = axis.point.back();
  node.length = axis.length;
  crown.axes[node.axis] = std::move(axis);
  crown.continuations.push_back(join);
}

void build_tube(const Axis& axis, f64 r0, f64 r1, u32 sides, Vec2 uv_offset, Emitter& out,
                bool flat_tip, const StemContinuation* continuation) {
  const u32 rows = axis.point.size();
  const f64 round = 2.0 * k_pi * 0.5 * (r0 + r1);  // metres once round, at the mean radius
  const f64 u0 = static_cast<f64>(uv_offset.x);
  const f64 v0 = static_cast<f64>(uv_offset.y);
  const u32 first = out.mesh.positions.size();
  // Preserve the exact cylindrical envelope promised to platform anchors. A relief changes
  // neither the core radii nor the motion skeleton; it is sampled at the budget's ring count.
  const bool straight = out.part < out.motion.structure.size() &&
                        (out.motion.structure[out.part].flags & k_straight) != 0u;
  const bool root = out.part < out.motion.parts.size() &&
                    out.motion.parts[out.part].parent == geometry::k_motion_no_parent;
  const WoodProfile& w = out.wood;
  const bool shaped = !straight && (continuation != nullptr || out.seated ||
                                    w.collar + w.flare + w.fluting + w.oval + w.knots > 0.0);
  Rng shape_rng = stream_of(w.seed, 9000000ull + out.part);
  const f64 phase = shape_rng.unit() * 2.0 * k_pi;
  const f64 knot_at = 0.2 + 0.6 * shape_rng.unit();
  const f64 collar_span = std::min(0.3 * axis.length, 3.0 * r0);
  const auto nominal_radius = [&](f64 u) {
    return continuation != nullptr
               ? continuation_radius(r0, r1, axis.length, *continuation, u * axis.length)
               : lerp(r0, r1, u);
  };
  const auto radius_at = [&](f64 arc, f64 phi, D3 radial, D3 tangent) {
    const f64 u = clamp01(arc / axis.length);
    const f64 blend = out.seated ? smoothstep01(arc / (2.0 * r0)) : 1.0;
    const f64 nominal = nominal_radius(u);
    const f64 core = out.seated ? lerp(out.seat_radius, nominal, blend) : nominal;
    const f64 basal = 1.0 - smoothstep01(arc / (collar_span + 1e-9));
    const f64 flute = det::cos(3.0 * phi + phase + 0.35 * u) * 0.65 +
                      det::cos(5.0 * phi - phase + 0.6 * u) * 0.35;
    const f64 gravity = dot(radial, k_up);
    const f64 level = 1.0 - tangent.y * tangent.y;
    const f64 oval =
        w.oval * (root ? det::cos(2.0 * phi + phase) : 2.0 * gravity * gravity - level);
    const f64 distance = (u - knot_at) / 0.07;
    const f64 bump = clamp01(0.5 + 0.5 * det::cos(phi - phase));
    const f64 knot = w.knots / (1.0 + distance * distance) * bump * bump * bump * bump;
    const f64 collar = root ? w.flare : w.collar * (1.0 + 0.35 * gravity);
    // A continuing axis has no exposed collar rim. Relief grows out of the buried seat;
    // ordinary lateral attachments retain their existing supported collar.
    if (out.seated)
      return core * (1.0 + blend * (w.fluting * flute * (1.0 - 0.65 * u) + oval + knot));
    // Preserve the existing operation order, including derivative normals, for other species.
    return core * (1.0 + basal * collar + w.fluting * flute * (1.0 - 0.65 * u) + oval + knot);
  };
  for (u32 r = 0; r < rows; ++r) {
    const f64 s = axis.arc[r] / axis.length;
    out.weight = s;
    const f64 radius = nominal_radius(s);
    const D3 t = axis.tangent[r];
    const D3 n = axis.normal[r];
    const D3 b = cross(t, n);
    for (u32 i = 0; i <= sides; ++i) {
      const f64 angle = 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(sides);
      const D3 radial = n * det::cos(angle) + b * det::sin(angle);
      f64 actual_radius = radius;
      D3 normal = radial;
      if (shaped) {
        actual_radius = radius_at(axis.arc[r], angle, radial, t);
        constexpr f64 epsilon = 0.0001;
        const auto around = [&](f64 phi) {
          return radius_at(axis.arc[r], phi, n * det::cos(phi) + b * det::sin(phi), t);
        };
        const f64 angular = (around(angle + epsilon) - around(angle - epsilon)) / (2.0 * epsilon);
        const f64 axial = (radius_at(axis.arc[r] + epsilon, angle, radial, t) -
                           radius_at(axis.arc[r] - epsilon, angle, radial, t)) /
                          (2.0 * epsilon);
        normal = radial - (n * -det::sin(angle) + b * det::cos(angle)) * (angular / actual_radius) -
                 t * axial;
        if (out.seated) {
          const f64 q = clamp01(axis.arc[r] / (2.0 * r0));
          const D3 derivative =
              t - (out.seat_origin - axis.point[0]) * (6.0 * q * (1.0 - q) / (2.0 * r0));
          const f64 along = dot(derivative, t);
          normal = radial -
                   (n * -det::sin(angle) + b * det::cos(angle)) * (angular / actual_radius) -
                   t * ((axial + dot(radial, derivative)) / along);
        }
      }
      const f64 join = out.seated ? 1.0 - smoothstep01(axis.arc[r] / (2.0 * r0)) : 0.0;
      const D3 centre =
          out.seated ? axis.point[r] + (out.seat_origin - axis.point[0]) * join : axis.point[r];
      out.vertex(centre + radial * actual_radius, normal,
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
  const u32 last = first + (rows - 1) * (sides + 1);
  if (flat_tip) {
    for (u32 i = 1; i + 1 < sides; ++i)
      out.face(last, last + i, last + i + 1, tip_dir);
    return;
  }
  const u32 tip = out.vertex(axis.point[rows - 1] + tip_dir * r1, tip_dir, u0 + 0.5 * round,
                             v0 + axis.length + r1);
  for (u32 i = 0; i < sides; ++i) {
    const Vec3 nv = out.mesh.normals[last + i];
    const D3 hint = from_f32(nv) + tip_dir;
    out.face(last + i, last + i + 1, tip, hint);
  }
}

void bind_continuation_cap(const Axis& sampled, f64 r0, f64 r1, u32 sides, u32 first,
                           const StemContinuation& join, u32 stem_part, u32 child_part,
                           f64 child_length, Motion& motion) {
  const u32 last = sampled.arc.size() - 1;
  const f64 stem_length = sampled.arc[last - 1];
  motion.parts[stem_part].length = static_cast<f32>(stem_length);
  if (stem_part < motion.structure.size()) {
    motion.structure[stem_part].tip_radius =
        static_cast<f32>(continuation_radius(r0, r1, sampled.length, join, stem_length));
  }
  for (u32 ring = 0; ring <= last; ++ring) {
    const bool cap = ring == last;
    const u32 part = cap ? child_part : stem_part;
    const f32 weight =
        static_cast<f32>(cap ? join.child_arc / child_length : sampled.arc[ring] / stem_length);
    for (u32 side = 0; side <= sides; ++side) {
      const u32 vertex = first + ring * (sides + 1) + side;
      motion.vertex_part[vertex] = part;
      motion.vertex_weight[vertex] = weight;
    }
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
  r.uniform_steps = axis.uniform_steps;
  return r;
}

D3 branch_direction(const Axis& parent, f64 arc, f64 azimuth, f64 angle, D3* radial_out) {
  const u32 rows = parent.point.size() - 1;
  const f64 f = arc / parent.length * static_cast<f64>(rows);
  u32 i = static_cast<u32>(f);
  if (i > rows) i = rows;
  if (!parent.uniform_steps) i = arc >= parent.length ? rows : segment_of(parent, arc);
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

// ---- the allometric crown (kit.h) ---------------------------------------------------------------

namespace {

f64 floor_of(f64 x) {
  const f64 t = static_cast<f64>(static_cast<i64>(x));
  return t > x ? t - 1.0 : t;
}
f64 ceil_of(f64 x) {
  const f64 t = floor_of(x);
  return t < x ? t + 1.0 : t;
}
f64 power(f64 x, f64 e) { return det::exp(det::log(x) * e); }

f64 lateral_share(const CrownSpec& spec, u32 laterals) {
  return laterals == 0 ? 0 : (1.0 - spec.leader_share) / static_cast<f64>(laterals);
}
f64 smallest_share(const CrownSpec& spec, u32 laterals) {
  const f64 lateral = lateral_share(spec, laterals);
  return lateral < spec.leader_share ? lateral : spec.leader_share;
}
// A shoot is a terminal twig where its thinnest child would be thinner than the species' twig.
bool is_terminal(const CrownSpec& spec, f64 radius, u32 laterals) {
  return radius * power(smallest_share(spec, laterals), 1.0 / spec.area_exponent) <
         spec.twig_radius;
}
// Its radius class: how many pipe steps (the thinnest child's ratio) above a twig.
i32 class_of(const CrownSpec& spec, f64 radius) {
  if (radius <= spec.twig_radius) return 0;
  const f64 step = -det::log(smallest_share(spec, spec.laterals)) / spec.area_exponent;
  const f64 c = floor_of(det::log(radius / spec.twig_radius) / step);
  return c < 0.0 ? 0 : static_cast<i32>(c);
}

// A straight shoot's axis: one row from its base to its tip.
Axis segment_axis(const CrownNode& n) {
  Axis a;
  const D3 t = normalized(n.tip - n.base);
  a.point.push_back(n.base);
  a.point.push_back(n.tip);
  a.tangent.push_back(t);
  a.tangent.push_back(t);
  const D3 normal = perpendicular(t, (t.y < 0.9 && t.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
  a.normal.push_back(normal);
  a.normal.push_back(normal);
  a.arc.push_back(0.0);
  a.arc.push_back(n.length);
  a.length = n.length;
  return a;
}

// `dir` turned towards straight down by `angle` in the plane the two span, stopping at straight
// down (the pendulous habit, CrownSpec::droop).
D3 hang_towards_ground(D3 dir, f64 angle) {
  const D3 down{0.0, -1.0, 0.0};
  const f64 c = dot(dir, down);
  const D3 across = down - dir * c;
  const f64 a = length_of(across);
  if (a < 1e-9) return dir;
  const f64 ca = det::cos(angle);
  if (ca <= c) return down;
  return normalized(dir * ca + across * (det::sin(angle) / a));
}

// The radius-only pass: how many recursion shoots of each class a root's sub-tree holds.
void count_classes(const CrownSpec& spec, f64 radius, u32 laterals, Vector<u32>& counts) {
  const f64 inv = 1.0 / spec.area_exponent;
  const f64 radii[2] = {radius * power(lateral_share(spec, laterals), inv),
                        radius * power(spec.leader_share, inv)};
  for (u32 j = 0; j <= laterals; ++j) {
    const f64 r = radii[j == laterals ? 1 : 0];
    const i32 c = is_terminal(spec, r, spec.laterals) ? 0 : class_of(spec, r);
    while (counts.size() <= static_cast<u32>(c))
      counts.push_back(0u);
    ++counts[static_cast<u32>(c)];
    if (!is_terminal(spec, r, spec.laterals)) count_classes(spec, r, spec.laterals, counts);
  }
}

struct Grower {
  Crown& crown;
  const CrownSpec& spec;
  u32 named_generations = 0;
  Vector<D3> tips;

  // The children of `index`: `laterals` along it and a leader at its tip.
  void children(u32 index, f64 radius, u32 laterals, f64 lateral_length, f64 leader_length,
                f64 from, f64 to, f64 spread_scale) {
    const f64 inv = 1.0 / spec.area_exponent;
    const f64 r_lateral = laterals == 0 ? 0 : radius * power(lateral_share(spec, laterals), inv);
    const f64 r_leader = laterals == 0 ? radius : radius * power(spec.leader_share, inv);
    Rng place = stream_of(spec.seed, spec.stream + 2u * static_cast<u64>(index));
    const f64 turn = place.unit() * 2.0 * k_pi;
    for (u32 j = 0; j <= laterals; ++j) {
      const CrownNode parent = crown.nodes[index];
      const Axis local = parent.axis != k_crown_none ? Axis{} : segment_axis(parent);
      const Axis& along = parent.axis != k_crown_none ? crown.axes[parent.axis] : local;
      const bool leader = j == laterals;
      const f64 f = leader ? 1.0
                    : spec.opposite
                        ? (static_cast<f64>(j / 2u) + 0.5) / static_cast<f64>((laterals + 1u) / 2u)
                        : (static_cast<f64>(j) + 0.5) / static_cast<f64>(laterals);
      const f64 s = leader ? along.length : along.length * (from + (to - from) * f);
      const f64 jitter_around = place.symmetric();
      const f64 around =
          turn +
          (spec.opposite ? static_cast<f64>(j / 2u) * 0.5 * k_pi + static_cast<f64>(j % 2u) * k_pi
                         : static_cast<f64>(j) * k_golden_angle) +
          0.2 * jitter_around;
      const f64 jitter_spread = place.symmetric();
      const f64 spread = (leader ? spec.leader_angle : spec.branch_angle * spread_scale) *
                         (1.0 + 0.1 * jitter_spread);
      const f64 jitter_length = place.unit();
      const f64 r = leader ? r_leader : r_lateral;
      const bool terminal = is_terminal(spec, r, spec.laterals);
      f64 length = (leader ? leader_length : lateral_length * (1.0 - 0.35 * f)) *
                   (0.9 + 0.2 * jitter_length);
      if (terminal) {
        length = spec.twig_length * (0.85 + 0.3 * jitter_length);
      } else if (length < spec.twig_length) {
        length = spec.twig_length;
      }
      D3 radial;
      D3 dir = branch_direction(along, s, around, spread, &radial);
      if (spec.droop > 0.0) {
        const i32 cls = terminal ? 0 : class_of(spec, r);
        if (cls <= spec.droop_class) {
          const f64 classes = static_cast<f64>(spec.droop_class + 1);
          dir = hang_towards_ground(dir, spec.droop * (classes - static_cast<f64>(cls)) / classes);
        }
      }
      const f64 r_here = lerp(parent.r0, parent.r1, s / along.length);
      CrownNode child;
      child.base = axis_point(along, s) + radial * (0.4 * r_here);
      child.r0 = r;
      child.r1 = terminal ? 0.6 * r : r * power(spec.leader_share, inv);
      child.length = length;
      child.parent = index;
      child.generation = parent.generation + 1;
      child.cls = terminal ? 0 : class_of(spec, r);
      const bool part = (parent.flags & k_crown_part) != 0u && child.cls >= crown.part_class;
      if ((parent.flags & k_crown_part) != 0u) {
        child.carrier = index;
        child.weight = s / along.length;
      } else {
        child.carrier = parent.carrier;
        child.weight = parent.weight;
      }
      const u32 child_index = crown.nodes.size();
      Rng grow = stream_of(spec.seed, spec.stream + 2u * static_cast<u64>(child_index) + 1u);
      if (part) {
        child.flags |= k_crown_part;
        child.carrier = child_index;
        child.weight = 0.0;
        Axis axis =
            grow_axis(child.base, dir, length, 8, k_up, spec.upturn / length, spec.wander, grow);
        child.dir = axis.tangent[0];
        child.tip = axis.point.back();
        child.axis = crown.axes.size();
        crown.axes.push_back(std::move(axis));
        ++crown.recursion_parts;
      } else {
        // A folded shoot is straight: half its turn towards the light taken at once, one wander.
        D3 d = dir;
        const D3 hinge = cross(d, k_up);
        const f64 sine = length_of(hinge);
        if (sine > 1e-9) d = rotate(d, hinge * (1.0 / sine), 0.5 * spec.upturn * sine);
        const D3 n = perpendicular(d, (d.y < 0.9 && d.y > -0.9) ? k_up : D3{1.0, 0.0, 0.0});
        const f64 w0 = spec.wander * grow.symmetric();
        d = rotate(d, n, w0);
        const f64 w1 = spec.wander * grow.symmetric();
        d = normalized(rotate(d, normalized(cross(d, n)), w1));
        child.dir = d;
        child.tip = child.base + d * length;
      }
      if (terminal) child.flags |= k_crown_terminal;
      tips.push_back(child.tip);
      crown.nodes.push_back(child);
      if (terminal) {
        crown.twigs.push_back(child_index);
        const u32 orders = named_generations + child.generation;
        if (orders > crown.orders) crown.orders = orders;
      } else {
        const f64 next = length * spec.length_ratio;
        children(child_index, r, spec.laterals, next, next, spec.from, 1.0, 1.0);
      }
    }
  }
};

// The convex hull's area of points seen from above (Andrew's monotone chain).
f64 footprint_of(Vector<D3>& points) {
  if (points.size() < 3) return 0.0;
  std::sort(points.begin(), points.end(),
            [](const D3& a, const D3& b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
  const auto turn = [](const D3& o, const D3& a, const D3& b) {
    return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
  };
  Vector<D3> hull;
  for (u32 pass = 0; pass < 2; ++pass) {
    const u32 start = hull.size();
    for (u32 k = 0; k < points.size(); ++k) {
      const D3 p = pass == 0 ? points[k] : points[points.size() - 1 - k];
      while (hull.size() >= start + 2 && turn(hull[hull.size() - 2], hull.back(), p) <= 0.0)
        hull.pop_back();
      hull.push_back(p);
    }
    hull.pop_back();
  }
  f64 twice = 0.0;
  for (u32 k = 0; k < hull.size(); ++k) {
    const D3& a = hull[k];
    const D3& b = hull[(k + 1) % hull.size()];
    twice += a.x * b.z - b.x * a.z;
  }
  return 0.5 * (twice < 0.0 ? -twice : twice);
}

}  // namespace

f64 crown_leader_radius(const CrownSpec& spec, f64 radius) {
  return radius * power(spec.leader_share, 1.0 / spec.area_exponent);
}

u32 crown_add_named(Crown& crown, const Axis& axis, f64 r0, f64 r1, u32 parent) {
  CrownNode n;
  n.base = axis.point[0];
  n.tip = axis.point.back();
  n.dir = axis.tangent[0];
  n.r0 = r0;
  n.r1 = r1;
  n.length = axis.length;
  n.parent = parent;
  n.flags = k_crown_named | k_crown_part;
  const u32 index = crown.nodes.size();
  n.carrier = index;
  n.axis = crown.axes.size();
  crown.axes.push_back(axis);
  crown.nodes.push_back(n);
  return index;
}

void crown_grow(Crown& crown, const CrownSpec& spec, const Vector<CrownRoot>& roots,
                u32 named_generations) {
  crown.first_recursion = crown.nodes.size();
  crown.laterals = spec.laterals;
  // The part class: the finest class whose shoots and every coarser one fit the cap.
  Vector<u32> counts;
  for (const CrownRoot& root : roots) {
    if (!is_terminal(spec, root.radius, root.laterals))
      count_classes(spec, root.radius, root.laterals, counts);
  }
  crown.part_class = static_cast<i32>(counts.size());
  u32 total = 0;
  for (u32 c = counts.size(); c > 0; --c) {
    total += counts[c - 1];
    if (total > spec.part_cap) break;
    crown.part_class = static_cast<i32>(c - 1);
  }
  Grower grower{crown, spec, named_generations, {}};
  for (const CrownNode& n : crown.nodes)
    grower.tips.push_back(n.tip);
  for (const CrownRoot& root : roots) {
    if (is_terminal(spec, root.radius, root.laterals)) {
      // A root too thin to branch is a twig itself (the smallest trees).
      crown.nodes[root.node].flags |= k_crown_terminal;
      crown.twigs.push_back(root.node);
      if (named_generations > crown.orders) crown.orders = named_generations;
      continue;
    }
    grower.children(root.node, root.radius, root.laterals, root.lateral_length,
                    root.lateral_length * 0.65, root.from, root.to, root.spread_scale);
  }
  crown.footprint =
      std::max(footprint_of(grower.tips), 4.0 * crown.leaf_length * crown.leaf_length);
}

void crown_distribute_leaves(Crown& crown, f64 cards, u32 cap) {
  const u32 n = crown.twigs.size();
  if (n == 0) return;
  const f64 rate = cards / (2.0 * static_cast<f64>(n));
  for (u32 t = 0; t < n; ++t) {
    const f64 a = floor_of(static_cast<f64>(t) * rate);
    const f64 b = floor_of(static_cast<f64>(t + 1) * rate);
    u32 c = static_cast<u32>(b - a);
    if (c > cap) c = cap;
    crown.nodes[crown.twigs[t]].cards = c;
  }
}

u32 crown_tube_sides(const CrownPlan& plan, f64 radius) {
  const f64 s = ceil_of(2.0 * k_pi * radius / plan.edge_m);
  if (s < 3.0) return 3;
  return s > static_cast<f64>(plan.max_sides) ? plan.max_sides : static_cast<u32>(s);
}

u32 crown_part_rows(const CrownPlan& plan, f64 length) {
  const f64 r = ceil_of(length / plan.row_m);
  if (r < 1.0) return 1;
  return r > 8.0 ? 8u : static_cast<u32>(r);
}

namespace {

// The sub-tree root a twig's leaves merge into at `merge` generations up: never past a part.
u32 group_of(const Crown& crown, u32 twig, u32 merge) {
  u32 g = twig;
  for (u32 k = 0; k < merge; ++k) {
    const CrownNode& n = crown.nodes[g];
    if ((n.flags & k_crown_part) != 0u || n.parent == k_crown_none) break;
    g = n.parent;
  }
  return g;
}

// Every group in the order its first twig comes, with its twigs' cards a side summed.
void crown_groups(const Crown& crown, u32 merge, Vector<u32>& order, Vector<u32>& sums) {
  sums.assign(crown.nodes.size(), 0u);
  order.clear();
  for (const u32 t : crown.twigs) {
    const u32 g = group_of(crown, t, merge);
    if (sums[g] == 0u && crown.nodes[t].cards > 0u) order.push_back(g);
    sums[g] += crown.nodes[t].cards;
  }
}

u32 merged_cards(u32 sum, u32 merge, u32 halvings, u32 laterals, u32 cap) {
  f64 share = 1.0;
  for (u32 k = 0; k < merge; ++k)
    share *= static_cast<f64>(laterals + 1);
  u32 n = static_cast<u32>(ceil_of(static_cast<f64>(sum) / share));
  if (n < 1) n = 1;
  if (n > cap) n = cap;
  for (u32 k = 0; k < halvings && n > 1; ++k)
    n = (n + 1) / 2;
  return n;
}

// The cap on a merged spray: the most cards a twig carries. A sub-tree's card stands for its
// leaves, and a spray of more cards than a twig would hold buys nothing at a distance.
u32 most_cards(const Crown& crown) {
  u32 most = 1;
  for (const u32 t : crown.twigs)
    if (crown.nodes[t].cards > most) most = crown.nodes[t].cards;
  return most;
}

}  // namespace

namespace {
u32 group_cards(const Crown& crown, const CrownPlan& plan, u32 group, u32 sum, u32 legacy_cap) {
  if (crown.leaf_placement != LeafPlacement::opposite_simple &&
      crown.leaf_placement != LeafPlacement::alternate_simple)
    return merged_cards(sum, plan.merge, plan.halvings, crown.laterals, legacy_cap);
  // A retained shoot can carry several actual leaves. Do not inherit a terminal twig's tiny
  // per-twig count after aggregating many twigs, and do not inflate blades to fill its extent.
  const f64 spacing = crown.leaf_placement == LeafPlacement::alternate_simple ? .8 : 1.2;
  const u32 space =
      std::max(1u, static_cast<u32>(.8 * crown.nodes[group].length /
                                    (spacing * crown.leaf_length * crown.leaf_width)));
  u32 cards = std::min(sum, space);
  for (u32 i = 0; i < plan.halvings; ++i)
    cards = (cards + 1) / 2;
  return cards;
}
}  // namespace

u32 crown_triangles(const Crown& crown, const CrownPlan& plan) {
  u32 n = 0;
  const bool compound =
      plan.merge == 0 && (crown.leaf_placement == LeafPlacement::opposite_compound ||
                          crown.leaf_placement == LeafPlacement::alternate_compound);
  for (u32 i = crown.first_recursion; i < crown.nodes.size(); ++i) {
    const CrownNode& node = crown.nodes[i];
    if (compound && (node.flags & k_crown_terminal) != 0u) continue;
    if ((node.flags & k_crown_part) != 0u) {
      n += tube_triangles(crown_part_rows(plan, node.length), crown_tube_sides(plan, node.r0));
    } else if (node.cls >= plan.wood_class) {
      n += tube_triangles(1, crown_tube_sides(plan, node.r0)) - (plan.flat_folded_tips ? 2u : 0u);
    }
  }
  if (plan.merge == 0) {
    for (const u32 t : crown.twigs) {
      const u32 pairs = crown.nodes[t].cards;
      if (crown.leaf_placement == LeafPlacement::opposite_compound ||
          crown.leaf_placement == LeafPlacement::alternate_compound) {
        n += compound_triangles(pairs, crown.leaf_placement == LeafPlacement::opposite_compound);
      } else {
        n += spray_triangles(pairs);
        if (crown.terminal_leaf && pairs > 0u) n += 2;
      }
    }
    return n;
  }
  Vector<u32> order;
  Vector<u32> sums;
  crown_groups(crown, plan.merge, order, sums);
  const u32 cap = most_cards(crown);
  for (const u32 g : order)
    n += spray_triangles(group_cards(crown, plan, g, sums[g], cap));
  return n;
}

void crown_emit_wood(Crown& crown, const CrownPlan& plan, Emitter& out, bool structural) {
  const bool compound =
      plan.merge == 0 && (crown.leaf_placement == LeafPlacement::opposite_compound ||
                          crown.leaf_placement == LeafPlacement::alternate_compound);
  for (u32 i = crown.first_recursion; i < crown.nodes.size(); ++i) {
    CrownNode& node = crown.nodes[i];
    if (compound && (node.flags & k_crown_terminal) != 0u) {
      if ((node.flags & k_crown_part) != 0u)
        node.part = out.begin_part(node.base, normalized(node.tip - node.base), node.length,
                                   crown.nodes[node.parent].part);
      continue;
    }
    if ((node.flags & k_crown_part) != 0u) {
      // A part's parent is a part (a class never grows from parent to child).
      const u32 parent_part = crown.nodes[node.parent].part;
      const Axis& axis = crown.axes[node.axis];
      const u32 rows = crown_part_rows(plan, node.length);
      const u32 sides = crown_tube_sides(plan, node.r0);
      if (structural) {
        node.part = emit_tube_part(out, axis, node.r0, node.r1, rows, sides, parent_part, 0u);
      } else {
        node.part = out.begin_part(axis.point[0], axis.tangent[0], axis.length, parent_part);
        build_tube(resample_axis(axis, rows), node.r0, node.r1, sides, Vec2{0.0f, 0.0f}, out);
      }
      continue;
    }
    if (node.cls < plan.wood_class) continue;
    out.part = crown.nodes[node.carrier].part;
    const u32 first = out.mesh.positions.size();
    build_tube(segment_axis(node), node.r0, node.r1, crown_tube_sides(plan, node.r0),
               Vec2{0.0f, 0.0f}, out, plan.flat_folded_tips);
    for (u32 v = first; v < out.mesh.positions.size(); ++v)
      out.motion.vertex_weight[v] = static_cast<f32>(node.weight);
  }
}

f64 crown_emit_leaves(const Crown& crown, const CrownPlan& plan, Emitter& out, f64 card_length,
                      f64 angle, f64 lift, f64 droop, Vector<SprayRange>* sprays) {
  const auto spray = [&](const CrownNode& node, f64 from, u32 cards, f64 length) {
    const u32 first_index = out.mesh.indices.size();
    const bool part = (node.flags & k_crown_part) != 0u;
    const Axis local = part ? Axis{} : segment_axis(node);
    const Axis& axis = part ? crown.axes[node.axis] : local;
    D3 side = cross(axis.tangent[0], k_up);
    if (dot(side, side) < 1e-6) side = D3{1.0, 0.0, 0.0};
    out.part = crown.nodes[node.carrier].part;
    const u32 first = out.mesh.positions.size();
    const bool simple = crown.leaf_placement == LeafPlacement::opposite_simple ||
                        crown.leaf_placement == LeafPlacement::alternate_simple;
    if (simple || (crown.leaf_placement != LeafPlacement::spray && plan.merge == 0)) {
      Rng rng = stream_of(crown.leaf_seed, 700000u + static_cast<u64>(&node - crown.nodes.data()));
      if (crown.leaf_placement == LeafPlacement::opposite_compound ||
          crown.leaf_placement == LeafPlacement::alternate_compound) {
        build_compound_leaves(axis, cards, length, length * crown.leaf_width, angle,
                              crown.leaf_placement, rng, out);
      } else {
        build_simple_leaves(axis, cards, length, length * crown.leaf_width, angle,
                            crown.leaf_placement, rng, out);
      }
    } else {
      build_spray(axis, from, cards, length, angle, lift, droop, normalized(side), out);
    }
    if (crown.terminal_leaf && crown.leaf_placement == LeafPlacement::spray && plan.merge == 0) {
      out.weight = 1.0;
      build_leaf_card(axis.point.back(), axis.tangent.back(), normalized(side), length,
                      length * 0.3, droop, out);
    }
    if (sprays != nullptr) {
      SprayRange r;
      r.node = static_cast<u32>(&node - crown.nodes.data());
      r.first_index = first_index;
      r.index_count = out.mesh.indices.size() - first_index;
      r.first_vertex = first;
      r.vertex_count = out.mesh.positions.size() - first;
      sprays->push_back(r);
    }
    if (!part) {
      for (u32 v = first; v < out.mesh.positions.size(); ++v)
        out.motion.vertex_weight[v] = static_cast<f32>(node.weight);
    }
  };
  f64 longest = 0.0;
  if (plan.merge == 0) {
    for (const u32 t : crown.twigs) {
      const CrownNode& node = crown.nodes[t];
      if (node.cards == 0u) continue;
      f64 from = node.length - static_cast<f64>(node.cards) * crown.card_step;
      if (from < 0.0) from = 0.0;
      spray(node, from, node.cards, card_length);
      longest = card_length;
    }
    return longest;
  }
  Vector<u32> order;
  Vector<u32> sums;
  crown_groups(crown, plan.merge, order, sums);
  // A merged card reaches as far as the sub-tree's leaves do: the farthest twig tip from the
  // root's line, and a leaf beyond it.
  Vector<f64> extent(crown.nodes.size(), 0.0);
  for (const u32 t : crown.twigs) {
    const u32 g = group_of(crown, t, plan.merge);
    const CrownNode& root = crown.nodes[g];
    const D3 d = root.tip - root.base;
    const f64 dd = dot(d, d);
    const D3 tip = crown.nodes[t].tip;
    const f64 along = dd > 0.0 ? clamp01(dot(tip - root.base, d) / dd) : 0.0;
    const D3 off = tip - (root.base + d * along);
    const f64 reach = length_of(off) + card_length;
    if (reach > extent[g]) extent[g] = reach;
  }
  const u32 most = most_cards(crown);
  for (const u32 g : order) {
    const CrownNode& root = crown.nodes[g];
    const bool simple = crown.leaf_placement == LeafPlacement::opposite_simple ||
                        crown.leaf_placement == LeafPlacement::alternate_simple;
    const f64 length = simple ? card_length : (extent[g] > card_length ? extent[g] : card_length);
    spray(root, 0.3 * root.length, group_cards(crown, plan, g, sums[g], most), length);
    if (length > longest) longest = length;
  }
  return longest;
}

void crown_table(const Crown& crown, Vector<CrownShoot>& out) {
  out.clear();
  out.reserve(crown.nodes.size());
  for (const CrownNode& n : crown.nodes) {
    CrownShoot s;
    s.parent = n.parent;
    s.generation = n.generation;
    s.base_radius = static_cast<f32>(n.r0);
    s.tip_radius = static_cast<f32>(n.r1);
    s.length = static_cast<f32>(n.length);
    s.flags = ((n.flags & k_crown_terminal) != 0u ? k_crown_shoot_terminal : 0u) |
              ((n.flags & k_crown_part) != 0u ? k_crown_shoot_part : 0u) |
              ((n.flags & k_crown_named) != 0u ? k_crown_shoot_named : 0u);
    out.push_back(s);
  }
}

f64 triangles_area(const assets::MeshData& mesh, u32 first_index) {
  f64 area = 0.0;
  for (u32 i = first_index; i + 2 < mesh.indices.size(); i += 3) {
    const D3 a = from_f32(mesh.positions[mesh.indices[i]]);
    const D3 b = from_f32(mesh.positions[mesh.indices[i + 1]]);
    const D3 c = from_f32(mesh.positions[mesh.indices[i + 2]]);
    area += 0.5 * length_of(cross(b - a, c - a));
  }
  return area;
}

// ---- the understory (kit.h) ---------------------------------------------------------------------

void build_spray_profile(const Axis& axis, f64 from, u32 cards, const Vector<f64>& lengths,
                         f64 angle, f64 lift, f64 droop, D3 side, Emitter& out) {
  const f64 span = axis.length - from;
  Vector<D3> centre(cards + 1);
  Vector<D3> tangent(cards + 1);
  Vector<D3> lateral(cards + 1);
  Vector<D3> upper(cards + 1);
  Vector<u32> mid(cards + 1);
  Vector<u32> tips[2] = {Vector<u32>(cards + 1), Vector<u32>(cards + 1)};
  const f64 ca = det::cos(angle);
  const f64 sa = det::sin(angle);
  const f64 cl = det::cos(lift);
  const f64 sl = det::sin(lift);
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
    const f64 length = lengths[k];
    for (u32 j = 0; j < 2; ++j) {
      const f64 sigma = j == 0 ? 1.0 : -1.0;
      const D3 out_dir = lateral[k] * (sigma * cl) + upper[k] * sl;
      const D3 d = normalized(tangent[k] * ca + out_dir * sa);
      const D3 tip = centre[k] + d * length - k_up * (droop * length);
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

void build_blade(const Axis& axis, f64 width, D3 side, Emitter& out) {
  const u32 rows = axis.point.size();
  Vector<u32> left(rows);
  Vector<u32> right(rows);
  Vector<D3> facing(rows);
  for (u32 r = 0; r < rows; ++r) {
    const f64 s = axis.arc[r] / axis.length;
    out.weight = s;
    const D3 t = axis.tangent[r];
    const D3 lateral = perpendicular(t, side);
    facing[r] = normalized(cross(lateral, t));
    if (r + 1 == rows) {
      left[r] = right[r] = out.vertex(axis.point[r], facing[r], 0.5, 1.0);
      break;
    }
    // Straight edges to 0.65 of the length, then closing to the point: the rows between are
    // collinear, so the simplifier takes them first without losing the blade's area.
    const f64 w = width * (s < 0.65 ? 1.0 - 0.3 * s : (1.0 - s) * (0.805 / 0.35));
    const f64 half = 0.5 * w / width;
    left[r] = out.vertex(axis.point[r] - lateral * (0.5 * w), facing[r], 0.5 - half, s);
    right[r] = out.vertex(axis.point[r] + lateral * (0.5 * w), facing[r], 0.5 + half, s);
  }
  for (u32 r = 0; r + 1 < rows; ++r) {
    if (r + 2 == rows) {
      out.face(left[r], right[r], left[r + 1], facing[r]);
      break;
    }
    out.face(left[r], right[r], right[r + 1], facing[r]);
    out.face(left[r], right[r + 1], left[r + 1], facing[r]);
  }
}

void build_opposite_leaves(const Axis& axis, u32 pairs, f64 length, f64 width, f64 angle, Rng& rng,
                           Emitter& out) {
  build_simple_leaves(axis, pairs, length, width, angle, LeafPlacement::opposite_simple, rng, out);
}

void build_simple_leaves(const Axis& axis, u32 pairs, f64 length, f64 width, f64 angle,
                         LeafPlacement placement, Rng& rng, Emitter& out) {
  const bool alternate = placement == LeafPlacement::alternate_simple;
  if (alternate) pairs *= 2;
  const f64 phase = rng.unit() * 2.0 * k_pi;
  for (u32 k = 0; k < pairs; ++k) {
    const f64 station =
        (static_cast<f64>(k) + 0.5 + 0.18 * rng.symmetric()) / static_cast<f64>(pairs);
    const f64 arc = axis.length * (0.15 + 0.8 * station);
    const D3 tangent = axis_tangent(axis, arc);
    const D3 base = axis_point(axis, arc);
    const D3 reference =
        perpendicular(tangent, (tangent.y > -.9 && tangent.y < .9) ? k_up : D3{1, 0, 0});
    const D3 radial = rotate(
        reference, tangent, phase + static_cast<f64>(k) * (alternate ? k_golden_angle : .5 * k_pi));
    const f64 spread = angle + 8.0 * k_degree * rng.symmetric();
    const f64 scale = .9 + .1 * rng.unit();
    out.weight = arc / axis.length;
    for (u32 side = 0; side < (alternate ? 1u : 2u); ++side) {
      const f64 sign = side == 0 ? 1.0 : -1.0;
      const D3 direction =
          normalized(tangent * det::cos(spread) + radial * (sign * det::sin(spread)) - k_up * .12);
      D3 across = cross(direction, k_up);
      if (dot(across, across) < 1e-6) across = perpendicular(direction, reference);
      across = rotate(normalized(across), direction, 20.0 * k_degree * rng.symmetric());
      D3 normal = normalized(cross(across, direction));
      if (normal.y < 0) normal = normal * -1.0;
      const D3 half_width = across * (.5 * width * scale);
      const D3 tip = base + direction * (length * scale);
      // Full rectangular UV coverage preserves the lobes and the image's short petiole.
      // No vertices are shared between blades or opposite pairs.
      const u32 a = out.vertex(base - half_width, normal, 0, 1);
      const u32 b = out.vertex(base + half_width, normal, 1, 1);
      const u32 c = out.vertex(tip + half_width, normal, 1, 0);
      const u32 d = out.vertex(tip - half_width, normal, 0, 0);
      out.face(a, b, c, normal);
      out.face(a, c, d, normal);
    }
  }
}

u32 compound_triangles(u32 pairs, bool terminal) {
  return pairs == 0 ? 0u : 4u * pairs + (terminal ? 2u : 0u) + tube_triangles(1, 3) - 2u;
}

void build_compound_leaves(const Axis& twig, u32 pairs, f64 length, f64 width, f64 angle,
                           LeafPlacement placement, Rng& rng, Emitter& out, bool rachises) {
  if (pairs == 0) return;
  const bool ash = placement == LeafPlacement::opposite_compound;
  // A terminal unit is one whole compound leaf. The crown's growth rules place those units
  // oppositely for ash and alternately for walnut; there is no extra woody twig underneath it.
  const D3 direction = twig.tangent[0];
  const D3 reference = perpendicular(direction, twig.normal[0]);
  const f64 phase = rng.unit() * 2.0 * k_pi;
  const f64 reach = ash ? .30 : .45;
  const Axis rachis = grow_axis(twig.point[0], direction, reach, 1, D3{0, -1, 0}, .25, 0, rng);
  if (rachises) {
    build_tube(rachis, .0008, .00025, 3, {}, out, true);
    return;
  }
  const D3 across = rotate(reference, direction, phase);
  for (u32 node = 0; node < pairs + (ash ? 1u : 0u); ++node) {
    const bool terminal = node == pairs;
    const f64 at = terminal ? reach : reach * (.2 + .7 * (node + .5) / pairs);
    const D3 base = axis_point(rachis, at);
    const D3 along = axis_tangent(rachis, at);
    out.weight = at / reach;
    for (u32 side = 0; side < (terminal ? 1u : 2u); ++side) {
      const D3 d = terminal ? along
                            : normalized(along * det::cos(angle) +
                                         across * ((side == 0 ? 1.0 : -1.0) * det::sin(angle)));
      D3 lateral = cross(d, k_up);
      if (dot(lateral, lateral) < 1e-6) lateral = perpendicular(d, reference);
      lateral = normalized(lateral);
      const D3 normal = normalized(cross(lateral, d));
      const f64 scale = .9 + .1 * rng.unit();
      const D3 half = lateral * (.5 * width * scale);
      const D3 tip = base + d * (length * scale);
      const u32 a = out.vertex(base - half, normal, 0, 1);
      const u32 b = out.vertex(base + half, normal, 1, 1);
      const u32 c = out.vertex(tip + half, normal, 1, 0);
      const u32 e = out.vertex(tip - half, normal, 0, 0);
      out.face(a, b, c, normal);
      out.face(a, c, e, normal);
    }
  }
}
void build_leaf_card(D3 base, D3 direction, D3 side, f64 length, f64 width, f64 droop,
                     Emitter& out) {
  const D3 d = normalized(direction);
  const D3 lateral = perpendicular(d, side);
  D3 facing = normalized(cross(lateral, d));
  if (facing.y < 0.0) facing = facing * -1.0;
  const D3 widest = base + d * (0.35 * length) - k_up * (0.2 * droop * length);
  const D3 tip = base + d * length - k_up * (droop * length);
  const u32 b = out.vertex(base, facing, 0.5, 0.0);
  const u32 l = out.vertex(widest - lateral * (0.5 * width), facing, 0.0, 0.35);
  const u32 r = out.vertex(widest + lateral * (0.5 * width), facing, 1.0, 0.35);
  const u32 t = out.vertex(tip, facing, 0.5, 1.0);
  out.face(b, r, t, facing);
  out.face(b, t, l, facing);
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
