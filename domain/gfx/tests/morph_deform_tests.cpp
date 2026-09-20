// The deform **chain** on the GPU (docs/subsystems/gfx.md, "The deform chain"): the four stages
// of `deform.slang` run over a mesh with morph channels and compared, vertex by vertex, against
// `deform_reference.h` — the same four stages in the same order on the CPU.
//
// What each case is for:
//   static only        the stage that is meant to cost nothing per frame, and the cache that
//                      makes it so. Also the case that proves the morph directory's binary
//                      search finds the right delta for the right vertex of the right cluster.
//   pose only          the same arithmetic over the other half of the weights array.
//   static + pose      the two composing, which is where a wrong `weight_base` shows up.
//   morph + skin       a two-bone cylinder with a bulge channel: the case the **order** exists
//                      for. Morph deltas are authored in bind space, so applying them after the
//                      skin gives a visibly different answer, and the test asserts the difference
//                      rather than only asserting the right one.
//   the whole chain    static, pose, skin and `lattice` on one instance — which is the thing E25
//                      said needed a second pass or a wider kind field.
//   the cache          weights unchanged for N frames: the static stage dispatches once, and the
//                      per-frame chain reads the cache and gets the same answer as recomputing.
//
// Like `skin_tests.cpp` this dispatches the pass and reads the pool back rather than drawing, so
// it needs no raster pipeline and no 64-bit atomics and runs on any device with a driver.
#include "deform_reference.h"

#include <domain/anim/skeleton.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/stress_mesh.h>
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
using namespace engine::gfx::test_reference;

namespace {

constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// A sphere with a face-like rig: several channels, each touching a few percent of the vertices,
// which is the shape the sparse cluster-ordered directory is designed for.
geometry::MorphFixtureOptions rig() {
  geometry::MorphFixtureOptions options;
  options.segments = 48;
  options.rings = 24;
  options.channels = 4;
  options.falloff = 0.3f;
  return options;
}

// The whole GPU side: the mesh's buffers, one instance, one deform record, the pool, and the
// per-entry slot table. The slot table is deliberately the **mesh-ordered** one — entry c's block
// starts at cluster c's own `vertex_offset` — so a pool slot is the mesh's vertex index and every
// comparison below is a plain walk. A frame's allocator packs the cut instead; that it can is
// exactly the point of the table being what decides (skin_tests.cpp says the same).
struct Harness {
  geometry::ClusterMesh mesh;
  Vector<anim::JointMatrix> joint_matrices;
  Vector<f32> weights;  // 2 * channel_count: the static half then the pose half
  u32 pool_vertices = 0;

  gfx::BufferResource clusters, quantized, attributes, skin, meshes, instances, deform_table;
  gfx::BufferResource joints, weights_buffer, pool, normal_pool, cache, slots, count_buffer;
  gfx::BufferResource visible, host, host_normals, morph_block;
  gfx::BufferResource morph_channels, morph_directory, morph_slices, morph_indices, morph_deltas,
      morph_normal_deltas;

  bool create(const gfx::Device& device, const geometry::MorphFixtureMesh& source, u32 stages,
              std::string* error, bool with_skin = false) {
    geometry::AttributeSource attribute_source;
    attribute_source.normals = std::span<const Vec3>(source.normals.data(), source.normals.size());
    attribute_source.uvs = std::span<const Vec2>(source.uvs.data(), source.uvs.size());
    attribute_source.morph =
        std::span<const geometry::MorphChannelSource>(source.morph.data(), source.morph.size());
    Vector<geometry::SkinBinding> bindings;
    if (with_skin) {
      // Two joints along +y, linear between them: the same gradient skin_tests.cpp uses, which
      // is the smallest skin a wrong blend shows up on.
      f32 lo = source.positions[0].y;
      f32 hi = lo;
      for (const Vec3& p : source.positions) {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
      }
      for (const Vec3& p : source.positions) {
        const f32 t = hi > lo ? (p.y - lo) / (hi - lo) : 0.0f;
        const u32 joint_indices[4] = {0, 1, 0, 0};
        const f32 joint_weights[4] = {1.0f - t, t, 0.0f, 0.0f};
        bindings.push_back(geometry::make_skin_binding(joint_indices, joint_weights));
      }
      attribute_source.skin =
          std::span<const geometry::SkinBinding>(bindings.data(), bindings.size());
      attribute_source.joint_count = 2;
    }
    if (!geometry::build_clusters(source.positions, source.indices, geometry::ClusterBuildOptions{},
                                  mesh, error, attribute_source)) {
      return false;
    }
    if (mesh.morph_channels.empty()) {
      if (error != nullptr) *error = "the cluster builder dropped the morph channels";
      return false;
    }
    pool_vertices = mesh.vertices.size();
    weights.resize(mesh.morph_channels.size() * 2, 0.0f);

    const u64 pool_bytes = u64{pool_vertices} * 3 * sizeof(f32);
    const u64 normal_bytes = u64{pool_vertices} * sizeof(u32);
    const VkBufferUsageFlags k_readback =
        k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!gfx::upload_buffer(device, mesh.clusters.data(),
                            mesh.clusters.size() * sizeof(geometry::ClusterDesc), k_storage,
                            clusters, error) ||
        !gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                            k_storage, quantized, error) ||
        !gfx::upload_buffer(device, mesh.attributes.data(),
                            mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                            attributes, error) ||
        !gfx::create_buffer(device, pool_bytes, k_readback, false, pool, error) ||
        !gfx::create_buffer(device, normal_bytes, k_readback, false, normal_pool, error) ||
        !gfx::create_buffer(device, u64{pool_vertices} * sizeof(gfx::DeformCacheVertex), k_readback,
                            false, cache, error) ||
        !gfx::create_buffer(device, pool_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host,
                            error) ||
        !gfx::create_buffer(device, normal_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                            host_normals, error) ||
        !gfx::create_buffer(device, u64{weights.size()} * sizeof(f32), k_address, true,
                            weights_buffer, error) ||
        !gfx::create_buffer(device, sizeof(anim::JointMatrix) * 2, k_address, true, joints,
                            error)) {
      return false;
    }
    std::memset(weights_buffer.mapped, 0, weights.size() * sizeof(f32));

    // The morph stream, as the scene-wide arrays the pass reads.
    if (!gfx::upload_buffer(device, mesh.morph_channels.data(),
                            mesh.morph_channels.size() * sizeof(geometry::MorphChannel), k_storage,
                            morph_channels, error) ||
        !gfx::upload_buffer(device, mesh.morph_cluster_slices.data(),
                            mesh.morph_cluster_slices.size() * sizeof(u32), k_storage,
                            morph_directory, error) ||
        !gfx::upload_buffer(device, mesh.morph_slices.data(),
                            mesh.morph_slices.size() * sizeof(geometry::MorphSlice), k_storage,
                            morph_slices, error) ||
        !gfx::upload_buffer(device, mesh.morph_indices.data(), mesh.morph_indices.size(), k_storage,
                            morph_indices, error) ||
        !gfx::upload_buffer(device, mesh.morph_deltas.data(),
                            mesh.morph_deltas.size() * sizeof(i16), k_storage, morph_deltas,
                            error)) {
      return false;
    }
    if (!mesh.morph_normal_deltas.empty() &&
        !gfx::upload_buffer(device, mesh.morph_normal_deltas.data(),
                            mesh.morph_normal_deltas.size() * sizeof(i16), k_storage,
                            morph_normal_deltas, error)) {
      return false;
    }
    gfx::MorphParams morph{};
    morph.channels = morph_channels.address;
    morph.directory = morph_directory.address;
    morph.slices = morph_slices.address;
    morph.indices = morph_indices.address;
    morph.deltas = morph_deltas.address;
    morph.normal_deltas =
        morph_normal_deltas.buffer != VK_NULL_HANDLE ? morph_normal_deltas.address : 0;
    if (!gfx::upload_buffer(device, &morph, sizeof(morph), k_storage, morph_block, error))
      return false;

    if (with_skin && !gfx::upload_buffer(device, mesh.skin.data(),
                                         mesh.skin.size() * sizeof(geometry::SkinBinding),
                                         k_storage, skin, error)) {
      return false;
    }

    gfx::DeformDesc desc{};
    desc.stages = stages;
    desc.channel_count = mesh.morph_channels.size();
    desc.weights = weights_buffer.address;
    desc.cache = cache.address;
    desc.joint_count = with_skin ? 2u : 0u;
    desc.joints = with_skin ? joints.address : 0;
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
    if (!gfx::create_buffer(device, sizeof(gfx::DeformDesc), k_address, true, deform_table,
                            error) ||
        !gfx::upload_buffer(device, &instance, sizeof(instance), k_storage, instances, error) ||
        !gfx::upload_buffer(device, &count, sizeof(count), k_storage, count_buffer, error) ||
        !gfx::upload_buffer(device, slot_table.data(), slot_table.size() * sizeof(u32), k_storage,
                            slots, error) ||
        !gfx::upload_buffer(device, entries.data(), entries.size() * sizeof(u32), k_storage,
                            visible, error)) {
      return false;
    }
    std::memcpy(deform_table.mapped, &desc, sizeof(desc));

    gfx::MeshDesc mesh_desc{};
    mesh_desc.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
    mesh_desc.quantized = quantized.address;
    mesh_desc.cluster_count = mesh.clusters.size();
    mesh_desc.deform_pool = pool.address;
    mesh_desc.deform_slots = slots.address;
    mesh_desc.skin = with_skin ? skin.address : 0;
    return gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, error);
  }

  gfx::DeformDesc& record() noexcept { return *static_cast<gfx::DeformDesc*>(deform_table.mapped); }

  void set_weights(std::span<const f32> statics, std::span<const f32> pose) {
    const u32 channels = mesh.morph_channels.size();
    for (u32 c = 0; c < channels; ++c) {
      weights[c] = c < statics.size() ? statics[c] : 0.0f;
      weights[channels + c] = c < pose.size() ? pose[c] : 0.0f;
    }
    std::memcpy(weights_buffer.mapped, weights.data(), weights.size() * sizeof(f32));
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
    p.morph = morph_block.address;
    p.normal_pool = normal_pool.address;
    p.max_entries = mesh.clusters.size();
    return p;
  }

  DeformChain chain(u32 stages) const noexcept {
    DeformChain out;
    out.mesh = &mesh;
    out.stages = stages;
    const u32 channels = mesh.morph_channels.size();
    out.static_weights = std::span<const f32>(weights.data(), channels);
    out.pose_weights = std::span<const f32>(weights.data() + channels, channels);
    out.joints = std::span<const anim::JointMatrix>(joint_matrices.data(), joint_matrices.size());
    return out;
  }

  void destroy(const gfx::Device& device) noexcept {
    for (gfx::BufferResource* b :
         {&clusters,       &quantized,     &attributes,     &skin,
          &meshes,         &instances,     &deform_table,   &joints,
          &weights_buffer, &pool,          &normal_pool,    &cache,
          &slots,          &count_buffer,  &visible,        &host,
          &host_normals,   &morph_block,   &morph_channels, &morph_directory,
          &morph_slices,   &morph_indices, &morph_deltas,   &morph_normal_deltas}) {
      gfx::destroy_buffer(device, *b);
    }
  }
};

// Dispatches `deform_main` (and, when `rebuild` is set, `deform_cache_main` first) and reads the
// two pools back. Returns how many workgroups the cache pass ran, so the cache test can count.
u32 run(const gfx::Device& device, Harness& harness, const gfx::ComputePipeline& chain_pipeline,
        const gfx::ComputePipeline& cache_pipeline, bool rebuild, Vector<Vec3>& positions,
        Vector<u32>& normals) {
  u32 cache_groups = 0;
  gfx::submit_immediate(device, [&](VkCommandBuffer commands) {
    if (rebuild) {
      gfx::DeformParams cache_params = harness.params();
      cache_params.visible_offset = 0;                          // the instance to rebuild
      cache_params.max_entries = harness.mesh.clusters.size();  // its mesh's cluster count
      cache_groups = harness.mesh.clusters.size();
      vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, cache_pipeline.pipeline);
      vkCmdPushConstants(commands, cache_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(cache_params), &cache_params);
      vkCmdDispatch(commands, cache_groups, 1, 1);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                           nullptr);
    }
    const gfx::DeformParams params = harness.params();
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline.pipeline);
    vkCmdPushConstants(commands, chain_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(params), &params);
    vkCmdDispatch(commands, harness.mesh.clusters.size(), 1, 1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{};
    copy.size = u64{harness.pool_vertices} * 3 * sizeof(f32);
    vkCmdCopyBuffer(commands, harness.pool.buffer, harness.host.buffer, 1, &copy);
    VkBufferCopy normal_copy{};
    normal_copy.size = u64{harness.pool_vertices} * sizeof(u32);
    vkCmdCopyBuffer(commands, harness.normal_pool.buffer, harness.host_normals.buffer, 1,
                    &normal_copy);
  });
  positions.resize(harness.pool_vertices);
  std::memcpy(positions.data(), harness.host.mapped, u64{harness.pool_vertices} * 3 * sizeof(f32));
  normals.resize(harness.pool_vertices);
  std::memcpy(normals.data(), harness.host_normals.mapped,
              u64{harness.pool_vertices} * sizeof(u32));
  return cache_groups;
}

struct Agreement {
  f32 position = 0.0f;  // worst |gpu - cpu| over every vertex
  f32 normal_deg = 0.0f;
  u32 compared = 0;
};

Agreement compare(const Harness& harness, const DeformChain& chain, std::span<const Vec3> positions,
                  std::span<const u32> normals) {
  Agreement out;
  for (u32 c = 0; c < harness.mesh.clusters.size(); ++c) {
    const geometry::ClusterDesc& cluster = harness.mesh.clusters[c];
    for (u32 local = 0; local < cluster.vertex_count; ++local) {
      Vec3 want_position;
      Vec3 want_normal;
      deform_vertex(chain, c, local, want_position, want_normal);
      const u32 slot = cluster.vertex_offset + local;
      out.position = std::max(out.position, length(positions[slot] - want_position));
      const Vec3 got = geometry::decode_normal_oct(normals[slot]);
      const f32 cosine =
          std::min(1.0f, std::max(-1.0f, dot(normalize(got), normalize(want_normal))));
      out.normal_deg = std::max(out.normal_deg, std::acos(cosine) * 57.29577951f);
      ++out.compared;
    }
  }
  return out;
}

}  // namespace

TEST_CASE("deform chain: every stage and the whole chain match the CPU reference") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  VkShaderModule module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                    shaders::k_deform_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  gfx::ComputePipeline chain_pipeline;
  gfx::ComputePipeline cache_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "deform_main", {},
                                               sizeof(gfx::DeformParams), chain_pipeline, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "deform_cache_main", {},
                                               sizeof(gfx::DeformParams), cache_pipeline, &error),
                  error);

  geometry::MorphFixtureMesh source;
  geometry::build_morph_sphere(rig(), source);

  Vector<Vec3> positions;
  Vector<u32> normals;

  SUBCASE("the static stage alone, and its cache") {
    Harness harness;
    REQUIRE_MESSAGE(harness.create(device, source, gfx::k_deform_stage_static, &error), error);
    const f32 statics[4] = {1.0f, 0.5f, 0.0f, -0.25f};
    harness.set_weights(std::span<const f32>(statics, 4), {});

    // Without a cache the stage runs per frame and is still right.
    harness.record().cache = 0;
    run(device, harness, chain_pipeline, cache_pipeline, false, positions, normals);
    const Agreement direct =
        compare(harness, harness.chain(gfx::k_deform_stage_static), positions, normals);
    MESSAGE("static stage, no cache: " << direct.compared << " vertices, worst position "
                                       << direct.position << ", worst normal " << direct.normal_deg
                                       << " deg");
    CHECK(direct.position < 1.0e-5f);
    CHECK(direct.normal_deg < 0.6f);  // the pool's normal is octahedral snorm16, like the source

    // With one, the cache pass fills it and the chain reads it: the same answer, and the stage's
    // directory walk no longer happens per frame.
    harness.record().cache = harness.cache.address;
    harness.record().stages = gfx::k_deform_stage_static | gfx::k_deform_stage_rebuild_cache;
    const u32 groups =
        run(device, harness, chain_pipeline, cache_pipeline, true, positions, normals);
    CHECK(groups == harness.mesh.clusters.size());
    harness.record().stages = gfx::k_deform_stage_static;
    Vector<Vec3> cached_positions;
    Vector<u32> cached_normals;
    // Five frames with the weights untouched: the cache pass never runs again, and every frame
    // is bit-identical to the one before it.
    for (u32 frame = 0; frame < 5; ++frame) {
      const u32 rebuilt = run(device, harness, chain_pipeline, cache_pipeline, false,
                              cached_positions, cached_normals);
      CHECK(rebuilt == 0);
      const Agreement cached = compare(harness, harness.chain(gfx::k_deform_stage_static),
                                       cached_positions, cached_normals);
      CHECK(cached.position < 1.0e-5f);
      CHECK(cached.normal_deg < 0.6f);
    }
    harness.destroy(device);
  }

  SUBCASE("the pose stage alone") {
    Harness harness;
    REQUIRE_MESSAGE(harness.create(device, source, gfx::k_deform_stage_pose, &error), error);
    const f32 pose[4] = {0.0f, 1.0f, 0.75f, 0.0f};
    harness.set_weights({}, std::span<const f32>(pose, 4));
    run(device, harness, chain_pipeline, cache_pipeline, false, positions, normals);
    const Agreement agree =
        compare(harness, harness.chain(gfx::k_deform_stage_pose), positions, normals);
    MESSAGE("pose stage: worst position " << agree.position << ", worst normal " << agree.normal_deg
                                          << " deg");
    CHECK(agree.position < 1.0e-5f);
    CHECK(agree.normal_deg < 0.6f);
    harness.destroy(device);
  }

  SUBCASE("static and pose together") {
    const u32 stages = gfx::k_deform_stage_static | gfx::k_deform_stage_pose;
    Harness harness;
    REQUIRE_MESSAGE(harness.create(device, source, stages, &error), error);
    const f32 statics[4] = {0.6f, 0.0f, 0.0f, 0.4f};
    const f32 pose[4] = {0.0f, -0.5f, 1.0f, 0.0f};
    harness.set_weights(std::span<const f32>(statics, 4), std::span<const f32>(pose, 4));
    harness.record().cache = 0;
    run(device, harness, chain_pipeline, cache_pipeline, false, positions, normals);
    const Agreement agree = compare(harness, harness.chain(stages), positions, normals);
    MESSAGE("static + pose: worst position " << agree.position << ", worst normal "
                                             << agree.normal_deg << " deg");
    CHECK(agree.position < 1.0e-5f);
    CHECK(agree.normal_deg < 0.6f);
    harness.destroy(device);
  }

  SUBCASE("morph before skin, which is the order the stages exist for") {
    const u32 stages = gfx::k_deform_stage_pose | gfx::k_deform_stage_skin;
    Harness harness;
    REQUIRE_MESSAGE(harness.create(device, source, stages, &error, true), error);
    const f32 pose[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    harness.set_weights({}, std::span<const f32>(pose, 4));
    // The tip turned 50 degrees and the root moved, so the two orders really do differ.
    harness.joint_matrices.resize(2);
    harness.joint_matrices[0] = anim::joint_matrix(translation(Vec3{0.1f, 0.0f, 0.0f}));
    harness.joint_matrices[1] =
        anim::joint_matrix(mat4_from_quat(quat_from_axis_angle(Vec3::forward(), 0.8726646f)));
    std::memcpy(harness.joints.mapped, harness.joint_matrices.data(),
                harness.joint_matrices.size() * sizeof(anim::JointMatrix));
    run(device, harness, chain_pipeline, cache_pipeline, false, positions, normals);
    const Agreement agree = compare(harness, harness.chain(stages), positions, normals);
    MESSAGE("morph + skin: worst position " << agree.position << ", worst normal "
                                            << agree.normal_deg << " deg");
    CHECK(agree.position < 1.0e-5f);
    CHECK(agree.normal_deg < 1.0f);

    // And the order is not a coin flip: skinning first and morphing after gives a different
    // surface, which is what "morph deltas are authored in bind space" means in numbers.
    DeformChain reversed = harness.chain(stages);
    f32 worst = 0.0f;
    for (u32 c = 0; c < harness.mesh.clusters.size(); ++c) {
      const geometry::ClusterDesc& cluster = harness.mesh.clusters[c];
      for (u32 local = 0; local < cluster.vertex_count; ++local) {
        const u32 vertex = cluster.vertex_offset + local;
        Vec3 position = geometry::dequantize_position(harness.mesh, vertex);
        Vec3 normal = geometry::decode_normal_oct(harness.mesh.attributes[vertex].normal_oct);
        skin_stage(reversed, harness.mesh.skin[vertex], position, normal);
        morph_stage(reversed, c, local, reversed.pose_weights, position, normal);
        worst = std::max(worst, length(positions[vertex] - position));
      }
    }
    MESSAGE("skin-then-morph differs from morph-then-skin by up to " << worst << " mesh units");
    CHECK(worst > 1.0e-3f);
    harness.destroy(device);
  }

  SUBCASE("the whole chain: static, pose, skin and a lattice on one instance") {
    const u32 stages = gfx::k_deform_stage_static | gfx::k_deform_stage_pose |
                       gfx::k_deform_stage_skin | gfx::k_deform_stage_procedural |
                       gfx::k_deform_lattice;
    Harness harness;
    REQUIRE_MESSAGE(harness.create(device, source, stages, &error, true), error);
    const f32 statics[4] = {0.5f, 0.0f, 0.25f, 0.0f};
    const f32 pose[4] = {0.0f, 0.8f, 0.0f, -0.3f};
    harness.set_weights(std::span<const f32>(statics, 4), std::span<const f32>(pose, 4));
    harness.joint_matrices.resize(2);
    harness.joint_matrices[0] = anim::joint_matrix(Mat4::identity());
    harness.joint_matrices[1] =
        anim::joint_matrix(mat4_from_quat(quat_from_axis_angle(Vec3::forward(), 0.3f)));
    std::memcpy(harness.joints.mapped, harness.joint_matrices.data(),
                harness.joint_matrices.size() * sizeof(anim::JointMatrix));
    // The cache carries the static stage while the other three run per frame, which is the shape
    // a real character has.
    harness.record().stages = stages | gfx::k_deform_stage_rebuild_cache;
    run(device, harness, chain_pipeline, cache_pipeline, true, positions, normals);
    harness.record().stages = stages;
    run(device, harness, chain_pipeline, cache_pipeline, false, positions, normals);
    DeformChain chain = harness.chain(stages);
    chain.time = 0.0f;
    chain.amplitude = 1.0f;
    const Agreement agree = compare(harness, chain, positions, normals);
    MESSAGE("the whole chain: worst position " << agree.position << ", worst normal "
                                               << agree.normal_deg << " deg over " << agree.compared
                                               << " vertices");
    CHECK(agree.position < 2.0e-5f);
    harness.destroy(device);
  }

  gfx::destroy_compute_pipeline(device, chain_pipeline);
  gfx::destroy_compute_pipeline(device, cache_pipeline);
  gfx::destroy_shader_module(device, module);
}
