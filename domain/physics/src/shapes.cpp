// Shapes. Each one is built, checked, and then owned by the world's shape table; bodies
// reference a handle, so the same collision geometry is shared by every instance of a mesh.

#include "world_impl.h"

#include <core/log/log.h>

#include <Jolt/Core/UnorderedSet.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <utility>

namespace engine::physics {

namespace {

// Every shape arrives here. A failed build says why in the log once, at the point where the
// caller's data is still identifiable, and the caller gets a Status.
Status finish(const JPH::ShapeSettings::ShapeResult& result, const char* kind,
              JPH::RefConst<JPH::Shape>& out) {
  if (result.HasError()) {
    ENGINE_LOG_ERROR(log_physics, "shape build failed", log::field("kind", kind),
                     log::field("error", result.GetError().c_str()));
    return Status::BackendError;
  }
  // The result owns the only reference and is a temporary at every call site, so take one.
  out = result.Get().GetPtr();
  return Status::Ok;
}

f32 smallest_of(Vec3 v) noexcept {
  const f32 xy = v.x < v.y ? v.x : v.y;
  return xy < v.z ? xy : v.z;
}

}  // namespace

Status World::Impl::adopt_shape(JPH::RefConst<JPH::Shape> shape, ShapeId& out) {
  ShapeEntry entry;
  entry.shape = std::move(shape);
  out = ShapeId{shapes.insert(std::move(entry))};
  return Status::Ok;
}

Status World::create_box(Vec3 half_extent, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (!(half_extent.x > 0.0f) || !(half_extent.y > 0.0f) || !(half_extent.z > 0.0f))
    return Status::InvalidArgument;
  // Jolt rounds a box's corners by the convex radius and refuses a radius larger than the
  // smallest half extent, so a 2 cm crate does not have to know what the default was.
  const f32 quarter = smallest_of(half_extent) * 0.25f;
  const f32 radius = quarter < JPH::cDefaultConvexRadius ? quarter : JPH::cDefaultConvexRadius;
  JPH::BoxShapeSettings settings(to_jph(half_extent), radius);
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "box", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_sphere(f32 radius, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (!(radius > 0.0f)) return Status::InvalidArgument;
  JPH::SphereShapeSettings settings(radius);
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "sphere", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_capsule(f32 half_height, f32 radius, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (!(half_height > 0.0f) || !(radius > 0.0f)) return Status::InvalidArgument;
  JPH::CapsuleShapeSettings settings(half_height, radius);
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "capsule", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_convex_hull(std::span<const Vec3> points, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (points.size() < 4) return Status::InvalidArgument;  // fewer cannot enclose a volume
  JPH::Array<JPH::Vec3> jph_points;
  jph_points.reserve(points.size());
  for (const Vec3& p : points)
    jph_points.push_back(to_jph(p));
  JPH::ConvexHullShapeSettings settings(jph_points);
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "convex-hull", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_mesh(std::span<const Vec3> positions, std::span<const u32> indices,
                          ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (positions.empty() || indices.empty() || indices.size() % 3 != 0)
    return Status::InvalidArgument;
  const u32 vertex_count = static_cast<u32>(positions.size());
  for (const u32 index : indices)
    if (index >= vertex_count) return Status::InvalidArgument;

  // This is the form a cluster mesh collides in: positions plus triangles, no LOD, no
  // clusters. The renderer's quantized 16-bit grid stays on the renderer's side; physics wants
  // the float positions the acceleration-structure builders already keep (AGENTS.md).
  JPH::VertexList vertices;
  vertices.reserve(positions.size());
  for (const Vec3& p : positions)
    vertices.push_back(JPH::Float3(p.x, p.y, p.z));
  JPH::IndexedTriangleList triangles;
  triangles.reserve(indices.size() / 3);
  for (usize i = 0; i + 2 < indices.size(); i += 3)
    triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));

  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "mesh", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_heightfield(const HeightfieldDesc& desc, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  // Jolt stores the field in blocks of 2x2 samples, so the side must be even and at least two
  // blocks across. A power of two is cheapest but not required.
  if (desc.sample_count < 4 || (desc.sample_count % 2) != 0) return Status::InvalidArgument;
  const usize needed = static_cast<usize>(desc.sample_count) * desc.sample_count;
  if (desc.heights.size() != needed) return Status::InvalidArgument;
  if (!(desc.scale.x > 0.0f) || !(desc.scale.z > 0.0f)) return Status::InvalidArgument;

  JPH::HeightFieldShapeSettings settings(desc.heights.data(), to_jph(desc.local_offset),
                                         to_jph(desc.scale), desc.sample_count);
  settings.SetEmbedded();
  JPH::RefConst<JPH::Shape> shape;
  const Status status = finish(settings.Create(), "heightfield", shape);
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

Status World::create_compound(std::span<const CompoundChild> children, ShapeId& out) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  if (children.empty()) return Status::InvalidArgument;
  for (const CompoundChild& child : children)
    if (impl_->shape_ptr(child.shape) == nullptr) return Status::NotFound;

  JPH::RefConst<JPH::Shape> shape;
  Status status = Status::Ok;
  if (children.size() == 1) {
    // A compound of one is a placed shape, and Jolt's static compound refuses fewer than two
    // children, so build what the caller meant instead of refusing.
    const CompoundChild& child = children[0];
    JPH::RotatedTranslatedShapeSettings settings(to_jph(child.transform.position),
                                                 to_jph(child.transform.rotation),
                                                 impl_->shape_ptr(child.shape));
    settings.SetEmbedded();
    status = finish(settings.Create(), "compound", shape);
  } else {
    JPH::StaticCompoundShapeSettings settings;
    settings.SetEmbedded();
    for (const CompoundChild& child : children) {
      settings.AddShape(to_jph(child.transform.position), to_jph(child.transform.rotation),
                        impl_->shape_ptr(child.shape));
    }
    status = finish(settings.Create(), "compound", shape);
  }
  if (status != Status::Ok) return status;
  return impl_->adopt_shape(shape, out);
}

bool World::destroy_shape(ShapeId shape) {
  if (impl_ == nullptr) return false;
  const Impl::ShapeEntry* entry = impl_->shapes.get(shape.handle);
  if (entry == nullptr) return false;
  // A shape a body still stands on is not the caller's to delete; the reference count is what
  // turns that into a refusal instead of a dangling collision test.
  if (entry->body_refs != 0) return false;
  impl_->shapes.erase(shape.handle);
  return true;
}

bool World::shape_memory(ShapeId shape, bool children, u64& bytes, u32& triangles) const {
  if (impl_ == nullptr) return false;
  const JPH::Shape* jph_shape = impl_->shape_ptr(shape);
  if (jph_shape == nullptr) return false;
  JPH::Shape::Stats stats = jph_shape->GetStats();
  if (children) {
    JPH::Shape::VisitedShapes visited;
    stats = jph_shape->GetStatsRecursive(visited);
  }
  bytes = static_cast<u64>(stats.mSizeBytes);
  triangles = static_cast<u32>(stats.mNumTriangles);
  return true;
}

bool World::shape_bounds(ShapeId shape, Aabb3& out) const {
  if (impl_ == nullptr) return false;
  const JPH::Shape* jph_shape = impl_->shape_ptr(shape);
  if (jph_shape == nullptr) return false;
  // Jolt keeps a shape's geometry relative to its centre of mass, so its "local" bounds are
  // centred on that rather than on the origin the caller placed the points at. Adding the
  // offset back is what makes a hull built from points in [0, 1] report bounds of [0, 1].
  const JPH::AABox box = jph_shape->GetLocalBounds();
  const JPH::Vec3 centre_of_mass = jph_shape->GetCenterOfMass();
  out.min = from_jph(box.mMin + centre_of_mass);
  out.max = from_jph(box.mMax + centre_of_mass);
  return true;
}

}  // namespace engine::physics
