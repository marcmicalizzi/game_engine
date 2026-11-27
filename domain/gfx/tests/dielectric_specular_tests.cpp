// A dielectric's own reflectance (R234; gfx.md, "A dielectric's own reflectance"): what
// `KHR_materials_ior` and `KHR_materials_specular` make of the BSDF, checked three ways.
//
// - The record (`gfx::set_dielectric_specular`) holds the two specifications' F0 and F90, from
//   their formulas in double precision (`brdf_ref::specular_of`), and the defaults hold zero, which
//   is what a material that states neither holds.
// - Energy is not gained: the BSDF's directional albedo, by importance sampling with the path
//   tracer's own sampler, stays at or under one for every index, factor and colour down to a view
//   a hundredth of a radian off grazing, and the hemisphere's two weights stay under one too.
// - The resolve draws a sphere as the reference does (brdf_reference.h, in double precision) at
//   three indices (1.33, 1.5, 2.0) and three specular factors (0, 0.5, 1), a coloured specular, a
//   coated glass, the specular-glossiness mode and a half-metal, every covered pixel within 2 of 255
//   — the shading test's tolerance. The sphere has no vertex normals, so each pixel is its own
//   triangle's plane, which the visibility buffer names, and the silhouette is the grazing view.
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
#include <cstring>
#include <shaders/visibility_resolve.spv.h>
#include <string>
#include <vector>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

// One material of the sweep: what it states, and the base it states it over.
struct Declared {
  const char* name;
  f32 ior;
  f32 specular;
  Vec3 colour;
  f32 metallic;
};

constexpr Vec3 k_white{1.0f, 1.0f, 1.0f};
constexpr Declared k_declared[] = {
    {"ior 1.33, specular 1", 1.33f, 1.0f, k_white, 0.0f},
    {"ior 1.5, specular 1 (the defaults)", 1.5f, 1.0f, k_white, 0.0f},
    {"ior 2.0, specular 1", 2.0f, 1.0f, k_white, 0.0f},
    {"ior 1.33, specular 0.5", 1.33f, 0.5f, k_white, 0.0f},
    {"ior 1.5, specular 0.5", 1.5f, 0.5f, k_white, 0.0f},
    {"ior 2.0, specular 0.5", 2.0f, 0.5f, k_white, 0.0f},
    {"ior 1.33, specular 0", 1.33f, 0.0f, k_white, 0.0f},
    {"ior 1.5, specular 0", 1.5f, 0.0f, k_white, 0.0f},
    {"ior 2.0, specular 0", 2.0f, 0.0f, k_white, 0.0f},
    {"ior 1.5, a coloured specular", 1.5f, 1.0f, Vec3{1.0f, 0.55f, 0.25f}, 0.0f},
    {"coated glass: ior 1.52, colour 2.5", 1.52f, 1.0f, Vec3{2.5f, 2.5f, 2.5f}, 0.0f},
    {"ior 0: the specular-glossiness mode", 0.0f, 0.8f, Vec3{0.3f, 0.25f, 0.2f}, 0.0f},
    {"half metal, ior 2.0, coloured", 2.0f, 1.0f, Vec3{1.0f, 0.55f, 0.25f}, 0.5f},
};
constexpr u32 k_count = sizeof(k_declared) / sizeof(k_declared[0]);

ref::Surface reference_surface(const gfx::ResolveMaterial& m) {
  ref::Surface s;
  s.albedo = ref::dvec3(m.albedo);
  s.roughness = static_cast<double>(m.albedo.w);
  s.metallic = static_cast<double>(m.emissive.w);
  s.specular = ref::dvec3(m.specular);
  s.specular_w = static_cast<double>(m.specular.w);
  return s;
}

}  // namespace

TEST_CASE("dielectric specular: the record is the extensions' F0 and F90") {
  // The defaults store zero, word for word: the stand-in, drawn by the lines it always took.
  gfx::ResolveMaterial defaults;
  gfx::set_dielectric_specular(defaults, 1.5f, 1.0f, k_white);
  CHECK(defaults.specular.x == 0.0f);
  CHECK(defaults.specular.y == 0.0f);
  CHECK(defaults.specular.z == 0.0f);
  CHECK(defaults.specular.w == 0.0f);
  // An IOR the extension does not allow reads as its default, and so does a NaN.
  for (const f32 bad : {0.5f, -1.0f, std::nanf("")}) {
    gfx::ResolveMaterial m;
    gfx::set_dielectric_specular(m, bad, 1.0f, k_white);
    CHECK(m.specular.x == 0.0f);
    CHECK(m.specular.w == 0.0f);
  }
  for (const Declared& d : k_declared) {
    gfx::ResolveMaterial m;
    gfx::set_dielectric_specular(m, d.ior, d.specular, d.colour);
    ref::Dvec3 delta;
    double w = 0.0;
    ref::specular_of(static_cast<double>(d.ior), static_cast<double>(d.specular), ref::dvec3(d.colour),
                     delta, w);
    // The record's floats are the formula's doubles rounded once.
    CHECK(std::abs(static_cast<double>(m.specular.x) - delta.x) < 1e-7);
    CHECK(std::abs(static_cast<double>(m.specular.y) - delta.y) < 1e-7);
    CHECK(std::abs(static_cast<double>(m.specular.z) - delta.z) < 1e-7);
    CHECK(std::abs(static_cast<double>(m.specular.w) - w) < 1e-7);
  }
  // The numbers the specifications name: an IOR of 1.5 is 4 %, 1.33 is 2.0 % and 2.0 is 11.1 %;
  // the specular factor scales F0 and is F90; a colour scales F0 and is clamped at one.
  ref::Dvec3 delta;
  double w = 0.0;
  ref::specular_of(2.0, 1.0, ref::splat(1.0), delta, w);
  CHECK(std::abs(delta.x + 0.04 - 1.0 / 9.0) < 1e-15);
  ref::specular_of(1.33, 0.5, ref::splat(1.0), delta, w);
  CHECK(std::abs(delta.x + 0.04 - 0.5 * (0.33 / 2.33) * (0.33 / 2.33)) < 1e-15);
  CHECK(w == 0.5);
  ref::specular_of(0.0, 1.0, ref::Dvec3{2.0, 0.5, 0.0}, delta, w);
  CHECK(delta.x + 0.04 == 1.0);
  CHECK(std::abs(delta.y + 0.04 - 0.5) < 1e-15);
  CHECK(std::abs(delta.z + 0.04) < 1e-15);
  // And the surface's Fresnel is F0 face on and F90 at grazing.
  for (const Declared& d : k_declared) {
    gfx::ResolveMaterial m;
    m.albedo = Vec4{0.6f, 0.3f, 0.1f, 0.4f};
    m.emissive.w = d.metallic;
    gfx::set_dielectric_specular(m, d.ior, d.specular, d.colour);
    const ref::Surface s = reference_surface(m);
    const ref::Dvec3 face_on = ref::fresnel_of(s, 1.0);
    const ref::Dvec3 grazing = ref::fresnel_of(s, 0.0);
    const ref::Dvec3 f0 = ref::surface_f0(s);
    CHECK(std::abs(face_on.x - f0.x) < 1e-15);
    CHECK(std::abs(grazing.y - ref::surface_f90(s)) < 1e-15);
  }
}

TEST_CASE("dielectric specular: energy is not gained at grazing angles") {
  // The specular lobe's directional albedo, the integral of f cos over the hemisphere with the
  // base colour black (no diffuse lobe), by the path tracer's own mixture (`sample_bsdf`: the GGX
  // lobe by its visible normals and the cosine lobe, chosen by the Fresnel), on a stratified 96 x 96
  // grid of the two directions' numbers, the lobe chosen by a third stratified number. What the
  // extensions set is F0 and F90, both at most one, so this lobe never passes one at any view.
  //
  // The whole BSDF with a white base colour is reported beside it and not held to one: its diffuse
  // lobe takes 1 - F(v.h) per direction (glTF's own sample implementation's coupling), which gains
  // energy at grazing views with or without the extensions — the stand-in reaches 1.7 at
  // roughness 0.1 and n.v 0.01 (gfx.md, "A dielectric's own reflectance").
  constexpr u32 k_grid = 96;
  const double views[] = {1.0, 0.5, 0.2, 0.05, 0.01};
  const double roughness[] = {0.1, 0.5, 1.0};
  const auto directional_albedo = [&](ref::Surface s, double n_dot_v) {
    const ref::Dvec3 v{std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v};
    s.normal = ref::Dvec3{0.0, 0.0, 1.0};
    s.view = v;
    ref::Dvec3 sum{};
    for (u32 i = 0; i < k_grid; ++i) {
      for (u32 j = 0; j < k_grid; ++j) {
        const double u1 = (static_cast<double>(i) + 0.5) / k_grid;
        const double u2 = (static_cast<double>(j) + 0.5) / k_grid;
        const double lobe = std::fmod((i * k_grid + j) * 0.6180339887498949, 1.0);
        ref::BsdfSample sample;
        if (ref::sample_bsdf(s, v, lobe, u1, u2, sample)) sum = sum + sample.weight;
      }
    }
    const ref::Dvec3 albedo = sum * (1.0 / (k_grid * k_grid));
    return std::max(albedo.x, std::max(albedo.y, albedo.z));
  };
  double worst = 0.0;
  std::string worst_at;
  double worst_ambient = -1.0;
  double whole_worst = 0.0;
  double whole_stand_in = 0.0;
  for (const Declared& d : k_declared) {
    for (const double r : roughness) {
      gfx::ResolveMaterial m;
      m.albedo = Vec4{0.0f, 0.0f, 0.0f, static_cast<f32>(r)};
      m.emissive.w = d.metallic;
      gfx::set_dielectric_specular(m, d.ior, d.specular, d.colour);
      ref::Surface black = reference_surface(m);
      ref::Surface white = black;
      white.albedo = ref::splat(1.0);
      ref::Surface stand_in = white;
      stand_in.specular = ref::Dvec3{};
      stand_in.specular_w = 0.0;
      for (const double n_dot_v : views) {
        const double lobe = directional_albedo(black, n_dot_v);
        if (lobe > worst) {
          worst = lobe;
          worst_at = std::string(d.name) + ", roughness " + std::to_string(r) + ", n.v " +
                     std::to_string(n_dot_v);
        }
        CHECK_MESSAGE(lobe <= 1.0 + 0.01, std::string(d.name) << " roughness " << r << " n.v "
                                                              << n_dot_v
                                                              << ": the specular lobe reflects "
                                                              << lobe);
        whole_worst = std::max(whole_worst, directional_albedo(white, n_dot_v));
        whole_stand_in = std::max(whole_stand_in, directional_albedo(stand_in, n_dot_v));
        // The hemisphere's weights (brdf.slang, `brdf_env_albedo` and `brdf_env_specular_of`):
        // under a uniform environment the diffuse and the specular share never pass one, beyond
        // what the stand-in's own fit gives at that view (which is not this row's to change).
        const auto ambient = [&](const ref::Surface& at) {
          const ref::Dvec3 sum = ref::env_albedo(at, n_dot_v) * (1.0 - at.metallic) +
                                 ref::env_specular_of(at, n_dot_v);
          return std::max(sum.x, std::max(sum.y, sum.z));
        };
        const double over = ambient(white) - std::max(1.0, ambient(stand_in));
        worst_ambient = std::max(worst_ambient, over);
        CHECK_MESSAGE(over <= 1e-9, std::string(d.name)
                                        << " roughness " << r << " n.v " << n_dot_v
                                        << ": the hemisphere's weights pass one by " << over);
      }
    }
  }
  MESSAGE("the specular lobe's largest directional albedo: "
          << worst << " (" << worst_at << "); the hemisphere's weights' largest over one: "
          << worst_ambient << "; the whole BSDF, white: " << whole_worst
          << " declared, " << whole_stand_in << " the stand-in");
}
TEST_CASE("dielectric specular: the resolve draws the reference's sphere") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // A sphere of radius 1 at the origin, 32 segments by 16 rings, counter-clockwise from outside,
  // and no vertex normals: every pixel is shaded by its own triangle's plane.
  constexpr u32 k_segments = 32;
  constexpr u32 k_rings = 16;
  std::vector<Vec3> positions;
  std::vector<u32> indices;
  for (u32 i = 0; i <= k_rings; ++i) {
    const f64 theta = 3.14159265358979323846 * i / k_rings;
    for (u32 j = 0; j <= k_segments; ++j) {
      const f64 phi = 2.0 * 3.14159265358979323846 * j / k_segments;
      positions.push_back(Vec3{static_cast<f32>(std::sin(theta) * std::cos(phi)),
                               static_cast<f32>(std::cos(theta)),
                               static_cast<f32>(std::sin(theta) * std::sin(phi))});
    }
  }
  for (u32 i = 0; i < k_rings; ++i) {
    for (u32 j = 0; j < k_segments; ++j) {
      const u32 a = i * (k_segments + 1) + j;
      const u32 b = a + k_segments + 1;
      if (i != 0) {
        indices.insert(indices.end(), {a, a + 1, b});
      }
      if (i + 1 != k_rings) {
        indices.insert(indices.end(), {a + 1, b + 1, b});
      }
    }
  }
  geometry::ClusterMesh mesh;
  REQUIRE(geometry::build_clusters(std::span<const Vec3>(positions.data(), positions.size()),
                                   std::span<const u32>(indices.data(), indices.size()),
                                   geometry::ClusterBuildOptions{}, mesh, &error));
  const u32 clusters = static_cast<u32>(mesh.clusters.size());
  REQUIRE(clusters > 1);

  constexpr u32 k_size = 128;
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource cluster_buffer;
  gfx::BufferResource triangles;
  gfx_test::SingleInstance scene;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx::BufferResource lights;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             mesh.clusters.size() * sizeof(geometry::ClusterDesc), k_storage,
                             cluster_buffer, &error));
  REQUIRE(scene.create(device, mesh, clusters, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));
  // One material a block, a dark glossy base so the reflectance is what the eye sees, and one
  // block of the stand-in (no extension) after them.
  constexpr u32 k_blocks = k_count + 1;
  gfx::ResolveMaterial table[k_blocks];
  for (u32 b = 0; b < k_blocks; ++b) {
    table[b].albedo = Vec4{0.08f, 0.09f, 0.10f, 0.3f};
    table[b].emissive = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  }
  for (u32 b = 0; b < k_count; ++b) {
    table[b].emissive.w = k_declared[b].metallic;
    if (k_declared[b].metallic > 0.0f) table[b].albedo = Vec4{0.9f, 0.6f, 0.3f, 0.3f};
    gfx::set_dielectric_specular(table[b], k_declared[b].ior, k_declared[b].specular,
                                 k_declared[b].colour);
  }
  // Every cluster of block b names material b.
  std::vector<u32> material_index(u64{k_blocks} * clusters);
  for (u32 b = 0; b < k_blocks; ++b) {
    for (u32 c = 0; c < clusters; ++c)
      material_index[u64{b} * clusters + c] = b;
  }
  REQUIRE(gfx::upload_buffer(device, table, sizeof(table), k_storage, materials, &error));
  REQUIRE(gfx::upload_buffer(device, material_index.data(), material_index.size() * sizeof(u32),
                             k_storage, cluster_materials, &error));
  gfx::ResolveLight light_table[1];
  light_table[0].position_radius = Vec4{-2.0f, 1.5f, 3.0f, 12.0f};
  light_table[0].color_intensity = Vec4{1.0f, 0.85f, 0.7f, 20.0f};
  REQUIRE(gfx::upload_buffer(device, light_table, sizeof(light_table), k_storage, lights, &error));

  const Vec3 eye{0.0f, 0.4f, 3.2f};
  const Vec3 target{};
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const f32 fov_y = radians(45.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, true, vis,
      &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  // Each block twice: by the pipeline unset, then by one made with `k_spec_scene_specular` 0.
  constexpr u32 k_targets = 2 * k_blocks;
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_targets,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
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
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error),
                  error);
  // What a scene that declares no reflectance is drawn with (R234): the same SPIR-V, the record's
  // read and the declared lines folded out by the driver.
  gfx::PipelineHandle folded_pipeline = {};
  resolve_desc.specialization.set(gfx::k_spec_scene_specular, 0u);
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, resolve_desc, folded_pipeline, &error),
                  error);

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = cluster_buffer.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.visibility = vis.address;
  draw.extent = gfx::draw_extent(k_size, k_size);

  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const Vec3 sun_dir = normalize(Vec3{0.6f, 0.7f, 0.4f});
  gfx::ResolveParams base{};
  base.sky = sky;
  base.sun = Vec4{sun_dir, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = cluster_buffer.address;
  base.mesh = scene.meshes.address;
  base.instances = scene.instances.address;
  base.triangles = triangles.address;
  base.materials = materials.address;
  base.lights = lights.address;
  base.light_count = 1;
  base.width = k_size;
  base.height = k_size;
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  u64 block_address[k_blocks];
  for (u32 b = 0; b < k_blocks; ++b) {
    blocks[b] = base;
    blocks[b].cluster_materials = cluster_materials.address + u64{b} * clusters * sizeof(u32);
    block_address[b] = params.address + u64{b} * sizeof(gfx::ResolveParams);
  }

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_targets];
  for (u32 b = 0; b < k_targets; ++b) {
    targets[b] = graph.create_image(
        "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& p) { p.write(rg_vis, gfx::Access::TransferWrite); },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "visibility", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& p) {
        p.render_area(k_size, k_size);
        p.write(rg_vis, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw, clusters); });
  for (u32 b = 0; b < k_targets; ++b) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, b](gfx::PassBuilder& p) {
          gfx::ClearColor clear{};
          p.color_attachment(targets[b], gfx::LoadOp::Clear, clear);
          p.read(rg_vis, gfx::Access::FragmentRead);
        },
        [&, b](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics,
                           b < k_blocks ? resolve_pipeline : folded_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                            &block_address[b % k_blocks]);
          cb.draw(3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& p) {
        for (u32 b = 0; b < k_targets; ++b)
          p.read(targets[b], gfx::Access::TransferRead);
        p.read(rg_vis, gfx::Access::TransferRead);
        p.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 b = 0; b < k_targets; ++b) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * b;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[b]).image),
                                 gfx::vk::native(g.image_layout(targets[b])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* ids = static_cast<const u64*>(vis.mapped);
  auto pixel = [&](u32 block, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * block + u64{y} * k_size + x) * 4;
  };
  const ref::Dvec3 eye_d = ref::dvec3(eye);
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  const ref::Dvec3 ground_ref = ref::dvec3(base.ground);
  ref::Light light;
  light.position = ref::dvec3(light_table[0].position_radius);
  light.radius = static_cast<double>(light_table[0].position_radius.w);
  light.color = ref::dvec3(light_table[0].color_intensity);
  light.intensity = static_cast<double>(light_table[0].color_intensity.w);

  int worst = 0;
  u32 covered = 0;
  u32 grazing = 0;  // covered pixels whose triangle is seen within 15 degrees of its plane
  double centre_luma[k_blocks] = {};
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      const u64 id = ids[u64{y} * k_size + x];
      if (id == 0) continue;
      const u32 word = static_cast<u32>(id & 0xffffffffull);
      const u32 cluster = word >> 8;
      const u32 triangle = word & 0xffu;
      REQUIRE(cluster < clusters);
      const geometry::ClusterDesc& desc = mesh.clusters[cluster];
      const u32 packed = mesh.triangles[desc.triangle_offset + triangle];
      ref::Dvec3 corner[3];
      for (u32 k = 0; k < 3; ++k) {
        corner[k] = ref::dvec3(
            mesh.vertices[desc.vertex_offset + geometry::ClusterMesh::unpack(packed, k)]);
      }
      const ref::Dvec3 direction = ref::pixel_direction(
          eye_d, ref::dvec3(target), ref::dvec3(up), static_cast<double>(fov_y), 1.0, k_size,
          k_size, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      const ref::TriangleHit hit = ref::pixel_on_triangle(eye_d, direction, corner);
      ref::Dvec3 normal = ref::normalize(ref::cross(corner[1] - corner[0], corner[2] - corner[0]));
      const ref::Dvec3 view = ref::normalize(eye_d - hit.position);
      if (ref::dot(normal, view) < 0.0) normal = -normal;
      ++covered;
      if (ref::dot(normal, view) < 0.2588) ++grazing;
      for (u32 b = 0; b < k_blocks; ++b) {
        ref::Surface s = reference_surface(table[b]);
        s.position = hit.position;
        s.normal = normal;
        s.view = view;
        const ref::Dvec3 linear =
            ref::shade(s, sun_ref, 1.0, sky_ref, ground_ref, &light, 1, ref::Dvec3{});
        const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y),
                              ref::display(linear.z)};
        const u8* got = pixel(b, x, y);
        int here = 0;
        for (u32 c = 0; c < 3; ++c)
          here = std::max(here, std::abs(int{got[c]} - int{expect[c]}));
        worst = std::max(worst, here);
        if (here > 2) {
          CHECK_MESSAGE(here <= 2, std::string(b < k_count ? k_declared[b].name : "the stand-in")
                                       << " at " << x << "," << y << ": gpu " << int{got[0]} << ","
                                       << int{got[1]} << "," << int{got[2]} << " reference "
                                       << int{expect[0]} << "," << int{expect[1]} << ","
                                       << int{expect[2]});
        }
        if (x == k_size / 2 + 12 && y == k_size / 2 - 12)
          centre_luma[b] = 0.2126 * got[0] + 0.7152 * got[1] + 0.0722 * got[2];
      }
    }
  }
  CHECK(covered > k_size * k_size / 5);
  CHECK(grazing > 50u);
  // The defaults, stated, are the stand-in's picture byte for byte; nothing else here is.
  u32 differ_defaults = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (std::memcmp(pixel(1, x, y), pixel(k_count, x, y), 4) != 0) ++differ_defaults;
    }
  }
  CHECK(differ_defaults == 0u);
  // R234's variant: with the constant 0 the stand-in is the unset pipeline's bytes, and so is every
  // declared dielectric over the same base — the record's reflectance is read through the constant
  // alone (`material_specular`), so a scene whose records are zero loses nothing by it.
  u32 folded_checked = 0;
  u32 differ_folded = 0;
  for (u32 b = 0; b < k_blocks; ++b) {
    if (b < k_count && k_declared[b].metallic != 0.0f) continue;
    ++folded_checked;
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        if (std::memcmp(pixel(k_blocks + b, x, y), pixel(k_count, x, y), 4) != 0) ++differ_folded;
      }
    }
  }
  MESSAGE("folded variant: " << folded_checked << " materials against the stand-in, "
                             << differ_folded << " pixels differ");
  CHECK(folded_checked == k_count);
  CHECK(differ_folded == 0u);
  // And the constant is what made them alike: unset, the 2.0 index is not the stand-in.
  u32 differ_declared = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (std::memcmp(pixel(2, x, y), pixel(k_count, x, y), 4) != 0) ++differ_declared;
    }
  }
  CHECK(differ_declared > 1000u);
  // A higher index reflects more: 1.33 under 1.5 under 2.0 at the same place on the sphere.
  CHECK(centre_luma[0] < centre_luma[1]);
  CHECK(centre_luma[1] < centre_luma[2]);
  MESSAGE("sphere, " << std::string(raster.name()) << " path on "
                     << std::string(device.adapter().name) << ": " << covered << " covered pixels ("
                     << grazing << " within 15 degrees of grazing), " << k_blocks
                     << " materials, worst " << worst << " of 255 (tolerance 2)");

  graph.reset();
  gfx::destroy_pipeline(device, folded_pipeline);
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &lights, &cluster_materials, &materials, &triangles,
        &cluster_buffer}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
