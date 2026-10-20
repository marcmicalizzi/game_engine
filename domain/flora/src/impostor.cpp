// The impostor chain (impostor.h; docs/subsystems/flora.md, "The impostor chain"; roadmap R89):
// the branch-group cards and the whole-tree star the solver appends above its finest leaves, and
// the CPU bake that draws each card's and each plane's view of the finest geometry.
//
// Determinism: everything is IEEE + - * / and sqrt in f64 (kit.h's rule), the frames come from a
// Jacobi eigensolver run for a fixed number of sweeps, and the rasterizer decides coverage in
// integer fixed point, so the atlas bytes are a function of the mesh's bits on every toolchain.

#include "kit.h"

#include <domain/texture/texture_build.h>

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

// Cyclic Jacobi on a symmetric 3x3, eight sweeps: square roots and divisions only.
Frame fit_frame(const assets::MeshData& mesh, std::span<const SprayRange> sprays,
                std::span<const u32> members) {
  Frame f;
  f64 count = 0.0;
  for (const u32 s : members) {
    const SprayRange& r = sprays[s];
    for (u32 v = r.first_vertex; v < r.first_vertex + r.vertex_count; ++v) {
      f.centre = f.centre + from_f32(mesh.positions[v]);
      count += 1.0;
    }
  }
  if (count == 0.0) return f;
  f.centre = f.centre * (1.0 / count);
  f64 a[3][3] = {};
  for (const u32 s : members) {
    const SprayRange& r = sprays[s];
    for (u32 v = r.first_vertex; v < r.first_vertex + r.vertex_count; ++v) {
      const D3 d = from_f32(mesh.positions[v]) - f.centre;
      const f64 c[3] = {d.x, d.y, d.z};
      for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
          a[i][j] += c[i] * c[j];
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
    level.groups = groups;
    level.tile_px = spec.card_px;
    const u32 tiles = spec.representatives < groups ? spec.representatives : groups;
    level.columns = 1;
    while (level.columns * level.columns < tiles)
      ++level.columns;
    level.rows = (tiles + level.columns - 1) / level.columns;
    f64 parallax = 0.0;
    f64 texel = 0.0;
    const f64 inner = static_cast<f64>(spec.card_px) - 2.0;
    for (u32 g = 0; g < groups; ++g) {
      const std::span<const u32> mine(members.data() + offsets[g], offsets[g + 1] - offsets[g]);
      const Frame f = fit_frame(mesh, sprays, mine);
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
      const u32 tile =
          static_cast<u32>(mix(static_cast<u64>(group_node[g]) ^ (static_cast<u64>(step) << 40)) %
                           static_cast<u64>(tiles));
      const CrownNode& root = crown.nodes[group_node[g]];
      out.part = crown.nodes[root.carrier].part;
      out.weight = (root.flags & k_crown_part) != 0u ? 0.7 : root.weight;
      emit_card(out, f.centre, f.e1, f.e2, f.e3, lo1, hi1, lo2, hi2,
                tile_uv(tile, level.columns, level.rows, spec.card_px), level.indices);
      // Representative i is group floor((2 i + 1) G / 2K): bake it into tile i.
      for (u32 i = 0; i < tiles; ++i) {
        if ((2u * static_cast<u64>(i) + 1u) * groups / (2u * static_cast<u64>(tiles)) != g)
          continue;
        BakeJob job;
        job.view = view_of(f.centre, f.e1, f.e2, f.e3, lo1, hi1, lo2, hi2, spec.card_px);
        job.tile = i;
        for (const u32 s : mine)
          job.ranges.push_back(BakeRange{sprays[s].first_index, sprays[s].index_count});
        level.jobs.push_back(std::move(job));
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
  // Four vertical planes 45 degrees apart and the level one: normal n, u = up x n, v = up; the
  // level plane's u east and v north, so u x v = n on every one.
  const D3 normals[5] = {{1.0, 0.0, 0.0}, {s, 0.0, s}, {0.0, 0.0, 1.0}, {-s, 0.0, s}, k_up};
  TreeLevel level;
  level.kind = ImpostorKind::tree;
  level.groups = 5;
  level.tile_px = spec.tree_px;
  level.columns = 3;
  level.rows = 2;
  f64 parallax = 0.0;
  f64 texel = 0.0;
  const f64 inner = static_cast<f64>(spec.tree_px) - 2.0;
  out.part = part;
  out.weight = 0.0;
  for (u32 k = 0; k < 5; ++k) {
    const D3 n = normals[k];
    const D3 v = k < 4 ? k_up : D3{0.0, 0.0, -1.0};
    const D3 u = normalized(cross(v, n));
    f64 lo1 = 1e300, hi1 = -1e300, lo2 = 1e300, hi2 = -1e300;
    for (u32 i = 0; i < levels.leaf_index_end; ++i) {
      const D3 d = from_f32(mesh.positions[mesh.indices[i]]) - centre;
      const f64 x = dot(d, u);
      const f64 y = dot(d, v);
      const f64 z = dot(d, n);
      lo1 = x < lo1 ? x : lo1;
      hi1 = x > hi1 ? x : hi1;
      lo2 = y < lo2 ? y : lo2;
      hi2 = y > hi2 ? y : hi2;
      // A pixel of this plane shows the nearest geometry behind it, which can be this far off it.
      const f64 off = z < 0.0 ? -z : z;
      parallax = off > parallax ? off : parallax;
    }
    const f64 extent = (hi1 - lo1) > (hi2 - lo2) ? (hi1 - lo1) : (hi2 - lo2);
    const f64 half_texel = 0.5 * extent / inner;
    texel = half_texel > texel ? half_texel : texel;
    emit_card(out, centre, u, v, n, lo1, hi1, lo2, hi2,
              tile_uv(k, level.columns, level.rows, spec.tree_px), level.indices);
    BakeJob job;
    job.view = view_of(centre, u, v, n, lo1, hi1, lo2, hi2, spec.tree_px);
    job.tile = k;
    job.ranges.push_back(BakeRange{0, levels.leaf_index_end});
    level.jobs.push_back(std::move(job));
  }
  level.parallax_m = static_cast<f32>(parallax);
  level.texel_m = static_cast<f32>(texel);
  f64 e = parallax > texel ? parallax : texel;
  const f64 below = levels.levels.empty() ? 0.0 : static_cast<f64>(levels.levels.back().error);
  if (e <= below) e = below * (1.0 + 1.0 / 1024.0);
  level.error = static_cast<f32>(e);
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
};

}  // namespace

void bake_level(const assets::MeshData& mesh, const TreeLevels& levels, const TreeLevel& level,
                const BakeLook& look, BakedAtlas& out) {
  const u32 px = level.tile_px;
  out.width = level.columns * px;
  out.height = level.rows * px;
  out.colour.assign(static_cast<u32>(out.width) * out.height * 4u, u8{0});
  out.normal.assign(static_cast<u32>(out.width) * out.height * 4u, u8{0});
  const u32 side = px * k_samples;
  Vector<Sample> samples;
  for (const BakeJob& job : level.jobs) {
    samples.assign(side * side, Sample{});
    const D3 o = from_f32(job.view.origin);
    const D3 u = from_f32(job.view.u_axis);
    const D3 v = from_f32(job.view.v_axis);
    const D3 n = from_f32(job.view.normal);
    const f64 su = static_cast<f64>(px) / (static_cast<f64>(job.view.u_max) - job.view.u_min);
    const f64 sv = static_cast<f64>(px) / (static_cast<f64>(job.view.v_max) - job.view.v_min);
    const f64 u_min = job.view.u_min;
    const f64 v_min = job.view.v_min;
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
        const bool outline = leaf && look.leaf_outline;
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
            if (outline) {
              // A pack's leaf image is one leaf a card, whole: its outline is near the ellipse
              // the card's cell holds, petiole at v 0 and tip at |v| 1.
              const f64 tu = static_cast<f64>(ta.x) * l0 + static_cast<f64>(tb.x) * l1 +
                             static_cast<f64>(tc.x) * l2;
              const f64 tv = static_cast<f64>(ta.y) * l0 + static_cast<f64>(tb.y) * l1 +
                             static_cast<f64>(tc.y) * l2;
              const f64 fu = (tu - std::floor(tu)) * 2.0 - 1.0;
              const f64 fv = (tv < 0.0 ? -tv : tv) * 2.0 - 1.0;
              if (fu * fu + fv * fv > 1.0) continue;
            }
            sample.depth = depth;
            sample.triangle = i;
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
        for (u32 sy = 0; sy < k_samples; ++sy) {
          for (u32 sx = 0; sx < k_samples; ++sx) {
            const Sample& s = samples[(py * k_samples + sy) * side + pxi * k_samples + sx];
            if (s.triangle == k_no_triangle) continue;
            ++covered;
            const bool leaf =
                s.triangle >= levels.leaf_first_index && s.triangle < levels.leaf_index_end;
            const Vec4 c = leaf ? look.leaf : look.bark;
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

}  // namespace engine::flora
