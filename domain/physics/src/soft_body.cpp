// Soft bodies, and the two cage builders the tests and the E19 precursor need.

#include "world_impl.h"

#include <core/log/log.h>

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyUpdateContext.h>

#include <utility>

namespace engine::physics {

// The public header promises that `k_soft_body_constraint_batch` is the backend's own batch
// size. This is where that promise is kept: a backend that changes it fails the build here
// rather than turning `soft_body_solve_width` into a plausible-looking wrong answer.
static_assert(k_soft_body_constraint_batch == JPH::SoftBodyUpdateContext::cVertexConstraintBatch,
              "physics: the mirrored soft-body constraint batch size no longer matches Jolt's");

namespace {

using SharedSettings = JPH::SoftBodySharedSettings;

bool indices_in_range(std::span<const u32> indices, u32 count) noexcept {
  for (const u32 index : indices)
    if (index >= count) return false;
  return true;
}

// The clamp is a constraint projection, and projecting one edge moves the particles its
// neighbours share, so a single sweep leaves part of a violation behind: in E19's lattice a
// particle carries eighteen edges. It therefore sweeps until the worst edge is inside the limit
// to within `k_strain_clamp_tolerance` of its own rest length.
//
// **A tolerance and not "until nothing moved", because "nothing moved" never happens under load.**
// Measured: with the exit condition "a sweep corrected no edge", E19's press burned all sixteen
// sweeps on every one of the sixty hold ticks at every element count — the solver re-stretches
// what the previous sweep pulled in, so Gauss-Seidel keeps correcting a smaller and smaller amount
// forever. That is 16 x 5,068 edge visits a tick for a 512-element cage, and it showed up as about
// 370 us a tick in the cost grid, a quarter of the whole step. With a tolerance the same press
// exits in 2 sweeps on ADR-0029's default cage and 7 to 15 on the wider ones, and holds the same
// 1.5000 stretch ratio either way.
//
// One part in a thousand is the number because it is what the module's strain test asserts, so
// the guarantee the code makes and the guarantee the test checks are the same sentence.
constexpr f32 k_strain_clamp_tolerance = 1.0e-3f;
// The bound for a cage whose limit is fighting its own geometry — a limit tighter than the press
// it is under, where no pose satisfies both — so that costs a bounded amount rather than spinning.
// `SoftBodyBudget::strain_clamp_saturated` says when it is being reached.
constexpr u32 k_strain_clamp_sweeps = 16;

}  // namespace

Status World::create_soft_body(const SoftBodyDesc& desc, SoftBodyId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  const u32 vertex_count = static_cast<u32>(desc.vertices.size());
  if (vertex_count < 2) return Status::InvalidArgument;
  if (desc.layer >= Layer::Count) return Status::InvalidArgument;
  if (!desc.inverse_masses.empty() && desc.inverse_masses.size() != desc.vertices.size())
    return Status::InvalidArgument;
  if (desc.faces.size() % 3 != 0) return Status::InvalidArgument;
  if (!indices_in_range(desc.faces, vertex_count)) return Status::InvalidArgument;
  if (desc.iterations == 0) return Status::InvalidArgument;
  // A negative limit is not "no limit", it is a typo. One at or above 1 is refused because the
  // clamp is symmetric in engineering strain — the compression side is the half that stops a cell
  // inverting — and a limit of 1 makes the lower bound a length of zero, which is no bound at
  // all. Plan 07 §7.10's default is 0.5.
  if (desc.max_strain < 0.0f || desc.max_strain >= 1.0f) return Status::InvalidArgument;
  for (const SoftEdge& edge : desc.edges)
    if (edge.a >= vertex_count || edge.b >= vertex_count || edge.a == edge.b)
      return Status::InvalidArgument;
  for (const SoftVolumeConstraint& volume : desc.volumes)
    for (const u32 index : volume.vertex)
      if (index >= vertex_count) return Status::InvalidArgument;
  for (const SoftAttachment& attachment : desc.attachments) {
    if (attachment.vertex >= vertex_count) return Status::InvalidArgument;
    if (!attachment.body.is_null() && impl_->body_entry(attachment.body) == nullptr)
      return Status::NotFound;
    // A spring with no rate never moves towards its anchor, so it is a free particle wearing
    // an attachment's name. Refusing beats behaving as if the attachment were not there.
    if (attachment.kind == AttachmentKind::Spring && !(attachment.follow_rate > 0.0f))
      return Status::InvalidArgument;
  }
  // Pressure needs a closed surface to measure a volume over; without faces it is a silent
  // no-op, which is worse than a refusal.
  if (desc.pressure != 0.0f && desc.faces.empty()) return Status::InvalidArgument;

  JPH::Ref<SharedSettings> shared = new SharedSettings();
  shared->mVertices.reserve(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i) {
    const Vec3 p = desc.vertices[i];
    const f32 inverse_mass = desc.inverse_masses.empty() ? 1.0f : desc.inverse_masses[i];
    shared->mVertices.push_back(
        SharedSettings::Vertex(JPH::Float3(p.x, p.y, p.z), JPH::Float3(0, 0, 0), inverse_mass));
  }
  // A rigidly attached particle is kinematic to the solver; the step drives it from its
  // anchor. A spring-attached one keeps its mass, so the solver, contact, and the cage can all
  // still move it — that is the whole difference between the two kinds. This has to happen
  // before Optimize(), which sorts each group's constraints by distance to the nearest
  // kinematic vertex and would otherwise sort against the wrong set.
  for (const SoftAttachment& attachment : desc.attachments)
    if (attachment.kind == AttachmentKind::Rigid)
      shared->mVertices[attachment.vertex].mInvMass = 0.0f;

  shared->mEdgeConstraints.reserve(desc.edges.size());
  for (const SoftEdge& edge : desc.edges)
    shared->mEdgeConstraints.push_back(SharedSettings::Edge(edge.a, edge.b, edge.compliance));
  shared->mVolumeConstraints.reserve(desc.volumes.size());
  for (const SoftVolumeConstraint& volume : desc.volumes) {
    shared->mVolumeConstraints.push_back(SharedSettings::Volume(
        volume.vertex[0], volume.vertex[1], volume.vertex[2], volume.vertex[3], volume.compliance));
  }
  shared->mFaces.reserve(desc.faces.size() / 3);
  for (usize i = 0; i + 2 < desc.faces.size(); i += 3) {
    const SharedSettings::Face face(desc.faces[i], desc.faces[i + 1], desc.faces[i + 2], 0);
    if (face.IsDegenerate()) return Status::InvalidArgument;
    shared->AddFace(face);
  }

  // Rest lengths and rest volumes come from the positions the caller handed us, so the cage is
  // at rest in its authored pose whatever that pose is.
  shared->CalculateEdgeLengths();
  shared->CalculateVolumeConstraintVolumes();
  // Groups constraints so they can be solved in parallel, and is not optional: the backend
  // asserts on a settings object that has no update groups, and without them the whole cage
  // would be one serial group. It reorders constraints but never vertices (Jolt says so where
  // it sorts them: reordering vertices "would be much more of a burden to the end user"),
  // which is why attachment, face, and read_soft_body_vertices indices stay valid across it.
  //
  // The grouping is a greedy spatial partition into batches of at most
  // SoftBodyUpdateContext::cVertexConstraintBatch vertices, plus one trailing group for the
  // constraints that straddle two batches. So the width of the constraint solve is
  // ceil(vertices / batch) and not the worker count, which is the finding E19 rests on and
  // what `soft_body_solve_width` reports. The group array itself is private to the backend,
  // so the batch size is mirrored in soft_body.h and pinned here instead.
  shared->Optimize();

  JPH::SoftBodyCreationSettings settings(shared, to_jph(desc.transform.position),
                                         to_jph(desc.transform.rotation),
                                         to_object_layer(desc.layer));
  settings.mNumIterations = desc.iterations;
  settings.mPressure = desc.pressure;
  settings.mLinearDamping = desc.linear_damping;
  settings.mFriction = desc.friction;
  settings.mRestitution = desc.restitution;
  settings.mVertexRadius = desc.vertex_radius;
  settings.mGravityFactor = desc.gravity_factor;
  settings.mAllowSleeping = desc.allow_sleeping;
  settings.mUpdatePosition = desc.update_position;

  const JPH::BodyID id =
      impl_->system.GetBodyInterface().CreateAndAddSoftBody(settings, JPH::EActivation::Activate);
  if (id.IsInvalid()) return Status::LimitReached;

  Impl::SoftEntry entry;
  entry.id = id;
  entry.vertex_count = vertex_count;
  entry.iterations = desc.iterations;
  entry.max_strain = desc.max_strain;
  entry.hero = desc.hero;
  entry.attachments.append(desc.attachments);
  out = SoftBodyId{impl_->soft_bodies.insert(std::move(entry))};
  return Status::Ok;
}

// --- the strain clamp (ADR-0029 decision 4) ---------------------------------------------------
//
// **Where it lives, and why there.** After `PhysicsSystem::Update` has returned, on the stepping
// thread, over the backend's own edge list. Three alternatives were on the table and each is
// worse:
//
//   *Inside the solver, as another constraint.* It is not a constraint — a constraint is a thing
//   the solver negotiates with compliance, and a limit is a thing that is true when the step is
//   over. Expressing it as a stiff edge constraint is what the cage already has, and E19 shows
//   it losing the argument to a kinematic plate 1.8 to 2.4 times over.
//   *Per sub-step.* The backend runs its sub-steps inside one `Update` call and there is no hook
//   between them without patching Jolt. Per step at 60 Hz turns out to be enough, because what
//   the clamp has to stop is an element being left somewhere it cannot come back from, and an
//   element cannot get far in one step from a pose that was already inside the limit.
//   *On velocities only.* A velocity limit bounds how fast an element leaves but not where it
//   ends up, and the failure E19 recorded is a position — an inverted tetrahedron whose volume
//   constraint then pushes it further inside out.
//
// **Position-only, and velocity-consistent at the step's own dt.** The pass moves positions and
// then makes the velocity agree with the move, `v += dx / dt`, so the next step integrates from a
// pose and a velocity that describe the same motion instead of spending the stretch's energy on
// re-stretching the same edge.
//
// `dt` is the *step*, not the backend's sub-step, and that is a measured choice rather than an
// obvious one. Jolt's soft body closes each of its XPBD sub-steps with `v = (x - x_prev)/dt_sub`,
// where `dt_sub = dt / (collision sub-steps x iterations)` — `mNumIterations` is a sub-step count,
// not a Gauss-Seidel iteration count — so "match what the solver does" argues for `dt_sub`. But
// this pass is not inside a sub-step: it runs once a step, and the error it removes is what a
// whole step accumulated, so the interval that correction belongs to is the step.
//
// All three were run on E19's press grid (docs/experiments/e19-lattice-cage.md). At the step's
// `dt` and with no velocity correction at all, nineteen of twenty configurations recover to 1.000
// of rest volume and the results are indistinguishable. At `dt_sub` the correction is up to 32
// times larger, overshoots, and 343 elements at sixteen iterations and two sub-steps goes from
// recovering to diverging. So: correct the velocity, at the step's dt.
//
// **Symmetric in strain: the compression half is not optional.** A limit of 0.5 is a length
// between half and one and a half times rest, and the lower bound is the half that stops a
// tetrahedron going inside out, which is the mechanism behind every failure E19 recorded. It was
// measured against a stretch-only clamp on the whole press grid: stretch-only is cheaper (two to
// three sweeps against seven to fifteen) and recovers the same eighteen of twenty configurations,
// but it lets **one to twenty-one** free particles through the rigid core against the symmetric
// clamp's **one to four** — and "no element passes through the core" is one of E19's four pass
// criteria while sweep count is not.
//
// **Cost.** One sweep is two loads, a square root and about a dozen flops per edge. The sweep
// count is what a cage's own geometry and load decide, and it is reported per step
// (`SoftBodyBudget::strain_clamp_sweeps`) rather than assumed: under E19's press it is **2 for
// ADR-0029's 216-element default cage and 7 to 15 at 343 to 729 elements**, which is one more
// reason the default is one solve group wide. See docs/experiments/e19-lattice-cage.md.
//
// The rest lengths come from `SoftBodySharedSettings::mEdgeConstraints`, the backend's own,
// rather than from a copy kept beside them: a copy would be 16 bytes an edge of duplicate state
// that could drift from what the solver actually used, and the settings object is shared by every
// instance of the cage, so reading it costs nothing per body.
void World::Impl::clamp_soft_body_strain(f32 dt_seconds) {
  const f32 inv_dt = 1.0f / dt_seconds;
  for (const SoftEntry& entry : soft_bodies.values()) {
    if (!(entry.max_strain > 0.0f)) continue;
    JPH::BodyLockWrite lock(system.GetBodyLockInterfaceNoLock(), entry.id);
    if (!lock.Succeeded()) continue;
    JPH::Body& body = lock.GetBody();
    auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    const SharedSettings* settings = motion->GetSettings();
    if (settings == nullptr) continue;

    const f32 high = 1.0f + entry.max_strain;
    const f32 low = 1.0f - entry.max_strain;
    u32 sweeps_used = 0;
    for (u32 sweep = 0; sweep < k_strain_clamp_sweeps; ++sweep) {
      ++sweeps_used;
      f32 worst = 0.0f;
      for (const SharedSettings::Edge& edge : settings->mEdgeConstraints) {
        JPH::SoftBodyVertex& a = motion->GetVertex(edge.mVertex[0]);
        JPH::SoftBodyVertex& b = motion->GetVertex(edge.mVertex[1]);
        const f32 inverse_mass_sum = a.mInvMass + b.mInvMass;
        // Two pinned or rigidly attached particles: there is nothing this pass may move, and the
        // cage's own topology is what has to change if that edge is out of range.
        if (!(inverse_mass_sum > 0.0f)) continue;
        const JPH::Vec3 delta = b.mPosition - a.mPosition;
        const f32 separation = delta.Length();
        if (!(separation > 0.0f)) continue;
        const f32 upper = edge.mRestLength * high;
        const f32 lower = edge.mRestLength * low;
        f32 target = separation;
        if (separation > upper) target = upper;
        if (separation < lower) target = lower;
        if (!(target < separation) && !(target > separation)) continue;
        // How far outside the limit, as a fraction of the edge's own rest length, so a cage of
        // mixed cell sizes is judged by the same number everywhere.
        const f32 excess =
            (separation > target ? separation - target : target - separation) / edge.mRestLength;
        if (excess > worst) worst = excess;
        // Move the two ends together (or apart) by exactly the excess, split by inverse mass, so
        // the pair's centre of mass does not move and a heavy element is not dragged by a light
        // one.
        const JPH::Vec3 correction =
            delta * ((separation - target) / (separation * inverse_mass_sum));
        const JPH::Vec3 move_a = correction * a.mInvMass;
        const JPH::Vec3 move_b = correction * -b.mInvMass;
        a.mPosition += move_a;
        b.mPosition += move_b;
        a.mVelocity += move_a * inv_dt;
        b.mVelocity += move_b * inv_dt;
      }
      if (worst <= k_strain_clamp_tolerance) break;
    }
    if (sweeps_used > soft_body_budget.strain_clamp_sweeps)
      soft_body_budget.strain_clamp_sweeps = sweeps_used;
    if (sweeps_used == k_strain_clamp_sweeps) soft_body_budget.strain_clamp_saturated = true;
  }
}

bool World::destroy_soft_body(SoftBodyId body) {
  if (impl_ == nullptr) return false;
  const Impl::SoftEntry* entry = impl_->soft_bodies.get(body.handle);
  if (entry == nullptr) return false;
  JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
  bodies.RemoveBody(entry->id);
  bodies.DestroyBody(entry->id);
  impl_->soft_bodies.erase(body.handle);
  return true;
}

bool World::contains(SoftBodyId body) const noexcept {
  return impl_ != nullptr && impl_->soft_bodies.contains(body.handle);
}

u32 World::soft_body_vertex_count(SoftBodyId body) const {
  if (impl_ == nullptr) return 0;
  const Impl::SoftEntry* entry = impl_->soft_bodies.get(body.handle);
  return entry != nullptr ? entry->vertex_count : 0;
}

u32 World::read_soft_body_vertices(SoftBodyId body, std::span<Vec3> out) const {
  if (impl_ == nullptr) return 0;
  const Impl::SoftEntry* entry = impl_->soft_bodies.get(body.handle);
  if (entry == nullptr) return 0;
  const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), entry->id);
  if (!lock.Succeeded()) return 0;
  const JPH::Body& jph_body = lock.GetBody();
  const auto* motion =
      static_cast<const JPH::SoftBodyMotionProperties*>(jph_body.GetMotionProperties());
  // Particles live in the body's centre-of-mass frame; the world positions the renderer and
  // the tests want are one transform away.
  const JPH::RMat44 to_world = jph_body.GetCenterOfMassTransform();
  const u32 available = static_cast<u32>(motion->GetVertices().size());
  const u32 count =
      static_cast<u32>(out.size()) < available ? static_cast<u32>(out.size()) : available;
  for (u32 i = 0; i < count; ++i)
    out[i] = from_jph(to_world * motion->GetVertex(i).mPosition);
  return count;
}

bool World::soft_body_bounds(SoftBodyId body, Aabb3& out) const {
  if (impl_ == nullptr) return false;
  const Impl::SoftEntry* entry = impl_->soft_bodies.get(body.handle);
  if (entry == nullptr) return false;
  const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), entry->id);
  if (!lock.Succeeded()) return false;
  const JPH::AABox box = lock.GetBody().GetWorldSpaceBounds();
  out.min = from_jph(box.mMin);
  out.max = from_jph(box.mMax);
  return true;
}

bool World::soft_body_active(SoftBodyId body) const {
  if (impl_ == nullptr) return false;
  const Impl::SoftEntry* entry = impl_->soft_bodies.get(body.handle);
  if (entry == nullptr) return false;
  return impl_->system.GetBodyInterfaceNoLock().IsActive(entry->id);
}

// --- fixtures --------------------------------------------------------------------------------

ClothSheet build_cloth_sheet(u32 columns, u32 rows, f32 spacing, f32 compliance,
                             f32 shear_compliance) {
  ClothSheet sheet;
  if (columns < 2 || rows < 2 || !(spacing > 0.0f)) return sheet;
  sheet.columns = columns;
  sheet.rows = rows;

  const f32 x0 = -0.5f * static_cast<f32>(columns - 1) * spacing;
  const f32 z0 = -0.5f * static_cast<f32>(rows - 1) * spacing;
  sheet.vertices.reserve(columns * rows);
  sheet.inverse_masses.reserve(columns * rows);
  for (u32 z = 0; z < rows; ++z) {
    for (u32 x = 0; x < columns; ++x) {
      sheet.vertices.push_back(
          Vec3(x0 + static_cast<f32>(x) * spacing, 0.0f, z0 + static_cast<f32>(z) * spacing));
      sheet.inverse_masses.push_back(1.0f);
    }
  }

  // Structural edges along both axes, then one shear edge across each diagonal of every quad.
  // Without the shear edges a grid of springs has no resistance to being sheared into a
  // rhombus and the sheet folds along its diagonals instead of sagging.
  for (u32 z = 0; z < rows; ++z)
    for (u32 x = 0; x + 1 < columns; ++x)
      sheet.edges.push_back(SoftEdge{sheet.index(x, z), sheet.index(x + 1, z), compliance});
  for (u32 z = 0; z + 1 < rows; ++z)
    for (u32 x = 0; x < columns; ++x)
      sheet.edges.push_back(SoftEdge{sheet.index(x, z), sheet.index(x, z + 1), compliance});
  for (u32 z = 0; z + 1 < rows; ++z) {
    for (u32 x = 0; x + 1 < columns; ++x) {
      sheet.edges.push_back(
          SoftEdge{sheet.index(x, z), sheet.index(x + 1, z + 1), shear_compliance});
      sheet.edges.push_back(
          SoftEdge{sheet.index(x + 1, z), sheet.index(x, z + 1), shear_compliance});
    }
  }

  sheet.faces.reserve((columns - 1) * (rows - 1) * 6);
  for (u32 z = 0; z + 1 < rows; ++z) {
    for (u32 x = 0; x + 1 < columns; ++x) {
      const u32 a = sheet.index(x, z);
      const u32 b = sheet.index(x + 1, z);
      const u32 c = sheet.index(x + 1, z + 1);
      const u32 d = sheet.index(x, z + 1);
      sheet.faces.push_back(a);
      sheet.faces.push_back(c);
      sheet.faces.push_back(b);
      sheet.faces.push_back(a);
      sheet.faces.push_back(d);
      sheet.faces.push_back(c);
    }
  }
  return sheet;
}

LatticeVolume build_lattice_volume(u32 n, f32 spacing, f32 edge_compliance, f32 volume_compliance) {
  LatticeVolume lattice;
  if (n < 2 || !(spacing > 0.0f)) return lattice;
  lattice.n = n;

  const f32 origin = -0.5f * static_cast<f32>(n - 1) * spacing;
  lattice.vertices.reserve(n * n * n);
  lattice.inverse_masses.reserve(n * n * n);
  for (u32 z = 0; z < n; ++z) {
    for (u32 y = 0; y < n; ++y) {
      for (u32 x = 0; x < n; ++x) {
        lattice.vertices.push_back(Vec3(origin + static_cast<f32>(x) * spacing,
                                        origin + static_cast<f32>(y) * spacing,
                                        origin + static_cast<f32>(z) * spacing));
        lattice.inverse_masses.push_back(1.0f);
      }
    }
  }

  const auto add_edge = [&lattice, edge_compliance](u32 a, u32 b) {
    lattice.edges.push_back(SoftEdge{a, b, edge_compliance});
  };

  // Axis edges. Generated once per direction, so no pair appears twice.
  for (u32 z = 0; z < n; ++z)
    for (u32 y = 0; y < n; ++y)
      for (u32 x = 0; x + 1 < n; ++x)
        add_edge(lattice.index(x, y, z), lattice.index(x + 1, y, z));
  for (u32 z = 0; z < n; ++z)
    for (u32 y = 0; y + 1 < n; ++y)
      for (u32 x = 0; x < n; ++x)
        add_edge(lattice.index(x, y, z), lattice.index(x, y + 1, z));
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 y = 0; y < n; ++y)
      for (u32 x = 0; x < n; ++x)
        add_edge(lattice.index(x, y, z), lattice.index(x, y, z + 1));

  // Face diagonals, one plane at a time, again without duplicates: a face shared by two cells
  // is visited once because the loop is over planes, not over cells.
  for (u32 z = 0; z < n; ++z)
    for (u32 y = 0; y + 1 < n; ++y)
      for (u32 x = 0; x + 1 < n; ++x) {
        add_edge(lattice.index(x, y, z), lattice.index(x + 1, y + 1, z));
        add_edge(lattice.index(x + 1, y, z), lattice.index(x, y + 1, z));
      }
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 y = 0; y + 1 < n; ++y)
      for (u32 x = 0; x < n; ++x) {
        add_edge(lattice.index(x, y, z), lattice.index(x, y + 1, z + 1));
        add_edge(lattice.index(x, y + 1, z), lattice.index(x, y, z + 1));
      }
  for (u32 z = 0; z + 1 < n; ++z)
    for (u32 y = 0; y < n; ++y)
      for (u32 x = 0; x + 1 < n; ++x) {
        add_edge(lattice.index(x, y, z), lattice.index(x + 1, y, z + 1));
        add_edge(lattice.index(x + 1, y, z), lattice.index(x, y, z + 1));
      }

  // Body diagonals and the five tetrahedra of each cell. Edges alone let a cube fold; the
  // volume constraints are what make it spring back from a press, which is the property
  // ADR-0026 cares about.
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 y = 0; y + 1 < n; ++y) {
      for (u32 x = 0; x + 1 < n; ++x) {
        const u32 v000 = lattice.index(x, y, z);
        const u32 v100 = lattice.index(x + 1, y, z);
        const u32 v010 = lattice.index(x, y + 1, z);
        const u32 v110 = lattice.index(x + 1, y + 1, z);
        const u32 v001 = lattice.index(x, y, z + 1);
        const u32 v101 = lattice.index(x + 1, y, z + 1);
        const u32 v011 = lattice.index(x, y + 1, z + 1);
        const u32 v111 = lattice.index(x + 1, y + 1, z + 1);
        add_edge(v000, v111);
        add_edge(v100, v011);
        add_edge(v010, v101);
        add_edge(v110, v001);
        // Six tetrahedra sharing the (0,0,0)-(1,1,1) diagonal: the Kuhn decomposition, and the
        // same one Jolt's own cube fixture uses. The five-tetrahedron alternative needs its
        // orientation flipped in every other cell, and a cell whose orientation is wrong
        // inverts under compression and pushes the cage inside out instead of back.
        const u32 tets[6][4] = {{v000, v011, v001, v111}, {v000, v010, v011, v111},
                                {v000, v001, v101, v111}, {v000, v101, v100, v111},
                                {v000, v110, v010, v111}, {v000, v100, v110, v111}};
        for (const auto& tet : tets) {
          SoftVolumeConstraint volume;
          volume.vertex[0] = tet[0];
          volume.vertex[1] = tet[1];
          volume.vertex[2] = tet[2];
          volume.vertex[3] = tet[3];
          volume.compliance = volume_compliance;
          lattice.volumes.push_back(volume);
        }
      }
    }
  }

  // The six outer faces, wound outwards, so a pressure constraint and a ray cast against the
  // cage both see a closed surface.
  const u32 last = n - 1;
  const auto quad = [&lattice](u32 a, u32 b, u32 c, u32 d) {
    lattice.faces.push_back(a);
    lattice.faces.push_back(b);
    lattice.faces.push_back(c);
    lattice.faces.push_back(a);
    lattice.faces.push_back(c);
    lattice.faces.push_back(d);
  };
  for (u32 j = 0; j + 1 < n; ++j) {
    for (u32 i = 0; i + 1 < n; ++i) {
      // -Z and +Z
      quad(lattice.index(i, j, 0), lattice.index(i, j + 1, 0), lattice.index(i + 1, j + 1, 0),
           lattice.index(i + 1, j, 0));
      quad(lattice.index(i, j, last), lattice.index(i + 1, j, last),
           lattice.index(i + 1, j + 1, last), lattice.index(i, j + 1, last));
      // -X and +X
      quad(lattice.index(0, i, j), lattice.index(0, i, j + 1), lattice.index(0, i + 1, j + 1),
           lattice.index(0, i + 1, j));
      quad(lattice.index(last, i, j), lattice.index(last, i + 1, j),
           lattice.index(last, i + 1, j + 1), lattice.index(last, i, j + 1));
      // -Y and +Y
      quad(lattice.index(i, 0, j), lattice.index(i + 1, 0, j), lattice.index(i + 1, 0, j + 1),
           lattice.index(i, 0, j + 1));
      quad(lattice.index(i, last, j), lattice.index(i, last, j + 1),
           lattice.index(i + 1, last, j + 1), lattice.index(i + 1, last, j));
    }
  }
  return lattice;
}

}  // namespace engine::physics
