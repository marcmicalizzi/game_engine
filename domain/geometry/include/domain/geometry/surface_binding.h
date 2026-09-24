#pragma once

// The surface binding: a render vertex bound to a point of a refined surface, and the displacement
// transfer that moves it (docs/subsystems/geometry.md, "Limit surfaces and surface bindings";
// docs/plan/07-content-pipeline.md §7.11, the binding paragraph; docs/plan/05-simulation.md §5.16,
// the 2026-09-23 addendum's item 3).
//
// Why it is shaped this way. A tissue region's render surface is bound to its cage through the
// cage's limit surface (limit_surface.h), evaluated at a dense refinement, so the per-vertex record
// can stay small: a triangle of the *refined* surface, a point in it, and an offset along its
// normal. The record stores **no** absolute position of the region's surface. It moves the
// observed base by a weight times the surface's *change* from a paired reference observation:
//
//     out = base + w * ( D(f) + h * (n(S_ref + D)(f) - n(S_ref)(f)) ),   D = L (x_state - x_ref)
//
// where `f` is the footpoint (triangle and barycentrics), `L` the limit operator, `x` the cage's
// nodes, `S_ref = L x_ref` the refined surface at the reference, `D` its displacement, `n` the
// footpoint triangle's unit normal and `h` the signed normal offset measured at binding time.
// Three properties follow, and each is a validator row the plan names:
//
// - **The base is bitwise where the weight is zero.** A vertex with weight 0 is copied, never
//   recomputed, so the torso outside a region, and a seam two bilateral modules share and neither
//   owns, stays exactly the base whatever the cage does and whichever module runs first.
// - **At the reference state the output is the base, bitwise**, whatever the binding's error: the
//   transfer moves a vertex by a difference, so a footpoint a little off, or an offset that does
//   not reconstruct the vertex, costs nothing at rest and only its product with the deformation
//   elsewhere.
// - **Positions are affine in the nodes; the offset term is not.** With `h = 0` the transfer is
//   linear in the node state (a midpoint state gives the midpoint output). The offset transports
//   as `h` times the *change* of the normal, never as an absolute offset along the current normal,
//   which is the linearization the plan asks the runtime to declare: it vanishes at the reference
//   and moves rigidly with a rigid motion, and its nonlinearity is second order in the rotation.
//
// **Why the displacement form, and not S_state - S_ref.** "Bitwise at the reference" is only true
// if every change the transfer computes is *exactly* zero when the nodes have not moved, and a
// difference of two evaluations is not: once a compiler may fuse a multiply into the subtraction
// that follows it (FMA contraction — GCC's default for C++ at x86-64-v3), `a*b - a*b` becomes
// `fma(a, b, -round(a*b))`, the product's rounding error. That happened here: the first transfer
// subtracted the two footpoint normals, GCC 13 fused one normal's final multiply into the
// subtraction, and one vertex in 300 moved at rest (linux-gcc-release, 2026-09-24). So nothing is
// written as a difference of evaluations. The nodes are subtracted first, `surface_displacement`
// pushes that difference through the operator, and the normal's change is expanded so that every
// term is a product with the displaced edges (`apply_binding`'s body). At the reference every such
// product has an exact zero factor, and however a compiler fuses it, the result is zero. Do not
// "simplify" it back into two normals and a subtraction.
//
// **The normal is the footpoint triangle's own**, not an interpolated limit normal, because that
// is the authoring side's transport frame and a binding authored there must mean the same here.
// It is piecewise constant, so two vertices with the same `h` on either side of a refined edge move
// apart by about `h` times the change of that edge's dihedral angle in radians: 17 µm for a 1 mm
// offset across an edge whose fold changes by one degree. The limit tangent operators are there if
// a fixture with real offsets shows that step.
//
// The record is **10 bytes** (the plan's proposed packing, pinned by the size table): a `u16`
// refined triangle, two `u16` barycentric weights in 1/65535 (the third is what they leave), an
// IEEE half-float normal offset in the positions' units, and a `u16` blend weight in 1/65535. A
// `u16` triangle caps a region's refined surface at 65,536 triangles — 1,024 control triangles at
// level 3, 4,096 at level 2 — and `surface_binding_can_represent` is the check: `bind_to_surface`
// refuses a larger surface rather than wrapping, since the plan's no-hidden-limits rule has no
// wider record to fall back to yet. What the packing costs is measured, not assumed:
// `SurfaceBindReport::max_quantization_error` is the distance between the exact footpoint plus
// offset and what the record decodes to.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/limit_surface.h>

#include <limits>
#include <span>
#include <string>

namespace engine::geometry {

struct SurfaceBinding {
  u16 triangle = 0;             // a triangle of the refined surface
  u16 barycentric[2] = {0, 0};  // corners 0 and 1, in 1/65535; corner 2 takes the rest
  u16 normal_offset = 0;        // IEEE half float, signed along the reference triangle normal
  u16 weight = 0;               // blend weight in 1/65535; 0 leaves the base untouched
};
static_assert(sizeof(SurfaceBinding) == 10, "SurfaceBinding is the plan's 10-byte record");

inline constexpr u32 k_surface_binding_max_triangles = 65536;

// Whether a refined surface of this many triangles can be addressed by the record's `u16`.
constexpr bool surface_binding_can_represent(u32 surface_triangles) noexcept {
  return surface_triangles <= k_surface_binding_max_triangles;
}

// The record's decoders: exact at the ends (a weight of 65535 is 1.0f, of 0 is 0.0f), and what the
// transfer itself uses.
f32 binding_weight(const SurfaceBinding& binding) noexcept;
Vec3 binding_barycentrics(const SurfaceBinding& binding) noexcept;  // corners 0, 1, 2
f32 binding_normal_offset(const SurfaceBinding& binding) noexcept;

struct SurfaceBindOptions {
  // The blend weight of each render vertex, in [0, 1]; empty binds every vertex at weight 1. A
  // vertex of weight 0 is not searched for (it is never moved) and gets a record naming corner 2
  // of triangle 0.
  std::span<const f32> weights;
  // A vertex whose |normal offset| exceeds this is reported (it is still bound).
  f32 offset_limit = std::numeric_limits<f32>::infinity();
};

struct SurfaceBindReport {
  // Render vertices (of nonzero weight) whose nearest point is on the refined surface's boundary:
  // on a boundary edge or at a boundary vertex. Such a vertex lies beyond the surface, and its
  // offset is not along the line to its footpoint.
  Vector<u32> boundary_footpoints;
  // Render vertices whose |normal offset| exceeds `SurfaceBindOptions::offset_limit`.
  Vector<u32> offset_exceeded;
  f32 max_abs_offset = 0.0f;
  // The largest distance between a vertex's exact footpoint plus offset and what its 10-byte record
  // decodes to: the barycentrics' 1/65535 and the half float's 2^-11 relative step, together.
  f32 max_quantization_error = 0.0f;
};

// Binds every render vertex to its nearest point of the refined surface (`surface_positions` at
// the reference state, `surface_faces` three indices a triangle): the triangle, the barycentrics
// of the nearest point, and the signed offset of the vertex along that triangle's unit normal.
// Ties between equally near triangles go to the lower triangle index, so the result is a function
// of the input alone. Refuses a surface of more than `k_surface_binding_max_triangles`, an index
// out of range, a weight outside [0, 1] and an offset a half float cannot hold.
bool bind_to_surface(std::span<const Vec3> render_vertices, std::span<const Vec3> surface_positions,
                     std::span<const u32> surface_faces, const SurfaceBindOptions& options,
                     Vector<SurfaceBinding>& out, SurfaceBindReport* report = nullptr,
                     std::string* error = nullptr);

// Load-time check of records that came from outside: every triangle in range, the two stored
// barycentrics summing to at most 65535, and a finite offset.
bool validate_surface_bindings(std::span<const SurfaceBinding> bindings, u32 surface_triangles,
                               std::string* error = nullptr);

// The refined surface's displacement from the paired reference observation, `L (x_state - x_ref)`:
// the nodes are subtracted into `node_displacement` (scratch, one per node) and the operator is
// applied to that, never to the two states separately, so it is exactly zero wherever the nodes
// have not moved (the header comment says why that matters). `out` has one entry per refined
// vertex (`limit.rows()`). Once per region per frame, the same cost as evaluating the state.
void surface_displacement(const CsrMatrix& limit, std::span<const Vec3> reference_nodes,
                          std::span<const Vec3> state_nodes, std::span<Vec3> node_displacement,
                          std::span<Vec3> out) noexcept;

// The per-frame displacement transfer (the header comment's formula), `bindings[i]` moving
// `base_positions[i]` into `out[i]`. `surface_reference` is the refined surface at the reference
// (`apply(limit, reference_nodes, ...)`, or the same positions stored), `surface_displacement` its
// displacement at the current state (`surface_displacement` above) and `surface_faces` its
// triangles. At the reference, where the displacement is zero, every output is its base bit for
// bit, whichever compiler built this and wherever `surface_reference` was evaluated. `out` may be
// `base_positions` itself (the same span), never a partial overlap. The bindings must be valid for
// the surface (`validate_surface_bindings`); this does not check them again.
void apply_binding(std::span<const SurfaceBinding> bindings, std::span<const Vec3> base_positions,
                   std::span<const u32> surface_faces, std::span<const Vec3> surface_reference,
                   std::span<const Vec3> surface_displacement, std::span<Vec3> out) noexcept;

}  // namespace engine::geometry
