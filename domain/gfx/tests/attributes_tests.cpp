// Per-vertex attributes through the material resolve: a quad whose vertex normals are tilted
// 45 degrees must shade by those normals rather than its plane, the normals view must show them,
// and a 2x2 checker texture sampled by the quad's UVs through the bindless set must land in the
// right quadrants. The expected colors come from the CPU mirror of the BSDF (brdf_reference.h),
// the same reference the shading test uses, evaluated with the tilted normal and the sampled
// texel as albedo. Skips without mesh shaders or 64-bit buffer atomics.
#include "brdf_reference.h"

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
#include <cstdlib>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

TEST_CASE("material resolve: vertex normals steer the shading and textures sample by UV") {
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

  // A quad on y = 0 with vertex normals tilted 45 degrees towards +z and UVs spanning [0, 1].
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 tilted = normalize(Vec3{0.0f, 1.0f, 1.0f});
  const Vec3 normals[4] = {tilted, tilted, tilted, tilted};
  const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));
  REQUIRE(mesh.attributes.size() == mesh.vertices.size());

  // A 2x2 texture: red, green / blue, white, sampled nearest through the bindless set.
  const u8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
  gfx::ImageResource texture;
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, VK_FORMAT_R8G8B8A8_UNORM, texels,
                                       sizeof(texels), texture, &error),
                  error);
  VkImageView texture_view = VK_NULL_HANDLE;
  REQUIRE(gfx::create_image_view(device, texture, texture_view, &error));
  VkSampler nearest = VK_NULL_HANDLE;
  REQUIRE(gfx::create_sampler(device, VK_FILTER_NEAREST, nearest, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 texture_slot =
      bindless.add_sampled_image(texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const u32 sampler_slot = bindless.add_sampler(nearest);

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource quantized;
  gfx::BufferResource mesh_buffer;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes;
  gfx::BufferResource materials;
  gfx::BufferResource plain_index;
  gfx::BufferResource textured_index;
  gfx::ResolveMaterial material_set[2];
  material_set[0].albedo = Vec4{0.8f, 0.4f, 0.2f, 0.5f};  // untextured
  material_set[1].albedo = Vec4{1.0f, 1.0f, 1.0f, 0.5f};  // white times the checker texture
  material_set[1].albedo_texture = texture_slot;
  material_set[1].sampler = sampler_slot;
  const u32 zero = 0;
  const u32 one = 1;
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
  REQUIRE(gfx::upload_buffer(device, mesh.attributes.data(),
                             mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                             attributes, &error));
  REQUIRE(
      gfx::upload_buffer(device, material_set, sizeof(material_set), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, &zero, sizeof(zero), k_storage, plain_index, &error));
  REQUIRE(gfx::upload_buffer(device, &one, sizeof(one), k_storage, textured_index, &error));

  constexpr u32 k_size = 128;
  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource params;
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
  REQUIRE(gfx::create_mesh_pipeline(device, hw_desc, hw_pipeline, &error));
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipeline resolve_pipeline = VK_NULL_HANDLE;
  REQUIRE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error));

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
  const Vec3 sun_dir{0.0f, 1.0f, 0.0f};
  gfx::ResolveParams base{};
  base.sky = sky;
  base.sun = Vec4{sun_dir, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = clusters.address;
  base.mesh = mesh_buffer.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.cluster_materials = plain_index.address;
  base.attributes = attributes.address;
  base.width = k_size;
  base.height = k_size;
  // Block 0: shaded, untextured. Block 1: normals. Block 2: shaded with the texture.
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  blocks[0] = base;
  blocks[0].mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  blocks[1] = base;
  blocks[1].mode = static_cast<u32>(gfx::ResolveMode::Normals);
  blocks[2] = base;
  blocks[2].mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  blocks[2].cluster_materials = textured_index.address;
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
  // The reference surface at a pixel: the point the resolve reconstructs on the quad's plane,
  // the tilted vertex normal (the same at every vertex, so interpolation cannot change it), the
  // view from there, and the albedo the material and the texture produce together.
  auto expect_pixel = [&](u32 image, u32 x, u32 y, Vec3 albedo, const std::string& what) {
    ref::Surface s;
    s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = ref::normalize(ref::dvec3(tilted));
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(albedo);
    s.roughness = static_cast<double>(material_set[0].albedo.w);
    s.metallic = 0.0;
    const ref::Dvec3 linear =
        ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky), nullptr, 0, ref::Dvec3{});
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    const u8* p = pixel(image, x, y);
    const bool matches = std::abs(int{p[0]} - int{expect[0]}) <= 2 &&
                         std::abs(int{p[1]} - int{expect[1]}) <= 2 &&
                         std::abs(int{p[2]} - int{expect[2]}) <= 2;
    CHECK_MESSAGE(matches, what << " at " << x << "," << y << ": gpu " << int{p[0]} << ","
                                << int{p[1]} << "," << int{p[2]} << " reference " << int{expect[0]}
                                << "," << int{expect[1]} << "," << int{expect[2]});
  };

  const u32 cx = k_size / 2;
  const u32 cy = k_size / 2;
  // Shaded by the tilted normal, not by the plane: the plane would give a different cosine, a
  // different half vector, and a different hemisphere elevation, all of which the reference sees.
  expect_pixel(0, cx, cy, material_set[0].albedo.xyz(), "tilted normal");
  const f32 cos45 = std::sqrt(0.5f);
  const u8 half_up = static_cast<u8>(std::lround((cos45 * 0.5f + 0.5f) * 255.0f));
  const u8* normal = pixel(1, cx, cy);
  const bool normal_ok = std::abs(int{normal[0]} - 128) <= 2 &&
                         std::abs(int{normal[1]} - int{half_up}) <= 2 &&
                         std::abs(int{normal[2]} - int{half_up}) <= 2;
  CHECK_MESSAGE(normal_ok,
                "normal " << int{normal[0]} << "," << int{normal[1]} << "," << int{normal[2]});

  // The texture: u grows with +x (screen right); v grows with +z, which this camera (looking down
  // -y with -z as up) maps to screen down. Sample a quarter of the quad's footprint from center.
  // The material is white, so the sampled texel is the albedo the reference shades.
  const u32 quarter = static_cast<u32>(5.0f / (10.0f * std::tan(radians(30.0f))) * k_size * 0.25f);
  expect_pixel(2, cx - quarter, cy - quarter, Vec3{1.0f, 0.0f, 0.0f}, "red texel");
  expect_pixel(2, cx + quarter, cy - quarter, Vec3{0.0f, 1.0f, 0.0f}, "green texel");
  expect_pixel(2, cx - quarter, cy + quarter, Vec3{0.0f, 0.0f, 1.0f}, "blue texel");
  expect_pixel(2, cx + quarter, cy + quarter, Vec3{1.0f, 1.0f, 1.0f}, "white texel");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  gfx::destroy_pipeline(device, hw_pipeline);
  gfx::destroy_shader_module(device, resolve_module);
  gfx::destroy_shader_module(device, mesh_module);
  bindless.destroy();
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_image_view(device, texture_view);
  gfx::destroy_image(device, texture);
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &textured_index, &plain_index, &materials, &attributes,
        &triangles, &mesh_buffer, &quantized, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
