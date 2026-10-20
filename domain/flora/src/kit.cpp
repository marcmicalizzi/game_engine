// The species solvers' shared shapes (kit.h): a grown axis, a tube along it, a spray of cards.
#include "kit.h"

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
  return (1.0 - spec.leader_share) / static_cast<f64>(laterals);
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
    const f64 r_lateral = radius * power(lateral_share(spec, laterals), inv);
    const f64 r_leader = radius * power(spec.leader_share, inv);
    Rng place = stream_of(spec.seed, spec.stream + 2u * static_cast<u64>(index));
    const f64 turn = place.unit() * 2.0 * k_pi;
    for (u32 j = 0; j <= laterals; ++j) {
      const CrownNode parent = crown.nodes[index];
      const Axis local = parent.axis != k_crown_none ? Axis{} : segment_axis(parent);
      const Axis& along = parent.axis != k_crown_none ? crown.axes[parent.axis] : local;
      const bool leader = j == laterals;
      const f64 f = leader ? 1.0 : (static_cast<f64>(j) + 0.5) / static_cast<f64>(laterals);
      const f64 s = leader ? along.length : along.length * (from + (to - from) * f);
      const f64 jitter_around = place.symmetric();
      const f64 around = turn + static_cast<f64>(j) * k_golden_angle + 0.2 * jitter_around;
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
      const D3 dir = branch_direction(along, s, around, spread, &radial);
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
  crown.footprint = footprint_of(grower.tips);
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

u32 crown_triangles(const Crown& crown, const CrownPlan& plan) {
  u32 n = 0;
  for (u32 i = crown.first_recursion; i < crown.nodes.size(); ++i) {
    const CrownNode& node = crown.nodes[i];
    if ((node.flags & k_crown_part) != 0u) {
      n += tube_triangles(crown_part_rows(plan, node.length), crown_tube_sides(plan, node.r0));
    } else if (node.cls >= plan.wood_class) {
      n += tube_triangles(1, crown_tube_sides(plan, node.r0));
    }
  }
  if (plan.merge == 0) {
    for (const u32 t : crown.twigs)
      n += spray_triangles(crown.nodes[t].cards);
    return n;
  }
  Vector<u32> order;
  Vector<u32> sums;
  crown_groups(crown, plan.merge, order, sums);
  const u32 cap = most_cards(crown);
  for (const u32 g : order)
    n += spray_triangles(merged_cards(sums[g], plan.merge, plan.halvings, crown.laterals, cap));
  return n;
}

void crown_emit_wood(Crown& crown, const CrownPlan& plan, Emitter& out, bool structural) {
  for (u32 i = crown.first_recursion; i < crown.nodes.size(); ++i) {
    CrownNode& node = crown.nodes[i];
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
               Vec2{0.0f, 0.0f}, out);
    for (u32 v = first; v < out.mesh.positions.size(); ++v)
      out.motion.vertex_weight[v] = static_cast<f32>(node.weight);
  }
}

f64 crown_emit_leaves(const Crown& crown, const CrownPlan& plan, Emitter& out, f64 card_length,
                      f64 angle, f64 lift, f64 droop) {
  const auto spray = [&](const CrownNode& node, f64 from, u32 cards, f64 length) {
    const bool part = (node.flags & k_crown_part) != 0u;
    const Axis local = part ? Axis{} : segment_axis(node);
    const Axis& axis = part ? crown.axes[node.axis] : local;
    D3 side = cross(axis.tangent[0], k_up);
    if (dot(side, side) < 1e-6) side = D3{1.0, 0.0, 0.0};
    out.part = crown.nodes[node.carrier].part;
    const u32 first = out.mesh.positions.size();
    build_spray(axis, from, cards, length, angle, lift, droop, normalized(side), out);
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
    const f64 length = extent[g] > card_length ? extent[g] : card_length;
    spray(root, 0.3 * root.length,
          merged_cards(sums[g], plan.merge, plan.halvings, crown.laterals, most), length);
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
