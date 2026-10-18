// The resolve's water branch against a second implementation (water_reference.h; docs/subsystems/
// water.md, "Testing"): a level quad with a water material, seen from above under a known sun and
// the stand-in sky, shaded once per block — waves at two clocks and two roughnesses, a mirror over
// shallow water, and the shore's fade — and every compared pixel held to the CPU reference in
// double precision within the shading test's own 2 of 255 (shading_tests.cpp says why that is the
// tolerance). The quad's UVs are the water mesh's: x the distance from the edge, y the depth,
// constant over the quad so the interpolation is not what is tested. The pixels compared are the
// quad's middle half, where every wave is longer than four footprints and so drawn whole by both
// (a wave the footprint fades is the same function in both, but its fade's edge is where a
// footprint computed from a float derivative and one from a double difference may disagree).
// Skips only without 64-bit buffer atomics.
#include "brdf_reference.h"
#include "raster_path.h"
#include "scene_fixture.h"
#include "water_reference.h"

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
#include <shaders/visibility_resolve_water.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;
namespace wref = engine::water_ref;

namespace {

constexpr u32 k_size = 128;
constexpr u32 k_blocks = 5;

}  // namespace

TEST_CASE("material resolve: the water branch matches its CPU reference") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const Vec3 normals[4] = {up, up, up, up};
  // Two attribute sets: open water (5 m from the edge, 1.2 m deep) and the shore (0.3 m from the
  // edge, 0.4 m deep), each constant over the quad.
  const Vec2 open_uv[4] = {Vec2{5.0f, 1.2f}, Vec2{5.0f, 1.2f}, Vec2{5.0f, 1.2f}, Vec2{5.0f, 1.2f}};
  const Vec2 shore_uv[4] = {Vec2{0.3f, 0.4f}, Vec2{0.3f, 0.4f}, Vec2{0.3f, 0.4f}, Vec2{0.3f, 0.4f}};
  geometry::ClusterMesh mesh;
  geometry::ClusterMesh shore_mesh;
  {
    geometry::AttributeSource source;
    source.normals = normals;
    source.uvs = open_uv;
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                     &error, source));
    source.uvs = shore_uv;
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{},
                                     shore_mesh, &error, source));
  }
  REQUIRE(mesh.clusters.size() == 1);

  // The materials: waves at a broad glitter, waves at the scene's, a mirror, the shore.
  gfx::WaterShading wave;
  wave.shallow = Vec3{0.30f, 0.55f, 0.50f};
  wave.deep = Vec3{0.02f, 0.06f, 0.08f};
  wave.roughness = 0.3f;
  wave.depth_fade_m = 2.0f;
  wave.wave_amplitude_m = 0.05f;
  wave.wave_length_m = 2.5f;
  wave.chop = 0.6f;
  wave.shore_m = 1.0f;
  wave.direction = Vec2{0.6f, 0.8f};
  wave.seed = 9;
  gfx::WaterShading sharp = wave;
  sharp.roughness = 0.08f;
  gfx::WaterShading mirror = wave;
  mirror.wave_amplitude_m = 0.0f;
  gfx::ResolveMaterial material_table[3];
  gfx::set_water_material(material_table[0], wave);
  gfx::set_water_material(material_table[1], sharp);
  gfx::set_water_material(material_table[2], mirror);
  const u32 material_index[3] = {0, 1, 2};
  // Block: material, attributes (0 open, 1 shore), clock.
  struct Block {
    u32 material;
    u32 attributes;
    f32 time;
  };
  const Block block_of[k_blocks] = {
      {0, 0, 0.0f}, {0, 0, 37.25f}, {1, 0, 500.0f}, {2, 0, 0.0f}, {0, 1, 0.0f}};

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes[2];
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(scene.create(device, mesh, 1, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.attributes.data(),
                             mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                             attributes[0], &error));
  REQUIRE(shore_mesh.attributes.size() == mesh.attributes.size());
  REQUIRE(gfx::upload_buffer(device, shore_mesh.attributes.data(),
                             shore_mesh.attributes.size() * sizeof(geometry::VertexAttributes),
                             k_storage, attributes[1], &error));
  REQUIRE(gfx::upload_buffer(device, material_table, sizeof(material_table), k_storage, materials,
                             &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));

  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 view_up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, view_up);
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
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_water_spirv,
                                shaders::k_visibility_resolve_water_spirv_size, &error);
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
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error),
                  error);

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.visibility = vis.address;
  draw.extent = gfx::draw_extent(k_size, k_size);

  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const Vec3 sun_dir = normalize(Vec3{0.0f, 1.0f, 0.45f});
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
  base.width = k_size;
  base.height = k_size;
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + block_of[i].material * sizeof(u32);
    blocks[i].attributes = attributes[block_of[i].attributes].address;
    blocks[i].water_time_s = block_of[i].time;
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
  auto on_plane = [&](u32 x, u32 y) {
    return ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(view_up),
                               static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
  };
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  // A level surface's ambient under the stand-in: the sky's radiance, all of it (the normal up).
  const ref::Dvec3 ambient = ref::sky_radiance(sky_ref);
  const Vec2* uv_of[2] = {open_uv, shore_uv};

  int worst_all = 0;
  int differs_with_time = 0;
  for (u32 b = 0; b < k_blocks; ++b) {
    const wref::Params w = wref::params_of(material_table[block_of[b].material]);
    const Vec2 uv = uv_of[block_of[b].attributes][0];
    int worst = 0;
    for (u32 y = k_size / 4; y < 3 * k_size / 4; y += 4) {
      for (u32 x = k_size / 4; x < 3 * k_size / 4; x += 4) {
        const ref::Dvec3 p = on_plane(x, y);
        // The footprint as the resolve takes it: the larger of a pixel's steps on the plane.
        const ref::Dvec3 px = on_plane(x + 1, y) - p;
        const ref::Dvec3 py = on_plane(x, y + 1) - p;
        const double footprint = std::max(std::sqrt(ref::dot(px, px)), std::sqrt(ref::dot(py, py)));
        const ref::Dvec3 n = wref::normal(w, p.x, p.z, static_cast<double>(block_of[b].time),
                                          footprint, static_cast<double>(uv.y));
        const ref::Dvec3 v = ref::normalize(ref::dvec3(eye) - p);
        ref::Dvec3 r = n * (2.0 * ref::dot(n, v)) - v;
        r.y = std::fabs(r.y);
        const ref::Dvec3 linear =
            wref::radiance(w, n, v, static_cast<double>(uv.x), static_cast<double>(uv.y), sun_ref,
                           ref::splat(1.0), ambient, sky_ref);
        const u8* got = pixel(b, x, y);
        const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y),
                              ref::display(linear.z)};
        for (u32 c = 0; c < 3; ++c)
          worst = std::max(worst, std::abs(int{got[c]} - int{expect[c]}));
        if (b == 1) {
          const u8* at_zero = pixel(0, x, y);
          if (at_zero[0] != got[0] || at_zero[1] != got[1] || at_zero[2] != got[2])
            ++differs_with_time;
        }
      }
    }
    MESSAGE("water block " << b << ": worst " << worst << " of 255 against the reference ("
                           << std::string(raster.name()) << " path)");
    CHECK(worst <= 2);
    worst_all = std::max(worst_all, worst);
  }
  MESSAGE("water branch against its reference: worst " << worst_all << " of 255");
  // The clock moves the waves: the same pixels 37.25 s on are not the same picture.
  CHECK(differs_with_time > 100);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &attributes[0], &attributes[1],
                                 &cluster_materials, &materials, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
