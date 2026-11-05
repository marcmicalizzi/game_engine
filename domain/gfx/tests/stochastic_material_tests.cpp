// Stochastic (hex-tiled) sampling of a mesh material (R131; renderer.md, "Stochastic mesh
// materials"): a quad whose UVs span eight repeats of a noise texture, drawn once plain and once
// with `k_material_stochastic`. The albedo view of the stochastic one must keep the texture's mean
// (within 1% of the plain picture's: the operator is variance-preserving, not mean-shifting) and
// its contrast, must not be the plain picture, and must be the same bytes in two frames (the
// lattice is a function of the UV alone). An alpha-tested material is refused by
// `gfx::set_stochastic`, and one whose row carries both bits anyway is drawn plain by the resolve.
// Drawn with the resolve's stochastic variant, the module the renderer selects for a scene with a
// flagged material. Through the device's own raster path (raster_path.h); skips only without
// 64-bit buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <core/math/math.h>
#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/visibility_resolve_stochastic.spv.h>
#include <span>
#include <string>
#include <vector>

using namespace engine;

namespace {

constexpr u32 k_texture_side = 64;
constexpr u32 k_size = 256;
constexpr f32 k_repeats = 8.0f;

u32 noise_hash(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// The albedo view stores display_encode(albedo) = albedo^(1/2.2) in UNORM; back to linear.
f64 linear_of(u8 code) { return std::pow(static_cast<f64>(code) / 255.0, 2.2); }

}  // namespace

TEST_CASE("material resolve: stochastic mesh materials keep the mean and lose the repeat") {
  // The helper first, no device: an alpha-tested material is refused (its cutoff is the word the
  // means would take), and marking one alpha-tested after the fact clears the flag.
  {
    gfx::ResolveMaterial masked;
    gfx::set_alpha_mask(masked, 0.5f, 1.0f);
    const gfx::ResolveMaterial before = masked;
    CHECK_FALSE(gfx::set_stochastic(masked, Vec3{0.5f, 0.5f, 0.5f}, 0.5f));
    CHECK(std::memcmp(&before, &masked, sizeof(masked)) == 0);
    gfx::ResolveMaterial later;
    CHECK(gfx::set_stochastic(later, Vec3{0.5f, 0.5f, 0.5f}, 0.5f));
    gfx::set_alpha_mask(later, 0.5f, 1.0f);
    CHECK((later.flags & gfx::k_material_stochastic) == 0);
    CHECK(later.uv_offset.z == 0.5f);
    // The means' word: the square roots in bytes, so 0.25 is 128 and 1 is 255.
    CHECK(gfx::pack_stochastic_means(Vec3{0.25f, 1.0f, 0.0f}, 0.25f) == 0x8000FF80u);
  }

  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // A 10 m quad on y = 0, its UVs eight repeats across, normals up.
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const Vec3 normals[4] = {up, up, up, up};
  const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{k_repeats, 0.0f}, Vec2{k_repeats, k_repeats},
                       Vec2{0.0f, k_repeats}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));

  // Noise in 0.25..0.75 per channel (no texel the operator's gain could push past 0 or 1), with a
  // box-filtered chain down to 1x1, and its exact mean.
  std::vector<std::vector<u8>> levels;
  levels.emplace_back(u64{k_texture_side} * k_texture_side * 4);
  f64 mean[3] = {0.0, 0.0, 0.0};
  for (u32 i = 0; i < k_texture_side * k_texture_side; ++i) {
    for (u32 c = 0; c < 3; ++c) {
      const u8 v = static_cast<u8>(64u + noise_hash(i * 3u + c + 17u) % 128u);
      levels[0][i * 4 + c] = v;
      mean[c] += static_cast<f64>(v) / 255.0;
    }
    levels[0][i * 4 + 3] = 255;
  }
  for (f64& m : mean)
    m /= static_cast<f64>(k_texture_side * k_texture_side);
  for (u32 side = k_texture_side / 2; side >= 1; side /= 2) {
    const std::vector<u8>& above = levels.back();
    std::vector<u8> next(u64{side} * side * 4);
    for (u32 y = 0; y < side; ++y)
      for (u32 x = 0; x < side; ++x)
        for (u32 c = 0; c < 4; ++c) {
          const u32 a = side * 2;
          const u32 sum = above[((2 * y) * a + 2 * x) * 4 + c] +
                          above[((2 * y) * a + 2 * x + 1) * 4 + c] +
                          above[((2 * y + 1) * a + 2 * x) * 4 + c] +
                          above[((2 * y + 1) * a + 2 * x + 1) * 4 + c];
          next[(y * side + x) * 4 + c] = static_cast<u8>((sum + 2) / 4);
        }
    levels.push_back(std::move(next));
  }
  std::vector<gfx::ImageLevelData> level_data;
  for (const std::vector<u8>& l : levels)
    level_data.push_back(gfx::ImageLevelData{l.data(), l.size()});
  gfx::ImageResource texture;
  REQUIRE_MESSAGE(gfx::upload_image_2d_levels(
                      device, k_texture_side, k_texture_side, gfx::Format::R8G8B8A8Unorm,
                      std::span<const gfx::ImageLevelData>(level_data.data(), level_data.size()),
                      texture, &error),
                  error);
  gfx::ImageViewHandle texture_view = {};
  REQUIRE(gfx::create_image_view(device, texture, texture_view, &error));
  gfx::SamplerHandle sampler = {};
  REQUIRE(gfx::create_sampler(device, gfx::SamplerDesc{}, sampler, &error));  // repeat, mipped
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 texture_slot =
      bindless.add_sampled_image(texture_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 sampler_slot = bindless.add_sampler(sampler);

  // 0 plain, 1 stochastic, 2 alpha-tested and flagged by hand (drawn plain), 3 alpha-tested.
  constexpr u32 k_materials = 4;
  gfx::ResolveMaterial material_set[k_materials];
  material_set[0].albedo = Vec4{1.0f, 1.0f, 1.0f, 0.5f};
  material_set[0].albedo_texture = texture_slot;
  material_set[0].sampler = sampler_slot;
  material_set[0].flags = gfx::k_material_mipped;
  material_set[1] = material_set[0];
  REQUIRE(gfx::set_stochastic(
      material_set[1],
      Vec3{static_cast<f32>(mean[0]), static_cast<f32>(mean[1]), static_cast<f32>(mean[2])}, 0.5f));
  material_set[3] = material_set[0];
  gfx::set_alpha_mask(material_set[3], 0.0f, 1.0f);
  material_set[2] = material_set[3];
  material_set[2].flags |= gfx::k_material_stochastic;

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx_test::SingleInstance scene;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes;
  gfx::BufferResource materials;
  gfx::BufferResource material_index[k_materials];
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
  for (u32 i = 0; i < k_materials; ++i)
    REQUIRE(gfx::upload_buffer(device, &i, sizeof(i), k_storage, material_index[i], &error));

  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 view_up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, view_up);
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  constexpr u32 k_blocks = k_materials;
  constexpr u64 k_image_bytes = u64{k_size} * k_size * 4;
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
  REQUIRE(gfx::create_buffer(device, k_image_bytes * k_blocks, gfx::BufferUsage::TransferDst, true,
                             host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_stochastic_spirv,
                                shaders::k_visibility_resolve_stochastic_spirv_size, &error);
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
  draw.extent = gfx::draw_extent(k_size, k_size);

  gfx::ResolveParams base{};
  base.sky = Vec4{0.2f, 0.3f, 0.4f, 1.0f};
  base.sun = Vec4{up, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = clusters.address;
  base.mesh = scene.meshes.address;
  base.instances = scene.instances.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.attributes = attributes.address;
  base.width = k_size;
  base.height = k_size;
  base.mode = static_cast<u32>(gfx::ResolveMode::Albedo);
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = material_index[i].address;
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
          region.bufferOffset = k_image_bytes * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[i]).image),
                                 gfx::vk::native(g.image_layout(targets[i])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);

  // Two frames; the second's stochastic picture must be the first's, byte for byte.
  std::vector<u8> first(k_image_bytes * k_blocks);
  for (u32 frame = 0; frame < 2; ++frame) {
    gfx::CommandList commands = frames.begin_frame();
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
    if (frame == 0) std::memcpy(first.data(), host_color.mapped, first.size());
  }
  const u8* image = static_cast<const u8*>(host_color.mapped);
  CHECK(std::memcmp(first.data() + k_image_bytes, image + k_image_bytes, k_image_bytes) == 0);

  // The quad covers the middle of the picture; read a square well inside it.
  const f32 half_px = 5.0f / (10.0f * std::tan(radians(30.0f))) * k_size * 0.5f;
  const i32 inner = static_cast<i32>(half_px * 0.8f);
  const i32 cx = static_cast<i32>(k_size / 2);
  const i32 cy = static_cast<i32>(k_size / 2);
  struct Stats {
    f64 mean[3] = {0.0, 0.0, 0.0};
    f64 deviation[3] = {0.0, 0.0, 0.0};
  };
  auto stats_of = [&](u32 block) {
    Stats s;
    f64 sq[3] = {0.0, 0.0, 0.0};
    u32 n = 0;
    for (i32 y = cy - inner; y < cy + inner; ++y)
      for (i32 x = cx - inner; x < cx + inner; ++x) {
        const u8* p = image + k_image_bytes * block + (u64(y) * k_size + u64(x)) * 4;
        for (u32 c = 0; c < 3; ++c) {
          const f64 v = linear_of(p[c]);
          s.mean[c] += v;
          sq[c] += v * v;
        }
        ++n;
      }
    for (u32 c = 0; c < 3; ++c) {
      s.mean[c] /= n;
      s.deviation[c] = std::sqrt(std::max(0.0, sq[c] / n - s.mean[c] * s.mean[c]));
    }
    return s;
  };
  const Stats plain = stats_of(0);
  const Stats stochastic = stats_of(1);
  for (u32 c = 0; c < 3; ++c) {
    const f64 shift = std::abs(stochastic.mean[c] / plain.mean[c] - 1.0);
    CHECK_MESSAGE(shift < 0.01, "channel " << c << ": plain mean " << plain.mean[c]
                                           << ", stochastic " << stochastic.mean[c]);
    // The contrast holds: a plain average of three tiles would cut the deviation by up to √3.
    CHECK_MESSAGE(stochastic.deviation[c] > 0.8 * plain.deviation[c],
                  "channel " << c << ": plain deviation " << plain.deviation[c] << ", stochastic "
                             << stochastic.deviation[c]);
    MESSAGE("channel " << c << ": texture mean " << mean[c] << ", plain " << plain.mean[c] << " +- "
                       << plain.deviation[c] << ", stochastic " << stochastic.mean[c] << " +- "
                       << stochastic.deviation[c]);
  }
  u32 differing = 0;
  u32 counted = 0;
  for (i32 y = cy - inner; y < cy + inner; ++y)
    for (i32 x = cx - inner; x < cx + inner; ++x) {
      const u64 at = (u64(y) * k_size + u64(x)) * 4;
      ++counted;
      if (std::memcmp(image + at, image + k_image_bytes + at, 3) != 0) ++differing;
    }
  CHECK_MESSAGE(differing * 2 > counted, differing << " of " << counted << " pixels differ");
  // An alpha-tested row that carries the flag anyway reads plain: the resolve does not take a
  // cutoff for means.
  CHECK(std::memcmp(image + k_image_bytes * 2, image + k_image_bytes * 3, k_image_bytes) == 0);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, sampler);
  gfx::destroy_image_view(device, texture_view);
  gfx::destroy_image(device, texture);
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &materials, &attributes, &triangles, &clusters})
    gfx::destroy_buffer(device, *b);
  for (gfx::BufferResource& b : material_index)
    gfx::destroy_buffer(device, b);
  frames.destroy();
  device.destroy();
}
