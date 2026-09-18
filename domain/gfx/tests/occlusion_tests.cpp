// Two-pass occlusion culling end to end. A low camera over the terrain hides back clusters
// behind front hills. The reference is one cull pass without occlusion drawn into a
// visibility buffer; then three frames run the two-pass scheme (pass 1: last frame's visible
// set against last frame's Hi-Z; Hi-Z rebuilt from pass 1; pass 2: the rest against it;
// Hi-Z rebuilt again for the next frame). Occlusion culling must not change the picture: the
// third frame's visibility buffer must match the reference, while drawing fewer clusters.
// The Hi-Z pyramid itself is checked against a CPU recomputation.
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/hiz_build.spv.h>
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

TEST_CASE("occlusion culling: two passes draw fewer clusters and the same picture") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().mesh_shader || !device.features().buffer_int64_atomics) {
    MESSAGE("no mesh shaders or 64-bit buffer atomics on " << device.adapter().name);
    device.destroy();
    return;
  }

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(129, 10.0f, positions, indices);  // 32,768 triangles
  // A wall across the whole view in front of the terrain's middle: everything behind it is hidden.
  const u32 wall = positions.size();
  positions.push_back(Vec3{-30.0f, -10.0f, 5.0f});
  positions.push_back(Vec3{30.0f, -10.0f, 5.0f});
  positions.push_back(Vec3{30.0f, 25.0f, 5.0f});
  positions.push_back(Vec3{-30.0f, 25.0f, 5.0f});
  for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
    indices.push_back(wall + i);
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  const u32 cluster_count = lod.mesh.clusters.size();

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  constexpr VkBufferUsageFlags k_args = k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  gfx_test::SingleInstance scene;
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

  // Low camera: front hills hide the back of the terrain.
  constexpr u32 k_w = 384;
  constexpr u32 k_h = 256;
  const Vec3 eye{0.0f, 1.6f, 24.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(50.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                         look_at(eye, Vec3{0.0f, 0.4f, 0.0f}, Vec3{0, 1, 0});
  const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;

  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  u32 hiz_offsets[gfx::k_hiz_max_mips];
  const u32 hiz_elements = gfx::hiz_layout(k_w, k_h, hiz_offsets);
  const u32 hiz_mips = gfx::hiz_mip_count(k_w, k_h);
  CHECK(hiz_mips == 10);  // 384 -> 1 takes 9 halvings
  CHECK(hiz_offsets[1] == k_w * k_h);
  CHECK(hiz_elements > k_w * k_h * 4 / 3);
  CHECK(hiz_elements < k_w * k_h * 4 / 3 + 64);

  gfx::BufferResource vis;
  gfx::BufferResource hiz;
  gfx::BufferResource visible;
  gfx::BufferResource args[2];
  gfx::BufferResource flags[2];
  gfx::BufferResource params;  // three CullParams: reference, pass 1, pass 2
  gfx::BufferResource host_vis_ref;
  gfx::BufferResource host_vis;
  gfx::BufferResource host_list_ref;
  gfx::BufferResource host_list;
  gfx::BufferResource host_hiz;
  gfx::BufferResource host_args;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(
      device, u64{hiz_elements} * 4,
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, hiz,
      &error));
  // The two passes append to their own runs of one visible list, so a visibility id names an
  // entry of the whole list and the resolve needs nothing else: pass 1 starts at 0 and pass 2 at
  // `cluster_count`, which is as many entries as either pass can produce.
  const u64 visible_bytes = u64{cluster_count} * 2 * 8;
  const u64 visible_run[2] = {visible_bytes / 2 * 0, u64{cluster_count} * 8};
  REQUIRE(gfx::create_buffer(device, visible_bytes, k_address | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, visible, &error));
  for (u32 i = 0; i < 2; ++i) {
    REQUIRE(gfx::create_buffer(device, 12, k_args, false, args[i], &error));
    REQUIRE(gfx::create_buffer(device, u64{cluster_count} * 4,
                               k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, flags[i],
                               &error));
  }
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams) * 3, k_address, true, params, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             host_vis_ref, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host_vis,
                             &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             host_list_ref, &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             host_list, &error));
  REQUIRE(gfx::create_buffer(device, u64{hiz_elements} * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             host_hiz, &error));
  REQUIRE(
      gfx::create_buffer(device, 24, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host_args, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  VkShaderModule mesh_module = gfx::create_shader_module(
      device, shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size, &error);
  VkShaderModule hiz_module = gfx::create_shader_module(device, shaders::k_hiz_build_spirv,
                                                        shaders::k_hiz_build_spirv_size, &error);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  REQUIRE(mesh_module != VK_NULL_HANDLE);
  REQUIRE(hiz_module != VK_NULL_HANDLE);
  gfx::ComputePipeline cull_pipeline;
  gfx::ComputePipeline hiz_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               cull_pipeline, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, hiz_module, "hiz_build_main", {},
                                               sizeof(gfx::HizParams), hiz_pipeline, &error),
                  error);
  gfx::MeshPipelineDesc mesh_desc;
  mesh_desc.mesh = mesh_module;
  mesh_desc.fragment = mesh_module;
  mesh_desc.fragment_entry = "fs_visibility";
  mesh_desc.layout = bindless.pipeline_layout();
  VkPipeline mesh_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_mesh_pipeline(device, mesh_desc, mesh_pipeline, &error), error);

  // Parameter blocks: reference (single pass, no Hi-Z), pass 1, pass 2.
  gfx::CullParams base{};
  gfx::set_frustum(base, frustum_from_view_proj(view_proj));
  base.view_proj = view_proj;
  base.camera = Vec4{eye, znear};
  base.lod = Vec4{proj_scale, 0.5f, 1.0f, 1.0f};
  base.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
  base.cluster_count = cluster_count;
  base.clusters = clusters.address;
  base.lods = lods.address;
  base.instances = scene.instances.address;
  base.meshes = scene.meshes.address;
  base.instance_count = 1;
  base.pair_count = scene.pair_count();
  base.hiz_width = k_w;
  base.hiz_height = k_h;
  base.hiz_mips = hiz_mips;
  std::memcpy(base.hiz_offsets, hiz_offsets, sizeof(hiz_offsets));
  auto* blocks = static_cast<gfx::CullParams*>(params.mapped);
  const u64 block_stride = sizeof(gfx::CullParams);
  gfx::CullParams reference = base;
  reference.visible = visible.address;
  reference.draw_args = args[0].address;
  blocks[0] = reference;

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.visibility = vis.address;
  draw.width = k_w;
  draw.height = k_h;
  gfx::ClusterDrawParams draw_pass[2] = {draw, draw};
  for (u32 i = 0; i < 2; ++i) {
    draw_pass[i].visible = visible.address + visible_run[i];
    draw_pass[i].visible_offset = static_cast<u32>(visible_run[i] / 8);
  }

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis;
  gfx::RgBuffer rg_hiz;
  gfx::RgBuffer rg_visible;
  gfx::RgBuffer rg_args[2];
  gfx::RgBuffer rg_flags[2];
  auto import_all = [&]() {
    rg_vis = graph.import_buffer("vis", vis);
    rg_visible = graph.import_buffer("visible", visible);
    rg_hiz = graph.import_buffer("hiz", hiz);
    for (u32 i = 0; i < 2; ++i) {
      rg_args[i] = graph.import_buffer("args", args[i]);
      rg_flags[i] = graph.import_buffer("flags", flags[i]);
    }
  };
  auto add_reset = [&](bool clear_hiz, u32 flags_to_clear) {
    graph.add_pass(
        "reset", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.write(rg_vis, gfx::Access::TransferWrite);
          for (u32 i = 0; i < 2; ++i)
            b.write(rg_args[i], gfx::Access::TransferWrite);
          if (clear_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
          if (flags_to_clear < 2) b.write(rg_flags[flags_to_clear], gfx::Access::TransferWrite);
        },
        [&, clear_hiz, flags_to_clear](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdFillBuffer(cb, vis.buffer, 0, VK_WHOLE_SIZE, 0);
          for (u32 i = 0; i < 2; ++i) {
            vkCmdFillBuffer(cb, args[i].buffer, 0, 4, 0);
            vkCmdFillBuffer(cb, args[i].buffer, 4, 8, 1);
          }
          if (clear_hiz) vkCmdFillBuffer(cb, hiz.buffer, 0, VK_WHOLE_SIZE, 0);
          if (flags_to_clear < 2)
            vkCmdFillBuffer(cb, flags[flags_to_clear].buffer, 0, VK_WHOLE_SIZE, 0);
        });
  };
  // Pass bodies capture by pointer/reference, so the per-pass values live in these arrays.
  u64 block_addresses[3] = {params.address, params.address + block_stride,
                            params.address + 2 * block_stride};
  auto add_cull = [&](u32 block, u32 list, bool reads_hiz, u32 prev_flags, u32 cur_flags) {
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&, list, reads_hiz, prev_flags, cur_flags](gfx::PassBuilder& b) {
          b.write(rg_args[list], gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          if (reads_hiz) b.read(rg_hiz, gfx::Access::ComputeRead);
          if (prev_flags < 2) b.read(rg_flags[prev_flags], gfx::Access::ComputeRead);
          if (cur_flags < 2) b.write(rg_flags[cur_flags], gfx::Access::ComputeReadWrite);
        },
        [&, block](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline);
          vkCmdPushConstants(cb, cull_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                             &block_addresses[block]);
          vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
        });
  };
  auto add_draw = [&](u32 list) {
    graph.add_pass(
        "draw", gfx::PassKind::Raster,
        [&, list](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_vis, gfx::Access::FragmentReadWrite);
          b.read(rg_args[list], gfx::Access::IndirectRead);
          b.read(rg_visible, gfx::Access::MeshRead);
        },
        [&, list](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(draw_pass[list]), &draw_pass[list]);
          vkCmdDrawMeshTasksIndirectEXT(cb, args[list].buffer, 0, 1, 12);
        });
  };
  // One dispatch folds a 32 x 32 tile into six mips, so the pyramid is a handful of dispatches
  // and the CPU recomputation below is what says the values are the mip-at-a-time ones.
  const u32 hiz_dispatches = gfx::hiz_dispatch_count(hiz_mips);
  CHECK(hiz_dispatches == 2);                             // 10 mips: 0..5, then 6..9
  Vector<gfx::HizParams> hiz_params(hiz_dispatches * 2);  // stable storage for pass bodies
  auto add_hiz = [&](u32 set) {
    for (u32 d = 0; d < hiz_dispatches; ++d) {
      gfx::HizParams* level = &hiz_params[set * hiz_dispatches + d];
      gfx::HizParams& p = *level;
      const u32 src_mip = gfx::hiz_dispatch_src_mip(d);
      p = gfx::HizParams{};
      p.from_visibility = d == 0 ? 1u : 0u;
      p.src = d == 0 ? vis.address : hiz.address + u64{hiz_offsets[src_mip]} * 4;
      p.pyramid = hiz.address;
      p.width = k_w;
      p.height = k_h;
      p.src_mip = src_mip;
      p.src_offset = hiz_offsets[src_mip];
      p.levels = gfx::hiz_dispatch_levels(hiz_mips, d);
      const u32 src_w = gfx::hiz_mip_extent(k_w, src_mip);
      const u32 src_h = gfx::hiz_mip_extent(k_h, src_mip);
      graph.add_pass(
          "hiz", gfx::PassKind::Compute,
          [&, d](gfx::PassBuilder& b) {
            if (d == 0) b.read(rg_vis, gfx::Access::ComputeRead);
            b.write(rg_hiz, gfx::Access::ComputeReadWrite);
          },
          [&, level, src_w, src_h](VkCommandBuffer cb, gfx::RenderGraph&) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline.pipeline);
            vkCmdPushConstants(cb, hiz_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(*level), level);
            vkCmdDispatch(cb, gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
          });
    }
  };
  auto add_readback = [&](gfx::BufferResource& vis_host_ref, gfx::BufferResource& list_host_ref,
                          bool with_hiz) {
    gfx::BufferResource* vis_host = &vis_host_ref;
    gfx::BufferResource* list_host = &list_host_ref;
    const gfx::RgBuffer rg_host = graph.import_buffer("host_vis", *vis_host);
    const gfx::RgBuffer rg_host_list = graph.import_buffer("host_list", *list_host);
    const gfx::RgBuffer rg_host_hiz = graph.import_buffer("host_hiz", host_hiz);
    const gfx::RgBuffer rg_host_args = graph.import_buffer("host_args", host_args);
    graph.add_pass(
        "readback", gfx::PassKind::Transfer,
        [&, with_hiz](gfx::PassBuilder& b) {
          b.read(rg_vis, gfx::Access::TransferRead);
          b.read(rg_visible, gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
          b.write(rg_host_list, gfx::Access::TransferWrite);
          for (u32 i = 0; i < 2; ++i)
            b.read(rg_args[i], gfx::Access::TransferRead);
          b.write(rg_host_args, gfx::Access::TransferWrite);
          if (with_hiz) {
            b.read(rg_hiz, gfx::Access::TransferRead);
            b.write(rg_host_hiz, gfx::Access::TransferWrite);
          }
        },
        [&, vis_host, list_host, with_hiz](VkCommandBuffer cb, gfx::RenderGraph&) {
          const VkBufferCopy vis_copy{0, 0, vis_bytes};
          vkCmdCopyBuffer(cb, vis.buffer, vis_host->buffer, 1, &vis_copy);
          const VkBufferCopy list_copy{0, 0, visible_bytes};
          vkCmdCopyBuffer(cb, visible.buffer, list_host->buffer, 1, &list_copy);
          for (u32 i = 0; i < 2; ++i) {
            const VkBufferCopy args_copy{0, i * 12, 12};
            vkCmdCopyBuffer(cb, args[i].buffer, host_args.buffer, 1, &args_copy);
          }
          if (with_hiz) {
            const VkBufferCopy hiz_copy{0, 0, u64{hiz_elements} * 4};
            vkCmdCopyBuffer(cb, hiz.buffer, host_hiz.buffer, 1, &hiz_copy);
          }
        });
  };
  auto run_frame = [&]() {
    VkCommandBuffer commands = frames.begin_frame();
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
  };

  // Reference: one pass, no occlusion.
  import_all();
  add_reset(true, 0);
  add_cull(0, 0, false, 2, 2);
  add_draw(0);
  add_readback(host_vis_ref, host_list_ref, false);
  REQUIRE_MESSAGE(graph.compile(&error), error);
  run_frame();
  const auto* args_out = static_cast<const u32*>(host_args.mapped);
  const u32 reference_count = args_out[0];
  REQUIRE(reference_count > 20);
  MESSAGE("reference: " << reference_count << " clusters of " << cluster_count);

  // Three frames of two-pass occlusion culling; Hi-Z starts empty (everything far).
  u32 drawn[3][2] = {};
  for (u32 frame = 0; frame < 3; ++frame) {
    const u32 cur = frame % 2;
    const u32 prev = 1 - cur;
    gfx::CullParams pass1 = base;
    pass1.pass = 1;
    pass1.hiz = hiz.address;
    pass1.prev_flags = flags[prev].address;
    pass1.flags = flags[cur].address;
    pass1.visible = visible.address + visible_run[0];
    pass1.draw_args = args[0].address;
    gfx::CullParams pass2 = pass1;
    pass2.pass = 2;
    pass2.visible = visible.address + visible_run[1];
    pass2.draw_args = args[1].address;
    frames.wait_idle();  // the params buffer is host-visible and shared by the blocks
    blocks[1] = pass1;
    blocks[2] = pass2;

    graph.reset();
    import_all();
    add_reset(frame == 0, cur);
    if (frame == 0) {
      // No previous frame: clear both flag buffers so pass 1 has no candidates.
      graph.add_pass(
          "clear prev", gfx::PassKind::Transfer,
          [&, prev](gfx::PassBuilder& b) { b.write(rg_flags[prev], gfx::Access::TransferWrite); },
          [&, prev](VkCommandBuffer cb, gfx::RenderGraph&) {
            vkCmdFillBuffer(cb, flags[prev].buffer, 0, VK_WHOLE_SIZE, 0);
          });
    }
    add_cull(1, 0, true, prev, cur);
    add_draw(0);
    add_hiz(0);
    add_cull(2, 1, true, 2, cur);
    add_draw(1);
    add_hiz(1);
    add_readback(host_vis, host_list, true);
    REQUIRE_MESSAGE(graph.compile(&error), error);
    run_frame();
    drawn[frame][0] = args_out[0];
    drawn[frame][1] = args_out[3];
    MESSAGE("frame " << frame << ": pass 1 drew " << drawn[frame][0] << ", pass 2 drew "
                     << drawn[frame][1]);
  }
  // Frame 0 has no history: pass 1 draws nothing, pass 2 draws the whole reference set.
  CHECK(drawn[0][0] == 0);
  CHECK(drawn[0][1] == reference_count);
  // From frame 1 on, occluded clusters drop out and pass 2 has little left to add.
  CHECK(drawn[1][0] + drawn[1][1] < reference_count);
  CHECK(drawn[2][0] + drawn[2][1] < reference_count);
  CHECK(drawn[2][1] <= drawn[1][1]);
  CHECK(drawn[2][0] + drawn[2][1] == drawn[1][0] + drawn[1][1]);  // static scene: stable set

  // The picture is unchanged.
  const auto* ref = static_cast<const u64*>(host_vis_ref.mapped);
  const auto* cur_vis = static_cast<const u64*>(host_vis.mapped);
  const auto* ref_list = static_cast<const u32*>(host_list_ref.mapped);
  const auto* cur_list = static_cast<const u32*>(host_list.mapped);
  // A visibility id names an entry of that frame's visible list, and the two frames filled the
  // list differently, so what must agree is the (cluster, triangle) the id leads to.
  auto surface_of = [](u64 word, const u32* list) {
    const u32 id = static_cast<u32>(word);
    return (u64{list[(id >> 8) * 2 + 1]} << 8) | (id & 0xff);
  };
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 id_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool a = ref[i] != 0;
    const bool b = cur_vis[i] != 0;
    covered += a;
    if (a != b) ++coverage_mismatch;
    if (a && b && surface_of(ref[i], ref_list) != surface_of(cur_vis[i], cur_list)) ++id_mismatch;
  }
  CHECK(covered > k_w * k_h / 16);
  CHECK(coverage_mismatch == 0);
  CHECK(id_mismatch * 1000 <= covered);  // depth ties along shared edges only
  MESSAGE("covered " << covered << " px, coverage mismatch " << coverage_mismatch
                     << ", id mismatch " << id_mismatch);

  // Hi-Z: mip 0 is the depth of the visibility buffer; every mip is the min of its children.
  const auto* hiz_out = static_cast<const f32*>(host_hiz.mapped);
  u32 mip0_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const u32 bits = static_cast<u32>(cur_vis[i] >> 32);
    f32 depth = 0.0f;
    std::memcpy(&depth, &bits, 4);
    if (hiz_out[i] != depth) ++mip0_mismatch;
  }
  CHECK(mip0_mismatch == 0);
  u32 mip_mismatch = 0;
  for (u32 m = 1; m < hiz_mips; ++m) {
    const u32 sw = gfx::hiz_mip_extent(k_w, m - 1);
    const u32 sh = gfx::hiz_mip_extent(k_h, m - 1);
    const u32 dw = gfx::hiz_mip_extent(k_w, m);
    const u32 dh = gfx::hiz_mip_extent(k_h, m);
    for (u32 y = 0; y < dh; ++y) {
      for (u32 x = 0; x < dw; ++x) {
        f32 expected = 1e30f;
        for (u32 sy = y * 2; sy <= (y * 2 + 1 < sh ? y * 2 + 1 : sh - 1); ++sy) {
          for (u32 sx = x * 2; sx <= (x * 2 + 1 < sw ? x * 2 + 1 : sw - 1); ++sx) {
            const f32 v = hiz_out[hiz_offsets[m - 1] + sy * sw + sx];
            expected = v < expected ? v : expected;
          }
        }
        if (hiz_out[hiz_offsets[m] + y * dw + x] != expected) ++mip_mismatch;
      }
    }
  }
  CHECK(mip_mismatch == 0);
  CHECK(hiz_out[hiz_offsets[hiz_mips - 1]] >
        0.0f);  // the wall fills the view: no sky, nothing at the far plane

  graph.reset();
  gfx::destroy_pipeline(device, mesh_pipeline);
  gfx::destroy_compute_pipeline(device, hiz_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, hiz_module);
  gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_shader_module(device, cull_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&host_args, &host_hiz, &host_list, &host_list_ref, &host_vis, &host_vis_ref, &params, &hiz,
        &vis, &lods, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  gfx::destroy_buffer(device, visible);
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_buffer(device, args[i]);
    gfx::destroy_buffer(device, flags[i]);
  }
  frames.destroy();
  device.destroy();
}

// The pyramid the shader builds is the pyramid the mip-at-a-time build used to write, texel for
// texel, at a size chosen to break it if it can be broken. One workgroup folds a 32 x 32 tile
// through six mips in shared memory, so the two things that could differ from a dispatch per mip
// are the tile boundaries and the **odd extents**: a mip of odd width rounds up, and its last
// texel is the one the per-mip pass clamped a missing source onto. 2053 x 1027 is odd at six of
// its thirteen levels, needs three dispatches (mips 0-5, 6-10, 11-12), and leaves a partial tile
// on both axes at every level. The source is a pseudo-random visibility buffer with a fifth of
// its pixels empty, so the depths are unordered and a fold that took the wrong four texels would
// show. The test that matters for the *picture* is the one above; this one is the arithmetic.
TEST_CASE("hi-z: the folded pyramid equals the mip-at-a-time one at an awkward size") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  constexpr u32 k_w = 2053;
  constexpr u32 k_h = 1027;
  u32 hiz_offsets[gfx::k_hiz_max_mips];
  const u32 hiz_elements = gfx::hiz_layout(k_w, k_h, hiz_offsets);
  const u32 hiz_mips = gfx::hiz_mip_count(k_w, k_h);
  const u32 dispatches = gfx::hiz_dispatch_count(hiz_mips);
  CHECK(hiz_mips == 13);
  CHECK(dispatches == 3);

  Vector<u64> source;
  source.resize(k_w * k_h);
  u32 state = 0x13579bdfu;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    state = state * 1664525u + 1013904223u;
    // A reversed-Z depth in (0, 1) in the high word, an arbitrary id in the low one; one pixel
    // in five is empty, which is the whole word zero and the far plane.
    const f32 depth = static_cast<f32>((state >> 8) & 0xffffu) / 65536.0f;
    u32 bits = 0;
    std::memcpy(&bits, &depth, 4);
    source[i] = (state % 5u) == 0 ? 0 : (u64{bits} << 32) | (state & 0xffffffu);
  }

  constexpr VkBufferUsageFlags k_address =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  gfx::BufferResource vis;
  gfx::BufferResource hiz;
  gfx::BufferResource host_hiz;
  REQUIRE(gfx::upload_buffer(device, source.data(), u64{k_w} * k_h * sizeof(u64), k_address, vis,
                             &error));
  REQUIRE(gfx::create_buffer(
      device, u64{hiz_elements} * 4,
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, hiz,
      &error));
  REQUIRE(gfx::create_buffer(device, u64{hiz_elements} * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             host_hiz, &error));

  VkShaderModule module = gfx::create_shader_module(device, shaders::k_hiz_build_spirv,
                                                    shaders::k_hiz_build_spirv_size, &error);
  REQUIRE(module != VK_NULL_HANDLE);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "hiz_build_main", {},
                                               sizeof(gfx::HizParams), pipeline, &error),
                  error);
  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 1, &error));
  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_vis = graph.import_buffer("visibility", vis);
  const gfx::RgBuffer rg_hiz = graph.import_buffer("hiz", hiz);
  const gfx::RgBuffer rg_host = graph.import_buffer("host_hiz", host_hiz);
  // A sentinel under every mip: a texel the shader forgets to write shows up as one that never
  // changed, not as whatever the allocator happened to leave there.
  graph.add_pass(
      "fill", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_hiz, gfx::Access::TransferWrite); },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdFillBuffer(cb, hiz.buffer, 0, VK_WHOLE_SIZE, 0xbadf00du);
      });
  Vector<gfx::HizParams> params(dispatches);
  for (u32 d = 0; d < dispatches; ++d) {
    gfx::HizParams* level = &params[d];
    const u32 src_mip = gfx::hiz_dispatch_src_mip(d);
    *level = gfx::HizParams{};
    level->from_visibility = d == 0 ? 1u : 0u;
    level->src = d == 0 ? vis.address : hiz.address + u64{hiz_offsets[src_mip]} * 4;
    level->pyramid = hiz.address;
    level->width = k_w;
    level->height = k_h;
    level->src_mip = src_mip;
    level->src_offset = hiz_offsets[src_mip];
    level->levels = gfx::hiz_dispatch_levels(hiz_mips, d);
    const u32 src_w = gfx::hiz_mip_extent(k_w, src_mip);
    const u32 src_h = gfx::hiz_mip_extent(k_h, src_mip);
    graph.add_pass(
        "hiz", gfx::PassKind::Compute,
        [&, d](gfx::PassBuilder& b) {
          if (d == 0) b.read(rg_vis, gfx::Access::ComputeRead);
          b.write(rg_hiz, gfx::Access::ComputeReadWrite);
        },
        [&, level, src_w, src_h](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
          vkCmdPushConstants(cb, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*level),
                             level);
          vkCmdDispatch(cb, gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_hiz, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        const VkBufferCopy copy{0, 0, u64{hiz_elements} * 4};
        vkCmdCopyBuffer(cb, hiz.buffer, host_hiz.buffer, 1, &copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* out = static_cast<const f32*>(host_hiz.mapped);
  u32 mip0_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const u32 bits = static_cast<u32>(source[i] >> 32);
    f32 depth = 0.0f;
    std::memcpy(&depth, &bits, 4);
    if (out[i] != depth) ++mip0_mismatch;
  }
  CHECK(mip0_mismatch == 0);
  u32 mismatch = 0;
  for (u32 m = 1; m < hiz_mips; ++m) {
    const u32 sw = gfx::hiz_mip_extent(k_w, m - 1);
    const u32 sh = gfx::hiz_mip_extent(k_h, m - 1);
    const u32 dw = gfx::hiz_mip_extent(k_w, m);
    const u32 dh = gfx::hiz_mip_extent(k_h, m);
    for (u32 y = 0; y < dh; ++y) {
      for (u32 x = 0; x < dw; ++x) {
        f32 expected = 1e30f;
        for (u32 sy = y * 2; sy <= (y * 2 + 1 < sh ? y * 2 + 1 : sh - 1); ++sy) {
          for (u32 sx = x * 2; sx <= (x * 2 + 1 < sw ? x * 2 + 1 : sw - 1); ++sx) {
            const f32 v = out[hiz_offsets[m - 1] + sy * sw + sx];
            expected = v < expected ? v : expected;
          }
        }
        if (out[hiz_offsets[m] + y * dw + x] != expected) ++mismatch;
      }
    }
  }
  CHECK(mismatch == 0);
  MESSAGE("hi-z " << k_w << "x" << k_h << ": " << hiz_mips << " mips in " << dispatches
                  << " dispatches, " << hiz_elements << " texels, " << mip0_mismatch << " + "
                  << mismatch << " mismatches");

  graph.reset();
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  gfx::destroy_buffer(device, host_hiz);
  gfx::destroy_buffer(device, hiz);
  gfx::destroy_buffer(device, vis);
  frames.destroy();
  device.destroy();
}
