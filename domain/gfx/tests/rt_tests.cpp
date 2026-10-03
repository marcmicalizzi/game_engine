// Ray-traced primary visibility against the rasterizer (docs/plan/04-renderer.md §4.4; the
// image-match half of experiment E2 on the first-class path). The LOD cut of a terrain is drawn
// through the vertex path into the visibility buffer and traced with a ray per pixel against a
// bottom-level structure with one geometry per cut cluster under a one-instance top-level
// structure; the two 64-bit buffers must agree on coverage, on the triangle under nearly every
// covered pixel, and on depth. Skips without VK_KHR_ray_query or 64-bit buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/ray_visibility.spv.h>
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

f32 depth_of(u64 word) {
  const u32 bits = static_cast<u32>(word >> 32);
  f32 depth = 0.0f;
  std::memcpy(&depth, &bits, 4);
  return depth;
}

}  // namespace

TEST_CASE("ray query: primary visibility matches the rasterized LOD cut") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer, gfx_test::Need::RayQuery})) {
    return;
  }

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(65, 10.0f, positions, indices);  // 8,192 triangles
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  // The camera and the CPU-selected cut it sees, as in the cull test.
  constexpr u32 k_w = 320;
  constexpr u32 k_h = 240;
  const Vec3 eye{0.0f, 9.0f, 24.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(60.0f);
  const Mat4 projection = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear);
  const Mat4 eye_view = look_at(eye, Vec3{}, Vec3{0, 1, 0});
  const Mat4 view_proj = projection * eye_view;
  const Frustum frustum = frustum_from_view_proj(view_proj);
  geometry::LodView view;
  view.camera = eye;
  view.znear = znear;
  view.proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;
  view.threshold_px = 1.0f;
  Vector<u32> cut;
  for (u32 i = 0; i < cluster_count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    if (frustum_contains_sphere(frustum, c.center, c.radius) &&
        geometry::lod_selects(lod.lod[i], view)) {
      cut.push_back(i);
    }
  }
  REQUIRE(cut.size() > 4);

  // Geometry on the GPU: the cluster format for the rasterizer, plus 16-bit indices per cut
  // cluster for the acceleration structure builder.
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource clusters;
  gfx::BufferResource vertices;
  gfx::BufferResource triangles;
  gfx::BufferResource cut_buffer;
  gfx::BufferResource indices16;
  gfx_test::SingleInstance scene_data;
  Vector<u16> expanded;
  Vector<u32> expanded_offset;  // element offset of each cut cluster's indices
  for (const u32 c : cut) {
    const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
    expanded_offset.push_back(expanded.size());
    gfx::expand_packed_triangles(
        std::span<const u32>(lod.mesh.triangles.data() + desc.triangle_offset, desc.triangle_count),
        expanded);
  }
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.vertices.data(),
                             lod.mesh.vertices.size() * sizeof(Vec3),
                             k_storage | gfx::k_build_input_usage, vertices, &error));
  // The rasterizer reads the 16-bit grid; the acceleration structure builder reads the floats.
  REQUIRE(scene_data.create(device, lod.mesh, cluster_count, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  // The cut as a visible list: {instance, cluster} per entry, in the order the bottom-level
  // structure's geometries were built, so a hit's GeometryIndex is the entry the rasterizer's
  // visibility id names.
  Vector<u32> cut_entries;
  for (const u32 c : cut) {
    cut_entries.push_back(0);  // the one instance
    cut_entries.push_back(c);
  }
  REQUIRE(gfx::upload_buffer(device, cut_entries.data(), cut_entries.size() * sizeof(u32),
                             k_storage, cut_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, expanded.data(), expanded.size() * sizeof(u16),
                             gfx::k_build_input_usage, indices16, &error));

  Vector<gfx::ClusterGeometry> geometries;
  for (u32 k = 0; k < cut.size(); ++k) {
    const geometry::ClusterDesc& desc = lod.mesh.clusters[cut[k]];
    gfx::ClusterGeometry g;
    g.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
    g.vertex_count = desc.vertex_count;
    g.indices = indices16.address + u64{expanded_offset[k]} * sizeof(u16);
    g.triangle_count = desc.triangle_count;
    geometries.push_back(g);
  }
  gfx::AccelerationStructure blas;
  gfx::AccelerationStructure tlas;
  REQUIRE_MESSAGE(gfx::create_blas(device, geometries, gfx::k_build_fast_trace, blas, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas, &error), error);
  CHECK(blas.geometry_count == cut.size());
  MESSAGE("blas " << blas.buffer.size << " bytes for " << cut.size() << " clusters, scratch "
                  << blas.build_scratch_bytes << "; tlas " << tlas.buffer.size << " bytes");
  gfx::BufferResource scratch;
  gfx::BufferResource instances;
  REQUIRE(gfx::create_scratch(device,
                              blas.build_scratch_bytes > tlas.build_scratch_bytes
                                  ? blas.build_scratch_bytes
                                  : tlas.build_scratch_bytes,
                              scratch, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                             instances, &error));
  gfx::TlasInstance instance;
  instance.blas = blas.address;
  gfx::write_instances(std::span<const gfx::TlasInstance>(&instance, 1), instances.mapped);
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        gfx::build_blas(cb, blas, geometries, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                        gfx::MemoryAccess::AccelerationStructureRead);
        gfx::build_tlas(cb, tlas, instances.address, 1, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                        gfx::MemoryAccess::AccelerationStructureRead);
      },
      &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  REQUIRE(bindless.has_acceleration_structures());
  const u32 scene = bindless.add_acceleration_structure(tlas.handle);
  REQUIRE(scene != gfx::BindlessSet::k_invalid_slot);
  gfx::GpuTimer timer;
  REQUIRE(timer.create(device, 2, 8, &error));

  gfx::ShaderModuleHandle vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  gfx::ShaderModuleHandle trace_module = gfx::create_shader_module(
      device, shaders::k_ray_visibility_spirv, shaders::k_ray_visibility_spirv_size, &error);
  REQUIRE(vertex_module.valid());
  REQUIRE(trace_module.valid());
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  gfx::PipelineHandle vertex_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
  const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
  gfx::ComputePipeline trace_pipeline;
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, trace_module, "trace_main",
                                   std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
                                   sizeof(u64), trace_pipeline, &error),
      error);

  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const gfx::BufferUsage k_vis =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  gfx::BufferResource vis_raster;
  gfx::BufferResource vis_rt;
  gfx::BufferResource params;
  gfx::BufferResource host;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_raster, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, vis_rt, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, params,
                             &error));
  REQUIRE(
      gfx::create_buffer(device, vis_bytes * 2, gfx::BufferUsage::TransferDst, true, host, &error));

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene_data.meshes.address;
  draw.instances = scene_data.instances.address;
  draw.triangles = triangles.address;
  draw.triangles_per_cluster = triangles_per_cluster;
  draw.visible = cut_buffer.address;
  draw.visibility = vis_raster.address;
  draw.width = k_w;
  draw.height = k_h;
  gfx::RayVisibilityParams ray{};
  ray.view_proj = view_proj;
  ray.clip_to_ray = gfx::clip_to_ray(projection, eye_view);
  ray.camera = Vec4{eye, 0.0f};
  ray.output = vis_rt.address;
  ray.instance_base = 0;  // GeometryIndex is already the entry of the visible list
  ray.width = k_w;
  ray.height = k_h;
  ray.scene = scene;
  // The entry's pair is the id both pictures write (gfx.md, "The tie rule").
  ray.visible = draw.visible;
  ray.instances = draw.instances;
  ray.meshes = draw.mesh;
  std::memcpy(params.mapped, &ray, sizeof(ray));
  const u64 params_address = params.address;

  gfx::RenderGraph graph(device);
  const gfx::RgBuffer rg_raster = graph.import_buffer("vis raster", vis_raster);
  const gfx::RgBuffer rg_rt = graph.import_buffer("vis rt", vis_rt);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.write(rg_raster, gfx::Access::TransferWrite);
        b.write(rg_rt, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        cb.fill_buffer(vis_raster.buffer, 0, gfx::k_whole_size, 0);
        cb.fill_buffer(vis_rt.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "raster", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_raster, gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        timer.begin(cb, "raster");
        cb.bind_pipeline(gfx::BindPoint::Graphics, vertex_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(draw),
                          &draw);
        cb.draw(triangles_per_cluster * 3, cut.size(), 0, 0);
        timer.end(cb);
      });
  graph.add_pass(
      "trace", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(rg_rt, gfx::Access::ComputeWrite); },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        timer.begin(cb, "trace");
        cb.bind_pipeline(gfx::BindPoint::Compute, trace_pipeline.pipeline);
        bindless.bind(cb, gfx::BindPoint::Compute);
        cb.push_constants(trace_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                          &params_address);
        cb.dispatch(gfx::ray_visibility_group_count(k_w), gfx::ray_visibility_group_count(k_h), 1);
        timer.end(cb);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        b.read(rg_raster, gfx::Access::TransferRead);
        b.read(rg_rt, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        const gfx::BufferCopy raster_copy{0, 0, vis_bytes};
        const gfx::BufferCopy rt_copy{0, vis_bytes, vis_bytes};
        cb.copy_buffer(vis_raster.buffer, host.buffer, raster_copy);
        cb.copy_buffer(vis_rt.buffer, host.buffer, rt_copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);

  // Two frames so the timer has a completed frame to report; the picture is the same each time.
  for (u32 frame = 0; frame < 2; ++frame) {
    gfx::CommandList commands = frames.begin_frame();
    timer.begin_frame(commands, frames.slot());
    graph.execute(commands);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  gfx::CommandList commands = frames.begin_frame();
  timer.begin_frame(commands, frames.slot());
  REQUIRE(frames.wait(frames.end_frame()));
  MESSAGE("gpu ms: raster " << timer.ms("raster") << ", trace " << timer.ms("trace") << " ("
                            << cut.size() << " clusters at " << k_w << "x" << k_h << ")");

  const auto* raster_out = static_cast<const u64*>(host.mapped);
  const u64* rt_out = raster_out + u64{k_w} * k_h;
  u32 covered = 0;
  u32 covered_rt = 0;
  u32 coverage_mismatch = 0;
  u32 both = 0;
  u32 id_mismatch = 0;
  f32 max_depth_diff = 0.0f;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool in_raster = raster_out[i] != 0;
    const bool in_rt = rt_out[i] != 0;
    covered += in_raster;
    covered_rt += in_rt;
    if (in_raster != in_rt) ++coverage_mismatch;
    if (in_raster && in_rt) {
      ++both;
      if (static_cast<u32>(raster_out[i]) != static_cast<u32>(rt_out[i])) {
        ++id_mismatch;
      } else {
        const f32 diff = std::fabs(depth_of(raster_out[i]) - depth_of(rt_out[i]));
        max_depth_diff = diff > max_depth_diff ? diff : max_depth_diff;
      }
    }
  }
  CHECK(covered > k_w * k_h / 8);
  CHECK(coverage_mismatch * 200 <= covered);  // silhouette and edge-rule pixels only
  CHECK(id_mismatch * 100 <= both);           // shared-edge pixels choose either triangle
  CHECK(max_depth_diff < 2e-3f);              // interpolated vs recomputed reversed-Z depth
  MESSAGE("covered raster " << covered << ", rt " << covered_rt << ", coverage mismatch "
                            << coverage_mismatch << ", id mismatch " << id_mismatch << " of "
                            << both << ", max depth diff " << max_depth_diff);

  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, trace_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, trace_module);
  timer.destroy();
  bindless.destroy();
  frames.destroy();
  gfx::destroy_acceleration_structure(device, blas);
  gfx::destroy_acceleration_structure(device, tlas);
  scene_data.destroy(device);
  for (gfx::BufferResource* b : {&scratch, &instances, &vis_raster, &vis_rt, &params, &host,
                                 &clusters, &vertices, &triangles, &cut_buffer, &indices16}) {
    gfx::destroy_buffer(device, *b);
  }
  device.destroy();
}
