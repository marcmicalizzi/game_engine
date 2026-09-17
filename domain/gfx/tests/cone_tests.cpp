// Normal-cone backface culling in the cull pass: a closed sphere is drawn through the vertex
// path twice, once with the cone test off and once with it on. The survivors with cones on must
// be exactly the clusters geometry::cluster_backfacing keeps (the CPU reference of the shader's
// test), a third to a half of the sphere must be culled, and the visibility buffer must not
// change: every pixel the culled draw covers holds the same word, and only silhouette pixels
// that back faces alone touched may differ. Runs on any GPU with 64-bit buffer atomics.
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
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
#include <shaders/cluster_vertex.spv.h>
#include <string>

using namespace engine;

namespace {

// A unit sphere wound counter-clockwise seen from outside (same as the geometry test).
void make_sphere(u32 rings, u32 segments, Vector<Vec3>& positions, Vector<u32>& indices) {
  positions.push_back(Vec3{0.0f, 1.0f, 0.0f});
  for (u32 r = 1; r < rings; ++r) {
    const f32 theta = k_pi * static_cast<f32>(r) / static_cast<f32>(rings);
    for (u32 s = 0; s < segments; ++s) {
      const f32 phi = k_two_pi * static_cast<f32>(s) / static_cast<f32>(segments);
      positions.push_back(
          Vec3{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
    }
  }
  positions.push_back(Vec3{0.0f, -1.0f, 0.0f});
  const u32 bottom = positions.size() - 1;
  auto ring = [segments](u32 r, u32 s) { return 1 + (r - 1) * segments + (s % segments); };
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(0);
    indices.push_back(ring(1, s + 1));
    indices.push_back(ring(1, s));
  }
  for (u32 r = 1; r + 1 < rings; ++r) {
    for (u32 s = 0; s < segments; ++s) {
      const u32 a = ring(r, s);
      const u32 b = ring(r, s + 1);
      const u32 c = ring(r + 1, s);
      const u32 d = ring(r + 1, s + 1);
      indices.push_back(a);
      indices.push_back(b);
      indices.push_back(c);
      indices.push_back(b);
      indices.push_back(d);
      indices.push_back(c);
    }
  }
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(ring(rings - 1, s));
    indices.push_back(ring(rings - 1, s + 1));
    indices.push_back(bottom);
  }
}

}  // namespace

TEST_CASE("normal cones: the cull pass drops backfacing clusters without changing the picture") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().buffer_int64_atomics) {
    MESSAGE("no 64-bit buffer atomics on " << device.adapter().name);
    device.destroy();
    return;
  }

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_sphere(24, 48, positions, indices);  // 2,208 triangles
  geometry::ClusterMesh mesh;
  REQUIRE_MESSAGE(
      geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh, &error),
      error);
  REQUIRE(geometry::validate_clusters(mesh, indices, geometry::ClusterBuildOptions{}, &error));
  const u32 cluster_count = mesh.clusters.size();
  const u32 triangles_per_cluster = geometry::ClusterBuildOptions{}.max_triangles;

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(scene.create(device, mesh, cluster_count, &error));
  REQUIRE(gfx::upload_buffer(device, mesh.triangles.data(), mesh.triangles.size() * sizeof(u32),
                             k_storage, triangles, &error));

  constexpr u32 k_size = 256;
  const Vec3 eye{0.8f, 0.6f, 3.2f};  // off-axis so no cluster sits on a symmetry line
  const f32 znear = 0.1f;
  const Mat4 view_proj =
      perspective_reversed_z(radians(60.0f), 1.0f, znear) * look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const Frustum frustum = frustum_from_view_proj(view_proj);

  // CPU reference: the sphere sits inside the frustum, so the cone test alone decides.
  Vector<u32> expected;
  for (u32 i = 0; i < cluster_count; ++i) {
    const geometry::ClusterDesc& c = mesh.clusters[i];
    REQUIRE(frustum_contains_sphere(frustum, c.center, c.radius));
    if (!geometry::cluster_backfacing(c, eye)) expected.push_back(i);
  }
  REQUIRE(expected.size() * 100 <= cluster_count * 70);  // at least 30% culled
  REQUIRE(expected.size() * 100 >= cluster_count * 40);  // at most 60% culled

  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  const u64 list_bytes = u64{cluster_count} * 2 * sizeof(u32);  // uint2 per entry
  const VkBufferUsageFlags k_vis =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const VkBufferUsageFlags k_args = k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  gfx::BufferResource vis[2];
  gfx::BufferResource visible[2];
  gfx::BufferResource args[2];
  gfx::BufferResource params[2];
  gfx::BufferResource host;
  for (u32 k = 0; k < 2; ++k) {
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis[k], &error));
    REQUIRE(gfx::create_buffer(device, list_bytes, k_vis, false, visible[k], &error));
    REQUIRE(gfx::create_buffer(device, 16, k_args, false, args[k], &error));
    REQUIRE(
        gfx::create_buffer(device, sizeof(gfx::CullParams), k_address, true, params[k], &error));
  }
  const u64 host_bytes = vis_bytes * 2 + list_bytes * 2 + 32;
  REQUIRE(
      gfx::create_buffer(device, host_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  REQUIRE(vertex_module != VK_NULL_HANDLE);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  VkPipeline vertex_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
  gfx::ComputePipeline cull_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                       cull_pipeline, &error));

  // Cull twice, cones off (k = 0) and on (k = 1); frustum on, LOD and occlusion off.
  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum);
  cull.view_proj = view_proj;
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{1.0f, 1.0f, 0.0f, 1.0f};
  cull.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
  cull.cluster_count = cluster_count;
  cull.count_index = 1;
  cull.clusters = clusters.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = 1;
  cull.pair_count = scene.pair_count();
  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.width = k_size;
  draw.height = k_size;
  gfx::ClusterDrawParams draws[2];
  u64 params_address[2];
  for (u32 k = 0; k < 2; ++k) {
    cull.cone_cull = k;
    cull.visible = visible[k].address;
    cull.draw_args = args[k].address;
    std::memcpy(params[k].mapped, &cull, sizeof(cull));
    params_address[k] = params[k].address;
    draws[k] = draw;
    draws[k].visible = visible[k].address;
    draws[k].visibility = vis[k].address;
  }

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[2];
  gfx::RgBuffer rg_visible[2];
  gfx::RgBuffer rg_args[2];
  for (u32 k = 0; k < 2; ++k) {
    rg_vis[k] = graph.import_buffer(k == 0 ? "vis off" : "vis on", vis[k]);
    rg_visible[k] = graph.import_buffer(k == 0 ? "visible off" : "visible on", visible[k]);
    rg_args[k] = graph.import_buffer(k == 0 ? "args off" : "args on", args[k]);
  }
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 k = 0; k < 2; ++k) {
          b.write(rg_vis[k], gfx::Access::TransferWrite);
          b.write(rg_args[k], gfx::Access::TransferWrite);
        }
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (u32 k = 0; k < 2; ++k) {
          vkCmdFillBuffer(cb, vis[k].buffer, 0, VK_WHOLE_SIZE, 0);
          vkCmdFillBuffer(cb, args[k].buffer, 0, 4, triangles_per_cluster * 3);  // vertexCount
          vkCmdFillBuffer(cb, args[k].buffer, 4, 12, 0);  // instanceCount, first vertex, instance
        }
      });
  for (u32 k = 0; k < 2; ++k) {
    graph.add_pass(
        k == 0 ? "cull off" : "cull on", gfx::PassKind::Compute,
        [&, k](gfx::PassBuilder& b) {
          b.write(rg_args[k], gfx::Access::ComputeReadWrite);
          b.write(rg_visible[k], gfx::Access::ComputeWrite);
        },
        [&, k](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline);
          vkCmdPushConstants(cb, cull_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                             &params_address[k]);
          vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
        });
    graph.add_pass(
        k == 0 ? "draw off" : "draw on", gfx::PassKind::Raster,
        [&, k](gfx::PassBuilder& b) {
          b.render_area(k_size, k_size);
          b.write(rg_vis[k], gfx::Access::FragmentReadWrite);
          b.read(rg_args[k], gfx::Access::IndirectRead);
          b.read(rg_visible[k], gfx::Access::VertexRead);
        },
        [&, k](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vertex_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(draws[k]), &draws[k]);
          vkCmdDrawIndirect(cb, args[k].buffer, 0, 1, sizeof(u32) * 4);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 k = 0; k < 2; ++k) {
          b.read(rg_vis[k], gfx::Access::TransferRead);
          b.read(rg_visible[k], gfx::Access::TransferRead);
          b.read(rg_args[k], gfx::Access::TransferRead);
        }
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (u32 k = 0; k < 2; ++k) {
          const VkBufferCopy vis_copy{0, vis_bytes * k, vis_bytes};
          const VkBufferCopy list_copy{0, vis_bytes * 2 + list_bytes * k, list_bytes};
          const VkBufferCopy args_copy{0, vis_bytes * 2 + list_bytes * 2 + 16 * k, 16};
          vkCmdCopyBuffer(cb, vis[k].buffer, host.buffer, 1, &vis_copy);
          vkCmdCopyBuffer(cb, visible[k].buffer, host.buffer, 1, &list_copy);
          vkCmdCopyBuffer(cb, args[k].buffer, host.buffer, 1, &args_copy);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* bytes = static_cast<const u8*>(host.mapped);
  const auto* vis_off = reinterpret_cast<const u64*>(bytes);
  const auto* vis_on = reinterpret_cast<const u64*>(bytes + vis_bytes);
  const auto* list_off = reinterpret_cast<const u32*>(bytes + vis_bytes * 2);
  const auto* list_on = reinterpret_cast<const u32*>(bytes + vis_bytes * 2 + list_bytes);
  const auto* args_off = reinterpret_cast<const u32*>(bytes + vis_bytes * 2 + list_bytes * 2);
  const u32* args_on = args_off + 4;

  // Cones off keeps the whole sphere; cones on keeps exactly the CPU reference set. An entry is
  // {instance, cluster}, and there is one instance.
  CHECK(args_off[0] == triangles_per_cluster * 3);
  CHECK(args_off[1] == cluster_count);
  CHECK(args_on[1] == expected.size());
  Vector<u32> got;
  for (u32 i = 0; i < args_on[1] && i < cluster_count; ++i)
    got.push_back(list_on[i * 2 + 1]);
  std::sort(got.begin(), got.end());
  REQUIRE(got.size() == expected.size());
  u32 mismatched = 0;
  for (u32 i = 0; i < got.size(); ++i)
    mismatched += got[i] != expected[i];
  CHECK(mismatched == 0);

  // The picture: every pixel the culled draw covers holds the same surface as the full draw (the
  // back faces of a closed convex body never win the depth race), and pixels only the full
  // draw covers are silhouette pixels back faces alone touched. The two draws have their own
  // visible lists, so a pixel's id names a different entry in each; the (cluster, triangle,
  // depth) it leads to is what must agree.
  auto surface_of = [&](u64 word, const u32* list) {
    const u32 id = static_cast<u32>(word);
    return (u64{list[(id >> 8) * 2 + 1]} << 40) | (u64{id & 0xff} << 32) | (word >> 32);
  };
  u32 covered = 0;
  u32 changed = 0;
  u32 lost = 0;
  for (u32 i = 0; i < k_size * k_size; ++i) {
    if (vis_on[i] != 0) {
      ++covered;
      if (vis_off[i] == 0 || surface_of(vis_off[i], list_off) != surface_of(vis_on[i], list_on))
        ++changed;
    } else if (vis_off[i] != 0) {
      ++lost;
    }
  }
  CHECK(covered > k_size * k_size / 6);  // the sphere fills about a third of the view
  CHECK(changed == 0);
  CHECK(lost * 500 <= covered);
  MESSAGE("clusters " << cluster_count << ", kept " << args_on[1] << ", covered " << covered
                      << " px, changed " << changed << ", lost " << lost);

  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, cull_module);
  bindless.destroy();
  frames.destroy();
  for (u32 k = 0; k < 2; ++k) {
    gfx::destroy_buffer(device, vis[k]);
    gfx::destroy_buffer(device, visible[k]);
    gfx::destroy_buffer(device, args[k]);
    gfx::destroy_buffer(device, params[k]);
  }
  scene.destroy(device);
  gfx::destroy_buffer(device, host);
  gfx::destroy_buffer(device, clusters);
  gfx::destroy_buffer(device, triangles);
  device.destroy();
}
