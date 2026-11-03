// Experiment E2 on the GPU: the LOD cut of a terrain becomes ray tracing geometry two ways, the
// first-class path (one KHR bottom-level structure with a geometry per cluster) and the cluster
// path (one CLAS per cluster built in bulk, a cluster bottom-level structure over their
// addresses), both under a one-instance top-level structure. The rasterized cut is the
// reference picture; both ray-traced pictures must match it word for word up to shared-edge
// pixels, and the two must match each other. The test reports build and trace times and
// memory for each path, which is the data E2 exists to produce. Skips without cluster
// acceleration structures (NVIDIA RTX only), ray queries, or 64-bit buffer atomics.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/opacity_micromap.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/micromap_probe.spv.h>
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

struct Comparison {
  u32 covered_a = 0;
  u32 coverage_mismatch = 0;
  u32 both = 0;
  u32 id_mismatch = 0;
  f32 max_depth_diff = 0.0f;
};

Comparison compare(const u64* a, const u64* b, u32 count) {
  Comparison c;
  for (u32 i = 0; i < count; ++i) {
    const bool in_a = a[i] != 0;
    const bool in_b = b[i] != 0;
    c.covered_a += in_a;
    if (in_a != in_b) ++c.coverage_mismatch;
    if (in_a && in_b) {
      ++c.both;
      if (static_cast<u32>(a[i]) != static_cast<u32>(b[i])) {
        ++c.id_mismatch;
      } else {
        const f32 diff = std::fabs(depth_of(a[i]) - depth_of(b[i]));
        c.max_depth_diff = diff > c.max_depth_diff ? diff : c.max_depth_diff;
      }
    }
  }
  return c;
}

}  // namespace

// One comparison: a `grid` x `grid` terrain, the cut at `threshold_px` (0 selects every leaf),
// pictures at `k_w` x `k_h`.
void run_comparison(gfx::Device& device, u32 grid, f32 threshold_px, u32 k_w, u32 k_h) {
  std::string error;
  gfx::ClusterAsProperties props;
  REQUIRE(gfx::cluster_as_properties(device, props));
  MESSAGE("cluster AS limits: " << props.max_triangles_per_cluster << " triangles, "
                                << props.max_vertices_per_cluster << " vertices per cluster; "
                                << "alignments cluster " << props.cluster_alignment << ", scratch "
                                << props.scratch_alignment << ", blas "
                                << props.bottom_level_alignment << "; geometry index at most "
                                << props.max_geometry_index);
  REQUIRE(props.max_triangles_per_cluster >= 124);
  REQUIRE(props.max_vertices_per_cluster >= 64);
  // The renderer decides what a scene's chain can name from the feature, before any structure
  // exists (renderer::resolve_settings); it has to be the property the builds are checked against.
  REQUIRE(device.features().cluster_max_geometry_index == props.max_geometry_index);
  REQUIRE(props.max_geometry_index > 0);

  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(grid, 10.0f, positions, indices);
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
  const Vec3 eye{3.0f, 7.0f, 20.0f};
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
  view.threshold_px = threshold_px;
  Vector<u32> cut;
  u32 cut_triangles = 0;
  for (u32 i = 0; i < cluster_count; ++i) {
    const geometry::ClusterDesc& c = lod.mesh.clusters[i];
    if (frustum_contains_sphere(frustum, c.center, c.radius) &&
        geometry::lod_selects(lod.lod[i], view)) {
      cut.push_back(i);
      cut_triangles += c.triangle_count;
    }
  }
  const u32 cut_count = cut.size();
  REQUIRE(cut_count > 8);

  // Geometry on the GPU: positions once, 16-bit indices for the KHR builder, 8-bit ones for CLAS.
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  Vector<u16> indices16;
  Vector<u8> indices8;
  Vector<u32> offset16;
  Vector<u32> offset8;
  for (const u32 c : cut) {
    const geometry::ClusterDesc& desc = lod.mesh.clusters[c];
    const std::span<const u32> packed(lod.mesh.triangles.data() + desc.triangle_offset,
                                      desc.triangle_count);
    offset16.push_back(indices16.size());
    offset8.push_back(indices8.size());
    gfx::expand_packed_triangles(packed, indices16);
    gfx::pack_cluster_indices(packed, indices8);
  }
  gfx::BufferResource clusters;
  gfx::BufferResource vertices;
  gfx::BufferResource triangles;
  gfx::BufferResource cut_buffer;
  gfx::BufferResource index16_buffer;
  gfx::BufferResource index8_buffer;
  gfx_test::SingleInstance scene;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.vertices.data(),
                             lod.mesh.vertices.size() * sizeof(Vec3),
                             k_storage | gfx::k_build_input_usage, vertices, &error));
  // The rasterizer reads the 16-bit grid; the cluster structure builds read the floats.
  REQUIRE(scene.create(device, lod.mesh, cluster_count, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  // The cut as a visible list, {instance, cluster} per entry: the entry index is what the
  // visibility id carries, and it is the geometry index both ray paths report.
  Vector<u32> cut_entries;
  for (const u32 c : cut) {
    cut_entries.push_back(0);  // the one instance
    cut_entries.push_back(c);
  }
  REQUIRE(gfx::upload_buffer(device, cut_entries.data(), cut_entries.size() * sizeof(u32),
                             k_storage, cut_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, indices16.data(), indices16.size() * sizeof(u16),
                             gfx::k_build_input_usage, index16_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, indices8.data(), indices8.size(), gfx::k_build_input_usage,
                             index8_buffer, &error));

  // The KHR path.
  Vector<gfx::ClusterGeometry> geometries;
  Vector<gfx::ClusterBuildInput> inputs;
  for (u32 k = 0; k < cut_count; ++k) {
    const geometry::ClusterDesc& desc = lod.mesh.clusters[cut[k]];
    gfx::ClusterGeometry g;
    g.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
    g.vertex_count = desc.vertex_count;
    g.indices = index16_buffer.address + u64{offset16[k]} * sizeof(u16);
    g.triangle_count = desc.triangle_count;
    geometries.push_back(g);
    gfx::ClusterBuildInput in;
    in.cluster_id = k;  // the entry of the visible list, which is what a visibility id names
    in.triangle_count = desc.triangle_count;
    in.vertex_count = desc.vertex_count;
    in.vertices = g.vertices;
    in.indices = index8_buffer.address + offset8[k];
    inputs.push_back(in);
  }
  gfx::AccelerationStructure khr_blas;
  gfx::AccelerationStructure khr_tlas;
  gfx::AccelerationStructure clas_tlas;
  REQUIRE_MESSAGE(gfx::create_blas(device, geometries, gfx::k_build_fast_trace, khr_blas, &error),
                  error);
  REQUIRE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, khr_tlas, &error));
  REQUIRE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, clas_tlas, &error));

  // The cluster path.
  gfx::ClusterSetLimits limits;
  limits.max_clusters = cut_count;
  limits.max_triangles_per_cluster = triangles_per_cluster;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  limits.max_geometry_index = cut_count - 1;
  gfx::ClusterSet set;
  gfx::ClusterBlas cluster_blas;
  REQUIRE_MESSAGE(gfx::create_cluster_set(device, limits, set, &error), error);
  REQUIRE_MESSAGE(gfx::create_cluster_blas(device, cut_count, cluster_blas, &error), error);
  gfx::BufferResource records;
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * cut_count,
                             gfx::k_build_input_usage, true, records, &error));
  gfx::write_cluster_build_records(inputs, records.mapped);

  u64 scratch_bytes = khr_blas.build_scratch_bytes;
  for (const u64 bytes : {khr_tlas.build_scratch_bytes, clas_tlas.build_scratch_bytes,
                          set.build_scratch_bytes, cluster_blas.build_scratch_bytes}) {
    scratch_bytes = bytes > scratch_bytes ? bytes : scratch_bytes;
  }
  gfx::BufferResource scratch;
  REQUIRE(gfx::create_scratch(device, scratch_bytes, scratch, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 1, &error));  // one slot: every begin_frame reads the last timings
  gfx::GpuTimer timer;
  REQUIRE(timer.create(device, 1, 8, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));

  // Frame A: the bottom-level builds, timed.
  {
    gfx::CommandList cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    timer.begin(cb, "khr blas");
    gfx::build_blas(cb, khr_blas, geometries, gfx::k_build_fast_trace, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                    gfx::MemoryAccess::AccelerationStructureRead |
                                        gfx::MemoryAccess::AccelerationStructureWrite);
    timer.begin(cb, "clas");
    gfx::build_cluster_set(cb, set, records.address, 0, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                    gfx::MemoryAccess::AccelerationStructureRead |
                                        gfx::MemoryAccess::AccelerationStructureWrite);
    timer.begin(cb, "cluster blas");
    gfx::build_cluster_blas(cb, cluster_blas, set.addresses.address, cut_count, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AllCommands,
                                    gfx::MemoryAccess::MemoryRead);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  const gfx::DeviceAddress cluster_blas_address = cluster_blas.address;
  REQUIRE(cluster_blas_address != 0);
  u64 clas_bytes = 0;
  const auto* clas_sizes = static_cast<const u32*>(set.sizes.mapped);
  const auto* clas_addresses = static_cast<const u64*>(set.addresses.mapped);
  for (u32 k = 0; k < cut_count; ++k) {
    clas_bytes += clas_sizes[k];
    CHECK(clas_addresses[k] != 0);
  }

  // Instances and top-level structures, one per path.
  gfx::BufferResource khr_instances;
  gfx::BufferResource clas_instances;
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                             khr_instances, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                             clas_instances, &error));
  gfx::TlasInstance khr_instance;
  khr_instance.blas = khr_blas.address;
  gfx::TlasInstance clas_instance;
  clas_instance.blas = cluster_blas_address;
  gfx::write_instances(std::span<const gfx::TlasInstance>(&khr_instance, 1), khr_instances.mapped);
  gfx::write_instances(std::span<const gfx::TlasInstance>(&clas_instance, 1),
                       clas_instances.mapped);
  const u32 khr_scene = bindless.add_acceleration_structure(khr_tlas.handle);
  const u32 clas_scene = bindless.add_acceleration_structure(clas_tlas.handle);
  REQUIRE(khr_scene != gfx::BindlessSet::k_invalid_slot);
  REQUIRE(clas_scene != gfx::BindlessSet::k_invalid_slot);

  // Pipelines and buffers for the pictures.
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
  REQUIRE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error));
  const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
  gfx::ComputePipeline trace_pipeline;
  REQUIRE(
      gfx::create_compute_pipeline(device, trace_module, "trace_main",
                                   std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
                                   sizeof(u64), trace_pipeline, &error));

  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const gfx::BufferUsage k_vis =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  gfx::BufferResource vis[3];  // raster, khr, clas
  gfx::BufferResource params[2];
  gfx::BufferResource host;
  for (gfx::BufferResource& v : vis)
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, v, &error));
  for (gfx::BufferResource& p : params)
    REQUIRE(
        gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, p, &error));
  REQUIRE(
      gfx::create_buffer(device, vis_bytes * 3, gfx::BufferUsage::TransferDst, true, host, &error));

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = scene.meshes.address;
  draw.instances = scene.instances.address;
  draw.triangles = triangles.address;
  draw.visible = cut_buffer.address;
  draw.visibility = vis[0].address;
  draw.extent = gfx::draw_extent(k_w, k_h);
  u64 params_address[2];
  for (u32 p = 0; p < 2; ++p) {
    gfx::RayVisibilityParams ray{};
    ray.view_proj = view_proj;
    ray.clip_to_ray = gfx::clip_to_ray(projection, eye_view);
    ray.camera = Vec4{eye, 0.0f};
    ray.output = vis[1 + p].address;
    ray.instance_base = 0;  // both paths report the visible entry as the geometry index
    ray.width = k_w;
    ray.height = k_h;
    ray.scene = p == 0 ? khr_scene : clas_scene;
    // The entry's pair is the id every picture writes (gfx.md, "The tie rule").
    ray.visible = draw.visible;
    ray.instances = draw.instances;
    ray.meshes = draw.mesh;
    std::memcpy(params[p].mapped, &ray, sizeof(ray));
    params_address[p] = params[p].address;
  }

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[3];
  for (u32 v = 0; v < 3; ++v)
    rg_vis[v] = graph.import_buffer(v == 0 ? "raster" : v == 1 ? "khr" : "clas", vis[v]);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.write(v, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        for (const gfx::BufferResource& v : vis)
          cb.fill_buffer(v.buffer, 0, gfx::k_whole_size, 0);
      });
  graph.add_pass(
      "raster", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) {
        b.render_area(k_w, k_h);
        b.write(rg_vis[0], gfx::Access::FragmentReadWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        timer.begin(cb, "raster");
        cb.bind_pipeline(gfx::BindPoint::Graphics, vertex_pipeline);
        bindless.bind(cb, gfx::BindPoint::Graphics);
        cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(draw),
                          &draw);
        cb.draw(triangles_per_cluster * 3, cut_count, 0, 0);
        timer.end(cb);
      });
  for (u32 p = 0; p < 2; ++p) {
    graph.add_pass(
        p == 0 ? "trace khr" : "trace clas", gfx::PassKind::Compute,
        [&, p](gfx::PassBuilder& b) { b.write(rg_vis[1 + p], gfx::Access::ComputeWrite); },
        [&, p](gfx::CommandList cb, gfx::RenderGraph&) {
          timer.begin(cb, p == 0 ? "trace khr" : "trace clas");
          cb.bind_pipeline(gfx::BindPoint::Compute, trace_pipeline.pipeline);
          bindless.bind(cb, gfx::BindPoint::Compute);
          cb.push_constants(trace_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &params_address[p]);
          cb.dispatch(gfx::ray_visibility_group_count(k_w), gfx::ray_visibility_group_count(k_h),
                      1);
          timer.end(cb);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.read(v, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        for (u32 v = 0; v < 3; ++v) {
          const gfx::BufferCopy copy{0, vis_bytes * v, vis_bytes};
          cb.copy_buffer(vis[v].buffer, host.buffer, copy);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);

  // Frame B: read frame A's build timings, build the top-level structures, draw and trace.
  f64 khr_blas_ms = 0.0;
  f64 clas_ms = 0.0;
  f64 cluster_blas_ms = 0.0;
  {
    gfx::CommandList cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    khr_blas_ms = timer.ms("khr blas");
    clas_ms = timer.ms("clas");
    cluster_blas_ms = timer.ms("cluster blas");
    timer.begin(cb, "tlas khr");
    gfx::build_tlas(cb, khr_tlas, khr_instances.address, 1, gfx::k_build_fast_trace, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                    gfx::MemoryAccess::AccelerationStructureRead |
                                        gfx::MemoryAccess::AccelerationStructureWrite);
    timer.begin(cb, "tlas clas");
    gfx::build_tlas(cb, clas_tlas, clas_instances.address, 1, gfx::k_build_fast_trace, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                    gfx::MemoryAccess::AccelerationStructureRead);
    graph.execute(cb);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  // Frame C: read frame B's timings.
  {
    gfx::CommandList cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    REQUIRE(frames.wait(frames.end_frame()));
  }
  MESSAGE("cut: " << cut_count << " clusters, " << cut_triangles << " triangles at " << k_w << "x"
                  << k_h);
  MESSAGE("memory: khr blas " << khr_blas.buffer.size << " B; clas " << clas_bytes << " B ("
                              << clas_bytes / cut_count << " per cluster) + cluster blas "
                              << cluster_blas.data.size << " B");
  MESSAGE("build ms: khr blas " << khr_blas_ms << "; clas " << clas_ms << " + cluster blas "
                                << cluster_blas_ms << "; tlas khr " << timer.ms("tlas khr")
                                << ", tlas clas " << timer.ms("tlas clas"));
  MESSAGE("frame ms: raster " << timer.ms("raster") << ", trace khr " << timer.ms("trace khr")
                              << ", trace clas " << timer.ms("trace clas"));

  const auto* raster_out = static_cast<const u64*>(host.mapped);
  const u64* khr_out = raster_out + u64{k_w} * k_h;
  const u64* clas_out = khr_out + u64{k_w} * k_h;
  const Comparison khr = compare(raster_out, khr_out, k_w * k_h);
  const Comparison clas = compare(raster_out, clas_out, k_w * k_h);
  const Comparison paths = compare(khr_out, clas_out, k_w * k_h);
  CHECK(khr.covered_a > k_w * k_h / 8);
  for (const Comparison* c : {&khr, &clas}) {
    CHECK(c->coverage_mismatch * 200 <= c->covered_a);
    CHECK(c->id_mismatch * 100 <= c->both);
    CHECK(c->max_depth_diff < 2e-3f);
  }
  CHECK(paths.coverage_mismatch * 1000 <= paths.covered_a);
  CHECK(paths.id_mismatch * 1000 <= paths.both);
  MESSAGE("raster vs khr: coverage mismatch " << khr.coverage_mismatch << ", id mismatch "
                                              << khr.id_mismatch << " of " << khr.both << ", depth "
                                              << khr.max_depth_diff);
  MESSAGE("raster vs clas: coverage mismatch " << clas.coverage_mismatch << ", id mismatch "
                                               << clas.id_mismatch << " of " << clas.both
                                               << ", depth " << clas.max_depth_diff);
  MESSAGE("khr vs clas: coverage mismatch " << paths.coverage_mismatch << ", id mismatch "
                                            << paths.id_mismatch << " of " << paths.both);

  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, trace_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, trace_module);
  bindless.destroy();
  timer.destroy();
  frames.destroy();
  gfx::destroy_acceleration_structure(device, khr_blas);
  gfx::destroy_acceleration_structure(device, khr_tlas);
  gfx::destroy_acceleration_structure(device, clas_tlas);
  gfx::destroy_cluster_set(device, set);
  gfx::destroy_cluster_blas(device, cluster_blas);
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&records, &scratch, &khr_instances, &clas_instances, &host, &clusters, &vertices,
        &triangles, &cut_buffer, &index16_buffer, &index8_buffer, &vis[0], &vis[1], &vis[2],
        &params[0], &params[1]}) {
    gfx::destroy_buffer(device, *b);
  }
}

TEST_CASE("cluster acceleration structures: the cut traced through CLAS matches KHR and raster") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer, gfx_test::Need::RayQuery,
                                  gfx_test::Need::ClusterAccelerationStructure})) {
    return;
  }
  // A frame-sized cut, then every leaf of the engine-view terrain: the per-frame build case and
  // the whole-mesh case.
  run_comparison(device, 129, 1.0f, 640, 480);
  run_comparison(device, 257, 0.0f, 1280, 720);
  device.destroy();
}

namespace {

// What one quad's cluster carries in the micromap test below.
enum class QuadMicromap : u8 {
  None,                // no micromap: the record is the one before R120
  SpecialTransparent,  // both triangles VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT_EXT
  SpecialOpaque,       // both FULLY_OPAQUE
  DataTransparent,     // both index entry 0, a level-0 entry whose one state is transparent
  DataOpaque,          // the same entry, opaque
};

// One quad (two triangles, one cluster) built as a CLAS, under a cluster bottom-level structure and
// a one-instance top-level structure, traced straight on by `ray_visibility.slang` (opaque
// geometry, RAY_FLAG_CULL_NON_OPAQUE). Returns the pixels it covered.
u32 trace_quad(gfx::Device& device, QuadMicromap variant, bool khr, const char* entry) {
  constexpr u32 k_w = 64;
  constexpr u32 k_h = 64;
  std::string error;
  Vector<Vec3> positions;
  Vector<u32> indices;
  make_terrain(2, 1.0f, positions, indices);
  geometry::ClusterLodMesh lod;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, &error),
      error);
  REQUIRE(lod.mesh.clusters.size() == 1);
  const geometry::ClusterDesc& desc = lod.mesh.clusters[0];
  REQUIRE(desc.triangle_count == 2);

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx_test::SingleInstance scene;
  REQUIRE(scene.create(device, lod.mesh, 1, &error));
  gfx::BufferResource vertices;
  gfx::BufferResource index8;
  gfx::BufferResource cut_buffer;
  Vector<u8> packed8;
  gfx::pack_cluster_indices(
      std::span<const u32>(lod.mesh.triangles.data() + desc.triangle_offset, desc.triangle_count),
      packed8);
  const u32 visible_entry[2] = {0, 0};
  REQUIRE(gfx::upload_buffer(device, lod.mesh.vertices.data(),
                             lod.mesh.vertices.size() * sizeof(Vec3),
                             k_storage | gfx::k_build_input_usage, vertices, &error));
  REQUIRE(gfx::upload_buffer(device, packed8.data(), packed8.size(), gfx::k_build_input_usage,
                             index8, &error));
  REQUIRE(gfx::upload_buffer(device, visible_entry, sizeof(visible_entry), k_storage, cut_buffer,
                             &error));

  // The micromap: one level-0 entry (one micro-triangle, two bits), and the quad's two indices.
  gfx::OpacityMicromapSet micromap;
  gfx::BufferResource omm_indices;
  const bool with = variant != QuadMicromap::None;
  if (with) {
    const u32 state =
        variant == QuadMicromap::DataOpaque ? gfx::k_micromap_opaque : gfx::k_micromap_transparent;
    const u32 data[1] = {state};
    const gfx::MicromapTriangle triangle{};
    REQUIRE_MESSAGE(gfx::build_opacity_micromap(
                        device, std::span<const u32>(data, 1),
                        std::span<const gfx::MicromapTriangle>(&triangle, 1), micromap, &error),
                    error);
    i32 index = 0;
    if (variant == QuadMicromap::SpecialTransparent) index = gfx::k_micromap_fully_transparent;
    if (variant == QuadMicromap::SpecialOpaque) index = gfx::k_micromap_fully_opaque;
    const i32 per_triangle[2] = {index, index};
    REQUIRE(gfx::upload_buffer(device, per_triangle, sizeof(per_triangle), gfx::k_build_input_usage,
                               omm_indices, &error));
  }

  gfx::ClusterSetLimits limits;
  limits.max_clusters = 1;
  limits.max_triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  limits.max_geometry_index = 0;
  limits.opacity_micromaps = with;
  gfx::ClusterSet set;
  gfx::ClusterBlas blas;
  REQUIRE_MESSAGE(gfx::create_cluster_set(device, limits, set, &error), error);
  REQUIRE_MESSAGE(gfx::create_cluster_blas(device, 1, blas, &error, with), error);
  gfx::ClusterBuildInput in;
  in.cluster_id = 0;
  in.triangle_count = desc.triangle_count;
  in.vertex_count = desc.vertex_count;
  in.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
  in.indices = index8.address;
  in.omm_array = with ? micromap.address : 0;
  in.omm_indices = with ? omm_indices.address : 0;
  gfx::BufferResource records;
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_build_record_bytes, gfx::k_build_input_usage,
                             true, records, &error));
  gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&in, 1), records.mapped);
  gfx::AccelerationStructure tlas;
  REQUIRE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas, &error));
  gfx::BufferResource scratch;
  REQUIRE(gfx::create_scratch(
      device,
      std::max({set.build_scratch_bytes, blas.build_scratch_bytes, tlas.build_scratch_bytes}),
      scratch, &error));
  gfx::BufferResource instances;
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                             instances, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 slot = bindless.add_acceleration_structure(tlas.handle);
  REQUIRE(slot != gfx::BindlessSet::k_invalid_slot);

  // `trace_main` is the engine's own camera-ray pass (ray_visibility.slang), the others the
  // probe's.
  const bool engine_pass = std::strcmp(entry, "trace_main") == 0;
  gfx::ShaderModuleHandle trace_module =
      engine_pass ? gfx::create_shader_module(device, shaders::k_ray_visibility_spirv,
                                              shaders::k_ray_visibility_spirv_size, &error)
                  : gfx::create_shader_module(device, shaders::k_micromap_probe_spirv,
                                              shaders::k_micromap_probe_spirv_size, &error);
  REQUIRE(trace_module.valid());
  const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
  struct ProbeParams {
    u64 output = 0;
    u32 scene = 0;
    u32 size = 0;
  };
  gfx::ComputePipeline trace_pipeline;
  REQUIRE(gfx::create_compute_pipeline(
      device, trace_module, entry, std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
      engine_pass ? sizeof(u64) : sizeof(ProbeParams), trace_pipeline, &error));

  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  gfx::BufferResource vis;
  gfx::BufferResource params;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_address | gfx::BufferUsage::TransferDst, true,
                             vis, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, params,
                             &error));
  ProbeParams probe;
  probe.output = vis.address;
  probe.scene = slot;
  probe.size = k_w;
  {
    const Vec3 eye{0.0f, 3.0f, 2.0f};
    const Mat4 projection =
        perspective_reversed_z(radians(60.0f), static_cast<f32>(k_w) / k_h, 0.1f);
    const Mat4 eye_view = look_at(eye, Vec3{}, Vec3{0, 1, 0});
    gfx::RayVisibilityParams ray{};
    ray.view_proj = projection * eye_view;
    ray.clip_to_ray = gfx::clip_to_ray(projection, eye_view);
    ray.camera = Vec4{eye, 0.0f};
    ray.output = vis.address;
    ray.width = k_w;
    ray.height = k_h;
    ray.scene = slot;
    ray.visible = cut_buffer.address;
    ray.instances = scene.instances.address;
    ray.meshes = scene.meshes.address;
    std::memcpy(params.mapped, &ray, sizeof(ray));
  }
  std::memset(vis.mapped, 0, vis_bytes);

  // The KHR path, for the bisection: the same quad as one triangle geometry whose micromap rides
  // on VkAccelerationStructureTrianglesOpacityMicromapEXT, as VK_EXT_opacity_micromap specifies.
  gfx::AccelerationStructure khr_blas;
  gfx::BufferResource index16;
  VkAccelerationStructureGeometryKHR geometry{};
  VkAccelerationStructureBuildGeometryInfoKHR khr_build{};
  VkAccelerationStructureTrianglesOpacityMicromapEXT khr_omm{};
  VkMicromapUsageEXT khr_usage{};
  if (khr) {
    Vector<u16> packed16;
    gfx::expand_packed_triangles(
        std::span<const u32>(lod.mesh.triangles.data() + desc.triangle_offset, desc.triangle_count),
        packed16);
    REQUIRE(gfx::upload_buffer(device, packed16.data(), packed16.size() * sizeof(u16),
                               gfx::k_build_input_usage, index16, &error));
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR& tri = geometry.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = in.vertices;
    tri.vertexStride = sizeof(f32) * 3;
    tri.maxVertex = desc.vertex_count - 1;
    tri.indexType = VK_INDEX_TYPE_UINT16;
    tri.indexData.deviceAddress = index16.address;
    if (with) {
      const bool data =
          variant == QuadMicromap::DataOpaque || variant == QuadMicromap::DataTransparent;
      khr_usage.count = data ? 2u : 0u;
      khr_usage.subdivisionLevel = 0;
      khr_usage.format = gfx::k_micromap_format_4state;
      khr_omm.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
      khr_omm.indexType = VK_INDEX_TYPE_UINT32;
      khr_omm.indexBuffer.deviceAddress = omm_indices.address;
      khr_omm.indexStride = sizeof(i32);
      khr_omm.baseTriangle = 0;
      khr_omm.usageCountsCount = data ? 1u : 0u;
      khr_omm.pUsageCounts = data ? &khr_usage : nullptr;
      khr_omm.micromap = gfx::vk::native(micromap.handle);
      tri.pNext = &khr_omm;
    }
    khr_build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    khr_build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    khr_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    khr_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    khr_build.geometryCount = 1;
    khr_build.pGeometries = &geometry;
    const u32 primitives = desc.triangle_count;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device.handles().device,
                                            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &khr_build, &primitives, &sizes);
    REQUIRE(gfx::create_buffer(
        device, sizes.accelerationStructureSize,
        gfx::BufferUsage::AccelerationStorage | gfx::BufferUsage::ShaderDeviceAddress, false,
        khr_blas.buffer, &error));
    VkAccelerationStructureCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create.buffer = gfx::vk::native(khr_blas.buffer.buffer);
    create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
    REQUIRE(vkCreateAccelerationStructureKHR(device.handles().device, &create, nullptr,
                                             &structure) == VK_SUCCESS);
    khr_blas.handle = gfx::vk::wrap(structure);
    VkAccelerationStructureDeviceAddressInfoKHR address_info{};
    address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    address_info.accelerationStructure = structure;
    khr_blas.address =
        vkGetAccelerationStructureDeviceAddressKHR(device.handles().device, &address_info);
    gfx::destroy_buffer(device, scratch);
    REQUIRE(gfx::create_scratch(device,
                                std::max({sizes.buildScratchSize, tlas.build_scratch_bytes}) + 256,
                                scratch, &error));
    khr_build.dstAccelerationStructure = structure;
    khr_build.scratchData.deviceAddress = (scratch.address + 255) / 256 * 256;
  }

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 1, &error));
  if (khr) {
    gfx::CommandList cb = frames.begin_frame();
    const VkAccelerationStructureBuildRangeInfoKHR range{desc.triangle_count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(gfx::vk::native(cb), 1, &khr_build, &ranges);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AllCommands,
                                    gfx::MemoryAccess::MemoryRead);
    REQUIRE(frames.wait(frames.end_frame()));
  } else {
    gfx::CommandList cb = frames.begin_frame();
    gfx::build_cluster_set(cb, set, records.address, 0, scratch);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                    gfx::MemoryAccess::AccelerationStructureRead |
                                        gfx::MemoryAccess::AccelerationStructureWrite);
    gfx::build_cluster_blas(cb, blas, set.addresses.address, 1, scratch);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AllCommands,
                                    gfx::MemoryAccess::MemoryRead);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  if (!khr && variant != QuadMicromap::DataOpaque && variant != QuadMicromap::DataTransparent) {
    // A record that names a micromap builds a larger structure (384 bytes and 512 for this quad on
    // the RTX 5090): the CLAS does take it in, whatever the traversal then does.
    const u32 bytes = static_cast<const u32*>(set.sizes.mapped)[0];
    MESSAGE("quad CLAS: " << bytes << " bytes, micromap " << (with ? 1 : 0));
  }
  gfx::TlasInstance instance;
  instance.blas = khr ? khr_blas.address : blas.address;
  gfx::write_instances(std::span<const gfx::TlasInstance>(&instance, 1), instances.mapped);
  {
    gfx::CommandList cb = frames.begin_frame();
    gfx::build_tlas(cb, tlas, instances.address, 1, gfx::k_build_fast_trace, scratch);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                    gfx::MemoryAccess::AccelerationStructureRead);
    cb.bind_pipeline(gfx::BindPoint::Compute, trace_pipeline.pipeline);
    bindless.bind(cb, gfx::BindPoint::Compute);
    if (engine_pass) {
      const u64 address = params.address;
      cb.push_constants(trace_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64), &address);
    } else {
      cb.push_constants(trace_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(probe), &probe);
    }
    cb.dispatch((k_w + 7) / 8, (k_h + 7) / 8, 1);
    gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AllCommands,
                                    gfx::MemoryAccess::MemoryRead);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  u32 covered = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    covered += (engine_pass ? static_cast<const u64*>(vis.mapped)[i]
                            : static_cast<const u32*>(vis.mapped)[i]) != 0
                   ? 1u
                   : 0u;
  }

  frames.destroy();

  gfx::destroy_compute_pipeline(device, trace_pipeline);
  gfx::destroy_shader_module(device, trace_module);
  bindless.destroy();
  gfx::destroy_acceleration_structure(device, tlas);
  if (khr) gfx::destroy_acceleration_structure(device, khr_blas);
  gfx::destroy_cluster_set(device, set);
  gfx::destroy_cluster_blas(device, blas);
  gfx::destroy_opacity_micromaps(device, micromap);
  scene.destroy(device);
  for (gfx::BufferResource* b : {&vertices, &index8, &index16, &cut_buffer, &omm_indices, &records,
                                 &scratch, &instances, &vis, &params}) {
    gfx::destroy_buffer(device, *b);
  }
  return covered;
}

}  // namespace

// **The record-level identity R120 needs** (renderer.md, "Opacity micromaps"): a cluster record
// that names a micromap is one the traversal honours, isolated from the renderer. A quad whose two
// triangles are transparent by the special index alone, or by a data entry, is missed by a ray
// that meets it head on; opaque by either, it is hit as it is with no micromap.
TEST_CASE("cluster acceleration structures: a CLAS's opacity micromap decides the traversal") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer, gfx_test::Need::RayQuery,
                                  gfx_test::Need::ClusterAccelerationStructure})) {
    return;
  }
  if (!device.features().opacity_micromap) {
    MESSAGE("skipped: the device has no VK_EXT_opacity_micromap");
    device.destroy();
    return;
  }
  // Each variant, on the cluster path and on the KHR path (the same micromap through
  // VkAccelerationStructureTrianglesOpacityMicromapEXT), traced by the probe and by the engine's
  // own camera-ray pass (ray_visibility.slang).
  for (const char* entry : {"probe_eye", "trace_main"}) {
    for (const bool khr : {false, true}) {
      const u32 none = trace_quad(device, QuadMicromap::None, khr, entry);
      const u32 special_transparent =
          trace_quad(device, QuadMicromap::SpecialTransparent, khr, entry);
      const u32 special_opaque = trace_quad(device, QuadMicromap::SpecialOpaque, khr, entry);
      const u32 data_transparent = trace_quad(device, QuadMicromap::DataTransparent, khr, entry);
      const u32 data_opaque = trace_quad(device, QuadMicromap::DataOpaque, khr, entry);
      MESSAGE(std::string(entry) << (khr ? " KHR" : " CLAS") << ": no micromap " << none
                                 << ", special transparent " << special_transparent
                                 << ", special opaque " << special_opaque << ", data transparent "
                                 << data_transparent << ", data opaque " << data_opaque);
      CHECK(none > 64);
      CHECK(special_opaque == none);
      CHECK(data_opaque == none);
      CHECK(special_transparent == 0);
      CHECK(data_transparent == 0);
    }
  }
  device.destroy();
}
// **A compile the driver made without micromaps is never traced with them** (device.cpp,
// `micromap_key`; renderer.md, "Opacity micromaps"). The driver keeps compiled shaders in a cache
// of its own whose key does not include VK_EXT_opacity_micromap, and a ray query from a compile
// made on a device without it traces every micromap as absent. Here a device without the extension
// compiles the probe first, then one with it traces a quad transparent by its special index: it
// must miss. Without the key every run after the first failed it on driver 617.42 (1,998 pixels
// hit of 1,998).
TEST_CASE(
    "cluster acceleration structures: a shader compiled without micromaps is not reused "
    "with them") {
  {
    gfx::DeviceOptions options;
    options.overrides.absent.push_back("VK_EXT_opacity_micromap");
    gfx::Device plain;
    if (!gfx_test::open_device(plain, options)) return;
    if (!gfx_test::require(
            plain, {gfx_test::Need::RayQuery, gfx_test::Need::ClusterAccelerationStructure})) {
      return;
    }
    REQUIRE(!plain.features().opacity_micromap);
    std::string error;
    gfx::ShaderModuleHandle module = gfx::create_shader_module(
        plain, shaders::k_micromap_probe_spirv, shaders::k_micromap_probe_spirv_size, &error);
    REQUIRE(module.valid());
    gfx::BindlessSet bindless;
    REQUIRE(bindless.create(plain, gfx::BindlessConfig{}, &error));
    const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
    gfx::ComputePipeline pipeline;
    REQUIRE(gfx::create_compute_pipeline(
        plain, module, "probe_eye", std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
        16, pipeline, &error));
    gfx::destroy_compute_pipeline(plain, pipeline);
    gfx::destroy_shader_module(plain, module);
    bindless.destroy();
    plain.destroy();
  }
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!device.features().opacity_micromap) {
    MESSAGE("skipped: the device has no VK_EXT_opacity_micromap");
    device.destroy();
    return;
  }
  const u32 none = trace_quad(device, QuadMicromap::None, false, "probe_eye");
  const u32 transparent = trace_quad(device, QuadMicromap::SpecialTransparent, false, "probe_eye");
  MESSAGE("after a compile without micromaps: no micromap " << none << ", special transparent "
                                                            << transparent);
  CHECK(none > 64);
  CHECK(transparent == 0);
  device.destroy();
}
