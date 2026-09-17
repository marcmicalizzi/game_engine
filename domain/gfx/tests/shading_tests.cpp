// The material resolve's lighting model, checked against a second implementation: a horizontal
// quad under a known sun and a known point light, seen from above, shaded once per material for
// a sweep of roughness and metallic. Every render's center pixel is compared to the CPU
// reference of brdf_reference.h, which evaluates the same Cook-Torrance BSDF in double
// precision, so no expected value in this file is a number someone wrote down by hand. Also:
// the normals mode returns the plane normal, a light outside its radius of influence changes
// nothing at all, and a white dielectric lit head-on reflects what the BSDF says it should.
// Skips without mesh shaders or 64-bit buffer atomics.
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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

// The sweep: four roughnesses at both ends of the metallic range, all with the same base color.
constexpr f32 k_roughness[4] = {0.05f, 0.3f, 0.7f, 1.0f};
constexpr u32 k_materials = 8;  // k_roughness x {dielectric, conductor}
constexpr u32 k_white = 8;      // the reflectance-sanity material
constexpr u32 k_blocks = 12;    // one ResolveParams, one render target, per block
constexpr u32 k_size = 128;

}  // namespace

TEST_CASE("material resolve: shading matches a CPU reference over roughness and metallic") {
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
  gfx::BufferResource lights;
  const Vec4 base_color{0.8f, 0.4f, 0.2f, 0.0f};
  gfx::ResolveMaterial material_table[k_materials + 1];
  for (u32 i = 0; i < k_materials; ++i) {
    material_table[i].albedo = Vec4{base_color.xyz(), k_roughness[i % 4]};
    material_table[i].emissive = Vec4{0.0f, 0.0f, 0.0f, i < 4 ? 0.0f : 1.0f};
  }
  material_table[k_white].albedo = Vec4{1.0f, 1.0f, 1.0f, 1.0f};  // white, fully rough
  material_table[k_white].emissive = Vec4{};                      // dielectric
  // One material index per block, so a block selects its material by pointing the resolve at its
  // own word of this array; the quad is a single cluster.
  u32 material_index[k_materials + 1];
  for (u32 i = 0; i <= k_materials; ++i)
    material_index[i] = i;
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
  REQUIRE(gfx::upload_buffer(device, material_table, sizeof(material_table), k_storage, materials,
                             &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));

  // Two lights: one inside its radius of influence, one whose radius ends far short of the quad.
  gfx::ResolveLight light_table[2];
  light_table[0].position_radius = Vec4{3.0f, 4.0f, -2.0f, 12.0f};
  light_table[0].color_intensity = Vec4{1.0f, 0.85f, 0.7f, 40.0f};
  light_table[1].position_radius = Vec4{0.0f, 30.0f, 0.0f, 5.0f};  // 30 units away, radius 5
  light_table[1].color_intensity = Vec4{1.0f, 1.0f, 1.0f, 400.0f};
  REQUIRE(gfx::upload_buffer(device, light_table, sizeof(light_table), k_storage, lights, &error));

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
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_blocks,
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_blocks,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host_color, &error));

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
  const Vec3 sun_dir = normalize(Vec3{0.0f, 1.0f, 0.45f});  // 24 degrees off the quad's normal
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
  base.cluster_materials = cluster_materials.address;
  base.lights = lights.address;
  base.light_count = 1;
  base.width = k_size;
  base.height = k_size;
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  // Blocks 0..7: one material of the sweep each, sun plus the near light.
  for (u32 i = 0; i < k_materials; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + i * sizeof(u32);
  }
  blocks[8] = base;  // the normals view
  blocks[8].mode = static_cast<u32>(gfx::ResolveMode::Normals);
  blocks[9] = base;  // a light whose radius of influence ends 25 units short of the quad
  blocks[9].cluster_materials = cluster_materials.address + 3 * sizeof(u32);
  blocks[9].lights = lights.address + sizeof(gfx::ResolveLight);
  blocks[10] = blocks[9];  // the same material with no lights at all
  blocks[10].light_count = 0;
  blocks[11] = base;  // reflectance sanity: white, fully rough, head-on sun, no sky, no lights
  blocks[11].cluster_materials = cluster_materials.address + k_white * sizeof(u32);
  blocks[11].sun = Vec4{0.0f, 1.0f, 0.0f, 1.0f};
  blocks[11].sky = Vec4{};
  blocks[11].light_count = 0;
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
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
  for (u32 i = 0; i < k_blocks; ++i) {
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
        for (u32 i = 0; i < k_blocks; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < k_blocks; ++i) {
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
  // the plane's normal (this mesh has no vertex attributes), and the view from there.
  auto surface_at = [&](u32 x, u32 y, const gfx::ResolveMaterial& m) {
    ref::Surface s;
    s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = ref::Dvec3{0.0, 1.0, 0.0};
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(m.albedo);
    s.roughness = static_cast<double>(m.albedo.w);
    s.metallic = static_cast<double>(m.emissive.w);
    return s;
  };
  ref::Light near_light;
  near_light.position = ref::dvec3(light_table[0].position_radius);
  near_light.radius = static_cast<double>(light_table[0].position_radius.w);
  near_light.color = ref::dvec3(light_table[0].color_intensity);
  near_light.intensity = static_cast<double>(light_table[0].color_intensity.w);
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);

  // Every material of the sweep: the center pixel against the reference, within 2 of 255.
  const u32 cx = k_size / 2;
  const u32 cy = k_size / 2;
  int worst = 0;
  for (u32 i = 0; i < k_materials; ++i) {
    const ref::Surface s = surface_at(cx, cy, material_table[i]);
    const ref::Dvec3 linear = ref::shade(s, sun_ref, 1.0, sky_ref, &near_light, 1, ref::Dvec3{});
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    const u8* got = pixel(i, cx, cy);
    int worst_here = 0;
    for (u32 c = 0; c < 3; ++c)
      worst_here = std::max(worst_here, std::abs(int{got[c]} - int{expect[c]}));
    worst = std::max(worst, worst_here);
    CHECK_MESSAGE(worst_here <= 2, "roughness " << material_table[i].albedo.w << " metallic "
                                                << material_table[i].emissive.w << ": gpu "
                                                << int{got[0]} << "," << int{got[1]} << ","
                                                << int{got[2]} << " reference " << int{expect[0]}
                                                << "," << int{expect[1]} << "," << int{expect[2]});
    CHECK(got[3] == 255);
    MESSAGE("roughness " << material_table[i].albedo.w << " metallic "
                         << material_table[i].emissive.w << ": gpu " << int{got[0]} << ","
                         << int{got[1]} << "," << int{got[2]} << " reference " << int{expect[0]}
                         << "," << int{expect[1]} << "," << int{expect[2]} << " (max " << worst_here
                         << ")");
  }
  MESSAGE("worst reference-vs-GPU difference over the sweep: " << worst << " of 255");

  // The same comparison away from the center, where the light's falloff and the view angle
  // differ: a quarter of the way out along both axes, on the roughest dielectric.
  {
    const u32 ox = cx + k_size / 4;
    const u32 oy = cy + k_size / 8;
    const ref::Surface s = surface_at(ox, oy, material_table[3]);
    const ref::Dvec3 linear = ref::shade(s, sun_ref, 1.0, sky_ref, &near_light, 1, ref::Dvec3{});
    const u8* got = pixel(3, ox, oy);
    for (u32 c = 0; c < 3; ++c) {
      const u8 expect = ref::display(c == 0 ? linear.x : (c == 1 ? linear.y : linear.z));
      CHECK_MESSAGE(std::abs(int{got[c]} - int{expect}) <= 2, "off-center channel "
                                                                  << c << ": gpu " << int{got[c]}
                                                                  << " reference " << int{expect});
    }
  }

  // Coverage: the quad fills the middle of the frame and the corner is sky. Empty pixels take
  // the sky color straight, with no gamma: the shader returns params->sky.
  auto is_sky = [](const u8* p) -> bool {
    return std::abs(int{p[0]} - 51) <= 1 && std::abs(int{p[1]} - 77) <= 1 &&
           std::abs(int{p[2]} - 102) <= 1;
  };
  u32 covered = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (!is_sky(pixel(0, x, y))) ++covered;
    }
  }
  CHECK(covered > k_size * k_size / 3);
  CHECK(covered < k_size * k_size);
  CHECK(is_sky(pixel(0, 0, 0)));

  // Normals: the quad's plane normal (0, 1, 0) encodes as (128, 255, 128).
  const u8* normal = pixel(8, cx, cy);
  const bool normal_x = std::abs(int{normal[0]} - 128) <= 2;
  const bool normal_z = std::abs(int{normal[2]} - 128) <= 2;
  CHECK(normal_x);
  CHECK(int{normal[1]} >= 253);
  CHECK(normal_z);

  // A point light 30 units away with a radius of influence of 5 contributes nothing anywhere:
  // the windowed falloff is exactly zero past the radius, so the picture is the unlit one.
  u32 differing = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (std::memcmp(pixel(9, x, y), pixel(10, x, y), 4) != 0) ++differing;
    }
  }
  CHECK(differing == 0);

  // Reflectance sanity, stated by the reference rather than by hand: a white dielectric at
  // roughness 1, sun and view along the normal, no sky and no other light. The diffuse lobe
  // returns (1 - F) / pi with F = F0 = 0.04, and the GGX lobe adds D V F = (1/pi)(1/4)(0.04),
  // so the surface reflects 0.3087606 of the sun's unit radiance, 149 of 255 after the gamma.
  ref::Surface head_on;
  head_on.normal = ref::Dvec3{0.0, 1.0, 0.0};
  head_on.view = ref::Dvec3{0.0, 1.0, 0.0};
  head_on.albedo = ref::Dvec3{1.0, 1.0, 1.0};
  head_on.roughness = 1.0;
  head_on.metallic = 0.0;
  const ref::Dvec3 head_on_linear =
      ref::shade(head_on, ref::Dvec3{0.0, 1.0, 0.0}, 1.0, ref::Dvec3{}, nullptr, 0, ref::Dvec3{});
  CHECK(std::abs(head_on_linear.x - 0.30876059) < 1.0e-7);
  CHECK(ref::display(head_on_linear.x) == 149);
  const u8* white = pixel(11, cx, cy);
  for (u32 c = 0; c < 3; ++c)
    CHECK_MESSAGE(std::abs(int{white[c]} - 149) <= 2,
                  "white dielectric channel " << c << ": " << int{white[c]});
  MESSAGE("white dielectric, roughness 1, head-on unit sun: reference "
          << head_on_linear.x << " linear, " << int{ref::display(head_on_linear.x)}
          << " displayed; gpu " << int{white[0]} << "," << int{white[1]} << "," << int{white[2]});

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  gfx::destroy_pipeline(device, hw_pipeline);
  gfx::destroy_shader_module(device, resolve_module);
  gfx::destroy_shader_module(device, mesh_module);
  bindless.destroy();
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &lights, &cluster_materials,
                                 &materials, &mesh_buffer, &quantized, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
