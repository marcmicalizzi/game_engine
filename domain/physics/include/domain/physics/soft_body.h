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
#include <domain/physics/types.h>

#include <span>

namespace engine::physics {

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

// A particle held to a rigid body (or, with a null body, to a fixed point in the world). The
// particle becomes kinematic and is driven from the body's transform every step, which is the
// "bound to a bone or a rigid body" attachment of ADR-0026 in its stiffest form. A springy
// attachment is a later addition to this struct, not a different mechanism.
struct SoftAttachment {
  u32 vertex = 0;
  BodyId body{};
  Vec3 local_point{};  // in the body's local space; the world point when `body` is null
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
  Transform3 transform;        // where the rest shape is placed; scale is ignored
  f32 pressure = 0.0f;         // n*R*T of the enclosed gas; needs a closed `faces` surface
  f32 linear_damping = 0.1f;
  f32 friction = 0.2f;
  f32 restitution = 0.0f;
  f32 vertex_radius = 0.0f;  // particles collide as spheres of this radius
  f32 gravity_factor = 1.0f;
  u32 iterations = 5;  // solver iterations per sub-step; the cost knob of ADR-0026's tiers
  Layer layer = Layer::Moving;
  bool allow_sleeping = true;
  bool update_position = true;  // false pins the body's origin: for a volume set into the world
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
// origin. Edges run along the three axes, across every face diagonal, and along the two body
// diagonals of each cell; each cell also contributes five tetrahedra as volume constraints, so
// the cube resists compression rather than only stretching.
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
