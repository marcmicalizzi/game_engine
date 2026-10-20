// The flora stage of the deformed-vertex pool (docs/subsystems/renderer.md, "The flora stage";
// roadmap S66). A two-part cylinder — a trunk from y = 0 to 1 and a child from 1 to 2 hanging from
// it — is bent on the GPU by `deform.slang`'s flora stage and compared, slot by slot, against
// `gfx::flora_bend` on the CPU: the same poses, the same motion records, the same rest positions
// off the mesh's 16-bit grid. The renderer composes the poses (its flora_motion.h); this case holds
// the shader to the arithmetic a pose means, and holds a pose at rest to the rest pose's own bits.
#include "raster_path.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/flora_motion.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/deform.spv.h>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_rings = 17;
constexpr u32 k_segments = 12;
constexpr f32 k_height = 2.0f;
constexpr f32 k_radius = 0.2f;

void make_cylinder(Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 ring = 0; ring < k_rings; ++ring) {
    const f32 y = k_height * static_cast<f32>(ring) / static_cast<f32>(k_rings - 1);
    for (u32 segment = 0; segment < k_segments; ++segment) {
      const f32 angle = 6.2831853f * static_cast<f32>(segment) / static_cast<f32>(k_segments);
      positions.push_back(Vec3{k_radius * std::cos(angle), y, k_radius * std::sin(angle)});
    }
  }
  for (u32 ring = 0; ring + 1 < k_rings; ++ring) {
    for (u32 segment = 0; segment < k_segments; ++segment) {
      const u32 next = (segment + 1) % k_segments;
      const u32 a = ring * k_segments + segment;
      const u32 b = ring * k_segments + next;
      const u32 c = (ring + 1) * k_segments + segment;
      const u32 d = (ring + 1) * k_segments + next;
      indices.push_back(a);
      indices.push_back(c);
      indices.push_back(b);
      indices.push_back(b);
      indices.push_back(c);
      indices.push_back(d);
    }
  }
}

// The motion record of a rest position: the trunk below y = 1 with its weight y, the child above
// with its weight y - 1 — what a species' solver emits, by arc length along the part.
u32 motion_record(Vec3 p) {
  const u32 part = p.y < 1.0f ? 0u : 1u;
  const f32 w = std::fmin(1.0f, std::fmax(0.0f, part == 0 ? p.y : p.y - 1.0f));
  const u32 weight = static_cast<u32>(std::lround(w * 65535.0f));
  return part | (weight << 16);
}
f32 record_weight(u32 record) { return static_cast<f32>(record >> 16) * (1.0f / 65535.0f); }

// The trunk bent `trunk` radians about +z through its foot, the child carried by the trunk's bend
// at its pivot (weight 1: the trunk's tip) and bent `child` radians about +x through its pivot.
void two_part_poses(f32 trunk, f32 child, gfx::FloraPartPose (&out)[2]) {
  out[0] = gfx::FloraPartPose{};
  out[0].pivot_angle = Vec4{0.0f, 0.0f, 0.0f, trunk};
  out[0].axis = Vec4{0.0f, 0.0f, 1.0f, trunk != 0.0f ? 1.0f : 0.0f};
  out[1] = gfx::FloraPartPose{};
  const f32 c = std::cos(trunk);
  const f32 s = std::sin(trunk);
  // Rotation about +z by `trunk` through the origin, as rows.
  out[1].rows[0] = Vec4{c, -s, 0.0f, 0.0f};
  out[1].rows[1] = Vec4{s, c, 0.0f, 0.0f};
  out[1].rows[2] = Vec4{0.0f, 0.0f, 1.0f, 0.0f};
  out[1].pivot_angle = Vec4{0.0f, 1.0f, 0.0f, child};
  out[1].axis = Vec4{1.0f, 0.0f, 0.0f, (trunk != 0.0f || child != 0.0f) ? 1.0f : 0.0f};
}

struct FloraScene {
  geometry::ClusterMesh mesh;
  u32 pool_vertices = 0;
  Vector<u32> records;
  gfx::BufferResource clusters, quantized, attributes, motion, poses, meshes, instances,
      deform_table, pool, normals, slots, count_buffer, visible, host, host_normals;

  bool create(const gfx::Device& device, std::string* error) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_cylinder(positions, indices);
    if (!geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, error))
      return false;
    pool_vertices = mesh.vertices.size();
    records.resize(pool_vertices, 0u);
    for (u32 v = 0; v < pool_vertices; ++v)
      records[v] = motion_record(geometry::dequantize_position(mesh, v));
    constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
    const gfx::BufferUsage k_pool = k_storage | gfx::BufferUsage::ShaderDeviceAddress |
                                    gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
    const u64 pool_bytes = u64{pool_vertices} * 3 * sizeof(f32);
    gfx::DeformDesc desc{};
    desc.stages = gfx::k_deform_stage_flora;
    desc.joint_count = 2;  // the part count
    desc.first_part = 0;
    gfx::InstanceDesc instance;
    gfx::set_instance_transform(instance, Mat4::identity());
    instance.deform = 0;
    Vector<u32> entries;
    Vector<u32> slot_table;
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
      entries.push_back(0);
      entries.push_back(c);
      slot_table.push_back(mesh.clusters[c].vertex_offset);
    }
    const u32 count = mesh.clusters.size();
    if (!gfx::upload_buffer(device, mesh.clusters.data(),
                            mesh.clusters.size() * sizeof(geometry::ClusterDesc), k_storage,
                            clusters, error) ||
        !gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                            k_storage, quantized, error) ||
        !gfx::upload_buffer(device, mesh.attributes.data(),
                            mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                            attributes, error) ||
        !gfx::upload_buffer(device, records.data(), records.size() * sizeof(u32), k_storage, motion,
                            error) ||
        !gfx::create_buffer(device, sizeof(gfx::FloraPartPose) * 2,
                            k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, poses,
                            error) ||
        !gfx::create_buffer(device, pool_bytes, k_pool, false, pool, error) ||
        !gfx::create_buffer(device, u64{pool_vertices} * sizeof(u32), k_pool, false, normals,
                            error) ||
        !gfx::create_buffer(device, pool_bytes, gfx::BufferUsage::TransferDst, true, host, error) ||
        !gfx::create_buffer(device, u64{pool_vertices} * sizeof(u32), gfx::BufferUsage::TransferDst,
                            true, host_normals, error) ||
        !gfx::upload_buffer(device, &desc, sizeof(desc), k_storage, deform_table, error) ||
        !gfx::upload_buffer(device, &instance, sizeof(instance), k_storage, instances, error) ||
        !gfx::upload_buffer(device, &count, sizeof(count), k_storage, count_buffer, error) ||
        !gfx::upload_buffer(device, slot_table.data(), slot_table.size() * sizeof(u32), k_storage,
                            slots, error) ||
        !gfx::upload_buffer(device, entries.data(), entries.size() * sizeof(u32), k_storage,
                            visible, error)) {
      return false;
    }
    gfx::MeshDesc mesh_desc{};
    mesh_desc.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
    mesh_desc.quantized = quantized.address;
    mesh_desc.cluster_count = mesh.clusters.size();
    mesh_desc.deform_pool = pool.address;
    mesh_desc.deform_slots = slots.address;
    return gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, error);
  }

  gfx::DeformParams params() const noexcept {
    gfx::DeformParams p{};
    p.clusters = clusters.address;
    p.instances = instances.address;
    p.meshes = meshes.address;
    p.attributes = attributes.address;
    p.visible = visible.address;
    p.visible_count = count_buffer.address;
    p.pool = pool.address;
    p.deform = deform_table.address;
    p.slots = slots.address;
    p.normal_pool = normals.address;
    p.max_entries = mesh.clusters.size();
    p.motion = motion.address;
    p.flora = poses.address;
    return p;
  }

  void destroy(const gfx::Device& device) noexcept {
    for (gfx::BufferResource* b :
         {&clusters, &quantized, &attributes, &motion, &poses, &meshes, &instances, &deform_table,
          &pool, &normals, &slots, &count_buffer, &visible, &host, &host_normals}) {
      gfx::destroy_buffer(device, *b);
    }
  }
};

Vec3 decode_oct(u32 packed) {
  const f32 x = static_cast<f32>(static_cast<i16>(packed & 0xFFFFu)) / 32767.0f;
  const f32 y = static_cast<f32>(static_cast<i16>(packed >> 16)) / 32767.0f;
  Vec3 n{x, y, 1.0f - std::fabs(x) - std::fabs(y)};
  if (n.z < 0.0f) {
    n.x = (1.0f - std::fabs(y)) * (x < 0.0f ? -1.0f : 1.0f);
    n.y = (1.0f - std::fabs(x)) * (y < 0.0f ? -1.0f : 1.0f);
  }
  return n * (1.0f / length(n));
}

}  // namespace

TEST_CASE("deform: the flora stage bends a part chain as the CPU reference does") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  FloraScene scene;
  REQUIRE_MESSAGE(scene.create(device, &error), error);

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 1, &error));
  gfx::ShaderModuleHandle module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                             shaders::k_deform_spirv_size, &error);
  REQUIRE(module.valid());
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "deform_main", {},
                                               sizeof(gfx::DeformParams), pipeline, &error),
                  error);
  const gfx::DeformParams params = scene.params();
  const u32 groups = scene.mesh.clusters.size();
  auto run = [&](const gfx::FloraPartPose(&poses)[2]) {
    std::memcpy(scene.poses.mapped, poses, sizeof(poses));
    gfx::RenderGraph graph(device);
    const gfx::RgBuffer rg_pool = graph.import_buffer("pool", scene.pool);
    const gfx::RgBuffer rg_normals = graph.import_buffer("normals", scene.normals);
    const gfx::RgBuffer rg_host = graph.import_buffer("host", scene.host);
    const gfx::RgBuffer rg_host_normals = graph.import_buffer("host_normals", scene.host_normals);
    graph.add_pass(
        "clear", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.write(rg_pool, gfx::Access::TransferWrite);
          b.write(rg_normals, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.fill_buffer(scene.pool.buffer, 0, gfx::k_whole_size, 0x7fc00000u);
          cb.fill_buffer(scene.normals.buffer, 0, gfx::k_whole_size, 0u);
        });
    graph.add_pass(
        "flora", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(rg_pool, gfx::Access::ComputeWrite);
          b.write(rg_normals, gfx::Access::ComputeWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
          cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
          cb.dispatch(groups, 1, 1);
        });
    graph.add_pass(
        "readback", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_pool, gfx::Access::TransferRead);
          b.read(rg_normals, gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
          b.write(rg_host_normals, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.copy_buffer(scene.pool.buffer, scene.host.buffer,
                         gfx::BufferCopy{0, 0, u64{scene.pool_vertices} * 3 * sizeof(f32)});
          cb.copy_buffer(scene.normals.buffer, scene.host_normals.buffer,
                         gfx::BufferCopy{0, 0, u64{scene.pool_vertices} * sizeof(u32)});
        });
    REQUIRE_MESSAGE(graph.compile(&error), error);
    gfx::CommandList commands = frames.begin_frame();
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
    graph.reset();
  };

  const auto* pool = static_cast<const f32*>(scene.host.mapped);
  const auto* normals = static_cast<const u32*>(scene.host_normals.mapped);

  // 1. Every part at rest: the rest position off the grid and the rest normal's own packed word,
  //    to the bit — what makes a tree in no wind the rigid picture.
  gfx::FloraPartPose poses[2];
  two_part_poses(0.0f, 0.0f, poses);
  run(poses);
  u32 position_bits = 0;
  u32 normal_bits = 0;
  for (u32 v = 0; v < scene.pool_vertices; ++v) {
    const Vec3 rest = geometry::dequantize_position(scene.mesh, v);
    const f32 got[3] = {pool[v * 3], pool[v * 3 + 1], pool[v * 3 + 2]};
    const f32 want[3] = {rest.x, rest.y, rest.z};
    position_bits += std::memcmp(got, want, sizeof(got)) != 0 ? 1u : 0u;
    normal_bits += normals[v] != scene.mesh.attributes[v].normal_oct ? 1u : 0u;
  }
  CHECK(position_bits == 0);
  CHECK(normal_bits == 0);

  // 2. The trunk bent 0.25 rad about +z and the child 0.6 about +x on top of it: every vertex
  //    against `gfx::flora_bend`, positions to 2e-6 m on a 2 m mesh and normals to 2e-3 (the
  //    octahedral word's step), and the tips moved where the bends say.
  two_part_poses(0.25f, 0.6f, poses);
  run(poses);
  f32 worst = 0.0f;
  f32 worst_normal = 0.0f;
  u32 moved = 0;
  Vec3 tip_sum{};
  u32 tips = 0;
  for (u32 v = 0; v < scene.pool_vertices; ++v) {
    const Vec3 rest = geometry::dequantize_position(scene.mesh, v);
    const u32 record = scene.records[v];
    Vec3 want_p;
    Vec3 want_n;
    gfx::flora_bend(poses[record & 0xFFFFu], record_weight(record), rest,
                    decode_oct(scene.mesh.attributes[v].normal_oct), want_p, want_n);
    const Vec3 got{pool[v * 3], pool[v * 3 + 1], pool[v * 3 + 2]};
    worst = std::max(worst, length(got - want_p));
    worst_normal = std::max(worst_normal, length(decode_oct(normals[v]) - want_n));
    moved += length(want_p - rest) > 1.0e-3f ? 1u : 0u;
    if (rest.y > k_height - 1.0e-3f) {
      tip_sum = tip_sum + got;
      ++tips;
    }
  }
  MESSAGE("flora stage: worst position difference " << worst << " m, worst normal " << worst_normal
                                                    << ", " << moved << " of "
                                                    << scene.pool_vertices << " vertices moved");
  CHECK(worst < 2.0e-6f);
  CHECK(worst_normal < 2.0e-3f);
  CHECK(moved > scene.pool_vertices / 2);
  // A bend about +z by a positive angle swings +y towards -x, and the child's about +x swings it
  // towards +z: the tip ring's centre is on both sides of the axis it left.
  REQUIRE(tips > 0);
  const Vec3 tip = tip_sum * (1.0f / static_cast<f32>(tips));
  CHECK(tip.x < -0.2f);
  CHECK(tip.z > 0.4f);

  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  frames.destroy();
  scene.destroy(device);
  device.destroy();
}
