// Branch-aware radii, at cook time only (G129; experiments/tree-branch-taper.md).
#include "kit.h"

#include <algorithm>
#include <utility>

namespace engine::flora::detail {
namespace {
f64 power(f64 value, f64 exponent) { return value > 0 ? det::exp(det::log(value) * exponent) : 0; }
f64 axial_taper(f64 u) { return 1.0 - .05 * clamp01(u); }
D3 point_at(const Crown& crown, const CrownNode& node, f64 arc) {
  return node.axis == k_crown_none ? node.base + (node.tip - node.base) * clamp01(arc / node.length)
                                   : axis_point(crown.axes[node.axis], arc);
}
struct Outlet {
  u32 child;
  f64 arc;
  f64 share;
  f64 half_span;
};
}  // namespace

f64 axis_radius(const Axis& axis, f64 r0, f64 r1, f64 arc) {
  if (axis.radii.empty()) return lerp(r0, r1, clamp01(arc / axis.length));
  const auto upper =
      std::upper_bound(axis.radii.begin(), axis.radii.end(), arc,
                       [](f64 value, const RadiusKnot& knot) { return value < knot.arc; });
  if (upper == axis.radii.begin()) return upper->radius;
  if (upper == axis.radii.end()) return axis.radii.back().radius;
  const RadiusKnot& a = *(upper - 1);
  return lerp(a.radius, upper->radius, (arc - a.arc) / (upper->arc - a.arc));
}

bool crown_allocate_upper(Crown& crown, Vector<CrownRoot>& roots, u32 stem, const CrownSpec& spec) {
  const CrownNode parent = crown.nodes[stem];
  if (parent.r0 < .03) return false;
  const Axis original = crown.axes[parent.axis];
  f64 last = 0, budget = 0, demand = 0;
  for (const CrownRoot& root : roots) {
    const CrownNode& node = crown.nodes[root.node];
    if (node.parent != stem) continue;
    last = std::max(last, node.parent_arc);
    budget += power(root.radius, spec.area_exponent);
    demand += node.length * node.length;
  }
  if (last <= 0 || last >= original.length || budget <= 0) return false;
  Axis leader;
  leader.length = original.length - last;
  leader.uniform_steps = true;
  D3 normal = original.normal[0];
  constexpr u32 steps = 16;
  for (u32 i = 0; i <= steps; ++i) {
    const f64 arc = leader.length * static_cast<f64>(i) / steps;
    const D3 tangent = axis_tangent(original, last + arc);
    normal = perpendicular(tangent, normal);
    leader.arc.push_back(arc);
    leader.point.push_back(axis_point(original, last + arc));
    leader.tangent.push_back(tangent);
    leader.normal.push_back(normal);
  }
  // A continuing leader retains the species' local apical allocation at the final fork.
  // Length alone underestimates its crown because this is only the remaining stem segment.
  f64 final_lateral = 0;
  for (const CrownRoot& root : roots) {
    const CrownNode& node = crown.nodes[root.node];
    if (node.parent == stem && node.parent_arc == last) final_lateral += node.length * node.length;
  }
  const f64 leader_demand = std::max(leader.length * leader.length,
                                     final_lateral * spec.leader_share / (1.0 - spec.leader_share));
  demand += leader_demand;
  const f64 inverse = 1.0 / spec.area_exponent;
  for (CrownRoot& root : roots) {
    CrownNode& node = crown.nodes[root.node];
    if (node.parent != stem) continue;
    root.radius = power(budget * node.length * node.length / demand, inverse);
    node.r0 = root.radius;
    node.r1 = crown_leader_radius(spec, node.r0);
  }
  CrownRoot upper;
  upper.radius = power(budget * leader_demand / demand, inverse);
  upper.node =
      crown_add_named(crown, leader, upper.radius, crown_leader_radius(spec, upper.radius), stem);
  crown.nodes[upper.node].parent_arc = last;
  crown.nodes[upper.node].flags |= k_crown_continuation;
  upper.laterals = spec.laterals;
  upper.lateral_length = .4 * leader.length;
  upper.from = spec.from;
  upper.to = .9;
  roots.push_back(upper);
  return true;
}
void crown_finish_stem(Crown& crown, u32 stem, f64 exponent) {
  CrownNode& node = crown.nodes[stem];
  // Fine young leaders already approach shoot dimensions; do not shorten their natural tip.
  if (node.r0 < .03) return;
  f64 last = 0;
  for (const CrownNode& child : crown.nodes)
    if (child.parent == stem) last = std::max(last, child.parent_arc);
  if (last <= 0 || last >= node.length) return;
  bool has_continuation = false;
  for (const CrownNode& child : crown.nodes)
    has_continuation |= child.parent == stem && (child.flags & k_crown_continuation) != 0u;
  if (!has_continuation) return;
  Axis& axis = crown.axes[node.axis];
  const D3 point = axis_point(axis, last);
  const D3 tangent = axis_tangent(axis, last);
  const D3 normal = perpendicular(tangent, axis.normal.back());
  while (!axis.arc.empty() && axis.arc.back() >= last) {
    axis.arc.pop_back();
    axis.point.pop_back();
    axis.tangent.pop_back();
    axis.normal.pop_back();
  }
  axis.arc.push_back(last);
  axis.point.push_back(point);
  axis.tangent.push_back(tangent);
  axis.normal.push_back(normal);
  axis.uniform_steps = false;
  axis.length = last;
  node.r1 = lerp(node.r0, node.r1, last / node.length);
  node.length = last;
  node.tip = point;
  node.flags |= k_crown_stem_relay;
  // Grow the dominant continuation from the stem's tangent before its laterals grow.
  // Returning a short cubic to the old straight branch ray forces an S-shaped correction:
  // integrate the turn instead, and let the distal crown follow its resulting offset.
  u32 chosen = k_crown_none;
  for (u32 i = 0; i < crown.nodes.size(); ++i) {
    const CrownNode& child = crown.nodes[i];
    if (child.parent == stem && child.parent_arc == last &&
        (child.flags & k_crown_continuation) != 0u)
      chosen = i;
  }
  if (chosen == k_crown_none) return;
  CrownNode& child = crown.nodes[chosen];
  const Axis original = crown.axes[child.axis];
  // Shift the continuing section away from the off-take: a concentric contraction
  // makes a bottle-shaped shoulder rather than an asymmetric fork.
  f64 total = 0, fork = 0, lateral = 0;
  D3 off_take{};
  for (const CrownNode& sibling : crown.nodes) {
    if (sibling.parent != stem) continue;
    const f64 share = power(sibling.r0, exponent);
    total += share;
    if (sibling.parent_arc != last) continue;
    fork += share;
    if ((sibling.flags & k_crown_continuation) != 0u) continue;
    const D3 radial = sibling.dir - tangent * dot(sibling.dir, tangent);
    const f64 reach = length_of(radial);
    if (reach > 1e-9) off_take = off_take + radial * (share / reach);
    lateral += share;
  }
  const f64 fork_radius = node.r0 * .95 * power(fork / total, 1.0 / exponent);
  const f64 leader_radius =
      node.r0 * .95 * power(power(child.r0, exponent) / total, 1.0 / exponent);
  const D3 displacement =
      lateral > 0 ? off_take * (-(fork_radius - leader_radius) / lateral) : D3{};
  const f64 span = std::min(.4 * child.length, 8.0 * child.r0);
  Vector<f64> stations = original.arc;
  for (u32 i = 0; i <= 32; ++i)
    stations.push_back(span * static_cast<f64>(i) / 32);
  std::sort(stations.begin(), stations.end());
  stations.erase(std::unique(stations.begin(), stations.end()), stations.end());
  Axis curved;
  curved.length = original.length;
  curved.uniform_steps = false;
  D3 center = original.point[0];
  D3 previous_tangent = tangent;
  D3 frame = perpendicular(tangent, original.normal[0]);
  f64 previous_arc = 0;
  for (const f64 arc : stations) {
    const f64 blend = smoothstep01(arc / span);
    const D3 direction = normalized(tangent * (1.0 - blend) + axis_tangent(original, arc) * blend);
    if (arc <= span)
      center = center + normalized(previous_tangent + direction) * (arc - previous_arc);
    else
      center = center + axis_point(original, arc) - axis_point(original, previous_arc);
    const f64 q = clamp01(arc / span);
    const D3 shifted_tangent = normalized(direction + displacement * (6.0 * q * (1.0 - q) / span));
    frame = perpendicular(shifted_tangent, frame);
    curved.arc.push_back(arc);
    curved.point.push_back(center + displacement * smoothstep01(q));
    curved.tangent.push_back(shifted_tangent);
    curved.normal.push_back(frame);
    previous_arc = arc;
    previous_tangent = direction;
  }
  child.dir = tangent;
  child.tip = curved.point.back();
  crown.axes[child.axis] = std::move(curved);
}
void crown_add_apex(Crown& crown, u32 stem, const CrownSpec& spec) {
  for (const CrownNode& child : crown.nodes)
    if (child.parent == stem && (child.flags & k_crown_continuation) != 0u) return;
  const CrownNode& parent = crown.nodes[stem];
  const Axis& axis = crown.axes[parent.axis];
  CrownNode twig;
  twig.base = axis.point.back();
  twig.dir = axis.tangent.back();
  twig.length = std::min(spec.twig_length, .05 * axis.length);
  twig.tip = twig.base + twig.dir * twig.length;
  twig.r0 = std::min(spec.twig_radius, .25 * parent.r0);
  twig.r1 = .6 * twig.r0;
  twig.parent = twig.carrier = stem;
  twig.parent_arc = axis.length;
  twig.generation = 1;
  twig.weight = 1;
  twig.flags = k_crown_terminal;
  crown.twigs.push_back(crown.nodes.size());
  crown.nodes.push_back(twig);
}

void crown_taper(Crown& crown, f64 exponent) {
  crown.tapered = true;
  const u32 count = crown.nodes.size();
  Vector<u32> first(count, k_crown_none), next(count, k_crown_none);
  Vector<f64> original_base, original_tip;
  Vector<D3> translation(count, D3{});
  original_base.reserve(count);
  original_tip.reserve(count);
  // Intrusive child lists avoid searching the entire crown for each axis.
  for (u32 i = 0; i < count; ++i) {
    const CrownNode& node = crown.nodes[i];
    original_base.push_back(node.r0);
    original_tip.push_back(node.r1);
    if (node.parent != k_crown_none) {
      next[i] = first[node.parent];
      first[node.parent] = i;
    }
  }
  Vector<Outlet> outlets;
  Vector<f64> stations;
  for (u32 i = 0; i < count; ++i) {
    CrownNode& node = crown.nodes[i];
    if (first[i] == k_crown_none) continue;
    outlets.clear();
    f64 sum = 0;
    for (u32 child = first[i]; child != k_crown_none; child = next[child]) {
      const f64 share = power(original_base[child] / original_base[i], exponent);
      const f64 arc = std::clamp(crown.nodes[child].parent_arc, 0.0, node.length);
      const f64 radius = node.r0 * power(share, 1.0 / exponent);
      // A finite collar debit, bounded away from both ends. Equal-station siblings debit together.
      const f64 span =
          arc == node.length
              ? 0.0
              : std::min({2.0 * radius, .12 * node.length, .45 * arc, .45 * (node.length - arc)});
      outlets.push_back({child, arc, share, span});
      sum += share;
    }
    // Existing growth is area-budgeted. Normalization also bounds an authored overfull fork.
    const bool relay = (node.flags & k_crown_stem_relay) != 0u;
    if (relay && sum < 1.0) {
      f64 terminal = 0;
      for (const Outlet& outlet : outlets)
        if ((crown.nodes[outlet.child].flags & k_crown_terminal) != 0u) terminal += outlet.share;
      const f64 scaffold = sum - terminal;
      if (scaffold > 0) {
        for (Outlet& outlet : outlets)
          if ((crown.nodes[outlet.child].flags & k_crown_terminal) == 0u)
            outlet.share *= (1.0 - terminal) / scaffold;
        sum = 1.0;
      }
    }
    const f64 normalization = std::max(1.0, sum);
    const f64 reserve = std::max(0.0, 1.0 - sum);
    const auto radius_at = [&](f64 arc) {
      const f64 u = clamp01(arc / node.length);
      f64 support = reserve * (1.0 - u) * (1.0 - u);
      for (const Outlet& outlet : outlets) {
        const f64 retained =
            outlet.arc == node.length ? 1.0
            : outlet.half_span == 0.0
                ? (arc < outlet.arc ? 1.0 : 0.0)
                : 1.0 - smoothstep01((arc - outlet.arc) / (2.0 * outlet.half_span));
        support += outlet.share / normalization * retained;
      }
      return node.r0 * axial_taper(u) * power(support, 1.0 / exponent);
    };
    node.r1 = radius_at(node.length);
    if (node.axis != k_crown_none) {
      Axis& axis = crown.axes[node.axis];
      stations.clear();
      // Definition samples are independent of the mesh budget, as are the attachment stations.
      for (u32 j = 0; j <= 32; ++j)
        stations.push_back(axis.length * static_cast<f64>(j) / 32);
      for (const Outlet& outlet : outlets) {
        stations.push_back(outlet.arc + 2.0 * outlet.half_span);
        stations.push_back(outlet.arc);
        stations.push_back(outlet.arc + outlet.half_span);
      }
      std::sort(stations.begin(), stations.end());
      axis.radii.clear();
      for (const f64 arc : stations) {
        if (!axis.radii.empty() && axis.radii.back().arc == arc) continue;
        axis.radii.push_back({arc, radius_at(arc)});
      }
    }
    for (const Outlet& outlet : outlets) {
      CrownNode& child = crown.nodes[outlet.child];
      const f64 u = outlet.arc / node.length;
      const D3 center = point_at(crown, node, outlet.arc);
      const D3 offset = child.base + translation[i] - center;
      const f64 old_radius = lerp(original_base[i], original_tip[i], u);
      // Trimming a stem at its last lateral does not make that lateral end-on.
      // Retain its radial seat; only actual continuations share the stem center.
      const D3 parent_tangent =
          node.axis == k_crown_none ? node.dir : axis_tangent(crown.axes[node.axis], outlet.arc);
      const bool end_on =
          outlet.arc == node.length &&
          ((child.flags & k_crown_continuation) != 0u || (child.flags & k_crown_named) == 0u ||
           dot(child.dir, parent_tangent) > .999);
      const D3 seat = end_on ? center : center + offset * (radius_at(outlet.arc) / old_radius);
      const D3 shift = seat - child.base;
      translation[outlet.child] = shift;
      child.base = seat;
      child.tip = child.tip + shift;
      if (child.axis != k_crown_none)
        for (D3& point : crown.axes[child.axis].point)
          point = point + shift;
      child.r0 = node.r0 * axial_taper(u) * power(outlet.share / normalization, 1.0 / exponent);
      child.r1 = original_tip[outlet.child] * child.r0 / original_base[outlet.child];
    }
  }
}

void crown_join_relays(Crown& crown) {
  for (u32 stem = 0; stem < crown.first_recursion; ++stem) {
    const CrownNode& parent = crown.nodes[stem];
    if ((parent.flags & k_crown_stem_relay) == 0u) continue;
    u32 chosen = k_crown_none;
    for (u32 i = 0; i < crown.first_recursion; ++i) {
      const CrownNode& child = crown.nodes[i];
      if (child.parent == stem && child.parent_arc == parent.length &&
          (child.flags & k_crown_continuation) != 0u)
        chosen = i;
    }
    if (chosen == k_crown_none) continue;
    const CrownNode& child = crown.nodes[chosen];
    const Axis& target = crown.axes[child.axis];
    StemRelay join;
    join.stem = stem;
    join.child = chosen;
    join.station = parent.length;
    join.child_arc = std::min(.4 * child.length, 5.0 * parent.r1);
    // This is the grown continuation itself, not a second curve forced back to an old ray.
    // Its laterals already use this centerline, so the shoulder may span their attachments.
    join.mesh_axis = crown.axes[parent.axis];
    Axis& axis = join.mesh_axis;
    D3 normal = axis.normal.back();
    for (u32 i = 1; i <= 24; ++i) {
      const f64 u = static_cast<f64>(i) / 24;
      const f64 arc = join.child_arc * u;
      const D3 point = axis_point(target, arc);
      const D3 tangent = axis_tangent(target, arc);
      normal = perpendicular(tangent, normal);
      axis.arc.push_back(axis.arc.back() + length_of(point - axis.point.back()));
      axis.point.push_back(point);
      axis.tangent.push_back(tangent);
      axis.normal.push_back(normal);
      axis.radii.push_back(
          {axis.arc.back(),
           lerp(parent.r1, axis_radius(target, child.r0, child.r1, arc), smoothstep01(u))});
    }
    axis.length = axis.arc.back();
    axis.uniform_steps = false;
    crown.relays.push_back(std::move(join));
  }
}

Axis crown_sample_stem(const Crown& crown, u32 stem, u32 rows) {
  for (const StemRelay& join : crown.relays) {
    if (join.stem != stem) continue;
    const Axis& source = join.mesh_axis;
    Axis sampled;
    sampled.radii = source.radii;
    // crown_bind_relays matches the visible overlap and insets only its hidden cap.
    sampled.length = source.length;
    sampled.uniform_steps = false;
    const u32 shoulder = std::min(4u, std::max(1u, rows / 3));
    const u32 lower = rows - shoulder;
    D3 normal = source.normal[0];
    for (u32 i = 0; i <= rows; ++i) {
      const f64 arc = i <= lower ? join.station * static_cast<f64>(i) / lower
                                 : join.station + (source.length - join.station) *
                                                      static_cast<f64>(i - lower) / shoulder;
      const D3 tangent = axis_tangent(source, arc);
      normal = perpendicular(tangent, normal);
      sampled.arc.push_back(arc);
      sampled.point.push_back(axis_point(source, arc));
      sampled.tangent.push_back(tangent);
      sampled.normal.push_back(normal);
    }
    return sampled;
  }
  return resample_axis(crown.axes[crown.nodes[stem].axis], rows);
}

Axis crown_sample_branch(const Crown& crown, u32 index, u32 rows, f64 seat_length) {
  Axis sampled = resample_axis(crown.axes[crown.nodes[index].axis], rows, seat_length);
  return sampled;
}
void crown_bind_relays(Crown& crown, const Vector<u32>& first, u32 rows, u32 sides, u32 child_rows,
                       u32 child_sides, assets::MeshData& mesh, Motion& motion) {
  for (const StemRelay& join : crown.relays) {
    const Axis sampled = crown_sample_stem(crown, join.stem, rows);
    const CrownNode& stem = crown.nodes[join.stem];
    const CrownNode& child = crown.nodes[join.child];
    const f64 fixed_length = sampled.arc[rows - 1];
    // Fit the visible overlap to the child's actual section as well as its centerline.
    // Independent bark relief and polygon frames otherwise leave a neck before the cap.
    // Only the closing ring is inset: preceding rings reach the child's outer surface.
    const Axis child_path = crown_sample_branch(crown, join.child, child_rows, join.child_arc);
    Vector<D3> polygon(child_sides);
    for (u32 ring = 1; ring <= rows; ++ring) {
      if (sampled.arc[ring] <= join.station) continue;
      const f64 fraction = (sampled.arc[ring] - join.station) / (sampled.length - join.station);
      const f64 child_arc = join.child_arc * fraction;
      const auto upper = std::upper_bound(child_path.arc.begin(), child_path.arc.end(), child_arc);
      const u32 high = std::min(child_rows, static_cast<u32>(upper - child_path.arc.begin()));
      const u32 low = high - 1;
      const f64 along =
          (child_arc - child_path.arc[low]) / (child_path.arc[high] - child_path.arc[low]);
      D3 center{};
      for (u32 side = 0; side < child_sides; ++side) {
        const D3 a = from_f32(mesh.positions[first[join.child] + low * (child_sides + 1) + side]);
        const D3 b = from_f32(mesh.positions[first[join.child] + high * (child_sides + 1) + side]);
        polygon[side] = a + (b - a) * along;
        center = center + polygon[side] * (1.0 / child_sides);
      }
      const D3 normal = normalized(cross(polygon[1] - polygon[0], polygon[2] - polygon[1]));
      const bool cap = ring == rows;
      const f64 blend = cap ? 1.0 : smoothstep01(2.0 * fraction);
      for (u32 side = 0; side <= sides; ++side) {
        const u32 vertex = first[join.stem] + ring * (sides + 1) + side;
        const D3 original = from_f32(mesh.positions[vertex]);
        D3 radial = original - center;
        radial = radial - normal * dot(radial, normal);
        f64 scale = 1e30;
        for (u32 edge = 0; edge < child_sides; ++edge) {
          const D3 a = polygon[edge], b = polygon[(edge + 1) % child_sides];
          D3 outward = cross(b - a, normal);
          if (dot(outward, a - center) < 0) outward = outward * -1;
          const f64 extent = dot(outward, radial);
          if (extent > 0)
            scale = std::min(scale, (cap ? .9 : 1.0) * dot(outward, a - center) / extent);
        }
        mesh.positions[vertex] =
            to_f32(original * (1.0 - blend) + (center + radial * scale) * blend);
      }
    }
    motion.parts[stem.part].length = static_cast<f32>(fixed_length);
    if (stem.part < motion.structure.size())
      motion.structure[stem.part].tip_radius =
          static_cast<f32>(axis_radius(sampled, stem.r0, stem.r1, fixed_length));
    for (u32 ring = 0; ring <= rows; ++ring) {
      const bool cap = ring == rows;
      for (u32 side = 0; side <= sides; ++side) {
        const u32 vertex = first[join.stem] + ring * (sides + 1) + side;
        motion.vertex_part[vertex] = cap ? child.part : stem.part;
        motion.vertex_weight[vertex] = static_cast<f32>(cap ? join.child_arc / child.length
                                                            : sampled.arc[ring] / fixed_length);
      }
    }
    for (CrownNode& node : crown.nodes)
      if (node.carrier == join.stem && (node.flags & k_crown_part) == 0u)
        node.weight = node.parent_arc / fixed_length;
  }
}
void crown_fit_motion(const Crown& crown, Motion& motion) {
  struct ArcMap {
    Vector<f64> distance;
  };
  Vector<ArcMap> maps(motion.parts.size());
  assets::MeshData empty;
  Emitter seat{empty, motion};
  constexpr u32 samples = 64;
  for (u32 i = 0; i < crown.nodes.size(); ++i) {
    const CrownNode& node = crown.nodes[i];
    if (node.parent == k_crown_none || (node.flags & k_crown_part) == 0u ||
        node.axis == k_crown_none)
      continue;
    crown_seat(crown, i, seat);
    if (!seat.seated || !seat.swept_seat) continue;
    // Fixed definition samples keep the motion table independent of meshing resolution.
    const Axis path = crown_sample_branch(crown, i, samples, 0);
    ArcMap& map = maps[node.part];
    map.distance.resize(samples + 1, 0);
    D3 previous = path.point[0] + tube_sweep_offset(path, seat, 0, false);
    const D3 base = previous;
    for (u32 sample = 1; sample <= samples; ++sample) {
      const D3 point = path.point[sample] + tube_sweep_offset(path, seat, path.arc[sample], false);
      map.distance[sample] = map.distance[sample - 1] + length_of(point - previous);
      previous = point;
    }
    auto& part = motion.parts[node.part];
    part.pivot = to_f32(base);
    part.direction = to_f32(normalized(previous - base));
    part.length = static_cast<f32>(map.distance.back());
  }
  for (u32 vertex = 0; vertex < motion.vertex_part.size(); ++vertex) {
    const ArcMap& map = maps[motion.vertex_part[vertex]];
    if (map.distance.empty()) continue;
    const f64 sample = clamp01(static_cast<f64>(motion.vertex_weight[vertex])) * samples;
    const u32 lower = std::min(samples - 1, static_cast<u32>(sample));
    motion.vertex_weight[vertex] = static_cast<f32>(
        lerp(map.distance[lower], map.distance[lower + 1], sample - lower) / map.distance.back());
  }
}
void crown_seat(const Crown& crown, u32 index, Emitter& out) {
  out.seated = false;
  out.swept_seat = false;
  out.seat_length = 0;
  for (const StemRelay& join : crown.relays)
    if (join.child == index) {
      const CrownNode& child = crown.nodes[index];
      out.seated = true;
      out.swept_seat = true;
      out.seat_origin = child.base;
      out.seat_tangent = crown.axes[crown.nodes[join.stem].axis].tangent.back();
      out.seat_radius = .35 * child.r0;
      out.seat_length = join.child_arc;
      return;
    }
  const CrownNode& child = crown.nodes[index];
  if (!crown.tapered || child.parent == k_crown_none) return;
  const CrownNode& parent = crown.nodes[child.parent];
  const f64 station = std::clamp(child.parent_arc, 0.0, parent.length);
  const f64 parent_radius =
      parent.axis == k_crown_none
          ? lerp(parent.r0, parent.r1, station / parent.length)
          : axis_radius(crown.axes[parent.axis], parent.r0, parent.r1, station);
  // Move the opening below the off-take, wholly into supporting wood, then grow out of it.
  // The debit starts at the off-take, not below it: the parent must still carry both axes there.
  const f64 size = smoothstep01(child.r0 / .08);
  const f64 buried =
      std::max(0.0, station - (0.75 + 4.25 * size) * std::min(child.r0, parent_radius));
  out.seat_origin = point_at(crown, parent, buried);
  out.seat_radius = (.35 + .25 * size) * std::min(child.r0, parent_radius);
  out.seat_length = std::min((2.0 + 6.0 * size) * child.r0, .4 * child.length);
  out.seat_tangent =
      parent.axis == k_crown_none ? parent.dir : axis_tangent(crown.axes[parent.axis], buried);
  out.swept_seat = size > 0;
  out.seated = true;
}
}  // namespace engine::flora::detail
