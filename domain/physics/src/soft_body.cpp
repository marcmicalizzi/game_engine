// Soft bodies, and the two cage builders the tests and the E19 precursor need.

#include "world_impl.h"

#include <core/log/log.h>

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <utility>

namespace engine::physics {

namespace {

using SharedSettings = JPH::SoftBodySharedSettings;

bool indices_in_range(std::span<const u32> indices, u32 count) noexcept {
  for (const u32 index : indices)
    if (index >= count) return false;
  return true;
}

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
  // An attached particle is kinematic to the solver; the step drives it from its anchor.
  for (const SoftAttachment& attachment : desc.attachments)
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
  // Groups constraints so they can be solved in parallel. It reorders constraints but never
  // vertices, which is why attachment indices stay valid across it.
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
  entry.attachments.append(desc.attachments);
  out = SoftBodyId{impl_->soft_bodies.insert(std::move(entry))};
  return Status::Ok;
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
