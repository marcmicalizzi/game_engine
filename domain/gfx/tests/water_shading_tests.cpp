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
#include <domain/gfx/water_spectrum.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <shaders/visibility_resolve_water.spv.h>
#include <string>
#include <vector>

using namespace engine;
namespace ref = engine::brdf_ref;
namespace wref = engine::water_ref;

namespace {

constexpr u32 k_size = 128;
constexpr u32 k_blocks = 13;
// The synthetic depth maps (water.md, "The depth map"): 64 texels a side over the quad's 10 m.
constexpr u32 k_map_size = 64;
constexpr f32 k_map_origin = -5.0f;
constexpr f32 k_map_texel = 10.0f / 63.0f;

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
  // And a bottom that slopes: 0.5 m deep at x = -5 to 1.5 m at x = 5 (both exact in a half float),
  // the depth 1 + 0.1 x, for the volume tier's plane through the triangle's depths.
  const Vec2 slope_uv[4] = {Vec2{5.0f, 0.5f}, Vec2{5.0f, 1.5f}, Vec2{5.0f, 1.5f}, Vec2{5.0f, 0.5f}};
  geometry::ClusterMesh mesh;
  geometry::ClusterMesh shore_mesh;
  geometry::ClusterMesh slope_mesh;
  {
    geometry::AttributeSource source;
    source.normals = normals;
    source.uvs = open_uv;
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                     &error, source));
    source.uvs = shore_uv;
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{},
                                     shore_mesh, &error, source));
    source.uvs = slope_uv;
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{},
                                     slope_mesh, &error, source));
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
  // The volume tier (gfx::set_water_optics): the sharp waves over a ground of sand (a plain
  // material, 7), and the broad ones over no ground (their shallow colour), silty and passing less.
  gfx::WaterOptics clear;
  clear.turbidity = 0.3f;
  clear.ground = 7;
  gfx::WaterOptics silty;
  silty.absorption = Vec3{0.6f, 0.2f, 0.12f};
  silty.turbidity = 1.0f;
  silty.transmission = 0.6f;
  gfx::ResolveMaterial material_table[21];
  gfx::set_water_material(material_table[0], wave);
  gfx::set_water_material(material_table[1], sharp);
  gfx::set_water_material(material_table[2], mirror);
  gfx::set_water_material(material_table[3], sharp);
  gfx::set_water_optics(material_table[3], material_table[4], clear);
  gfx::set_water_material(material_table[5], wave);
  gfx::set_water_optics(material_table[5], material_table[6], silty);
  material_table[7].albedo = Vec4{0.55f, 0.45f, 0.32f, 0.9f};
  // The depth maps (gfx::set_water_depth_map): a bottom that slopes as the attributes' plane does,
  // 1 + 0.1 x, and a bowl, 2.3 m deep in the middle and 0.3 m at the corners. The sharp waves over
  // the sand see through each (the volume tier's march), and the broad ones fade by the bowl (the
  // low tier's fade from the map).
  const auto slope_depth = [](f64 x, f64) { return 1.0 + 0.1 * x; };
  const auto bowl_depth = [](f64 x, f64 z) { return 0.3 + 2.0 * (1.0 - (x * x + z * z) / 50.0); };
  std::vector<u16> map_texels[2];
  const f32 map_max[2] = {1.5f, 2.3f};
  for (u32 k = 0; k < 2; ++k) {
    map_texels[k].resize(k_map_size * k_map_size);
    for (u32 j = 0; j < k_map_size; ++j) {
      for (u32 i = 0; i < k_map_size; ++i) {
        const f64 x = static_cast<f64>(k_map_origin) + i * static_cast<f64>(k_map_texel);
        const f64 z = static_cast<f64>(k_map_origin) + j * static_cast<f64>(k_map_texel);
        const f64 d = k == 0 ? slope_depth(x, z) : bowl_depth(x, z);
        map_texels[k][j * k_map_size + i] =
            static_cast<u16>(std::floor(d / static_cast<f64>(map_max[k]) * 65535.0 + 0.5));
      }
    }
  }
  gfx::ImageResource map_image[2];
  gfx::ImageViewHandle map_view[2];
  gfx::WaterDepthFrame map_frame[2];
  gfx::set_water_material(material_table[8], sharp);
  gfx::set_water_optics(material_table[8], material_table[9], clear);
  gfx::set_water_material(material_table[10], sharp);
  gfx::set_water_optics(material_table[10], material_table[11], clear);
  gfx::set_water_material(material_table[12], wave);
  // An open sea's waves from the spectrum (water_spectrum.h; S93): the sharp waves' material made
  // open, its waves sixteen Gerstner waves from JONSWAP at 6 m/s over 5 km of fetch (a 5.9 m peak,
  // the band 23 m to 0.4 m), its next entry empty and the table in the five after it.
  gfx::set_water_material(material_table[14], sharp);
  material_table[14].flags |= gfx::k_material_water_open;
  gfx::WaterWave spectrum_waves[gfx::k_water_waves];
  gfx::water_spectrum(6.0, 5000.0, Vec2{0.6f, 0.8f}, 9, spectrum_waves);
  gfx::set_water_spectrum(material_table[14], &material_table[16], spectrum_waves);
  const u32 material_index[21] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                  11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  // Block: material, attributes (0 open, 1 shore, 2 the slope), clock.
  struct Block {
    u32 material;
    u32 attributes;
    f32 time;
  };
  const Block block_of[k_blocks] = {
      {0, 0, 0.0f},   {0, 0, 37.25f},  {1, 0, 500.0f}, {2, 0, 0.0f},   {0, 1, 0.0f},
      {3, 0, 500.0f}, {3, 2, 37.25f},  {5, 1, 0.0f},   {8, 0, 37.25f}, {10, 0, 500.0f},
      {12, 0, 0.0f},  {14, 0, 37.25f}, {14, 1, 0.0f}};
  // The map each material reads (k_material_water_depth_map), or none.
  const i32 map_of_material[21] = {-1, -1, -1, -1, -1, -1, -1, -1, 0,  -1, 1,
                                   -1, 1,  -1, -1, -1, -1, -1, -1, -1, -1};
  const f32 slope_of[3] = {0.0f, 0.0f, 0.1f};  // the depth's change a metre along x

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes[3];
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
  REQUIRE(slope_mesh.attributes.size() == mesh.attributes.size());
  REQUIRE(gfx::upload_buffer(device, slope_mesh.attributes.data(),
                             slope_mesh.attributes.size() * sizeof(geometry::VertexAttributes),
                             k_storage, attributes[2], &error));
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
  gfx::SamplerHandle map_sampler;
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Linear, map_sampler, &error));
  const u32 map_sampler_slot = bindless.add_sampler(map_sampler);
  for (u32 k = 0; k < 2; ++k) {
    std::vector<u8> rgba(k_map_size * k_map_size * 4);
    for (u32 t = 0; t < k_map_size * k_map_size; ++t) {
      rgba[t * 4 + 0] = static_cast<u8>(map_texels[k][t] >> 8);
      rgba[t * 4 + 1] = static_cast<u8>(map_texels[k][t] & 0xffu);
      rgba[t * 4 + 2] = 0;
      rgba[t * 4 + 3] = 255;
    }
    REQUIRE(gfx::upload_image_2d(device, k_map_size, k_map_size, gfx::Format::R8G8B8A8Unorm,
                                 rgba.data(), rgba.size(), map_image[k], &error));
    REQUIRE(gfx::create_image_view(device, map_image[k], map_view[k], &error));
    map_frame[k].texture =
        bindless.add_sampled_image(map_view[k], gfx::ImageLayout::ShaderReadOnly);
    map_frame[k].sampler = map_sampler_slot;
    map_frame[k].size = k_map_size;
    map_frame[k].origin_m = Vec2{k_map_origin, k_map_origin};
    map_frame[k].texel_m = k_map_texel;
    map_frame[k].depth_max = map_max[k];
  }
  gfx::set_water_depth_map(material_table[8], material_table[9], map_frame[0]);
  gfx::set_water_depth_map(material_table[10], material_table[11], map_frame[1]);
  gfx::set_water_depth_map(material_table[12], material_table[13], map_frame[1]);
  REQUIRE((material_table[12].flags & gfx::k_material_water_volume) == 0u);
  REQUIRE((material_table[12].flags & gfx::k_material_water_depth_map) != 0u);
  REQUIRE(gfx::upload_buffer(device, material_table, sizeof(material_table), k_storage, materials,
                             &error));
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
          gfx::ClearColor clear_color{};
          b.color_attachment(targets[i], gfx::LoadOp::Clear, clear_color);
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
  const Vec2* uv_of[3] = {open_uv, shore_uv, slope_uv};

  int worst_all = 0;
  int differs_with_time = 0;
  int differs_with_volume = 0;
  for (u32 b = 0; b < k_blocks; ++b) {
    const gfx::ResolveMaterial& material = material_table[block_of[b].material];
    const wref::Params w_block = wref::params_of(material);
    const bool volume = (material.flags & gfx::k_material_water_volume) != 0;
    const Vec2 uv = uv_of[block_of[b].attributes][0];
    const double slope = static_cast<double>(slope_of[block_of[b].attributes]);
    const i32 map = map_of_material[block_of[b].material];
    const wref::DepthMap depth_map =
        map >= 0 ? wref::depth_map_of(material_table[block_of[b].material + 1],
                                      map_texels[map].data(), k_map_size)
                 : wref::DepthMap{};
    int worst = 0;
    for (u32 y = k_size / 4; y < 3 * k_size / 4; y += 4) {
      for (u32 x = k_size / 4; x < 3 * k_size / 4; x += 4) {
        const ref::Dvec3 p = on_plane(x, y);
        // The footprint as the resolve takes it: the larger of a pixel's steps on the plane.
        const ref::Dvec3 px = on_plane(x + 1, y) - p;
        const ref::Dvec3 py = on_plane(x, y + 1) - p;
        const double footprint = std::max(std::sqrt(ref::dot(px, px)), std::sqrt(ref::dot(py, py)));
        // The depth at the point: the attribute's, or on the slope its plane through the corners.
        // With a map, the map's bilinear sample there.
        const double depth = map >= 0       ? wref::depth_sample(depth_map, p.x, p.z)
                             : slope != 0.0 ? 1.0 + slope * p.x
                                            : static_cast<double>(uv.y);
        // The waves: the spectrum's table where the material has one (S93), the four sines else.
        const bool spectral = (material.flags & gfx::k_material_water_spectrum) != 0;
        gfx::WaterWave table[gfx::k_water_waves];
        if (spectral)
          std::memcpy(static_cast<void*>(table), &material_table[block_of[b].material + 2],
                      sizeof(table));
        // The slope variance the footprint leaves out of the spectrum's normal widens the lobe
        // (S100 stage B; water.slang, `water_roughness`).
        wref::Params w = w_block;
        double dropped = 0.0;
        const ref::Dvec3 n =
            spectral ? wref::normal_spectrum(table, p.x, p.z, static_cast<double>(block_of[b].time),
                                             footprint, depth, &dropped)
                     : wref::normal(w, p.x, p.z, static_cast<double>(block_of[b].time), footprint,
                                    depth);
        w.roughness = wref::roughness_with(w.roughness, (1.0 - gfx::k_water_micro_share) * dropped);
        // The shore's fade: the distance from the edge, or for an open body the depth's share of
        // the calm depth (water_edge).
        const double from_edge = wref::edge(w, material.flags, static_cast<double>(uv.x), depth);
        const ref::Dvec3 v = ref::normalize(ref::dvec3(eye) - p);
        ref::Dvec3 linear;
        if (volume) {
          // No shadow is asked for, so the sun reaches the surface and the bottom whole; the
          // ground is the plain material's colour, or the shallow colour with none.
          const gfx::ResolveMaterial& optics = material_table[block_of[b].material + 1];
          ref::Dvec3 n_b = ref::normalize(ref::Dvec3{slope, 1.0, 0.0});
          const wref::Path path = map >= 0 ? wref::path_map(depth_map, p.x, p.z, n, v, depth, n_b)
                                           : wref::path(n, v, depth, slope, 0.0);
          const ref::Dvec3 albedo = optics.sampler == gfx::k_water_no_ground
                                        ? w.shallow
                                        : ref::dvec3(material_table[optics.sampler].albedo);
          const ref::Dvec3 under = wref::volume(wref::optics_of(optics), path, n_b, albedo, sun_ref,
                                                ref::splat(1.0), 1.0, 1.0, ambient);
          linear =
              wref::radiance_volume(w, n, v, from_edge, sun_ref, ref::splat(1.0), sky_ref, under);
        } else {
          linear =
              wref::radiance(w, n, v, from_edge, depth, sun_ref, ref::splat(1.0), ambient, sky_ref);
        }
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
        // The volume tier draws another picture than the painted bottom under the same waves.
        if (b == 5) {
          const u8* low = pixel(2, x, y);
          if (low[0] != got[0] || low[1] != got[1] || low[2] != got[2]) ++differs_with_volume;
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
  CHECK(differs_with_volume > 100);
  // The march over the sloped map lands where the plane through the same slope does: block 8 is
  // block 6's waves and clock over the map instead of the attributes, and draws within 2 of it.
  int slope_map_vs_plane = 0;
  for (u32 y = k_size / 4; y < 3 * k_size / 4; ++y) {
    for (u32 x = k_size / 4; x < 3 * k_size / 4; ++x) {
      const u8* plane = pixel(6, x, y);
      const u8* marched = pixel(8, x, y);
      for (u32 c = 0; c < 3; ++c)
        slope_map_vs_plane =
            std::max(slope_map_vs_plane, std::abs(int{plane[c]} - int{marched[c]}));
    }
  }
  MESSAGE("the sloped map's march against the plane through its slope: worst " << slope_map_vs_plane
                                                                               << " of 255");
  CHECK(slope_map_vs_plane <= 2);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  for (u32 k = 0; k < 2; ++k) {
    gfx::destroy_image_view(device, map_view[k]);
    gfx::destroy_image(device, map_image[k]);
  }
  gfx::destroy_sampler(device, map_sampler);
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &attributes[0], &attributes[1], &attributes[2],
        &cluster_materials, &materials, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

// ---- S100 stage B, on the CPU: the wind's sea, the lee and the shallows -------------------------
// (water_spectrum.h, water_swash.h and water_reference.h, which mirrors water_waves.slang; the pool
// pass holds the shader to the same reference in deform_tests.cpp.)

// **A lake's wind sea** (docs/experiments/shore.md, "The spectrum for every body"): 7 m/s over 0.9
// km of water. The table's sixteen bands hold the height the fetch-limited growth law gives and
// JONSWAP's peak period; drawn as narrow bands — a carrier under a travelling envelope — they keep
// the variance of the sines they replace, the gradient is the height's, the clock wraps with no
// jump, and one band alone does not repeat forty wavelengths on (a sine does, exactly).
TEST_CASE("water: a lake's wind sea has the growth law's height and period, and never repeats") {
  const f64 u10 = 7.0;
  const f64 fetch = 900.0;
  const f64 g = 9.81;
  gfx::WaterWave waves[gfx::k_water_waves];
  gfx::water_spectrum(u10, fetch, Vec2{1.0f, 0.0f}, 631, waves);
  f64 variance = 0.0;
  u32 peak_band = 0;
  for (u32 i = 0; i < gfx::k_water_waves; ++i) {
    variance += 0.5 * static_cast<f64>(waves[i].amplitude) * static_cast<f64>(waves[i].amplitude);
    if (waves[i].amplitude > waves[peak_band].amplitude) peak_band = i;
  }
  // The Coastal Engineering Manual's fetch-limited growth (II-2): g H / U^2 = 1.6e-3 (g F /
  // U^2)^1/2 and JONSWAP's peak, f_p U / g = 3.5 (g F / U^2)^(-1/3) (Hasselmann et al. 1973).
  const f64 x = g * fetch / (u10 * u10);
  const f64 law_height = 1.6e-3 * u10 * u10 / g * std::sqrt(x);
  const f64 law_period = u10 / (3.5 * g) * std::cbrt(x);
  const f64 height = 4.0 * std::sqrt(variance);
  const f64 period = 6.283185307179586 / gfx::jonswap_peak(u10, fetch);
  MESSAGE("7 m/s over 0.9 km: H_m0 "
          << height << " m (the growth law " << law_height << "), T_p " << period << " s (the law "
          << law_period << "), peak wavelength " << g * period * period / 6.283185307 << " m");
  CHECK(height == doctest::Approx(law_height).epsilon(0.2));
  CHECK(period == doctest::Approx(law_period).epsilon(0.03));
  // The band nearest the peak is the table's largest.
  const f64 peak_omega = static_cast<f64>(waves[peak_band].cycles) * 6.283185307179586 / 1024.0;
  CHECK(peak_omega == doctest::Approx(gfx::jonswap_peak(u10, fetch)).epsilon(0.15));
  // A fully developed sea grows no further: 7 m/s past 110 km is the same table.
  CHECK(gfx::jonswap_peak(u10, 600000.0) == gfx::jonswap_peak(u10, 2.2e4 * u10 * u10 / g));
  CHECK(gfx::jonswap_peak(u10, 100000.0) > gfx::jonswap_peak(u10, 600000.0));

  // The bands' sum over a 60 m patch at eight clocks: the variance of the sines.
  const auto sum_at = [&](f64 px, f64 pz, f64 t) {
    wref::Sum s;
    for (const gfx::WaterWave& w : waves)
      wref::wave_add(w, px, pz, t, 0.0, 1.0, 0.0, s);
    return s;
  };
  f64 mean = 0.0, square = 0.0;
  u32 count = 0;
  for (u32 k = 0; k < 8; ++k)
    for (u32 j = 0; j < 80; ++j)
      for (u32 i = 0; i < 80; ++i) {
        const f64 h = sum_at(-30.0 + 0.77 * i, -30.0 + 0.77 * j, 3.0 + 117.0 * k).height;
        mean += h;
        square += h * h;
        ++count;
      }
  mean /= count;
  const f64 measured = square / count - mean * mean;
  MESSAGE("the bands' variance over the patch " << measured << " m^2 against the sines' "
                                                << variance);
  CHECK(measured == doctest::Approx(variance).epsilon(0.15));
  // The gradient is the height's, by a central difference over a millimetre.
  for (u32 k = 0; k < 16; ++k) {
    const f64 px = -7.0 + 1.37 * k, pz = 3.0 - 0.91 * k, t = 11.0 + 61.0 * k, e = 1e-3;
    const wref::Sum s = sum_at(px, pz, t);
    const f64 dx = (sum_at(px + e, pz, t).height - sum_at(px - e, pz, t).height) / (2.0 * e);
    const f64 dz = (sum_at(px, pz + e, t).height - sum_at(px, pz - e, t).height) / (2.0 * e);
    CHECK(std::fabs(s.gx - dx) < 2e-4);
    CHECK(std::fabs(s.gz - dz) < 2e-4);
  }
  // The clock wraps at 1,024 s with no jump: every carrier and every envelope is periodic in it.
  for (u32 k = 0; k < 16; ++k) {
    const f64 px = 40.0 - 5.3 * k, pz = -12.0 + 2.9 * k;
    CHECK(std::fabs(sum_at(px, pz, 0.0).height - sum_at(px, pz, 1024.0).height) < 1e-9);
  }
  // One band alone, forty of its wavelengths on along its travel: a sine is itself again; the band
  // is not (the correlation over 4,000 places).
  const gfx::WaterWave& band = waves[peak_band];
  const f64 lambda = 6.283185307179586 / static_cast<f64>(band.k);
  f64 aa = 0.0, bb = 0.0, ab = 0.0;
  for (u32 i = 0; i < 4000; ++i) {
    const f64 px = -200.0 + 0.613 * i, pz = 150.0 - 0.357 * i;
    wref::Sum a, b;
    wref::wave_add(band, px, pz, 5.0, 0.0, 1.0, 0.0, a);
    wref::wave_add(band, px + 40.0 * lambda * static_cast<f64>(band.direction.x),
                   pz + 40.0 * lambda * static_cast<f64>(band.direction.y), 5.0, 0.0, 1.0, 0.0, b);
    aa += a.height * a.height;
    bb += b.height * b.height;
    ab += a.height * b.height;
  }
  const f64 correlation = ab / std::sqrt(aa * bb);
  MESSAGE("one band against itself forty wavelengths on: correlation " << correlation);
  CHECK(std::fabs(correlation) < 0.2);
  // In a lee the long waves go first: at a tenth of the fetch the peak is all but gone and the
  // shortest band barely touched.
  CHECK(gfx::water_grown_share(static_cast<f64>(waves[peak_band].grow), 0.1) < 0.01);
  CHECK(gfx::water_grown_share(static_cast<f64>(waves[gfx::k_water_waves - 1].grow), 0.1) > 0.9);
  CHECK(gfx::water_grown_share(static_cast<f64>(waves[peak_band].grow), 1.0) == 1.0);
}

// **The fetch at every place** (gfx::water_fetch_field): a round lake of 80 m radius on a metre
// grid. Along the wind's diameter the fetch is the chord behind the place; the wind's own shore has
// none; an island casts a lee; and a beach's texels keep the fetch of the water at their foot.
TEST_CASE("water: a closed body's fetch is its own reach along the wind") {
  const u32 n = 201;
  Vector<f32> depth(n * n);
  const auto fill = [&](bool island) {
    for (u32 j = 0; j < n; ++j)
      for (u32 i = 0; i < n; ++i) {
        const f64 px = static_cast<f64>(i) - 100.0, pz = static_cast<f64>(j) - 100.0;
        const bool land =
            std::hypot(px, pz) > 80.0 || (island && std::fabs(px) < 10.0 && std::fabs(pz) < 20.0);
        depth[j * n + i] = land ? -0.5f : 1.0f;
      }
  };
  fill(false);
  Vector<f32> fetch(n * n);
  const auto at = [&](i32 px, i32 pz) {
    return static_cast<f64>(fetch[(pz + 100) * n + px + 100]);
  };
  const f64 longest = gfx::water_fetch_field(depth.data(), n, 1.0, Vec2{1.0f, 0.0f}, fetch.data());
  CHECK(longest == doctest::Approx(160.0).epsilon(0.02));
  CHECK(at(0, 0) == doctest::Approx(81.0).epsilon(0.03));
  CHECK(at(78, 0) == doctest::Approx(159.0).epsilon(0.03));
  CHECK(at(-79, 0) < 3.0);
  CHECK(at(-90, 0) == 0.0);
  // The beach past the downwind shore holds the sea's fetch, decaying over 20 m of land.
  CHECK(at(84, 0) > 0.75 * at(78, 0));
  // Half way up the lake's side the chord is shorter: 2 sqrt(80^2 - 60^2) = 105.8 m.
  CHECK(at(50, 60) == doctest::Approx(52.9 + 50.0).epsilon(0.08));
  // Across the wind's diagonal the centre still has the radius behind it.
  gfx::water_fetch_field(depth.data(), n, 1.0, Vec2{0.6f, 0.8f}, fetch.data());
  CHECK(at(0, 0) == doctest::Approx(81.0).epsilon(0.06));
  CHECK(at(-48, -64) < 4.0);
  // An island 20 m long along the wind: right behind it the fetch is a fraction of the open
  // lake's, and it regrows.
  fill(true);
  gfx::water_fetch_field(depth.data(), n, 1.0, Vec2{1.0f, 0.0f}, fetch.data());
  CHECK(at(12, 0) < 0.5 * 93.0);
  CHECK(at(70, 0) > at(12, 0) + 50.0);
  CHECK(at(0, 60) == doctest::Approx(52.9).epsilon(0.1));
}

// **A crest crossing a bay has no corner** (docs/experiments/shore.md, "The shallows"). A round bay
// 60 m across on a metre grid, shelving 1:15 from its rim: the train's crests are level lines of
// the arrival field, and a crest's direction is the field's gradient's. Walked along a depth
// contour in millimetre steps, the direction the read gives turns from one step to the next by at
// most what a C1 field allows — the field's largest second difference over its gradient, times the
// step (the bound below) — and never by a jump. A bilinear read of the same texels, which is what
// stage A drew, jumps at every texel border the walk crosses: the corners counted here are the
// facets of the forest's item 29.
TEST_CASE("water: a crest that crosses a bay's depth contours turns no corner") {
  const u32 n = 129;
  const f64 texel = 1.0;
  gfx::WaterSurface s{};
  s.size = n;
  s.texel_m = static_cast<f32>(texel);
  s.origin = Vec2{-64.0f, -64.0f};
  s.period_s = static_cast<f32>(gfx::swash_period(1.2));
  Vector<f32> depth(n * n);
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i)
      depth[j * n + i] = static_cast<f32>(
          (30.0 - std::hypot(static_cast<f64>(i) - 64.0, static_cast<f64>(j) - 64.0)) / 15.0);
  Vector<f32> arrival(n * n);
  gfx::water_arrival_field(depth.data(), n, texel, static_cast<f64>(s.period_s), arrival.data());
  Vector<f32> field(gfx::k_water_field_channels * n * n);
  for (u32 i = 0; i < n * n; ++i) {
    for (u32 c = 0; c < gfx::k_water_field_channels; ++c)
      field[gfx::k_water_field_channels * i + c] = 0.0f;  // no slope, no second train, no flux
    field[gfx::k_water_field_channels * i] = depth[i];
    field[gfx::k_water_field_channels * i + 1] = arrival[i];
    field[gfx::k_water_field_channels * i + 2] = 1.0f;
  }
  // Stage A's read: the bilinear's own gradient in the cell.
  const auto bilinear_grad = [&](f64 px, f64 pz, f64& gx, f64& gz) {
    const f64 fx = px + 64.0, fz = pz + 64.0;
    const u32 i = static_cast<u32>(std::floor(fx)), j = static_cast<u32>(std::floor(fz));
    const f64 u = fx - i, w = fz - j;
    const auto tau = [&](u32 at) { return static_cast<f64>(arrival[at]); };
    const f64 a = tau(j * n + i), b = tau(j * n + i + 1);
    const f64 c = tau((j + 1) * n + i), d = tau((j + 1) * n + i + 1);
    gx = (b - a) + ((d - c) - (b - a)) * w;
    gz = (c - a) + ((d - b) - (c - a)) * u;
  };
  // The field's largest second difference inside the walked ring, and its smallest gradient there.
  f64 second = 0.0;
  for (u32 j = 30; j < 99; ++j)
    for (u32 i = 30; i < 99; ++i) {
      const f64 r = std::hypot(static_cast<f64>(i) - 64.0, static_cast<f64>(j) - 64.0);
      if (r < 25.5 || r > 28.5) continue;
      const auto tau = [&](u32 at) { return static_cast<f64>(arrival[at]); };
      const f64 c = tau(j * n + i);
      second = std::max(second, std::fabs(tau(j * n + i + 1) - 2.0 * c + tau(j * n + i - 1)));
      second = std::max(second, std::fabs(tau((j + 1) * n + i) - 2.0 * c + tau((j - 1) * n + i)));
      second = std::max(second, std::fabs(tau((j + 1) * n + i + 1) - tau((j + 1) * n + i) -
                                          tau(j * n + i + 1) + c));
    }
  // The contour 0.2 m deep: inshore of the train's source (1 / k0 = 0.36 m deep, 24.6 m out), where
  // the depth governs.
  const f64 radius = 27.0;
  const f64 step = 1e-3;
  const u32 steps = static_cast<u32>(6.283185307179586 * radius / step);
  f64 smallest = 1e30;
  f64 worst_smooth = 0.0, worst_bilinear = 0.0;
  u32 corners_smooth = 0, corners_bilinear = 0;
  f64 last_smooth = 0.0, last_bilinear = 0.0;
  const auto turn = [](f64 a, f64 b) {
    f64 d = std::fabs(a - b);
    if (d > 3.14159265358979) d = 6.283185307179586 - d;
    return d;
  };
  // Two passes: the gradient's smallest length first, for the bound; then the turns against it.
  for (u32 pass = 0; pass < 2; ++pass) {
    const f64 bound = pass == 1 ? 4.0 * second / (texel * texel) / smallest * step : 0.0;
    for (u32 k = 0; k <= steps; ++k) {
      const f64 a = static_cast<f64>(k) * step / radius;
      const f64 px = radius * std::cos(a), pz = radius * std::sin(a);
      const wref::Field f = wref::field_at(s, field.data(), px, pz);
      if (pass == 0) {
        smallest = std::min(smallest, std::hypot(f.arrival_grad[0], f.arrival_grad[1]));
        continue;
      }
      f64 bx = 0.0, bz = 0.0;
      bilinear_grad(px, pz, bx, bz);
      const f64 smooth = std::atan2(f.arrival_grad[1], f.arrival_grad[0]);
      const f64 faceted = std::atan2(bz, bx);
      if (k > 0) {
        const f64 ts = turn(smooth, last_smooth), tb = turn(faceted, last_bilinear);
        worst_smooth = std::max(worst_smooth, ts);
        worst_bilinear = std::max(worst_bilinear, tb);
        if (ts > bound) ++corners_smooth;
        if (tb > bound) ++corners_bilinear;
      }
      last_smooth = smooth;
      last_bilinear = faceted;
    }
    if (pass == 1)
      MESSAGE("a crest round the 27 m contour in 1 mm steps: the C1 read turns at most "
              << worst_smooth * 57.2957795 << " deg a step (the bound " << bound * 57.2957795
              << "), 0 corners wanted and " << corners_smooth << " found; the bilinear read turns "
              << worst_bilinear * 57.2957795 << " deg at a texel's border, " << corners_bilinear
              << " corners in " << 6.283185307 * radius << " m");
  }
  CHECK(smallest > 0.0);
  CHECK(corners_smooth == 0u);
  CHECK(corners_bilinear > 100u);
}
