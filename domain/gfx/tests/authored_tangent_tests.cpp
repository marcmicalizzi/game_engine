// The author's tangent frame in the material resolve (roadmap G2; gfx.md, "The author's tangent
// frame"). Two quads side by side, the left one's UVs mirrored in u — the case where a derived
// frame and an exporter's disagree about the bitangent — drawn once into one visibility buffer and
// resolved under six parameter blocks that differ only in the imported stream they name and the
// normal map they sample:
//
//   0. No stream: the frame the resolve derives from each triangle's positions and UVs, a map
//      leaning the normal along the bitangent.
//   1. Tangents that state the derived frame (T = dP/du, w so that cross(N, T) * w = +dP/dv on both
//      quads): **the picture is block 0's**, pixel for pixel, mirrored island included.
//   2. Tangents as a glTF exporter writes them for its top-left UV origin (the bitangent -dP/dv on
//      both islands): the reference's authored frame, and a picture apart from block 0's by the
//      normal mirrored about the T-N plane, which a sun leaning towards +z turns into a brightness.
//   3. No stream, a map leaning the normal along the tangent.
//   4. Tangents that turn across the right quad's corners: the frame interpolated at each pixel's
//      barycentrics, which no per-triangle frame can give, held to the reference pixel by pixel.
//   5. Block 2's tangents with one corner of the right quad stated as none: its triangle takes the
//      derived frame and the other keeps the author's.
//
// The quads are drawn through the device's own raster path (raster_path.h), the mesh path or the
// baseline tier's vertex path, so a run as the device is and one as the older card's profile hold
// both tiers to the same reference (brdf_reference.h). Skips only without 64-bit buffer atomics.
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

namespace {

constexpr u32 k_size = 128;

// The barycentrics of (x, z) in the triangle (a, b, c) of the y = 0 plane.
ref::Dvec3 barycentrics_xz(Vec3 a, Vec3 b, Vec3 c, f64 x, f64 z) {
  const f64 ax = static_cast<f64>(a.x), az = static_cast<f64>(a.z), bx = static_cast<f64>(b.x),
            bz = static_cast<f64>(b.z), cx = static_cast<f64>(c.x), cz = static_cast<f64>(c.z);
  const f64 det = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
  const f64 u = ((x - ax) * (cz - az) - (cx - ax) * (z - az)) / det;
  const f64 v = ((bx - ax) * (z - az) - (x - ax) * (bz - az)) / det;
  return ref::Dvec3{1.0 - u - v, u, v};
}

}  // namespace

TEST_CASE("material resolve: the author's tangent frame against the derived one") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // Two quads of the y = 0 plane, the left one x in [-5, 0] with u falling as x rises (mirrored),
  // the right one x in [0, 5] with u rising; v rises with z on both. The seam's vertices are two
  // pairs, as the weld leaves a mirrored seam.
  const Vec3 positions[8] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, -5.0f},
                             Vec3{0.0f, 0.0f, 5.0f},   Vec3{-5.0f, 0.0f, 5.0f},
                             Vec3{0.0f, 0.0f, -5.0f},  Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f},   Vec3{0.0f, 0.0f, 5.0f}};
  const u32 indices[12] = {0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6};
  const Vec3 flat{0.0f, 1.0f, 0.0f};
  const Vec3 normals[8] = {flat, flat, flat, flat, flat, flat, flat, flat};
  const Vec2 uvs[8] = {Vec2{1.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f},
                       Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  // Block 1: the derived frame stated. Left: T = -x, cross(+y, -x) = +z, so w = +1; right: T = +x,
  // cross(+y, +x) = -z, so w = -1.
  const Vec4 as_derived[8] = {Vec4{-1, 0, 0, 1}, Vec4{-1, 0, 0, 1}, Vec4{-1, 0, 0, 1},
                              Vec4{-1, 0, 0, 1}, Vec4{1, 0, 0, -1}, Vec4{1, 0, 0, -1},
                              Vec4{1, 0, 0, -1}, Vec4{1, 0, 0, -1}};
  // Block 2: the exporter's, the bitangent -z on both islands.
  const Vec4 as_exported[8] = {Vec4{-1, 0, 0, -1}, Vec4{-1, 0, 0, -1}, Vec4{-1, 0, 0, -1},
                               Vec4{-1, 0, 0, -1}, Vec4{1, 0, 0, 1},   Vec4{1, 0, 0, 1},
                               Vec4{1, 0, 0, 1},   Vec4{1, 0, 0, 1}};
  // Block 4: the right quad's tangents turned +-40 degrees about the normal, corner by corner.
  const f32 c40 = std::cos(radians(40.0f));
  const f32 s40 = std::sin(radians(40.0f));
  Vec4 turned[8];
  for (u32 i = 0; i < 8; ++i)
    turned[i] = as_derived[i];
  turned[4] = Vec4{c40, 0.0f, -s40, -1.0f};
  turned[5] = Vec4{c40, 0.0f, s40, -1.0f};
  turned[6] = Vec4{c40, 0.0f, -s40, -1.0f};
  turned[7] = Vec4{c40, 0.0f, s40, -1.0f};
  // Block 5: block 2's with the right quad's corner 5 stated as none (w = 0).
  Vec4 partial[8];
  for (u32 i = 0; i < 8; ++i)
    partial[i] = as_exported[i];
  partial[5] = Vec4{0.0f, 0.0f, 0.0f, 0.0f};

  const Vec4* sets[4] = {as_derived, as_exported, turned, partial};
  geometry::ClusterMesh meshes[4];
  for (u32 s = 0; s < 4; ++s) {
    geometry::AttributeSource source;
    source.normals = normals;
    source.uvs = uvs;
    source.tangents = std::span<const Vec4>(sets[s], 8);
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, meshes[s],
                                     &error, source));
    REQUIRE(meshes[s].clusters.size() == 1);
    REQUIRE(meshes[s].extra.size() == meshes[s].vertices.size());
  }
  // One topology for all four: the builds differ in their imported stream alone.
  const geometry::ClusterMesh& mesh = meshes[0];
  for (u32 s = 1; s < 4; ++s) {
    REQUIRE(meshes[s].triangles.size() == mesh.triangles.size());
    for (u32 t = 0; t < mesh.triangles.size(); ++t)
      REQUIRE(meshes[s].triangles[t] == mesh.triangles[t]);
    for (u32 v = 0; v < mesh.vertices.size(); ++v) {
      REQUIRE(meshes[s].vertices[v].x == mesh.vertices[v].x);
      REQUIRE(meshes[s].vertices[v].z == mesh.vertices[v].z);
    }
  }

  // Two maps, one direction each: leaning 45 degrees along the bitangent, and along the tangent.
  constexpr u8 k_zero = 128;
  constexpr u8 k_cos45 = 218;
  const u8 lean_b[16] = {k_zero, k_cos45, k_cos45, 255, k_zero, k_cos45, k_cos45, 255,
                         k_zero, k_cos45, k_cos45, 255, k_zero, k_cos45, k_cos45, 255};
  const u8 lean_t[16] = {k_cos45, k_zero, k_cos45, 255, k_cos45, k_zero, k_cos45, 255,
                         k_cos45, k_zero, k_cos45, 255, k_cos45, k_zero, k_cos45, 255};
  gfx::ImageResource b_image;
  gfx::ImageResource t_image;
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, lean_b,
                                       sizeof(lean_b), b_image, &error),
                  error);
  REQUIRE_MESSAGE(gfx::upload_image_2d(device, 2, 2, gfx::Format::R8G8B8A8Unorm, lean_t,
                                       sizeof(lean_t), t_image, &error),
                  error);
  gfx::ImageViewHandle b_view = {};
  gfx::ImageViewHandle t_view = {};
  REQUIRE(gfx::create_image_view(device, b_image, b_view, &error));
  REQUIRE(gfx::create_image_view(device, t_image, t_view, &error));
  gfx::SamplerHandle nearest = {};
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, nearest, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 b_slot = bindless.add_sampled_image(b_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 t_slot = bindless.add_sampled_image(t_view, gfx::ImageLayout::ShaderReadOnly);
  const u32 sampler_slot = bindless.add_sampler(nearest);

  const Vec3 base_color{0.8f, 0.6f, 0.3f};
  constexpr f32 k_roughness = 0.6f;
  gfx::ResolveMaterial material_set[2];
  for (gfx::ResolveMaterial& material : material_set) {
    material.albedo = Vec4{base_color, k_roughness};
    material.emissive = Vec4{};
    material.sampler = sampler_slot;
    material.normal_scale = 1.0f;
  }
  material_set[0].normal_texture = b_slot;
  material_set[1].normal_texture = t_slot;
  const u32 material_index[2] = {0, 1};

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx_test::SingleInstance scene;
  gfx::BufferResource triangles;
  gfx::BufferResource attributes;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx::BufferResource extras[4];
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(scene.create(device, mesh, 1, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.attributes.data(),
                             mesh.attributes.size() * sizeof(geometry::VertexAttributes), k_storage,
                             attributes, &error));
  for (u32 s = 0; s < 4; ++s) {
    REQUIRE(gfx::upload_buffer(device, meshes[s].extra.data(),
                               meshes[s].extra.size() * sizeof(geometry::VertexExtra), k_storage,
                               extras[s], &error));
  }
  REQUIRE(
      gfx::upload_buffer(device, material_set, sizeof(material_set), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));

  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  constexpr u32 k_blocks = 6;
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
  draw.extent = gfx::draw_extent(k_size, k_size);

  // The sun leans towards +z and +x, so a normal leaning along +z or +x is lit more than one
  // leaning the other way, and a frame's sign shows as a brightness.
  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const Vec3 sun_dir = normalize(Vec3{0.4f, 1.0f, 0.7f});
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
  // Per block: which stream (-1 none) and which map (0 along B, 1 along T).
  const int block_stream[k_blocks] = {-1, 0, 1, -1, 2, 3};
  const u32 block_map[k_blocks] = {0, 0, 0, 1, 1, 0};
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + block_map[i] * sizeof(u32);
    if (block_stream[i] >= 0) {
      blocks[i].extra = extras[block_stream[i]].address;
      blocks[i].extra_vertices = static_cast<u32>(mesh.vertices.size());
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
  const ref::Dvec3 n_flat = ref::dvec3(flat);
  auto shade = [&](ref::Dvec3 normal, ref::Dvec3 position) {
    ref::Surface s;
    s.position = position;
    s.normal = normal;
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(base_color);
    s.roughness = static_cast<double>(k_roughness);
    s.metallic = 0.0;
    return ref::shade(s, ref::dvec3(sun_dir), 1.0, ref::dvec3(sky), ref::dvec3(base.ground),
                      nullptr, 0, ref::Dvec3{});
  };
  auto apart = [](const u8* a, const u8* b) {
    int d = 0;
    for (u32 c = 0; c < 3; ++c)
      d = std::max(d, std::abs(int{a[c]} - int{b[c]}));
    return d;
  };
  auto against = [](const u8* got, ref::Dvec3 linear) {
    const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
    return std::max({std::abs(int{got[0]} - int{expect[0]}), std::abs(int{got[1]} - int{expect[1]}),
                     std::abs(int{got[2]} - int{expect[2]})});
  };

  // Every pixel well inside one triangle, with its world position and that triangle's corners.
  int worst[k_blocks] = {};
  u32 inside = 0;
  u32 left_apart = 0;
  u32 right_apart = 0;
  u32 turned_apart = 0;
  u32 partial_derived = 0;
  u32 partial_authored = 0;
  int same_worst = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const ref::Dvec3 p =
          ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                              static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
      const bool left = p.x < 0.0;
      if (std::abs(p.x) < 0.3 || std::abs(p.x) > 4.7 || std::abs(p.z) > 4.7) continue;
      // The diagonal of each quad runs from its first corner to its third.
      const u32 q = left ? 0u : 4u;
      const Vec3 a = positions[q];
      const Vec3 c = positions[q + 2];
      const f64 along =
          ((p.x - static_cast<f64>(a.x)) * static_cast<f64>(c.z - a.z) -
           (p.z - static_cast<f64>(a.z)) * static_cast<f64>(c.x - a.x)) /
          static_cast<f64>(std::sqrt((c.x - a.x) * (c.x - a.x) + (c.z - a.z) * (c.z - a.z)));
      if (std::abs(along) < 0.3) continue;
      // Which of the quad's two triangles holds the pixel: (q, q+2, q+1) or (q, q+3, q+2).
      const ref::Dvec3 b_first =
          barycentrics_xz(positions[q], positions[q + 2], positions[q + 1], p.x, p.z);
      const bool first = b_first.x >= 0.0 && b_first.y >= 0.0 && b_first.z >= 0.0;
      const u32 corner[3] = {q, q + (first ? 2u : 3u), q + (first ? 1u : 2u)};
      const ref::Dvec3 bary = first ? b_first
                                    : barycentrics_xz(positions[corner[0]], positions[corner[1]],
                                                      positions[corner[2]], p.x, p.z);
      ++inside;
      // The derived frame, the reference's own: from the triangle's corners and UVs.
      const ref::Dvec3 tri_p[3] = {ref::dvec3(positions[corner[0]]),
                                   ref::dvec3(positions[corner[1]]),
                                   ref::dvec3(positions[corner[2]])};
      const Vec2 tri_uv[3] = {uvs[corner[0]], uvs[corner[1]], uvs[corner[2]]};
      ref::Dvec3 dt;
      ref::Dvec3 db;
      REQUIRE(ref::tangent_frame(tri_p, tri_uv, n_flat, dt, db));
      auto authored = [&](const Vec4* set, ref::Dvec3& t, ref::Dvec3& b) {
        const Vec4 corner_t[3] = {set[corner[0]], set[corner[1]], set[corner[2]]};
        return ref::authored_frame(corner_t, bary, n_flat, t, b);
      };
      auto expect = [&](u32 image, ref::Dvec3 t, ref::Dvec3 b, const u8* texel) {
        const int d =
            against(pixel(image, x, y), shade(ref::map_normal(n_flat, t, b, texel, 1.0), p));
        worst[image] = std::max(worst[image], d);
        return d;
      };
      expect(0, dt, db, lean_b);
      expect(3, dt, db, lean_t);
      // 1: the derived frame stated is the derived picture.
      same_worst = std::max(same_worst, apart(pixel(1, x, y), pixel(0, x, y)));
      ref::Dvec3 at;
      ref::Dvec3 ab;
      REQUIRE(authored(as_derived, at, ab));
      expect(1, at, ab, lean_b);
      // 2: the exporter's frame, apart from the derived picture by the mirrored lean.
      REQUIRE(authored(as_exported, at, ab));
      CHECK(ref::dot(ab, db) < -0.99);
      expect(2, at, ab, lean_b);
      if (apart(pixel(2, x, y), pixel(0, x, y)) > 8) ++(left ? left_apart : right_apart);
      // 4: the turned tangents, interpolated at the pixel.
      REQUIRE(authored(turned, at, ab));
      expect(4, at, ab, lean_t);
      if (!left && apart(pixel(4, x, y), pixel(3, x, y)) > 8) ++turned_apart;
      // 5: a corner without a tangent sends its triangle to the derived frame.
      const bool touches_none = corner[0] == 5 || corner[1] == 5 || corner[2] == 5;
      if (touches_none) {
        expect(5, dt, db, lean_b);
        ++partial_derived;
      } else {
        REQUIRE(authored(partial, at, ab));
        expect(5, at, ab, lean_b);
        ++partial_authored;
      }
    }
  }
  MESSAGE(
      std::string(raster.name())
      << " path, " << inside << " pixels inside a triangle: worst against the reference "
      << worst[0] << " derived, " << worst[1] << " stated as derived, " << worst[2] << " exported, "
      << worst[3] << " derived along T, " << worst[4] << " turned, " << worst[5]
      << " with a corner of none (tolerance 2); the stated-as-derived picture is the derived one "
         "to "
      << same_worst << "; the exported frame is apart on " << left_apart << " mirrored and "
      << right_apart << " unmirrored pixels, the turned one on " << turned_apart);
  CHECK(inside > 4000);
  for (u32 i = 0; i < k_blocks; ++i)
    CHECK_MESSAGE(worst[i] <= 2, "block " << i << " is " << worst[i] << " from the reference");
  CHECK(same_worst <= 1);
  CHECK(left_apart > 1000);
  CHECK(right_apart > 1000);
  CHECK(turned_apart > 500);
  CHECK(partial_derived > 500);
  CHECK(partial_authored > 500);

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, nearest);
  gfx::destroy_image_view(device, t_view);
  gfx::destroy_image_view(device, b_view);
  gfx::destroy_image(device, t_image);
  gfx::destroy_image(device, b_image);
  scene.destroy(device);
  for (gfx::BufferResource& b : extras)
    gfx::destroy_buffer(device, b);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &cluster_materials, &materials,
                                 &attributes, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
