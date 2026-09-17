// The visibility buffer written two ways: every leaf cluster of a terrain through the hardware
// path (mesh shader plus fs_visibility, an attachment-less raster pass) and through the software
// rasterizer (one compute workgroup per cluster). The two buffers must agree on coverage and,
// where both cover a pixel, on which triangle is nearest, up to the edge-rule differences along
// shared edges and silhouettes. Then the resolve pass turns the buffer into colors, and the GPU
// timer reports how long each path took. Skips without mesh shaders or 64-bit buffer atomics.
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/capture.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/visibility_resolve.spv.h>
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

TEST_CASE("visibility buffer: hardware and software rasterization agree, resolve shows it") {
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

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterMesh mesh;
  REQUIRE_MESSAGE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error),
      error);
  const u32 cluster_count = mesh.clusters.size();

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource quantized;
  gfx::BufferResource mesh_buffer;
  gfx::BufferResource triangles;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, mesh.quantized.data(), mesh.quantized.size() * sizeof(u16),
                             k_storage, quantized, &error));
  gfx::MeshDesc mesh_block{};
  mesh_block.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
  mesh_block.quantized = quantized.address;
  REQUIRE(
      gfx::upload_buffer(device, &mesh_block, sizeof(mesh_block), k_storage, mesh_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));

  // A camera where the triangles are a few pixels each: the software rasterizer's home turf.
  constexpr u32 k_size = 320;
  const Vec3 eye{0.0f, 14.0f, 30.0f};
  const Mat4 view_proj =
      perspective_reversed_z(radians(50.0f), 1.0f, 0.1f) * look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis_hw;
  gfx::BufferResource vis_sw;
  gfx::BufferResource host_hw;
  gfx::BufferResource host_sw;
  gfx::BufferResource host_color;
  const VkBufferUsageFlags k_vis = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_hw, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_sw, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host_hw,
                             &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host_sw,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::GpuTimer timer;
  REQUIRE_MESSAGE(timer.create(device, 2, 8, &error), error);
  REQUIRE(bindless.capacity().push_constant_bytes >= sizeof(gfx::ClusterDrawParams));

  VkShaderModule mesh_module = gfx::create_shader_module(
      device, shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size, &error);
  VkShaderModule sw_module = gfx::create_shader_module(
      device, shaders::k_cluster_sw_raster_spirv, shaders::k_cluster_sw_raster_spirv_size, &error);
  VkShaderModule resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(mesh_module != VK_NULL_HANDLE);
  REQUIRE(sw_module != VK_NULL_HANDLE);
  REQUIRE(resolve_module != VK_NULL_HANDLE);

  // Hardware path: mesh shader with the visibility fragment, no attachments.
  gfx::MeshPipelineDesc hw_desc;
  hw_desc.mesh = mesh_module;
  hw_desc.fragment = mesh_module;
  hw_desc.fragment_entry = "fs_visibility";
  hw_desc.layout = bindless.pipeline_layout();
  VkPipeline hw_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_mesh_pipeline(device, hw_desc, hw_pipeline, &error), error);
  // Software path: compute, same push constants.
  gfx::ComputePipeline sw_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, sw_module, "sw_raster_main", {},
                                               sizeof(gfx::ClusterDrawParams), sw_pipeline, &error),
                  error);
  // Resolve: fullscreen triangle into a color image.
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
  draw.cluster_count = cluster_count;
  draw.width = k_size;
  draw.height = k_size;
  gfx::ClusterDrawParams draw_hw = draw;
  draw_hw.visibility = vis_hw.address;
  gfx::ClusterDrawParams draw_sw = draw;
  draw_sw.visibility = vis_sw.address;
  // The resolve reads its parameters through an address; cluster colors need no materials.
  gfx::BufferResource resolve_params;
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                             resolve_params, &error));
  gfx::ResolveParams resolve{};
  resolve.visibility = vis_sw.address;
  resolve.width = k_size;
  resolve.height = k_size;
  resolve.mode = static_cast<u32>(gfx::ResolveMode::ClusterColors);
  resolve.sky = Vec4{0.0f, 0.0f, 1.0f, 1.0f};
  std::memcpy(resolve_params.mapped, &resolve, sizeof(resolve));
  const u64 resolve_address = resolve_params.address;

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_hw = graph.import_buffer("vis_hw", vis_hw);
  const gfx::RgBuffer rg_sw = graph.import_buffer("vis_sw", vis_sw);
  const gfx::RgBuffer rg_host_hw = graph.import_buffer("host_hw", host_hw);
  const gfx::RgBuffer rg_host_sw = graph.import_buffer("host_sw", host_sw);
  const gfx::RgBuffer rg_host_color = graph.import_buffer("host_color", host_color);
  const gfx::RgImage color = graph.create_image(
      "resolved", {k_size, k_size, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_hw, gfx::Access::TransferWrite);
        b.write(rg_sw, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdFillBuffer(cb, vis_hw.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, vis_sw.buffer, 0, VK_WHOLE_SIZE, 0);
      });
  graph.add_pass(
      "hardware", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_hw, gfx::Access::FragmentReadWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        timer.begin(cb, "hardware");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hw_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(draw_hw),
                           &draw_hw);
        vkCmdDrawMeshTasksEXT(cb, cluster_count, 1, 1);
        timer.end(cb);
      });
  graph.add_pass(
      "software", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(rg_sw, gfx::Access::ComputeReadWrite); },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        timer.begin(cb, "software");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sw_pipeline.pipeline);
        vkCmdPushConstants(cb, sw_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(draw_sw),
                           &draw_sw);
        vkCmdDispatch(cb, cluster_count, 1, 1);
        timer.end(cb);
      });
  graph.add_pass(
      "resolve", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        VkClearColorValue clear{};
        b.color_attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
        b.read(rg_sw, gfx::Access::FragmentRead);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0, sizeof(u64),
                           &resolve_address);
        vkCmdDraw(cb, 3, 1, 0, 0);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_hw, gfx::Access::TransferRead);
        b.read(rg_sw, gfx::Access::TransferRead);
        b.read(color, gfx::Access::TransferRead);
        b.write(rg_host_hw, gfx::Access::TransferWrite);
        b.write(rg_host_sw, gfx::Access::TransferWrite);
        b.write(rg_host_color, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph& g) {
        const VkBufferCopy copy{0, 0, vis_bytes};
        vkCmdCopyBuffer(cb, vis_hw.buffer, host_hw.buffer, 1, &copy);
        vkCmdCopyBuffer(cb, vis_sw.buffer, host_sw.buffer, 1, &copy);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(cb, g.image(color).image, g.image_layout(color), host_color.buffer,
                               1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);

  // Two frames so the timer has a completed frame to report.
  for (u32 frame = 0; frame < 2; ++frame) {
    VkCommandBuffer commands = frames.begin_frame();
    timer.begin_frame(commands, frames.slot());
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  frames.wait_idle();
  // Force the last frame's results through: begin another frame in the slot that just ran.
  {
    VkCommandBuffer commands = frames.begin_frame();
    timer.begin_frame(commands, frames.slot());
    frames.end_frame();
    frames.wait_idle();
  }
  CHECK(timer.results().size() == 2);
  MESSAGE("gpu ms: hardware " << timer.ms("hardware") << ", software " << timer.ms("software")
                              << " (" << cluster_count << " clusters, "
                              << mesh.source_triangle_count << " triangles at " << k_size << "x"
                              << k_size << ")");
  CHECK(timer.ms("hardware") > 0.0);
  CHECK(timer.ms("software") > 0.0);

  // Compare the two buffers.
  const auto* hw = static_cast<const u64*>(host_hw.mapped);
  const auto* sw = static_cast<const u64*>(host_sw.mapped);
  u32 covered_hw = 0;
  u32 covered_sw = 0;
  u32 coverage_mismatch = 0;
  u32 both = 0;
  u32 same_id = 0;
  u32 bad_id = 0;
  f64 depth_delta = 0.0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    const bool in_hw = hw[i] != 0;
    const bool in_sw = sw[i] != 0;
    covered_hw += in_hw;
    covered_sw += in_sw;
    if (in_hw != in_sw) ++coverage_mismatch;
    if (in_hw && in_sw) {
      ++both;
      const u32 id_hw = static_cast<u32>(hw[i] & 0xFFFFFFFFu);
      const u32 id_sw = static_cast<u32>(sw[i] & 0xFFFFFFFFu);
      if (id_hw == id_sw) ++same_id;
      f32 d_hw = 0.0f;
      f32 d_sw = 0.0f;
      const u32 hw_bits = static_cast<u32>(hw[i] >> 32);
      const u32 sw_bits = static_cast<u32>(sw[i] >> 32);
      std::memcpy(&d_hw, &hw_bits, 4);
      std::memcpy(&d_sw, &sw_bits, 4);
      depth_delta += std::fabs(static_cast<f64>(d_hw) - static_cast<f64>(d_sw));
    }
    if (in_sw && (static_cast<u32>(sw[i] & 0xFFFFFFFFu) >> 8) >= cluster_count) ++bad_id;
  }
  CHECK(covered_hw > k_size * k_size / 8);
  CHECK(coverage_mismatch * 100 < covered_hw);  // under 1% of pixels differ in coverage
  CHECK(same_id * 100 >= both * 90);            // 90%+ of shared pixels name the same triangle
  CHECK(depth_delta / static_cast<f64>(both) < 1.0e-3);  // depth agrees to a fraction of a mil
  CHECK(bad_id == 0);
  MESSAGE("covered hw " << covered_hw << ", sw " << covered_sw << ", coverage mismatch "
                        << coverage_mismatch << ", same id " << same_id << " of " << both);

  // The resolve: sky where the buffer is empty, a cluster color where it is not.
  const auto* pixels = static_cast<const u8*>(host_color.mapped);
  u32 resolve_mismatch = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    const u8* p = pixels + i * 4;
    const bool sky = p[0] == 0 && p[1] == 0 && p[2] == 255;
    if (sky == (sw[i] != 0)) ++resolve_mismatch;
  }
  CHECK(resolve_mismatch == 0);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  gfx::destroy_pipeline(device, hw_pipeline);
  gfx::destroy_compute_pipeline(device, sw_pipeline);
  gfx::destroy_shader_module(device, resolve_module);
  gfx::destroy_shader_module(device, sw_module);
  gfx::destroy_shader_module(device, mesh_module);
  timer.destroy();
  bindless.destroy();
  for (gfx::BufferResource* b : {&resolve_params, &host_color, &host_sw, &host_hw, &vis_sw, &vis_hw,
                                 &mesh_buffer, &quantized, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
