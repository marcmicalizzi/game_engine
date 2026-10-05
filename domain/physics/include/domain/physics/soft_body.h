#pragma once

// Soft bodies: the deformable primitive of ADR-0026 as the physics module sees it.
//
// ADR-0026 puts a solver-agnostic `DeformableVolume` above this: cage topology, materials,
// layers, attachments, constraint edits, an LOD policy, and a per-tick solve. This header is
// the *backend surface* that abstraction is expected to sit on — a cage as particles plus
// constraints, with no statement about where the cage came from, how it is bound to a render
// mesh, or which tier it is running at. Nothing here is XPBD-shaped beyond the word
// "compliance", which every position-based and finite-element solver can honour (a compliance
// of 0 is a hard constraint; larger is softer, in metres per newton).
//
// What is deliberately absent, because ADR-0026 puts it a level up: cage generation from a
// signed distance field, layers and their boundary behaviour, adhesion, damage as constraint
// edits, simulation LOD, strain output, and the binding to render vertices. What is absent
// because the backend does not do it yet: soft-against-soft contact (deferred by ADR-0026),
// skinning a cage to a skeleton, and per-region materials.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/physics/deformable.h>
#include <domain/physics/types.h>

#include <span>

namespace engine::physics {

// --- how wide one cage's solve can go ---------------------------------------------------------
//
// The backend solves a cage's constraints in groups that may run concurrently, and it builds
// those groups by partitioning the cage's *vertices* into batches of at most this many. One
// thread takes one group; the constraints that straddle two groups are left to a single
// trailing group solved after them. So the width of a cage's constraint solve is a property of
// the cage's size and not of the worker count, and a cage under one batch is solved serially
// however many workers the world was given. That is the single most important thing to know
// before sizing a cage (E19, docs/experiments/e19-lattice-cage.md): two cages of 400 particles
// scale where one of 800 does not.
//
// The number mirrors the backend's own batch size and is pinned to it by a static assertion in
// src/soft_body.cpp, so a backend that changes it fails the build rather than making this
// comment quietly wrong.
inline constexpr u32 k_soft_body_constraint_batch = 256;

// How many ways the backend can split a cage of `vertex_count` particles. An upper bound: the
// partition is greedy and spatial, so an awkward cage can end up with a slightly different
// count, and the trailing group is serial either way.
constexpr u32 soft_body_solve_width(u32 vertex_count) noexcept {
  return (vertex_count + k_soft_body_constraint_batch - 1) / k_soft_body_constraint_batch;
}

// A distance constraint between two particles. `compliance` is the inverse stiffness: 0 is
// inextensible, 1e-4 is rubbery, 1e-2 is slack.
struct SoftEdge {
  u32 a = 0;
  u32 b = 0;
  f32 compliance = 0.0f;
};

// A tetrahedron whose signed volume is preserved. This is what keeps a lattice from collapsing
// flat under a press: edges alone let a cube fold along its diagonals.
struct SoftVolumeConstraint {
  u32 vertex[4] = {0, 0, 0, 0};
  f32 compliance = 0.0f;
};

// How hard a particle is held to its anchor. Both kinds drive the particle from the anchor's
// transform once per step; they differ in whether the particle still has mass while they do it.
//
//   Rigid   the particle's inverse mass is zeroed and its velocity is set to cover the whole
//           gap to the anchor in one step — the particle form of `move_kinematic`. Nothing the
//           solver does can move it, so it neither lags nor collides its way out of the anchor.
//   Spring  the particle keeps its mass and its velocity is steered a fraction of the way
//           towards the anchor each step (`follow_rate`, in inverse seconds). It therefore
//           lags a fast anchor, carries momentum, and can be pushed off the anchor by contact
//           or by the cage pulling on it — which is what plan 05 §5.14 means by "'bound' is a
//           stiff spring rather than a weld, so flesh lags a fast bone instead of tracking it
//           exactly", and what makes "no element passes through the core" a real measurement
//           rather than a consequence of the attachment (E19).
enum class AttachmentKind : u8 { Rigid, Spring };

// A particle held to a rigid body (or, with a null body, to a fixed point in the world). This
// is the "bound to a bone or a rigid body" attachment of ADR-0026; `kind` chooses between its
// stiffest form and a lagging one. Implemented without a backend constraint kind at all: the
// step writes the particle's velocity, so a new kind is a new formula here and not a new
// solver feature.
struct SoftAttachment {
  u32 vertex = 0;
  BodyId body{};
  // In the body's local space. With a null body, a fixed point given **in the soft body's own
  // frame as it was placed** (`SoftBodyDesc::transform`, the frame its rest `vertices` are in), so
  // pinning a particle where it starts is `vertices[i]` wherever in the world the cage is put; a
  // float here never holds a world position (ADR-0053).
  Vec3 local_point{};
  // Spring only, and required there: the rate at which the gap to the anchor is closed, in
  // inverse seconds. `follow_rate * dt` is the fraction of the remaining gap covered in one
  // step and is clamped to 1, so a rate at or above the step rate tracks the anchor as closely
  // as Rigid does while still carrying mass. Ignored by Rigid.
  f32 follow_rate = 0.0f;
  AttachmentKind kind = AttachmentKind::Rigid;
};

// Everything needed to instantiate one soft body. The spans are read during creation and not
// retained: the backend copies what it needs.
struct SoftBodyDesc {
  std::span<const Vec3> vertices;       // rest positions, in the body's local space
  std::span<const f32> inverse_masses;  // empty means 1 for every vertex; 0 pins one
  std::span<const SoftEdge> edges;      // rest lengths are computed from `vertices`
  std::span<const SoftVolumeConstraint> volumes;
  std::span<const SoftAttachment> attachments;
  std::span<const u32> faces;  // optional triangle indices: the collision surface, and what a
                               // pressure constraint measures its volume over
  BodyTransform transform;     // where the rest shape is placed in the world
  f32 pressure = 0.0f;         // n*R*T of the enclosed gas; needs a closed `faces` surface
  f32 linear_damping = 0.1f;
  f32 friction = 0.2f;
  f32 restitution = 0.0f;
  f32 vertex_radius = 0.0f;  // particles collide as spheres of this radius
  f32 gravity_factor = 1.0f;
  // plan 07 §7.10's `limits.max_strain`, enforced (ADR-0029 decision 4). The largest engineering
  // strain any edge is allowed to hold at a step boundary: 0.5 is a length between half and one
  // and a half times rest. 0 switches the clamp off and costs nothing — no rest lengths are kept
  // and no pass runs (plan 11 §11.10).
  //
  // It is a clamp and not a stiffness. Compliance decides how hard the solver *argues* about a
  // length; this decides what the cage is allowed to be left holding when the argument is over,
  // which is the only thing that stops an element being driven somewhere it cannot come back
  // from. E19 measured held stretch of 1.76 to 2.41 against an authored limit of 1.5, a peak of
  // 2.81, and one configuration diverging outright, all with nothing enforcing this number.
  f32 max_strain = 0.0f;
  u32 iterations = 5;  // solver iterations per sub-step; the cost knob of ADR-0026's tiers
  Layer layer = Layer::Moving;
  bool allow_sleeping = true;
  bool update_position = true;  // false pins the body's origin: for a volume set into the world
  // ADR-0029 decision 1: this volume draws on the hero allowance rather than on the ambient
  // budget, and the tiers never demote it to fit a budget. At most one volume in a world should
  // carry it — the reference interaction of plan 13 §13.7 is the case it exists for — and the
  // world reports how many do (`SoftBodyBudget::hero_count`) rather than refusing a second,
  // because refusing would break a tier transition that legitimately overlaps two heroes for a
  // few ticks.
  bool hero = false;
};

// --- fixtures -------------------------------------------------------------------------------
//
// Two cage builders. They are the shapes the tests and the E19 experiments need, and they are
// the two cage kinds ADR-0026 names for v1 (a shell with pressure, and a lattice); a generated
// adaptive lattice from a signed distance field produces the same structure with an irregular
// element size.

// A flat sheet of `columns` x `rows` particles in the XZ plane, centred on the origin, with
// structural edges along both axes and shear edges across each quad. Vertex (x, z) is at index
// z * columns + x, so pinning a corner is index 0 or columns - 1.
struct ClothSheet {
  Vector<Vec3> vertices;
  Vector<f32> inverse_masses;
  Vector<SoftEdge> edges;
  Vector<u32> faces;
  u32 columns = 0;
  u32 rows = 0;

  u32 index(u32 x, u32 z) const noexcept { return z * columns + x; }
};

ClothSheet build_cloth_sheet(u32 columns, u32 rows, f32 spacing, f32 compliance,
                             f32 shear_compliance);

// An n x n x n lattice of particles filling a cube of side (n - 1) * spacing, centred on the
// origin. Edges run along the three axes, across every face diagonal, and along all four body
// diagonals of each cell; each cell also contributes the six tetrahedra of the Kuhn
// decomposition as volume constraints, so the cube resists compression rather than only
// stretching (see docs/subsystems/physics.md for why six and not five).
//
// Vertex (x, y, z) is at index (z * n + y) * n + x.
struct LatticeVolume {
  Vector<Vec3> vertices;
  Vector<f32> inverse_masses;
  Vector<SoftEdge> edges;
  Vector<SoftVolumeConstraint> volumes;
  Vector<u32> faces;  // the outer surface, wound outwards
  u32 n = 0;

  u32 index(u32 x, u32 y, u32 z) const noexcept { return (z * n + y) * n + x; }
};

LatticeVolume build_lattice_volume(u32 n, f32 spacing, f32 edge_compliance, f32 volume_compliance);

}  // namespace engine::physics
