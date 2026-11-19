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
//
// An exterior wall's outer face may lean (`Wall.top_thickness`) and its plinth may stand proud
// (`Description.plinth_proud`), so the joins are solved three times: `ends[0]` with the faces as
// described (`sl` and `sr` are its `[..][0]` and `[..][1]`), `ends[1]` with every outer face
// leaned in to its top thickness, `ends[2]` with every outer face stood out by the plinth's
// projection. `ends[m][side][e]`: side 0 the left face, 1 the right. `neighbour[e]` is the one
// wall that shares end `e` at a corner of two (-1 otherwise), `neighbour_from[e]` whether it
// leaves the corner from its own `from` end.
struct WallGeom {
  Frame2 frame;
  f64 length = 0;
  f64 half = 0;
  f64 sl[2] = {0, 0};
  f64 sr[2] = {0, 0};
  f64 ends[3][2][2] = {};
  i32 neighbour[2] = {-1, -1};
  bool neighbour_from[2] = {false, false};
  bool cap[2] = {true, true};
};

// How an exterior wall's outer face leans: `total` centimetres in between `y0` (the plinth's top,
// or the storey's floor) and `y1` (the storey's top), plumb above. `total` 0: it does not.
struct Lean {
  f64 total = 0, y0 = 0, y1 = 1;
  f64 fraction(f64 y) const noexcept {
    const f64 f = (y - y0) / (y1 - y0);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
  }
  f64 at(f64 y) const noexcept { return total * fraction(y); }
};
Lean lean_of(const structure::Description& d, const structure::Wall& w) noexcept;

// What a mesh pass reads: the description, its walls solved, the scope, and where triangles go.
struct Context {
  const structure::Description* d = nullptr;
  structure::MeshScope scope = structure::MeshScope::whole;
  u32 leaf = 0;  // the `leaf` scope's leaf, in `door_records`' order
  Vector<WallGeom> walls;
  MeshPart* parts = nullptr;  // k_surfaces of them

  MeshPart& part(structure::Surface s) const noexcept { return parts[index_of(s)]; }
  // Whether a piece seen from outside (`exterior`) or from inside belongs to this pass's scope. The
  // `leaf` scope holds one leaf and nothing else.
  bool wants(bool exterior) const noexcept {
    if (scope == structure::MeshScope::leaf) return false;
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

// The sine and the cosine of a whole number of degrees, 0 to 90, by sums and products alone (a
// polynomial over at most 45 degrees), so every toolchain turns a leaf by the same bits; 0 and 90
// are exact.
void turn_deg(i32 degrees, f64& sine, f64& cosine) noexcept;

// One leaf of an opening as it stands (structures.md, "Leaves and mullions"): its pivot on the
// ground, the unit direction from its hinge to its free edge (`u`) and through its thickness
// from the face it opens toward (`v`), its width, its foot and head, and its boards' and ledges'
// thicknesses. Centimetres, the building's frame.
struct LeafPlace {
  f64 px = 0, pz = 0;
  f64 ux = 1, uz = 0;
  f64 vx = 0, vz = 1;
  f64 width = 0;
  f64 y0 = 0, y1 = 0;
  f64 boards = 0, ledges = 0;
  bool door = true;
};
// The leaves of opening `o` (0, 1 or 2), for the mesher and the walk records alike (shell.cpp).
u32 leaf_places(const structure::Description& d, const structure::Opening& o,
                LeafPlace (&out)[2]) noexcept;
// The description's `index`-th leaf, in `door_records`' order (openings, then hinge first), shut:
// its place with the opening's angle undone, and the opening. False past the last.
bool leaf_shut(const structure::Description& d, u32 index, LeafPlace& out,
               const structure::Opening*& opening) noexcept;
// Which way a shut leaf's thickness lies in its own frame, whose +x is from the hinge to the free
// edge: +1 along +z, -1 along -z, chosen so that the leaf opens by turning its +x toward -z for +1
// (a positive angle about +y) and toward +z for -1 (structures.md, "A leaf is a mesh of its own").
f64 leaf_side(const LeafPlace& shut) noexcept;
// The `leaf` scope's mesh (shell.cpp): leaf `ctx.leaf` alone, shut, its hinge's foot at the origin.
void mesh_leaf(const Context& ctx);

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
