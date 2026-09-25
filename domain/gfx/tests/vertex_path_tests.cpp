// The baseline tier: the vertex-shader cluster path must fill the visibility buffer exactly as
// the mesh-shader path does from the same clusters. Two draws: the **capacity** draw
// (cluster_vertex.slang: three vertices per triangle of every cluster's capacity, directly and
// through the cull pass counting into vkCmdDrawIndirect's instance count), which is what the
// renderer draws with culling off or without geometryShader and fullDrawIndexUint32; and the
// **indexed** draw
// (cluster_vertex_indexed.slang: exactly the cut's triangles, indices sharing a cluster's
// vertices), which is its culled draw everywhere else. The mesh comparison needs mesh shaders, and
// where there are none the indexed draw is held to the software rasterizer's buffer instead; the
// vertex path itself runs on any device with 64-bit buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/cluster_vertex_indexed.spv.h>
#include <shaders/vertex_expand.spv.h>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("vertex path: the baseline tier fills the visibility buffer like the mesh path") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  const bool have_mesh =
      gfx_test::part(device, "the comparison with the mesh path", {gfx_test::Need::MeshShader});

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterLodMesh lod;
  REQUIRE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error));
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 leaf_count = lod.level_cluster_counts[0];
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(scene.create(device, lod.mesh, leaf_count, &error));  // the cull candidates: the leaves
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));

  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                         look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  gfx::BufferResource vis_mesh;
  gfx::BufferResource vis_vertex;
  gfx::BufferResource vis_indirect;
  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource host;
  const gfx::BufferUsage k_vis =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_mesh, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_vertex, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_indirect, &error));
  const u64 visible_bytes = u64{cluster_count} * 2 * sizeof(u32);  // uint2 per entry
  REQUIRE(gfx::create_buffer(device, visible_bytes, k_address | gfx::BufferUsage::TransferSrc,
                             false, visible, &error));
  REQUIRE(gfx::create_buffer(device, 16,
                             k_address | gfx::BufferUsage::Indirect |
                                 gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc,
                             false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams), k_address, true, params, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * 3 + 16 + visible_bytes,
                             gfx::BufferUsage::TransferDst, true, host, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ShaderModuleHandle vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  gfx::ShaderModuleHandle cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  REQUIRE(vertex_module.valid());
  REQUIRE(cull_module.valid());
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  gfx::PipelineHandle vertex_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
  gfx::ComputePipeline cull_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                       cull_pipeline, &error));
  gfx::ShaderModuleHandle mesh_module = {};
  gfx::PipelineHandle mesh_pipeline = {};
  if (have_mesh) {
    mesh_module = gfx::create_shader_module(device, shaders::k_cluster_mesh_spirv,
                                            shaders::k_cluster_mesh_spirv_size, &error);
    REQUIRE(mesh_module.valid());
    gfx::MeshPipelineDesc mesh_desc;
    mesh_desc.mesh = mesh_module;
    mesh_desc.fragment = mesh_module;
    mesh_desc.fragment_entry = "fs_visibility";
    mesh_desc.layout = bindless.pipeline_layout();
    REQUIRE(gfx::create_mesh_pipeline(device, mesh_desc, mesh_pipeline, &error));
  }

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.width = k_w;
  draw.height = k_h;
  gfx::ClusterDrawParams draw_mesh = draw;
  draw_mesh.visibility = vis_mesh.address;
  gfx::ClusterDrawParams draw_vertex = draw;
  draw_vertex.visibility = vis_vertex.address;
  gfx::ClusterDrawParams draw_indirect = draw;
  draw_indirect.visibility = vis_indirect.address;
  draw_indirect.visible = visible.address;

  // Cull: frustum only, no LOD, counting into the instance count of a vkCmdDrawIndirect block;
  // the survivors are the frustum-visible leaves plus every coarser level, so the LOD filter is
  // replaced by restricting the candidate set to the leaves, which the scene's one mesh does by
  // claiming only the first leaf_count clusters (leaves come first).
  gfx::CullParams cull{};
  gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
  cull.view_proj = view_proj;
  cull.camera = Vec4{eye, znear};
  cull.lod = Vec4{1.0f, 1.0f, 0.0f, 1.0f};  // LOD selection off, frustum on
  cull.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
  cull.cluster_count = cluster_count;
  cull.count_index = 1;
  cull.clusters = clusters.address;
  cull.lods = lods.address;
  cull.visible = visible.address;
  cull.draw_args = args.address;
  cull.instances = scene.instances.address;
  cull.meshes = scene.meshes.address;
  cull.instance_count = 1;
  cull.pair_count = scene.pair_count();
  std::memcpy(params.mapped, &cull, sizeof(cull));
  const u64 params_address = params.address;

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_mesh = graph.import_buffer("vis_mesh", vis_mesh);
  const gfx::RgBuffer rg_vertex = graph.import_buffer("vis_vertex", vis_vertex);
  const gfx::RgBuffer rg_indirect = graph.import_buffer("vis_indirect", vis_indirect);
  const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible);
  const gfx::RgBuffer rg_args = graph.import_buffer("args", args);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_mesh, gfx::Access::TransferWrite);
        b.write(rg_vertex, gfx::Access::TransferWrite);
        b.write(rg_indirect, gfx::Access::TransferWrite);
        b.write(rg_args, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis_mesh.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_vertex.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_indirect.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(args.buffer, 0, 4, triangles_per_cluster * 3);  // vertexCount
        cb.fill_buffer(args.buffer, 4, 12, 0);                         // instanceCount, first*
      });
  if (have_mesh) {
    graph.add_pass(
        "mesh", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_mesh, gfx::Access::FragmentReadWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, mesh_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(draw_mesh),
                            &draw_mesh);
          cb.draw_mesh_tasks(leaf_count, 1, 1);
        });
  }
  graph.add_pass(
      "vertex", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_vertex, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Graphics, vertex_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(draw_vertex),
                          &draw_vertex);
        cb.draw(triangles_per_cluster * 3, leaf_count, 0, 0);
      });
  graph.add_pass(
      "cull", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.write(rg_args, gfx::Access::ComputeReadWrite);
        b.write(rg_visible, gfx::Access::ComputeWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Compute, cull_pipeline.pipeline);
        cb.push_constants(cull_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                          &params_address);
        cb.dispatch(gfx::cull_group_count(leaf_count), 1, 1);
      });
  graph.add_pass(
      "vertex indirect", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_indirect, gfx::Access::FragmentReadWrite);
        b.read(rg_args, gfx::Access::IndirectRead);
        b.read(rg_visible, gfx::Access::VertexRead);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Graphics, vertex_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                          sizeof(draw_indirect), &draw_indirect);
        cb.draw_indirect(args.buffer, 0, 1, sizeof(u32) * 4);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_mesh, gfx::Access::TransferRead);
        b.read(rg_vertex, gfx::Access::TransferRead);
        b.read(rg_indirect, gfx::Access::TransferRead);
        b.read(rg_args, gfx::Access::TransferRead);
        b.read(rg_visible, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        const gfx::BufferCopy mesh_copy{0, 0, vis_bytes};
        const gfx::BufferCopy vertex_copy{0, vis_bytes, vis_bytes};
        const gfx::BufferCopy indirect_copy{0, vis_bytes * 2, vis_bytes};
        const gfx::BufferCopy args_copy{0, vis_bytes * 3, 16};
        const gfx::BufferCopy visible_copy{0, vis_bytes * 3 + 16, visible_bytes};
        cb.copy_buffer(vis_mesh.buffer, host.buffer, mesh_copy);
        cb.copy_buffer(vis_vertex.buffer, host.buffer, vertex_copy);
        cb.copy_buffer(vis_indirect.buffer, host.buffer, indirect_copy);
        cb.copy_buffer(args.buffer, host.buffer, args_copy);
        cb.copy_buffer(visible.buffer, host.buffer, visible_copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* mesh_out = static_cast<const u64*>(host.mapped);
  const u64* vertex_out = mesh_out + k_w * k_h;
  const u64* indirect_out = vertex_out + k_w * k_h;
  const auto* args_out = reinterpret_cast<const u32*>(indirect_out + k_w * k_h);
  CHECK(args_out[0] == triangles_per_cluster * 3);
  CHECK(args_out[1] > 0);
  CHECK(args_out[1] <= leaf_count);
  MESSAGE("indirect: " << args_out[1] << " of " << leaf_count << " leaf clusters in the frustum");

  // A visibility id names the scene's pair, not an entry of the visible list (gfx.md, "The tie
  // rule"), and for the one identity instance of one mesh the pair is the cluster, so the indirect
  // draw and the direct draw write the same word for the same triangle at the same depth.
  auto surface_of = [](u64 word, u64) { return word; };
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 id_mismatch = 0;
  u32 indirect_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool in_vertex = vertex_out[i] != 0;
    covered += in_vertex;
    if (have_mesh) {
      const bool in_mesh = mesh_out[i] != 0;
      if (in_mesh != in_vertex) ++coverage_mismatch;
      if (in_mesh && in_vertex && static_cast<u32>(mesh_out[i]) != static_cast<u32>(vertex_out[i]))
        ++id_mismatch;
    }
    // The frustum-culled indirect draw must draw exactly what the direct draw drew.
    const bool in_indirect = indirect_out[i] != 0;
    if (in_indirect != in_vertex ||
        (in_indirect && surface_of(indirect_out[i], 1) != surface_of(vertex_out[i], 0))) {
      ++indirect_mismatch;
    }
  }
  CHECK(covered > k_w * k_h / 8);
  // Exact, since the id became the scene's pair (gfx.md, "The tie rule"): a depth tie along a
  // shared edge goes to the larger (pair, triangle) in every draw, where it used to follow the
  // order each draw's list happened to be in.
  if (have_mesh) {
    CHECK(coverage_mismatch == 0);
    CHECK(id_mismatch == 0);
  }
  CHECK(indirect_mismatch == 0);
  MESSAGE("covered " << covered << " px; mesh vs vertex coverage mismatch " << coverage_mismatch
                     << ", id mismatch " << id_mismatch << "; indirect mismatch "
                     << indirect_mismatch);

  graph.reset();
  if (mesh_pipeline.valid()) gfx::destroy_pipeline(device, mesh_pipeline);
  if (mesh_module.valid()) gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, cull_module);
  gfx::destroy_shader_module(device, vertex_module);
  bindless.destroy();
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host, &params, &args, &visible, &vis_indirect, &vis_vertex,
                                 &vis_mesh, &lods, &triangles, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}

// What quantization costs the picture. Both rasterizers and the resolve read the same 16-bit
// stream, so comparing them against each other says nothing about the grid; this compares the
// grid against the floats it came from, on the CPU, through the projection the test above uses.
// The terrain is 20 units across, so the step is 20 / 65535 and no vertex may move a pixel.
TEST_CASE("vertex path: quantized positions project within a twentieth of a pixel") {
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  geometry::ClusterLodMesh lod;
  std::string error;
  REQUIRE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error));

  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const Mat4 view_proj =
      perspective_reversed_z(radians(60.0f), static_cast<f32>(k_w) / k_h, znear) *
      look_at(eye, Vec3{}, Vec3{0, 1, 0});
  f32 worst_px = 0.0f;
  u32 projected = 0;
  for (u32 v = 0; v < lod.mesh.vertices.size(); ++v) {
    const Vec4 exact = view_proj * Vec4{lod.mesh.vertices[v], 1.0f};
    const Vec4 grid = view_proj * Vec4{geometry::dequantize_position(lod.mesh, v), 1.0f};
    if (exact.w <= znear || grid.w <= znear) continue;
    ++projected;
    const f32 dx = std::fabs(exact.x / exact.w - grid.x / grid.w) * 0.5f * static_cast<f32>(k_w);
    const f32 dy = std::fabs(exact.y / exact.w - grid.y / grid.w) * 0.5f * static_cast<f32>(k_h);
    worst_px = dx > worst_px ? dx : worst_px;
    worst_px = dy > worst_px ? dy : worst_px;
  }
  CHECK(projected > lod.mesh.vertices.size() / 2);
  CHECK(lod.mesh.quant_scale < 3.1e-4f);  // 20 units over 65535 steps
  CHECK(worst_px < 0.05f);
  MESSAGE("grid step " << lod.mesh.quant_scale << " moves " << projected << " vertices by at most "
                       << worst_px << " px at " << k_w << "x" << k_h);
}

// ---- the indexed draw ---------------------------------------------------------------------------
//
// The renderer's culled draw on the baseline tier (gfx::VertexDrawHeader, docs/subsystems/gfx.md
// "Baseline tier"): the cull pass's `cull_vertex_main` allocates each hardware survivor's triangles
// and writes its record, vertex_expand.slang writes the indices, and cluster_vertex_indexed.slang
// draws the run with one indexed draw plus the capacity-drawn fallback. What this asserts:
//
//   1. the draw is **the cut's own triangles**: the cursor is the survivors' triangle count, fewer
//      than their capacity, and the records partition the index array in allocation order;
//   2. **the picture is the capacity draw's**, surface for surface, which is what the vertex path
//      drew before (and still draws with culling off, or without geometryShader or
//      fullDrawIndexUint32);
//   3. **it agrees with the software rasterizer** over the same visible list, the comparison that
//      holds on a device with no mesh shaders — the TITAN Xp — as well as on any other;
//   4. **it agrees with the mesh path** where there is one;
//   5. **an index array too small for the cut costs time, never a hole**: the same cut with a
//      budget of a third of its triangles draws the same picture, the overflow through the
//      fallback.
//
// Each of the two cuts has its own visible list and header, so the two culls cannot race; their
// visible lists are the same set in whatever order the atomics took, which is why the comparisons
// go through each list to the (cluster, triangle) a pixel's id names.
namespace {

struct IndexedRun {
  gfx::BufferResource visible;  // uint2 per pair
  gfx::BufferResource args;     // {3 x capacity, survivors, 0, 0}: count_index 1
  gfx::BufferResource header;   // gfx::VertexDrawHeader
  gfx::BufferResource records;  // gfx::VertexDrawRecord per pair
  gfx::BufferResource indices;  // three u32 per triangle of `capacity`
  gfx::BufferResource vis;      // the indexed draw's visibility buffer
  gfx::BufferResource params;   // this run's CullParams
  gfx::VertexDrawHeader reset{};

  bool create(const gfx::Device& device, u32 pairs, u32 capacity, u32 triangles_per_cluster,
              u64 vis_bytes, std::string* error) {
    constexpr gfx::BufferUsage k_address =
        gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
    constexpr gfx::BufferUsage k_readable = k_address | gfx::BufferUsage::TransferSrc;
    constexpr gfx::BufferUsage k_args =
        k_readable | gfx::BufferUsage::Indirect | gfx::BufferUsage::TransferDst;
    reset = gfx::vertex_draw_reset(capacity, triangles_per_cluster);
    return gfx::create_buffer(device, u64{pairs} * 8, k_readable, false, visible, error) &&
           gfx::create_buffer(device, gfx::k_draw_args_bytes, k_args, false, args, error) &&
           gfx::create_buffer(device, sizeof(gfx::VertexDrawHeader), k_args, false, header,
                              error) &&
           gfx::create_buffer(device, u64{pairs} * sizeof(gfx::VertexDrawRecord), k_readable, false,
                              records, error) &&
           gfx::create_buffer(device, u64{capacity} * gfx::k_vertex_draw_index_bytes,
                              k_readable | gfx::BufferUsage::Index, false, indices, error) &&
           gfx::create_buffer(device, vis_bytes, k_args, false, vis, error) &&
           gfx::create_buffer(device, sizeof(gfx::CullParams), k_address, true, params, error);
  }

  void destroy(const gfx::Device& device) noexcept {
    for (gfx::BufferResource* b : {&visible, &args, &header, &records, &indices, &vis, &params})
      gfx::destroy_buffer(device, *b);
  }
};

}  // namespace

TEST_CASE("vertex path: the indexed draw draws the cut's own triangles, the same picture") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer, gfx_test::Need::GeometryShader,
                                  gfx_test::Need::FullDrawIndex}))
    return;
  const bool have_mesh =
      gfx_test::part(device, "the comparison with the mesh path", {gfx_test::Need::MeshShader});

  // The terrain of the case above, but drawn at its LOD cut rather than its leaves, so the
  // survivors are clusters of every size the builder makes and not only full ones.
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);
  geometry::ClusterLodMesh lod;
  REQUIRE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error));
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource lods;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(scene.create(device, lod.mesh, cluster_count, &error));  // every cluster a candidate
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));

  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                         look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const u32 pairs = scene.pair_count();

  // Run A has the whole cut's worth of index array; run B a third of it, so most of its
  // survivors overflow and the fallback draws them.
  u32 all_triangles = 0;
  for (const geometry::ClusterDesc& c : lod.mesh.clusters)
    all_triangles += c.triangle_count;
  IndexedRun run_a;
  IndexedRun run_b;
  REQUIRE(run_a.create(device, pairs, all_triangles, triangles_per_cluster, vis_bytes, &error));
  // (sized below once the cut is known: a third of it)
  gfx::BufferResource vis_capacity;  // the capacity draw over run A's visible list
  gfx::BufferResource vis_sw;        // the software rasterizer over run A's visible list
  gfx::BufferResource vis_mesh;      // the mesh path over run A's visible list
  const gfx::BufferUsage k_vis = k_storage | gfx::BufferUsage::ShaderDeviceAddress |
                                 gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_capacity, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_sw, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_mesh, &error));

  // The CPU reference cut, which is what sizes run B: a third of its triangles.
  geometry::LodView view;
  view.camera = eye;
  view.znear = znear;
  view.proj_scale = proj_scale;
  view.threshold_px = 1.0f;
  const Frustum frustum = frustum_from_view_proj(view_proj);
  u32 cut_clusters = 0;
  u32 cut_triangles = 0;
  for (u32 i = 0; i < cluster_count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    if (frustum_contains_sphere(frustum, c.center, c.radius) &&
        geometry::lod_selects(lod.lod[i], view)) {
      ++cut_clusters;
      cut_triangles += c.triangle_count;
    }
  }
  REQUIRE(cut_clusters > 8);
  REQUIRE(run_b.create(device, pairs, cut_triangles / 3, triangles_per_cluster, vis_bytes, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));

  gfx::ShaderModuleHandle cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  gfx::ShaderModuleHandle expand_module = gfx::create_shader_module(
      device, shaders::k_vertex_expand_spirv, shaders::k_vertex_expand_spirv_size, &error);
  gfx::ShaderModuleHandle indexed_module =
      gfx::create_shader_module(device, shaders::k_cluster_vertex_indexed_spirv,
                                shaders::k_cluster_vertex_indexed_spirv_size, &error);
  gfx::ShaderModuleHandle vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  gfx::ShaderModuleHandle sw_module = gfx::create_shader_module(
      device, shaders::k_cluster_sw_raster_spirv, shaders::k_cluster_sw_raster_spirv_size, &error);
  REQUIRE(cull_module.valid());
  REQUIRE(expand_module.valid());
  REQUIRE(indexed_module.valid());
  REQUIRE(vertex_module.valid());
  REQUIRE(sw_module.valid());
  gfx::ComputePipeline cull_pipeline;
  gfx::ComputePipeline expand_pipeline;
  gfx::ComputePipeline sw_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_vertex_main", {},
                                               sizeof(u64), cull_pipeline, &error),
                  error);
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, expand_module, "expand_main", {},
                                   sizeof(gfx::VertexExpandParams), expand_pipeline, &error),
      error);
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, sw_module, "sw_raster_main", {},
                                               sizeof(gfx::ClusterDrawParams), sw_pipeline, &error),
                  error);
  gfx::GraphicsPipelineDesc desc;
  desc.layout = bindless.pipeline_layout();
  desc.vertex = indexed_module;
  desc.vertex_entry = "vs_cluster_indexed";
  desc.fragment = indexed_module;
  desc.fragment_entry = "fs_visibility_indexed";
  gfx::PipelineHandle indexed_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, desc, indexed_pipeline, &error), error);
  desc.vertex_entry = "vs_cluster_fallback";
  desc.fragment = vertex_module;
  desc.fragment_entry = "fs_visibility";
  gfx::PipelineHandle fallback_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, desc, fallback_pipeline, &error), error);
  desc.vertex = vertex_module;
  desc.vertex_entry = "vs_cluster";
  gfx::PipelineHandle capacity_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, desc, capacity_pipeline, &error), error);
  gfx_test::ClusterRaster mesh_raster;
  if (have_mesh) {
    REQUIRE_MESSAGE(mesh_raster.create(device, gfx_test::RasterPath::Mesh,
                                       bindless.pipeline_layout(), triangles_per_cluster, &error),
                    error);
  }

  // The two culls: frustum and LOD, exactly the reference's; the vertex path counts into word 1.
  auto cull_for = [&](IndexedRun& run) {
    gfx::CullParams cull{};
    gfx::set_frustum(cull, frustum);
    cull.view_proj = view_proj;
    cull.camera = Vec4{eye, znear};
    cull.lod = Vec4{proj_scale, view.threshold_px, 1.0f, 1.0f};
    cull.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
    cull.cluster_count = cluster_count;
    cull.count_index = 1;
    cull.clusters = clusters.address;
    cull.lods = lods.address;
    cull.visible = run.visible.address;
    cull.draw_args = run.args.address;
    cull.instances = scene.instances.address;
    cull.meshes = scene.meshes.address;
    cull.instance_count = 1;
    cull.pair_count = pairs;
    cull.vertex_draw = run.header.address;
    cull.vertex_records = run.records.address;
    std::memcpy(run.params.mapped, &cull, sizeof(cull));
  };
  cull_for(run_a);
  cull_for(run_b);
  auto expand_for = [&](const IndexedRun& run) {
    gfx::VertexExpandParams e{};
    e.header = run.header.address;
    e.records = run.records.address;
    e.indices = run.indices.address;
    e.clusters = clusters.address;
    e.triangles = triangles.address;
    return e;
  };
  const gfx::VertexExpandParams expand_a = expand_for(run_a);
  const gfx::VertexExpandParams expand_b = expand_for(run_b);

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.width = k_w;
  draw.height = k_h;
  gfx::ClusterDrawParams draw_a = draw;  // the indexed draws read records
  draw_a.visible = run_a.records.address;
  draw_a.visibility = run_a.vis.address;
  gfx::ClusterDrawParams draw_b = draw;
  draw_b.visible = run_b.records.address;
  draw_b.visibility = run_b.vis.address;
  gfx::ClusterDrawParams draw_capacity = draw;  // the rest read run A's visible list
  draw_capacity.visible = run_a.visible.address;
  draw_capacity.visibility = vis_capacity.address;
  gfx::ClusterDrawParams draw_sw = draw_capacity;
  draw_sw.visibility = vis_sw.address;
  gfx::ClusterDrawParams draw_mesh = draw_capacity;
  draw_mesh.visibility = vis_mesh.address;

  gfx::RenderGraph graph(device);
  struct RgRun {
    gfx::RgBuffer visible, args, header, records, indices, vis;
  };
  auto import_run = [&](IndexedRun& run, const char* name) {
    RgRun r;
    const std::string n(name);
    r.visible = graph.import_buffer((n + " visible").c_str(), run.visible);
    r.args = graph.import_buffer((n + " args").c_str(), run.args);
    r.header = graph.import_buffer((n + " header").c_str(), run.header);
    r.records = graph.import_buffer((n + " records").c_str(), run.records);
    r.indices = graph.import_buffer((n + " indices").c_str(), run.indices);
    r.vis = graph.import_buffer((n + " vis").c_str(), run.vis);
    return r;
  };
  const RgRun rg_a = import_run(run_a, "a");
  const RgRun rg_b = import_run(run_b, "b");
  const gfx::RgBuffer rg_capacity = graph.import_buffer("vis capacity", vis_capacity);
  const gfx::RgBuffer rg_sw = graph.import_buffer("vis sw", vis_sw);
  const gfx::RgBuffer rg_mesh = graph.import_buffer("vis mesh", vis_mesh);

  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const RgRun* r : {&rg_a, &rg_b}) {
          b.write(r->args, gfx::Access::TransferWrite);
          b.write(r->header, gfx::Access::TransferWrite);
          b.write(r->vis, gfx::Access::TransferWrite);
        }
        b.write(rg_capacity, gfx::Access::TransferWrite);
        b.write(rg_sw, gfx::Access::TransferWrite);
        b.write(rg_mesh, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        for (IndexedRun* run : {&run_a, &run_b}) {
          cb.fill_buffer(run->args.buffer, 0, 4, triangles_per_cluster * 3);
          cb.fill_buffer(run->args.buffer, 4, 12, 0);
          cb.update_buffer(run->header.buffer, 0, sizeof(run->reset), &run->reset);
          cb.fill_buffer(run->vis.buffer, 0, gfx::k_whole_size, 0);
        }
        cb.fill_buffer(vis_capacity.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_sw.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_mesh.buffer, 0, gfx::k_whole_size, 0);
      });
  auto add_run = [&](IndexedRun& run, const RgRun& r, const gfx::VertexExpandParams& expand,
                     const gfx::ClusterDrawParams& draw_params) {
    const u64 params_address = run.params.address;
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.write(r.args, gfx::Access::ComputeReadWrite);
          b.write(r.visible, gfx::Access::ComputeWrite);
          b.write(r.header, gfx::Access::ComputeReadWrite);
          b.write(r.records, gfx::Access::ComputeWrite);
        },
        [&, params_address](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, cull_pipeline.pipeline);
          cb.push_constants(cull_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &params_address);
          cb.dispatch(gfx::cull_group_count(pairs), 1, 1);
        });
    graph.add_pass(
        "expand", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.read(r.header,
                 gfx::Access::IndirectRead);  // first: the last use is the state it leaves
          b.write(r.header, gfx::Access::ComputeReadWrite);
          b.read(r.records, gfx::Access::ComputeRead);
          b.write(r.indices, gfx::Access::ComputeWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, expand_pipeline.pipeline);
          cb.push_constants(expand_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(expand),
                            &expand);
          cb.dispatch_indirect(run.header.buffer, gfx::k_vertex_draw_expand_offset);
        });
    graph.add_pass(
        "indexed", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(r.vis, gfx::Access::FragmentReadWrite);
          b.read(r.header, gfx::Access::IndirectRead);
          b.read(r.records, gfx::Access::VertexRead);
          b.read(r.indices, gfx::Access::IndexRead);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, indexed_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                            sizeof(draw_params), &draw_params);
          cb.bind_index_buffer(run.indices.buffer, 0, gfx::IndexType::Uint32);
          cb.draw_indexed_indirect(run.header.buffer, 0, 1, sizeof(gfx::DrawIndexedIndirectArgs));
          cb.bind_pipeline(gfx::BindPoint::Graphics, fallback_pipeline);
          cb.draw_indirect(run.header.buffer, gfx::k_vertex_draw_fallback_offset, 1,
                           sizeof(gfx::DrawIndirectArgs));
        });
  };
  add_run(run_a, rg_a, expand_a, draw_a);
  add_run(run_b, rg_b, expand_b, draw_b);
  // Run A's visible list three more ways: the capacity draw, the software rasterizer (one workgroup
  // per entry, dispatched from the header's count of them) and, where there is one, the mesh path.
  graph.add_pass(
      "capacity", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_capacity, gfx::Access::FragmentReadWrite);
        b.read(rg_a.args, gfx::Access::IndirectRead);
        b.read(rg_a.visible, gfx::Access::VertexRead);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Graphics, capacity_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                          sizeof(draw_capacity), &draw_capacity);
        cb.draw_indirect(run_a.args.buffer, 0, 1, sizeof(u32) * 4);
      });
  graph.add_pass(
      "software", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.write(rg_sw, gfx::Access::ComputeReadWrite);
        b.read(rg_a.header, gfx::Access::IndirectRead);
        b.read(rg_a.visible, gfx::Access::ComputeRead);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.bind_pipeline(gfx::BindPoint::Compute, sw_pipeline.pipeline);
        cb.push_constants(sw_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(draw_sw),
                          &draw_sw);
        cb.dispatch_indirect(run_a.header.buffer, gfx::k_vertex_draw_expand_offset);
      });
  if (have_mesh) {
    graph.add_pass(
        "mesh", gfx::PassKind::Raster,
        [&](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_mesh, gfx::Access::FragmentReadWrite);
          b.read(rg_a.header, gfx::Access::IndirectRead);
          b.read(rg_a.visible, gfx::Access::MeshRead);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          // The expansion's dispatch block is {survivors, 1, 1}: a mesh-task argument block too.
          mesh_raster.draw_indirect(cb, bindless, draw_mesh, run_a.header.buffer,
                                    gfx::k_vertex_draw_expand_offset);
        });
  }
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  // Everything back to the host.
  auto read_back = [&](const gfx::BufferResource& src, u64 bytes) {
    std::vector<u8> out(bytes);
    gfx::BufferResource host;
    REQUIRE(gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferDst, true, host, &error));
    gfx::CommandList cb = frames.begin_frame();
    const gfx::BufferCopy region{0, 0, bytes};
    cb.copy_buffer(src.buffer, host.buffer, region);
    REQUIRE(frames.wait(frames.end_frame()));
    std::memcpy(out.data(), host.mapped, bytes);
    gfx::destroy_buffer(device, host);
    return out;
  };
  auto as_header = [](const std::vector<u8>& bytes) {
    gfx::VertexDrawHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    return h;
  };
  const gfx::VertexDrawHeader header_a =
      as_header(read_back(run_a.header, sizeof(gfx::VertexDrawHeader)));
  const gfx::VertexDrawHeader header_b =
      as_header(read_back(run_b.header, sizeof(gfx::VertexDrawHeader)));
  const std::vector<u8> args_bytes = read_back(run_a.args, gfx::k_draw_args_bytes);
  const std::vector<u8> visible_a_bytes = read_back(run_a.visible, u64{pairs} * 8);
  const std::vector<u8> visible_b_bytes = read_back(run_b.visible, u64{pairs} * 8);
  const std::vector<u8> records_a_bytes =
      read_back(run_a.records, u64{pairs} * sizeof(gfx::VertexDrawRecord));
  const std::vector<u8> records_b_bytes =
      read_back(run_b.records, u64{pairs} * sizeof(gfx::VertexDrawRecord));
  const std::vector<u8> out_a = read_back(run_a.vis, vis_bytes);
  const std::vector<u8> out_b = read_back(run_b.vis, vis_bytes);
  const std::vector<u8> out_capacity = read_back(vis_capacity, vis_bytes);
  const std::vector<u8> out_sw = read_back(vis_sw, vis_bytes);
  const std::vector<u8> out_mesh = read_back(vis_mesh, vis_bytes);
  u32 args_words[4];
  std::memcpy(args_words, args_bytes.data(), sizeof(args_words));
  const auto* visible_a = reinterpret_cast<const u32*>(visible_a_bytes.data());
  const auto* visible_b = reinterpret_cast<const u32*>(visible_b_bytes.data());
  const auto* records_a = reinterpret_cast<const gfx::VertexDrawRecord*>(records_a_bytes.data());
  const auto* records_b = reinterpret_cast<const gfx::VertexDrawRecord*>(records_b_bytes.data());

  // 1. The cut's own triangles, allocated in one partition of the index array.
  const u32 survivors = args_words[1];
  CHECK(survivors == cut_clusters);
  CHECK(header_a.expand_groups == survivors);
  CHECK(header_a.cursor == cut_triangles);
  CHECK(header_a.cursor < survivors * triangles_per_cluster);  // not the capacity draw
  CHECK(header_a.index_count == header_a.cursor * 3);
  CHECK(header_a.overflow == 0);
  CHECK(header_a.fallback_instance_count == 0);
  std::vector<std::pair<u32, u32>> ranges;  // {first, count} per survivor
  for (u32 slot = 0; slot < survivors; ++slot) {
    const gfx::VertexDrawRecord& r = records_a[slot];
    CHECK(r.instance == 0);
    CHECK(r.cluster == visible_a[slot * 2 + 1]);
    CHECK(r.overflow == 0);
    ranges.push_back({r.first_triangle, lod.mesh.clusters[r.cluster].triangle_count});
  }
  std::sort(ranges.begin(), ranges.end());
  u32 next = 0;
  bool partition = true;
  for (const auto& [first, count] : ranges) {
    partition = partition && first == next;
    next = first + count;
  }
  CHECK(partition);
  CHECK(next == header_a.cursor);
  // The index array: each survivor's triangles at its record's start, `slot << 8 | local vertex`.
  const std::vector<u8> indices_a_bytes =
      read_back(run_a.indices, u64{header_a.cursor} * gfx::k_vertex_draw_index_bytes);
  const auto* indices_a = reinterpret_cast<const u32*>(indices_a_bytes.data());
  u32 wrong_indices = 0;
  for (u32 slot = 0; slot < survivors; ++slot) {
    const gfx::VertexDrawRecord& r = records_a[slot];
    const geometry::ClusterDesc& c = lod.mesh.clusters[r.cluster];
    for (u32 k = 0; k < c.triangle_count; ++k) {
      const u32 packed = lod.mesh.triangles[c.triangle_offset + k];
      for (u32 corner = 0; corner < 3; ++corner) {
        const u32 want = (slot << 8) | ((packed >> (8 * corner)) & 0xff);
        if (indices_a[(r.first_triangle + k) * 3 + corner] != want) ++wrong_indices;
      }
    }
  }
  CHECK(wrong_indices == 0);
  MESSAGE("cut: " << survivors << " clusters, " << header_a.cursor << " triangles against "
                  << survivors * triangles_per_cluster << " of capacity ("
                  << 100.0 * header_a.cursor / (survivors * triangles_per_cluster) << "%)");

  // 5's bookkeeping: run B ran out a third of the way through and sent the rest to the fallback.
  CHECK(header_b.cursor == cut_triangles);
  CHECK(header_b.overflow > 0);
  CHECK(header_b.fallback_instance_count > 0);
  CHECK(header_b.index_count <= header_b.index_capacity * 3);
  CHECK(header_b.index_count == header_b.fit_end * 3);
  u32 overflowed = 0;
  for (u32 slot = 0; slot < survivors; ++slot) {
    const gfx::VertexDrawRecord& r = records_b[slot];
    const bool fits =
        r.first_triangle + lod.mesh.clusters[r.cluster].triangle_count <= header_b.index_capacity;
    CHECK(r.overflow == (fits ? 0u : 1u));
    overflowed += r.overflow;
  }
  CHECK(overflowed == header_b.overflow);
  MESSAGE("budget of " << header_b.index_capacity << " triangles: " << overflowed << " of "
                       << survivors << " clusters through the fallback");

  // What a pixel shows: the word itself, whose id names the scene's pair — for one identity
  // instance the cluster — and not an entry of a list (gfx.md, "The tie rule"), so two draws of
  // one surface through different lists write the same word.
  auto surface = [](u64 word, const u32*) -> u64 { return word; };
  const auto* a = reinterpret_cast<const u64*>(out_a.data());
  const auto* b = reinterpret_cast<const u64*>(out_b.data());
  const auto* capacity = reinterpret_cast<const u64*>(out_capacity.data());
  const auto* sw = reinterpret_cast<const u64*>(out_sw.data());
  const auto* mesh = reinterpret_cast<const u64*>(out_mesh.data());
  u32 covered = 0;
  u32 vs_capacity = 0;  // coverage or surface differs
  u32 vs_capacity_coverage = 0;
  u32 vs_budget = 0;
  u32 vs_budget_coverage = 0;
  u32 vs_sw_coverage = 0;
  u32 vs_sw_both = 0;
  u32 vs_sw_same_id = 0;
  u32 vs_mesh_coverage = 0;
  u32 vs_mesh_ids = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool in_a = a[i] != 0;
    covered += in_a;
    if (in_a != (capacity[i] != 0)) ++vs_capacity_coverage;
    if (surface(a[i], visible_a) != surface(capacity[i], visible_a)) ++vs_capacity;
    if (in_a != (b[i] != 0)) ++vs_budget_coverage;
    if (surface(a[i], visible_a) != surface(b[i], visible_b)) ++vs_budget;
    if (in_a != (sw[i] != 0)) ++vs_sw_coverage;
    if (in_a && sw[i] != 0) {
      ++vs_sw_both;
      if (static_cast<u32>(a[i]) == static_cast<u32>(sw[i])) ++vs_sw_same_id;
    }
    if (have_mesh) {
      if (in_a != (mesh[i] != 0)) ++vs_mesh_coverage;
      if (in_a && static_cast<u32>(a[i]) != static_cast<u32>(mesh[i])) ++vs_mesh_ids;
    }
  }
  CHECK(covered > k_w * k_h / 8);
  // 2. The capacity draw's picture: the same coverage and the same words. A depth tie along a
  //    shared edge goes to the larger (pair, triangle) in both draws (gfx.md, "The tie rule").
  CHECK(vs_capacity_coverage == 0);
  CHECK(vs_capacity == 0);
  // 3. The software rasterizer's, to its edge rules (the visibility test's tolerances).
  CHECK(vs_sw_coverage * 100 < covered);
  CHECK(vs_sw_same_id * 100 >= vs_sw_both * 90);
  // 4. The mesh path's, where there is one: the same buffer the vertex path test holds it to.
  if (have_mesh) {
    CHECK(vs_mesh_coverage == 0);
    CHECK(vs_mesh_ids == 0);
  }
  // 5. A budget too small changes nothing in the picture.
  CHECK(vs_budget_coverage == 0);
  CHECK(vs_budget == 0);
  MESSAGE("covered " << covered << " px; against the capacity draw " << vs_capacity_coverage
                     << " coverage / " << vs_capacity << " surface; against software "
                     << vs_sw_coverage << " coverage, " << vs_sw_same_id << " of " << vs_sw_both
                     << " same id; against the mesh path "
                     << (have_mesh ? std::to_string(vs_mesh_coverage) + " coverage / " +
                                         std::to_string(vs_mesh_ids) + " ids"
                                   : std::string("(none)"))
                     << "; with a third of the budget " << vs_budget_coverage << " coverage / "
                     << vs_budget << " surface");

  graph.reset();
  if (have_mesh) mesh_raster.destroy(device);
  for (gfx::PipelineHandle p : {indexed_pipeline, fallback_pipeline, capacity_pipeline})
    gfx::destroy_pipeline(device, p);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_compute_pipeline(device, expand_pipeline);
  gfx::destroy_compute_pipeline(device, sw_pipeline);
  for (gfx::ShaderModuleHandle m :
       {cull_module, expand_module, indexed_module, vertex_module, sw_module})
    gfx::destroy_shader_module(device, m);
  bindless.destroy();
  run_a.destroy(device);
  run_b.destroy(device);
  for (gfx::BufferResource* buffer :
       {&vis_capacity, &vis_sw, &vis_mesh, &lods, &triangles, &clusters})
    gfx::destroy_buffer(device, *buffer);
  scene.destroy(device);
  frames.destroy();
  device.destroy();
}
