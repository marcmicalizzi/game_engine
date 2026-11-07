// A water body that fills (water.h; docs/subsystems/water.md, "A body that fills"; roadmap S97):
// the flood of the ground below the level from a seed over a lattice, its edge traced by marching
// squares, and the general mesher over the region. Everything is a function of the ground at the
// lattice's nodes: the flood is on integer indices (a connected component, which no visiting order
// changes), and the edge's points, the chamfer distance and the simplification are f32 + - * / and
// sqrt, so the mesh is the same bytes at any job count and on every toolchain.
#include <systems/water/water.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace engine::water {

namespace {

constexpr u32 k_none = 0xffffffffu;
// The lattice's largest side: a flood's state is a byte a node, so 4,097 is 16.8 MB at most; a
// bound that would pass it at the asked spacing grows the spacing first.
constexpr u32 k_max_side = 4097;
// An edge point is kept off the nodes by this share of a cell, so no triangle is degenerate.
constexpr f32 k_edge_margin = 1.0f / 64.0f;

// The flood on the full lattice about the seed: `half` nodes either side.
struct Flood {
  f32 spacing = 0.0f;
  u32 half = 0;
  u32 side = 0;
  Vector<u8> state;  // 0 not asked, 1 in the region, 2 asked and not in it
  u32 i0 = 0, i1 = 0, j0 = 0, j1 = 0;
  u32 inside = 0;
  bool spilled = false;
};

f64 node_depth(const Body& body, const scene_gen::Ground& ground, f64 x, f64 z) {
  const WorldPos at{body.center.x + x, body.level_m, body.center.z + z};
  return body.level_m - ground.surface(at);
}

void flood(const Body& body, const scene_gen::Ground& ground, f32 spacing, Flood& f) {
  f = Flood{};
  f.spacing = spacing;
  f.half =
      static_cast<u32>(std::ceil(static_cast<f64>(body.fill_extent_m) / static_cast<f64>(spacing)));
  f.side = 2 * f.half + 1;
  f.state.resize(f.side * f.side);
  std::fill(f.state.begin(), f.state.end(), u8{0});
  const auto depth_of = [&](u32 i, u32 j) {
    return node_depth(body, ground,
                      static_cast<f64>(static_cast<i64>(i) - static_cast<i64>(f.half)) *
                          static_cast<f64>(spacing),
                      static_cast<f64>(static_cast<i64>(j) - static_cast<i64>(f.half)) *
                          static_cast<f64>(spacing));
  };
  f.i0 = f.j0 = f.half;
  f.i1 = f.j1 = f.half;
  Vector<u32> stack;
  const u32 seed = f.half * f.side + f.half;
  if (!(depth_of(f.half, f.half) > 0.0)) {
    f.state[seed] = 2;
    return;
  }
  f.state[seed] = 1;
  stack.push_back(seed);
  while (!stack.empty()) {
    const u32 node = stack.back();
    stack.pop_back();
    ++f.inside;
    const u32 i = node % f.side;
    const u32 j = node / f.side;
    f.i0 = std::min(f.i0, i);
    f.i1 = std::max(f.i1, i);
    f.j0 = std::min(f.j0, j);
    f.j1 = std::max(f.j1, j);
    if (i == 0 || j == 0 || i + 1 == f.side || j + 1 == f.side) {
      f.spilled = true;
      return;
    }
    const u32 next[4] = {node - 1, node + 1, node - f.side, node + f.side};
    for (const u32 n : next) {
      if (f.state[n] != 0) continue;
      const bool wet = depth_of(n % f.side, n / f.side) > 0.0;
      f.state[n] = wet ? 1 : 2;
      if (wet) stack.push_back(n);
    }
  }
}

// The lattice edges between a node in the region and one out of it: the mesh's edge vertices.
u32 count_crossings(const Flood& f) {
  u32 crossings = 0;
  for (u32 j = f.j0 - 1; j <= f.j1 + 1; ++j) {
    for (u32 i = f.i0 - 1; i <= f.i1 + 1; ++i) {
      const bool in = f.state[j * f.side + i] == 1;
      if (i + 1 <= f.i1 + 1 && in != (f.state[j * f.side + i + 1] == 1)) ++crossings;
      if (j + 1 <= f.j1 + 1 && in != (f.state[(j + 1) * f.side + i] == 1)) ++crossings;
    }
  }
  return crossings;
}

f32 cross_t(f32 a, f32 b) noexcept {
  // a and b of opposite signs: the zero's place from a towards b.
  return std::clamp(a / (a - b), k_edge_margin, 1.0f - k_edge_margin);
}

// Douglas-Peucker over points [a, b] of a loop, keeping those farther than `tol` from the chord.
void simplify(const Vector<Vec2>& pts, u32 a, u32 b, f32 tol, Vector<u8>& keep) {
  if (b <= a + 1) return;
  const Vec2 p = pts[a];
  const Vec2 q = pts[b];
  const f32 dx = q.x - p.x, dz = q.y - p.y;
  const f32 len = std::sqrt(dx * dx + dz * dz);
  f32 worst = -1.0f;
  u32 at = a;
  for (u32 k = a + 1; k < b; ++k) {
    const f32 ex = pts[k].x - p.x, ez = pts[k].y - p.y;
    const f32 d = len > 0.0f ? std::fabs(ex * dz - ez * dx) / len : std::sqrt(ex * ex + ez * ez);
    if (d > worst) {
      worst = d;
      at = k;
    }
  }
  if (worst <= tol) return;
  keep[at] = 1;
  simplify(pts, a, at, tol, keep);
  simplify(pts, at, b, tol, keep);
}

}  // namespace

bool fill_body(Body& body, const scene_gen::Ground& ground, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "water body \"" + body.name + "\": " + std::move(sentence);
    return false;
  };
  body.region = FillRegion{};
  if (!body.fill) return true;
  f32 spacing = spacing_for(body);
  while (2.0 * std::ceil(static_cast<f64>(body.fill_extent_m) / static_cast<f64>(spacing)) + 1.0 >
         static_cast<f64>(k_max_side))
    spacing *= 1.25f;
  Flood f;
  for (;;) {
    flood(body, ground, spacing, f);
    if (f.inside == 0)
      return fail("its fill's seed stands on ground at or above its level, so nothing fills");
    if (f.spilled) {
      char bound[32];
      std::snprintf(bound, sizeof(bound), "%g", static_cast<f64>(body.fill_extent_m));
      return fail(std::string("its fill reaches its max_extent_m (") + bound +
                  " m) from the seed: the level spills out of the hollow");
    }
    if (u64{f.inside} + count_crossings(f) <= k_max_vertices) break;
    spacing *= 1.25f;
  }
  // The region's square, a node of margin round it (inside the lattice, since nothing spilled).
  FillRegion& r = body.region;
  r.spacing = spacing;
  const u32 bi = f.i0 - 1, bj = f.j0 - 1;
  r.nx = f.i1 - f.i0 + 3;
  r.nz = f.j1 - f.j0 + 3;
  r.origin =
      Vec2{static_cast<f32>(static_cast<f64>(static_cast<i64>(bi) - static_cast<i64>(f.half)) *
                            static_cast<f64>(spacing)),
           static_cast<f32>(static_cast<f64>(static_cast<i64>(bj) - static_cast<i64>(f.half)) *
                            static_cast<f64>(spacing))};
  r.value.resize(r.nx * r.nz);
  for (u32 j = 0; j < r.nz; ++j) {
    for (u32 i = 0; i < r.nx; ++i) {
      const u32 node = (bj + j) * f.side + (bi + i);
      const f64 x = static_cast<f64>(static_cast<i64>(bi + i) - static_cast<i64>(f.half)) *
                    static_cast<f64>(spacing);
      const f64 z = static_cast<f64>(static_cast<i64>(bj + j) - static_cast<i64>(f.half)) *
                    static_cast<f64>(spacing);
      const f32 d = static_cast<f32>(node_depth(body, ground, x, z));
      // In the region: its depth, which the flood found positive; out of it, at most -outside, so
      // a hollow the flood did not reach is never inside.
      r.value[j * r.nx + i] =
          f.state[node] == 1 ? std::max(d, k_fill_outside) : std::min(d, -k_fill_outside);
    }
  }
  r.inside = f.inside;
  r.crossings = count_crossings(f);
  // The edge traced: every cell's boundary segments (marching squares, as the mesher cuts them),
  // chained into loops by their end points, which are lattice edges.
  const auto node_at = [&](u32 i, u32 j) {
    return Vec2{r.origin.x + static_cast<f32>(i) * spacing,
                r.origin.y + static_cast<f32>(j) * spacing};
  };
  // An edge key: 2 * node + (0 along +x, 1 along +z).
  Vector<u32> next(r.nx * r.nz * 2);
  std::fill(next.begin(), next.end(), k_none);
  Vector<Vec2> point(r.nx * r.nz * 2);
  for (u32 j = 0; j + 1 < r.nz; ++j) {
    for (u32 i = 0; i + 1 < r.nx; ++i) {
      const u32 c[4] = {j * r.nx + i, j * r.nx + i + 1, (j + 1) * r.nx + i + 1, (j + 1) * r.nx + i};
      const u32 e[4] = {2 * c[0], 2 * c[1] + 1, 2 * c[3], 2 * c[0] + 1};
      f32 v[4];
      bool in[4];
      for (u32 k = 0; k < 4; ++k) {
        v[k] = r.value[c[k]];
        in[k] = v[k] > 0.0f;
      }
      for (u32 k = 0; k < 4; ++k) {
        const u32 k1 = (k + 1) & 3;
        if (in[k] == in[k1]) continue;
        const u32 a = in[k] ? k : k1;
        const u32 b = in[k] ? k1 : k;
        const Vec2 pa = node_at(c[a] % r.nx, c[a] / r.nx);
        const Vec2 pb = node_at(c[b] % r.nx, c[b] / r.nx);
        const f32 t = cross_t(v[a], v[b]);
        point[e[k]] = Vec2{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t};
      }
      const bool saddle = in[0] == in[2] && in[1] == in[3] && in[0] != in[1];
      const bool joined = (v[0] + v[1] + v[2] + v[3]) > 0.0f;
      if (saddle && !joined) {
        // Two corners apart: each its own triangle, the segment from the edge after it to the edge
        // before it.
        for (u32 k = 0; k < 4; ++k) {
          if (!in[k]) continue;
          next[e[k]] = e[(k + 3) & 3];
        }
        continue;
      }
      // The inside polygon walked round the corners: a segment from the edge where it leaves the
      // inside to the next edge where it enters it.
      for (u32 k = 0; k < 4; ++k) {
        const u32 k1 = (k + 1) & 3;
        if (!(in[k] && !in[k1])) continue;
        // Leaving at edge k; the next crossing round the cell enters the inside.
        u32 m = k1;
        while (!(!in[m] && in[(m + 1) & 3]))
          m = (m + 1) & 3;
        next[e[k]] = e[m];
      }
    }
  }
  Vector<u8> seen(next.size());
  std::fill(seen.begin(), seen.end(), u8{0});
  Vector<Vec2> loop;
  for (u32 start = 0; start < next.size(); ++start) {
    if (next[start] == k_none || seen[start] != 0) continue;
    loop.clear();
    u32 at = start;
    while (at != k_none && seen[at] == 0) {
      seen[at] = 1;
      loop.push_back(point[at]);
      at = next[at];
    }
    if (loop.size() < 3) continue;
    // Closed at its first point, then simplified to a quarter of the spacing.
    const Vec2 first = loop[0];
    loop.push_back(first);
    Vector<u8> keep(loop.size());
    std::fill(keep.begin(), keep.end(), u8{0});
    keep[0] = 1;
    keep[loop.size() - 1] = 1;
    u32 far = 0;
    f32 far_d = -1.0f;
    for (u32 k = 1; k + 1 < loop.size(); ++k) {
      const f32 dx = loop[k].x - first.x, dz = loop[k].y - first.y;
      const f32 d = dx * dx + dz * dz;
      if (d > far_d) {
        far_d = d;
        far = k;
      }
    }
    keep[far] = 1;
    simplify(loop, 0, far, 0.25f * spacing, keep);
    simplify(loop, far, loop.size() - 1, 0.25f * spacing, keep);
    r.loop_starts.push_back(r.loop_points.size());
    for (u32 k = 0; k + 1 < loop.size(); ++k)
      if (keep[k] != 0) r.loop_points.push_back(loop[k]);
  }
  return true;
}

f32 fill_value(const Body& body, f32 x, f32 z) noexcept {
  const FillRegion& r = body.region;
  if (r.nx < 2 || r.nz < 2) return -1.0f;
  const f32 fx = (x - r.origin.x) / r.spacing;
  const f32 fz = (z - r.origin.y) / r.spacing;
  if (!(fx >= 0.0f) || !(fz >= 0.0f) || fx > static_cast<f32>(r.nx - 1) ||
      fz > static_cast<f32>(r.nz - 1))
    return -k_fill_outside;
  const u32 i = std::min(static_cast<u32>(fx), r.nx - 2);
  const u32 j = std::min(static_cast<u32>(fz), r.nz - 2);
  const f32 u = fx - static_cast<f32>(i);
  const f32 w = fz - static_cast<f32>(j);
  const f32 a = r.value[j * r.nx + i], b = r.value[j * r.nx + i + 1];
  const f32 c = r.value[(j + 1) * r.nx + i], d = r.value[(j + 1) * r.nx + i + 1];
  return (a * (1.0f - u) + b * u) * (1.0f - w) + (c * (1.0f - u) + d * u) * w;
}

void build_fill_mesh(const Body& body, const scene_gen::Ground& ground, Mesh& out) {
  out = Mesh{};
  const FillRegion& r = body.region;
  if (r.nx < 2 || r.nz < 2) return;
  const f32 spacing = r.spacing;
  out.spacing = spacing;
  const u32 nodes = r.nx * r.nz;
  // The distance from the edge at every node in the region: the edge's point along each of its
  // lattice edges that leave the region, then a chamfer (a cell straight, a diagonal by sqrt 2)
  // forwards and backwards over the lattice.
  Vector<f32> dist(nodes);
  const f32 far = 1e30f;
  for (u32 j = 0; j < r.nz; ++j) {
    for (u32 i = 0; i < r.nx; ++i) {
      const u32 n = j * r.nx + i;
      const f32 v = r.value[n];
      if (!(v > 0.0f)) {
        dist[n] = 0.0f;
        continue;
      }
      f32 d = far;
      const i32 di[4] = {-1, 1, 0, 0};
      const i32 dj[4] = {0, 0, -1, 1};
      for (u32 k = 0; k < 4; ++k) {
        const i64 ni = static_cast<i64>(i) + di[k], nj = static_cast<i64>(j) + dj[k];
        if (ni < 0 || nj < 0 || ni >= static_cast<i64>(r.nx) || nj >= static_cast<i64>(r.nz))
          continue;
        const f32 w = r.value[static_cast<u32>(nj) * r.nx + static_cast<u32>(ni)];
        if (w > 0.0f) continue;
        d = std::min(d, cross_t(v, w) * spacing);
      }
      dist[n] = d;
    }
  }
  const f32 diagonal = spacing * 1.41421356f;
  for (u32 pass = 0; pass < 2; ++pass) {
    for (u32 s = 0; s < nodes; ++s) {
      const u32 n = pass == 0 ? s : nodes - 1 - s;
      if (!(r.value[n] > 0.0f)) continue;
      const i64 i = n % r.nx, j = n / r.nx;
      const i64 sj = pass == 0 ? -1 : 1;
      const i64 cand[4][2] = {{i - sj, j}, {i - 1, j + sj}, {i, j + sj}, {i + 1, j + sj}};
      const f32 step[4] = {spacing, diagonal, spacing, diagonal};
      for (u32 k = 0; k < 4; ++k) {
        const i64 ci = cand[k][0], cj = cand[k][1];
        if (ci < 0 || cj < 0 || ci >= static_cast<i64>(r.nx) || cj >= static_cast<i64>(r.nz))
          continue;
        const u32 c = static_cast<u32>(cj) * r.nx + static_cast<u32>(ci);
        if (!(r.value[c] > 0.0f)) continue;
        dist[n] = std::min(dist[n], dist[c] + step[k]);
      }
    }
  }
  // Vertices on first use, cell by cell in row order: a node's, or an edge point's (keyed 2 * node
  // + axis, as the trace keys them), so two cells sharing a lattice edge share its vertex.
  Vector<u32> node_vertex(nodes);
  Vector<u32> edge_vertex(nodes * 2);
  std::fill(node_vertex.begin(), node_vertex.end(), k_none);
  std::fill(edge_vertex.begin(), edge_vertex.end(), k_none);
  out.positions.reserve(r.inside + r.crossings);
  out.normals.reserve(r.inside + r.crossings);
  out.uvs.reserve(r.inside + r.crossings);
  const auto add = [&](Vec2 p, f32 from_edge) {
    out.positions.push_back(Vec3{p.x, 0.0f, p.y});
    out.normals.push_back(Vec3{0.0f, 1.0f, 0.0f});
    const WorldPos at{body.center.x + static_cast<f64>(p.x), body.level_m,
                      body.center.z + static_cast<f64>(p.y)};
    const f64 depth = std::max(0.0, body.level_m - ground.surface(at));
    out.max_depth = std::max(out.max_depth, static_cast<f32>(depth));
    out.uvs.push_back(Vec2{from_edge, static_cast<f32>(depth)});
    return out.positions.size() - 1;
  };
  const auto node_at = [&](u32 n) {
    return Vec2{r.origin.x + static_cast<f32>(n % r.nx) * spacing,
                r.origin.y + static_cast<f32>(n / r.nx) * spacing};
  };
  const auto tri = [&](u32 a, u32 b, u32 c) {
    const Vec3 pa = out.positions[a], pb = out.positions[b], pc = out.positions[c];
    const f32 ny = (pc.x - pa.x) * (pb.z - pa.z) - (pb.x - pa.x) * (pc.z - pa.z);
    out.indices.push_back(a);
    if (ny >= 0.0f) {
      out.indices.push_back(b);
      out.indices.push_back(c);
    } else {
      out.indices.push_back(c);
      out.indices.push_back(b);
    }
  };
  u32 poly[8];
  for (u32 j = 0; j + 1 < r.nz; ++j) {
    for (u32 i = 0; i + 1 < r.nx; ++i) {
      const u32 c[4] = {j * r.nx + i, j * r.nx + i + 1, (j + 1) * r.nx + i + 1, (j + 1) * r.nx + i};
      const u32 e[4] = {2 * c[0], 2 * c[1] + 1, 2 * c[3], 2 * c[0] + 1};
      f32 v[4];
      bool in[4];
      u32 count = 0;
      for (u32 k = 0; k < 4; ++k) {
        v[k] = r.value[c[k]];
        in[k] = v[k] > 0.0f;
        count += in[k] ? 1u : 0u;
      }
      if (count == 0) continue;
      const auto corner = [&](u32 k) {
        if (node_vertex[c[k]] == k_none) node_vertex[c[k]] = add(node_at(c[k]), dist[c[k]]);
        return node_vertex[c[k]];
      };
      const auto edge = [&](u32 k) {
        if (edge_vertex[e[k]] == k_none) {
          const u32 k1 = (k + 1) & 3;
          const u32 a = in[k] ? k : k1;
          const u32 b = in[k] ? k1 : k;
          const Vec2 pa = node_at(c[a]);
          const Vec2 pb = node_at(c[b]);
          const f32 t = cross_t(v[a], v[b]);
          edge_vertex[e[k]] = add(Vec2{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t}, 0.0f);
        }
        return edge_vertex[e[k]];
      };
      const bool saddle = in[0] == in[2] && in[1] == in[3] && in[0] != in[1];
      const bool joined = (v[0] + v[1] + v[2] + v[3]) > 0.0f;
      if (saddle && !joined) {
        for (u32 k = 0; k < 4; ++k) {
          if (!in[k]) continue;
          const u32 a = corner(k);
          const u32 b = edge(k);
          const u32 d = edge((k + 3) & 3);
          tri(a, b, d);
        }
        continue;
      }
      // The inside polygon, walked round the cell's corners: a corner inside, and an edge point
      // where the inside begins or ends; convex in every case marching squares makes, so a fan.
      u32 n = 0;
      for (u32 k = 0; k < 4; ++k) {
        if (in[k]) poly[n++] = corner(k);
        if (in[k] != in[(k + 1) & 3]) poly[n++] = edge(k);
      }
      for (u32 k = 1; k + 1 < n; ++k)
        tri(poly[0], poly[k], poly[k + 1]);
    }
  }
}

}  // namespace engine::water
