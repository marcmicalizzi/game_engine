// GPU cluster culling and LOD selection end to end: build a terrain's LOD DAG, run the cull
// pass against a camera, and check that the survivors are exactly what the CPU reference
// selects, that the indirect draw of the cut covers the same pixels as drawing every leaf
// cluster, and that a camera looking away culls everything. Both draws go through the device's
// own raster path into visibility buffers (raster_path.h): mesh tasks where there are mesh
// shaders, and on the baseline tier the vertex shader with the cull pass counting into
// vkCmdDrawIndirect's instance count (`CullParams::count_index` = 1). The cull is the same pass
// either way, and it is checked against its CPU reference on both. Runs on any device with 64-bit
// buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <string>

using namespace engine;

namespace {

void make_terrain(u32 n, f32 extent, Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const f32 fx = -extent + 2.0f * extent * static_cast<f32>(x) / (n - 1);
      const f32 fz = -extent + 2.0f * extent * static_cast<f32>(z) / (n - 1);
      const f32 h = 0.9f * std::sin(fx * 0.55f) * std::cos(fz * 0.4f) +
                    0.35f * std::sin(fx * 1.7f + fz * 1.1f);
      positions.push_back(Vec3{fx, h, fz});
    }
  }
  for (u32 z = 0; z + 1 < n; ++z) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = z * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

}  // namespace

TEST_CASE("cluster cull: GPU selection matches the CPU reference and the cut covers the leaves") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  REQUIRE(geometry::validate_cluster_lod(lod, indices, &error));
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 leaf_count = lod.level_cluster_counts[0];
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  gfx_test::SingleInstance scene;
  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(scene.create(device, lod.mesh, cluster_count, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));

  // One argument block of `gfx::k_draw_args_bytes`, which holds either path's: {survivors, 1, 1}
  // for mesh tasks, {vertex count, survivors, 0, 0} for vkCmdDrawIndirect.
  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource visible_host;
  gfx::BufferResource args_host;
  const u64 visible_bytes = u64{cluster_count} * 2 * sizeof(u32);  // uint2 per entry
  REQUIRE(gfx::create_buffer(
      device, visible_bytes,
      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      false, visible, &error));
  REQUIRE(gfx::create_buffer(
      device, gfx::k_draw_args_bytes,
      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             visible_host, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_draw_args_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             args_host, &error));

  // Camera above and in front of the terrain, 60 degree vertical field of view, 256 px tall.
  constexpr u32 k_size = 256;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const Mat4 view_proj =
      perspective_reversed_z(radians(60.0f), 1.0f, znear) * look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const Frustum frustum = frustum_from_view_proj(view_proj);
  geometry::LodView view;
  view.camera = eye;
  view.znear = znear;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * static_cast<f32>(k_size) * 0.5f;
  view.threshold_px = 1.0f;

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx_test::ClusterRaster raster;  // the mesh path, or the vertex path without mesh shaders
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(), triangles_per_cluster, &error),
                  error);

  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum);
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{view.proj_scale, view.threshold_px, 1.0f, 1.0f};
  cull.cluster_count = cluster_count;
  cull.count_index = raster.count_index();
  cull.clusters = clusters.address;
  cull.lods = lods.address;
  cull.visible = visible.address;
  cull.draw_args = args.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = 1;
  cull.pair_count = scene.pair_count();
  std::memcpy(params.mapped, &cull, sizeof(cull));

  // CPU reference.
  Vector<u32> expected;
  for (u32 i = 0; i < cluster_count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    if (frustum_contains_sphere(frustum, c.center, c.radius) &&
        geometry::lod_selects(lod.lod[i], view)) {
      expected.push_back(i);
    }
  }
  REQUIRE(expected.size() > 4);
  REQUIRE(expected.size() < cluster_count);
  MESSAGE("clusters " << cluster_count << " (leaves " << leaf_count << "), expected visible "
                      << expected.size() << ", " << std::string(raster.name()) << " path");

  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  gfx::ComputePipeline cull_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               cull_pipeline, &error),
                  error);

  // Two visibility buffers, the cut's and every leaf's. The id's high bits are the entry of the
  // visible list the draw read, so the cut's pixels name survivors through that list.
  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  const VkBufferUsageFlags k_vis = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  gfx::BufferResource vis_cut;
  gfx::BufferResource vis_leaves;
  gfx::BufferResource cut_host;
  gfx::BufferResource leaves_host;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_cut, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_leaves, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, cut_host,
                             &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, leaves_host,
                             &error));

  gfx::ClusterDrawParams cut_params{};
  cut_params.view_proj = view_proj;
  cut_params.clusters = clusters.address;
  cut_params.mesh = scene.meshes.address;
  cut_params.instances = scene.instances.address;
  cut_params.triangles = triangles.address;
  cut_params.visible = visible.address;
  cut_params.visibility = vis_cut.address;
  cut_params.width = k_size;
  cut_params.height = k_size;
  gfx::ClusterDrawParams leaf_params = cut_params;
  leaf_params.visible = 0;  // clusters 0..leaf_count-1: the leaves come first
  leaf_params.visibility = vis_leaves.address;
  const u64 params_address = params.address;

  gfx::RenderGraph graph(device);
  auto add_cull_passes = [&](gfx::RgBuffer rg_args, gfx::RgBuffer rg_visible) {
    graph.add_pass(
        "reset", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) { b.write(rg_args, gfx::Access::TransferWrite); },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) { raster.reset_args(cb, args.buffer); });
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(rg_args, gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline);
          vkCmdPushConstants(cb, cull_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                             &params_address);
          vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
        });
  };

  const gfx::RgBuffer rg_args = graph.import_buffer("args", args);
  const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible);
  const gfx::RgBuffer rg_visible_host = graph.import_buffer("visible_host", visible_host);
  const gfx::RgBuffer rg_args_host = graph.import_buffer("args_host", args_host);
  const gfx::RgBuffer rg_cut = graph.import_buffer("vis_cut", vis_cut);
  const gfx::RgBuffer rg_leaves = graph.import_buffer("vis_leaves", vis_leaves);
  const gfx::RgBuffer rg_cut_host = graph.import_buffer("cut_host", cut_host);
  const gfx::RgBuffer rg_leaves_host = graph.import_buffer("leaves_host", leaves_host);
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_cut, gfx::Access::TransferWrite);
        b.write(rg_leaves, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdFillBuffer(cb, vis_cut.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, vis_leaves.buffer, 0, VK_WHOLE_SIZE, 0);
      });
  add_cull_passes(rg_args, rg_visible);
  graph.add_pass(
      "draw cut", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_cut, gfx::Access::FragmentReadWrite);
        b.read(rg_args, gfx::Access::IndirectRead);
        b.read(rg_visible, raster.geometry_read());
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        raster.draw_indirect(cb, bindless, cut_params, args.buffer);
      });
  graph.add_pass(
      "draw leaves", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_size, k_size);
        b.write(rg_leaves, gfx::Access::FragmentReadWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        raster.draw(cb, bindless, leaf_params, leaf_count);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_args, gfx::Access::TransferRead);
        b.read(rg_visible, gfx::Access::TransferRead);
        b.read(rg_cut, gfx::Access::TransferRead);
        b.read(rg_leaves, gfx::Access::TransferRead);
        b.write(rg_args_host, gfx::Access::TransferWrite);
        b.write(rg_visible_host, gfx::Access::TransferWrite);
        b.write(rg_cut_host, gfx::Access::TransferWrite);
        b.write(rg_leaves_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        const VkBufferCopy args_copy{0, 0, gfx::k_draw_args_bytes};
        vkCmdCopyBuffer(cb, args.buffer, args_host.buffer, 1, &args_copy);
        const VkBufferCopy visible_copy{0, 0, visible_bytes};
        vkCmdCopyBuffer(cb, visible.buffer, visible_host.buffer, 1, &visible_copy);
        const VkBufferCopy vis_copy{0, 0, vis_bytes};
        vkCmdCopyBuffer(cb, vis_cut.buffer, cut_host.buffer, 1, &vis_copy);
        vkCmdCopyBuffer(cb, vis_leaves.buffer, leaves_host.buffer, 1, &vis_copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  CHECK(graph.stats().buffer_barriers >= 3);  // fill -> cull, cull -> indirect/draw read, -> copies

  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  // Survivors equal the reference set, and the cull pass counted into the raster path's word and
  // left the rest of the block as the reset wrote it.
  const auto* args_out = static_cast<const u32*>(args_host.mapped);
  if (raster.path() == gfx_test::RasterPath::Mesh) {
    CHECK(args_out[1] == 1);
    CHECK(args_out[2] == 1);
  } else {
    CHECK(args_out[0] == triangles_per_cluster * 3);
    CHECK(args_out[2] == 0);
    CHECK(args_out[3] == 0);
  }
  const u32 count = raster.survivors(args_out);
  CHECK(count == expected.size());
  // A visible entry is {instance, cluster}; there is one instance, so every entry names it.
  Vector<u32> got;
  const auto* visible_out = static_cast<const u32*>(visible_host.mapped);
  u32 foreign_instance = 0;
  for (u32 i = 0; i < count && i < cluster_count; ++i) {
    if (visible_out[i * 2] != 0) ++foreign_instance;
    got.push_back(visible_out[i * 2 + 1]);
  }
  CHECK(foreign_instance == 0);
  std::sort(got.begin(), got.end());
  REQUIRE(got.size() == expected.size());
  u32 mismatched = 0;
  for (u32 i = 0; i < got.size(); ++i) {
    if (got[i] != expected[i]) ++mismatched;
  }
  CHECK(mismatched == 0);

  // The cut covers the same pixels as every leaf cluster, and every cut pixel names a survivor.
  const auto* cut_pixels = static_cast<const u64*>(cut_host.mapped);
  const auto* leaf_pixels = static_cast<const u64*>(leaves_host.mapped);
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 foreign = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    const bool in_cut = cut_pixels[i] != 0;
    const bool in_leaves = leaf_pixels[i] != 0;
    if (in_leaves) ++covered;
    if (in_cut != in_leaves) ++coverage_mismatch;
    // The id's high bits are the scene's pair (gfx.md, "The tie rule"), which for the one identity
    // instance of one mesh is the cluster itself.
    if (in_cut) {
      const u32 cluster = static_cast<u32>(cut_pixels[i]) >> 8;
      if (!std::binary_search(expected.begin(), expected.end(), cluster)) ++foreign;
    }
  }
  CHECK(covered > k_size * k_size / 8);     // the terrain fills about a fifth of this view
  CHECK(coverage_mismatch * 50 < covered);  // under 2% of the covered area along silhouettes
  CHECK(foreign == 0);
  MESSAGE("visible " << count << " of " << cluster_count << ", covered " << covered
                     << " px, coverage mismatch " << coverage_mismatch << " px");

  // A camera looking away from the terrain culls everything.
  graph.reset();
  gfx::CullParams away = cull;
  const Mat4 away_vp = perspective_reversed_z(radians(60.0f), 1.0f, znear) *
                       look_at(eye, Vec3{0.0f, 9.0f, 100.0f}, Vec3{0, 1, 0});
  gfx::set_frustum(away, frustum_from_view_proj(away_vp));
  std::memcpy(params.mapped, &away, sizeof(away));
  const gfx::RgBuffer rg_args2 = graph.import_buffer("args", args);
  const gfx::RgBuffer rg_visible2 = graph.import_buffer("visible", visible);
  const gfx::RgBuffer rg_args_host2 = graph.import_buffer("args_host", args_host);
  add_cull_passes(rg_args2, rg_visible2);
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_args2, gfx::Access::TransferRead);
        b.write(rg_args_host2, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        const VkBufferCopy args_copy{0, 0, gfx::k_draw_args_bytes};
        vkCmdCopyBuffer(cb, args.buffer, args_host.buffer, 1, &args_copy);
      });
  REQUIRE(graph.compile(&error));
  commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));
  CHECK(raster.survivors(args_out) == 0);

  graph.reset();
  raster.destroy(device);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, cull_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&cut_host, &leaves_host, &vis_cut, &vis_leaves, &visible_host, &args_host, &params, &args,
        &visible, &lods, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
