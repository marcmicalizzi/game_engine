// The GPU half of geometry streaming (docs/plan/04-renderer.md §4.9): the cull pass's drawing rule
// against `geometry::select_lod_streaming`, the CPU reference, for the same residency.
//
// The two have to agree **exactly**, not approximately, because the rule is what keeps a cut
// crack-free when pages are missing: a cluster is drawn when its parent is too coarse and either it
// is fine enough or its children are not all here. One extra cluster is a surface drawn twice and
// one missing cluster is a hole, and neither shows up as anything but a subtly wrong picture.
//
// The tests run the cull pass with the frustum, the cone test and occlusion **off**, because the
// CPU reference is a pure LOD cut and knows nothing about any of them. Everything else — the
// instance transform, the projection, the threshold — is the identity or shared.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
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

// A deterministic 32-bit stream, so a failure is reproducible from the seed alone.
struct Rng {
  u32 state = 0x9e3779b9u;
  u32 next() noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
};

// A residency the drawing rule is allowed to see: **ancestor-closed**, which is the invariant
// `PageResidencyManager` maintains and without which the rule is not crack-free at all
// ([geometry](../../../docs/subsystems/geometry.md), "Why it is safe"). Start from everything
// resident and peel pages off the fine end, exactly as eviction does.
void random_residency(const geometry::ClusterPages& pages, Rng& rng, u32 evictions,
                      geometry::PageResidency& out) {
  const u32 count = pages.pages.size();
  out.resident.assign(count, u8{1});
  for (u32 attempt = 0; attempt < evictions * 8 && evictions > 0; ++attempt) {
    const u32 page = rng.next() % count;
    if (out.resident[page] == 0) continue;
    if ((pages.pages[page].flags & geometry::k_page_root) != 0) continue;
    bool free_below = true;
    const geometry::ClusterPageDesc& desc = pages.pages[page];
    for (u32 k = 0; k < desc.child_page_count && free_below; ++k)
      free_below = out.resident[pages.child_pages[desc.first_child_page + k]] == 0;
    if (!free_below) continue;
    out.resident[page] = 0;
    if (--evictions == 0) break;
  }
}

}  // namespace

TEST_CASE("cluster cull: the streaming drawing rule is the CPU reference, page for page") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  // A small target so the mesh is many pages rather than one: the rule only says anything when a
  // page can be missing.
  geometry::ClusterPages pages;
  geometry::ClusterPagesOptions options;
  options.page_bytes = 16 * 1024;
  REQUIRE_MESSAGE(geometry::build_cluster_pages(lod, options, pages, &error), error);
  REQUIRE_MESSAGE(geometry::validate_cluster_pages(lod, pages, &error), error);
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 page_count = pages.pages.size();
  REQUIRE(page_count > 4);
  MESSAGE("clusters " << cluster_count << " in " << page_count << " pages");

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  gfx::BufferResource clusters;
  gfx::BufferResource lods;
  gfx::BufferResource page_table;
  gfx::BufferResource page_of_cluster;
  gfx::BufferResource children;
  gfx::BufferResource residency;
  gfx::BufferResource used;
  gfx::BufferResource requests;
  gfx::BufferResource request_count;
  gfx::BufferResource request_mask;
  gfx::BufferResource stream_block;
  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource visible_host;
  gfx::BufferResource args_host;
  gfx::BufferResource requests_host;
  gfx_test::SingleInstance scene;
  Vector<u32> page_index(cluster_count);
  Vector<u32> child_ranges(cluster_count * 2);
  for (u32 c = 0; c < cluster_count; ++c) {
    page_index[c] = pages.page_of_cluster[c];
    child_ranges[c * 2 + 0] = pages.children[c].first_cluster;
    child_ranges[c * 2 + 1] = pages.children[c].cluster_count;
  }
  const u32 max_requests = page_count;
  const u64 visible_bytes = u64{cluster_count} * 2 * sizeof(u32);
  const u64 request_bytes = u64{max_requests} * sizeof(geometry::PageRequest);
  REQUIRE(scene.create(device, lod.mesh, cluster_count, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));
  REQUIRE(gfx::upload_buffer(device, pages.pages.data(),
                             page_count * sizeof(geometry::ClusterPageDesc), k_storage, page_table,
                             &error));
  REQUIRE(gfx::upload_buffer(device, page_index.data(), cluster_count * sizeof(u32), k_storage,
                             page_of_cluster, &error));
  REQUIRE(gfx::upload_buffer(device, child_ranges.data(), cluster_count * 2 * sizeof(u32),
                             k_storage, children, &error));
  REQUIRE(gfx::create_buffer(device, u64{page_count} * sizeof(u32),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, residency,
                             &error));
  REQUIRE(
      gfx::create_buffer(device, u64{page_count} * sizeof(u32), k_address, false, used, &error));
  REQUIRE(gfx::create_buffer(device, request_bytes, k_address, false, requests, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32), k_address, false, request_count, &error));
  REQUIRE(gfx::create_buffer(device, u64{page_count} * sizeof(u32), k_address, false, request_mask,
                             &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::StreamParams),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                             stream_block, &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, k_address, false, visible, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * 4,
                             k_address | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams),
                             k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             visible_host, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                             args_host, &error));
  REQUIRE(gfx::create_buffer(device, request_bytes + sizeof(u32), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, requests_host, &error));

  auto* block = static_cast<gfx::StreamParams*>(stream_block.mapped);
  *block = gfx::StreamParams{};
  block->pages = page_table.address;
  block->page_of_cluster = page_of_cluster.address;
  block->children = children.address;
  block->residency = residency.address;
  block->used = used.address;
  block->requests = requests.address;
  block->request_count = request_count.address;
  block->request_mask = request_mask.address;

  constexpr u32 k_size = 256;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  geometry::LodView view;
  view.camera = eye;
  view.znear = znear;
  view.proj_scale = 1.0f / std::tan(radians(60.0f) * 0.5f) * static_cast<f32>(k_size) * 0.5f;
  view.threshold_px = 1.0f;

  gfx::CullParams cull{};
  cull.camera = Vec4{eye, znear};
  // z: LOD selection on. w: frustum culling **off** — the reference is a pure LOD cut.
  cull.lod = Vec4{view.proj_scale, view.threshold_px, 1.0f, 0.0f};
  cull.cluster_count = cluster_count;
  cull.clusters = clusters.address;
  cull.lods = lods.address;
  cull.visible = visible.address;
  cull.draw_args = args.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = 1;
  cull.pair_count = cluster_count;
  cull.streaming = stream_block.address;
  cull.page_count = page_count;
  cull.max_requests = max_requests;
  std::memcpy(params.mapped, &cull, sizeof(cull));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  VkShaderModule cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  REQUIRE(cull_module != VK_NULL_HANDLE);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               pipeline, &error),
                  error);

  gfx::RenderGraph graph(device);
  const u64 params_address = params.address;
  // One dispatch and the three readbacks. The residency array is host-visible and written before
  // the frame is recorded, exactly as the renderer writes a frame slot's region.
  auto run = [&](const geometry::PageResidency& want, Vector<u32>& cut_out,
                 Vector<u32>& requests_out) {
    auto* words = static_cast<u32*>(residency.mapped);
    for (u32 p = 0; p < page_count; ++p)
      words[p] = want.is_resident(p) ? 1u : 0u;
    graph.reset();
    const gfx::RgBuffer rg_args = graph.import_buffer("args", args);
    const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible);
    const gfx::RgBuffer rg_used = graph.import_buffer("used", used);
    const gfx::RgBuffer rg_requests = graph.import_buffer("requests", requests);
    const gfx::RgBuffer rg_count = graph.import_buffer("count", request_count);
    const gfx::RgBuffer rg_mask = graph.import_buffer("mask", request_mask);
    const gfx::RgBuffer rg_visible_host = graph.import_buffer("visible_host", visible_host);
    const gfx::RgBuffer rg_args_host = graph.import_buffer("args_host", args_host);
    const gfx::RgBuffer rg_requests_host = graph.import_buffer("requests_host", requests_host);
    graph.add_pass(
        "reset", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.write(rg_args, gfx::Access::TransferWrite);
          b.write(rg_used, gfx::Access::TransferWrite);
          b.write(rg_count, gfx::Access::TransferWrite);
          b.write(rg_mask, gfx::Access::TransferWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdFillBuffer(cb, args.buffer, 0, sizeof(u32), 0);
          vkCmdFillBuffer(cb, args.buffer, sizeof(u32), sizeof(u32) * 2, 1);
          vkCmdFillBuffer(cb, used.buffer, 0, VK_WHOLE_SIZE, 0);
          vkCmdFillBuffer(cb, request_count.buffer, 0, VK_WHOLE_SIZE, 0);
          vkCmdFillBuffer(cb, request_mask.buffer, 0, VK_WHOLE_SIZE, 0);
        });
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(rg_args, gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          b.write(rg_used, gfx::Access::ComputeWrite);
          b.write(rg_requests, gfx::Access::ComputeWrite);
          b.write(rg_count, gfx::Access::ComputeReadWrite);
          b.write(rg_mask, gfx::Access::ComputeReadWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
          vkCmdPushConstants(cb, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                             &params_address);
          vkCmdDispatch(cb, gfx::cull_group_count(cluster_count), 1, 1);
        });
    graph.add_pass(
        "readback", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_args, gfx::Access::TransferRead);
          b.read(rg_visible, gfx::Access::TransferRead);
          b.read(rg_requests, gfx::Access::TransferRead);
          b.read(rg_count, gfx::Access::TransferRead);
          b.write(rg_args_host, gfx::Access::TransferWrite);
          b.write(rg_visible_host, gfx::Access::TransferWrite);
          b.write(rg_requests_host, gfx::Access::TransferWrite);
        },
        [&](VkCommandBuffer cb, gfx::RenderGraph&) {
          const VkBufferCopy a{0, 0, sizeof(u32) * 4};
          vkCmdCopyBuffer(cb, args.buffer, args_host.buffer, 1, &a);
          const VkBufferCopy v{0, 0, visible_bytes};
          vkCmdCopyBuffer(cb, visible.buffer, visible_host.buffer, 1, &v);
          const VkBufferCopy c{0, 0, sizeof(u32)};
          vkCmdCopyBuffer(cb, request_count.buffer, requests_host.buffer, 1, &c);
          const VkBufferCopy r{0, sizeof(u32), request_bytes};
          vkCmdCopyBuffer(cb, requests.buffer, requests_host.buffer, 1, &r);
        });
    VkCommandBuffer commands = frames.begin_frame();
    REQUIRE(graph.compile(&error));
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));

    const u32 count = static_cast<const u32*>(args_host.mapped)[0];
    const auto* entries = static_cast<const u32*>(visible_host.mapped);
    cut_out.clear();
    for (u32 i = 0; i < count; ++i)
      cut_out.push_back(entries[i * 2 + 1]);
    std::sort(cut_out.begin(), cut_out.end());
    const auto* head = static_cast<const u32*>(requests_host.mapped);
    const u32 written = head[0] < max_requests ? head[0] : max_requests;
    const auto* list = reinterpret_cast<const geometry::PageRequest*>(head + 1);
    requests_out.clear();
    for (u32 i = 0; i < written; ++i)
      requests_out.push_back(list[i].page);
    std::sort(requests_out.begin(), requests_out.end());
  };

  Vector<u32> gpu_cut;
  Vector<u32> gpu_requests;
  Vector<u32> cpu_cut;
  Vector<u32> cpu_requests;

  SUBCASE("everything resident is today's cut, cluster for cluster") {
    geometry::PageResidency all;
    all.resident.assign(page_count, u8{1});
    run(all, gpu_cut, gpu_requests);
    cpu_cut.clear();
    cpu_requests.clear();
    geometry::select_lod_streaming(lod, view, pages, all, cpu_cut, cpu_requests);
    std::sort(cpu_cut.begin(), cpu_cut.end());
    CHECK(gpu_requests.empty());  // nothing can be missing
    REQUIRE(gpu_cut.size() == cpu_cut.size());
    CHECK(std::equal(gpu_cut.begin(), gpu_cut.end(), cpu_cut.begin()));
    // And the streaming rule with everything resident is the plain rule, which is what makes
    // "streaming changes no picture when the budget is the whole scene" true.
    Vector<u32> plain;
    geometry::select_lod(lod, view, plain);
    std::sort(plain.begin(), plain.end());
    REQUIRE(plain.size() == cpu_cut.size());
    CHECK(std::equal(plain.begin(), plain.end(), cpu_cut.begin()));
    MESSAGE("cut " << gpu_cut.size() << " of " << cluster_count << " clusters");
  }

  SUBCASE("the pinned pages alone still draw the surface") {
    geometry::PageResidency roots;
    roots.resident.assign(page_count, u8{0});
    for (u32 p = 0; p < page_count; ++p)
      roots.resident[p] = (pages.pages[p].flags & geometry::k_page_root) != 0 ? u8{1} : u8{0};
    run(roots, gpu_cut, gpu_requests);
    cpu_cut.clear();
    cpu_requests.clear();
    geometry::select_lod_streaming(lod, view, pages, roots, cpu_cut, cpu_requests);
    std::sort(cpu_cut.begin(), cpu_cut.end());
    REQUIRE(gpu_cut.size() == cpu_cut.size());
    CHECK(std::equal(gpu_cut.begin(), gpu_cut.end(), cpu_cut.begin()));
    REQUIRE(gpu_requests.size() == cpu_requests.size());
    CHECK(std::equal(gpu_requests.begin(), gpu_requests.end(), cpu_requests.begin()));
    CHECK(gpu_cut.size() > 0);
    CHECK(gpu_requests.size() > 0);  // it wants to refine and cannot: that is the feedback
  }

  SUBCASE("random ancestor-closed residencies, cut and requests both") {
    Rng rng;
    for (u32 trial = 0; trial < 12; ++trial) {
      geometry::PageResidency residency_set;
      random_residency(pages, rng, 1 + trial % (page_count > 1 ? page_count - 1 : 1),
                       residency_set);
      run(residency_set, gpu_cut, gpu_requests);
      cpu_cut.clear();
      cpu_requests.clear();
      geometry::select_lod_streaming(lod, view, pages, residency_set, cpu_cut, cpu_requests);
      std::sort(cpu_cut.begin(), cpu_cut.end());
      REQUIRE_MESSAGE(gpu_cut.size() == cpu_cut.size(), "trial " << trial);
      CHECK(std::equal(gpu_cut.begin(), gpu_cut.end(), cpu_cut.begin()));
      REQUIRE_MESSAGE(gpu_requests.size() == cpu_requests.size(), "trial " << trial);
      CHECK(std::equal(gpu_requests.begin(), gpu_requests.end(), cpu_requests.begin()));
    }
  }

  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, cull_module);
  graph.reset();
  frames.destroy();
  gfx::destroy_buffer(device, requests_host);
  gfx::destroy_buffer(device, args_host);
  gfx::destroy_buffer(device, visible_host);
  gfx::destroy_buffer(device, params);
  gfx::destroy_buffer(device, args);
  gfx::destroy_buffer(device, visible);
  gfx::destroy_buffer(device, stream_block);
  gfx::destroy_buffer(device, request_mask);
  gfx::destroy_buffer(device, request_count);
  gfx::destroy_buffer(device, requests);
  gfx::destroy_buffer(device, used);
  gfx::destroy_buffer(device, residency);
  gfx::destroy_buffer(device, children);
  gfx::destroy_buffer(device, page_of_cluster);
  gfx::destroy_buffer(device, page_table);
  gfx::destroy_buffer(device, lods);
  gfx::destroy_buffer(device, clusters);
  scene.destroy(device);
  device.destroy();
}
