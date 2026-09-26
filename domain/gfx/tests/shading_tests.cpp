// The material resolve's lighting model, checked against a second implementation: a horizontal
// quad under a known sun and a known point light, seen from above, shaded once per material for
// a sweep of roughness and metallic. Every render's center pixel is compared to the CPU
// reference of brdf_reference.h, which evaluates the same Cook-Torrance BSDF in double
// precision, so no expected value in this file is a number someone wrote down by hand. Also:
// the normals mode returns the plane normal, a light outside its radius of influence changes
// nothing at all, and a white dielectric lit head-on reflects what the BSDF says it should. The
// quad reaches the visibility buffer through the device's own raster path (raster_path.h): the
// mesh path where there are mesh shaders and the vertex path, the baseline tier, where there are
// not — the lighting model is the resolve's and has to hold on both tiers. Skips only without
// 64-bit buffer atomics.
//
// The tolerance is 2 of 255 on the displayed value, and it is meant to hold on any conformant
// device rather than to be one GPU's calibration: the RTX 5090 through the mesh path and the
// TITAN Xp through the vertex path both land on the reference exactly (0 of 255, 2026-09-24), and
// the allowance is for a GPU whose pow and sqrt are within Vulkan's precision bounds without being
// correctly rounded, which can move a displayed value by one where it sits on a rounding edge.
//
// The second case is the ray-traced shadows: the same quad with an occluder above it that only
// the acceleration structure holds, shaded with the sun's shadow ray, with the point light's, and
// against a scene with nothing to occlude. Every pixel is compared against the same CPU
// reference, told whether that light is shadowed by a CPU ray-quad intersection. Skips without
// VK_KHR_ray_query.
//
// The third is the same comparison for the **cascaded shadow maps**, the baseline tier's shadow:
// the occluder is drawn into a depth map from the sun by the cluster rasterizer — through the mesh
// path and the vertex path, each where the device has it — and not into the picture, and the
// resolve filters the map. It needs nothing but the visibility buffer, so it runs on every device
// the renderer runs on, the TITAN Xp included. The filter makes a penumbra where the ray query
// makes an edge, so the comparison leaves out a band around the shadow's edge as wide as the
// filter's footprint (stated where it is computed); everywhere else a shadowed pixel is the
// reference with the sun removed, within 2 of 255, and a lit pixel is the unshadowed picture byte
// for byte.
#include "brdf_reference.h"
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/acceleration.h>
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
#include <shaders/cluster_vertex.spv.h>
#include <shaders/visibility_resolve.spv.h>
#include <shaders/visibility_resolve_rt.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

// The sweep: four roughnesses at both ends of the metallic range, all with the same base color.
constexpr f32 k_roughness[4] = {0.05f, 0.3f, 0.7f, 1.0f};
constexpr u32 k_materials = 8;  // k_roughness x {dielectric, conductor}
constexpr u32 k_white = 8;      // the reflectance-sanity material
constexpr u32 k_blocks = 15;    // one ResolveParams, one render target, per block
constexpr u32 k_below = 12;     // the first block that sees the quad from below
constexpr u32 k_size = 128;

// A horizontal rectangle, the shadow test's occluder: y = plane, x in [x0, x1], z in [z0, z1].
struct Quad {
  double plane, x0, x1, z0, z1;
};

// How far inside the rectangle a ray crosses its plane, in world units: positive when the ray is
// blocked, negative when it passes beside, and far negative when it never reaches the plane
// within `max_t`. The sign is the CPU's own answer to "is this point shadowed" and the magnitude
// is the distance to the shadow's edge, so the pixels straddling that edge — where a fraction of
// a pixel decides the answer — can be left out of the comparison instead of being fudged.
double quad_margin(ref::Dvec3 origin, ref::Dvec3 direction, const Quad& quad, double max_t) {
  if (std::fabs(direction.y) < 1e-12) return -1.0e30;
  const double t = (quad.plane - origin.y) / direction.y;
  if (t <= 0.0 || t >= max_t) return -1.0e30;
  const double x = origin.x + direction.x * t;
  const double z = origin.z + direction.z * t;
  return std::min(std::min(x - quad.x0, quad.x1 - x), std::min(z - quad.z0, quad.z1 - z));
}

}  // namespace

TEST_CASE("material resolve: shading matches a CPU reference over roughness and metallic") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // One quad on y = 0, two triangles, one cluster.
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  geometry::ClusterMesh mesh;
  REQUIRE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error));
  REQUIRE(mesh.clusters.size() == 1);

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx_test::SingleInstance scene;
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
  REQUIRE(scene.create(device, mesh, 1, &error));
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
  // The same quad seen from below, into a visibility buffer of its own. The mesh has no vertex
  // normals, so the resolve shades it with the plane's normal turned towards the camera, (0, -1,
  // 0): a surface facing straight down at the ground, with the sun and the point light both above
  // it.
  const Vec3 eye_below{0.0f, -10.0f, 0.0f};
  const Mat4 view_proj_below =
      perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye_below, target, up);
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource vis_below;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false,
      vis_below, &error));
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
  const Vec3 sun_dir = normalize(Vec3{0.0f, 1.0f, 0.45f});  // 24 degrees off the quad's normal
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
  // Blocks 12..14: the roughest dielectric seen from below, facing the ground. Nothing above the
  // quad reaches it — the sun and the point light are both behind it — so what lights it is the
  // hemisphere's lower half alone: the ground lit by the sun and the sky. 12 is the default,
  // neutral ground; 13 a sand-coloured one; 14 no ground at all, which must leave it black.
  const Vec4 sand{0.84f, 0.69f, 0.47f, 0.0f};
  for (u32 i = k_below; i < k_blocks; ++i) {
    blocks[i] = base;
    blocks[i].cluster_materials = cluster_materials.address + 3 * sizeof(u32);
    blocks[i].camera = Vec4{eye_below, 0.0f};
    blocks[i].view_proj = view_proj_below;
    blocks[i].visibility = vis_below.address;
  }
  blocks[k_below + 1].ground = sand;
  blocks[k_below + 2].ground = Vec4{};
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

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
          b.read(i < k_below ? rg_vis : rg_vis_below, gfx::Access::FragmentRead);
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
  // The default ground, which the quad seen from above never looks at: its normal is straight up,
  // where the hemisphere is the sky alone.
  const ref::Dvec3 ground_ref = ref::dvec3(base.ground);

  // Every material of the sweep: the center pixel against the reference, within 2 of 255.
  const u32 cx = k_size / 2;
  const u32 cy = k_size / 2;
  int worst = 0;
  for (u32 i = 0; i < k_materials; ++i) {
    const ref::Surface s = surface_at(cx, cy, material_table[i]);
    const ref::Dvec3 linear =
        ref::shade(s, sun_ref, 1.0, sky_ref, ground_ref, &near_light, 1, ref::Dvec3{});
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
  MESSAGE("worst reference-vs-GPU difference over the sweep, " << std::string(raster.name())
                                                               << " path: " << worst << " of 255");

  // The same comparison away from the center, where the light's falloff and the view angle
  // differ: a quarter of the way out along both axes, on the roughest dielectric.
  {
    const u32 ox = cx + k_size / 4;
    const u32 oy = cy + k_size / 8;
    const ref::Surface s = surface_at(ox, oy, material_table[3]);
    const ref::Dvec3 linear =
        ref::shade(s, sun_ref, 1.0, sky_ref, ground_ref, &near_light, 1, ref::Dvec3{});
    const u8* got = pixel(3, ox, oy);
    for (u32 c = 0; c < 3; ++c) {
      const u8 expect = ref::display(c == 0 ? linear.x : (c == 1 ? linear.y : linear.z));
      worst = std::max(worst, std::abs(int{got[c]} - int{expect}));
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
  const ref::Dvec3 head_on_linear = ref::shade(head_on, ref::Dvec3{0.0, 1.0, 0.0}, 1.0,
                                               ref::Dvec3{}, ground_ref, nullptr, 0, ref::Dvec3{});
  CHECK(std::abs(head_on_linear.x - 0.30876059) < 1.0e-7);
  CHECK(ref::display(head_on_linear.x) == 149);
  const u8* white = pixel(11, cx, cy);
  for (u32 c = 0; c < 3; ++c) {
    worst = std::max(worst, std::abs(int{white[c]} - 149));
    CHECK_MESSAGE(std::abs(int{white[c]} - 149) <= 2,
                  "white dielectric channel " << c << ": " << int{white[c]});
  }
  MESSAGE("white dielectric, roughness 1, head-on unit sun: reference "
          << head_on_linear.x << " linear, " << int{ref::display(head_on_linear.x)}
          << " displayed; gpu " << int{white[0]} << "," << int{white[1]} << "," << int{white[2]});

  // **The ground.** Seen from below, the quad faces straight down: the sun and the point light are
  // both behind it, so the hemisphere's lower half — the ground's albedo lit by the sun's
  // irradiance on a horizontal surface and by the sky's zenith — is all that lights it. Every
  // covered pixel is compared against the reference, a neutral ground and a sand-coloured one.
  auto below_at = [&](u32 x, u32 y) {
    ref::Surface s = surface_at(x, y, material_table[3]);
    s.position = ref::pixel_on_plane(ref::dvec3(eye_below), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = ref::Dvec3{0.0, -1.0, 0.0};
    s.view = ref::normalize(ref::dvec3(eye_below) - s.position);
    return s;
  };
  u32 below_covered = 0;
  int below_worst = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (is_sky(pixel(k_below, x, y))) continue;
      ++below_covered;
      for (u32 b = k_below; b < k_below + 2; ++b) {
        const ref::Dvec3 linear =
            ref::shade(below_at(x, y), sun_ref, 1.0, sky_ref, ref::dvec3(blocks[b].ground),
                       &near_light, 1, ref::Dvec3{});
        const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y),
                              ref::display(linear.z)};
        const u8* got = pixel(b, x, y);
        int here = 0;
        for (u32 c = 0; c < 3; ++c)
          here = std::max(here, std::abs(int{got[c]} - int{expect[c]}));
        below_worst = std::max(below_worst, here);
        if (here > 2) {
          CHECK_MESSAGE(here <= 2, "facing the ground, block "
                                       << b << " at " << x << "," << y << ": gpu " << int{got[0]}
                                       << "," << int{got[1]} << "," << int{got[2]} << " reference "
                                       << int{expect[0]} << "," << int{expect[1]} << ","
                                       << int{expect[2]});
        }
      }
    }
  }
  CHECK(below_covered > k_size * k_size / 3);
  worst = std::max(worst, below_worst);
  // The sand ground is warmer than the neutral one — red over blue — by what its albedo says.
  const u8* grey_ground = pixel(k_below, cx, cy);
  const u8* sand_ground = pixel(k_below + 1, cx, cy);
  CHECK(int{sand_ground[0]} - int{sand_ground[2]} > int{grey_ground[0]} - int{grey_ground[2]} + 10);
  // With no ground at all nothing lights a face turned to it: every covered pixel is black.
  u32 lit_without_ground = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (is_sky(pixel(k_below, x, y))) continue;
      const u8* p = pixel(k_below + 2, x, y);
      if (p[0] != 0 || p[1] != 0 || p[2] != 0) ++lit_without_ground;
    }
  }
  CHECK(lit_without_ground == 0u);
  MESSAGE("facing the ground: " << below_covered << " covered pixels, neutral ground gpu "
                                << int{grey_ground[0]} << "," << int{grey_ground[1]} << ","
                                << int{grey_ground[2]} << ", sand ground gpu "
                                << int{sand_ground[0]} << "," << int{sand_ground[1]} << ","
                                << int{sand_ground[2]} << ", worst " << below_worst
                                << " of 255; lit with no ground: " << lit_without_ground);
  // The one number to quote for a device: every pixel this case holds to the reference.
  MESSAGE("worst reference-vs-GPU difference over every compared pixel, "
          << std::string(raster.name()) << " path on " << std::string(device.adapter().name) << ": "
          << worst << " of 255 (tolerance 2)");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host_color, &params, &vis, &vis_below, &lights,
                                 &cluster_materials, &materials, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

TEST_CASE("material resolve: ray-traced shadows against the geometry the rasterizer drew") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device,
                         {gfx_test::Need::VisibilityBuffer, gfx_test::Need::AccelerationStructure,
                          gfx_test::Need::RayQuery})) {
    return;
  }

  // The lit quad on y = 0, rasterized and traced, and an occluder the acceleration structure
  // holds but no rasterizer ever draws: the picture is the quad alone with the occluder's shadow
  // across it. The quad is drawn through the vertex path, so this case needs a ray query and
  // nothing else.
  const Vec3 positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                             Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  geometry::ClusterMesh mesh;
  REQUIRE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error));
  REQUIRE(mesh.clusters.size() == 1);
  const geometry::ClusterDesc& cluster = mesh.clusters[0];
  const u32 triangles_per_cluster = geometry::ClusterBuildOptions{}.max_triangles;

  // A sun ray travels 4 / sun.y to the occluder's plane and moves 1.80 in z on the way, so an
  // occluder at z in [-0.2, 3.8] throws its shadow over the middle of the quad; the point light
  // 12 units up throws a wider one from the same rectangle.
  const Quad occluder{4.0, -2.0, 2.0, -0.2, 3.8};
  const Vec3 sun_dir = normalize(Vec3{0.0f, 1.0f, 0.45f});
  constexpr f32 k_bias = 0.01f;  // world units: 65 steps of this mesh's 16-bit position grid

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx::BufferResource lights;
  gfx::BufferResource as_vertices;
  gfx::BufferResource as_indices;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(), sizeof(geometry::ClusterDesc), k_storage,
                             clusters, &error));
  REQUIRE(scene.create(device, mesh, 1, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));

  gfx::ResolveMaterial material_table[1];
  material_table[0].albedo = Vec4{0.8f, 0.4f, 0.2f, 0.7f};  // a rough dielectric
  material_table[0].emissive = Vec4{};
  const u32 material_index[1] = {0};
  REQUIRE(gfx::upload_buffer(device, material_table, sizeof(material_table), k_storage, materials,
                             &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));
  gfx::ResolveLight light_table[1];
  light_table[0].position_radius = Vec4{0.0f, 12.0f, 1.8f, 40.0f};
  light_table[0].color_intensity = Vec4{1.0f, 0.9f, 0.8f, 100.0f};
  REQUIRE(gfx::upload_buffer(device, light_table, sizeof(light_table), k_storage, lights, &error));

  // The traced geometry: the quad exactly as the rasterizer draws it — the cluster's own vertices
  // and its own triangles, widened to 16-bit indices — and the occluder after it, so one
  // bottom-level structure holds two geometries.
  Vector<Vec3> as_position_data;
  for (const Vec3& v : mesh.vertices)
    as_position_data.push_back(v);
  const u32 occluder_vertex = as_position_data.size();
  const double corner_x[4] = {occluder.x0, occluder.x1, occluder.x1, occluder.x0};
  const double corner_z[4] = {occluder.z0, occluder.z0, occluder.z1, occluder.z1};
  for (u32 i = 0; i < 4; ++i) {
    as_position_data.push_back(Vec3{static_cast<f32>(corner_x[i]), static_cast<f32>(occluder.plane),
                                    static_cast<f32>(corner_z[i])});
  }
  Vector<u16> as_index_data;
  gfx::expand_packed_triangles(
      std::span<const u32>(mesh.triangles.data() + cluster.triangle_offset, cluster.triangle_count),
      as_index_data);
  const u32 occluder_index = as_index_data.size();
  const u16 occluder_triangles[6] = {0, 1, 2, 0, 2, 3};
  for (const u16 i : occluder_triangles)
    as_index_data.push_back(i);
  REQUIRE(gfx::upload_buffer(device, as_position_data.data(),
                             as_position_data.size() * sizeof(Vec3), gfx::k_build_input_usage,
                             as_vertices, &error));
  REQUIRE(gfx::upload_buffer(device, as_index_data.data(), as_index_data.size() * sizeof(u16),
                             gfx::k_build_input_usage, as_indices, &error));
  gfx::ClusterGeometry geometries[2];
  geometries[0].vertices = as_vertices.address + u64{cluster.vertex_offset} * sizeof(Vec3);
  geometries[0].vertex_count = cluster.vertex_count;
  geometries[0].indices = as_indices.address;
  geometries[0].triangle_count = cluster.triangle_count;
  geometries[1].vertices = as_vertices.address + u64{occluder_vertex} * sizeof(Vec3);
  geometries[1].vertex_count = 4;
  geometries[1].indices = as_indices.address + u64{occluder_index} * sizeof(u16);
  geometries[1].triangle_count = 2;

  // Two scenes: the quad with the occluder, and the quad alone. The second is what says the bias
  // keeps a flat surface from shadowing itself.
  gfx::AccelerationStructure blas_scene;
  gfx::AccelerationStructure blas_quad;
  gfx::AccelerationStructure tlas_scene;
  gfx::AccelerationStructure tlas_quad;
  const std::span<const gfx::ClusterGeometry> both(geometries, 2);
  const std::span<const gfx::ClusterGeometry> quad_only(geometries, 1);
  REQUIRE_MESSAGE(gfx::create_blas(device, both, gfx::k_build_fast_trace, blas_scene, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_blas(device, quad_only, gfx::k_build_fast_trace, blas_quad, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas_scene, &error), error);
  REQUIRE_MESSAGE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas_quad, &error), error);
  gfx::BufferResource scratch;
  gfx::BufferResource instances;
  u64 scratch_bytes = blas_scene.build_scratch_bytes;
  scratch_bytes = std::max(scratch_bytes, blas_quad.build_scratch_bytes);
  scratch_bytes = std::max(scratch_bytes, tlas_scene.build_scratch_bytes);
  scratch_bytes = std::max(scratch_bytes, tlas_quad.build_scratch_bytes);
  REQUIRE(gfx::create_scratch(device, scratch_bytes, scratch, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes * 2, gfx::k_build_input_usage,
                             true, instances, &error));
  gfx::TlasInstance instance_records[2];
  instance_records[0].blas = blas_scene.address;
  instance_records[1].blas = blas_quad.address;
  gfx::write_instances(std::span<const gfx::TlasInstance>(instance_records, 2), instances.mapped);
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        // One scratch buffer, so every build waits for the one before it.
        constexpr gfx::MemoryAccess k_build_rw = gfx::MemoryAccess::AccelerationStructureRead |
                                                 gfx::MemoryAccess::AccelerationStructureWrite;
        gfx::build_blas(cb, blas_scene, both, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild, k_build_rw);
        gfx::build_blas(cb, blas_quad, quad_only, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild, k_build_rw);
        gfx::build_tlas(cb, tlas_scene, instances.address, 1, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild, k_build_rw);
        gfx::build_tlas(cb, tlas_quad, instances.address + gfx::k_instance_record_bytes, 1,
                        gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::FragmentShader,
                                        gfx::MemoryAccess::AccelerationStructureRead);
      },
      &error));

  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  constexpr u32 k_shadow_blocks = 4;  // unshadowed, sun, light, and a scene with no occluder
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_shadow_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_shadow_blocks,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  REQUIRE(bindless.has_acceleration_structures());
  const u32 scene_slot = bindless.add_acceleration_structure(tlas_scene.handle);
  const u32 quad_slot = bindless.add_acceleration_structure(tlas_quad.handle);
  REQUIRE(scene_slot != gfx::BindlessSet::k_invalid_slot);
  REQUIRE(quad_slot != gfx::BindlessSet::k_invalid_slot);

  gfx::ShaderModuleHandle vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_rt_spirv,
                                shaders::k_visibility_resolve_rt_spirv_size, &error);
  REQUIRE(vertex_module.valid());
  REQUIRE(resolve_module.valid());
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  gfx::PipelineHandle vertex_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
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
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.visibility = vis.address;
  draw.width = k_size;
  draw.height = k_size;

  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
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
  base.cluster_materials = cluster_materials.address;
  base.lights = lights.address;
  base.light_count = 1;
  base.width = k_size;
  base.height = k_size;
  base.shadow_bias = k_bias;
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  blocks[0] = base;  // no shadow ray at all: the lit reference
  blocks[1] = base;  // the sun casts
  blocks[1].scene = scene_slot;
  blocks[1].shadow_flags = gfx::k_shadow_sun;
  blocks[2] = base;  // the point light casts
  blocks[2].scene = scene_slot;
  blocks[2].shadow_flags = gfx::k_shadow_lights;
  blocks[3] = base;  // both cast, against a scene holding the lit quad alone
  blocks[3].scene = quad_slot;
  blocks[3].shadow_flags = gfx::k_shadow_sun | gfx::k_shadow_lights;
  u64 block_address[k_shadow_blocks];
  for (u32 i = 0; i < k_shadow_blocks; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  gfx::RgImage targets[k_shadow_blocks];
  for (u32 i = 0; i < k_shadow_blocks; ++i) {
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
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Graphics, vertex_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(draw),
                          &draw);
        cb.draw(triangles_per_cluster * 3, 1, 0, 0);
      });
  for (u32 i = 0; i < k_shadow_blocks; ++i) {
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
        for (u32 i = 0; i < k_shadow_blocks; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < k_shadow_blocks; ++i) {
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
  auto is_sky = [](const u8* p) -> bool {
    return std::abs(int{p[0]} - 51) <= 1 && std::abs(int{p[1]} - 77) <= 1 &&
           std::abs(int{p[2]} - 102) <= 1;
  };
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
  const ref::Dvec3 light_position = ref::dvec3(light_table[0].position_radius);
  // A pixel within this much of the shadow's edge is partly covered, while the CPU decides the
  // whole pixel either way, so those are left out of the comparison rather than fudged. A pixel
  // is 11.5 / 128 = 0.09 world units wide here.
  constexpr double k_margin = 0.25;

  auto surface_at = [&](u32 x, u32 y) {
    ref::Surface s;
    s.position = ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                     static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
    s.normal = ref::Dvec3{0.0, 1.0, 0.0};
    s.view = ref::normalize(ref::dvec3(eye) - s.position);
    s.albedo = ref::dvec3(material_table[0].albedo);
    s.roughness = static_cast<double>(material_table[0].albedo.w);
    s.metallic = static_cast<double>(material_table[0].emissive.w);
    return s;
  };

  // The sun's shadow, then the point light's: every covered pixel whose shadow test is not a
  // close call is compared against the reference, told what the ray found.
  for (u32 pass = 0; pass < 2; ++pass) {
    const u32 image = pass == 0 ? 1u : 2u;
    const std::string caster = pass == 0 ? "sun" : "point light";
    u32 shadowed = 0;
    u32 lit = 0;
    u32 skipped = 0;
    u32 lit_differs = 0;
    int worst = 0;
    bool reported = false;
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const u8* got = pixel(image, x, y);
        if (is_sky(got)) continue;
        const ref::Surface s = surface_at(x, y);
        const ref::Dvec3 origin = s.position + ref::Dvec3{0.0, static_cast<double>(k_bias), 0.0};
        ref::Light light;
        light.position = light_position;
        light.radius = static_cast<double>(light_table[0].position_radius.w);
        light.color = ref::dvec3(light_table[0].color_intensity);
        light.intensity = static_cast<double>(light_table[0].color_intensity.w);
        double margin = 0.0;
        bool sun_shadowed = false;
        if (pass == 0) {
          margin = quad_margin(origin, sun_ref, occluder, 1.0e30);
          sun_shadowed = margin > 0.0;
        } else {
          const ref::Dvec3 to_light = light_position - s.position;
          const double distance = std::sqrt(ref::dot(to_light, to_light));
          margin = quad_margin(origin, ref::normalize(to_light), occluder,
                               distance - static_cast<double>(k_bias));
          light.shadowed = margin > 0.0;
        }
        if (std::fabs(margin) < k_margin) {
          ++skipped;
          continue;
        }
        (margin > 0.0 ? shadowed : lit) += 1;
        const ref::Dvec3 linear = ref::shade(s, sun_ref, 1.0, sky_ref, ref::dvec3(base.ground),
                                             &light, 1, ref::Dvec3{}, sun_shadowed);
        const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y),
                              ref::display(linear.z)};
        int here = 0;
        for (u32 c = 0; c < 3; ++c)
          here = std::max(here, std::abs(int{got[c]} - int{expect[c]}));
        worst = std::max(worst, here);
        if (here > 2 && !reported) {
          reported = true;
          CHECK_MESSAGE(here <= 2, caster << " shadow at " << x << "," << y
                                          << (margin > 0.0 ? " (shadowed)" : " (lit)") << ": gpu "
                                          << int{got[0]} << "," << int{got[1]} << "," << int{got[2]}
                                          << " reference " << int{expect[0]} << ","
                                          << int{expect[1]} << "," << int{expect[2]});
        }
        // A pixel the shadow ray misses must be the unshadowed picture, byte for byte.
        if (margin < 0.0 && std::memcmp(got, pixel(0, x, y), 4) != 0) ++lit_differs;
      }
    }
    CHECK(shadowed > 500);
    CHECK(lit > 500);
    CHECK(worst <= 2);
    CHECK(lit_differs == 0);
    MESSAGE(caster << " shadow: " << shadowed << " shadowed and " << lit
                   << " lit pixels within 2 of the reference (worst " << worst << " of 255), "
                   << skipped << " edge pixels skipped");
  }

  // No acne: the same two shadow rays against a scene holding only the lit quad leave the picture
  // byte-identical to the one traced with no shadow ray at all. The bias is what does it.
  u32 differing = 0;
  for (u32 y = 0; y < k_size; ++y) {
    for (u32 x = 0; x < k_size; ++x) {
      if (std::memcmp(pixel(3, x, y), pixel(0, x, y), 4) != 0) ++differing;
    }
  }
  CHECK(differing == 0);
  MESSAGE("self-shadowing on the unoccluded quad at a bias of "
          << static_cast<double>(k_bias) << ": " << differing << " differing pixels");

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_shader_module(device, resolve_module);
  gfx::destroy_shader_module(device, vertex_module);
  bindless.destroy();
  scene.destroy(device);
  gfx::destroy_acceleration_structure(device, tlas_quad);
  gfx::destroy_acceleration_structure(device, tlas_scene);
  gfx::destroy_acceleration_structure(device, blas_quad);
  gfx::destroy_acceleration_structure(device, blas_scene);
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &instances, &scratch, &as_indices, &as_vertices, &lights,
        &cluster_materials, &materials, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

TEST_CASE("material resolve: cascaded shadow maps against the geometry the rasterizer drew") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // The receiver, the lit quad on y = 0, is the only thing in the picture. The occluder is the
  // ray-traced case's rectangle, drawn into the sun's depth map and nowhere else. They are two
  // meshes, each on its own 16-bit grid, as two scenes of one instance each.
  const Vec3 quad_positions[4] = {Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, -5.0f},
                                  Vec3{5.0f, 0.0f, 5.0f}, Vec3{-5.0f, 0.0f, 5.0f}};
  const Quad occluder{4.0, -2.0, 2.0, -0.2, 3.8};
  const f32 ox0 = static_cast<f32>(occluder.x0);
  const f32 ox1 = static_cast<f32>(occluder.x1);
  const f32 oz0 = static_cast<f32>(occluder.z0);
  const f32 oz1 = static_cast<f32>(occluder.z1);
  const f32 oy = static_cast<f32>(occluder.plane);
  const Vec3 occluder_positions[4] = {Vec3{ox0, oy, oz0}, Vec3{ox1, oy, oz0}, Vec3{ox1, oy, oz1},
                                      Vec3{ox0, oy, oz1}};
  const u32 indices[6] = {0, 2, 1, 0, 3, 2};
  geometry::ClusterMesh meshes[2];
  REQUIRE(geometry::build_clusters(quad_positions, indices, geometry::ClusterBuildOptions{},
                                   meshes[0], &error));
  REQUIRE(geometry::build_clusters(occluder_positions, indices, geometry::ClusterBuildOptions{},
                                   meshes[1], &error));
  REQUIRE(meshes[0].clusters.size() == 1);
  REQUIRE(meshes[1].clusters.size() == 1);
  const u32 triangles_per_cluster = geometry::ClusterBuildOptions{}.max_triangles;

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters[2];
  gfx::BufferResource triangles[2];
  gfx_test::SingleInstance scenes[2];
  for (u32 m = 0; m < 2; ++m) {
    REQUIRE(gfx::upload_buffer(device, meshes[m].clusters.data(), sizeof(geometry::ClusterDesc),
                               k_storage, clusters[m], &error));
    REQUIRE(scenes[m].create(device, meshes[m], 1, &error));
    REQUIRE(gfx::upload_buffer(device, meshes[m].triangles.data(),
                               meshes[m].triangles.size() * sizeof(u32), k_storage, triangles[m],
                               &error));
  }
  gfx::BufferResource materials;
  gfx::BufferResource cluster_materials;
  gfx::BufferResource lights;
  gfx::ResolveMaterial material_table[1];
  material_table[0].albedo = Vec4{0.8f, 0.4f, 0.2f, 0.7f};  // a rough dielectric
  material_table[0].emissive = Vec4{};
  const u32 material_index[1] = {0};
  REQUIRE(gfx::upload_buffer(device, material_table, sizeof(material_table), k_storage, materials,
                             &error));
  REQUIRE(gfx::upload_buffer(device, material_index, sizeof(material_index), k_storage,
                             cluster_materials, &error));
  gfx::ResolveLight light_table[1];
  light_table[0].position_radius = Vec4{0.0f, 12.0f, 1.8f, 40.0f};
  light_table[0].color_intensity = Vec4{1.0f, 0.9f, 0.8f, 100.0f};
  REQUIRE(gfx::upload_buffer(device, light_table, sizeof(light_table), k_storage, lights, &error));

  // Two suns: the ray-traced case's, 24 degrees off the quad's normal, and a grazing one at 75
  // degrees, where a depth slope of 3.7 world units along the light per unit across it is what the
  // receiver-plane bias has to hold without the flat quad shadowing itself.
  const Vec3 sun_dir = normalize(Vec3{0.0f, 1.0f, 0.45f});
  const Vec3 grazing_dir =
      normalize(Vec3{0.0f, std::cos(radians(75.0f)), std::sin(radians(75.0f))});

  // One cascade per map, fit the way the renderer fits one: the receivers' bounding sphere (the
  // quad's, radius 5 sqrt 2), its centre snapped to the texel grid, and every caster within ten
  // units towards the light. 512 texels over 14.2 units is 2.8 cm a texel, against a 9 cm pixel.
  constexpr u32 k_map = 512;
  constexpr f32 k_radius = 7.1f;
  constexpr f32 k_bias_texels = 0.5f;  // the renderer's default (renderer::k_shadow_bias_texels)
  constexpr f32 k_max_slope = 8.0f;
  constexpr f32 k_normal_offset = 3.0f;  // renderer::k_shadow_normal_offset_texels
  const gfx::ShadowLight suns[2] = {gfx::shadow_light(sun_dir), gfx::shadow_light(grazing_dir)};
  gfx::ShadowCascade cascades[2];
  for (u32 s = 0; s < 2; ++s) {
    const Vec3 center = gfx::shadow_snap(suns[s], Vec3{}, 2.0f * k_radius / k_map);
    cascades[s] = gfx::make_shadow_cascade(suns[s], center, k_radius, 10.0f, k_map, k_bias_texels);
  }

  // Three depth maps: the 24-degree sun over the quad and the occluder, the same sun over the quad
  // alone, and the grazing sun over the quad alone.
  constexpr u32 k_maps = 3;
  const u32 map_sun[k_maps] = {0, 0, 1};
  const bool map_occluder[k_maps] = {true, false, false};
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ImageResource atlas[k_maps];
  gfx::ImageViewHandle atlas_view[k_maps] = {};
  u32 atlas_slot[k_maps] = {};
  for (u32 a = 0; a < k_maps; ++a) {
    REQUIRE_MESSAGE(
        gfx::create_image_2d(device, k_map, k_map, gfx::Format::D32Sfloat,
                             gfx::ImageUsage::DepthStencilAttachment | gfx::ImageUsage::Sampled,
                             atlas[a], &error),
        error);
    REQUIRE_MESSAGE(gfx::create_image_view(device, atlas[a], atlas_view[a], &error), error);
    atlas_slot[a] = bindless.add_sampled_image(atlas_view[a], gfx::ImageLayout::ShaderReadOnly);
    REQUIRE(atlas_slot[a] != gfx::BindlessSet::k_invalid_slot);
  }
  gfx::SamplerHandle point_sampler = {};
  REQUIRE(gfx::create_sampler(device, gfx::Filter::Nearest, point_sampler, &error));
  const u32 sampler_slot = bindless.add_sampler(point_sampler);
  REQUIRE(sampler_slot != gfx::BindlessSet::k_invalid_slot);

  gfx::BufferResource map_params;
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ShadowMapParams) * k_maps,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, map_params,
                             &error));
  auto* maps = static_cast<gfx::ShadowMapParams*>(map_params.mapped);
  u64 map_address[k_maps];
  for (u32 a = 0; a < k_maps; ++a) {
    const gfx::ShadowLight& l = suns[map_sun[a]];
    maps[a] = gfx::ShadowMapParams{};
    maps[a].cascades[0] = cascades[map_sun[a]];
    maps[a].light_right = Vec4{l.right, 0.0f};
    maps[a].light_up = Vec4{l.up, 0.0f};
    maps[a].light_dir = Vec4{l.direction, 0.0f};
    maps[a].cascade_count = 1;
    maps[a].tiles = 1;
    maps[a].resolution = k_map;
    maps[a].texture = atlas_slot[a];
    maps[a].sampler = sampler_slot;
    maps[a].max_slope = k_max_slope;
    maps[a].normal_offset = k_normal_offset;
    map_address[a] = map_params.address + a * sizeof(gfx::ShadowMapParams);
  }

  const Vec3 eye{0.0f, 10.0f, 0.0f};
  const Vec3 target{};
  const Vec3 up{0.0f, 0.0f, -1.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.1f) * look_at(eye, target, up);
  const Vec4 sky{0.2f, 0.3f, 0.4f, 1.0f};
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  gfx::ResolveParams base{};
  base.sky = sky;
  base.sun = Vec4{sun_dir, 1.0f};
  base.camera = Vec4{eye, 0.0f};
  base.view_proj = view_proj;
  base.visibility = vis.address;
  base.clusters = clusters[0].address;
  base.mesh = scenes[0].meshes.address;
  base.instances = scenes[0].instances.address;
  base.triangles = triangles[0].address;
  base.materials = materials.address;
  base.cluster_materials = cluster_materials.address;
  base.lights = lights.address;
  base.light_count = 1;
  base.width = k_size;
  base.height = k_size;
  // The blocks: 0 unshadowed; 1 the sun through the map with the occluder; 2 the same with the
  // point light's bit set too, which the maps do not answer; 3 the sun through the map of the
  // quad alone; 4 and 5 the grazing sun unshadowed and through its map of the quad alone.
  constexpr u32 k_map_blocks = 6;
  constexpr u32 k_cascaded = gfx::k_shadow_sun | gfx::k_shadow_cascades;
  gfx::ResolveParams blocks[k_map_blocks];
  blocks[0] = base;
  blocks[1] = base;
  blocks[1].shadow_flags = k_cascaded;
  blocks[1].shadow_maps = map_address[0];
  blocks[2] = blocks[1];
  blocks[2].shadow_flags = k_cascaded | gfx::k_shadow_lights;
  blocks[3] = base;
  blocks[3].shadow_flags = k_cascaded | gfx::k_shadow_lights;
  blocks[3].shadow_maps = map_address[1];
  blocks[4] = base;
  blocks[4].sun = Vec4{grazing_dir, 1.0f};
  blocks[5] = blocks[4];
  blocks[5].shadow_flags = k_cascaded;
  blocks[5].shadow_maps = map_address[2];

  gfx::BufferResource params;
  gfx::BufferResource host_color;
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * k_map_blocks,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * k_map_blocks,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));
  u64 block_address[k_map_blocks];
  for (u32 i = 0; i < k_map_blocks; ++i) {
    static_cast<gfx::ResolveParams*>(params.mapped)[i] = blocks[i];
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);
  }

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(resolve_module.valid());
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
  gfx_test::ClusterRaster picture;  // the quad into the visibility buffer
  REQUIRE_MESSAGE(picture.create(device, bindless.pipeline_layout(), triangles_per_cluster, &error),
                  error);

  auto draw_of = [&](u32 m, const Mat4& vp, u64 visibility, u32 extent) {
    gfx::ClusterDrawParams d{};
    d.view_proj = vp;
    d.clusters = clusters[m].address;
    d.mesh = scenes[m].meshes.address;
    d.instances = scenes[m].instances.address;
    d.triangles = triangles[m].address;
    d.visibility = visibility;
    d.width = extent;
    d.height = extent;
    return d;
  };
  const gfx::ClusterDrawParams quad_draw = draw_of(0, view_proj, vis.address, k_size);

  auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  auto is_sky = [](const u8* p) -> bool {
    return std::abs(int{p[0]} - 51) <= 1 && std::abs(int{p[1]} - 77) <= 1 &&
           std::abs(int{p[2]} - 102) <= 1;
  };
  auto differing = [&](u32 a, u32 b) {
    u32 n = 0;
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x)
        n += std::memcmp(pixel(a, x, y), pixel(b, x, y), 4) != 0 ? 1u : 0u;
    }
    return n;
  };

  // The maps are drawn by every path the device has, and every path must give the same shadow.
  Vector<u8> first_path;
  const gfx_test::RasterPath paths[2] = {gfx_test::RasterPath::Mesh, gfx_test::RasterPath::Vertex};
  for (const gfx_test::RasterPath path : paths) {
    if (path == gfx_test::RasterPath::Mesh &&
        !gfx_test::part(device, "the maps drawn through the mesh path",
                        {gfx_test::Need::MeshShader})) {
      continue;
    }
    gfx_test::ClusterRaster depth;
    REQUIRE_MESSAGE(depth.create(device, path, bindless.pipeline_layout(), triangles_per_cluster,
                                 &error, gfx::Format::D32Sfloat),
                    error);
    gfx::RenderGraph graph(device);
    const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
    const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
    gfx::RgImage rg_atlas[k_maps];
    for (u32 a = 0; a < k_maps; ++a)
      rg_atlas[a] = graph.import_image("shadow map", atlas[a]);
    gfx::RgImage targets[k_map_blocks];
    for (u32 i = 0; i < k_map_blocks; ++i) {
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
        [&](gfx::CommandList cb, gfx::RenderGraph&) { picture.draw(cb, bindless, quad_draw, 1); });
    for (u32 a = 0; a < k_maps; ++a) {
      graph.add_pass(
          "shadow map", gfx::PassKind::Raster,
          [&, a](gfx::PassBuilder& b) {
            b.depth_attachment(rg_atlas[a], gfx::LoadOp::Clear, 0.0f);  // far is 0
          },
          [&, a](gfx::CommandList cb, gfx::RenderGraph&) {
            const Mat4& light_vp = cascades[map_sun[a]].view_proj;
            depth.draw(cb, bindless, draw_of(0, light_vp, 0, k_map), 1);
            if (map_occluder[a]) depth.draw(cb, bindless, draw_of(1, light_vp, 0, k_map), 1);
          });
    }
    for (u32 i = 0; i < k_map_blocks; ++i) {
      graph.add_pass(
          "resolve", gfx::PassKind::Raster,
          [&, i](gfx::PassBuilder& b) {
            gfx::ClearColor clear{};
            b.color_attachment(targets[i], gfx::LoadOp::Clear, clear);
            b.read(rg_vis, gfx::Access::FragmentRead);
            for (u32 a = 0; a < k_maps; ++a)
              b.read(rg_atlas[a], gfx::Access::SampledRead);
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
          for (u32 i = 0; i < k_map_blocks; ++i)
            b.read(targets[i], gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph& g) {
          for (u32 i = 0; i < k_map_blocks; ++i) {
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
    graph.reset();
    depth.destroy(device);

    // **The tolerance.** The filter's footprint is the 4 x 4 texels around the point, so a point
    // whose sun ray passes within about three texels of the occluder's edge (the farthest
    // footprint centre is two and a half texels away along an axis, 2.9 on the diagonal) may be
    // partly lit by the map where the CPU calls it wholly one or the other. A texel is
    // `texel_world` across the light, and on the occluder's horizontal plane a distance across the
    // light is at most 1 / cos(24 degrees) longer, so the band left out around the edge is the
    // ray-traced case's quarter of a unit (a whole 9 cm pixel's worth of CPU-against-GPU edge)
    // plus three texels over that cosine: 0.25 + 0.09 = 0.34 world units, 3.8 pixels either side.
    // The normal offset lifts the lookup `k_normal_offset * (1 - N.L)` texels off the quad, 0.26
    // of one at 24 degrees, whose ray meets the occluder's plane that height times tan(24) away:
    // a third of a centimetre more.
    const double texel = static_cast<double>(cascades[0].texel_world);
    const double cos_sun = static_cast<double>(sun_dir.y);
    const double lift = static_cast<double>(k_normal_offset) * (1.0 - cos_sun) * texel;
    const double band =
        0.25 + 3.0 * texel / cos_sun + lift * static_cast<double>(sun_dir.z) / cos_sun;
    const ref::Dvec3 sky_ref = ref::dvec3(sky);
    const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
    u32 shadowed = 0;
    u32 lit = 0;
    u32 skipped = 0;
    u32 penumbra = 0;      // band pixels the map left neither the lit nor the shadowed picture
    u32 band_differs = 0;  // band pixels that are not what the hard edge says
    u32 lit_differs = 0;
    int worst = 0;
    bool reported = false;
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const u8* got = pixel(1, x, y);
        if (is_sky(got)) continue;
        ref::Surface s;
        s.position =
            ref::pixel_on_plane(ref::dvec3(eye), ref::dvec3(target), ref::dvec3(up),
                                static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
        s.normal = ref::Dvec3{0.0, 1.0, 0.0};
        s.view = ref::normalize(ref::dvec3(eye) - s.position);
        s.albedo = ref::dvec3(material_table[0].albedo);
        s.roughness = static_cast<double>(material_table[0].albedo.w);
        s.metallic = static_cast<double>(material_table[0].emissive.w);
        ref::Light light;
        light.position = ref::dvec3(light_table[0].position_radius);
        light.radius = static_cast<double>(light_table[0].position_radius.w);
        light.color = ref::dvec3(light_table[0].color_intensity);
        light.intensity = static_cast<double>(light_table[0].color_intensity.w);
        const double margin = quad_margin(s.position, sun_ref, occluder, 1.0e30);
        const bool sun_shadowed = margin > 0.0;
        // The ground's bounce of the sun stays on a shadowed pixel: it is the ground around it.
        const ref::Dvec3 linear = ref::shade(s, sun_ref, 1.0, sky_ref, ref::dvec3(base.ground),
                                             &light, 1, ref::Dvec3{}, sun_shadowed);
        const u8 expect[3] = {ref::display(linear.x), ref::display(linear.y),
                              ref::display(linear.z)};
        int here = 0;
        for (u32 c = 0; c < 3; ++c)
          here = std::max(here, std::abs(int{got[c]} - int{expect[c]}));
        const bool as_lit = std::memcmp(got, pixel(0, x, y), 4) == 0;
        if (std::fabs(margin) < band) {
          ++skipped;
          const bool as_shadowed = !as_lit && sun_shadowed && here <= 2;
          if (!as_lit && !as_shadowed) ++penumbra;
          if (sun_shadowed ? !as_shadowed : !as_lit) ++band_differs;
          continue;
        }
        (sun_shadowed ? shadowed : lit) += 1;
        worst = std::max(worst, here);
        if (here > 2 && !reported) {
          reported = true;
          CHECK_MESSAGE(here <= 2, std::string(gfx_test::raster_path_name(path))
                                       << " map, sun shadow at " << x << "," << y
                                       << (sun_shadowed ? " (shadowed)" : " (lit)") << ": gpu "
                                       << int{got[0]} << "," << int{got[1]} << "," << int{got[2]}
                                       << " reference " << int{expect[0]} << "," << int{expect[1]}
                                       << "," << int{expect[2]});
        }
        // A pixel the map leaves lit must be the unshadowed picture, byte for byte.
        if (!sun_shadowed && !as_lit) ++lit_differs;
      }
    }
    CHECK(shadowed > 500);
    CHECK(lit > 500);
    CHECK(worst <= 2);
    CHECK(lit_differs == 0);
    MESSAGE(std::string(gfx_test::raster_path_name(path))
            << " path's map, 3x3 bilinear PCF at " << k_map << " texels ("
            << static_cast<f64>(cascades[0].texel_world) * 100.0 << " cm a texel): " << shadowed
            << " shadowed and " << lit << " lit pixels within 2 of the reference (worst " << worst
            << " of 255); " << skipped << " pixels in the " << band
            << "-unit band around the edge, of which " << penumbra << " are penumbra and "
            << band_differs << " differ from the hard edge");

    // The maps shadow the sun alone: the point light's bit changes nothing.
    const u32 lights_differ = differing(2, 1);
    CHECK(lights_differ == 0);
    // No acne: the quad alone in the map leaves the picture byte-identical to the unshadowed one,
    // at the case's sun and at a grazing one.
    const u32 acne = differing(3, 0);
    const u32 grazing_acne = differing(5, 4);
    CHECK(acne == 0);
    CHECK(grazing_acne == 0);
    MESSAGE(std::string(gfx_test::raster_path_name(path))
            << " path's map: " << acne << " pixels self-shadowed at 24 degrees, " << grazing_acne
            << " at 75 degrees (the receiver's plane plus " << k_bias_texels << " texel of bias)");

    // Both paths rasterize the same triangles from the same grid into the map, so the shadow is
    // the same bytes whichever drew it.
    const u8* shadow_image = pixel(1, 0, 0);
    const u64 image_bytes = u64{k_size} * k_size * 4;
    if (first_path.empty()) {
      first_path.resize(static_cast<u32>(image_bytes));
      std::memcpy(first_path.data(), shadow_image, image_bytes);
    } else {
      u32 path_differs = 0;
      for (u64 i = 0; i < image_bytes; i += 4)
        path_differs += std::memcmp(shadow_image + i, first_path.data() + i, 4) != 0 ? 1u : 0u;
      CHECK(path_differs == 0);
      MESSAGE("the mesh and vertex paths' maps: " << path_differs
                                                  << " pixels of the shadow differ");
    }
  }

  gfx::destroy_pipeline(device, resolve_pipeline);
  picture.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  gfx::destroy_sampler(device, point_sampler);
  for (u32 a = 0; a < k_maps; ++a) {
    gfx::destroy_image_view(device, atlas_view[a]);
    gfx::destroy_image(device, atlas[a]);
  }
  for (u32 m = 0; m < 2; ++m) {
    scenes[m].destroy(device);
    gfx::destroy_buffer(device, clusters[m]);
    gfx::destroy_buffer(device, triangles[m]);
  }
  for (gfx::BufferResource* b :
       {&host_color, &params, &vis, &map_params, &lights, &cluster_materials, &materials}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
