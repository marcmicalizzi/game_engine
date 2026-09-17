// The material resolve, checked numerically: a horizontal quad with a known albedo under a
// known sun, seen from straight above. The shaded mode must produce the Lambert-plus-ambient
// color the formula predicts after the display gamma, the normals mode must return the plane
// normal, a tilted sun must scale the direct term by its cosine, and pixels off the quad must
// show the sky. Skips without mesh shaders or 64-bit buffer atomics.
#include <domain/geometry/cluster.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <string>

using namespace engine;

namespace {

u8 display(f32 linear) {
  const f32 v = std::pow(linear < 0.0f ? 0.0f : linear, 1.0f / 2.2f);
  return static_cast<u8>(std::lround((v > 1.0f ? 1.0f : v) * 255.0f));
}

}  // namespace

TEST_CASE("material resolve: flat shading matches the formula, normals and sky are exact") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().mesh_shader || !device.features().buffer_int64_atomics) {
    MESSAGE("no mesh shaders or 64-bit buffer atomics on " << device.adapter().name);
    device.destroy();
    return;
  }

  // One quad on y = 0, two triangles, one cluster.
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  geometry::ClusterMesh mesh;
  REQUIRE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error));
  REQUIRE(mesh.clusters.size() == 1);

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource quantized;
  gfx::BufferResource mesh_buffer;
  gfx::BufferResource triangles;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx::ResolveMaterial material;
  material.albedo = Vec4{0.8f, 0.4f, 0.2f, 0.5f};
  const u32 zero = 0;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                             k_storage, quantized, &error));
  gfx::MeshDesc mesh_block{};
  mesh_block.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
  mesh_block.quantized = quantized.address;
  REQUIRE(
      gfx::upload_buffer(device, &mesh_block, sizeof(mesh_block), k_storage, mesh_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  REQUIRE(gfx::upload_buffer(device, &material, sizeof(material), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, &zero, sizeof(zero), k_storage, cluster_materials, &error));

  constexpr u32 k_size = 128;
  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Mat4 view_proj = perspective_reversed_z(radians(60.0f), 1.0f, 0.1f) *
                         look_at(eye, Vec3{}, Vec3{0.0f, 0.0f, -1.0f});
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource params;  // three ResolveParams: shaded, normals, tilted sun
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      false, vis, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * 3,
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule mesh_module = gfx::create_shader_module(
      device, shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size, &error);
  VkShaderModule resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(mesh_module != VK_NULL_HANDLE);
  REQUIRE(resolve_module != VK_NULL_HANDLE);
  gfx::MeshPipelineDesc hw_desc;
  hw_desc.mesh = mesh_module;
  hw_desc.fragment = mesh_module;
  hw_desc.fragment_entry = "fs_visibility";
  hw_desc.layout = bindless.pipeline_layout();
  VkPipeline hw_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_mesh_pipeline(device, hw_desc, hw_pipeline, &error), error);
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipeline resolve_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error),
                  error);

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = mesh_buffer.address;
  draw.triangles = triangles.address;
  draw.cluster_count = 1;
  draw.visibility = vis.address;
  draw.width = k_size;
  draw.height = k_size;

  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  gfx::ResolveParams base{};
  base.sky = sky;
  base.sun = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = clusters.address;
  base.mesh = mesh_buffer.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.cluster_materials = cluster_materials.address;
  base.width = k_size;
  base.height = k_size;
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  blocks[0] = base;
  blocks[0].mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  blocks[1] = base;
  blocks[1].mode = static_cast<u32>(gfx::ResolveMode::Normals);
  blocks[2] = base;
  blocks[2].sun = Vec4{0.0f, std::sqrt(0.5f), std::sqrt(0.5f), 1.0f};  // 45 degrees off the normal
  const u64 block_address[3] = {params.address, params.address + sizeof(gfx::ResolveParams),
                                params.address + 2 * sizeof(gfx::ResolveParams)};

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[3];
  for (u32 i = 0; i < 3; ++i) {
    targets[i] = graph.create_image(
        "resolved", {k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::TransferWrite); },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdFillBuffer(cb, vis.buffer, 0, VK_WHOLE_SIZE, 0);
      });
  graph.add_pass(
      "visibility", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_vis, gfx::Access::FragmentReadWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hw_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(draw),
                           &draw);
        vkCmdDrawMeshTasksEXT(cb, 1, 1, 1);
      });
  for (u32 i = 0; i < 3; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          VkClearColorValue clear{};
          b.color_attachment(targets[i], VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
          b.read(rg_vis, gfx::Access::FragmentRead);
        },
        [&, i](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(u64),
                             &block_address[i]);
          vkCmdDraw(cb, 3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 i = 0; i < 3; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < 3; ++i) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(cb, g.image(targets[i]).image, g.image_layout(targets[i]),
                                 host_color.buffer, 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  auto close = [](const u8* p, u8 r, u8 g, u8 b, int tolerance) {
    return std::abs(int{p[0]} - r) <= tolerance && std::abs(int{p[1]} - g) <= tolerance &&
           std::abs(int{p[2]} - b) <= tolerance;
  };

  // Shaded: albedo * (n.l * intensity + sky * 0.35 for an upward normal), then gamma.
  const f32 ambient_scale = 0.35f;
  const u8* center = pixel(0, k_size / 2, k_size / 2);
  const u8 expected_r = display(0.8f * (1.0f + sky.x * ambient_scale));
  const u8 expected_g = display(0.4f * (1.0f + sky.y * ambient_scale));
  const u8 expected_b = display(0.2f * (1.0f + sky.z * ambient_scale));
  CHECK_MESSAGE(close(center, expected_r, expected_g, expected_b, 3),
                "center " << int{center[0]} << "," << int{center[1]} << "," << int{center[2]}
                          << " expected " << int{expected_r} << "," << int{expected_g} << ","
                          << int{expected_b});
  CHECK(center[3] == 255);
  // Flat: every covered pixel of the quad shades the same.
  u32 covered = 0;
  u32 off = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* p = pixel(0, x, y);
      const bool is_sky =
          close(p, static_cast<u8>(sky.x * 255.0f + 0.5f), static_cast<u8>(sky.y * 255.0f + 0.5f),
                static_cast<u8>(sky.z * 255.0f + 0.5f), 1);
      if (is_sky) continue;
      ++covered;
      if (!close(p, expected_r, expected_g, expected_b, 3)) ++off;
    }
  }
  CHECK(covered > k_size * k_size / 3);
  CHECK(covered < k_size * k_size);
  CHECK(off == 0);
  // The corner is off the quad: sky.
  CHECK(close(pixel(0, 0, 0), 51, 77, 102, 1));

  // Normals: (0, 1, 0) encodes as (128, 255, 128).
  const u8* normal = pixel(1, k_size / 2, k_size / 2);
  CHECK(close(normal, 128, 255, 128, 2));

  // A 45 degree sun: direct term scaled by cos 45.
  const f32 cos45 = std::sqrt(0.5f);
  const u8* tilted = pixel(2, k_size / 2, k_size / 2);
  CHECK(close(tilted, display(0.8f * (cos45 + sky.x * ambient_scale)),
              display(0.4f * (cos45 + sky.y * ambient_scale)),
              display(0.2f * (cos45 + sky.z * ambient_scale)), 3));
  MESSAGE("shaded center " << int{center[0]} << "," << int{center[1]} << "," << int{center[2]}
                           << "; normal " << int{normal[0]} << "," << int{normal[1]} << ","
                           << int{normal[2]} << "; covered " << covered);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  gfx::destroy_pipeline(device, hw_pipeline);
  gfx::destroy_shader_module(device, resolve_module);
  gfx::destroy_shader_module(device, mesh_module);
  bindless.destroy();
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &cluster_materials, &materials,
                                 &mesh_buffer, &quantized, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
