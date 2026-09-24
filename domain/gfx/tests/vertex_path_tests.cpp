// The baseline tier: the vertex-shader cluster path must fill the visibility buffer exactly as
// the mesh-shader path does from the same clusters, both drawn directly and through the cull
// pass counting into vkCmdDrawIndirect's instance count. The mesh comparison needs mesh shaders;
// the vertex path itself and its indirect draw run on any device with 64-bit buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <string>

using namespace engine;

namespace {

void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
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

TEST_CASE("vertex path: the baseline tier fills the visibility buffer like the mesh path") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  const bool have_mesh =
      gfx_test::part(device, "the comparison with the mesh path", {gfx_test::Need::MeshShader});

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterLodMesh lod;
  REQUIRE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error));
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 leaf_count = lod.level_cluster_counts[0];
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(scene.create(device, lod.mesh, leaf_count, &error));  // the cull candidates: the leaves
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));

  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                         look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  gfx::BufferResource vis_mesh;
  gfx::BufferResource vis_vertex;
  gfx::BufferResource vis_indirect;
  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource host;
  const VkBufferUsageFlags k_vis =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_mesh, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_vertex, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_indirect, &error));
  const u64 visible_bytes = u64{cluster_count} * 2 * sizeof(u32);  // uint2 per entry
  REQUIRE(gfx::create_buffer(device, visible_bytes, k_address | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, visible, &error));
  REQUIRE(gfx::create_buffer(device, 16,
                             k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams), k_address, true, params, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * 3 + 16 + visible_bytes,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  REQUIRE(vertex_module != VK_NULL_HANDLE);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  VkPipeline vertex_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
  gfx::ComputePipeline cull_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                       cull_pipeline, &error));
  VkShaderModule mesh_module = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline = VK_NULL_HANDLE;
  if (have_mesh) {
    mesh_module = gfx::create_shader_module(device, shaders::k_cluster_mesh_spirv,
                                            shaders::k_cluster_mesh_spirv_size, &error);
    REQUIRE(mesh_module != VK_NULL_HANDLE);
    gfx::MeshPipelineDesc mesh_desc;
    mesh_desc.mesh = mesh_module;
    mesh_desc.fragment = mesh_module;
    mesh_desc.fragment_entry = "fs_visibility";
    mesh_desc.layout = bindless.pipeline_layout();
    REQUIRE(gfx::create_mesh_pipeline(device, mesh_desc, mesh_pipeline, &error));
  }

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.width = k_w;
  draw.height = k_h;
  gfx::ClusterDrawParams draw_mesh = draw;
  draw_mesh.visibility = vis_mesh.address;
  gfx::ClusterDrawParams draw_vertex = draw;
  draw_vertex.visibility = vis_vertex.address;
  gfx::ClusterDrawParams draw_indirect = draw;
  draw_indirect.visibility = vis_indirect.address;
  draw_indirect.visible = visible.address;

  // Cull: frustum only, no LOD, counting into the instance count of a vkCmdDrawIndirect block;
  // the survivors are the frustum-visible leaves plus every coarser level, so the LOD filter is
  // replaced by restricting the candidate set to the leaves, which the scene's one mesh does by
  // claiming only the first leaf_count clusters (leaves come first).
  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
  cull.view_proj = view_proj;
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{1.0f, 1.0f, 0.0f, 1.0f};  // LOD selection off, frustum on
  cull.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
  cull.cluster_count = cluster_count;
  cull.count_index = 1;
  cull.clusters = clusters.address;
  cull.lods = lods.address;
  cull.visible = visible.address;
  cull.draw_args = args.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = 1;
  cull.pair_count = scene.pair_count();
  std::memcpy(params.mapped, &cull, sizeof(cull));
  const u64 params_address = params.address;

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_mesh = graph.import_buffer("vis_mesh", vis_mesh);
  const gfx::RgBuffer rg_vertex = graph.import_buffer("vis_vertex", vis_vertex);
  const gfx::RgBuffer rg_indirect = graph.import_buffer("vis_indirect", vis_indirect);
  const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible);
  const gfx::RgBuffer rg_args = graph.import_buffer("args", args);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_mesh, gfx::Access::TransferWrite);
        b.write(rg_vertex, gfx::Access::TransferWrite);
        b.write(rg_indirect, gfx::Access::TransferWrite);
        b.write(rg_args, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdFillBuffer(cb, vis_mesh.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, vis_vertex.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, vis_indirect.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, args.buffer, 0, 4, triangles_per_cluster * 3);  // vertexCount
        vkCmdFillBuffer(cb, args.buffer, 4, 12, 0);                         // instanceCount, first*
      });
  if (have_mesh) {
    graph.add_pass(
        "mesh", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_mesh, gfx::Access::FragmentReadWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(draw_mesh), &draw_mesh);
          vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
        });
  }
  graph.add_pass(
      "vertex", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_vertex, gfx::Access::FragmentReadWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vertex_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(draw_vertex), &draw_vertex);
        vkCmdDraw(cb, triangles_per_cluster * 3, leaf_count, 0, 0);
      });
  graph.add_pass(
      "cull", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.write(rg_args, gfx::Access::ComputeReadWrite);
        b.write(rg_visible, gfx::Access::ComputeWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline);
        vkCmdPushConstants(cb, cull_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                           &params_address);
        vkCmdDispatch(cb, gfx::cull_group_count(leaf_count), 1, 1);
      });
  graph.add_pass(
      "vertex indirect", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_indirect, gfx::Access::FragmentReadWrite);
        b.read(rg_args, gfx::Access::IndirectRead);
        b.read(rg_visible, gfx::Access::VertexRead);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vertex_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(draw_indirect), &draw_indirect);
        vkCmdDrawIndirect(cb, args.buffer, 0, 1, sizeof(u32) * 4);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_mesh, gfx::Access::TransferRead);
        b.read(rg_vertex, gfx::Access::TransferRead);
        b.read(rg_indirect, gfx::Access::TransferRead);
        b.read(rg_args, gfx::Access::TransferRead);
        b.read(rg_visible, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        const VkBufferCopy mesh_copy{0, 0, vis_bytes};
        const VkBufferCopy vertex_copy{0, vis_bytes, vis_bytes};
        const VkBufferCopy indirect_copy{0, vis_bytes * 2, vis_bytes};
        const VkBufferCopy args_copy{0, vis_bytes * 3, 16};
        const VkBufferCopy visible_copy{0, vis_bytes * 3 + 16, visible_bytes};
        vkCmdCopyBuffer(cb, vis_mesh.buffer, host.buffer, 1, &mesh_copy);
        vkCmdCopyBuffer(cb, vis_vertex.buffer, host.buffer, 1, &vertex_copy);
        vkCmdCopyBuffer(cb, vis_indirect.buffer, host.buffer, 1, &indirect_copy);
        vkCmdCopyBuffer(cb, args.buffer, host.buffer, 1, &args_copy);
        vkCmdCopyBuffer(cb, visible.buffer, host.buffer, 1, &visible_copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* mesh_out = static_cast<const u64*>(host.mapped);
  const u64* vertex_out = mesh_out + k_w * k_h;
  const u64* indirect_out = vertex_out + k_w * k_h;
  const auto* args_out = reinterpret_cast<const u32*>(indirect_out + k_w * k_h);
  const auto* visible_out = args_out + 4;  // uint2 per entry: {instance, cluster}
  CHECK(args_out[0] == triangles_per_cluster * 3);
  CHECK(args_out[1] > 0);
  CHECK(args_out[1] <= leaf_count);
  MESSAGE("indirect: " << args_out[1] << " of " << leaf_count << " leaf clusters in the frustum");

  // A visibility id names an entry of the visible list, not a cluster, so the indirect draw's
  // words differ from the direct draw's even where they name the same triangle. What must agree
  // is the surface: the (cluster, triangle) the id leads to, and the depth.
  auto surface_of = [&](u64 word, u64 offset) {
    const u32 id = static_cast<u32>(word);
    const u32 entry = id >> 8;
    const u32 cluster = offset == 0 ? entry : visible_out[entry * 2 + 1];
    return (u64{cluster} << 40) | (u64{id & 0xff} << 32) | (word >> 32);
  };
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 id_mismatch = 0;
  u32 indirect_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool in_vertex = vertex_out[i] != 0;
    covered += in_vertex;
    if (have_mesh) {
      const bool in_mesh = mesh_out[i] != 0;
      if (in_mesh != in_vertex) ++coverage_mismatch;
      if (in_mesh && in_vertex && static_cast<u32>(mesh_out[i]) != static_cast<u32>(vertex_out[i]))
        ++id_mismatch;
    }
    // The frustum-culled indirect draw must draw exactly what the direct draw drew.
    const bool in_indirect = indirect_out[i] != 0;
    if (in_indirect != in_vertex ||
        (in_indirect && surface_of(indirect_out[i], 1) != surface_of(vertex_out[i], 0))) {
      ++indirect_mismatch;
    }
  }
  CHECK(covered > k_w * k_h / 8);
  if (have_mesh) {
    CHECK(coverage_mismatch == 0);
    CHECK(id_mismatch * 500 <= covered);  // depth ties along shared edges differ between two draws
  }
  CHECK(indirect_mismatch * 500 <= covered);
  MESSAGE("covered " << covered << " px; mesh vs vertex coverage mismatch " << coverage_mismatch
                     << ", id mismatch " << id_mismatch << "; indirect mismatch "
                     << indirect_mismatch);

  graph.reset();
  if (mesh_pipeline != VK_NULL_HANDLE) gfx::destroy_pipeline(device, mesh_pipeline);
  if (mesh_module != VK_NULL_HANDLE) gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, cull_module);
  gfx::destroy_shader_module(device, vertex_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host, &params, &args, &visible, &vis_indirect, &vis_vertex,
                                 &vis_mesh, &lods, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

// What quantization costs the picture. Both rasterizers and the resolve read the same 16-bit
// stream, so comparing them against each other says nothing about the grid; this compares the
// grid against the floats it came from, on the CPU, through the projection the test above uses.
// The terrain is 20 units across, so the step is 20 / 65535 and no vertex may move a pixel.
TEST_CASE("vertex path: quantized positions project within a twentieth of a pixel") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  geometry::ClusterLodMesh lod;
  std::string error;
  REQUIRE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error));

  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const Mat4 view_proj =
      perspective_reversed_z(radians(60.0f), static_cast<f32>(k_w) / k_h, znear) *
      look_at(eye, Vec3{}, Vec3{0, 1, 0});
  f32 worst_px = 0.0f;
  u32 projected = 0;
  for (u32 v = 0; v < lod.mesh.vertices.size(); ++v) {
    const Vec4 exact = view_proj * Vec4{lod.mesh.vertices[v], 1.0f};
    const Vec4 grid = view_proj * Vec4{geometry::dequantize_position(lod.mesh, v), 1.0f};
    if (exact.w <= znear || grid.w <= znear) continue;
    ++projected;
    const f32 dx = std::fabs(exact.x / exact.w - grid.x / grid.w) * 0.5f * static_cast<f32>(k_w);
    const f32 dy = std::fabs(exact.y / exact.w - grid.y / grid.w) * 0.5f * static_cast<f32>(k_h);
    worst_px = dx > worst_px ? dx : worst_px;
    worst_px = dy > worst_px ? dy : worst_px;
  }
  CHECK(projected > lod.mesh.vertices.size() / 2);
  CHECK(lod.mesh.quant_scale < 3.1e-4f);  // 20 units over 65535 steps
  CHECK(worst_px < 0.05f);
  MESSAGE("grid step " << lod.mesh.quant_scale << " moves " << projected << " vertices by at most "
                       << worst_px << " px at " << k_w << "x" << k_h);
}
