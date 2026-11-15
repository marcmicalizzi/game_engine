#pragma once

// What the shell's and the interior's meshers share (docs/subsystems/structures.md, "The
// mesher"): the context a description is meshed in, the walls' solved ends, and the emitters. Every
// length here is f64 centimetres from the description's integers until it is written as an f32
// metre; a decision uses sums, products, one division and `std::sqrt`, never the C library's
// transcendental functions.

#include <domain/structures/structures.h>

#include <cmath>

namespace engine::structures::detail {

inline constexpr f64 k_m = 0.01;    // centimetres to metres
inline constexpr f64 k_eps = 1e-7;  // centimetres: two stations or levels nearer are one

inline u32 index_of(structure::Surface s) noexcept { return static_cast<u32>(s); }

inline Vec3 metres(f64 x_cm, f64 y_cm, f64 z_cm) noexcept {
  return {static_cast<f32>(x_cm * k_m), static_cast<f32>(y_cm * k_m), static_cast<f32>(z_cm * k_m)};
}

// A frame on the ground: an origin, a unit direction `d` and its left normal `n` (the left of `d`
// seen from above, y up: `(d.z, -d.x)`). A point's coordinates in it are `s` along `d`, `y` up
// and `q` along `n`.
struct Frame2 {
  f64 ox = 0, oz = 0;
  f64 dx = 1, dz = 0;
  f64 nx = 0, nz = -1;
  Vec3 at(f64 s, f64 y, f64 q) const noexcept {
    return metres(ox + dx * s + nx * q, y, oz + dz * s + nz * q);
  }
  Vec3 dir() const noexcept { return {static_cast<f32>(dx), 0.0f, static_cast<f32>(dz)}; }
  Vec3 left() const noexcept { return {static_cast<f32>(nx), 0.0f, static_cast<f32>(nz)}; }
};
// The frame at `(ox, oz)` whose `d` is a quarter turn (0 +x, 1 +z, 2 -x, 3 -z). Turn 0 from the
// origin is the building's own axes: s is x and q is -z.
inline Frame2 axis_frame(f64 ox, f64 oz, u32 turn) noexcept {
  const f64 dx[4] = {1, 0, -1, 0}, dz[4] = {0, 1, 0, -1};
  Frame2 f;
  f.ox = ox, f.oz = oz;
  f.dx = dx[turn & 3], f.dz = dz[turn & 3];
  f.nx = f.dz, f.nz = -f.dx;
  return f;
}

// One wall, solved: its frame from its `from` end (centimetres), its length and half thickness,
// and where each side's face ends once the wall is joined to its neighbours (structures.md,
// "Joins"): `sl[e]` and `sr[e]` are the left and the right face's end stations at end `e` (0 at
// `from`, 1 at `to`), `cap[e]` a free end that is drawn closed.
struct WallGeom {
  Frame2 frame;
  f64 length = 0;
  f64 half = 0;
  f64 sl[2] = {0, 0};
  f64 sr[2] = {0, 0};
  bool cap[2] = {true, true};
};

// What a mesh pass reads: the description, its walls solved, the scope, and where triangles go.
struct Context {
  const structure::Description* d = nullptr;
  structure::MeshScope scope = structure::MeshScope::whole;
  Vector<WallGeom> walls;
  MeshPart* parts = nullptr;  // k_surfaces of them

  MeshPart& part(structure::Surface s) const noexcept { return parts[index_of(s)]; }
  // Whether a piece seen from outside (`exterior`) or from inside belongs to this pass's scope.
  bool wants(bool exterior) const noexcept {
    return scope == structure::MeshScope::whole ||
           (scope == structure::MeshScope::exterior) == exterior;
  }
};

// The walls' frames and joined ends (shell.cpp).
void solve_walls(const structure::Description& d, Vector<WallGeom>& out);
// The top of wall `wall` over the ground point `(x, z)`, centimetres above the origin: its
// storey's top, or under a roof on the top storey the roof's underside there (shell.cpp).
f64 wall_top(const structure::Description& d, u32 wall, f64 x, f64 z) noexcept;
// A roof's underside over `(x, z)`, centimetres above the origin.
f64 roof_underside(const structure::Roof& roof, f64 x, f64 z) noexcept;

// The two halves of the mesher. `mesh_shell` (shell.cpp): the walls with their openings, reveals
// and frames, the roofs and the chimneys. `mesh_interior` (interior.cpp, **the interiors' seam**):
// the floors and their wells, the ceilings, the stairs and the plain members.
void mesh_shell(const Context& ctx);
void mesh_interior(const Context& ctx);

// ---- emitters
// ------------------------------------------------------------------------------------

// A planar convex polygon as a fan, wound so its front faces `normal` (which every corner takes).
void polygon(MeshPart& part, const Vec3* points, const Vec2* uvs, u32 n, Vec3 normal);
// The same with a normal a corner (a soft surface).
void polygon_smooth(MeshPart& part, const Vec3* points, const Vec2* uvs, const Vec3* normals, u32 n,
                    Vec3 facing);

// A closed box in `frame`: `s0..s1` along, `y0..y1` up, `q0..q1` to the left, centimetres. UVs are
// metres of surface with u along `grain` (0: s, 1: y, 2: q) on every face that holds it.
void box(MeshPart& part, const Frame2& frame, f64 s0, f64 s1, f64 y0, f64 y1, f64 q0, f64 q1,
         u32 grain);

// A frame in space: an origin (x, y, z, centimetres) and three unit axes square to one another
// (a rafter's: up its slope, square to the slope, along the ridge).
struct Frame3 {
  f64 o[3] = {0, 0, 0};
  f64 axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
// A closed box in `frame`: `a0..a1` along its first axis, `b0..b1` its second, `c0..c1` its
// third, centimetres. UVs as `box`'s, `grain` naming the axis (0, 1 or 2; 3: none).
void box3(MeshPart& part, const Frame3& frame, f64 a0, f64 a1, f64 b0, f64 b1, f64 c0, f64 c1,
          u32 grain);

// A simple polygon on the ground (x0 z0 x1 z1 ..., either winding, centimetres) as triangles:
// three indices of its corners each, by ear clipping in exact integer arithmetic. False for a
// polygon with fewer than three corners or one that crosses itself.
bool triangulate(const Vector<i32>& outline, Vector<u32>& triangles);

}  // namespace engine::structures::detail
