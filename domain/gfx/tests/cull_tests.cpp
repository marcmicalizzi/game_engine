// GPU cluster culling and LOD selection end to end: build a terrain's LOD DAG, run the cull
// pass against a camera, and check that the survivors are exactly what the CPU reference
// selects, that the indirect mesh draw of the cut covers the same pixels as drawing every
// leaf cluster, and that a camera looking away culls everything.
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
#include <shaders/cluster_mesh.spv.h>
#include <string>

using namespace engine;

namespace {

struct MeshParams {
  Mat4 view_proj;
  u64 clusters;
  u64 vertices;
  u64 triangles;
  u32 cluster_count;
  u32 pad = 0;
  u64 visible = 0;
};
static_assert(sizeof(MeshParams) == 104);

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
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().mesh_shader) {
    MESSAGE("no mesh shader support on " << device.adapter().name);
    device.destroy();
    return;
  }

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

  gfx::BufferResource clusters;
  gfx::BufferResource vertices;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.vertices.data(),
                             lod.mesh.vertices.size() * sizeof(Vec3), k_storage, vertices, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));

  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource visible_host;
  gfx::BufferResource args_host;
  REQUIRE(gfx::create_buffer(
      device, cluster_count * sizeof(u32),
      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      false, visible, &error));
  REQUIRE(gfx::create_buffer(
      device, sizeof(u32) * 3,
      k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, cluster_count * sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, visible_host, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
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

  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum);
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{view.proj_scale, view.threshold_px, 1.0f, 1.0f};
  cull.cluster_count = cluster_count;
  cull.clusters = clusters.address;
  cull.lods = lods.address;
  cull.visible = visible.address;
  cull.draw_args = args.address;
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
                      << expected.size());

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  VkShaderModule mesh_module = gfx::create_shader_module(
      device, shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size, &error);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  REQUIRE(mesh_module != VK_NULL_HANDLE);
  gfx::ComputePipeline cull_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               cull_pipeline, &error),
                  error);
  gfx::MeshPipelineDesc mesh_desc;
  mesh_desc.mesh = mesh_module;
  mesh_desc.fragment = mesh_module;
  mesh_desc.layout = bindless.pipeline_layout();
  mesh_desc.color_format = VK_FORMAT_R32_UINT;
  VkPipeline mesh_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_mesh_pipeline(device, mesh_desc, mesh_pipeline, &error), error);

  const u64 image_bytes = u64{k_size} * k_size * 4;
  gfx::BufferResource cut_host;
  gfx::BufferResource leaves_host;
  REQUIRE(gfx::create_buffer(device, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, cut_host,
                             &error));
  REQUIRE(gfx::create_buffer(device, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             leaves_host, &error));

  MeshParams mesh_params{};
  mesh_params.view_proj = view_proj;
  mesh_params.clusters = clusters.address;
  mesh_params.vertices = vertices.address;
  mesh_params.triangles = triangles.address;
  mesh_params.cluster_count = cluster_count;
  mesh_params.visible = visible.address;
  MeshParams leaf_params = mesh_params;
  leaf_params.cluster_count = leaf_count;
  leaf_params.visible = 0;
  const u64 params_address = params.address;

  gfx::RenderGraph graph(device);
  auto add_cull_passes = [&](gfx::RgBuffer rg_args, gfx::RgBuffer rg_visible) {
    graph.add_pass(
        "reset", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) { b.write(rg_args, gfx::Access::TransferWrite); },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdFillBuffer(cb, args.buffer, 0, sizeof(u32), 0);
          vkCmdFillBuffer(cb, args.buffer, sizeof(u32), sizeof(u32) * 2, 1);
        });
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
  const gfx::RgBuffer rg_cut_host = graph.import_buffer("cut_host", cut_host);
  const gfx::RgBuffer rg_leaves_host = graph.import_buffer("leaves_host", leaves_host);
  const gfx::RgImageDesc id_desc{
      k_size, k_size, VK_FORMAT_R32_UINT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  const gfx::RgImage cut_ids = graph.create_image("cut", id_desc);
  const gfx::RgImage leaf_ids = graph.create_image("leaves", id_desc);
  VkClearColorValue clear{};
  clear.uint32[0] = 0xFFFFFFFFu;
  add_cull_passes(rg_args, rg_visible);
  graph.add_pass(
      "draw cut", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.color_attachment(cut_ids, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
        b.read(rg_args, gfx::Access::IndirectRead);
        b.read(rg_visible, gfx::Access::MeshRead);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(mesh_params), &mesh_params);
        vkCmdDrawMeshTasksIndirectEXT(cb, args.buffer, 0, 1, sizeof(u32) * 3);
      });
  graph.add_pass(
      "draw leaves", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.color_attachment(leaf_ids, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                           sizeof(leaf_params), &leaf_params);
        vkCmdDrawMeshTasksEXT(cb, leaf_count, 1, 1);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_args, gfx::Access::TransferRead);
        b.read(rg_visible, gfx::Access::TransferRead);
        b.read(cut_ids, gfx::Access::TransferRead);
        b.read(leaf_ids, gfx::Access::TransferRead);
        b.write(rg_args_host, gfx::Access::TransferWrite);
        b.write(rg_visible_host, gfx::Access::TransferWrite);
        b.write(rg_cut_host, gfx::Access::TransferWrite);
        b.write(rg_leaves_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph& g) {
        const VkBufferCopy args_copy{0, 0, sizeof(u32) * 3};
        vkCmdCopyBuffer(cb, args.buffer, args_host.buffer, 1, &args_copy);
        const VkBufferCopy visible_copy{0, 0, u64{cluster_count} * sizeof(u32)};
        vkCmdCopyBuffer(cb, visible.buffer, visible_host.buffer, 1, &visible_copy);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_size, k_size, 1};
        vkCmdCopyImageToBuffer(cb, g.image(cut_ids).image, g.image_layout(cut_ids), cut_host.buffer,
                               1, &region);
        vkCmdCopyImageToBuffer(cb, g.image(leaf_ids).image, g.image_layout(leaf_ids),
                               leaves_host.buffer, 1, &region);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  CHECK(graph.stats().buffer_barriers >= 3);  // fill -> cull, cull -> indirect/mesh read, -> copies

  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  // Survivors equal the reference set.
  const auto* args_out = static_cast<const u32*>(args_host.mapped);
  CHECK(args_out[1] == 1);
  CHECK(args_out[2] == 1);
  const u32 count = args_out[0];
  CHECK(count == expected.size());
  Vector<u32> got;
  const auto* visible_out = static_cast<const u32*>(visible_host.mapped);
  for (u32 i = 0; i < count && i < cluster_count; ++i)
    got.push_back(visible_out[i]);
  std::sort(got.begin(), got.end());
  REQUIRE(got.size() == expected.size());
  u32 mismatched = 0;
  for (u32 i = 0; i < got.size(); ++i) {
    if (got[i] != expected[i]) ++mismatched;
  }
  CHECK(mismatched == 0);

  // The cut covers the same pixels as every leaf cluster, and every cut pixel names a survivor.
  const auto* cut_pixels = static_cast<const u32*>(cut_host.mapped);
  const auto* leaf_pixels = static_cast<const u32*>(leaves_host.mapped);
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 foreign = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    const bool in_cut = cut_pixels[i] != 0xFFFFFFFFu;
    const bool in_leaves = leaf_pixels[i] != 0xFFFFFFFFu;
    if (in_leaves) ++covered;
    if (in_cut != in_leaves) ++coverage_mismatch;
    if (in_cut && !std::binary_search(expected.begin(), expected.end(), cut_pixels[i] >> 8))
      ++foreign;
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
        const VkBufferCopy args_copy{0, 0, sizeof(u32) * 3};
        vkCmdCopyBuffer(cb, args.buffer, args_host.buffer, 1, &args_copy);
      });
  REQUIRE(graph.compile(&error));
  commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));
  CHECK(args_out[0] == 0);

  graph.reset();
  gfx::destroy_pipeline(device, mesh_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_shader_module(device, cull_module);
  bindless.destroy();
  for (gfx::BufferResource* b : {&cut_host, &leaves_host, &visible_host, &args_host, &params, &args,
                                 &visible, &lods, &triangles, &vertices, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
