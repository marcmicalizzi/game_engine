// Mesh-shader cluster rasterization end to end: build clusters on the CPU, upload them to
// device-address buffers, draw one workgroup per cluster into a visibility-ID target through
// the render graph, and check that every triangle's ID appears where the mesh covers pixels.
// This is the case about the mesh stage itself — its workgroup per cluster, and the
// `SV_PrimitiveID` it must write per primitive for the fragment stage to read — so it is the one
// raster case in this suite that needs VK_EXT_mesh_shader, and it skips naming it. Every case
// about the picture runs on the vertex path where mesh shaders are absent (raster_path.h).
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/cluster_mesh.spv.h>
#include <string>

using namespace engine;

namespace {

using MeshParams = gfx::ClusterDrawParams;  // the push constants of cluster_mesh.slang

// (n x n) vertices spanning [-extent, extent]^2 in clip space, z = 0.
void make_grid(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fy = -extent + 2.0f * extent * static_cast<f32>(y) / (n - 1);
      positions.push_back(Vec3{fx, fy, 0.0f});
    }
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

TEST_CASE("mesh shaders: clusters rasterize to visibility IDs") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::MeshShader})) return;

  // Geometry: a 17x17 grid (512 triangles) covering [-0.9, 0.9]^2 in clip space.
  Vector<Vec3> positions;
  Vector<u32> indices;
  constexpr u32 k_grid = 17;
  constexpr f32 k_extent = 0.9f;
  make_grid(k_grid, k_extent, positions, indices);
  geometry::ClusterMesh mesh;
  REQUIRE_MESSAGE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error),
      error);
  REQUIRE(geometry::validate_clusters(mesh, indices, geometry::ClusterBuildOptions{}, &error));
  CHECK(mesh.clusters.size() >= 5);
  MESSAGE("clusters: " << mesh.clusters.size());

  gfx::BufferResource cluster_buffer;
  gfx::BufferResource triangle_buffer;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             mesh.clusters.size() * sizeof(geometry::ClusterDesc),
                             gfx::BufferUsage::Storage, cluster_buffer, &error));
  REQUIRE(scene.create(device, mesh, mesh.clusters.size(), &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             gfx::BufferUsage::Storage, triangle_buffer, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  REQUIRE(bindless.capacity().push_constant_bytes >= sizeof(MeshParams));

  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::MeshPipelineDesc desc;
  desc.mesh = module;
  desc.fragment = module;
  desc.layout = bindless.pipeline_layout();
  desc.color_format = gfx::Format::R32Uint;
  gfx::PipelineHandle pipeline = {};
  REQUIRE_MESSAGE(gfx::create_mesh_pipeline(device, desc, pipeline, &error), error);

  constexpr u32 k_size = 128;
  const u64 bytes = u64{k_size} * k_size * 4;
  gfx::BufferResource readback;
  REQUIRE(gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferDst, true, readback, &error));

  gfx::RenderGraph graph(device);
  const gfx::RgImage ids = graph.create_image(
      "visibility", {k_size, k_size, gfx::Format::R32Uint,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  const gfx::RgBuffer host = graph.import_buffer("readback", readback);
  gfx::ClearColor clear{};
  clear.uint32[0] = 0xFFFFFFFFu;
  graph.add_pass(
      "clusters", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) { b.color_attachment(ids, gfx::LoadOp::Clear, clear); },
      [&](gfx::CommandList commands, gfx::RenderGraph&) {
        MeshParams params{};
        params.view_proj = Mat4::identity();
        params.clusters = cluster_buffer.address;
        params.mesh = scene.meshes.address;
        params.instances = scene.instances.address;
        params.triangles = triangle_buffer.address;
        commands.bind_pipeline(gfx::BindPoint::Graphics, pipeline);
        bindless.bind(commands, gfx::BindPoint::Graphics);
        commands.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                                sizeof(params), &params);
        commands.draw_mesh_tasks(mesh.clusters.size(), 1, 1);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(ids, gfx::Access::TransferRead);
        b.write(host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList commands, gfx::RenderGraph& g) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(gfx::vk::native(commands), gfx::vk::native(g.image(ids).image),
                               gfx::vk::native(g.image_layout(ids)),
                               gfx::vk::native(readback.buffer), 1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);

  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  const u64 value = frames.end_frame();
  REQUIRE(frames.wait(value));

  // Every pixel inside the grid carries a valid (cluster, triangle) id; every pixel outside is
  // the clear value; every triangle appears at least once.
  const auto* pixels = static_cast<const u32*>(readback.mapped);
  Vector<u32> hits(mesh.source_triangle_count);
  u32 bad_inside = 0;
  u32 bad_outside = 0;
  u32 covered = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const f32 cx = (static_cast<f32>(x) + 0.5f) / k_size * 2.0f - 1.0f;
      const f32 cy = (static_cast<f32>(y) + 0.5f) / k_size * 2.0f - 1.0f;
      const bool inside = cx > -k_extent && cx < k_extent && cy > -k_extent && cy < k_extent;
      const u32 id = pixels[y * k_size + x];
      if (inside) {
        ++covered;
        const u32 cluster = id >> 8;
        const u32 triangle = id & 0xff;
        if (id == 0xFFFFFFFFu || cluster >= mesh.clusters.size() ||
            triangle >= mesh.clusters[cluster].triangle_count) {
          ++bad_inside;
        } else {
          ++hits[mesh.clusters[cluster].triangle_offset + triangle];
        }
      } else if (id != 0xFFFFFFFFu) {
        ++bad_outside;
      }
    }
  }
  CHECK(bad_inside == 0);
  CHECK(bad_outside == 0);
  CHECK(covered > k_size * k_size * 7 / 10);
  u32 unseen = 0;
  for (u32 i = 0; i < hits.size(); ++i) {
    if (hits[i] == 0) ++unseen;
  }
  CHECK(unseen == 0);
  MESSAGE("covered pixels: " << covered << ", triangles: " << hits.size());

  graph.reset();
  gfx::destroy_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  gfx::destroy_buffer(device, readback);
  gfx::destroy_buffer(device, triangle_buffer);
  scene.destroy(device);
  gfx::destroy_buffer(device, cluster_buffer);
  frames.destroy();
  device.destroy();
}
