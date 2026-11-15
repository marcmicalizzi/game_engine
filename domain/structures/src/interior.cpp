// **The interiors' seam** (docs/subsystems/structures.md, "The interiors' seam"): what stands
// inside the shell and is meshed with it. The inner walls and their doorways are walls and
// openings like any other and are the shell's (shell.cpp); this file meshes the rest of what the
// description says of an interior: the floors with their stairs' wells, the ceilings, the stair
// flights, and the plain members (`Description::boxes`). A new kind of interior part is a new
// function here, called from `mesh_interior`, drawing through emit.h's emitters.

#include "emit.h"

#include <algorithm>

namespace engine::structures::detail {

using structure::Description;
using structure::Surface;

namespace {

Vec2 ground_uv(f64 x, f64 z) noexcept {
  return {static_cast<f32>(x * k_m), static_cast<f32>(z * k_m)};
}

// Storey `storey`'s floor outline at height `y` (centimetres), facing up or down, less the wells
// of the stairs that land on it. With no well it is the outline's triangles; with wells, the
// cells of the grid the outline's and the wells' edges make (the validator holds the outline to
// axis-aligned edges then).
void floor_sheet(const Context& ctx, u32 storey, f64 y, bool up, Surface surface) {
  const Description& d = *ctx.d;
  MeshPart& part = ctx.part(surface);
  const Vec3 normal{0.0f, up ? 1.0f : -1.0f, 0.0f};
  Vector<const structure::Stair*> wells;
  for (const structure::Stair& s : d.stairs)
    if (s.storey + 1 == storey) wells.push_back(&s);
  const u32 n = d.footprint.size() / 2;
  if (wells.empty()) {
    Vector<u32> triangles;
    if (!triangulate(d.footprint, triangles)) return;
    const u32 base = part.positions.size();
    for (u32 i = 0; i < n; ++i) {
      part.positions.push_back(metres(d.footprint[2 * i], y, d.footprint[2 * i + 1]));
      part.normals.push_back(normal);
      part.uvs.push_back(ground_uv(d.footprint[2 * i], d.footprint[2 * i + 1]));
    }
    for (u32 t = 0; t + 2 < triangles.size(); t += 3) {
      const u32 a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
      const i64 ax = d.footprint[2 * a], az = d.footprint[2 * a + 1];
      const i64 bx = d.footprint[2 * b], bz = d.footprint[2 * b + 1];
      const i64 cx = d.footprint[2 * c], cz = d.footprint[2 * c + 1];
      // The triangle's normal's y: (b - a) x (c - a).
      const bool faces_up = (bz - az) * (cx - ax) - (bx - ax) * (cz - az) > 0;
      part.indices.push_back(base + a);
      part.indices.push_back(base + (faces_up == up ? b : c));
      part.indices.push_back(base + (faces_up == up ? c : b));
    }
    return;
  }
  Vector<i32> xs, zs;
  for (u32 i = 0; i < n; ++i) {
    xs.push_back(d.footprint[2 * i]);
    zs.push_back(d.footprint[2 * i + 1]);
  }
  for (const structure::Stair* s : wells) {
    xs.push_back(s->well[0]);
    xs.push_back(s->well[2]);
    zs.push_back(s->well[1]);
    zs.push_back(s->well[3]);
  }
  const auto unique = [](Vector<i32>& v) {
    std::sort(v.data(), v.data() + v.size());
    u32 kept = 0;
    for (u32 i = 0; i < v.size(); ++i)
      if (kept == 0 || v[i] != v[kept - 1]) v[kept++] = v[i];
    v.resize(kept);
  };
  unique(xs);
  unique(zs);
  // Whether the cell's middle (doubled, so it stays an integer) is inside the outline.
  const auto inside = [&](i64 x2, i64 z2) {
    bool in = false;
    for (u32 i = 0; i < n; ++i) {
      const u32 j = (i + 1) % n;
      const i64 xi = 2 * static_cast<i64>(d.footprint[2 * i]),
                zi = 2 * static_cast<i64>(d.footprint[2 * i + 1]);
      const i64 xj = 2 * static_cast<i64>(d.footprint[2 * j]),
                zj = 2 * static_cast<i64>(d.footprint[2 * j + 1]);
      if ((zi > z2) == (zj > z2)) continue;
      // The edge's x where it crosses the line z = z2, against x2, without a division.
      const i64 lhs = (x2 - xi) * (zj - zi), rhs = (xj - xi) * (z2 - zi);
      if (zj > zi ? lhs < rhs : lhs > rhs) in = !in;
    }
    return in;
  };
  Vector<u32> ids(xs.size() * zs.size(), ~0u);
  const auto corner = [&](u32 ix, u32 iz) {
    u32& id = ids[iz * xs.size() + ix];
    if (id == ~0u) {
      id = part.positions.size();
      part.positions.push_back(metres(xs[ix], y, zs[iz]));
      part.normals.push_back(normal);
      part.uvs.push_back(ground_uv(xs[ix], zs[iz]));
    }
    return id;
  };
  for (u32 iz = 0; iz + 1 < zs.size(); ++iz)
    for (u32 ix = 0; ix + 1 < xs.size(); ++ix) {
      const i64 x2 = static_cast<i64>(xs[ix]) + xs[ix + 1],
                z2 = static_cast<i64>(zs[iz]) + zs[iz + 1];
      if (!inside(x2, z2)) continue;
      bool hole = false;
      for (const structure::Stair* s : wells)
        hole = hole ||
               (x2 > 2 * static_cast<i64>(s->well[0]) && x2 < 2 * static_cast<i64>(s->well[2]) &&
                z2 > 2 * static_cast<i64>(s->well[1]) && z2 < 2 * static_cast<i64>(s->well[3]));
      if (hole) continue;
      const u32 a = corner(ix, iz), b = corner(ix + 1, iz), c = corner(ix + 1, iz + 1);
      const u32 e = corner(ix, iz + 1);
      // (x, z) to (x + 1, z) to (x + 1, z + 1) faces down (y up, right-handed).
      const u32 quad[4] = {a, up ? e : b, c, up ? b : e};
      part.indices.push_back(quad[0]);
      part.indices.push_back(quad[1]);
      part.indices.push_back(quad[2]);
      part.indices.push_back(quad[0]);
      part.indices.push_back(quad[2]);
      part.indices.push_back(quad[3]);
    }
}

void floors(const Context& ctx) {
  const Description& d = *ctx.d;
  if (!ctx.wants(false)) return;
  for (u32 s = 0; s < d.storeys.size(); ++s) {
    const structure::Storey& storey = d.storeys[s];
    const f64 top = storey.elevation;
    if (s == 0) {
      floor_sheet(ctx, s, top, true, Surface::floor);
      continue;
    }
    const f64 under = top - static_cast<f64>(storey.floor_thickness);
    floor_sheet(ctx, s, top, true, Surface::boards);
    floor_sheet(ctx, s, under, false, Surface::ceiling);
    // The wells' sides: the floor's depth, facing into the well.
    MeshPart& timber = ctx.part(Surface::timber);
    for (const structure::Stair& stair : d.stairs) {
      if (stair.storey + 1 != s) continue;
      const f64 x0 = stair.well[0], z0 = stair.well[1], x1 = stair.well[2], z1 = stair.well[3];
      const f64 px[5] = {x0, x1, x1, x0, x0}, pz[5] = {z0, z0, z1, z1, z0};
      const Vec3 inward[4] = {{0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}};
      for (u32 e = 0; e < 4; ++e) {
        const Vec3 points[4] = {metres(px[e], under, pz[e]), metres(px[e + 1], under, pz[e + 1]),
                                metres(px[e + 1], top, pz[e + 1]), metres(px[e], top, pz[e])};
        const f64 run = e % 2 == 0 ? x1 - x0 : z1 - z0;
        const Vec2 uvs[4] = {{0.0f, static_cast<f32>(under * k_m)},
                             {static_cast<f32>(run * k_m), static_cast<f32>(under * k_m)},
                             {static_cast<f32>(run * k_m), static_cast<f32>(top * k_m)},
                             {0.0f, static_cast<f32>(top * k_m)}};
        polygon(timber, points, uvs, 4, inward[e]);
      }
    }
  }
  const structure::Storey& last = d.storeys[d.storeys.size() - 1];
  if (last.ceiling)
    floor_sheet(ctx, d.storeys.size(), static_cast<f64>(last.elevation + last.height), false,
                Surface::ceiling);
}

// A straight flight: every step a box from under its tread to its top, the last riser the
// landing's own edge. Walked by the walker's step rule (plan 14 §14.4).
void stairs(const Context& ctx) {
  const Description& d = *ctx.d;
  if (!ctx.wants(false)) return;
  MeshPart& part = ctx.part(Surface::boards);
  for (const structure::Stair& s : d.stairs) {
    if (s.kind != structure::StairKind::straight) continue;
    const structure::Storey& below = d.storeys[s.storey];
    const f64 base = below.elevation;
    const f64 rise = static_cast<f64>(below.height) / static_cast<f64>(s.risers);
    const Frame2 frame = axis_frame(s.foot[0], s.foot[1], s.dir);
    const f64 half = static_cast<f64>(s.width) * 0.5;
    for (u32 i = 0; i + 1 < s.risers; ++i) {
      const f64 top = base + rise * static_cast<f64>(i + 1);
      const f64 under = std::max(base, base + rise * static_cast<f64>(i) - 12.0);
      box(part, frame, static_cast<f64>(s.going) * static_cast<f64>(i),
          static_cast<f64>(s.going) * static_cast<f64>(i + 1), under, top, -half, half, 2);
    }
  }
}

void boxes(const Context& ctx) {
  const Description& d = *ctx.d;
  const Frame2 axes = axis_frame(0.0, 0.0, 0);  // s is x, q is -z
  for (const structure::Box& b : d.boxes) {
    if (!ctx.wants(b.room < 0)) continue;
    const i32 dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2];
    const u32 grain = dy >= dx && dy >= dz ? 1u : (dx >= dz ? 0u : 2u);
    box(ctx.part(b.surface), axes, b.min[0], b.max[0], b.min[1], b.max[1],
        -static_cast<f64>(b.max[2]), -static_cast<f64>(b.min[2]), grain);
  }
}

}  // namespace

void mesh_interior(const Context& ctx) {
  floors(ctx);
  stairs(ctx);
  boxes(ctx);
}

}  // namespace engine::structures::detail
