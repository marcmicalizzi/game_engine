// Where the resolve takes a pixel's point (docs/subsystems/gfx.md, "Where a pixel meets its
// triangle"). Large triangles around a walker looking down at his feet — one with a corner behind
// the camera's plane, one with two, one with a corner on it, a wall beside him that reaches behind
// him, and one wholly in front of him for a control — are drawn by every rasterizer the device has
// into the visibility buffer, and for every pixel each covers, what the resolve takes for it
// (material.slang, read out as floats by shaders/reconstruct_probe.slang: the barycentrics, the
// point and the UV interpolated at them, the UV's and the point's change per pixel) is held to the
// CPU's ray through the pixel's centre against the triangle's plane (brdf_reference.h,
// `pixel_on_triangle`), with the derivatives by central differences of the same. Until 2026-10-03
// the resolve took screen-space barycentrics of the corners divided by w and clamped them before
// the perspective correction: behind the plane the point was 1,279 footprints from where the
// pixel looks, the wall's 563, and on the plane 1.4 with the derivatives 2% off.
//
// The rasterizers: the vertex path (every device), the mesh path (where the device has mesh
// shaders), and the ray path's primary visibility (where it has ray queries), each a part of its
// own. The software rasterizer is not one of them: it skips a triangle with a corner behind the
// camera, and the cull pass sends a cluster that close to the hardware (cluster_sw_raster.slang).
// The vertex path's indexed draw writes the capacity draw's buffer word for word
// (vertex_path_tests.cpp), so the capacity draw stands for it. Skips without 64-bit buffer atomics.
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
#include <domain/gfx/pipeline.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/ray_visibility.spv.h>
#include <shaders/reconstruct_probe.spv.h>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;

namespace {

// Mirrors ProbeParams in shaders/reconstruct_probe.slang.
struct ProbeParams {
  Vec4 camera;
  Mat4 view_proj;
  u64 visibility = 0;
  u64 clusters = 0;
  u64 mesh = 0;
  u64 triangles = 0;
  u64 instances = 0;
  u64 attributes = 0;
  u64 out = 0;
  u32 width = 0;
  u32 height = 0;
};
static_assert(sizeof(ProbeParams) == 144);

constexpr u32 k_size = 128;
constexpr u32 k_probe_rows = 6;  // float4s a pixel

// One triangle and what the case says about it: how many of its corners are behind the camera's
// plane and how many are on it, which the case checks before it draws.
struct Case {
  const char* name;
  Vec3 corner[3];
  u32 behind;
  u32 on_plane;
};

enum class Source : u8 { Vertex, Mesh, Ray };

std::string source_name(Source s) {
  return s == Source::Vertex ? "the vertex path" : (s == Source::Mesh ? "the mesh path" : "rays");
}

// What one (case, rasterizer) measured, against the CPU's ray.
struct Measured {
  u32 covered = 0;  // pixels the rasterizer drew
  u32 clamped = 0;  // ... whose ray lands outside the triangle by the snap, moved onto its edge
  u32 outside = 0;  // ... whose ray misses it by more than a snap could explain
  u32 missing = 0;  // pixels whose ray lands well inside the triangle and that nothing drew
  f64 point = 0.0;  // worst distance from the CPU's point, in the pixel's own footprints
  f64 bary = 0.0;   // worst barycentric difference
  f64 uv = 0.0;     // worst UV difference
  f64 slope = 0.0;  // worst derivative difference, relative to the derivative's length
};

f64 length(ref::Dvec3 v) { return std::sqrt(ref::dot(v, v)); }

}  // namespace

TEST_CASE(
    "material resolve: a pixel's point is its ray against its triangle, wherever its corners are") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // A walker's eyes 1.6 m over the ground, looking 60 degrees down along -z: the camera's plane
  // meets the ground 2.77 m behind him (h tan 60), and a triangle reaching past that has a corner
  // the rasterizer clips and a divide by w puts on the far side of the screen.
  const Vec3 eye{0.0f, 1.6f, 0.0f};
  const Vec3 target{0.0f, 1.6f - 0.8660254f, -0.5f};
  const Vec3 up{0.0f, 1.0f, 0.0f};
  const f32 fov_y = radians(60.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, 1.0f, 0.05f) * look_at(eye, target, up);
  const Case cases[] = {
      {"one corner behind",
       {Vec3{-12.0f, 0.0f, -16.0f}, Vec3{12.0f, 0.0f, -16.0f}, Vec3{0.0f, 0.0f, 9.0f}},
       1,
       0},
      {"two corners behind",
       {Vec3{-12.0f, 0.0f, 9.0f}, Vec3{12.0f, 0.0f, 9.0f}, Vec3{0.0f, 0.0f, -16.0f}},
       2,
       0},
      // Where the camera's plane meets the ground, 1.6 tan 60 behind the feet: a clip w of zero to
      // within the float's rounding, which a divide by it turns into a corner at infinity.
      {"a corner on the camera's plane",
       {Vec3{-12.0f, 0.0f, -16.0f}, Vec3{12.0f, 0.0f, -16.0f}, Vec3{0.0f, 0.0f, 2.7712813f}},
       0,
       1},
      // A wall to the right, from behind his back to ten metres ahead, leaning out a little so
      // that its plane is not one of the axes'.
      {"a wall reaching behind",
       {Vec3{0.6f, -1.0f, 6.0f}, Vec3{0.9f, 4.0f, -10.0f}, Vec3{0.6f, -1.0f, -10.0f}},
       1,
       0},
      {"in front (the control)",
       {Vec3{-3.0f, 0.0f, -3.5f}, Vec3{3.0f, 0.0f, -3.5f}, Vec3{0.0f, 0.0f, 1.5f}},
       0,
       0},
  };
  constexpr u32 k_cases = sizeof(cases) / sizeof(cases[0]);

  const ref::Dvec3 eye_d = ref::dvec3(eye);
  const ref::Dvec3 target_d = ref::dvec3(target);
  const ref::Dvec3 up_d = ref::dvec3(up);
  const ref::Dvec3 forward = ref::normalize(target_d - eye_d);
  for (const Case& c : cases) {
    u32 behind = 0;
    u32 on_plane = 0;
    for (const Vec3& p : c.corner) {
      const f64 ahead = ref::dot(ref::dvec3(p) - eye_d, forward);
      // The plane is clip w = 0, and behind it w is negative.
      const Vec4 clip = view_proj * Vec4{p, 1.0f};
      if (std::abs(ahead) < 1e-5) {
        ++on_plane;
        CHECK(std::abs(clip.w) < 1e-5f);
      } else {
        behind += ahead < 0.0 ? 1u : 0u;
        CHECK((clip.w < 0.0f) == (ahead < 0.0));
      }
    }
    REQUIRE_MESSAGE(behind == c.behind, c.name);
    REQUIRE_MESSAGE(on_plane == c.on_plane, c.name);
  }

  // Each triangle its own mesh, its UVs the corners' own barycentrics (1, 0), (0, 1), (0, 0), so
  // that the interpolated UV is two of the barycentrics and its derivatives theirs.
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  geometry::ClusterMesh meshes[k_cases];
  gfx_test::SingleInstance scenes[k_cases];
  gfx::BufferResource clusters[k_cases];
  gfx::BufferResource triangles[k_cases];
  gfx::BufferResource attributes[k_cases];
  gfx::BufferResource floats[k_cases];  // the float corners, for the ray path's structure
  gfx::BufferResource indices16[k_cases];
  const Vec3 normals[3] = {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
  const Vec2 uvs[3] = {Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{0.0f, 0.0f}};
  const u32 indices[3] = {0, 1, 2};
  for (u32 i = 0; i < k_cases; ++i) {
    geometry::AttributeSource source;
    source.normals = normals;
    source.uvs = uvs;
    REQUIRE(geometry::build_clusters(cases[i].corner, indices, geometry::ClusterBuildOptions{},
                                     meshes[i], &error, source));
    REQUIRE(meshes[i].clusters.size() == 1u);
    REQUIRE(meshes[i].clusters[0].triangle_count == 1u);
    REQUIRE(gfx::upload_buffer(device, meshes[i].clusters.data(), sizeof(geometry::ClusterDesc),
                               k_storage, clusters[i], &error));
    REQUIRE(scenes[i].create(device, meshes[i], 1, &error));
    REQUIRE(gfx::upload_buffer(device, meshes[i].triangles.data(),
                               meshes[i].triangles.size() * sizeof(u32), k_storage, triangles[i],
                               &error));
    REQUIRE(gfx::upload_buffer(device, meshes[i].attributes.data(),
                               meshes[i].attributes.size() * sizeof(geometry::VertexAttributes),
                               k_storage, attributes[i], &error));
    REQUIRE(gfx::upload_buffer(device, meshes[i].vertices.data(),
                               meshes[i].vertices.size() * sizeof(Vec3),
                               k_storage | gfx::k_build_input_usage, floats[i], &error));
    Vector<u16> expanded;
    gfx::expand_packed_triangles(std::span<const u32>(meshes[i].triangles.data(), 1), expanded);
    REQUIRE(gfx::upload_buffer(device, expanded.data(), expanded.size() * sizeof(u16),
                               gfx::k_build_input_usage, indices16[i], &error));
  }

  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::ShaderModuleHandle probe_module = gfx::create_shader_module(
      device, shaders::k_reconstruct_probe_spirv, shaders::k_reconstruct_probe_spirv_size, &error);
  REQUIRE(probe_module.valid());
  const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
  gfx::ComputePipeline probe;
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, probe_module, "probe_main",
                                   std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
                                   sizeof(u64), probe, &error),
      error);

  // The ray path: one bottom-level structure a case over its float corners, one top-level
  // structure each, and the trace pipeline, where the device can.
  const bool rays =
      gfx_test::part(device, "the ray path",
                     {gfx_test::Need::AccelerationStructure, gfx_test::Need::RayQuery}) &&
      bindless.has_acceleration_structures();
  gfx::AccelerationStructure blas[k_cases];
  gfx::AccelerationStructure tlas[k_cases];
  gfx::BufferResource tlas_instances[k_cases];
  u32 scene_slot[k_cases] = {};
  gfx::ShaderModuleHandle trace_module = {};
  gfx::ComputePipeline trace;
  gfx::BufferResource visible;  // {instance 0, cluster 0}: what the structure's one geometry is
  if (rays) {
    const u32 entry[2] = {0, 0};
    REQUIRE(gfx::upload_buffer(device, entry, sizeof(entry), k_storage, visible, &error));
    for (u32 i = 0; i < k_cases; ++i) {
      gfx::ClusterGeometry g;
      g.vertices = floats[i].address;
      g.vertex_count = meshes[i].clusters[0].vertex_count;
      g.indices = indices16[i].address;
      g.triangle_count = 1;
      REQUIRE_MESSAGE(gfx::create_blas(device, std::span<const gfx::ClusterGeometry>(&g, 1),
                                       gfx::k_build_fast_trace, blas[i], &error),
                      error);
      REQUIRE_MESSAGE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas[i], &error), error);
      gfx::BufferResource scratch;
      REQUIRE(gfx::create_scratch(
          device, std::max(blas[i].build_scratch_bytes, tlas[i].build_scratch_bytes), scratch,
          &error));
      REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage,
                                 true, tlas_instances[i], &error));
      gfx::TlasInstance instance;
      instance.blas = blas[i].address;
      gfx::write_instances(std::span<const gfx::TlasInstance>(&instance, 1),
                           tlas_instances[i].mapped);
      REQUIRE(gfx::submit_immediate(
          device,
          [&](gfx::CommandList cb) {
            gfx::build_blas(cb, blas[i], std::span<const gfx::ClusterGeometry>(&g, 1),
                            gfx::k_build_fast_trace, scratch);
            gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                            gfx::MemoryAccess::AccelerationStructureRead);
            gfx::build_tlas(cb, tlas[i], tlas_instances[i].address, 1, gfx::k_build_fast_trace,
                            scratch);
            gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                            gfx::MemoryAccess::AccelerationStructureRead);
          },
          &error));
      gfx::destroy_buffer(device, scratch);
      scene_slot[i] = bindless.add_acceleration_structure(tlas[i].handle);
      REQUIRE(scene_slot[i] != gfx::BindlessSet::k_invalid_slot);
    }
    trace_module = gfx::create_shader_module(device, shaders::k_ray_visibility_spirv,
                                             shaders::k_ray_visibility_spirv_size, &error);
    REQUIRE(trace_module.valid());
    REQUIRE_MESSAGE(gfx::create_compute_pipeline(
                        device, trace_module, "trace_main",
                        std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1),
                        sizeof(u64), trace, &error),
                    error);
  }

  Source sources[3] = {Source::Vertex};
  u32 source_count = 1;
  if (gfx_test::part(device, "the mesh path", {gfx_test::Need::MeshShader})) {
    sources[source_count++] = Source::Mesh;
  }
  if (rays) sources[source_count++] = Source::Ray;

  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  const u64 probe_bytes = u64{k_size} * k_size * k_probe_rows * sizeof(Vec4);
  gfx::BufferResource vis;
  gfx::BufferResource probe_out;
  gfx::BufferResource host;
  gfx::BufferResource params;
  REQUIRE(gfx::create_buffer(
      device, vis_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst, false, vis,
      &error));
  REQUIRE(gfx::create_buffer(
      device, probe_bytes,
      k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferSrc, false,
      probe_out, &error));
  REQUIRE(
      gfx::create_buffer(device, probe_bytes, gfx::BufferUsage::TransferDst, true, host, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(ProbeParams) + sizeof(gfx::RayVisibilityParams),
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  const u64 probe_address = params.address;
  const u64 trace_address = params.address + sizeof(ProbeParams);

  for (u32 si = 0; si < source_count; ++si) {
    const Source source = sources[si];
    gfx_test::ClusterRaster raster;
    if (source != Source::Ray) {
      REQUIRE_MESSAGE(
          raster.create(
              device,
              source == Source::Mesh ? gfx_test::RasterPath::Mesh : gfx_test::RasterPath::Vertex,
              bindless.pipeline_layout(), geometry::ClusterBuildOptions{}.max_triangles, &error),
          error);
    }
    for (u32 ci = 0; ci < k_cases; ++ci) {
      const Case& c = cases[ci];
      ProbeParams p{};
      p.camera = Vec4{eye, 0.0f};
      p.view_proj = view_proj;
      p.visibility = vis.address;
      p.clusters = clusters[ci].address;
      p.mesh = scenes[ci].meshes.address;
      p.triangles = triangles[ci].address;
      p.instances = scenes[ci].instances.address;
      p.attributes = attributes[ci].address;
      p.out = probe_out.address;
      p.width = k_size;
      p.height = k_size;
      std::memcpy(params.mapped, &p, sizeof(p));
      gfx::RayVisibilityParams ray{};
      if (source == Source::Ray) {
        ray.view_proj = view_proj;
        ray.inv_view_proj = inverse(view_proj);
        ray.camera = Vec4{eye, 0.0f};
        ray.output = vis.address;
        ray.width = k_size;
        ray.height = k_size;
        ray.scene = scene_slot[ci];
        ray.visible = visible.address;
        ray.instances = scenes[ci].instances.address;
        ray.meshes = scenes[ci].meshes.address;
        std::memcpy(static_cast<u8*>(params.mapped) + sizeof(ProbeParams), &ray, sizeof(ray));
      }
      gfx::ClusterDrawParams draw{};
      draw.view_proj = view_proj;
      draw.clusters = clusters[ci].address;
      draw.mesh = scenes[ci].meshes.address;
      draw.instances = scenes[ci].instances.address;
      draw.triangles = triangles[ci].address;
      draw.visibility = vis.address;
      draw.width = k_size;
      draw.height = k_size;

      gfx::RenderGraph graph(device);
      const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
      const gfx::RgBuffer rg_out = graph.import_buffer("probe", probe_out);
      const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
      graph.add_pass(
          "clear", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::TransferWrite); },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
          });
      if (source == Source::Ray) {
        graph.add_pass(
            "trace", gfx::PassKind::Compute,
            [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::ComputeReadWrite); },
            [&](gfx::CommandList cb, gfx::RenderGraph&) {
              cb.bind_pipeline(gfx::BindPoint::Compute, trace.pipeline);
              bindless.bind(cb, gfx::BindPoint::Compute);
              cb.push_constants(trace.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                                &trace_address);
              cb.dispatch(gfx::ray_visibility_group_count(k_size),
                          gfx::ray_visibility_group_count(k_size), 1);
            });
      } else {
        graph.add_pass(
            "visibility", gfx::PassKind::Raster,
            [&](gfx::PassBuilder& b) {
              b.render_area(k_size, k_size);
              b.write(rg_vis, gfx::Access::FragmentReadWrite);
            },
            [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless, draw, 1); });
      }
      graph.add_pass(
          "probe", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.read(rg_vis, gfx::Access::ComputeRead);
            b.write(rg_out, gfx::Access::ComputeWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, probe.pipeline);
            bindless.bind(cb, gfx::BindPoint::Compute);
            cb.push_constants(probe.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &probe_address);
            cb.dispatch(k_size / 8, k_size / 8, 1);
          });
      graph.add_pass(
          "readback", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            b.read(rg_out, gfx::Access::TransferRead);
            b.write(rg_host, gfx::Access::TransferWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.copy_buffer(probe_out.buffer, host.buffer, gfx::BufferCopy{0, 0, probe_bytes});
          });
      REQUIRE_MESSAGE(graph.compile(&error), error);
      gfx::CommandList commands = frames.begin_frame();
      graph.execute(commands);
      REQUIRE(frames.wait(frames.end_frame()));
      graph.reset();

      // The triangle as the shader read it: its corners off the mesh's 16-bit grid in the
      // cluster's own order, and the UV each carries.
      const geometry::ClusterMesh& mesh = meshes[ci];
      const geometry::ClusterDesc& cluster = mesh.clusters[0];
      const u32 packed = mesh.triangles[cluster.triangle_offset];
      ref::Dvec3 corner[3];
      Vec2 uv[3];
      for (u32 k = 0; k < 3; ++k) {
        const u32 v = cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k);
        corner[k] = ref::dvec3(geometry::dequantize_position(mesh, v));
        uv[k] = geometry::decode_half2(mesh.attributes[v].uv_half2);
      }
      const auto uv_at = [&](ref::Dvec3 b, u32 axis) {
        const auto of = [&](u32 k) { return static_cast<f64>(axis == 0 ? uv[k].x : uv[k].y); };
        return b.x * of(0) + b.y * of(1) + b.z * of(2);
      };
      const auto* rows = static_cast<const Vec4*>(host.mapped);
      Measured m;
      for (u32 y = 0; y < k_size; ++y) {
        for (u32 x = 0; x < k_size; ++x) {
          const f64 cx = static_cast<f64>(x) + 0.5;
          const f64 cy = static_cast<f64>(y) + 0.5;
          const ref::Dvec3 dir = ref::pixel_direction(
              eye_d, target_d, up_d, static_cast<f64>(fov_y), 1.0, k_size, k_size, cx, cy);
          const ref::TriangleHit hit = ref::pixel_on_triangle(eye_d, dir, corner);
          const bool in_front = ref::dot(hit.position - eye_d, forward) > 0.06;
          const f64 margin = std::min(hit.ray.x, std::min(hit.ray.y, hit.ray.z));
          const Vec4* r = rows + (u64{y} * k_size + x) * k_probe_rows;
          if (r[0].w == 0.0f) {
            // A hit a hundredth of the triangle inside its edges, in front of the near plane:
            // something must have drawn it.
            if (in_front && margin > 0.01) ++m.missing;
            continue;
          }
          ++m.covered;
          if (hit.clamped) ++m.clamped;
          if (margin < -1e-3) ++m.outside;
          // The footprint: the point's change per pixel, from the ray's barycentrics' central
          // differences, which is the scale every difference below is read against.
          ref::Dvec3 db_dx;
          ref::Dvec3 db_dy;
          ref::pixel_barycentric_gradients(eye_d, target_d, up_d, static_cast<f64>(fov_y), 1.0,
                                           k_size, k_size, cx, cy, corner, db_dx, db_dy);
          const ref::Dvec3 dp_dx = corner[0] * db_dx.x + corner[1] * db_dx.y + corner[2] * db_dx.z;
          const ref::Dvec3 dp_dy = corner[0] * db_dy.x + corner[1] * db_dy.y + corner[2] * db_dy.z;
          const f64 footprint = std::max(length(dp_dx), length(dp_dy));
          const ref::Dvec3 b_gpu = ref::dvec3(r[0]);
          const ref::Dvec3 p_gpu = ref::dvec3(r[1]);
          m.bary = std::max(m.bary, length(b_gpu - hit.b));
          m.point = std::max(m.point, length(p_gpu - hit.position) / footprint);
          m.uv = std::max(m.uv, std::max(std::abs(static_cast<f64>(r[2].x) - uv_at(hit.b, 0)),
                                         std::abs(static_cast<f64>(r[2].y) - uv_at(hit.b, 1))));
          const auto relative = [](ref::Dvec3 got, ref::Dvec3 want) {
            return length(got - want) / std::max(length(want), 1e-9);
          };
          const ref::Dvec3 duv_dx{uv_at(db_dx, 0), uv_at(db_dx, 1), 0.0};
          const ref::Dvec3 duv_dy{uv_at(db_dy, 0), uv_at(db_dy, 1), 0.0};
          const ref::Dvec3 gpu_duv_dx{static_cast<f64>(r[2].z), static_cast<f64>(r[2].w), 0.0};
          const ref::Dvec3 gpu_duv_dy{static_cast<f64>(r[3].x), static_cast<f64>(r[3].y), 0.0};
          m.slope =
              std::max({m.slope, relative(gpu_duv_dx, duv_dx), relative(gpu_duv_dy, duv_dy),
                        relative(ref::dvec3(r[4]), dp_dx), relative(ref::dvec3(r[5]), dp_dy)});
        }
      }
      MESSAGE(std::string(c.name) << ", " << source_name(source) << ": " << m.covered
                                  << " pixels drawn (" << m.clamped
                                  << " with the ray a snap outside, " << m.outside << " further), "
                                  << m.missing << " missing; worst point " << m.point
                                  << " of a footprint, barycentric " << m.bary << ", uv " << m.uv
                                  << ", derivative " << m.slope << " of its length");
      // Most of the picture: every case puts its triangle under most of the 128 x 128 pixels.
      CHECK(m.covered > k_size * k_size / 8);
      CHECK(m.missing == 0u);
      CHECK(m.outside == 0u);
      // A point a hundredth of a pixel from where the pixel looks, and derivatives a thousandth of
      // their own length from the ray's; measured, both are float rounding (the message above).
      CHECK(m.point < 0.01);
      CHECK(m.bary < 1e-4);
      CHECK(m.uv < 1e-4);
      CHECK(m.slope < 1e-3);
    }
    if (source != Source::Ray) raster.destroy(device);
  }

  gfx::destroy_compute_pipeline(device, probe);
  gfx::destroy_shader_module(device, probe_module);
  if (rays) {
    gfx::destroy_compute_pipeline(device, trace);
    gfx::destroy_shader_module(device, trace_module);
    gfx::destroy_buffer(device, visible);
    for (u32 i = 0; i < k_cases; ++i) {
      gfx::destroy_acceleration_structure(device, tlas[i]);
      gfx::destroy_acceleration_structure(device, blas[i]);
      gfx::destroy_buffer(device, tlas_instances[i]);
    }
  }
  bindless.destroy();
  frames.destroy();
  for (u32 i = 0; i < k_cases; ++i) {
    scenes[i].destroy(device);
    for (gfx::BufferResource* b :
         {&clusters[i], &triangles[i], &attributes[i], &floats[i], &indices16[i]})
      gfx::destroy_buffer(device, *b);
  }
  for (gfx::BufferResource* b : {&vis, &probe_out, &host, &params})
    gfx::destroy_buffer(device, *b);
  device.destroy();
}
