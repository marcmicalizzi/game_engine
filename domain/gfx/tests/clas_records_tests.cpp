// The GPU-driven half of the cluster acceleration structure path: clas_records.slang turns a
// scene's visible list and its count into the 64-byte CLAS build records, grouped by instance so
// every instance can have its own cluster bottom-level structure, plus the record count and one
// bottom-level record per instance. The shader's bytes must equal what write_cluster_build_records
// writes on the CPU for the same clusters, so the CPU and GPU paths can never drift apart. One of
// the two instances is **deformed**, so its records must name the frame's deformed-vertex pool
// instead of the mesh's rest positions, which is the rule every position reader follows. The
// shader needs no ray tracing feature, so this runs on any device with a driver.
#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/clas_records.spv.h>
#include <string>

using namespace engine;

namespace {

void make_grid(u32 n, Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x)
      positions.push_back(
          Vec3{static_cast<f32>(x), static_cast<f32>(y) * 0.5f, static_cast<f32>(x * y) * 0.01f});
  }
  for (u32 y = 0; y + 1 < n; ++y) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = y * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

}  // namespace

TEST_CASE("clas records: the shader writes the same build records as the CPU") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_grid(33, positions, indices);  // 2,048 triangles
  geometry::ClusterMesh mesh;
  REQUIRE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error));
  const u32 cluster_count = mesh.clusters.size();
  REQUIRE(cluster_count >= 8);

  // Two instances of the one mesh, so the compaction the shader does has something to compact:
  // the pair space is twice the cluster count and the second instance's pairs start at its
  // first_pair. The second one is deformed, so its records must point into the pool.
  constexpr u32 k_instances = 2;
  constexpr u32 k_deformed = 1;
  const u32 pair_count = cluster_count * k_instances;
  gfx::InstanceDesc instance_table[k_instances];
  for (u32 i = 0; i < k_instances; ++i) {
    gfx::set_instance_transform(instance_table[i],
                                translation(Vec3{40.0f * static_cast<f32>(i), 0.0f, 0.0f}));
    instance_table[i].mesh = 0;
    instance_table[i].first_pair = i * cluster_count;
  }
  instance_table[k_deformed].deform = 0;
  // The pool block starts a few slots in, so a wrong offset shows up rather than cancelling out.
  gfx::DeformDesc deform{};
  deform.pool_offset = 7;
  deform.vertex_count = mesh.vertices.size();
  deform.flags = gfx::k_deform_wave;

  // A visible list in a scrambled order, both instances interleaved, with a count below it: the
  // last entry is beyond the count and must be ignored.
  Vector<u32> visible;  // uint2 per entry
  for (u32 i = cluster_count; i-- > 0;) {
    if (i % 2 != 0) continue;
    for (u32 k = 0; k < k_instances; ++k) {
      visible.push_back(k);
      visible.push_back(i);
    }
  }
  const u32 entry_count = visible.size() / 2;
  const u32 count = entry_count - 1;
  Vector<u8> indices8;
  gfx::pack_cluster_indices(std::span<const u32>(mesh.triangles.data(), mesh.triangles.size()),
                            indices8);

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource vertices;
  gfx::BufferResource index8;
  gfx::BufferResource instances;
  gfx::BufferResource visible_buffer;
  gfx::BufferResource count_buffer;
  gfx::BufferResource slots;
  gfx::BufferResource instance_counts;
  gfx::BufferResource instance_first;
  gfx::BufferResource records;
  gfx::BufferResource record_count;
  gfx::BufferResource blas_records;
  gfx::BufferResource meshes;
  gfx::BufferResource deform_table;
  gfx::BufferResource pool;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vec3),
                             k_storage, vertices, &error));
  REQUIRE(gfx::upload_buffer(device, indices8.data(), indices8.size(), k_storage, index8, &error));
  REQUIRE(gfx::upload_buffer(device, instance_table, sizeof(instance_table), k_storage, instances,
                             &error));
  REQUIRE(gfx::upload_buffer(device, visible.data(), visible.size() * sizeof(u32), k_storage,
                             visible_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, &count, sizeof(u32), k_storage, count_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, &deform, sizeof(deform), k_storage, deform_table, &error));
  REQUIRE(gfx::create_buffer(device, (u64{deform.pool_offset} + deform.vertex_count) * 12,
                             k_address, false, pool, &error));
  gfx::MeshDesc mesh_desc{};
  mesh_desc.cluster_count = cluster_count;
  mesh_desc.deform_pool = pool.address;
  mesh_desc.deform = deform_table.address;
  REQUIRE(gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, &error));
  const u64 records_bytes = gfx::k_cluster_build_record_bytes * pair_count;
  REQUIRE(
      gfx::create_buffer(device, u64{pair_count} * sizeof(u32), k_address, false, slots, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * k_instances,
                             k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, instance_counts,
                             &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * k_instances, k_address, true, instance_first,
                             &error));
  REQUIRE(gfx::create_buffer(device, records_bytes, k_address, true, records, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32), k_address, true, record_count, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * k_instances, k_address,
                             true, blas_records, &error));
  std::memset(records.mapped, 0xcd, records_bytes);
  std::memset(record_count.mapped, 0xcd, sizeof(u32));
  std::memset(blas_records.mapped, 0xcd, gfx::k_cluster_blas_record_bytes * k_instances);

  const u64 fake_addresses = 0x1000;  // the shader offsets this into the BLAS records
  gfx::ClusterRecordParams params{};
  params.clusters = clusters.address;
  params.vertices = vertices.address;
  params.indices8 = index8.address;
  params.instances = instances.address;
  params.meshes = meshes.address;
  params.visible = visible_buffer.address;
  params.visible_count = count_buffer.address;
  params.slots = slots.address;
  params.instance_counts = instance_counts.address;
  params.instance_first = instance_first.address;
  params.records = records.address;
  params.record_count = record_count.address;
  params.blas_records = blas_records.address;
  params.clas_addresses = fake_addresses;
  params.instance_count = k_instances;
  params.pair_count = pair_count;
  params.views = 1;  // one view: the run is the whole list and its count word is the first

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_clas_records_spirv,
                                                    shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  gfx::ComputePipeline bucket;
  gfx::ComputePipeline ranges;
  gfx::ComputePipeline emit;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "records_main", {}, sizeof(params),
                                               bucket, &error),
                  error);
  REQUIRE(gfx::create_compute_pipeline(device, module, "ranges_main", {}, sizeof(params), ranges,
                                       &error));
  REQUIRE(
      gfx::create_compute_pipeline(device, module, "emit_main", {}, sizeof(params), emit, &error));
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        vkCmdFillBuffer(cb, instance_counts.buffer, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        const gfx::ComputePipeline* passes[3] = {&bucket, &ranges, &emit};
        const u32 groups[3] = {(pair_count + 63) / 64, 1, (pair_count + 63) / 64};
        for (u32 p = 0; p < 3; ++p) {
          vkCmdPipelineBarrier2(cb, &dependency);
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, passes[p]->pipeline);
          vkCmdPushConstants(cb, passes[p]->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                             &params);
          vkCmdDispatch(cb, groups[p], 1, 1);
        }
      },
      &error));

  // The CPU reference: the same clusters, grouped by instance, in the order the shader's
  // per-instance atomic can produce (within an instance the order is arbitrary, so the records
  // are compared as a set of entries rather than position by position).
  const auto* counts_out = static_cast<const u32*>(instance_counts.mapped);
  const auto* first_out = static_cast<const u32*>(instance_first.mapped);
  u32 expected_per_instance[k_instances] = {};
  for (u32 i = 0; i < count; ++i)
    ++expected_per_instance[visible[i * 2]];
  u32 total = 0;
  for (u32 k = 0; k < k_instances; ++k) {
    CHECK(counts_out[k] == expected_per_instance[k]);
    CHECK(first_out[k] == total);
    total += expected_per_instance[k];
  }
  u32 got_count = 0;
  std::memcpy(&got_count, record_count.mapped, sizeof(got_count));
  CHECK(got_count == count);
  CHECK(total == count);

  // Every record must be byte-identical to what the CPU writes for the entry its cluster id
  // names, and must sit in its instance's run.
  const auto* got = static_cast<const u8*>(records.mapped);
  u32 differing = 0;
  u32 misplaced = 0;
  Vector<u32> seen(count, 0u);
  for (u32 r = 0; r < count; ++r) {
    u32 entry = 0;
    std::memcpy(&entry, got + u64{r} * gfx::k_cluster_build_record_bytes, sizeof(entry));
    REQUIRE(entry < count);
    ++seen[entry];
    const u32 instance_index = visible[entry * 2];
    if (r < first_out[instance_index] ||
        r >= first_out[instance_index] + counts_out[instance_index]) {
      ++misplaced;
    }
    const geometry::ClusterDesc& desc = mesh.clusters[visible[entry * 2 + 1]];
    gfx::ClusterBuildInput in;
    in.cluster_id = entry;  // the visible index is the cluster id and the base geometry index
    in.triangle_count = desc.triangle_count;
    in.vertex_count = desc.vertex_count;
    // A deformed instance's positions come out of the pool, at its block's own offset.
    in.vertices = instance_index == k_deformed
                      ? pool.address + (u64{deform.pool_offset} + desc.vertex_offset) * sizeof(Vec3)
                      : vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
    in.indices = index8.address + u64{desc.triangle_offset} * 3;
    u8 expected[gfx::k_cluster_build_record_bytes];
    gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&in, 1), expected);
    for (u32 b = 0; b < gfx::k_cluster_build_record_bytes; ++b)
      differing += got[u64{r} * gfx::k_cluster_build_record_bytes + b] != expected[b];
  }
  CHECK(differing == 0);
  CHECK(misplaced == 0);
  u32 unseen = 0;
  for (const u32 s : seen)
    unseen += s != 1;
  CHECK(unseen == 0);
  // The entry past the count stays untouched.
  CHECK(got[u64{pair_count - 1} * gfx::k_cluster_build_record_bytes] == 0xcd);

  // One bottom-level record per instance, over that instance's run of CLAS addresses.
  const auto* blas_bytes = static_cast<const u8*>(blas_records.mapped);
  for (u32 k = 0; k < k_instances; ++k) {
    u32 blas_count = 0;
    u32 blas_stride = 0;
    u64 blas_references = 0;
    const u8* record = blas_bytes + u64{k} * gfx::k_cluster_blas_record_bytes;
    std::memcpy(&blas_count, record, 4);
    std::memcpy(&blas_stride, record + 4, 4);
    std::memcpy(&blas_references, record + 8, 8);
    CHECK(blas_count == expected_per_instance[k]);
    CHECK(blas_stride == 8);
    CHECK(blas_references == fake_addresses + u64{first_out[k]} * 8);
  }
  MESSAGE("records for " << count << " of " << pair_count << " pairs over " << k_instances
                         << " instances (one deformed) match the CPU");

  gfx::destroy_compute_pipeline(device, bucket);
  gfx::destroy_compute_pipeline(device, ranges);
  gfx::destroy_compute_pipeline(device, emit);
  gfx::destroy_shader_module(device, module);
  for (gfx::BufferResource* b : {&clusters, &vertices, &index8, &instances, &visible_buffer,
                                 &count_buffer, &slots, &instance_counts, &instance_first, &records,
                                 &record_count, &blas_records, &meshes, &deform_table, &pool}) {
    gfx::destroy_buffer(device, *b);
  }
  device.destroy();
}
