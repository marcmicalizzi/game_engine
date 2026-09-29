// The ground's detail on the GPU against its CPU mirror (ground_detail.slang against
// ground_detail_reference.h, docs/subsystems/gfx.md "The ground's detail"): a horizontal quad of
// sand with `k_material_ground_detail`, seen from a walker's eye height looking along it and
// looking down at the feet, at two places — by the origin, and 3.7 km out, where a float has a
// quarter of a millimetre under the point and the ripples a phase error to show for it — each drawn
// shaded and in the detail view. Every covered pixel is held to the reference: the point the pixel
// sees on the plane, its footprint by central differences, the detail by `ground_ref::shade`, and
// the shading by `brdf_ref::shade`. So the function the resolve draws is the function the CPU
// computes, the fades are the same functions of the same footprint, and the tolerance says by how
// much float and double disagree about all of it.
//
// Also, without a device: the block the scene's numbers make, and the rule the filter moves
// variance by — that the slope variance it carries is the pattern's own, measured over the CPU
// function — and that the function is continuous (it has no seams of its own: no lattice edge, no
// kernel edge, no wrap of the phase shows up as a jump between neighbouring points).
#include "brdf_reference.h"
#include "ground_detail_reference.h"
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/ground_detail.h>
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
namespace gref = engine::ground_ref;

namespace {

gfx::GroundDetailParams test_block() {
  gfx::GroundDetailDesc desc;  // the defaults a scene's `detail` block starts from
  return gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
}

}  // namespace

TEST_CASE("ground detail: the block is the scene's numbers, and the pattern is continuous") {
  const gfx::GroundDetailParams d = test_block();
  CHECK(d.flags == (gfx::k_ground_ripples | gfx::k_ground_grain));
  CHECK(std::abs(d.wind.x - 0.6f) < 1e-6f);
  CHECK(std::abs(d.wind.y + 0.8f) < 1e-6f);
  CHECK(d.amplitude == doctest::Approx(0.004f));
  CHECK(d.cell == doctest::Approx(0.12f * (6.0f - 4.5f * 0.35f)));
  // No ripples at a height of nothing, no grain at a strength of nothing.
  gfx::GroundDetailDesc none;
  none.ripple_height = 0.0f;
  none.grain_albedo = 0.0f;
  none.grain_roughness = 0.0f;
  CHECK(gfx::ground_detail_block(none, Vec2{1.0f, 0.0f}, 1u).flags == 0u);
  // A zero wind is +x rather than a NaN.
  const gfx::GroundDetailParams still =
      gfx::ground_detail_block(gfx::GroundDetailDesc{}, Vec2{}, 1u);
  CHECK(still.wind.x == 1.0f);
  CHECK(still.wind.y == 0.0f);

  // Continuity: along lines crossing many lattice cells, kernel edges and phase wraps, the height
  // never jumps by more than its steepest honest slope allows over the step, and the numeric
  // derivative agrees with the analytic gradient wherever the pattern is not at a defect's core —
  // at the scene's asymmetry and at the plain sinusoid the filter eases it to.
  constexpr double k_step = 0.0005;  // half a millimetre
  u32 samples = 0;
  u32 gradient_checked = 0;
  u32 gradient_off = 0;
  double largest_jump = 0.0;
  double largest_slope = 0.0;
  for (u32 line = 0; line < 6; ++line) {
    const double a = line < 4 ? static_cast<double>(d.asymmetry) : 0.5;
    const double z = 0.37 + 1.13 * line;
    const double x0 = -3.0 + 0.71 * line;
    gref::Ripple previous = gref::ripple(d, x0, z, a);
    for (u32 i = 1; i <= 12000; ++i) {
      const double x = x0 + k_step * i;
      const gref::Ripple r = gref::ripple(d, x, z, a);
      const double slope = std::max(std::fabs(r.gx), std::fabs(previous.gx));
      largest_slope = std::max(largest_slope, slope);
      const double jump = std::fabs(r.height - previous.height);
      largest_jump = std::max(largest_jump, jump);
      // A step's change is its mean slope over it; twice the larger end's slope plus a
      // micrometre covers the curvature within half a millimetre.
      CHECK_MESSAGE(jump <= 2.0 * slope * k_step + 1e-6,
                    "the height jumps " << jump << " m in " << k_step << " m at x " << x);
      if (slope < 0.5) {
        ++gradient_checked;
        const double numeric = (r.height - previous.height) / k_step;
        const double analytic = 0.5 * (r.gx + previous.gx);
        if (std::fabs(numeric - analytic) > 0.02 + 0.02 * std::fabs(analytic)) ++gradient_off;
      }
      previous = r;
      ++samples;
    }
  }
  CHECK(gradient_off * 1000 <= gradient_checked);
  MESSAGE("continuity over " << samples << " steps of 0.5 mm: largest jump " << largest_jump
                             << " m, largest slope " << largest_slope << ", analytic gradient off "
                             << gradient_off << " of " << gradient_checked);

  // The filter's transfer: the slope variance the block says the ripples have is the pattern's
  // own, E|grad h|^2 over a patch of many kernels — which is what `k_ground_slope_share` was
  // measured to be — at the scene's asymmetry, at the sinusoid the filter eases it to, and with
  // more defects and fewer.
  for (u32 variant = 0; variant < 4; ++variant) {
    gfx::GroundDetailDesc desc;
    if (variant == 2) desc.ripple_defects = 0.0f;
    if (variant == 3) desc.ripple_defects = 1.0f;
    const gfx::GroundDetailParams v = gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
    const f32 a = variant == 1 ? 0.5f : v.asymmetry;
    double sum = 0.0;
    u32 count = 0;
    for (u32 j = 0; j < 400; ++j) {
      for (u32 i = 0; i < 400; ++i) {
        const gref::Ripple r =
            gref::ripple(v, 0.0103 * i - 2.1, 0.0107 * j + 5.3, static_cast<double>(a));
        sum += r.gx * r.gx + r.gz * r.gz;
        ++count;
      }
    }
    const double measured = sum / count;
    const double block = static_cast<double>(gfx::ground_slope_variance(v, a));
    const double ratio = measured / block;
    MESSAGE("slope variance at asymmetry " << a << ", defects " << desc.ripple_defects
                                           << ": measured " << measured << ", block " << block
                                           << " (ratio " << ratio << ")");
    CHECK(ratio > 0.8);
    CHECK(ratio < 1.2);
  }
}

TEST_CASE("ground detail: the resolve draws the CPU's function of position") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // Two places and two looks at each: a walker's eyes 1.6 m over the sand looking along it, and
  // looking down at the feet. Each is drawn shaded and in the detail view.
  constexpr u32 k_size = 160;
  constexpr u32 k_views = 4;
  struct Site {
    f32 x, z;
  };
  const Site sites[2] = {{0.0f, 0.0f}, {2917.37f, -2403.71f}};
  struct Look {
    Vec3 eye, target;
  };
  auto look = [&](u32 v) {
    const Site s = sites[v / 2];
    if (v % 2 == 0) return Look{Vec3{s.x, 1.6f, s.z + 6.0f}, Vec3{s.x + 2.0f, 0.0f, s.z - 20.0f}};
    return Look{Vec3{s.x, 1.6f, s.z + 0.4f}, Vec3{s.x + 0.1f, 0.0f, s.z - 0.1f}};
  };
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const f32 fov_y = radians(60.0f);

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  const gfx::GroundDetailParams detail = test_block();
  gfx::BufferResource detail_buffer;
  REQUIRE(gfx::upload_buffer(device, &detail, sizeof(detail), k_storage, detail_buffer, &error));

  gfx::ResolveMaterial material;
  material.albedo = Vec4{0.84f, 0.69f, 0.47f, 0.92f};
  material.flags = gfx::k_material_ground_detail;
  gfx::BufferResource materials;
  REQUIRE(gfx::upload_buffer(device, &material, sizeof(material), k_storage, materials, &error));

  // Sixty metres of sand round each site at y = 0, in cells of a metre. Not one quad: the resolve
  // rebuilds a pixel's point from its triangle's corners projected to the screen, which is exact
  // for a triangle in front of the camera and meaningless for one reaching behind it, and a quad
  // under a walker's feet reaches behind the walker.
  constexpr u32 k_cells = 60;
  geometry::ClusterMesh meshes[2];
  gfx_test::SingleInstance scenes[2];
  gfx::BufferResource clusters[2];
  gfx::BufferResource triangles[2];
  gfx::BufferResource cluster_materials[2];
  u32 cluster_count[2] = {};
  for (u32 s = 0; s < 2; ++s) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    for (u32 j = 0; j <= k_cells; ++j) {
      for (u32 i = 0; i <= k_cells; ++i) {
        positions.push_back(Vec3{sites[s].x - 30.0f + static_cast<f32>(i), 0.0f,
                                 sites[s].z - 30.0f + static_cast<f32>(j)});
      }
    }
    for (u32 j = 0; j < k_cells; ++j) {
      for (u32 i = 0; i < k_cells; ++i) {
        const u32 v00 = j * (k_cells + 1) + i;
        const u32 v10 = v00 + 1;
        const u32 v01 = v00 + k_cells + 1;
        const u32 v11 = v01 + 1;
        for (u32 index : {v00, v11, v10, v00, v01, v11})
          indices.push_back(index);
      }
    }
    REQUIRE(geometry::build_clusters(std::span<const Vec3>(positions.data(), positions.size()),
                                     std::span<const u32>(indices.data(), indices.size()),
                                     geometry::ClusterBuildOptions{}, meshes[s], &error));
    cluster_count[s] = meshes[s].clusters.size();
    const Vector<u32> zeros(cluster_count[s], 0u);
    REQUIRE(gfx::upload_buffer(device, meshes[s].clusters.data(),
                               cluster_count[s] * sizeof(geometry::ClusterDesc), k_storage,
                               clusters[s], &error));
    REQUIRE(gfx::upload_buffer(device, zeros.data(), cluster_count[s] * sizeof(u32), k_storage,
                               cluster_materials[s], &error));
    REQUIRE(scenes[s].create(device, meshes[s], cluster_count[s], &error));
    REQUIRE(gfx::upload_buffer(device, meshes[s].triangles.data(),
                               meshes[s].triangles.size() * sizeof(u32), k_storage, triangles[s],
                               &error));
  }

  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis[k_views];
  for (u32 v = 0; v < k_views; ++v) {
    REQUIRE(gfx::create_buffer(
        device, vis_bytes,
        k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false,
        vis[v], &error));
  }
  constexpr u32 k_blocks = k_views * 2;  // shaded, and the detail view
  gfx::BufferResource params;
  gfx::BufferResource host_color;
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

  const Vec4 sky{0.45f, 0.62f, 0.80f, 1.0f};
  // 21 degrees up, from down the wind: across the crests, where the ripples show.
  const Vec3 sun_dir = normalize(Vec3{0.6f, 0.38f, -0.8f});
  const Vec4 ground{0.84f, 0.69f, 0.47f, 0.0f};
  Mat4 view_proj[k_views];
  gfx::ClusterDrawParams draws[k_views];
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  for (u32 v = 0; v < k_views; ++v) {
    const Look l = look(v);
    const u32 s = v / 2;
    view_proj[v] = perspective_reversed_z(fov_y, 1.0f, 0.05f) * look_at(l.eye, l.target, up);
    gfx::ClusterDrawParams& draw = draws[v];
    draw = gfx::ClusterDrawParams{};
    draw.view_proj = view_proj[v];
    draw.clusters = clusters[s].address;
    draw.mesh = scenes[s].meshes.address;
    draw.instances = scenes[s].instances.address;
    draw.triangles = triangles[s].address;
    draw.visibility = vis[v].address;
    draw.width = k_size;
    draw.height = k_size;
    for (u32 mode = 0; mode < 2; ++mode) {
      gfx::ResolveParams& b = blocks[v * 2 + mode];
      b = gfx::ResolveParams{};
      b.sky = sky;
      b.sun = Vec4{sun_dir, 1.0f};
      b.ground = ground;
      b.camera = Vec4{l.eye, 0.0f};
      b.view_proj = view_proj[v];
      b.visibility = vis[v].address;
      b.clusters = clusters[s].address;
      b.mesh = scenes[s].meshes.address;
      b.instances = scenes[s].instances.address;
      b.triangles = triangles[s].address;
      b.materials = materials.address;
      b.cluster_materials = cluster_materials[s].address;
      b.width = k_size;
      b.height = k_size;
      b.ground_detail = detail_buffer.address;
      b.mode =
          static_cast<u32>(mode == 0 ? gfx::ResolveMode::Shaded : gfx::ResolveMode::GroundDetail);
    }
  }
  u64 block_address[k_blocks];
  for (u32 i = 0; i < k_blocks; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[k_views];
  for (u32 v = 0; v < k_views; ++v)
    rg_vis[v] = graph.import_buffer("vis", vis[v]);
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
        for (u32 v = 0; v < k_views; ++v)
          b.write(rg_vis[v], gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        for (u32 v = 0; v < k_views; ++v)
          cb.fill_buffer(vis[v].buffer, 0, gfx::k_whole_size, 0);
      });
  for (u32 v = 0; v < k_views; ++v) {
    graph.add_pass(
        "visibility", gfx::PassKind::Raster,
        [&, v](gfx::PassBuilder& b) {
          b.render_area(k_size, k_size);
          b.write(rg_vis[v], gfx::Access::FragmentReadWrite);
        },
        [&, v](gfx::CommandList cb, gfx::RenderGraph&) {
          raster.draw(cb, bindless, draws[v], cluster_count[v / 2]);
        });
  }
  for (u32 i = 0; i < k_blocks; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          b.color_attachment(targets[i], gfx::LoadOp::Clear, gfx::ClearColor{});
          b.read(rg_vis[i / 2], gfx::Access::FragmentRead);
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
  auto unorm = [](double v) { return static_cast<int>(std::lround(ref::clamp01(v) * 255.0)); };

  // Every covered pixel of every view against the reference. A pixel is covered where the
  // reference's ray meets the quad in front of the camera; the ones within a pixel of the quad's
  // edge or the horizon are left out, since a point sample there may be either side.
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
  const ref::Dvec3 ground_ref = ref::dvec3(ground);
  const ref::Dvec3 sand{0.84, 0.69, 0.47};
  int worst_shaded[k_views] = {};
  int worst_data[k_views] = {};
  u32 over_one[k_views] = {};
  u32 compared[k_views] = {};
  u32 rippled[k_views] = {};
  for (u32 v = 0; v < k_views; ++v) {
    const Look l = look(v);
    const Site s = sites[v / 2];
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const ref::Dvec3 p =
            ref::pixel_on_plane(ref::dvec3(l.eye), ref::dvec3(l.target), ref::dvec3(up),
                                static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0);
        // At or above the horizon, the ray meets the plane behind the eye.
        const ref::Dvec3 to_p = p - ref::dvec3(l.eye);
        if (ref::dot(to_p, ref::dvec3(l.target) - ref::dvec3(l.eye)) <= 0.0) continue;
        if (std::fabs(p.x - static_cast<double>(s.x)) > 29.5 ||
            std::fabs(p.z - static_cast<double>(s.z)) > 29.5) {
          continue;
        }
        ref::Dvec3 dpdx;
        ref::Dvec3 dpdy;
        gref::plane_footprint(ref::dvec3(l.eye), ref::dvec3(l.target), ref::dvec3(up),
                              static_cast<double>(fov_y), 1.0, k_size, k_size, x, y, 0.0, dpdx,
                              dpdy);
        const double reach =
            std::max(std::sqrt(ref::dot(dpdx, dpdx)), std::sqrt(ref::dot(dpdy, dpdy)));
        if (reach > 1.0) continue;  // a pixel a metre long: the horizon's own rounding
        const gref::Shading g =
            gref::shade(detail, p, ref::Dvec3{0.0, 1.0, 0.0}, dpdx, dpdy, 1.0, sand, 0.92, false);
        const gref::Shading data =
            gref::shade(detail, p, ref::Dvec3{0.0, 1.0, 0.0}, dpdx, dpdy, 1.0, sand, 0.92, true);
        ref::Surface surface;
        surface.position = p;
        surface.normal = g.normal;
        surface.view = ref::normalize(ref::dvec3(l.eye) - p);
        surface.albedo = g.albedo;
        surface.roughness = g.roughness;
        surface.metallic = 0.0;
        const ref::Dvec3 linear =
            ref::shade(surface, sun_ref, 1.0, sky_ref, ground_ref, nullptr, 0, ref::Dvec3{});
        const int expect[3] = {ref::display(linear.x), ref::display(linear.y),
                               ref::display(linear.z)};
        const int expect_data[3] = {unorm(data.ripple * 0.5 + 0.5), unorm(data.weight * data.fade),
                                    unorm(data.grain * 0.5 + 0.5)};
        const u8* got = pixel(v * 2, x, y);
        const u8* got_data = pixel(v * 2 + 1, x, y);
        int here = 0;
        int here_data = 0;
        for (u32 c = 0; c < 3; ++c) {
          here = std::max(here, std::abs(int{got[c]} - expect[c]));
          here_data = std::max(here_data, std::abs(int{got_data[c]} - expect_data[c]));
        }
        const int data_tolerance = v < 2 ? 2 : 16;
        const int shaded_tolerance = v < 2 ? 2 : 4;
        if ((here > shaded_tolerance && worst_shaded[v] <= shaded_tolerance) ||
            (here_data > data_tolerance && worst_data[v] <= data_tolerance)) {
          MESSAGE("view " << v << " pixel " << x << "," << y << " at " << p.x << "," << p.z
                          << ": gpu data " << int{got_data[0]} << "," << int{got_data[1]} << ","
                          << int{got_data[2]} << " cpu " << expect_data[0] << "," << expect_data[1]
                          << "," << expect_data[2] << "; gpu shaded " << int{got[0]} << ","
                          << int{got[1]} << "," << int{got[2]} << " cpu " << expect[0] << ","
                          << expect[1] << "," << expect[2]);
        }
        worst_shaded[v] = std::max(worst_shaded[v], here);
        worst_data[v] = std::max(worst_data[v], here_data);
        if (here > 1) ++over_one[v];
        if (data.fade > 0.0) ++rippled[v];
        ++compared[v];
      }
    }
  }
  const char* names[k_views] = {"by the origin, along the sand", "by the origin, at the feet",
                                "3.7 km out, along the sand", "3.7 km out, at the feet"};
  for (u32 v = 0; v < k_views; ++v) {
    MESSAGE(std::string(names[v]) << ": " << compared[v] << " pixels compared, " << rippled[v]
                                  << " with ripples drawn; shaded worst " << worst_shaded[v]
                                  << " of 255 (" << over_one[v] << " over 1), detail view worst "
                                  << worst_data[v]);
    CHECK(compared[v] > k_size * k_size / 4);
    CHECK(rippled[v] > 0u);
    // The tolerance. By the origin, 2 of 255 on the shaded picture and on the detail view — the
    // shading tests' own, and where both land (1, RTX 5090, 2026-09-29). 3.7 km out, 4 on the
    // shaded picture and 16 on the detail view: a float there has a quarter of a millimetre under
    // the point, the reconstruction interpolates corners that far out and is off by about a
    // millimetre, and a millimetre is a thirtieth of a ripple's steep lee — which the raw height
    // channel shows at full contrast (12–14 measured) and the shaded picture at a few levels
    // where the sun falls straight across the crests (3 measured, on 485 of 25,600 pixels).
    CHECK(worst_shaded[v] <= (v < 2 ? 2 : 4));
    CHECK(worst_data[v] <= (v < 2 ? 2 : 16));
  }

  // The ripples are in the picture: at the feet, with the sun low along the wind, the shaded sand
  // varies by more than the grain alone could make it (the grain moves the albedo by 8% either way
  // at most, a handful of levels here).
  {
    int lo = 255;
    int hi = 0;
    for (u32 y = k_size / 4; y < 3 * k_size / 4; ++y) {
      for (u32 x = k_size / 4; x < 3 * k_size / 4; ++x) {
        lo = std::min(lo, int{pixel(2, x, y)[1]});
        hi = std::max(hi, int{pixel(2, x, y)[1]});
      }
    }
    MESSAGE("at the feet the green channel spans " << lo << " to " << hi);
    CHECK(hi - lo > 20);
  }

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  for (u32 s = 0; s < 2; ++s) {
    scenes[s].destroy(device);
    gfx::destroy_buffer(device, clusters[s]);
    gfx::destroy_buffer(device, triangles[s]);
    gfx::destroy_buffer(device, cluster_materials[s]);
  }
  for (u32 v = 0; v < k_views; ++v)
    gfx::destroy_buffer(device, vis[v]);
  for (gfx::BufferResource* b : {&host_color, &params, &detail_buffer, &materials})
    gfx::destroy_buffer(device, *b);
  frames.destroy();
  device.destroy();
}
