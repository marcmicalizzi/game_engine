// The skinning mode of the deformed-vertex pool (docs/plan/04-renderer.md §4.3,
// docs/plan/05-simulation.md §5.11). A two-bone cylinder is skinned on the GPU by `deform.slang`
// and compared, slot by slot, against `anim::skin_positions` on the CPU — the same bone matrices,
// the same bindings, the same rest positions off the mesh's 16-bit grid.
//
// The reference is the point. Every other way of checking a skinning shader (does it look right,
// does it match last week's picture) tolerates a wrong weight normalization or a transposed
// matrix; comparing against the CPU implementation the rest of the engine uses does not, and it
// is what lets a collision proxy or a headless server skin the same mesh and get the same answer.
//
// The pass itself is the one E25 built, so this case dispatches it and reads the pool back rather
// than drawing anything: the rasterizers' side of the pool is already covered by
// `deform_tests.cpp`, and nothing here needs a raster pipeline or 64-bit atomics.
#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/deform.spv.h>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_rings = 9;
constexpr u32 k_segments = 16;
constexpr f32 k_height = 2.0f;
constexpr f32 k_radius = 0.5f;

// A cylinder standing along +y, bound to two joints: all root at the base, all tip at the top,
// linear in between. Two bones and a smooth gradient is the smallest mesh on which a wrong
// weight normalization is visible — a pinch at the middle ring.
void make_cylinder(Vector<Vec3>& positions, Vector<u32>& indices,
                   Vector<geometry::SkinBinding>& skin) {
  for (u32 ring = 0; ring < k_rings; ++ring) {
    const f32 y = k_height * static_cast<f32>(ring) / static_cast<f32>(k_rings - 1);
    for (u32 segment = 0; segment < k_segments; ++segment) {
      const f32 angle = 6.2831853f * static_cast<f32>(segment) / static_cast<f32>(k_segments);
      positions.push_back(Vec3{k_radius * std::cos(angle), y, k_radius * std::sin(angle)});
      const u32 joints[4] = {0, 1, 0, 0};
      const f32 t = y / k_height;
      const f32 weights[4] = {1.0f - t, t, 0.0f, 0.0f};
      skin.push_back(geometry::make_skin_binding(joints, weights));
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

// The two-bone skeleton the cylinder is bound to: a root at the origin and a tip halfway up.
anim::Skeleton two_bone() {
  anim::Skeleton skeleton;
  skeleton.resize(2);
  skeleton.names[0] = "root";
  skeleton.names[1] = "tip";
  skeleton.parents[1] = 0;
  skeleton.local_bind[1].position = Vec3{0.0f, k_height * 0.5f, 0.0f};
  anim::compute_inverse_bind(skeleton);
  return skeleton;
}

// Everything the pass reads, and the pool it writes.
struct SkinScene {
  geometry::ClusterMesh mesh;
  u32 pool_vertices = 0;
  gfx::BufferResource clusters;
  gfx::BufferResource quantized;
  gfx::BufferResource skin;
  gfx::BufferResource meshes;
  gfx::BufferResource instances;
  gfx::BufferResource deform_table;
  gfx::BufferResource joints;
  gfx::BufferResource pool;
  gfx::BufferResource slots;  // u32 per visible entry: its block's first pool vertex
  gfx::BufferResource count_buffer;
  gfx::BufferResource visible;
  gfx::BufferResource host;

  bool create(const gfx::Device& device, u32 joint_count, std::string* error) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    Vector<geometry::SkinBinding> bindings;
    make_cylinder(positions, indices, bindings);
    geometry::AttributeSource attributes;
    attributes.skin = std::span<const geometry::SkinBinding>(bindings.data(), bindings.size());
    attributes.joint_count = 2;
    if (!geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, error,
                                  attributes)) {
      return false;
    }
    if (mesh.skin.size() != mesh.vertices.size()) {
      if (error != nullptr) *error = "the cluster builder dropped the skin bindings";
      return false;
    }
    pool_vertices = mesh.vertices.size();

    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags k_pool = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const u64 pool_bytes = u64{pool_vertices} * 3 * sizeof(f32);
    if (!gfx::upload_buffer(device, mesh.clusters.data(),
                            mesh.clusters.size() * sizeof(geometry::ClusterDesc), k_storage,
                            clusters, error) ||
        !gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                            k_storage, quantized, error) ||
        !gfx::upload_buffer(device, mesh.skin.data(),
                            mesh.skin.size() * sizeof(geometry::SkinBinding), k_storage, skin,
                            error) ||
        !gfx::create_buffer(device, pool_bytes, k_pool, false, pool, error) ||
        !gfx::create_buffer(device, pool_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host,
                            error) ||
        // Host visible, so a pose is one memcpy, and with the address bit because the pass reads
        // the matrices through a device address rather than a descriptor.
        !gfx::create_buffer(device, sizeof(anim::JointMatrix) * 2,
                            k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, joints,
                            error)) {
      return false;
    }

    gfx::DeformDesc desc{};
    desc.flags = gfx::k_deform_skin;
    desc.joint_count = joint_count;
    desc.joints = joints.address;
    gfx::InstanceDesc instance;
    gfx::set_instance_transform(instance, Mat4::identity());
    instance.deform = 0;
    Vector<u32> entries;
    // The pool is addressed per visible entry, and *this* test chooses the mesh-ordered layout:
    // entry c's block starts at cluster c's own `vertex_offset`, so a pool slot is still the
    // mesh's vertex index and the comparison against `anim::skin_positions` below stays a plain
    // walk of the mesh. A frame's allocator packs the cut instead; that it can is exactly the
    // point of the table being what decides.
    Vector<u32> slot_table;
    for (u32 c = 0; c < mesh.clusters.size(); ++c) {
      entries.push_back(0);
      entries.push_back(c);
      slot_table.push_back(mesh.clusters[c].vertex_offset);
    }
    const u32 count = mesh.clusters.size();
    if (!gfx::upload_buffer(device, &desc, sizeof(desc), k_storage, deform_table, error) ||
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
    mesh_desc.skin = skin.address;
    return gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, error);
  }

  gfx::DeformParams params() const noexcept {
    gfx::DeformParams p{};
    p.clusters = clusters.address;
    p.instances = instances.address;
    p.meshes = meshes.address;
    p.visible = visible.address;
    p.visible_count = count_buffer.address;
    p.pool = pool.address;
    p.deform = deform_table.address;
    p.slots = slots.address;
    p.max_entries = mesh.clusters.size();
    p.visible_offset = 0;
    return p;
  }

  void destroy(const gfx::Device& device) noexcept {
    for (gfx::BufferResource* b : {&clusters, &quantized, &skin, &meshes, &instances, &deform_table,
                                   &joints, &pool, &slots, &count_buffer, &visible, &host}) {
      gfx::destroy_buffer(device, *b);
    }
  }
};

}  // namespace

TEST_CASE("deform: the skinning mode matches the CPU reference on a two-bone cylinder") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }

  // `joint_count` is 2 in the first two poses and 1 in the last, which is how the out-of-range
  // clamp is exercised on both sides at once.
  SkinScene scene;
  REQUIRE_MESSAGE(scene.create(device, 2, &error), error);

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 1, &error));
  VkShaderModule module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                    shaders::k_deform_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "deform_main", {},
                                               sizeof(gfx::DeformParams), pipeline, &error),
                  error);

  // The rest positions the shader reads are the *dequantized* ones, so the reference reads them
  // the same way: a difference of half a grid step here would swamp the 1e-5 the skinning is
  // being held to.
  const anim::Skeleton skeleton = two_bone();
  Vector<Vec3> rest(scene.pool_vertices, Vec3{});
  for (u32 v = 0; v < scene.pool_vertices; ++v)
    rest[v] = geometry::dequantize_position(scene.mesh, v);

  const gfx::DeformParams params = scene.params();
  const u32 groups = scene.mesh.clusters.size();
  auto run = [&](const Vector<anim::JointMatrix>& matrices, u32 joint_count) {
    // The bone matrices are host visible, so a pose is one memcpy; a real frame would stage them.
    std::memcpy(scene.joints.mapped, matrices.data(), matrices.size() * sizeof(anim::JointMatrix));
    gfx::DeformDesc desc{};
    desc.flags = gfx::k_deform_skin;
    desc.joint_count = joint_count;
    desc.joints = scene.joints.address;
    gfx::BufferResource table;
    REQUIRE(gfx::upload_buffer(device, &desc, sizeof(desc), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               table, &error));
    gfx::MeshDesc mesh_desc{};
    mesh_desc.quant = Vec4{scene.mesh.quant_origin, scene.mesh.quant_scale};
    mesh_desc.quantized = scene.quantized.address;
    mesh_desc.cluster_count = scene.mesh.clusters.size();
    mesh_desc.deform_pool = scene.pool.address;
    mesh_desc.deform_slots = scene.slots.address;
    mesh_desc.skin = scene.skin.address;
    gfx::BufferResource meshes;
    REQUIRE(gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, meshes, &error));
    gfx::DeformParams local = params;
    local.deform = table.address;
    local.meshes = meshes.address;

    gfx::RenderGraph graph(device);
    const gfx::RgBuffer rg_pool = graph.import_buffer("pool", scene.pool);
    const gfx::RgBuffer rg_host = graph.import_buffer("host", scene.host);
    graph.add_pass(
        "clear", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) { b.write(rg_pool, gfx::Access::TransferWrite); },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdFillBuffer(cb, scene.pool.buffer, 0, VK_WHOLE_SIZE, 0x7fc00000u);  // a quiet NaN
        });
    graph.add_pass(
        "skin", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) { b.write(rg_pool, gfx::Access::ComputeWrite); },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
          vkCmdPushConstants(cb, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(local),
                             &local);
          vkCmdDispatch(cb, groups, 1, 1);
        });
    graph.add_pass(
        "readback", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_pool, gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          const VkBufferCopy copy{0, 0, u64{scene.pool_vertices} * 3 * sizeof(f32)};
          vkCmdCopyBuffer(cb, scene.pool.buffer, scene.host.buffer, 1, &copy);
        });
    REQUIRE_MESSAGE(graph.compile(&error), error);
    VkCommandBuffer commands = frames.begin_frame();
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
    graph.reset();
    gfx::destroy_buffer(device, table);
    gfx::destroy_buffer(device, meshes);
  };

  auto compare = [&](const Vector<anim::JointMatrix>& matrices, const char* what) {
    Vector<Vec3> expected(scene.pool_vertices, Vec3{});
    anim::skin_positions(
        std::span<const Vec3>(rest.data(), rest.size()),
        std::span<const geometry::SkinBinding>(scene.mesh.skin.data(), scene.mesh.skin.size()),
        std::span<const anim::JointMatrix>(matrices.data(), matrices.size()),
        std::span<Vec3>(expected.data(), expected.size()));
    const auto* pool = static_cast<const f32*>(scene.host.mapped);
    f32 worst = 0.0f;
    u32 moved = 0;
    for (u32 v = 0; v < scene.pool_vertices; ++v) {
      const Vec3 got{pool[v * 3], pool[v * 3 + 1], pool[v * 3 + 2]};
      const Vec3 want = expected[v];
      worst = std::max(worst, std::fabs(got.x - want.x));
      worst = std::max(worst, std::fabs(got.y - want.y));
      worst = std::max(worst, std::fabs(got.z - want.z));
      moved += length(want - rest[v]) > 1.0e-4f ? 1 : 0;
    }
    CHECK(worst < 1.0e-5f);
    MESSAGE(std::string(what) << ": worst component difference " << worst << " over "
                              << scene.pool_vertices << " vertices, " << moved << " of them moved");
    return moved;
  };

  // 1. The bind pose. Every skinning matrix is the identity, so the pool holds the rest pose
  //    whatever the weights are — the property that makes a skinned instance and a rigid one
  //    draw the same picture, and the first thing a transposed matrix breaks.
  anim::Pose pose;
  anim::rest_pose(skeleton, pose);
  Vector<Mat4> model(2, Mat4::identity());
  Vector<anim::JointMatrix> matrices(2, anim::JointMatrix{});
  auto build = [&]() {
    anim::local_to_model(skeleton, pose, std::span<Mat4>(model.data(), model.size()));
    anim::skinning_matrices(std::span<const Mat4>(model.data(), model.size()),
                            std::span<const Mat4>(skeleton.inverse_bind.data(), 2),
                            std::span<anim::JointMatrix>(matrices.data(), 2));
  };
  build();
  run(matrices, 2);
  CHECK(compare(matrices, "bind pose") == 0);
  {
    const auto* pool = static_cast<const f32*>(scene.host.mapped);
    f32 worst = 0.0f;
    for (u32 v = 0; v < scene.pool_vertices; ++v) {
      worst = std::max(worst, std::fabs(pool[v * 3] - rest[v].x));
      worst = std::max(worst, std::fabs(pool[v * 3 + 1] - rest[v].y));
      worst = std::max(worst, std::fabs(pool[v * 3 + 2] - rest[v].z));
    }
    CHECK(worst < 1.0e-6f);
  }

  // 2. A pose that bends the cylinder: the tip turns 50 degrees about +z and scales, so the top
  //    ring swings, the bottom ring does not, and every ring between is a blend of two matrices.
  //    This is the case a wrong weight normalization, a missing divide by 255, or a matrix read
  //    column-major instead of row-major all fail.
  pose.rotation[1] = quat_from_axis_angle(Vec3::unit_z(), radians(50.0f));
  pose.scale[1] = Vec3{1.2f, 1.0f, 1.2f};
  pose.translation[0] = Vec3{0.1f, 0.0f, -0.05f};
  build();
  run(matrices, 2);
  const u32 moved = compare(matrices, "bent pose");
  CHECK(moved > scene.pool_vertices / 2);  // the deformation reaches most of the mesh

  // 3. A joint count that does not cover the bindings: the influences naming joint 1 are skipped
  //    on the GPU exactly as `skin_positions` skips them, so bad data deforms a vertex short
  //    rather than reading past the end of the array. The top ring, which is joint 1's alone,
  //    collapses to the origin on both sides — and that agreement is the assertion.
  run(matrices, 1);
  Vector<anim::JointMatrix> one(1, matrices[0]);
  compare(one, "a palette shorter than the bindings");

  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  frames.destroy();
  scene.destroy(device);
  device.destroy();
}
