// The impostor chain (impostor.h; docs/subsystems/flora.md, "The impostor chain"; roadmap R89):
// the branch-group cards and the whole-tree star the solver appends above its finest leaves, and
// the CPU bake that draws each card's and each plane's view of the finest geometry.
//
// Determinism: everything is IEEE + - * / and sqrt in f64 (kit.h's rule), the frames come from a
// Jacobi eigensolver run for a fixed number of sweeps, and the rasterizer decides coverage in
// integer fixed point, so the atlas bytes are a function of the mesh's bits on every toolchain.

#include "kit.h"

#include <domain/texture/texture_build.h>

#include <algorithm>
#include <cmath>

namespace engine::flora {

namespace detail {

namespace {

constexpr u32 k_none = 0xFFFFFFFFu;

// The sub-tree root a spray's leaves gather on `steps` orders further up: never past a part (the
// ladder's own rule, kit.cpp's `group_of`).
u32 walk_up(const Crown& crown, u32 node, u32 steps) {
  u32 g = node;
  for (u32 k = 0; k < steps; ++k) {
    const CrownNode& n = crown.nodes[g];
    if ((n.flags & k_crown_part) != 0u || n.parent == k_crown_none) break;
    g = n.parent;
  }
  return g;
}

// A plane fit to points: the centroid and the covariance's eigenvectors, `e3` the least variance's
// (the plane's normal), `e1` the most's.
struct Frame {
  D3 centre;
  D3 e1{1.0, 0.0, 0.0};
  D3 e2{0.0, 1.0, 0.0};
  D3 e3{0.0, 0.0, 1.0};
};

// The plane of the leaves' **area**, not of their vertices (R91): a spray's cards share their
// centre-line vertices, so a vertex average weighs the twig's line over the leaves either side of
// it, and a sub-tree of a few sprays fit that way could take the plane through the twigs edge-on
// to the leaves and bake as a sliver. Each triangle counts by its area, with its exact second
// moment about the centroid — A / 12 (sum of d_i d_i^T + s s^T), s the sum of its corners' offsets
// — so the plane is the leaves' own. Cyclic Jacobi on the symmetric 3x3, eight sweeps: square roots
// and divisions only. A group of zero area falls back to its vertices' average.
Frame fit_frame(const assets::MeshData& mesh, std::span<const SprayRange> sprays,
                std::span<const u32> members) {
  Frame f;
  f64 area = 0.0;
  D3 weighted{0.0, 0.0, 0.0};
  f64 count = 0.0;
  D3 plain{0.0, 0.0, 0.0};
  for (const u32 s : members) {
    const SprayRange& r = sprays[s];
    for (u32 v = r.first_vertex; v < r.first_vertex + r.vertex_count; ++v) {
      plain = plain + from_f32(mesh.positions[v]);
      count += 1.0;
    }
    for (u32 i = r.first_index; i + 2 < r.first_index + r.index_count; i += 3) {
      const D3 a = from_f32(mesh.positions[mesh.indices[i]]);
      const D3 b = from_f32(mesh.positions[mesh.indices[i + 1]]);
      const D3 c = from_f32(mesh.positions[mesh.indices[i + 2]]);
      const f64 t = 0.5 * length_of(cross(b - a, c - a));
      weighted = weighted + (a + b + c) * (t / 3.0);
      area += t;
    }
  }
  if (count == 0.0) return f;
  const bool by_area = area > 0.0;
  f.centre = by_area ? weighted * (1.0 / area) : plain * (1.0 / count);
  f64 a[3][3] = {};
  for (const u32 s : members) {
    const SprayRange& r = sprays[s];
    if (!by_area) {
      for (u32 v = r.first_vertex; v < r.first_vertex + r.vertex_count; ++v) {
        const D3 d = from_f32(mesh.positions[v]) - f.centre;
        const f64 c[3] = {d.x, d.y, d.z};
        for (u32 i = 0; i < 3; ++i)
          for (u32 j = 0; j < 3; ++j)
            a[i][j] += c[i] * c[j];
      }
      continue;
    }
    for (u32 i = r.first_index; i + 2 < r.first_index + r.index_count; i += 3) {
      const D3 p0 = from_f32(mesh.positions[mesh.indices[i]]) - f.centre;
      const D3 p1 = from_f32(mesh.positions[mesh.indices[i + 1]]) - f.centre;
      const D3 p2 = from_f32(mesh.positions[mesh.indices[i + 2]]) - f.centre;
      const f64 t = 0.5 * length_of(cross(p1 - p0, p2 - p0)) / 12.0;
      const D3 sum = p0 + p1 + p2;
      const f64 c[4][3] = {
          {p0.x, p0.y, p0.z}, {p1.x, p1.y, p1.z}, {p2.x, p2.y, p2.z}, {sum.x, sum.y, sum.z}};
      for (u32 k = 0; k < 4; ++k)
        for (u32 x = 0; x < 3; ++x)
          for (u32 y = 0; y < 3; ++y)
            a[x][y] += t * c[k][x] * c[k][y];
    }
  }
  f64 vec[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
  for (u32 sweep = 0; sweep < 8; ++sweep) {
    for (u32 p = 0; p < 2; ++p) {
      for (u32 q = p + 1; q < 3; ++q) {
        const f64 apq = a[p][q];
        const f64 scale =
            (a[p][p] < 0.0 ? -a[p][p] : a[p][p]) + (a[q][q] < 0.0 ? -a[q][q] : a[q][q]);
        if (!(apq != 0.0) || (apq < 0.0 ? -apq : apq) <= 1e-300 + 1e-18 * scale) continue;
        const f64 theta = (a[q][q] - a[p][p]) / (2.0 * apq);
        const f64 magnitude = theta < 0.0 ? -theta : theta;
        f64 t = 1.0 / (magnitude + std::sqrt(theta * theta + 1.0));
        if (theta < 0.0) t = -t;
        const f64 c = 1.0 / std::sqrt(t * t + 1.0);
        const f64 s = t * c;
        for (u32 k = 0; k < 3; ++k) {
          const f64 akp = a[k][p];
          const f64 akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (u32 k = 0; k < 3; ++k) {
          const f64 apk = a[p][k];
          const f64 aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (u32 k = 0; k < 3; ++k) {
          const f64 vkp = vec[k][p];
          const f64 vkq = vec[k][q];
          vec[k][p] = c * vkp - s * vkq;
          vec[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  // Order by eigenvalue, largest first; ties keep the axis order.
  u32 order[3] = {0, 1, 2};
  for (u32 i = 0; i < 3; ++i)
    for (u32 j = i + 1; j < 3; ++j)
      if (a[order[j]][order[j]] > a[order[i]][order[i]]) {
        const u32 swap = order[i];
        order[i] = order[j];
        order[j] = swap;
      }
  const auto column = [&](u32 k) { return normalized(D3{vec[0][k], vec[1][k], vec[2][k]}); };
  f.e1 = column(order[0]);
  f.e3 = column(order[2]);
  // Canonical signs: the normal faces the sky (a level card is seen from above as the leaves'
  // upper faces), and the frame is right-handed.
  if (f.e3.y < 0.0 || (f.e3.y == 0.0 && f.e3.x < 0.0)) f.e3 = f.e3 * -1.0;
  f.e2 = normalized(cross(f.e3, f.e1));
  f.e1 = normalized(cross(f.e2, f.e3));
  return f;
}

u64 mix(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// The UV rectangle of tile `t` in a `columns` x `rows` atlas of `px`-texel tiles, one texel in from
// each edge so the filter never reads a neighbour's texels at level 0.
struct TileUv {
  f64 u0, v0, u1, v1;
};
TileUv tile_uv(u32 t, u32 columns, u32 rows, u32 px) {
  const f64 col = static_cast<f64>(t % columns);
  const f64 row = static_cast<f64>(t / columns);
  const f64 inset = 1.0 / static_cast<f64>(px);
  return TileUv{(col + inset) / static_cast<f64>(columns), (row + inset) / static_cast<f64>(rows),
                (col + 1.0 - inset) / static_cast<f64>(columns),
                (row + 1.0 - inset) / static_cast<f64>(rows)};
}

// A view of the rectangle [u0, u1] x [v0, v1] whose tile's inner (px - 2) texels hold it: one
// texel of margin round it, which the inset UVs leave out.
BakeView view_of(D3 origin, D3 u, D3 v, D3 n, f64 u0, f64 u1, f64 v0, f64 v1, u32 px) {
  const f64 inner = static_cast<f64>(px) - 2.0;
  const f64 mu = (u1 - u0) / inner;
  const f64 mv = (v1 - v0) / inner;
  BakeView view;
  view.origin = to_f32(origin);
  view.u_axis = to_f32(u);
  view.v_axis = to_f32(v);
  view.normal = to_f32(n);
  view.u_min = static_cast<f32>(u0 - mu);
  view.u_max = static_cast<f32>(u1 + mu);
  view.v_min = static_cast<f32>(v0 - mv);
  view.v_max = static_cast<f32>(v1 + mv);
  return view;
}

// The card's own rectangle, with no margin: what the coverage bake draws it as.
BakeView exact_view(D3 origin, D3 u, D3 v, D3 n, f64 u0, f64 u1, f64 v0, f64 v1) {
  BakeView view;
  view.origin = to_f32(origin);
  view.u_axis = to_f32(u);
  view.v_axis = to_f32(v);
  view.normal = to_f32(n);
  view.u_min = static_cast<f32>(u0);
  view.u_max = static_cast<f32>(u1);
  view.v_min = static_cast<f32>(v0);
  view.v_max = static_cast<f32>(v1);
  return view;
}

// One two-sided quad of the level: corners at the frame's rectangle, UVs the tile's, the normal
// the plane's, riding `part` at `weight`. Its triangles go to the level's indices.
void emit_card(Emitter& out, D3 origin, D3 u, D3 v, D3 n, f64 u0, f64 u1, f64 v0, f64 v1,
               const TileUv& uv, Vector<u32>& indices) {
  const u32 a = out.vertex(origin + u * u0 + v * v0, n, uv.u0, uv.v0);
  const u32 b = out.vertex(origin + u * u1 + v * v0, n, uv.u1, uv.v0);
  const u32 c = out.vertex(origin + u * u1 + v * v1, n, uv.u1, uv.v1);
  const u32 d = out.vertex(origin + u * u0 + v * v1, n, uv.u0, uv.v1);
  for (const u32 i : {a, b, c, a, c, d})
    indices.push_back(i);
}

}  // namespace

void crown_levels(const Crown& crown, const CrownPlan& plan, const Vector<SprayRange>& sprays,
                  const ImpostorSpec& spec, Emitter& out, TreeLevels& levels) {
  if (spec.card_px < 8 || sprays.empty()) return;
  const assets::MeshData& mesh = out.mesh;
  u32 previous_groups = sprays.size();
  f64 previous_error = 0.0;
  Vector<u32> slot(crown.nodes.size(), k_none);
  // The first level stands in for leaves: its feature is a leaf's size, the square root of a leaf
  // card's area (two triangles).
  f64 leaf_area = 0.0;
  u32 leaf_triangles = 0;
  for (const SprayRange& r : sprays) {
    for (u32 i = r.first_index; i + 2 < r.first_index + r.index_count; i += 3) {
      const D3 a = from_f32(mesh.positions[mesh.indices[i]]);
      const D3 b = from_f32(mesh.positions[mesh.indices[i + 1]]);
      const D3 c = from_f32(mesh.positions[mesh.indices[i + 2]]);
      leaf_area += 0.5 * length_of(cross(b - a, c - a));
      ++leaf_triangles;
    }
  }
  f64 feature = leaf_triangles > 0 ? std::sqrt(2.0 * leaf_area / leaf_triangles) : 0.0;
  Vector<u32> previous_first_spray;  // each card of the level below: its first spray
  for (u32 step = 1; step <= spec.most_levels; ++step) {
    // Each spray's group, numbered in the order its first spray comes.
    Vector<u32> group_node;
    Vector<u32> group_of_spray(sprays.size());
    for (u32 s = 0; s < sprays.size(); ++s) {
      const u32 g = walk_up(crown, sprays[s].node, step);
      if (slot[g] == k_none) {
        slot[g] = group_node.size();
        group_node.push_back(g);
      }
      group_of_spray[s] = slot[g];
    }
    for (const u32 g : group_node)
      slot[g] = k_none;
    const u32 groups = group_node.size();
    if (groups >= previous_groups) break;  // nothing merged further: every root is a part
    // How the level nests: the finest leaves' cards at the first step, and every card of the level
    // below its card here (a sub-tree's root k + 1 orders up is its k-th root's one order up, so
    // all of a card's sprays agree).
    if (step == 1) {
      levels.leaf_element.assign((levels.leaf_index_end - levels.leaf_first_index) / 3u, 0u);
      for (u32 s = 0; s < sprays.size(); ++s) {
        const SprayRange& r = sprays[s];
        for (u32 i = r.first_index; i + 2 < r.first_index + r.index_count; i += 3) {
          if (i < levels.leaf_first_index || i >= levels.leaf_index_end) continue;
          levels.leaf_element[(i - levels.leaf_first_index) / 3u] = group_of_spray[s];
        }
      }
    } else {
      TreeLevel& below = levels.levels.back();
      below.parent_of_element.resize(below.elements);
      for (u32 g = 0; g < below.elements; ++g)
        below.parent_of_element[g] = group_of_spray[previous_first_spray[g]];
    }
    // The sprays of each group, contiguous (a counting sort keeps their order).
    Vector<u32> offsets(groups + 1, 0u);
    for (const u32 g : group_of_spray)
      ++offsets[g + 1];
    for (u32 g = 0; g < groups; ++g)
      offsets[g + 1] += offsets[g];
    Vector<u32> members(sprays.size());
    Vector<u32> cursor(groups, 0u);
    for (u32 g = 0; g < groups; ++g)
      cursor[g] = offsets[g];
    for (u32 s = 0; s < sprays.size(); ++s)
      members[cursor[group_of_spray[s]]++] = s;

    TreeLevel level;
    level.kind = ImpostorKind::branch_group;
    level.merge = plan.merge + step;
    const u32 planes = ImpostorSpec::k_card_planes;
    level.groups = groups * planes;
    level.tile_px = spec.card_px;
    const u32 tiles = spec.representatives < groups ? spec.representatives : groups;
    level.columns = 1;
    while (level.columns * level.columns < tiles * planes)
      ++level.columns;
    level.rows = (tiles * planes + level.columns - 1) / level.columns;
    f64 parallax = 0.0;
    f64 texel = 0.0;
    f64 card_area = 0.0;
    const f64 inner = static_cast<f64>(spec.card_px) - 2.0;
    for (u32 g = 0; g < groups; ++g) {
      const std::span<const u32> mine(members.data() + offsets[g], offsets[g + 1] - offsets[g]);
      const Frame fitted = fit_frame(mesh, sprays, mine);
      for (u32 plane = 0; plane < planes; ++plane) {
        const Frame f = plane == 0   ? fitted
                        : plane == 1 ? Frame{fitted.centre, fitted.e2, fitted.e3, fitted.e1}
                                     : Frame{fitted.centre, fitted.e3, fitted.e1, fitted.e2};
        f64 lo1 = 1e300, hi1 = -1e300, lo2 = 1e300, hi2 = -1e300;
        for (const u32 s : mine) {
          const SprayRange& r = sprays[s];
          for (u32 v = r.first_vertex; v < r.first_vertex + r.vertex_count; ++v) {
            const D3 d = from_f32(mesh.positions[v]) - f.centre;
            const f64 x = dot(d, f.e1);
            const f64 y = dot(d, f.e2);
            const f64 z = dot(d, f.e3);
            lo1 = x < lo1 ? x : lo1;
            hi1 = x > hi1 ? x : hi1;
            lo2 = y < lo2 ? y : lo2;
            hi2 = y > hi2 ? y : hi2;
            const f64 off = z < 0.0 ? -z : z;
            parallax = off > parallax ? off : parallax;
          }
        }
        const f64 extent = (hi1 - lo1) > (hi2 - lo2) ? (hi1 - lo1) : (hi2 - lo2);
        const f64 half_texel = 0.5 * extent / inner;
        texel = half_texel > texel ? half_texel : texel;
        // The tile: one of the representatives, by the sub-tree's own hash. The representatives are
        // spread evenly over the groups' order and baked from their own sprays.
        const u32 representative = groups == tiles
                                       ? g
                                       : static_cast<u32>(mix(static_cast<u64>(group_node[g]) ^
                                                              (static_cast<u64>(step) << 40)) %
                                                          static_cast<u64>(tiles));
        const u32 tile = representative * planes + plane;
        const CrownNode& root = crown.nodes[group_node[g]];
        out.part = crown.nodes[root.carrier].part;
        out.weight = (root.flags & k_crown_part) != 0u ? 0.7 : root.weight;
        emit_card(out, f.centre, f.e1, f.e2, f.e3, lo1, hi1, lo2, hi2,
                  tile_uv(tile, level.columns, level.rows, spec.card_px), level.indices);
        level.element_of_triangle.push_back(g);
        level.element_of_triangle.push_back(g);
        card_area += (hi1 - lo1) * (hi2 - lo2);
        // Representative i is group floor((2 i + 1) G / 2K): bake it into tile i.
        for (u32 i = 0; i < tiles; ++i) {
          if ((2u * static_cast<u64>(i) + 1u) * groups / (2u * static_cast<u64>(tiles)) != g)
            continue;
          BakeJob job;
          job.view = view_of(f.centre, f.e1, f.e2, f.e3, lo1, hi1, lo2, hi2, spec.card_px);
          job.tile = i * planes + plane;
          job.group = i;
          job.card = exact_view(f.centre, f.e1, f.e2, f.e3, lo1, hi1, lo2, hi2);
          job.card_tile = tile;
          for (const u32 s : mine)
            job.ranges.push_back(BakeRange{sprays[s].first_index, sprays[s].index_count});
          level.jobs.push_back(std::move(job));
        }
      }
    }
    level.parallax_m = static_cast<f32>(parallax);
    level.texel_m = static_cast<f32>(texel);
    f64 e = parallax > texel ? parallax : texel;
    if (e <= previous_error) e = previous_error * (1.0 + 1.0 / 1024.0);
    level.error = static_cast<f32>(e);
    if (static_cast<f64>(level.error) <= previous_error)
      level.error = static_cast<f32>(previous_error * (1.0 + 1.0 / 1024.0));
    previous_error = static_cast<f64>(level.error);
    previous_groups = groups;
    level.elements = groups;
    level.feature_m = static_cast<f32>(feature);
    // The next level stands in for this one's cards: its feature is a card's size.
    feature = std::sqrt(card_area / static_cast<f64>(groups));
    previous_first_spray.resize(groups);
    for (u32 g = 0; g < groups; ++g)
      previous_first_spray[g] = members[offsets[g]];
    levels.levels.push_back(std::move(level));
  }
}

void tree_star(const ImpostorSpec& spec, u32 part, Emitter& out, TreeLevels& levels) {
  if (spec.tree_px < 8 || levels.leaf_index_end == 0) return;
  const assets::MeshData& mesh = out.mesh;
  D3 lo{1e300, 1e300, 1e300};
  D3 hi{-1e300, -1e300, -1e300};
  for (u32 i = 0; i < levels.leaf_index_end; ++i) {
    const D3 p = from_f32(mesh.positions[mesh.indices[i]]);
    lo = D3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
    hi = D3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
  }
  // The vertical planes stand on the trunk's axis, so the trunk is where it is in every one of
  // them (through the box's centre instead, an asymmetric crown drew a trunk a plane); the level
  // plane at the box's mid-height.
  D3 centre = (lo + hi) * 0.5;
  if (part < out.motion.parts.size()) {
    const D3 pivot = from_f32(out.motion.parts[part].pivot);
    centre.x = pivot.x;
    centre.z = pivot.z;
  }
  const f64 s = std::sqrt(0.5);
  // Four vertical planes 45 degrees apart: normal n, u = up x n, v = up, so u x v = n on every one.
  // **No level plane** (R91): seen from near the crown's mid-height it was edge-on, a thin dark
  // line across the tree, and a plane the masks could hide by the view's angle would make the
  // coverage a function of the view on every path (renderer.md, "Impostors are masked clusters");
  // from high above the four read as a cross, which is the octahedral frames' case (R90).
  const D3 normals[4] = {{1.0, 0.0, 0.0}, {s, 0.0, s}, {0.0, 0.0, 1.0}, {-s, 0.0, s}};
  TreeLevel level;
  level.kind = ImpostorKind::tree;
  level.groups = 0;
  level.tile_px = spec.tree_px;
  level.columns = 1;
  while (level.columns * level.columns < 4 * ImpostorSpec::k_tree_slices)
    ++level.columns;
  level.rows = (4 * ImpostorSpec::k_tree_slices + level.columns - 1) / level.columns;
  // What the star stands in for: the last branch-group level's cards, or with none the leaves, by
  // the square root of an element's area.
  {
    const bool cards = !levels.levels.empty();
    const Vector<u32>& source = cards ? levels.levels.back().indices : mesh.indices;
    const u32 first = cards ? 0u : levels.leaf_first_index;
    const u32 end = cards ? source.size() : levels.leaf_index_end;
    f64 area = 0.0;
    u32 triangles = 0;
    for (u32 i = first; i + 2 < end; i += 3) {
      const D3 a = from_f32(mesh.positions[source[i]]);
      const D3 b = from_f32(mesh.positions[source[i + 1]]);
      const D3 c = from_f32(mesh.positions[source[i + 2]]);
      area += 0.5 * length_of(cross(b - a, c - a));
      ++triangles;
    }
    level.feature_m = triangles > 0 ? static_cast<f32>(std::sqrt(2.0 * area / triangles)) : 0.0f;
  }
  // Everything below nests in the one star: the last branch-group level's cards, or the leaves.
  if (!levels.levels.empty()) {
    TreeLevel& below = levels.levels.back();
    below.parent_of_element.assign(below.elements, 0u);
  } else {
    levels.leaf_element.assign((levels.leaf_index_end - levels.leaf_first_index) / 3u, 0u);
  }
  f64 parallax = 0.0;
  f64 texel = 0.0;
  const f64 inner = static_cast<f64>(spec.tree_px) - 2.0;
  out.part = part;
  out.weight = 0.0;
  for (u32 k = 0; k < 4; ++k) {
    const D3 n = normals[k];
    const D3 v = k_up;
    const D3 u = normalized(cross(v, n));
    f64 near = 1e300, far = -1e300;
    for (u32 i = 0; i < levels.leaf_index_end; ++i) {
      const f64 z = dot(from_f32(mesh.positions[mesh.indices[i]]) - centre, n);
      near = std::min(near, z);
      far = std::max(far, z);
    }
    for (u32 slice = 0; slice < ImpostorSpec::k_tree_slices; ++slice) {
      BakeJob job;
      job.tile = k * ImpostorSpec::k_tree_slices + slice;
      job.card_tile = job.tile;
      const f64 depth = near + (far - near) * (slice + .5) / ImpostorSpec::k_tree_slices;
      const D3 origin = centre + n * depth;
      for (u32 i = 0; i + 2 < levels.leaf_index_end; i += 3) {
        const D3 centroid = (from_f32(mesh.positions[mesh.indices[i]]) +
                             from_f32(mesh.positions[mesh.indices[i + 1]]) +
                             from_f32(mesh.positions[mesh.indices[i + 2]])) *
                            (1.0 / 3.0);
        const f64 f = (dot(centroid - centre, n) - near) / std::max(1e-9, far - near);
        const u32 bin = std::min(ImpostorSpec::k_tree_slices - 1,
                                 static_cast<u32>(std::max(0.0, f) * ImpostorSpec::k_tree_slices));
        if (bin != slice) continue;
        if (!job.ranges.empty() &&
            job.ranges.back().first_index + job.ranges.back().index_count == i)
          job.ranges.back().index_count += 3;
        else
          job.ranges.push_back({i, 3});
      }
      if (job.ranges.empty()) continue;
      f64 lo1 = 1e300, hi1 = -1e300, lo2 = 1e300, hi2 = -1e300;
      for (const BakeRange& range : job.ranges) {
        for (u32 i = range.first_index; i < range.first_index + range.index_count; ++i) {
          const D3 d = from_f32(mesh.positions[mesh.indices[i]]) - origin;
          const f64 x = dot(d, u);
          const f64 y = dot(d, v);
          const f64 z = dot(d, n);
          lo1 = x < lo1 ? x : lo1;
          hi1 = x > hi1 ? x : hi1;
          lo2 = y < lo2 ? y : lo2;
          hi2 = y > hi2 ? y : hi2;
          // A pixel of this plane shows the nearest geometry behind it, which can be this far off
          // it.
          const f64 off = z < 0.0 ? -z : z;
          parallax = off > parallax ? off : parallax;
        }
      }
      const f64 extent = (hi1 - lo1) > (hi2 - lo2) ? (hi1 - lo1) : (hi2 - lo2);
      const f64 half_texel = 0.5 * extent / inner;
      texel = half_texel > texel ? half_texel : texel;
      emit_card(out, origin, u, v, n, lo1, hi1, lo2, hi2,
                tile_uv(job.tile, level.columns, level.rows, spec.tree_px), level.indices);
      ++level.groups;
      for (u32 t = 0; t < 2; ++t)
        level.element_of_triangle.push_back(0u);
      job.view = view_of(origin, u, v, n, lo1, hi1, lo2, hi2, spec.tree_px);
      job.card = exact_view(origin, u, v, n, lo1, hi1, lo2, hi2);
      level.jobs.push_back(std::move(job));
    }
  }
  level.parallax_m = static_cast<f32>(parallax);
  level.texel_m = static_cast<f32>(texel);
  f64 e = parallax > texel ? parallax : texel;
  const f64 below = levels.levels.empty() ? 0.0 : static_cast<f64>(levels.levels.back().error);
  if (e <= below) e = below * (1.0 + 1.0 / 1024.0);
  level.error = static_cast<f32>(e);
  level.elements = 1;
  levels.levels.push_back(std::move(level));
}

}  // namespace detail

// ---- the bake -----------------------------------------------------------------------------------

namespace {

using detail::D3;
using detail::from_f32;

constexpr u32 k_samples = 4;  // a pixel's samples a side
constexpr i64 k_sub = 256;    // fixed-point steps a pixel
constexpr u32 k_no_triangle = 0xFFFFFFFFu;

struct Sample {
  f64 depth = -1e300;  // along the view's normal: larger is nearer the viewer
  u32 triangle = k_no_triangle;
  u32 colour = 0;  // sampled sRGB; uses the former padding, still 16 bytes a sample
};

u32 pack_rgb(const Vec4& linear);

bool surface_sample(const BakeLook& look, bool leaf, f64 u, f64 v, u32& rgb) {
  const Vec4 factor = leaf ? look.leaf : look.bark;
  rgb = pack_rgb(factor);
  const image::Image* image = leaf ? look.leaf_image : look.bark_image;
  if (image == nullptr || image->width == 0 || image->height == 0 || image->channels != 4) {
    if (!leaf || !look.leaf_outline) return true;
    const f64 x = (u - std::floor(u)) * 2.0 - 1.0;
    const f64 y = (v < 0 ? -v : v) * 2.0 - 1.0;
    return x * x + y * y <= 1.0;
  }
  if (!leaf) {
    u *= static_cast<f64>(look.bark_scale.x);
    v *= static_cast<f64>(look.bark_scale.y);
  }
  const f64 x = (u - std::floor(u)) * image->width - .5;
  const f64 y = (v - std::floor(v)) * image->height - .5;
  const i64 ix = static_cast<i64>(std::floor(x));
  const i64 iy = static_cast<i64>(std::floor(y));
  const f64 fx = x - std::floor(x), fy = y - std::floor(y);
  f64 rgba[4]{};
  for (u32 j = 0; j < 2; ++j) {
    for (u32 i = 0; i < 2; ++i) {
      const u32 sx = static_cast<u32>((ix + i + image->width) % image->width);
      const u32 sy = static_cast<u32>((iy + j + image->height) % image->height);
      const u32 at = (sy * image->width + sx) * 4;
      const f64 weight = (i == 0 ? 1 - fx : fx) * (j == 0 ? 1 - fy : fy);
      for (u32 c = 0; c < 3; ++c)
        rgba[c] += weight * static_cast<f64>(texture::srgb8_to_linear(image->pixels[at + c]));
      rgba[3] += weight * image->pixels[at + 3] / 255.0;
    }
  }
  if (leaf && rgba[3] * static_cast<f64>(factor.w) < static_cast<f64>(look.leaf_cutoff))
    return false;
  rgb = pack_rgb({static_cast<f32>(rgba[0]) * factor.x, static_cast<f32>(rgba[1]) * factor.y,
                  static_cast<f32>(rgba[2]) * factor.z, 1});
  return true;
}

}  // namespace

void bake_level(const assets::MeshData& mesh, const TreeLevels& levels, const TreeLevel& level,
                const BakeLook& look, BakedAtlas& out, Vector<f32>* depth_out) {
  const u32 px = level.tile_px;
  out.width = level.columns * px;
  out.height = level.rows * px;
  out.colour.assign(static_cast<u32>(out.width) * out.height * 4u, u8{0});
  out.normal.assign(static_cast<u32>(out.width) * out.height * 4u, u8{0});
  if (depth_out != nullptr) depth_out->assign(out.width * out.height, 0.0f);
  const u32 side = px * k_samples;
  Vector<Sample> samples;
  for (const BakeJob& job : level.jobs) {
    samples.assign(side * side, Sample{});
    const D3 o = from_f32(job.view.origin);
    const D3 u = from_f32(job.view.u_axis);
    const D3 v = from_f32(job.view.v_axis);
    const D3 n = from_f32(job.view.normal);
    const f64 su = static_cast<f64>(px) /
                   (static_cast<f64>(job.view.u_max) - static_cast<f64>(job.view.u_min));
    const f64 sv = static_cast<f64>(px) /
                   (static_cast<f64>(job.view.v_max) - static_cast<f64>(job.view.v_min));
    const f64 u_min = static_cast<f64>(job.view.u_min);
    const f64 v_min = static_cast<f64>(job.view.v_min);
    for (const BakeRange& range : job.ranges) {
      for (u32 i = range.first_index; i + 2 < range.first_index + range.index_count; i += 3) {
        i64 x[3], y[3];
        f64 z[3];
        bool bad = false;
        for (u32 c = 0; c < 3; ++c) {
          const D3 d = from_f32(mesh.positions[mesh.indices[i + c]]) - o;
          const f64 fx = (detail::dot(d, u) - u_min) * su * static_cast<f64>(k_sub);
          const f64 fy = (detail::dot(d, v) - v_min) * sv * static_cast<f64>(k_sub);
          if (!(fx > -1e9 && fx < 1e9 && fy > -1e9 && fy < 1e9)) bad = true;
          x[c] = static_cast<i64>(std::floor(fx + 0.5));
          y[c] = static_cast<i64>(std::floor(fy + 0.5));
          z[c] = detail::dot(d, n);
        }
        if (bad) continue;
        i64 area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
        if (area == 0) continue;
        u32 i1 = 1, i2 = 2;
        if (area < 0) {
          i1 = 2;
          i2 = 1;
          area = -area;
        }
        const i64 ax = x[0], ay = y[0], bx = x[i1], by = y[i1], cx = x[i2], cy = y[i2];
        const f64 za = z[0], zb = z[i1], zc = z[i2];
        const Vec2 ta = mesh.uvs[mesh.indices[i]];
        const Vec2 tb = mesh.uvs[mesh.indices[i + i1]];
        const Vec2 tc = mesh.uvs[mesh.indices[i + i2]];
        const bool leaf = i >= levels.leaf_first_index && i < levels.leaf_index_end;
        i64 lo_x = ax < bx ? ax : bx;
        lo_x = cx < lo_x ? cx : lo_x;
        i64 hi_x = ax > bx ? ax : bx;
        hi_x = cx > hi_x ? cx : hi_x;
        i64 lo_y = ay < by ? ay : by;
        lo_y = cy < lo_y ? cy : lo_y;
        i64 hi_y = ay > by ? ay : by;
        hi_y = cy > hi_y ? cy : hi_y;
        // Samples sit at (s + 0.5) / k_samples of a pixel: index s at s * step + step / 2.
        constexpr i64 step = k_sub / static_cast<i64>(k_samples);
        i64 s0x = (lo_x - step / 2 + step - 1) / step;
        i64 s1x = (hi_x - step / 2) / step;
        i64 s0y = (lo_y - step / 2 + step - 1) / step;
        i64 s1y = (hi_y - step / 2) / step;
        if (lo_x - step / 2 < 0) s0x = 0;
        if (lo_y - step / 2 < 0) s0y = 0;
        if (s1x >= static_cast<i64>(side)) s1x = static_cast<i64>(side) - 1;
        if (s1y >= static_cast<i64>(side)) s1y = static_cast<i64>(side) - 1;
        // Top-left rule, counter-clockwise in (x, y): an edge is top or left when it runs down
        // (dy < 0) or along the top (dy == 0, dx > 0); a sample on it is inside only then.
        const auto bias = [](i64 ex, i64 ey) { return (ey < 0 || (ey == 0 && ex > 0)) ? 0 : -1; };
        const i64 b0 = bias(cx - bx, cy - by);
        const i64 b1 = bias(ax - cx, ay - cy);
        const i64 b2 = bias(bx - ax, by - ay);
        for (i64 sy = s0y; sy <= s1y; ++sy) {
          const i64 py = sy * step + step / 2;
          for (i64 sx = s0x; sx <= s1x; ++sx) {
            const i64 qx = sx * step + step / 2;
            const i64 w0 = (cx - bx) * (py - by) - (cy - by) * (qx - bx);
            const i64 w1 = (ax - cx) * (py - cy) - (ay - cy) * (qx - cx);
            const i64 w2 = (bx - ax) * (py - ay) - (by - ay) * (qx - ax);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            const f64 l0 = static_cast<f64>(w0) / static_cast<f64>(area);
            const f64 l1 = static_cast<f64>(w1) / static_cast<f64>(area);
            const f64 l2 = 1.0 - l0 - l1;
            const f64 depth = za * l0 + zb * l1 + zc * l2;
            Sample& sample = samples[static_cast<u32>(sy) * side + static_cast<u32>(sx)];
            if (depth < sample.depth || (depth == sample.depth && i < sample.triangle)) continue;
            u32 colour = pack_rgb(leaf ? look.leaf : look.bark);
            {
              const f64 tu = static_cast<f64>(ta.x) * l0 + static_cast<f64>(tb.x) * l1 +
                             static_cast<f64>(tc.x) * l2;
              const f64 tv = static_cast<f64>(ta.y) * l0 + static_cast<f64>(tb.y) * l1 +
                             static_cast<f64>(tc.y) * l2;
              if (!surface_sample(look, leaf, tu, tv, colour)) continue;
            }
            sample.depth = depth;
            sample.triangle = i;
            sample.colour = colour;
          }
        }
      }
    }
    // Resolve: a pixel's coverage is its samples', its colour and normal their mean.
    const u32 tx = (job.tile % level.columns) * px;
    const u32 ty = (job.tile / level.columns) * px;
    for (u32 py = 0; py < px; ++py) {
      for (u32 pxi = 0; pxi < px; ++pxi) {
        u32 covered = 0;
        f64 rgb[3] = {0.0, 0.0, 0.0};
        D3 normal{0.0, 0.0, 0.0};
        f64 depth_sum = 0;
        for (u32 sy = 0; sy < k_samples; ++sy) {
          for (u32 sx = 0; sx < k_samples; ++sx) {
            const Sample& s = samples[(py * k_samples + sy) * side + pxi * k_samples + sx];
            if (s.triangle == k_no_triangle) continue;
            ++covered;
            if (depth_out != nullptr) depth_sum += s.depth;
            const bool leaf =
                s.triangle >= levels.leaf_first_index && s.triangle < levels.leaf_index_end;
            Vec4 c = leaf ? look.leaf : look.bark;
            if ((leaf ? look.leaf_image : look.bark_image) != nullptr) {
              c.x = texture::srgb8_to_linear(static_cast<u8>(s.colour));
              c.y = texture::srgb8_to_linear(static_cast<u8>(s.colour >> 8));
              c.z = texture::srgb8_to_linear(static_cast<u8>(s.colour >> 16));
            }
            rgb[0] += static_cast<f64>(c.x);
            rgb[1] += static_cast<f64>(c.y);
            rgb[2] += static_cast<f64>(c.z);
            const D3 a = from_f32(mesh.positions[mesh.indices[s.triangle]]);
            const D3 b = from_f32(mesh.positions[mesh.indices[s.triangle + 1]]);
            const D3 c3 = from_f32(mesh.positions[mesh.indices[s.triangle + 2]]);
            D3 face = detail::normalized(detail::cross(b - a, c3 - a));
            if (detail::dot(face, n) < 0.0) face = face * -1.0;
            normal = normal + D3{detail::dot(face, u), detail::dot(face, v), detail::dot(face, n)};
          }
        }
        if (covered == 0) continue;
        const u32 at = ((ty + py) * out.width + tx + pxi) * 4u;
        const f64 inv = 1.0 / static_cast<f64>(covered);
        if (depth_out != nullptr) (*depth_out)[at / 4] = static_cast<f32>(depth_sum * inv);
        for (u32 k = 0; k < 3; ++k)
          out.colour[at + k] = texture::linear_to_srgb8(static_cast<f32>(rgb[k] * inv));
        const u8 alpha = static_cast<u8>((covered * 255u + k_samples * k_samples / 2u) /
                                         (k_samples * k_samples));
        out.colour[at + 3] = alpha;
        const D3 m = detail::normalized(normal);
        const f64 nm[3] = {m.x, m.y, m.z};
        for (u32 k = 0; k < 3; ++k)
          out.normal[at + k] = static_cast<u8>(std::floor((nm[k] * 0.5 + 0.5) * 255.0 + 0.5));
        out.normal[at + 3] = alpha;
      }
    }
  }
}

// ---- the coverage bake --------------------------------------------------------------------------

namespace {

constexpr u32 k_measure_samples = 2;  // a texel's samples a side

// One sample of a measuring view: nearest depth wins, and what it shows — a colour in sRGB8, or
// nothing.
struct MeasureSample {
  f64 depth = -1e300;
  u32 rgb = 0;
  bool covered = false;
};

// An orthographic view looking back along `n` at the square [-half, half]^2 about `origin`, `side`
// samples a side: the bake's rasterizer (fixed point, top-left rule) over one triangle, with a
// caller's `shade` deciding at each covered sample whether it is kept and what colour it shows
// (barycentrics of the second and third corners).
struct MeasureView {
  D3 origin, u, v, n;
  f64 half = 1.0;
  u32 side = 16;
};

template <typename Shade>
void measure_triangle(const MeasureView& view, const D3 corners[3], Vector<MeasureSample>& samples,
                      const Shade& shade) {
  const f64 scale = static_cast<f64>(view.side) / (2.0 * view.half) * static_cast<f64>(k_sub);
  i64 x[3], y[3];
  f64 z[3];
  for (u32 c = 0; c < 3; ++c) {
    const D3 d = corners[c] - view.origin;
    const f64 fx = (detail::dot(d, view.u) + view.half) * scale;
    const f64 fy = (detail::dot(d, view.v) + view.half) * scale;
    if (!(fx > -1e9 && fx < 1e9 && fy > -1e9 && fy < 1e9)) return;
    x[c] = static_cast<i64>(std::floor(fx + 0.5));
    y[c] = static_cast<i64>(std::floor(fy + 0.5));
    z[c] = detail::dot(d, view.n);
  }
  i64 area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
  if (area == 0) return;
  u32 i1 = 1, i2 = 2;
  if (area < 0) {
    i1 = 2;
    i2 = 1;
    area = -area;
  }
  const i64 ax = x[0], ay = y[0], bx = x[i1], by = y[i1], cx = x[i2], cy = y[i2];
  const i64 lo_x = std::min(ax, std::min(bx, cx)), hi_x = std::max(ax, std::max(bx, cx));
  const i64 lo_y = std::min(ay, std::min(by, cy)), hi_y = std::max(ay, std::max(by, cy));
  constexpr i64 step = k_sub / static_cast<i64>(k_measure_samples);
  const i64 side = static_cast<i64>(view.side * k_measure_samples);
  const i64 s0x = lo_x - step / 2 < 0 ? 0 : (lo_x - step / 2 + step - 1) / step;
  const i64 s0y = lo_y - step / 2 < 0 ? 0 : (lo_y - step / 2 + step - 1) / step;
  const i64 s1x = std::min((hi_x - step / 2) / step, side - 1);
  const i64 s1y = std::min((hi_y - step / 2) / step, side - 1);
  const auto bias = [](i64 ex, i64 ey) { return (ey < 0 || (ey == 0 && ex > 0)) ? 0 : -1; };
  const i64 b0 = bias(cx - bx, cy - by), b1 = bias(ax - cx, ay - cy), b2 = bias(bx - ax, by - ay);
  for (i64 sy = s0y; sy <= s1y; ++sy) {
    const i64 py = sy * step + step / 2;
    for (i64 sx = s0x; sx <= s1x; ++sx) {
      const i64 qx = sx * step + step / 2;
      const i64 w0 = (cx - bx) * (py - by) - (cy - by) * (qx - bx);
      const i64 w1 = (ax - cx) * (py - cy) - (ay - cy) * (qx - cx);
      const i64 w2 = (bx - ax) * (py - ay) - (by - ay) * (qx - ax);
      if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
      const f64 l0 = static_cast<f64>(w0) / static_cast<f64>(area);
      const f64 l1 = static_cast<f64>(w1) / static_cast<f64>(area);
      const f64 depth = z[0] * l0 + z[i1] * l1 + z[i2] * (1.0 - l0 - l1);
      MeasureSample& sample = samples[static_cast<u32>(sy * side + sx)];
      if (depth <= sample.depth) continue;
      // The corners' weights in the caller's order.
      f64 w[3];
      w[0] = l0;
      w[i1] = l1;
      w[i2] = 1.0 - l0 - l1;
      u32 rgb = 0;
      if (!shade(w[1], w[2], rgb)) continue;
      sample.depth = depth;
      sample.rgb = rgb;
      sample.covered = true;
    }
  }
}

u32 pack_rgb(const Vec4& linear) {
  return static_cast<u32>(texture::linear_to_srgb8(linear.x)) |
         (static_cast<u32>(texture::linear_to_srgb8(linear.y)) << 8) |
         (static_cast<u32>(texture::linear_to_srgb8(linear.z)) << 16);
}

// The pooled sums one level's views add to.
struct Difference {
  f64 coverage_delta = 0.0;  // sum over texels of |a - b|
  f64 coverage_union = 0.0;  // sum over texels of max(a, b)
  f64 colour_delta = 0.0;    // sum over texels both cover of the mean channel difference / 255
  f64 colour_count = 0.0;
};

}  // namespace

f32 measure_level(const assets::MeshData& mesh, const TreeLevels& levels, const TreeLevel& level,
                  const BakeLook& look, const BakedAtlas& baked) {
  if (level.jobs.empty() || baked.width == 0 || level.feature_m <= 0.0f) return 0.0f;
  const u32 leaf_rgb = pack_rgb(look.leaf);
  const u32 bark_rgb = pack_rgb(look.bark);
  // The ring: eight azimuths at the horizon, four at 30 degrees above and four below.
  D3 directions[16];
  for (u32 a = 0; a < 8; ++a) {
    const f64 angle = (static_cast<f64>(a) + 0.5) * (3.14159265358979323846 / 4.0);
    directions[a] = D3{std::cos(angle), 0.0, std::sin(angle)};
  }
  const f64 rise = 0.5;                 // sin 30
  const f64 run = 0.86602540378443865;  // cos 30
  for (u32 a = 0; a < 4; ++a) {
    const f64 angle = static_cast<f64>(a) * (3.14159265358979323846 / 2.0);
    directions[8 + a] = D3{run * std::cos(angle), rise, run * std::sin(angle)};
    directions[12 + a] = D3{run * std::cos(angle), -rise, run * std::sin(angle)};
  }
  // A branch-group level compares each job's sub-tree with its own card; the star compares the
  // whole tree with all of its planes, once.
  const bool star = level.kind == ImpostorKind::tree;
  const u32 cases = star ? 1u : level.jobs.size();
  Difference sum;
  Vector<MeasureSample> truth;
  Vector<MeasureSample> cards;
  const u32 px = level.tile_px;
  for (u32 j = 0; j < cases; ++j) {
    if (!star && j > 0 && level.jobs[j].group == level.jobs[j - 1].group) continue;
    const BakeJob& job = level.jobs[j];
    const BakeRange whole{0, levels.leaf_index_end};
    const std::span<const BakeRange> ranges =
        star ? std::span<const BakeRange>(&whole, 1)
             : std::span<const BakeRange>(job.ranges.data(), job.ranges.size());
    // The view's centre and reach: the card's centre and the farthest geometry from it.
    const D3 centre =
        from_f32(job.card.origin) +
        from_f32(job.card.u_axis) *
            (0.5 * (static_cast<f64>(job.card.u_min) + static_cast<f64>(job.card.u_max))) +
        from_f32(job.card.v_axis) *
            (0.5 * (static_cast<f64>(job.card.v_min) + static_cast<f64>(job.card.v_max)));
    f64 reach = 0.0;
    for (const BakeRange& range : ranges)
      for (u32 i = range.first_index; i < range.first_index + range.index_count; ++i) {
        const f64 r = detail::length_of(from_f32(mesh.positions[mesh.indices[i]]) - centre);
        reach = r > reach ? r : reach;
      }
    if (!(reach > 0.0)) continue;
    const f64 texel = static_cast<f64>(level.feature_m) * 0.25;
    f64 texels = std::ceil(2.0 * reach / texel);
    texels = texels < 16.0 ? 16.0 : (texels > 96.0 ? 96.0 : texels);
    for (const D3& w : directions) {
      MeasureView view;
      view.origin = centre;
      view.n = w;
      view.v = detail::perpendicular(w, D3{0.0, 1.0, 0.0});
      if (detail::length_of(view.v) < 0.5) view.v = detail::perpendicular(w, D3{1.0, 0.0, 0.0});
      view.u = detail::normalized(detail::cross(view.v, w));
      view.half = reach;
      view.side = static_cast<u32>(texels);
      const u32 count = view.side * k_measure_samples * view.side * k_measure_samples;
      truth.assign(count, MeasureSample{});
      cards.assign(count, MeasureSample{});
      // The geometry, shaded flat as the bake shades it and cut to the leaf where the bake cuts.
      for (const BakeRange& range : ranges) {
        for (u32 i = range.first_index; i + 2 < range.first_index + range.index_count; i += 3) {
          const D3 corners[3] = {from_f32(mesh.positions[mesh.indices[i]]),
                                 from_f32(mesh.positions[mesh.indices[i + 1]]),
                                 from_f32(mesh.positions[mesh.indices[i + 2]])};
          const bool leaf = i >= levels.leaf_first_index && i < levels.leaf_index_end;
          const Vec2 ta = mesh.uvs[mesh.indices[i]];
          const Vec2 tb = mesh.uvs[mesh.indices[i + 1]];
          const Vec2 tc = mesh.uvs[mesh.indices[i + 2]];
          measure_triangle(view, corners, truth, [&](f64 l1, f64 l2, u32& rgb) {
            rgb = leaf ? leaf_rgb : bark_rgb;
            const f64 l0 = 1.0 - l1 - l2;
            const f64 tu = static_cast<f64>(ta.x) * l0 + static_cast<f64>(tb.x) * l1 +
                           static_cast<f64>(tc.x) * l2;
            const f64 tv = static_cast<f64>(ta.y) * l0 + static_cast<f64>(tb.y) * l1 +
                           static_cast<f64>(tc.y) * l2;
            return surface_sample(look, leaf, tu, tv, rgb);
          });
        }
      }
      // The card or cards, each sampling its tile of the atlas at the nearest texel, kept where the
      // mask (alpha at one half) keeps it.
      const u32 first_card = star ? 0u : j;
      u32 last_card = star ? level.jobs.size() : j + 1;
      while (!star && last_card < level.jobs.size() && level.jobs[last_card].group == job.group)
        ++last_card;
      for (u32 k = first_card; k < last_card; ++k) {
        const BakeView& card = level.jobs[k].card;
        const D3 o = from_f32(card.origin);
        const D3 cu = from_f32(card.u_axis);
        const D3 cv = from_f32(card.v_axis);
        const f64 u0 = static_cast<f64>(card.u_min), u1 = static_cast<f64>(card.u_max);
        const f64 v0 = static_cast<f64>(card.v_min), v1 = static_cast<f64>(card.v_max);
        const D3 quad[4] = {o + cu * u0 + cv * v0, o + cu * u1 + cv * v0, o + cu * u1 + cv * v1,
                            o + cu * u0 + cv * v1};
        // The tile's inner texels hold the card (view_of's one-texel margin).
        const u32 tile = level.jobs[k].card_tile;
        const f64 tx0 = static_cast<f64>((tile % level.columns) * px) + 1.0;
        const f64 ty0 = static_cast<f64>((tile / level.columns) * px) + 1.0;
        const f64 inner = static_cast<f64>(px) - 2.0;
        const f64 quad_uv[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
        const u32 triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& t : triangles) {
          const D3 corners[3] = {quad[t[0]], quad[t[1]], quad[t[2]]};
          measure_triangle(view, corners, cards, [&](f64 l1, f64 l2, u32& rgb) {
            const f64 l0 = 1.0 - l1 - l2;
            const f64 s = quad_uv[t[0]][0] * l0 + quad_uv[t[1]][0] * l1 + quad_uv[t[2]][0] * l2;
            const f64 r = quad_uv[t[0]][1] * l0 + quad_uv[t[1]][1] * l1 + quad_uv[t[2]][1] * l2;
            f64 fx = std::floor(tx0 + s * inner);
            f64 fy = std::floor(ty0 + r * inner);
            fx = fx < 0.0 ? 0.0 : (fx > baked.width - 1.0 ? baked.width - 1.0 : fx);
            fy = fy < 0.0 ? 0.0 : (fy > baked.height - 1.0 ? baked.height - 1.0 : fy);
            const u32 at = (static_cast<u32>(fy) * baked.width + static_cast<u32>(fx)) * 4u;
            if (static_cast<f32>(baked.colour[at + 3]) / 255.0f < level.alpha_cutoff) return false;
            rgb = static_cast<u32>(baked.colour[at]) |
                  (static_cast<u32>(baked.colour[at + 1]) << 8) |
                  (static_cast<u32>(baked.colour[at + 2]) << 16);
            return true;
          });
        }
      }
      // Texel by texel: coverage as the fraction of samples, colour as the covered samples' mean.
      const u32 sub = view.side * k_measure_samples;
      for (u32 ty = 0; ty < view.side; ++ty) {
        for (u32 tx = 0; tx < view.side; ++tx) {
          f64 a = 0.0, b = 0.0;
          f64 ca[3] = {0.0, 0.0, 0.0}, cb[3] = {0.0, 0.0, 0.0};
          for (u32 sy = 0; sy < k_measure_samples; ++sy)
            for (u32 sx = 0; sx < k_measure_samples; ++sx) {
              const u32 at = (ty * k_measure_samples + sy) * sub + tx * k_measure_samples + sx;
              for (u32 side = 0; side < 2; ++side) {
                const MeasureSample& m = side == 0 ? truth[at] : cards[at];
                if (!m.covered) continue;
                f64* c = side == 0 ? ca : cb;
                (side == 0 ? a : b) += 1.0;
                for (u32 k = 0; k < 3; ++k)
                  c[k] += static_cast<f64>((m.rgb >> (8u * k)) & 0xFFu);
              }
            }
          const f64 n = static_cast<f64>(k_measure_samples * k_measure_samples);
          sum.coverage_delta += (a > b ? a - b : b - a) / n;
          sum.coverage_union += (a > b ? a : b) / n;
          if (a > 0.0 && b > 0.0) {
            f64 delta = 0.0;
            for (u32 k = 0; k < 3; ++k) {
              const f64 d = ca[k] / a - cb[k] / b;
              delta += d < 0.0 ? -d : d;
            }
            sum.colour_delta += delta / (3.0 * 255.0);
            sum.colour_count += 1.0;
          }
        }
      }
    }
  }
  const f64 coverage = sum.coverage_union > 0.0 ? sum.coverage_delta / sum.coverage_union : 0.0;
  const f64 colour = sum.colour_count > 0.0 ? sum.colour_delta / sum.colour_count : 0.0;
  return static_cast<f32>(1.0 - (1.0 - coverage) * (1.0 - colour));
}

}  // namespace engine::flora
