// The GPU-driven half of the cluster acceleration structure path: clas_records.slang turns a
// visible list and its count into the 64-byte CLAS build records, the record count, and the
// cluster bottom-level record. The shader's bytes must equal what write_cluster_build_records
// writes on the CPU for the same clusters, so the CPU and GPU paths can never drift apart. The
// shader needs no ray tracing feature, so this runs on any device with a driver.
#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_acceleration.h>
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

  // A visible list of every other cluster, in a scrambled order, with a count below the list.
  Vector<u32> visible;
  for (u32 i = cluster_count; i-- > 0;) {
    if (i % 2 == 0) visible.push_back(i);
  }
  const u32 count = visible.size() - 1;  // the last entry is beyond the count and must be ignored
  Vector<u8> indices8;
  gfx::pack_cluster_indices(std::span<const u32>(mesh.triangles.data(), mesh.triangles.size()),
                            indices8);

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource vertices;
  gfx::BufferResource index8;
  gfx::BufferResource visible_buffer;
  gfx::BufferResource count_buffer;
  gfx::BufferResource records;
  gfx::BufferResource record_count;
  gfx::BufferResource blas_record;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vec3),
                             k_storage, vertices, &error));
  REQUIRE(gfx::upload_buffer(device, indices8.data(), indices8.size(), k_storage, index8, &error));
  REQUIRE(gfx::upload_buffer(device, visible.data(), visible.size() * sizeof(u32), k_storage,
                             visible_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, &count, sizeof(u32), k_storage, count_buffer, &error));
  const u64 records_bytes = gfx::k_cluster_build_record_bytes * cluster_count;
  REQUIRE(gfx::create_buffer(device, records_bytes, k_address, true, records, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32), k_address, true, record_count, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes, k_address, true, blas_record,
                             &error));
  std::memset(records.mapped, 0xcd, records_bytes);
  std::memset(record_count.mapped, 0xcd, sizeof(u32));
  std::memset(blas_record.mapped, 0xcd, gfx::k_cluster_blas_record_bytes);

  const u64 fake_addresses = 0x1000;  // the shader copies this address into the BLAS record
  gfx::ClusterRecordParams params{};
  params.clusters = clusters.address;
  params.vertices = vertices.address;
  params.indices8 = index8.address;
  params.visible = visible_buffer.address;
  params.visible_count = count_buffer.address;
  params.records = records.address;
  params.record_count = record_count.address;
  params.blas_record = blas_record.address;
  params.clas_addresses = fake_addresses;
  params.max_clusters = cluster_count;

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_clas_records_spirv,
                                                    shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "records_main", {}, sizeof(params),
                                               pipeline, &error),
                  error);
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
        vkCmdPushConstants(cb, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           &params);
        vkCmdDispatch(cb, (cluster_count + 63) / 64, 1, 1);
      },
      &error));

  // The CPU reference for the same clusters.
  Vector<gfx::ClusterBuildInput> inputs;
  for (u32 i = 0; i < count; ++i) {
    const geometry::ClusterDesc& desc = mesh.clusters[visible[i]];
    gfx::ClusterBuildInput in;
    in.cluster_id = visible[i];
    in.triangle_count = desc.triangle_count;
    in.vertex_count = desc.vertex_count;
    in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
    in.indices = index8.address + u64{desc.triangle_offset} * 3;
    inputs.push_back(in);
  }
  Vector<u8> expected(static_cast<u32>(gfx::k_cluster_build_record_bytes) * count);
  gfx::write_cluster_build_records(inputs, expected.data());
  const auto* got = static_cast<const u8*>(records.mapped);
  u32 differing = 0;
  for (u32 i = 0; i < expected.size(); ++i)
    differing += got[i] != expected[i];
  CHECK(differing == 0);
  // The entry past the count stays untouched.
  CHECK(got[count * gfx::k_cluster_build_record_bytes] == 0xcd);
  u32 got_count = 0;
  std::memcpy(&got_count, record_count.mapped, sizeof(got_count));
  CHECK(got_count == count);
  u32 blas_count = 0;
  u32 blas_stride = 0;
  u64 blas_references = 0;
  const auto* blas_bytes = static_cast<const u8*>(blas_record.mapped);
  std::memcpy(&blas_count, blas_bytes, 4);
  std::memcpy(&blas_stride, blas_bytes + 4, 4);
  std::memcpy(&blas_references, blas_bytes + 8, 8);
  CHECK(blas_count == count);
  CHECK(blas_stride == 8);
  CHECK(blas_references == fake_addresses);
  MESSAGE("records for " << count << " of " << cluster_count << " clusters match the CPU");

  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  for (gfx::BufferResource* b : {&clusters, &vertices, &index8, &visible_buffer, &count_buffer,
                                 &records, &record_count, &blas_record}) {
    gfx::destroy_buffer(device, *b);
  }
  device.destroy();
}
