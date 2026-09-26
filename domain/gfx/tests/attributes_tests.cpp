// Per-vertex attributes and the material textures they address, through the material resolve. A
// quad whose vertex normals are tilted 45 degrees must shade by those normals rather than its
// plane, the normals view must show them, and a 2x2 checker texture sampled by the quad's UVs
// through the bindless set must land in the right quadrants; then a flat quad must take its
// roughness and metallic from a 2x2 metallic-roughness map, and its shading normal from a
// tangent-space normal map. The expected colors come from the CPU mirror of the BSDF
// (brdf_reference.h), the same reference the shading test uses, evaluated with the normal, the
// roughness, the metallic, and the albedo the maps produce. The quads are drawn through the
// device's own raster path (raster_path.h) — the vertex path, the baseline tier, where there are
// no mesh shaders — so both tiers are held to the reference. Skips only without 64-bit buffer
// atomics.
#include "brdf_reference.h"
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <shaders/visibility_resolve.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

TEST_CASE("material resolve: vertex normals steer the shading and textures sample by UV") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

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
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, texels,
                                       sizeof(texels), texture, &error),
                  error);
  gfx::ImageViewHandle texture_view = {};
  REQUIRE(gfx::create_image_view(device, texture, texture_view, &error));
  gfx::SamplerHandle nearest = {};
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, nearest, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 texture_slot =
      bindless.add_sampled_image(texture_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 sampler_slot = bindless.add_sampler(nearest);

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx_test::SingleInstance scene;
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
  REQUIRE(scene.create(device, mesh, 1, &error));
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
  constexpr u32 k_blocks = 4;
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
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_blocks,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(resolve_module.valid());
  gfx_test::ClusterRaster raster;  // the mesh path, or the vertex path without mesh shaders
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                geometry::ClusterBuildOptions{}.max_triangles, &error),
                  error);
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = gfx::Format::R8G8B8A8Unorm;
  gfx::PipelineHandle resolve_pipeline = {};
  REQUIRE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error));

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
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
  base.mesh = scene.meshes.address;
  base.instances = scene.instances.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.cluster_materials = plain_index.address;
  base.attributes = attributes.address;
  base.width = k_size;
  base.height = k_size;
  // Block 0: shaded, untextured. Block 1: normals. Block 2: shaded with the texture. Block 3:
  // block 0 over a sand-coloured ground instead of the neutral default — the tilted normal sees
  // (1 - cos 45°) / 2 of its cosine lobe below the horizon, and that share is the ground's.
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  blocks[0] = base;
  blocks[0].mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  blocks[1] = base;
  blocks[1].mode = static_cast<u32>(gfx::ResolveMode::Normals);
  blocks[2] = base;
  blocks[2].mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  blocks[2].cluster_materials = textured_index.address;
  blocks[3] = blocks[0];
  blocks[3].ground = Vec4{0.84f, 0.69f, 0.47f, 0.0f};
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    targets[i] = graph.create_image(
        "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::TransferWrite); },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "visibility", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_vis, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw, 1); });
  for (u32 i = 0; i < k_blocks; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          gfx::ClearColor clear{};
          b.color_attachment(targets[i], gfx::LoadOp::Clear, clear);
          b.read(rg_vis, gfx::Access::FragmentRead);
        },
        [&, i](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, resolve_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                            &block_address[i]);
          cb.draw(3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 i = 0; i < k_blocks; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < k_blocks; ++i) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[i]).image),
                                 gfx::vk::native(g.image_layout(targets[i])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  // The reference surface at a pixel: the point the resolve reconstructs on the quad's plane,
  // the tilted vertex normal (the same at every vertex, so interpolation cannot change it), the
  // view from there, and the albedo the material and the texture produce together, under the
  // block's own ground.
  int worst = 0;
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
        ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky), ref::dvec3(blocks[image].ground),
                   nullptr, 0, ref::Dvec3{});
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    const u8* p = pixel(image, x, y);
    for (u32 c = 0; c < 3; ++c)
      worst = std::max(worst, std::abs(int{p[c]} - int{expect[c]}));
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

  // The ground's share: the same tilted surface over a sand-coloured ground is the reference with
  // that ground, and it is not the neutral ground's picture — a seventh of the tilted normal's
  // cosine lobe is below the horizon, and it lands on the pixel.
  expect_pixel(3, cx, cy, material_set[0].albedo.xyz(), "tilted normal over sand");
  const u8* neutral = pixel(0, cx, cy);
  const u8* sand = pixel(3, cx, cy);
  CHECK(int{sand[0]} - int{neutral[0]} >= 4);
  MESSAGE("tilted normal: neutral ground gpu "
          << int{neutral[0]} << "," << int{neutral[1]} << "," << int{neutral[2]}
          << ", sand ground gpu " << int{sand[0]} << "," << int{sand[1]} << "," << int{sand[2]});

  // The texture: u grows with +x (screen right); v grows with +z, which this camera (looking down
  // -y with -z as up) maps to screen down. Sample a quarter of the quad's footprint from center.
  // The material is white, so the sampled texel is the albedo the reference shades.
  const u32 quarter = static_cast<u32>(5.0f / (10.0f * std::tan(radians(30.0f))) * k_size * 0.25f);
  expect_pixel(2, cx - quarter, cy - quarter, Vec3{1.0f, 0.0f, 0.0f}, "red texel");
  expect_pixel(2, cx + quarter, cy - quarter, Vec3{0.0f, 1.0f, 0.0f}, "green texel");
  expect_pixel(2, cx - quarter, cy + quarter, Vec3{0.0f, 0.0f, 1.0f}, "blue texel");
  expect_pixel(2, cx + quarter, cy + quarter, Vec3{1.0f, 1.0f, 1.0f}, "white texel");
  MESSAGE("worst reference-vs-GPU difference over the tilted normal and the texels, "
          << std::string(raster.name()) << " path: " << worst << " of 255 (tolerance 2)");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_image_view(device, texture_view);
  gfx::destroy_image(device, texture);
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &textured_index, &plain_index,
                                 &materials, &attributes, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

// The two data textures of the material path, on a quad whose vertex normals are the plane's, so
// that everything the shading does to the normal comes from the map and not from the geometry: a
// 2x2 metallic-roughness map (glTF packs roughness in G and metallic in B) whose quadrants are a
// rough dielectric, a smooth dielectric, a rough metal, and a smooth metal, and a constant normal
// map tilted 45 degrees about the tangent axis, once at normal_scale 1 and once at 0. The
// expected colors again come from brdf_reference.h, now with the quadrant's roughness and
// metallic and with the tangent frame and the perturbed normal it computes the way the shader
// does. Then the two ways a shading normal parts from its surface: a map tilted 80 degrees, whose
// normal faces away from the camera over half the quad while it faces the sun (n.v is clamped,
// so the sun stays), and the quad seen from below, whose normals are behind it (turned to the
// camera's side, so the sun does not come through). Through the device's raster path, as above;
// skips only without 64-bit buffer atomics.
TEST_CASE("material resolve: metallic-roughness and normal maps") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // The same quad as above, but with the plane's own normal at every vertex.
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 flat{0.0f, 1.0f, 0.0f};
  const Vec3 normals[4] = {flat, flat, flat, flat};
  const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));

  // The metallic-roughness map: R unused, G roughness, B metallic, A unused. Texel order is
  // screen top-left, top-right, bottom-left, bottom-right, as the checker above established.
  constexpr u8 k_rough = 230;      // 0.902 perceptual roughness
  constexpr u8 k_smooth = 38;      // 0.149
  constexpr u8 k_conductor = 255;  // metallic 1
  constexpr u8 k_dielectric = 0;   // metallic 0
  const u8 mr_texels[16] = {0, k_rough, k_dielectric, 255, 0, k_smooth, k_dielectric, 255,
                            0, k_rough, k_conductor,  255, 0, k_smooth, k_conductor,  255};
  // The normal map: one tangent-space direction everywhere, 45 degrees from the surface normal
  // about the tangent axis, which is +x here, so it leans the shading normal towards +z.
  constexpr u8 k_zero = 128;  // 0 remapped to 0..255
  constexpr u8 k_cos45 = 218;
  const u8 normal_texels[16] = {k_zero, k_cos45, k_cos45, 255, k_zero, k_cos45, k_cos45, 255,
                                k_zero, k_cos45, k_cos45, 255, k_zero, k_cos45, k_cos45, 255};
  // A steep one: 80 degrees about the same axis, so the shading normal leans so far towards +z
  // that over the far half of the quad it faces away from the camera above it while still facing
  // the sun overhead (n.v < 0 < n.l) — the grazing-view case of the dunes' grey seams.
  constexpr u8 k_sin80 = 253;
  constexpr u8 k_cos80 = 150;
  const u8 steep_texels[16] = {k_zero, k_sin80, k_cos80, 255, k_zero, k_sin80, k_cos80, 255,
                               k_zero, k_sin80, k_cos80, 255, k_zero, k_sin80, k_cos80, 255};
  gfx::ImageResource mr_image;
  gfx::ImageResource normal_image;
  gfx::ImageResource steep_image;
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, mr_texels,
                                       sizeof(mr_texels), mr_image, &error),
                  error);
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, normal_texels,
                                       sizeof(normal_texels), normal_image, &error),
                  error);
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, steep_texels,
                                       sizeof(steep_texels), steep_image, &error),
                  error);
  gfx::ImageViewHandle mr_view = {};
  gfx::ImageViewHandle normal_view = {};
  gfx::ImageViewHandle steep_view = {};
  REQUIRE(gfx::create_image_view(device, mr_image, mr_view, &error));
  REQUIRE(gfx::create_image_view(device, normal_image, normal_view, &error));
  REQUIRE(gfx::create_image_view(device, steep_image, steep_view, &error));
  gfx::SamplerHandle nearest = {};
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, nearest, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 mr_slot = bindless.add_sampled_image(mr_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 normal_slot = bindless.add_sampled_image(normal_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 steep_slot = bindless.add_sampled_image(steep_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 sampler_slot = bindless.add_sampler(nearest);

  // Five materials, one per block: plain, metallic-roughness mapped (both factors 1, so the
  // texture passes straight through), normal mapped, normal mapped with the scale at zero, and
  // the steep normal map. A sixth block draws the plain material from below.
  constexpr u32 k_materials = 5;
  constexpr u32 k_blocks = 6;
  constexpr u32 k_below = 5;
  const Vec3 base_color{0.8f, 0.6f, 0.3f};
  constexpr f32 k_plain_roughness = 0.6f;
  gfx::ResolveMaterial material_set[k_materials];
  for (gfx::ResolveMaterial& material : material_set) {
    material.albedo = Vec4{base_color, k_plain_roughness};
    material.emissive = Vec4{};  // dielectric
    material.sampler = sampler_slot;
  }
  material_set[1].albedo = Vec4{base_color, 1.0f};          // the map is the whole roughness
  material_set[1].emissive = Vec4{0.0f, 0.0f, 0.0f, 1.0f};  // ... and the whole metallic
  material_set[1].metallic_roughness_texture = mr_slot;
  material_set[2].normal_texture = normal_slot;
  material_set[2].normal_scale = 1.0f;
  material_set[3].normal_texture = normal_slot;
  material_set[3].normal_scale = 0.0f;
  material_set[4].normal_texture = steep_slot;
  material_set[4].normal_scale = 1.0f;
  u32 material_index[k_materials];
  for (u32 i = 0; i < k_materials; ++i)
    material_index[i] = i;

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx_test::SingleInstance scene;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(scene.create(device, mesh, 1, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.attributes.data(),
                             mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                             attributes, &error));
  REQUIRE(
      gfx::upload_buffer(device, material_set, sizeof(material_set), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));

  constexpr u32 k_size = 128;
  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  // The same quad from below, into a visibility buffer of its own: its vertex normals still point
  // up, away from this camera and through the quad at the sun.
  const Vec3 eye_below{0.0f, -10.0f, 0.0f};
  const Mat4 view_proj_below =
      perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye_below, target, up);
  gfx::BufferResource vis;
  gfx::BufferResource vis_below;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, u64{k_size} * k_size * sizeof(u64),
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(
      device, u64{k_size} * k_size * sizeof(u64),
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false,
      vis_below, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_blocks,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(resolve_module.valid());
  gfx_test::ClusterRaster raster;  // the mesh path, or the vertex path without mesh shaders
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                geometry::ClusterBuildOptions{}.max_triangles, &error),
                  error);
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = gfx::Format::R8G8B8A8Unorm;
  gfx::PipelineHandle resolve_pipeline = {};
  REQUIRE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error));

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.visibility = vis.address;
  draw.width = k_size;
  draw.height = k_size;
  gfx::ClusterDrawParams draw_below = draw;
  draw_below.view_proj = view_proj_below;
  draw_below.visibility = vis_below.address;

  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const Vec3 sun_dir{0.0f, 1.0f, 0.0f};
  gfx::ResolveParams base{};
  base.sky = sky;
  base.sun = Vec4{sun_dir, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = clusters.address;
  base.mesh = scene.meshes.address;
  base.instances = scene.instances.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.attributes = attributes.address;
  base.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  base.width = k_size;
  base.height = k_size;
  // One block per material: the quad is a single cluster, so a block selects its material by
  // pointing the resolve at its own word of the cluster-material array. The last block is the
  // plain material from below.
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + (i % k_materials) * sizeof(u32);
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);
  }
  blocks[k_below].camera = Vec4{eye_below, 0.0f};
  blocks[k_below].view_proj = view_proj_below;
  blocks[k_below].visibility = vis_below.address;

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_vis_below = graph.import_buffer("vis below", vis_below);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    targets[i] = graph.create_image(
        "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_vis, gfx::Access::TransferWrite);
        b.write(rg_vis_below, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_below.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "visibility", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_vis, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw, 1); });
  graph.add_pass(
      "visibility below", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_vis_below, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw_below, 1); });
  for (u32 i = 0; i < k_blocks; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          gfx::ClearColor clear{};
          b.color_attachment(targets[i], gfx::LoadOp::Clear, clear);
          b.read(i == k_below ? rg_vis_below : rg_vis, gfx::Access::FragmentRead);
        },
        [&, i](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, resolve_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                            &block_address[i]);
          cb.draw(3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 i = 0; i < k_blocks; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < k_blocks; ++i) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[i]).image),
                                 gfx::vk::native(g.image_layout(targets[i])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  // The tangent frame the resolve solves for, from the quad's own corners and UVs: +x and +z
  // here, which is what makes the map's y lean the shading normal towards +z.
  const ref::Dvec3 corner[3] = {ref::dvec3(positions[0]), ref::dvec3(positions[1]),
                                ref::dvec3(positions[2])};
  ref::Dvec3 tangent;
  ref::Dvec3 bitangent;
  REQUIRE(ref::tangent_frame(corner, uvs, ref::dvec3(flat), tangent, bitangent));
  int worst = 0;
  auto expect_pixel = [&](u32 image, u32 x, u32 y, ref::Dvec3 normal, double roughness,
                          double metallic, const std::string& what) {
    ref::Surface s;
    s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = normal;
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(base_color);
    s.roughness = roughness;
    s.metallic = metallic;
    const ref::Dvec3 linear = ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky),
                                         ref::dvec3(base.ground), nullptr, 0, ref::Dvec3{});
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    const u8* p = pixel(image, x, y);
    int here = 0;
    for (u32 c = 0; c < 3; ++c)
      here = std::max(here, std::abs(int{p[c]} - int{expect[c]}));
    worst = std::max(worst, here);
    CHECK_MESSAGE(here <= 2, what << " at " << x << "," << y << ": gpu " << int{p[0]} << ","
                                  << int{p[1]} << "," << int{p[2]} << " reference "
                                  << int{expect[0]} << "," << int{expect[1]} << ","
                                  << int{expect[2]});
    MESSAGE(what << ": gpu " << int{p[0]} << "," << int{p[1]} << "," << int{p[2]} << " reference "
                 << int{expect[0]} << "," << int{expect[1]} << "," << int{expect[2]} << " (max "
                 << here << ")");
  };

  const u32 cx = k_size / 2;
  const u32 cy = k_size / 2;
  const u32 quarter = static_cast<u32>(5.0f / (10.0f * std::tan(radians(30.0f))) * k_size * 0.25f);
  const ref::Dvec3 flat_normal = ref::dvec3(flat);
  // The metallic-roughness map, quadrant by quadrant: G is the roughness, B the metallic, and
  // both multiply factors of 1, so the reference shades exactly what the texel says.
  const double rough = static_cast<double>(k_rough) / 255.0;
  const double smooth = static_cast<double>(k_smooth) / 255.0;
  expect_pixel(1, cx - quarter, cy - quarter, flat_normal, rough, 0.0, "rough dielectric");
  expect_pixel(1, cx + quarter, cy - quarter, flat_normal, smooth, 0.0, "smooth dielectric");
  expect_pixel(1, cx - quarter, cy + quarter, flat_normal, rough, 1.0, "rough metal");
  expect_pixel(1, cx + quarter, cy + quarter, flat_normal, smooth, 1.0, "smooth metal");
  // The four quadrants must not be one picture, or the comparisons above prove nothing.
  const u8* rough_metal = pixel(1, cx - quarter, cy + quarter);
  const u8* smooth_metal = pixel(1, cx + quarter, cy + quarter);
  const u8* rough_dielectric = pixel(1, cx - quarter, cy - quarter);
  CHECK(std::abs(int{rough_metal[0]} - int{smooth_metal[0]}) > 8);
  CHECK(std::abs(int{rough_metal[0]} - int{rough_dielectric[0]}) > 8);

  // The normal map: the shading normal is the map's direction in the quad's tangent frame, and
  // it is not the plane's, which the difference against the unmapped render shows.
  const ref::Dvec3 mapped = ref::map_normal(flat_normal, tangent, bitangent, normal_texels, 1.0);
  expect_pixel(2, cx, cy, mapped, static_cast<double>(k_plain_roughness), 0.0, "normal map");
  MESSAGE("the normal map tilts the shading normal to " << mapped.x << "," << mapped.y << ","
                                                        << mapped.z);
  const u8* plain_center = pixel(0, cx, cy);
  const u8* mapped_center = pixel(2, cx, cy);
  CHECK(std::abs(int{plain_center[0]} - int{mapped_center[0]}) > 8);

  // normal_scale 0 scales the map's tangential part away, so the render is the unmapped one.
  expect_pixel(0, cx, cy, flat_normal, static_cast<double>(k_plain_roughness), 0.0, "plain");
  expect_pixel(3, cx, cy, flat_normal, static_cast<double>(k_plain_roughness), 0.0,
               "normal map at scale 0");
  int scale_zero = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* a = pixel(0, x, y);
      const u8* b = pixel(3, x, y);
      for (u32 c = 0; c < 3; ++c)
        scale_zero = std::max(scale_zero, std::abs(int{a[c]} - int{b[c]}));
    }
  }
  CHECK_MESSAGE(scale_zero <= 1, "normal_scale 0 changed the picture by " << scale_zero);
  MESSAGE("normal_scale 0 against no normal map at all: worst pixel difference " << scale_zero
                                                                                 << " of 255");

  // **A shading normal turned from the camera keeps the sun.** The steep map leans the normal
  // 80 degrees towards +z, so wherever the view comes from +z of it — the far half of the quad —
  // n.v is negative while n.l is 0.17. The BSDF clamps n.v rather than rejecting it, so every
  // covered pixel is the reference, which clamps it too; and on the pixels whose n.v is negative
  // the picture is brighter than the same surface with no sun, which is the whole difference from
  // a BSDF that returned nothing there and left the flat grey of the dunes' seams.
  const ref::Dvec3 steep = ref::map_normal(flat_normal, tangent, bitangent, steep_texels, 1.0);
  u32 steep_covered = 0;
  u32 facing_away = 0;
  u32 away_unlit = 0;
  int steep_worst = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* got = pixel(4, x, y);
      if (std::abs(int{got[0]} - 51) <= 1 && std::abs(int{got[1]} - 77) <= 1 &&
          std::abs(int{got[2]} - 102) <= 1) {
        continue;  // the sky
      }
      ++steep_covered;
      ref::Surface s;
      s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                       static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
      s.normal = steep;
      s.view = ref::normalize(ref::dvec3(eye) - s.position);
      s.albedo = ref::dvec3(base_color);
      s.roughness = static_cast<double>(k_plain_roughness);
      s.metallic = 0.0;
      const ref::Dvec3 lit = ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky),
                                        ref::dvec3(base.ground), nullptr, 0, ref::Dvec3{});
      int here = 0;
      here = std::max(here, std::abs(int{got[0]} - int{ref::display(lit.x)}));
      here = std::max(here, std::abs(int{got[1]} - int{ref::display(lit.y)}));
      here = std::max(here, std::abs(int{got[2]} - int{ref::display(lit.z)}));
      steep_worst = std::max(steep_worst, here);
      if (ref::dot(s.normal, s.view) < 0.0) {
        ++facing_away;
        // The same pixel with the sun's direct term taken out and its ground bounce kept: what a
        // BSDF that rejected n.v <= 0 would have drawn. It must be clearly darker.
        const ref::Dvec3 sunless =
            ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky), ref::dvec3(base.ground),
                       nullptr, 0, ref::Dvec3{}, true);
        if (int{got[0]} < int{ref::display(sunless.x)} + 8) ++away_unlit;
      }
    }
  }
  CHECK_MESSAGE(steep_worst <= 2, "steep normal map: worst " << steep_worst << " of 255");
  CHECK(facing_away > steep_covered / 4);
  CHECK_MESSAGE(away_unlit == 0u, away_unlit << " pixels facing away from the camera lost the sun");
  worst = std::max(worst, steep_worst);
  MESSAGE("steep normal map: " << steep_covered << " covered pixels, " << facing_away
                               << " with n.v < 0, all lit by the sun; worst " << steep_worst
                               << " of 255 against the reference");

  // **A shading normal behind the surface is turned to the camera's side**, as the reference path
  // tracer turns it. From below, the quad's vertex normals point up — behind the surface as this
  // camera sees it, and straight at the sun. Turned, the normal faces the ground, which is all that
  // lights it: every covered pixel is the reference for a normal of (0, -1, 0). Left alone, the
  // clamped n.v would have let the sun through the quad.
  u32 below_covered = 0;
  int below_worst = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* got = pixel(k_below, x, y);
      if (std::abs(int{got[0]} - 51) <= 1 && std::abs(int{got[1]} - 77) <= 1 &&
          std::abs(int{got[2]} - 102) <= 1) {
        continue;  // the sky
      }
      ++below_covered;
      ref::Surface s;
      s.position = ref::pixel_on_plane(ref::dvec3(eye_below), ref::dvec3(target), ref::dvec3(up),
                                       static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
      s.normal = ref::Dvec3{0.0, -1.0, 0.0};
      s.view = ref::normalize(ref::dvec3(eye_below) - s.position);
      s.albedo = ref::dvec3(base_color);
      s.roughness = static_cast<double>(k_plain_roughness);
      s.metallic = 0.0;
      const ref::Dvec3 lit = ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky),
                                        ref::dvec3(base.ground), nullptr, 0, ref::Dvec3{});
      int here = 0;
      here = std::max(here, std::abs(int{got[0]} - int{ref::display(lit.x)}));
      here = std::max(here, std::abs(int{got[1]} - int{ref::display(lit.y)}));
      here = std::max(here, std::abs(int{got[2]} - int{ref::display(lit.z)}));
      below_worst = std::max(below_worst, here);
    }
  }
  CHECK(below_covered > k_size * k_size / 3);
  CHECK_MESSAGE(below_worst <= 2, "from below: worst " << below_worst << " of 255");
  worst = std::max(worst, below_worst);
  MESSAGE("from below, the normal behind the surface turned: "
          << below_covered << " covered pixels, worst " << below_worst << " of 255");
  MESSAGE("worst reference-vs-GPU difference over the maps, "
          << std::string(raster.name()) << " path: " << worst << " of 255 (tolerance 2)");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_image_view(device, steep_view);
  gfx::destroy_image_view(device, normal_view);
  gfx::destroy_image_view(device, mr_view);
  gfx::destroy_image(device, steep_image);
  gfx::destroy_image(device, normal_image);
  gfx::destroy_image(device, mr_image);
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &vis_below, &cluster_materials,
                                 &materials, &attributes, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

// The emissive and occlusion slots and the UV transform, on the flat quad again, each block
// against the CPU reference (brdf_reference.h):
//
//   0. **Unlit emission.** No sun, no sky, no lights: what the resolve writes is the emissive
//      factor times the sRGB-decoded emissive texel and nothing else, quadrant by quadrant.
//   1. **Lit emission.** The same material under the sun and the sky: the reference's shade()
//      with the emissive term added last, as fs_resolve adds it.
//   2. **Occlusion, lit.** A 2x2 occlusion map at strength 0.75 multiplies the sky hemisphere term
//      and nothing else: the reference's shade() with `occlusion_of(texel, 0.75)`.
//   3. **The occlusion view** (ResolveMode::Occlusion) returns 1 + strength * (r - 1) as a byte,
//      unencoded.
//   4, 5. **No indirect term, no difference.** With the sky and the ground's albedo at zero the
//      resolve has no indirect light, so the occluded material and the same material without its
//      map are the same picture byte for byte: occlusion never touches the sun.
//   6. **A quarter turn of KHR_texture_transform** on a 2x2 checker read through a repeating,
//      nearest sampler: each quadrant shows the texel `gfx::set_uv_transform`'s matrix sends it
//      to, which pins the GPU's arithmetic to the CPU's and the rotation's direction to the one
//      that function documents.
//   7. **An ORM texture** — occlusion in R, roughness in G — named by the metallic-roughness and
//      the occlusion slot through one sampler, as every FlightHelmet material is: the same
//      reference as block 2.
//
// The tolerance is the file's: 2 of 255 on a displayed value, 1 on the unencoded occlusion.
TEST_CASE("material resolve: emissive and occlusion maps, and the UV transform") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 flat{0.0f, 1.0f, 0.0f};
  const Vec3 normals[4] = {flat, flat, flat, flat};
  const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));

  // Texel order is screen top-left, top-right, bottom-left, bottom-right (the first case above).
  // The emissive texture is sRGB colour; four unlike values, none of them at an end of the range.
  const u8 emissive_texels[16] = {200, 40, 10,  255, 30,  180, 90,  255,
                                  60,  90, 220, 255, 250, 250, 250, 255};
  // The occlusion map is data, read from R: fully open, half, a quarter, and fully shut.
  const u8 occlusion_texels[16] = {255, 0, 0, 255, 128, 0, 0, 255, 64, 0, 0, 255, 0, 0, 0, 255};
  // The checker for the transform: red, green / blue, white.
  const u8 checker_texels[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                                 0,   0, 255, 255, 255, 255, 255, 255};
  // An "ORM" texture — occlusion in R beside roughness in G and metallic in B, as glTF exporters
  // pack them — named by both the metallic-roughness and the occlusion slot, one texture serving
  // two slots. G is 153, a roughness of 0.6, and B 0, a dielectric, so the reference is
  // the occlusion block's with that R.
  const u8 orm_texels[16] = {255, 153, 0, 255, 128, 153, 0, 255, 64, 153, 0, 255, 0, 153, 0, 255};
  gfx::ImageResource emissive_image;
  gfx::ImageResource occlusion_image;
  gfx::ImageResource checker_image;
  gfx::ImageResource orm_image;
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, orm_texels,
                                       sizeof(orm_texels), orm_image, &error),
                  error);
  gfx::ImageViewHandle orm_view = {};
  REQUIRE(gfx::create_image_view(device, orm_image, orm_view, &error));
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Srgb, emissive_texels,
                                       sizeof(emissive_texels), emissive_image, &error),
                  error);
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, occlusion_texels,
                                       sizeof(occlusion_texels), occlusion_image, &error),
                  error);
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, checker_texels,
                                       sizeof(checker_texels), checker_image, &error),
                  error);
  gfx::ImageViewHandle emissive_view = {};
  gfx::ImageViewHandle occlusion_view = {};
  gfx::ImageViewHandle checker_view = {};
  REQUIRE(gfx::create_image_view(device, emissive_image, emissive_view, &error));
  REQUIRE(gfx::create_image_view(device, occlusion_image, occlusion_view, &error));
  REQUIRE(gfx::create_image_view(device, checker_image, checker_view, &error));
  // Nearest and clamped for the emissive and occlusion quadrants (the tooling sampler); nearest
  // and repeating for the transform, whose rotated UVs leave [0, 1].
  gfx::SamplerHandle nearest = {};
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, nearest, &error));
  gfx::SamplerDesc repeat_desc;
  repeat_desc.mag = gfx::Filter::Nearest;
  repeat_desc.min = gfx::Filter::Nearest;
  repeat_desc.mipmapped = false;
  gfx::SamplerHandle repeat = {};
  REQUIRE_MESSAGE(gfx::create_sampler(device, repeat_desc, repeat, &error), error);
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 emissive_slot =
      bindless.add_sampled_image(emissive_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 occlusion_slot =
      bindless.add_sampled_image(occlusion_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 checker_slot =
      bindless.add_sampled_image(checker_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 orm_slot = bindless.add_sampled_image(orm_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 nearest_slot = bindless.add_sampler(nearest);
  const u32 repeat_slot = bindless.add_sampler(repeat);

  // The materials: emissive, occluded, plain (the occluded one without its map), and the rotated
  // checker. Every material's `sampler` is the repeating one, and the emissive and occlusion slots
  // name the clamped one through their own halves of the per-slot words — so a shader that read
  // `sampler` for those slots instead would read another sampler, which at these UVs happens to
  // give the same texels; what the halves are checked by is the transform block, whose base
  // colour must read `sampler` to repeat.
  const Vec3 base_color{0.8f, 0.6f, 0.3f};
  constexpr f32 k_roughness = 0.6f;
  const Vec3 emissive_factor{0.9f, 0.6f, 0.3f};
  constexpr f32 k_strength = 0.75f;
  enum : u32 {
    k_emissive = 0,
    k_occluded = 1,
    k_plain = 2,
    k_rotated = 3,
    k_orm = 4,
    k_materials = 5
  };
  gfx::ResolveMaterial material_set[k_materials];
  for (gfx::ResolveMaterial& material : material_set) {
    material.albedo = Vec4{base_color, k_roughness};
    material.emissive = Vec4{};
    material.sampler = repeat_slot;
  }
  material_set[k_emissive].emissive = Vec4{emissive_factor, 0.0f};
  material_set[k_emissive].emissive_texture = emissive_slot;
  material_set[k_emissive].samplers_occlusion_emissive = (nearest_slot << 16) | gfx::k_same_sampler;
  material_set[k_occluded].occlusion_texture = occlusion_slot;
  material_set[k_occluded].occlusion_strength = k_strength;
  material_set[k_occluded].samplers_occlusion_emissive = (gfx::k_same_sampler << 16) | nearest_slot;
  material_set[k_orm].albedo = Vec4{base_color, 1.0f};          // the map is the whole roughness
  material_set[k_orm].emissive = Vec4{0.0f, 0.0f, 0.0f, 1.0f};  // ... and the whole metallic
  material_set[k_orm].metallic_roughness_texture = orm_slot;
  material_set[k_orm].occlusion_texture = orm_slot;
  material_set[k_orm].occlusion_strength = k_strength;
  material_set[k_orm].samplers_mr_normal = (gfx::k_same_sampler << 16) | nearest_slot;
  material_set[k_orm].samplers_occlusion_emissive = (gfx::k_same_sampler << 16) | nearest_slot;
  material_set[k_rotated].albedo = Vec4{1.0f, 1.0f, 1.0f, k_roughness};
  material_set[k_rotated].albedo_texture = checker_slot;
  gfx::set_uv_transform(material_set[k_rotated], Vec2{0.0f, 0.0f}, radians(90.0f),
                        Vec2{1.0f, 1.0f});
  CHECK((material_set[k_rotated].flags & gfx::k_material_uv_transform) != 0);
  u32 material_index[k_materials];
  for (u32 i = 0; i < k_materials; ++i)
    material_index[i] = i;
  // An identity transform sets no flag: a material without one reads its UVs untouched.
  gfx::ResolveMaterial untouched;
  gfx::set_uv_transform(untouched, Vec2{0.0f, 0.0f}, 0.0f, Vec2{1.0f, 1.0f});
  CHECK(untouched.flags == 0);

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx_test::SingleInstance scene;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(scene.create(device, mesh, 1, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.attributes.data(),
                             mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                             attributes, &error));
  REQUIRE(
      gfx::upload_buffer(device, material_set, sizeof(material_set), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));

  constexpr u32 k_size = 128;
  constexpr u32 k_blocks = 8;
  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  gfx::BufferResource vis;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, u64{k_size} * k_size * sizeof(u64),
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_blocks,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(resolve_module.valid());
  gfx_test::ClusterRaster raster;
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                geometry::ClusterBuildOptions{}.max_triangles, &error),
                  error);
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = gfx::Format::R8G8B8A8Unorm;
  gfx::PipelineHandle resolve_pipeline = {};
  REQUIRE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error));

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
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
  base.mesh = scene.meshes.address;
  base.instances = scene.instances.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.attributes = attributes.address;
  base.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
  base.width = k_size;
  base.height = k_size;
  struct Block {
    u32 material;
    bool lit;  // the sun; off, and with no sky, the frame has no light at all
    // The hemisphere, the resolve's whole indirect term: the sky, and the ground it and the sun
    // light. Off is both at zero — a ground with no albedo — or the sun would still reach the
    // indirect term through the ground's bounce of it.
    bool sky;
    gfx::ResolveMode mode;
  };
  const Block block_desc[k_blocks] = {
      {k_emissive, false, false, gfx::ResolveMode::Shaded},
      {k_emissive, true, true, gfx::ResolveMode::Shaded},
      {k_occluded, true, true, gfx::ResolveMode::Shaded},
      {k_occluded, true, true, gfx::ResolveMode::Occlusion},
      {k_occluded, true, false, gfx::ResolveMode::Shaded},
      {k_plain, true, false, gfx::ResolveMode::Shaded},
      {k_rotated, true, true, gfx::ResolveMode::Albedo},
      {k_orm, true, true, gfx::ResolveMode::Shaded},
  };
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + block_desc[i].material * sizeof(u32);
    blocks[i].mode = static_cast<u32>(block_desc[i].mode);
    if (!block_desc[i].lit) blocks[i].sun = Vec4{sun_dir, 0.0f};
    if (!block_desc[i].sky) {
      blocks[i].sky = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
      blocks[i].ground = Vec4{};
    }
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);
  }

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    targets[i] = graph.create_image(
        "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::TransferWrite); },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "visibility", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_vis, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw, 1); });
  for (u32 i = 0; i < k_blocks; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          gfx::ClearColor clear{};
          b.color_attachment(targets[i], gfx::LoadOp::Clear, clear);
          b.read(rg_vis, gfx::Access::FragmentRead);
        },
        [&, i](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, resolve_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                            &block_address[i]);
          cb.draw(3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 i = 0; i < k_blocks; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < k_blocks; ++i) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[i]).image),
                                 gfx::vk::native(g.image_layout(targets[i])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  int worst = 0;
  auto expect_bytes = [&](u32 image, u32 x, u32 y, const u8* expect, int tolerance,
                          const std::string& what) {
    const u8* p = pixel(image, x, y);
    int here = 0;
    for (u32 c = 0; c < 3; ++c)
      here = std::max(here, std::abs(int{p[c]} - int{expect[c]}));
    worst = std::max(worst, here);
    CHECK_MESSAGE(here <= tolerance, what << " at " << x << "," << y << ": gpu " << int{p[0]} << ","
                                          << int{p[1]} << "," << int{p[2]} << " expected "
                                          << int{expect[0]} << "," << int{expect[1]} << ","
                                          << int{expect[2]});
  };
  // The reference for a shaded block: the surface at the pixel, under the block's light, with the
  // material's emission and occlusion.
  auto expect_shaded = [&](u32 image, u32 x, u32 y, const Block& block, ref::Dvec3 emissive,
                           double occlusion, const std::string& what) {
    ref::Surface s;
    s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = ref::dvec3(flat);
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(base_color);
    s.roughness = static_cast<double>(k_roughness);
    s.metallic = 0.0;
    const ref::Dvec3 sky_used = block.sky ? ref::dvec3(sky) : ref::Dvec3{};
    const ref::Dvec3 ground_used = block.sky ? ref::dvec3(base.ground) : ref::Dvec3{};
    const ref::Dvec3 linear = ref::shade(s, ref::dvec3(sun_dir), block.lit ? 1.0 : 0.0, sky_used,
                                         ground_used, nullptr, 0, emissive, false, occlusion);
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    expect_bytes(image, x, y, expect, 2, what);
  };

  const u32 cx = k_size / 2;
  const u32 cy = k_size / 2;
  const u32 quarter = static_cast<u32>(5.0f / (10.0f * std::tan(radians(30.0f))) * k_size * 0.25f);
  const u32 qx[4] = {cx - quarter, cx + quarter, cx - quarter, cx + quarter};
  const u32 qy[4] = {cy - quarter, cy - quarter, cy + quarter, cy + quarter};
  for (u32 q = 0; q < 4; ++q) {
    const std::string quadrant = ", quadrant " + std::to_string(q);
    const ref::Dvec3 emission =
        ref::emissive_of(ref::dvec3(emissive_factor), &emissive_texels[q * 4]);
    // 0: unlit, so the picture is the emission alone — the emissive colour, through the display
    // transform and nothing else.
    expect_shaded(0, qx[q], qy[q], block_desc[0], emission, 1.0, "unlit emission" + quadrant);
    // 1: the same emission on top of the lit surface.
    expect_shaded(1, qx[q], qy[q], block_desc[1], emission, 1.0, "lit emission" + quadrant);
    // 2: the occlusion of this quadrant on the sky hemisphere term alone.
    const double occlusion =
        ref::occlusion_of(occlusion_texels[q * 4], static_cast<double>(k_strength));
    expect_shaded(2, qx[q], qy[q], block_desc[2], ref::Dvec3{}, occlusion, "occlusion" + quadrant);
    // 7: the same occlusion read out of an ORM texture's R, while its G is the roughness the
    // other blocks have: one texture serving two slots gives the reference's answer.
    expect_shaded(7, qx[q], qy[q], block_desc[7], ref::Dvec3{}, occlusion, "ORM" + quadrant);
    // 3: the occlusion view is the same number, as a byte.
    const u8 grey = static_cast<u8>(std::lround(occlusion * 255.0));
    const u8 expect_grey[3] = {grey, grey, grey};
    expect_bytes(3, qx[q], qy[q], expect_grey, 1, "occlusion view" + quadrant);
  }
  // The emissive quadrants differ, or the comparisons above would pass on a constant.
  CHECK(std::abs(int{pixel(0, qx[0], qy[0])[0]} - int{pixel(0, qx[1], qy[1])[0]}) > 30);
  // And the occlusion darkens the lit picture where it is shut and not where it is open.
  CHECK(int{pixel(2, qx[3], qy[3])[2]} < int{pixel(2, qx[0], qy[0])[2]});

  // 4 and 5: with no sky and no ground there is no indirect term, and the occluded material's
  // picture is the unoccluded one's, byte for byte.
  int no_indirect = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u8* a = pixel(4, x, y);
      const u8* b = pixel(5, x, y);
      for (u32 c = 0; c < 3; ++c)
        no_indirect = std::max(no_indirect, std::abs(int{a[c]} - int{b[c]}));
    }
  }
  CHECK_MESSAGE(no_indirect == 0,
                "an occlusion map changed a picture with no indirect term by " << no_indirect);

  // 6: the quarter turn. gfx::set_uv_transform's matrix at 90 degrees is u' = v, v' = -u, so the
  // top-left quadrant (u, v) = (0.25, 0.25) reads (0.25, -0.25), which repeats to (0.25, 0.75):
  // the bottom-left texel. The CPU mirror of that arithmetic picks the texel for every quadrant.
  const f32 quadrant_uv[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
  const Vec4 m = material_set[k_rotated].uv_transform;
  for (u32 q = 0; q < 4; ++q) {
    const f32 u = quadrant_uv[q][0];
    const f32 v = quadrant_uv[q][1];
    f32 tu = m.x * u + m.y * v;
    f32 tv = m.z * u + m.w * v;
    tu -= std::floor(tu);
    tv -= std::floor(tv);
    const u32 texel = (tv < 0.5f ? 0u : 2u) + (tu < 0.5f ? 0u : 1u);
    // The albedo view is linear colour through the display transform; the checker's bytes are 0
    // or 255, which that transform maps to themselves.
    expect_bytes(6, qx[q], qy[q], &checker_texels[texel * 4], 2,
                 "rotated checker, quadrant " + std::to_string(q));
  }
  // A quarter turn is not the identity: the top-left quadrant no longer shows the red texel.
  CHECK(int{pixel(6, qx[0], qy[0])[0]} < 128);
  MESSAGE("worst difference over emission, occlusion and the transform, "
          << std::string(raster.name()) << " path: " << worst << " of 255");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, repeat);
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_image_view(device, orm_view);
  gfx::destroy_image(device, orm_image);
  gfx::destroy_image_view(device, checker_view);
  gfx::destroy_image_view(device, occlusion_view);
  gfx::destroy_image_view(device, emissive_view);
  gfx::destroy_image(device, checker_image);
  gfx::destroy_image(device, occlusion_image);
  gfx::destroy_image(device, emissive_image);
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &cluster_materials, &materials,
                                 &attributes, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
