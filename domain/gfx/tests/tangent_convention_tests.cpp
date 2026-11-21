// Which way a normal map's green points, and which frame reads it (roadmap G2; gfx.md, "The
// author's tangent frame"; docs/experiments/tangent-space-convention.md). Two cases, each drawn
// through the device's own raster path (raster_path.h) and resolved, every pixel held to the
// double-precision reference (brdf_reference.h):
//
//   - **A bump lights the flank its height says.** A Gaussian bump's normal map generated in each
//     green convention — +Y up the image (glTF 2.0, OpenGL, `engine-content matgen`, the material
//     pack) and +Y down it (DirectX) — on two quads, one of them mirrored in u, under a low sun
//     from +z, through the derived frame and through the frame an exporter writes, placed once as
//     they are and once by a mirroring instance, scale (-1, 1, 1). The flank that faces the sun is
//     the brighter one exactly where the convention and the frame agree.
//   - **A deformed instance's frame follows its deformed surface.** The author's tangent is a rest
//     tangent, so an instance whose `deform` is not `k_invalid_deform` takes the frame derived from
//     the deformed positions: at rest it draws what the rigid instance draws, turned 90 degrees
//     about the surface (a bend: the rest tangent lies along the new normal) and about its normal
//     (a twist: the rest tangent stays where it was while the surface turns) it draws what the
//     reference computes from the deformed corners.
//
// Skips only without 64-bit buffer atomics.
#include "brdf_reference.h"
#include "raster_path.h"

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
#include <cstring>
#include <shaders/visibility_resolve.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

constexpr u32 k_size = 128;
constexpr f32 k_fov_y = 1.0471976f;  // 60 degrees
constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;

// The device, the bindless set, the raster path and the resolve: what every draw below shares.
struct Rig {
  gfx::Device device;
  gfx::BindlessSet bindless;
  gfx_test::ClusterRaster raster;
  gfx::ShaderModuleHandle module = {};
  gfx::PipelineHandle pipeline = {};
  gfx::FrameContext frames;
  gfx::SamplerHandle nearest = {};
  u32 sampler_slot = 0;
  Vector<gfx::ImageResource> images;
  Vector<gfx::ImageViewHandle> views;
  Vector<gfx::BufferResource> buffers;
  bool made = false;

  bool open() {
    if (!gfx_test::open_device(device)) return false;
    if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return false;
    std::string error;
    REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, nearest, &error));
    REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
    sampler_slot = bindless.add_sampler(nearest);
    REQUIRE(frames.create(device, 2, &error));
    module = gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                       shaders::k_visibility_resolve_spirv_size, &error);
    REQUIRE(module.valid());
    REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                  geometry::ClusterBuildOptions{}.max_triangles, &error),
                    error);
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = module;
    desc.vertex_entry = "vs_fullscreen";
    desc.fragment = module;
    desc.fragment_entry = "fs_resolve";
    desc.layout = bindless.pipeline_layout();
    desc.color_format = gfx::Format::R8G8B8A8Unorm;
    REQUIRE(gfx::create_graphics_pipeline(device, desc, pipeline, &error));
    made = true;
    return true;
  }

  // An RGBA8 image of side `n`, as a bindless slot.
  u32 image(u32 n, const Vector<u8>& rgba) {
    std::string error;
    gfx::ImageResource im;
    REQUIRE_MESSAGE(gfx::upload_image_2d(device, n, n, gfx::Format::R8G8B8A8Unorm, rgba.data(),
                                         rgba.size(), im, &error),
                    error);
    gfx::ImageViewHandle view = {};
    REQUIRE(gfx::create_image_view(device, im, view, &error));
    images.push_back(im);
    views.push_back(view);
    return bindless.add_sampled_image(view, gfx::ImageLayout::ShaderReadOnly);
  }

  u64 upload(const void* data, u64 bytes) {
    std::string error;
    gfx::BufferResource b;
    REQUIRE_MESSAGE(gfx::upload_buffer(device, data, bytes, k_storage, b, &error), error);
    buffers.push_back(b);
    return b.address;
  }

  // Draws `draw` into a fresh visibility buffer and resolves it under each of `count` blocks;
  // `out` holds the pictures one after another, RGBA8.
  void run(gfx::ClusterDrawParams draw, const gfx::ResolveParams* blocks, u32 count,
           Vector<u8>& out) {
    std::string error;
    gfx::BufferResource vis;
    gfx::BufferResource params;
    gfx::BufferResource host;
    REQUIRE(gfx::create_buffer(
        device, u64{k_size} * k_size * sizeof(u64),
        k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false,
        vis, &error));
    REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * count,
                               k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                               &error));
    REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * count,
                               gfx::BufferUsage::TransferDst, true, host, &error));
    draw.visibility = vis.address;
    draw.extent = gfx::draw_extent(k_size, k_size);
    auto* mapped = static_cast<gfx::ResolveParams*>(params.mapped);
    Vector<u64> address;
    for (u32 i = 0; i < count; ++i) {
      mapped[i] = blocks[i];
      mapped[i].visibility = vis.address;
      mapped[i].width = k_size;
      mapped[i].height = k_size;
      address.push_back(params.address + i * sizeof(gfx::ResolveParams));
    }
    {
      gfx::RenderGraph graph(device);
      const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
      const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
      Vector<gfx::RgImage> targets;
      for (u32 i = 0; i < count; ++i) {
        targets.push_back(graph.create_image(
            "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                         gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc}));
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
      for (u32 i = 0; i < count; ++i) {
        graph.add_pass(
            "resolve", gfx::PassKind::Raster,
            [&, i](gfx::PassBuilder& b) {
              gfx::ClearColor clear{};
              b.color_attachment(targets[i], gfx::LoadOp::Clear, clear);
              b.read(rg_vis, gfx::Access::FragmentRead);
            },
            [&, i](gfx::CommandList cb, gfx::RenderGraph&) {
              cb.bind_pipeline(gfx::BindPoint::Graphics, pipeline);
              bindless.bind(cb, gfx::BindPoint::Graphics);
              cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                                &address[i]);
              cb.draw(3, 1, 0, 0);
            });
      }
      graph.add_pass(
          "readback", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            for (u32 i = 0; i < count; ++i)
              b.read(targets[i], gfx::Access::TransferRead);
            b.write(rg_host, gfx::Access::TransferWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph& g) {
            for (u32 i = 0; i < count; ++i) {
              VkBufferImageCopy region{};
              region.bufferOffset = u64{k_size} * k_size * 4 * i;
              region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
              region.imageExtent = {k_size, k_size, 1};
              vkCmdCopyImageToBuffer(gfx::vk::native(cb),
                                     gfx::vk::native(g.image(targets[i]).image),
                                     gfx::vk::native(g.image_layout(targets[i])),
                                     gfx::vk::native(host.buffer), 1, &region);
            }
          });
      REQUIRE_MESSAGE(graph.compile(&error), error);
      gfx::CommandList commands = frames.begin_frame();
      graph.execute(commands);
      REQUIRE(frames.wait(frames.end_frame()));
      graph.reset();
    }
    const u64 bytes = u64{k_size} * k_size * 4 * count;
    out.resize(static_cast<u32>(bytes));
    std::memcpy(out.data(), host.mapped, static_cast<size_t>(bytes));
    for (gfx::BufferResource* b : {&host, &params, &vis})
      gfx::destroy_buffer(device, *b);
  }

  ~Rig() {
    if (!made) return;
    gfx::destroy_pipeline(device, pipeline);
    raster.destroy(device);
    gfx::destroy_shader_module(device, module);
    bindless.destroy();
    gfx::destroy_sampler(device, nearest);
    for (const gfx::ImageViewHandle v : views)
      gfx::destroy_image_view(device, v);
    for (gfx::ImageResource& im : images)
      gfx::destroy_image(device, im);
    for (gfx::BufferResource& b : buffers)
      gfx::destroy_buffer(device, b);
    frames.destroy();
    device.destroy();
  }
};

// One mesh's buffers and the descriptors over them.
struct MeshBuffers {
  u64 clusters = 0;
  u64 triangles = 0;
  u64 attributes = 0;
  u64 quantized = 0;
  u64 extra = 0;
  u32 vertices = 0;
};

MeshBuffers upload_mesh(Rig& rig, const geometry::ClusterMesh& mesh) {
  MeshBuffers m;
  m.clusters =
      rig.upload(mesh.clusters.data(), mesh.clusters.size() * sizeof(geometry::ClusterDesc));
  m.triangles = rig.upload(mesh.triangles.data(), mesh.triangles.size() * sizeof(u32));
  m.attributes = rig.upload(mesh.attributes.data(),
                            mesh.attributes.size() * sizeof(geometry::VertexAttributes));
  m.quantized = rig.upload(mesh.quantized.data(), mesh.quantized.size() * sizeof(u16));
  if (!mesh.extra.empty())
    m.extra = rig.upload(mesh.extra.data(), mesh.extra.size() * sizeof(geometry::VertexExtra));
  m.vertices = static_cast<u32>(mesh.vertices.size());
  return m;
}

// The MeshDesc and InstanceDesc of the mesh's one instance at the origin, placed by `linear`
// (identity unless named), rigid unless `pool` (the deformed positions, f32 x 3 per vertex) is
// named.
void place(Rig& rig, const geometry::ClusterMesh& mesh, const MeshBuffers& m, u64 pool, u64 slots,
           u64& mesh_desc, u64& instance_desc, const Mat4& linear = Mat4::identity()) {
  gfx::MeshDesc desc{};
  desc.quant = Vec4{mesh.quant_origin, mesh.quant_scale};
  desc.first_cluster = 0;
  desc.cluster_count = static_cast<u32>(mesh.clusters.size());
  desc.quantized = m.quantized;
  desc.deform_pool = pool;
  desc.deform_slots = slots;
  gfx::InstanceDesc instance{};
  gfx::set_instance_placement(instance, linear, WorldPos::origin());
  if (pool != 0) instance.deform = 0;
  mesh_desc = rig.upload(&desc, sizeof(desc));
  instance_desc = rig.upload(&instance, sizeof(instance));
}

int channel_apart(const u8* got, ref::Dvec3 linear) {
  const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y), ref::display(linear.z)};
  return std::max({std::abs(int{got[0]} - int{expect[0]}), std::abs(int{got[1]} - int{expect[1]}),
                   std::abs(int{got[2]} - int{expect[2]})});
}

ref::Dvec3 shade_at(ref::Dvec3 normal, ref::Dvec3 position, ref::Dvec3 eye, Vec3 albedo,
                    f32 roughness, Vec3 sun, Vec4 sky, Vec4 ground) {
  ref::Surface s;
  s.position = position;
  s.normal = normal;
  s.view = ref::normalize(eye - s.position);
  s.albedo = ref::dvec3(albedo);
  s.roughness = static_cast<double>(roughness);
  s.metallic = 0.0;
  return ref::shade(s, ref::dvec3(sun), 1.0, ref::dvec3(sky), ref::dvec3(ground), nullptr, 0,
                    ref::Dvec3{});
}

u8 unorm(double v) {
  return static_cast<u8>(std::clamp(std::lround((v * 0.5 + 0.5) * 255.0), 0l, 255l));
}

}  // namespace

// Step A's known relief (docs/experiments/tangent-space-convention.md): a height bump whose normal
// map is generated in each green convention, under a sun from +z. The surface's own normal at a
// point of the bump's +z flank leans towards +z, so a correct reading of the map lights that flank
// more than the -z one. The derived frame and the frame an exporter writes for glTF's top-left UV
// origin must both read a +Y-up map correctly — that is the convention of every map the engine
// draws — and so both must read a +Y-down map upside down.
TEST_CASE("material resolve: a bump lights the flank its normal map's convention says") {
  Rig rig;
  if (!rig.open()) return;
  std::string error;

  // The two quads of authored_tangent_tests.cpp: y = 0, the left one mirrored in u, v rising with
  // z.
  const Vec3 positions[8] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, -5.0f},
                             Vec3{0.0f, 0.0f, 5.0f},   Vec3{-5.0f, 0.0f, 5.0f},
                             Vec3{0.0f, 0.0f, -5.0f},  Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f},   Vec3{0.0f, 0.0f, 5.0f}};
  const u32 indices[12] = {0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6};
  const Vec3 flat{0.0f, 1.0f, 0.0f};
  const Vec3 normals[8] = {flat, flat, flat, flat, flat, flat, flat, flat};
  const Vec2 uvs[8] = {Vec2{1.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f},
                       Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  // The exporter's tangents: T = dP/du, the bitangent cross(N, T) * w = -z = -dP/dv on both.
  const Vec4 exported[8] = {Vec4{-1, 0, 0, -1}, Vec4{-1, 0, 0, -1}, Vec4{-1, 0, 0, -1},
                            Vec4{-1, 0, 0, -1}, Vec4{1, 0, 0, 1},   Vec4{1, 0, 0, 1},
                            Vec4{1, 0, 0, 1},   Vec4{1, 0, 0, 1}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  source.tangents = std::span<const Vec4>(exported, 8);
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));
  REQUIRE(mesh.clusters.size() == 1);

  // The bump: h = A exp(-r^2 / 2 sigma^2) about the image's centre, in UV units. The +Y-up map is
  // `engine-content matgen`'s formula (domain/matgen, "+Y is up the image"): x = -dh/du and
  // y = +dh/dv, since v runs down the image; the +Y-down map has y = -dh/dv.
  constexpr u32 k_n = 32;
  constexpr double k_sigma = 0.12;
  constexpr double k_amplitude = 0.158;  // a steepest slope of 0.8
  Vector<u8> up_map(k_n * k_n * 4);
  Vector<u8> down_map(k_n * k_n * 4);
  for (u32 j = 0; j < k_n; ++j) {
    for (u32 i = 0; i < k_n; ++i) {
      const double du = (i + 0.5) / k_n - 0.5;
      const double dv = (j + 0.5) / k_n - 0.5;
      const double g = k_amplitude * std::exp(-(du * du + dv * dv) / (2.0 * k_sigma * k_sigma));
      const double hu = -g * du / (k_sigma * k_sigma);
      const double hv = -g * dv / (k_sigma * k_sigma);
      for (u32 m = 0; m < 2; ++m) {
        const double y = m == 0 ? hv : -hv;
        const double inv = 1.0 / std::sqrt(hu * hu + y * y + 1.0);
        u8* t = (m == 0 ? up_map : down_map).data() + (j * k_n + i) * 4;
        t[0] = unorm(-hu * inv);
        t[1] = unorm(y * inv);
        t[2] = unorm(inv);
        t[3] = 255;
      }
    }
  }
  const u32 up_slot = rig.image(k_n, up_map);
  const u32 down_slot = rig.image(k_n, down_map);
  const Vec3 albedo{0.7f, 0.7f, 0.7f};
  constexpr f32 k_roughness = 0.8f;
  gfx::ResolveMaterial materials[2];
  for (gfx::ResolveMaterial& material : materials) {
    material.albedo = Vec4{albedo, k_roughness};
    material.emissive = Vec4{};
    material.sampler = rig.sampler_slot;
    material.normal_scale = 1.0f;
  }
  materials[0].normal_texture = up_slot;
  materials[1].normal_texture = down_slot;
  const u32 material_index[2] = {0, 1};

  const MeshBuffers m = upload_mesh(rig, mesh);
  const u64 material_table = rig.upload(materials, sizeof(materials));
  const u64 cluster_materials = rig.upload(material_index, sizeof(material_index));

  // **A mirroring instance** (R202; docs/subsystems/gfx.md, "A mirroring instance"): the same quads
  // placed by scale (-1, 1, 1), which swaps them across x and leaves the bumps' flanks along z
  // where they were, so the sunward flank is the same one. Until the outward normal, the cofactor
  // normal pointed into the surface there and the last turn towards the geometric normal lit the
  // other.
  for (u32 mirror = 0; mirror < 2; ++mirror) {
    const f64 sx = mirror != 0 ? -1.0 : 1.0;
    u64 mesh_desc = 0;
    u64 instance_desc = 0;
    place(rig, mesh, m, 0, 0, mesh_desc, instance_desc,
          mirror != 0 ? scaling(Vec3{-1.0f, 1.0f, 1.0f}) : Mat4::identity());

    const Vec3 eye{0.0f, 10.0f, 0.0f};
    const Vec3 target{};
    const Vec3 up{0.0f, 0.0f, -1.0f};
    const Mat4 view_proj = perspective_reversed_z(k_fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
    gfx::ClusterDrawParams draw{};
    draw.view_proj = view_proj;
    draw.clusters = m.clusters;
    draw.mesh = mesh_desc;
    draw.instances = instance_desc;
    draw.triangles = m.triangles;

    // The sun low over +z, in the y-z plane so that only the bitangent's sign shows.
    const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
    const Vec3 sun = normalize(Vec3{0.0f, 0.6f, 1.0f});
    gfx::ResolveParams base{};
    base.sky = sky;
    base.sun = Vec4{sun, 1.0f};
    base.camera = Vec4{eye, 0.0f};
    base.view_proj = view_proj;
    base.clusters = m.clusters;
    base.mesh = mesh_desc;
    base.instances = instance_desc;
    base.triangles = m.triangles;
    base.materials = material_table;
    base.attributes = m.attributes;
    base.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
    // Blocks: (frame, convention) = (derived, +Y up), (exported, +Y up), (derived, +Y down),
    // (exported, +Y down).
    constexpr u32 k_blocks = 4;
    const bool block_stream[k_blocks] = {false, true, false, true};
    const u32 block_map[k_blocks] = {0, 0, 1, 1};
    gfx::ResolveParams blocks[k_blocks];
    for (u32 i = 0; i < k_blocks; ++i) {
      blocks[i] = base;
      blocks[i].cluster_materials = cluster_materials + block_map[i] * sizeof(u32);
      if (block_stream[i]) {
        blocks[i].extra = m.extra;
        blocks[i].extra_vertices = m.vertices;
      }
    }
    Vector<u8> pictures;
    rig.run(draw, blocks, k_blocks, pictures);
    auto pixel = [&](u32 image, u32 x, u32 y) {
      return pictures.data() + (u64{k_size} * k_size * image + y * k_size + x) * 4;
    };

    // The reference's two frames, each constant over a quad: the derived one from a triangle's
    // corners and UVs, the authored one from the exporter's tangents, both as placed: positions and
    // tangents through the instance, the outward normal (+y either way) and its handedness.
    ref::Dvec3 frame_t[2][2];  // [quad: 0 the mesh's left, 1 its right][0 derived, 1 authored]
    ref::Dvec3 frame_b[2][2];
    for (u32 q = 0; q < 2; ++q) {
      const u32 c[3] = {q * 4, q * 4 + 2, q * 4 + 1};
      ref::Dvec3 p[3];
      Vec4 corner_t[3];
      for (u32 k = 0; k < 3; ++k) {
        p[k] = ref::dvec3(positions[c[k]]);
        p[k].x *= sx;
        corner_t[k] = exported[c[k]];
        corner_t[k].x *= static_cast<f32>(sx);
      }
      const Vec2 tri_uv[3] = {uvs[c[0]], uvs[c[1]], uvs[c[2]]};
      REQUIRE(ref::tangent_frame(p, tri_uv, ref::dvec3(flat), frame_t[q][0], frame_b[q][0]));
      REQUIRE(ref::authored_frame(corner_t, ref::Dvec3{1.0 / 3, 1.0 / 3, 1.0 / 3}, ref::dvec3(flat),
                                  frame_t[q][1], frame_b[q][1], sx));
    }

    int worst[k_blocks] = {};
    u32 held = 0;
    double flank_sum[k_blocks][2][2] = {};  // [block][quad][0 the +z flank, 1 the -z flank]
    u32 flank_count[2][2] = {};
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const ref::Dvec3 p =
            ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                static_cast<double>(k_fov_y), 1.0, k_size, k_size, x, y, 0.0);
        const double mx = p.x * sx;  // the point's x in the mesh
        const u32 q = mx < 0.0 ? 0u : 1u;
        if (std::abs(p.x) < 0.3 || std::abs(p.x) > 4.7 || std::abs(p.z) > 4.7) continue;
        const double cx = q == 0 ? -2.5 : 2.5;
        if (std::abs(mx - cx) < 0.5 && std::abs(p.z) > 0.6 && std::abs(p.z) < 2.0) {
          const u32 side = p.z > 0.0 ? 0u : 1u;
          ++flank_count[q][side];
          for (u32 i = 0; i < k_blocks; ++i) {
            const u8* c = pixel(i, x, y);
            flank_sum[i][q][side] += static_cast<double>(c[0]) + c[1] + c[2];
          }
        }
        // The reference where a pixel is well inside one texel of the nearest-sampled map.
        const double u = q == 0 ? -mx / 5.0 : mx / 5.0;
        const double v = (p.z + 5.0) / 10.0;
        const double tu = u * k_n;
        const double tv = v * k_n;
        const double fu = tu - std::floor(tu);
        const double fv = tv - std::floor(tv);
        if (fu < 0.12 || fu > 0.88 || fv < 0.12 || fv > 0.88) continue;
        const u32 texel = static_cast<u32>(tv) * k_n + static_cast<u32>(tu);
        ++held;
        for (u32 i = 0; i < k_blocks; ++i) {
          const u8* t = (block_map[i] == 0 ? up_map : down_map).data() + texel * 4;
          const u32 f = block_stream[i] ? 1u : 0u;
          const ref::Dvec3 n =
              ref::map_normal(ref::dvec3(flat), frame_t[q][f], frame_b[q][f], t, 1.0);
          const int d = channel_apart(pixel(i, x, y), shade_at(n, p, ref::dvec3(eye), albedo,
                                                               k_roughness, sun, sky, base.ground));
          worst[i] = std::max(worst[i], d);
        }
      }
    }
    const std::string names[k_blocks] = {"derived frame, +Y-up map", "exported frame, +Y-up map",
                                         "derived frame, +Y-down map",
                                         "exported frame, +Y-down map"};
    const std::string path =
        std::string(rig.raster.name()) + (mirror != 0 ? " path, a mirroring instance" : " path");
    CHECK(held > 3000);
    for (u32 q = 0; q < 2; ++q)
      for (u32 side = 0; side < 2; ++side)
        CHECK(flank_count[q][side] > 40);
    for (u32 i = 0; i < k_blocks; ++i) {
      bool lit_right[2];
      for (u32 q = 0; q < 2; ++q) {
        const double plus = flank_sum[i][q][0] / flank_count[q][0];
        const double minus = flank_sum[i][q][1] / flank_count[q][1];
        lit_right[q] = plus > minus;
        MESSAGE(path << ", " << names[i] << ", " << std::string(q == 0 ? "mirrored" : "unmirrored")
                     << " quad: the sunward flank " << plus / 3.0 << ", the other " << minus / 3.0
                     << " (mean code): "
                     << std::string(plus > minus ? "the right flank lit" : "the wrong flank lit"));
      }
      // A +Y-up map is read the right way up by both frames, on both islands; a +Y-down one the
      // wrong way up, which is what glTF's convention says of it.
      const bool expect_right = block_map[i] == 0;
      CHECK_MESSAGE(lit_right[0] == expect_right,
                    path << ", " << names[i] << " on the mirrored quad");
      CHECK_MESSAGE(lit_right[1] == expect_right,
                    path << ", " << names[i] << " on the unmirrored quad");
      MESSAGE(path << ", " << names[i] << ": worst " << worst[i]
                   << " of 255 from the reference over " << held << " pixels");
      CHECK_MESSAGE(worst[i] <= 2,
                    path << ", " << names[i] << " is " << worst[i] << " from the reference");
    }
  }
}

// **M3**: a deformed instance takes the derived frame, built from the positions it is drawn at, so
// its normal map follows the skin. One quad (the unmirrored one above) carrying the exporter's
// tangents, a map leaning the normal along both T and B, drawn four ways: rigid; deformed at rest
// (the pool holding the rest positions and normals); bent, the pool turned 90 degrees about z so
// the surface faces +x and its rest tangent +x lies along its normal; twisted, turned 90 degrees
// about y, its own normal, so the rest tangent stays +x while the surface's du runs along -z. The
// camera and the sun turn with the bend and the twist. Every pixel is held to the reference built
// from the deformed corners, and the rest and the twist are the rigid picture.
TEST_CASE("material resolve: a deformed instance's tangent frame follows its deformed surface") {
  Rig rig;
  if (!rig.open()) return;
  std::string error;

  const Vec3 positions[4] = {Vec3{0.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  const Vec3 flat{0.0f, 1.0f, 0.0f};
  const Vec3 normals[4] = {flat, flat, flat, flat};
  const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
  const Vec4 exported[4] = {Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}};
  geometry::AttributeSource source;
  source.normals = normals;
  source.uvs = uvs;
  source.tangents = std::span<const Vec4>(exported, 4);
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                   &error, source));
  REQUIRE(mesh.clusters.size() == 1);
  REQUIRE(mesh.clusters[0].vertex_offset == 0);

  // A constant map leaning the normal half along T and half along B.
  const u8 lean[4] = {191, 191, 218, 255};
  Vector<u8> map;
  for (u32 i = 0; i < 16; ++i)
    map.push_back(lean[i % 4]);
  const Vec3 albedo{0.8f, 0.6f, 0.3f};
  constexpr f32 k_roughness = 0.6f;
  gfx::ResolveMaterial material;
  material.albedo = Vec4{albedo, k_roughness};
  material.emissive = Vec4{};
  material.sampler = rig.sampler_slot;
  material.normal_scale = 1.0f;
  material.normal_texture = rig.image(2, map);
  const u32 material_index = 0;
  const MeshBuffers m = upload_mesh(rig, mesh);
  const u64 material_table = rig.upload(&material, sizeof(material));
  const u64 cluster_materials = rig.upload(&material_index, sizeof(material_index));
  const u32 slot_base = 0;
  const u64 slots = rig.upload(&slot_base, sizeof(slot_base));

  // The four poses: a rotation each (rows), applied to positions, normals, the camera and the sun.
  struct Pose {
    const char* name;
    bool deformed;
    Mat4 turn;
  };
  const Pose poses[4] = {
      {"rigid", false, Mat4::identity()},
      {"deformed at rest", true, Mat4::identity()},
      {"bent 90 degrees about z", true,
       mat4_from_quat(quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(-90.0f)))},
      {"twisted 90 degrees about y", true,
       mat4_from_quat(quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(90.0f)))}};
  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const Vec3 sun_rest = normalize(Vec3{0.4f, 1.0f, 0.7f});
  const Vec3 eye_rest{2.5f, 10.0f, 0.0f};
  const Vec3 target_rest{2.5f, 0.0f, 0.0f};
  const Vec3 up_rest{0.0f, 0.0f, -1.0f};
  Vector<u8> pictures[4];
  int worst[4] = {};
  u32 held[4] = {};
  for (u32 k = 0; k < 4; ++k) {
    const Pose& pose = poses[k];
    auto turn_point = [&](Vec3 v) { return transform_point(pose.turn, v); };
    auto turn_direction = [&](Vec3 v) { return transform_direction(pose.turn, v); };
    u64 pool = 0;
    u64 normal_pool = 0;
    if (pose.deformed) {
      Vector<f32> moved;
      Vector<u32> moved_normals;
      for (u32 v = 0; v < mesh.vertices.size(); ++v) {
        const Vec3 p = turn_point(mesh.vertices[v]);
        moved.push_back(p.x);
        moved.push_back(p.y);
        moved.push_back(p.z);
        moved_normals.push_back(geometry::encode_normal_oct(turn_direction(flat)));
      }
      pool = rig.upload(moved.data(), moved.size() * sizeof(f32));
      normal_pool = rig.upload(moved_normals.data(), moved_normals.size() * sizeof(u32));
    }
    u64 mesh_desc = 0;
    u64 instance_desc = 0;
    place(rig, mesh, m, pool, pose.deformed ? slots : 0, mesh_desc, instance_desc);
    const Vec3 eye = turn_point(eye_rest);
    const Vec3 target = turn_point(target_rest);
    const Vec3 up = turn_direction(up_rest);
    const Vec3 sun = turn_direction(sun_rest);
    const Mat4 view_proj = perspective_reversed_z(k_fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
    gfx::ClusterDrawParams draw{};
    draw.view_proj = view_proj;
    draw.clusters = m.clusters;
    draw.mesh = mesh_desc;
    draw.instances = instance_desc;
    draw.triangles = m.triangles;
    gfx::ResolveParams block{};
    block.sky = sky;
    block.sun = Vec4{sun, 1.0f};
    block.camera = Vec4{eye, 0.0f};
    block.view_proj = view_proj;
    block.clusters = m.clusters;
    block.mesh = mesh_desc;
    block.instances = instance_desc;
    block.triangles = m.triangles;
    block.materials = material_table;
    block.cluster_materials = cluster_materials;
    block.attributes = m.attributes;
    block.normal_pool = normal_pool;
    block.extra = m.extra;
    block.extra_vertices = m.vertices;
    block.mode = static_cast<u32>(gfx::ResolveMode::Shaded);
    rig.run(draw, &block, 1, pictures[k]);

    // The reference from the deformed corners: the derived frame of the turned triangle, against
    // the turned normal, shaded at the point the pixel's ray meets the turned plane.
    ref::Dvec3 corner[3];
    const u32 c[3] = {0, 2, 1};
    for (u32 i = 0; i < 3; ++i)
      corner[i] = ref::dvec3(turn_point(positions[c[i]]));
    const Vec2 tri_uv[3] = {uvs[c[0]], uvs[c[1]], uvs[c[2]]};
    const ref::Dvec3 n = ref::dvec3(turn_direction(flat));
    ref::Dvec3 t;
    ref::Dvec3 b;
    REQUIRE(ref::tangent_frame(corner, tri_uv, n, t, b));
    const ref::Dvec3 mapped = ref::map_normal(n, t, b, lean, 1.0);
    if (k == 2) {
      // What the rest tangent leaves once it is made orthogonal to the bent normal: the reason the
      // author's frame cannot stand for a deformed instance.
      const ref::Dvec3 rest_t{1.0, 0.0, 0.0};
      const ref::Dvec3 left = rest_t - n * ref::dot(n, rest_t);
      MESSAGE("bent: the rest tangent's part in the surface is " << std::sqrt(ref::dot(left, left))
                                                                 << " long");
    }
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const ref::Dvec3 dir = ref::pixel_direction(ref::dvec3(eye), ref::dvec3(target),
                                                    ref::dvec3(up), static_cast<double>(k_fov_y),
                                                    1.0, k_size, k_size, x + 0.5, y + 0.5);
        // Where the ray meets the quad's plane: the hit's own barycentrics, unclamped, since the
        // quad's other triangle lies in the same plane under the same frame.
        const ref::TriangleHit hit = ref::pixel_on_triangle(ref::dvec3(eye), dir, corner);
        const ref::Dvec3 point =
            corner[0] * hit.ray.x + corner[1] * hit.ray.y + corner[2] * hit.ray.z;
        // Back in the rest frame, the pixel must be well inside the quad.
        const Vec3 rest = transform_point(
            transpose(pose.turn),
            Vec3{static_cast<f32>(point.x), static_cast<f32>(point.y), static_cast<f32>(point.z)});
        if (rest.x < 0.3f || rest.x > 4.7f || std::abs(rest.z) > 4.7f) continue;
        ++held[k];
        const u8* got = pictures[k].data() + (y * k_size + x) * 4;
        worst[k] =
            std::max(worst[k], channel_apart(got, shade_at(mapped, point, ref::dvec3(eye), albedo,
                                                           k_roughness, sun, sky, block.ground)));
      }
    }
  }
  int rest_apart = 0;
  int twist_apart = 0;
  for (u32 i = 0; i < k_size * k_size * 4; ++i) {
    if (i % 4 == 3) continue;
    rest_apart = std::max(rest_apart, std::abs(int{pictures[1][i]} - int{pictures[0][i]}));
    twist_apart = std::max(twist_apart, std::abs(int{pictures[3][i]} - int{pictures[0][i]}));
  }
  for (u32 k = 0; k < 4; ++k) {
    const std::string pose = poses[k].name;
    MESSAGE(std::string(rig.raster.name())
            << " path, " << pose << ": worst " << worst[k] << " of 255 from the reference over "
            << held[k] << " pixels");
    CHECK(held[k] > 4000);
    CHECK_MESSAGE(worst[k] <= 2, pose << " is " << worst[k] << " from the reference");
  }
  MESSAGE("at rest against rigid: worst " << rest_apart << "; twisted against rigid: worst "
                                          << twist_apart);
  CHECK(rest_apart <= 1);
  CHECK(twist_apart <= 1);
}
