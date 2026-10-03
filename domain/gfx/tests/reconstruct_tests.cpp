// Where the resolve takes a pixel's point (docs/subsystems/gfx.md, "Where a pixel meets its
// triangle"). Large triangles around a walker looking down at his feet — one with a corner behind
// the camera's plane, one with two, one with a corner on it, a wall beside him that reaches behind
// him, and one wholly in front of him for a control — are drawn by every rasterizer the device has
// into the visibility buffer, and for every pixel each covers, what the resolve takes for it
// (material.slang, read out as floats by shaders/reconstruct_probe.slang: the barycentrics, the
// point and the UV interpolated at them, the UV's and the point's change per pixel, the view
// vector) is held to the CPU's ray through the pixel's centre against the triangle's plane
// (brdf_reference.h, `pixel_on_triangle`), with the derivatives by central differences of the same.
// Until 2026-10-03 the resolve took screen-space barycentrics of the corners divided by w and
// clamped them before the perspective correction: behind the plane the point was 1,279 footprints
// from where the pixel looks, the wall's 563, and on the plane 1.4 with the derivatives 2% off.
//
// Then the same triangles 2, 10 and 50 km from the origin, where a float's step is a quarter of a
// millimetre, two millimetres and four: the point, its place on its triangle and the view vector
// against the same ray, and at 50 km a millimetre's move of the camera with its rotation unchanged
// to the bit, which must move the point a pixel shades by the millimetre and nothing else.
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
#include <domain/gfx/view_ray.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/ray_visibility.spv.h>
#include <shaders/reconstruct_probe.spv.h>
#include <string>
#include <vector>

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
// float4s a pixel: barycentrics, point, UV and its x derivative, its y derivative, the point's two
// derivatives, the view vector, and the three corners as the shader fetched them.
constexpr u32 k_probe_rows = 10;

// One triangle and what the case says about it: how many of its corners are behind the camera's
// plane and how many are on it, which the case checks before it draws. Corners are around a walker
// standing at the origin of the case's own frame.
struct Case {
  const char* name;
  Vec3 corner[3];
  u32 behind;
  u32 on_plane;
};

const Case k_cases[] = {
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
    // A wall to the right, from behind his back to ten metres ahead, leaning out a little so that
    // its plane is not one of the axes'.
    {"a wall reaching behind",
     {Vec3{0.6f, -1.0f, 6.0f}, Vec3{0.9f, 4.0f, -10.0f}, Vec3{0.6f, -1.0f, -10.0f}},
     1,
     0},
    {"in front (the control)",
     {Vec3{-3.0f, 0.0f, -3.5f}, Vec3{3.0f, 0.0f, -3.5f}, Vec3{0.0f, 0.0f, 1.5f}},
     0,
     0},
};
constexpr u32 k_case_count = sizeof(k_cases) / sizeof(k_cases[0]);

enum class Source : u8 { Vertex, Mesh, Ray };

std::string source_name(Source s) {
  return s == Source::Vertex ? "the vertex path" : (s == Source::Mesh ? "the mesh path" : "rays");
}

f64 length(ref::Dvec3 v) { return std::sqrt(ref::dot(v, v)); }

ref::Dvec3 dvec(f32 x, f32 y, f32 z) {
  return ref::Dvec3{static_cast<f64>(x), static_cast<f64>(y), static_cast<f64>(z)};
}

// A walker's eyes looking 60 degrees down along -z from `eye`, with a 60 degree field of view. The
// view matrix is the rotation of `look_at` at the origin with this eye's translation put in after,
// so two cameras at different eyes have the same rotation to the bit.
struct Camera {
  Vec3 eye;
  Vec3 forward{0.0f, -0.8660254f, -0.5f};
  f32 fov_y = radians(60.0f);
  Mat4 projection;
  Mat4 view;
  Mat4 view_proj;
};

//  turned by yaw radians about +y: the far case turns the walker and his triangles together.
Vec3 turn(Vec3 v, f32 yaw) {
  const f32 c = std::cos(yaw);
  const f32 s = std::sin(yaw);
  return Vec3{c * v.x + s * v.z, v.y, c * v.z - s * v.x};
}

Camera walker(Vec3 eye, f32 yaw = 0.0f) {
  Camera c;
  c.eye = eye;
  c.forward = turn(c.forward, yaw);
  c.projection = perspective_reversed_z(c.fov_y, 1.0f, 0.05f);
  c.view = look_at(Vec3{0.0f, 0.0f, 0.0f}, c.forward, Vec3{0.0f, 1.0f, 0.0f});
  for (u32 r = 0; r < 3; ++r) {
    c.view.at(r, 3) =
        -(c.view.at(r, 0) * eye.x + c.view.at(r, 1) * eye.y + c.view.at(r, 2) * eye.z);
  }
  c.view_proj = c.projection * c.view;
  return c;
}

// The CPU's ray through pixel centre (sx, sy) of a camera, in double.
ref::Dvec3 ray_of(const Camera& c, f64 sx, f64 sy) {
  const ref::Dvec3 eye = ref::dvec3(c.eye);
  return ref::pixel_direction(eye, eye + ref::dvec3(c.forward), ref::Dvec3{0.0, 1.0, 0.0},
                              static_cast<f64>(c.fov_y), 1.0, k_size, k_size, sx, sy);
}

// What the probe wrote for one pixel.
struct ProbePixel {
  bool covered = false;
  ref::Dvec3 b;
  ref::Dvec3 position;
  f64 u = 0.0, v = 0.0;
  ref::Dvec3 duv_dx;  // z unused
  ref::Dvec3 duv_dy;
  ref::Dvec3 dp_dx;
  ref::Dvec3 dp_dy;
  ref::Dvec3 view;
  ref::Dvec3 corner[3];
};

// The device, the probe and the ray path, and the five triangles as meshes at some offset from the
// origin: everything a case draws with but the camera.
class Rig {
 public:
  bool open(gfx::Device& device, std::string& error) {
    device_ = &device;
    if (!bindless_.create(device, gfx::BindlessConfig{}, &error)) return false;
    if (!frames_.create(device, 2, &error)) return false;
    probe_module_ = gfx::create_shader_module(device, shaders::k_reconstruct_probe_spirv,
                                              shaders::k_reconstruct_probe_spirv_size, &error);
    if (!probe_module_.valid()) return false;
    const gfx::DescriptorSetLayoutHandle set_layout = bindless_.layout();
    if (!gfx::create_compute_pipeline(
            device, probe_module_, "probe_main",
            std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1), sizeof(u64), probe_,
            &error)) {
      return false;
    }
    sources_[source_count_++] = Source::Vertex;
    if (gfx_test::part(device, "the mesh path", {gfx_test::Need::MeshShader})) {
      sources_[source_count_++] = Source::Mesh;
    }
    rays_ = gfx_test::part(device, "the ray path",
                           {gfx_test::Need::AccelerationStructure, gfx_test::Need::RayQuery}) &&
            bindless_.has_acceleration_structures();
    if (rays_) {
      sources_[source_count_++] = Source::Ray;
      const u32 entry[2] = {0, 0};  // {instance 0, cluster 0}: the structure's one geometry
      if (!gfx::upload_buffer(device, entry, sizeof(entry), gfx::BufferUsage::Storage, visible_,
                              &error)) {
        return false;
      }
      trace_module_ = gfx::create_shader_module(device, shaders::k_ray_visibility_spirv,
                                                shaders::k_ray_visibility_spirv_size, &error);
      if (!trace_module_.valid()) return false;
      if (!gfx::create_compute_pipeline(
              device, trace_module_, "trace_main",
              std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1), sizeof(u64), trace_,
              &error)) {
        return false;
      }
    }
    constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
    const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
    probe_bytes_ = u64{k_size} * k_size * k_probe_rows * sizeof(Vec4);
    return gfx::create_buffer(
               device, vis_bytes,
               k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferDst,
               false, vis_, &error) &&
           gfx::create_buffer(
               device, probe_bytes_,
               k_storage | gfx::BufferUsage::ShaderDeviceAddress | gfx::BufferUsage::TransferSrc,
               false, probe_out_, &error) &&
           gfx::create_buffer(device, probe_bytes_, gfx::BufferUsage::TransferDst, true, host_,
                              &error) &&
           gfx::create_buffer(device, sizeof(ProbeParams) + sizeof(gfx::RayVisibilityParams),
                              k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params_,
                              &error);
  }

  u32 source_count() const { return source_count_; }
  Source source(u32 i) const { return sources_[i]; }

  // The five triangles, each its own mesh with its UVs the corners' own barycentrics (1, 0),
  // (0, 1), (0, 0), so that the interpolated UV is two of the barycentrics and its derivatives
  // theirs; `offset` added to every corner, and a structure of each for the ray path.
  bool build(Vec3 offset, std::string& error, f32 yaw = 0.0f) {
    release();
    const gfx::Device& device = *device_;
    constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
    const Vec3 normals[3] = {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                             Vec3{0.0f, 1.0f, 0.0f}};
    const Vec2 uvs[3] = {Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{0.0f, 0.0f}};
    const u32 indices[3] = {0, 1, 2};
    for (u32 i = 0; i < k_case_count; ++i) {
      Vec3 corners[3];
      for (u32 k = 0; k < 3; ++k)
        corners[k] = turn(k_cases[i].corner[k], yaw) + offset;
      geometry::AttributeSource attrs;
      attrs.normals = normals;
      attrs.uvs = uvs;
      if (!geometry::build_clusters(corners, indices, geometry::ClusterBuildOptions{}, meshes_[i],
                                    &error, attrs)) {
        return false;
      }
      if (meshes_[i].clusters.size() != 1u || meshes_[i].clusters[0].triangle_count != 1u) {
        error = "a case's triangle is not one cluster of one triangle";
        return false;
      }
      Vector<u16> expanded;
      gfx::expand_packed_triangles(std::span<const u32>(meshes_[i].triangles.data(), 1), expanded);
      if (!gfx::upload_buffer(device, meshes_[i].clusters.data(), sizeof(geometry::ClusterDesc),
                              k_storage, clusters_[i], &error) ||
          !scenes_[i].create(device, meshes_[i], 1, &error) ||
          !gfx::upload_buffer(device, meshes_[i].triangles.data(),
                              meshes_[i].triangles.size() * sizeof(u32), k_storage, triangles_[i],
                              &error) ||
          !gfx::upload_buffer(device, meshes_[i].attributes.data(),
                              meshes_[i].attributes.size() * sizeof(geometry::VertexAttributes),
                              k_storage, attributes_[i], &error) ||
          !gfx::upload_buffer(device, meshes_[i].vertices.data(),
                              meshes_[i].vertices.size() * sizeof(Vec3),
                              k_storage | gfx::k_build_input_usage, floats_[i], &error) ||
          !gfx::upload_buffer(device, expanded.data(), expanded.size() * sizeof(u16),
                              gfx::k_build_input_usage, indices16_[i], &error)) {
        return false;
      }
      if (rays_ && !build_structure(i, error)) return false;
    }
    built_ = true;
    return true;
  }

  // Draws case `ci` from `camera` with `source`, runs the probe over the visibility buffer, and
  // reads it back: one row of `k_probe_rows` float4s a pixel, row-major from the top.
  const Vec4* draw(u32 ci, const Camera& camera, Source source, std::string& error) {
    const gfx::Device& device = *device_;
    gfx_test::ClusterRaster raster;
    if (source != Source::Ray &&
        !raster.create(
            device,
            source == Source::Mesh ? gfx_test::RasterPath::Mesh : gfx_test::RasterPath::Vertex,
            bindless_.pipeline_layout(), geometry::ClusterBuildOptions{}.max_triangles, &error)) {
      return nullptr;
    }
    ProbeParams p{};
    p.camera = Vec4{camera.eye, 0.0f};
    p.view_proj = camera.view_proj;
    p.visibility = vis_.address;
    p.clusters = clusters_[ci].address;
    p.mesh = scenes_[ci].meshes.address;
    p.triangles = triangles_[ci].address;
    p.instances = scenes_[ci].instances.address;
    p.attributes = attributes_[ci].address;
    p.out = probe_out_.address;
    p.width = k_size;
    p.height = k_size;
    std::memcpy(params_.mapped, &p, sizeof(p));
    if (source == Source::Ray) {
      gfx::RayVisibilityParams ray{};
      ray.view_proj = camera.view_proj;
      ray.clip_to_ray = gfx::clip_to_ray(camera.projection, camera.view);
      ray.camera = Vec4{camera.eye, 0.0f};
      ray.output = vis_.address;
      ray.width = k_size;
      ray.height = k_size;
      ray.scene = scene_slot_[ci];
      ray.visible = visible_.address;
      ray.instances = scenes_[ci].instances.address;
      ray.meshes = scenes_[ci].meshes.address;
      std::memcpy(static_cast<u8*>(params_.mapped) + sizeof(ProbeParams), &ray, sizeof(ray));
    }
    gfx::ClusterDrawParams drawn{};
    drawn.view_proj = camera.view_proj;
    drawn.clusters = clusters_[ci].address;
    drawn.mesh = scenes_[ci].meshes.address;
    drawn.instances = scenes_[ci].instances.address;
    drawn.triangles = triangles_[ci].address;
    drawn.visibility = vis_.address;
    drawn.width = k_size;
    drawn.height = k_size;
    const u64 probe_address = params_.address;
    const u64 trace_address = params_.address + sizeof(ProbeParams);

    gfx::RenderGraph graph(device);
    const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis_);
    const gfx::RgBuffer rg_out = graph.import_buffer("probe", probe_out_);
    const gfx::RgBuffer rg_host = graph.import_buffer("host", host_);
    graph.add_pass(
        "clear", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::TransferWrite); },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.fill_buffer(vis_.buffer, 0, gfx::k_whole_size, 0);
        });
    if (source == Source::Ray) {
      graph.add_pass(
          "trace", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) { b.write(rg_vis, gfx::Access::ComputeReadWrite); },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, trace_.pipeline);
            bindless_.bind(cb, gfx::BindPoint::Compute);
            cb.push_constants(trace_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
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
          [&](gfx::CommandList cb, gfx::RenderGraph&) { raster.draw(cb, bindless_, drawn, 1); });
    }
    graph.add_pass(
        "probe", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          b.read(rg_vis, gfx::Access::ComputeRead);
          b.write(rg_out, gfx::Access::ComputeWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, probe_.pipeline);
          bindless_.bind(cb, gfx::BindPoint::Compute);
          cb.push_constants(probe_.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
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
          cb.copy_buffer(probe_out_.buffer, host_.buffer, gfx::BufferCopy{0, 0, probe_bytes_});
        });
    const Vec4* rows = nullptr;
    if (graph.compile(&error)) {
      gfx::CommandList commands = frames_.begin_frame();
      graph.execute(commands);
      if (frames_.wait(frames_.end_frame())) rows = static_cast<const Vec4*>(host_.mapped);
    }
    graph.reset();
    if (source != Source::Ray) raster.destroy(device);
    return rows;
  }

  const geometry::ClusterMesh& mesh(u32 ci) const { return meshes_[ci]; }

  void destroy() {
    release();
    const gfx::Device& device = *device_;
    gfx::destroy_compute_pipeline(device, probe_);
    gfx::destroy_shader_module(device, probe_module_);
    if (rays_) {
      gfx::destroy_compute_pipeline(device, trace_);
      gfx::destroy_shader_module(device, trace_module_);
      gfx::destroy_buffer(device, visible_);
    }
    for (gfx::BufferResource* b : {&vis_, &probe_out_, &host_, &params_})
      gfx::destroy_buffer(device, *b);
    bindless_.destroy();
    frames_.destroy();
  }

 private:
  bool build_structure(u32 i, std::string& error) {
    const gfx::Device& device = *device_;
    gfx::ClusterGeometry g;
    g.vertices = floats_[i].address;
    g.vertex_count = meshes_[i].clusters[0].vertex_count;
    g.indices = indices16_[i].address;
    g.triangle_count = 1;
    if (!gfx::create_blas(device, std::span<const gfx::ClusterGeometry>(&g, 1),
                          gfx::k_build_fast_trace, blas_[i], &error) ||
        !gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas_[i], &error)) {
      return false;
    }
    gfx::BufferResource scratch;
    if (!gfx::create_scratch(device,
                             std::max(blas_[i].build_scratch_bytes, tlas_[i].build_scratch_bytes),
                             scratch, &error) ||
        !gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                            tlas_instances_[i], &error)) {
      return false;
    }
    gfx::TlasInstance instance;
    instance.blas = blas_[i].address;
    gfx::write_instances(std::span<const gfx::TlasInstance>(&instance, 1),
                         tlas_instances_[i].mapped);
    const bool built = gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          gfx::build_blas(cb, blas_[i], std::span<const gfx::ClusterGeometry>(&g, 1),
                          gfx::k_build_fast_trace, scratch);
          gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                          gfx::MemoryAccess::AccelerationStructureRead);
          gfx::build_tlas(cb, tlas_[i], tlas_instances_[i].address, 1, gfx::k_build_fast_trace,
                          scratch);
          gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                          gfx::MemoryAccess::AccelerationStructureRead);
        },
        &error);
    gfx::destroy_buffer(device, scratch);
    if (!built) return false;
    scene_slot_[i] = bindless_.add_acceleration_structure(tlas_[i].handle);
    if (scene_slot_[i] == gfx::BindlessSet::k_invalid_slot) {
      error = "no bindless slot for a top-level structure";
      return false;
    }
    return true;
  }

  void release() {
    if (!built_) return;
    const gfx::Device& device = *device_;
    for (u32 i = 0; i < k_case_count; ++i) {
      scenes_[i].destroy(device);
      for (gfx::BufferResource* b :
           {&clusters_[i], &triangles_[i], &attributes_[i], &floats_[i], &indices16_[i]})
        gfx::destroy_buffer(device, *b);
      if (rays_) {
        bindless_.release_acceleration_structure(scene_slot_[i], 0);
        gfx::destroy_acceleration_structure(device, tlas_[i]);
        gfx::destroy_acceleration_structure(device, blas_[i]);
        gfx::destroy_buffer(device, tlas_instances_[i]);
      }
    }
    if (rays_) bindless_.recycle(0);
    built_ = false;
  }

  gfx::Device* device_ = nullptr;
  gfx::BindlessSet bindless_;
  gfx::FrameContext frames_;
  gfx::ShaderModuleHandle probe_module_ = {};
  gfx::ComputePipeline probe_;
  gfx::ShaderModuleHandle trace_module_ = {};
  gfx::ComputePipeline trace_;
  gfx::BufferResource visible_;
  gfx::BufferResource vis_;
  gfx::BufferResource probe_out_;
  gfx::BufferResource host_;
  gfx::BufferResource params_;
  u64 probe_bytes_ = 0;
  Source sources_[3] = {};
  u32 source_count_ = 0;
  bool rays_ = false;
  bool built_ = false;
  geometry::ClusterMesh meshes_[k_case_count];
  gfx_test::SingleInstance scenes_[k_case_count];
  gfx::BufferResource clusters_[k_case_count];
  gfx::BufferResource triangles_[k_case_count];
  gfx::BufferResource attributes_[k_case_count];
  gfx::BufferResource floats_[k_case_count];
  gfx::BufferResource indices16_[k_case_count];
  gfx::AccelerationStructure blas_[k_case_count];
  gfx::AccelerationStructure tlas_[k_case_count];
  gfx::BufferResource tlas_instances_[k_case_count];
  u32 scene_slot_[k_case_count] = {};
};

ProbePixel probe_pixel(const Vec4* rows, u32 x, u32 y) {
  const Vec4* r = rows + (u64{y} * k_size + x) * k_probe_rows;
  ProbePixel p;
  p.covered = r[0].w != 0.0f;
  if (!p.covered) return p;
  p.b = ref::dvec3(r[0]);
  p.position = ref::dvec3(r[1]);
  p.u = static_cast<f64>(r[2].x);
  p.v = static_cast<f64>(r[2].y);
  p.duv_dx = dvec(r[2].z, r[2].w, 0.0f);
  p.duv_dy = dvec(r[3].x, r[3].y, 0.0f);
  p.dp_dx = ref::dvec3(r[4]);
  p.dp_dy = ref::dvec3(r[5]);
  p.view = ref::dvec3(r[6]);
  for (u32 k = 0; k < 3; ++k)
    p.corner[k] = ref::dvec3(r[7 + k]);
  return p;
}

// A point of a triangle from its barycentrics, as a change from its first corner: in double, from
// the corners the shader fetched.
ref::Dvec3 point_at(const ProbePixel& p, ref::Dvec3 b) {
  return p.corner[0] + (p.corner[1] - p.corner[0]) * b.y + (p.corner[2] - p.corner[0]) * b.z;
}

// The angle between two directions, radians.
f64 angle(ref::Dvec3 a, ref::Dvec3 b) {
  return std::atan2(length(ref::cross(a, b)), ref::dot(a, b));
}

// A float's step at `x`: what one rounding of a position there can be off by, twice over.
f64 float_step(f64 x) {
  const f32 f = static_cast<f32>(std::abs(x));
  return static_cast<f64>(std::nextafter(f, 2.0f * f + 1.0f) - f);
}

}  // namespace

TEST_CASE(
    "material resolve: a pixel's point is its ray against its triangle, wherever its corners are") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // A walker's eyes 1.6 m over the ground at the origin, looking 60 degrees down along -z: the
  // camera's plane meets the ground 2.77 m behind him (h tan 60), and a triangle reaching past
  // that has a corner the rasterizer clips and a divide by w puts on the far side of the screen.
  const Camera camera = walker(Vec3{0.0f, 1.6f, 0.0f});
  const ref::Dvec3 eye = ref::dvec3(camera.eye);
  const ref::Dvec3 forward = ref::normalize(ref::dvec3(camera.forward));
  for (const Case& c : k_cases) {
    u32 behind = 0;
    u32 on_plane = 0;
    for (const Vec3& p : c.corner) {
      const f64 ahead = ref::dot(ref::dvec3(p) - eye, forward);
      // The plane is clip w = 0, and behind it w is negative.
      const Vec4 clip = camera.view_proj * Vec4{p, 1.0f};
      if (std::abs(ahead) < 1e-5) {
        ++on_plane;
        CHECK(std::abs(clip.w) < 1e-5f);
      } else {
        behind += ahead < 0.0 ? 1u : 0u;
        CHECK((clip.w < 0.0f) == (ahead < 0.0));
      }
    }
    REQUIRE_MESSAGE(behind == c.behind, std::string(c.name));
    REQUIRE_MESSAGE(on_plane == c.on_plane, std::string(c.name));
  }

  Rig rig;
  REQUIRE_MESSAGE(rig.open(device, error), error);
  REQUIRE_MESSAGE(rig.build(Vec3{0.0f, 0.0f, 0.0f}, error), error);
  for (u32 si = 0; si < rig.source_count(); ++si) {
    const Source source = rig.source(si);
    for (u32 ci = 0; ci < k_case_count; ++ci) {
      const Case& c = k_cases[ci];
      const Vec4* rows = rig.draw(ci, camera, source, error);
      REQUIRE_MESSAGE(rows != nullptr, error);
      // The UV each corner carries, in the cluster's own order, which is the order the shader
      // fetched the corners in.
      const geometry::ClusterMesh& mesh = rig.mesh(ci);
      const geometry::ClusterDesc& cluster = mesh.clusters[0];
      const u32 packed = mesh.triangles[cluster.triangle_offset];
      Vec2 uv[3];
      for (u32 k = 0; k < 3; ++k) {
        uv[k] = geometry::decode_half2(
            mesh.attributes[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k)]
                .uv_half2);
      }
      const auto uv_at = [&](ref::Dvec3 b, u32 axis) {
        const auto of = [&](u32 k) { return static_cast<f64>(axis == 0 ? uv[k].x : uv[k].y); };
        return b.x * of(0) + b.y * of(1) + b.z * of(2);
      };
      ref::Dvec3 corner[3];
      for (u32 k = 0; k < 3; ++k)
        corner[k] = ref::dvec3(c.corner[k]);
      u32 covered = 0;
      u32 clamped = 0;
      u32 outside = 0;  // pixels whose ray misses the triangle by more than a snap could explain
      u32 missing = 0;  // pixels whose ray lands well inside it and that nothing drew
      f64 worst_point = 0.0;  // in the pixel's own footprints
      f64 worst_bary = 0.0;
      f64 worst_uv = 0.0;
      f64 worst_slope = 0.0;  // relative to the derivative's length
      for (u32 y = 0; y < k_size; ++y) {
        for (u32 x = 0; x < k_size; ++x) {
          const f64 cx = static_cast<f64>(x) + 0.5;
          const f64 cy = static_cast<f64>(y) + 0.5;
          const ProbePixel got = probe_pixel(rows, x, y);
          // The corners the shader read are the case's own here, to the grid's rounding.
          const ref::Dvec3* tri = got.covered ? got.corner : corner;
          const ref::TriangleHit hit = ref::pixel_on_triangle(eye, ray_of(camera, cx, cy), tri);
          const bool in_front = ref::dot(hit.position - eye, forward) > 0.06;
          const f64 margin = std::min(hit.ray.x, std::min(hit.ray.y, hit.ray.z));
          if (!got.covered) {
            // A hit a hundredth of the triangle inside its edges, in front of the near plane:
            // something must have drawn it.
            if (in_front && margin > 0.01) ++missing;
            continue;
          }
          ++covered;
          if (hit.clamped) ++clamped;
          if (margin < -1e-3) ++outside;
          // The footprint: the point's change per pixel, from the ray's barycentrics' central
          // differences, which is the scale every difference below is read against.
          ref::Dvec3 db_dx;
          ref::Dvec3 db_dy;
          ref::pixel_barycentric_gradients(
              eye, eye + ref::dvec3(camera.forward), ref::Dvec3{0.0, 1.0, 0.0},
              static_cast<f64>(camera.fov_y), 1.0, k_size, k_size, cx, cy, tri, db_dx, db_dy);
          const ref::Dvec3 dp_dx = tri[0] * db_dx.x + tri[1] * db_dx.y + tri[2] * db_dx.z;
          const ref::Dvec3 dp_dy = tri[0] * db_dy.x + tri[1] * db_dy.y + tri[2] * db_dy.z;
          const f64 footprint = std::max(length(dp_dx), length(dp_dy));
          worst_bary = std::max(worst_bary, length(got.b - hit.b));
          worst_point = std::max(worst_point, length(got.position - hit.position) / footprint);
          worst_uv = std::max(worst_uv, std::max(std::abs(got.u - uv_at(hit.b, 0)),
                                                 std::abs(got.v - uv_at(hit.b, 1))));
          const auto relative = [](ref::Dvec3 g, ref::Dvec3 want) {
            return length(g - want) / std::max(length(want), 1e-9);
          };
          const ref::Dvec3 duv_dx{uv_at(db_dx, 0), uv_at(db_dx, 1), 0.0};
          const ref::Dvec3 duv_dy{uv_at(db_dy, 0), uv_at(db_dy, 1), 0.0};
          worst_slope =
              std::max({worst_slope, relative(got.duv_dx, duv_dx), relative(got.duv_dy, duv_dy),
                        relative(got.dp_dx, dp_dx), relative(got.dp_dy, dp_dy)});
        }
      }
      MESSAGE(
          std::string(c.name) << ", " << source_name(source) << ": " << covered << " pixels drawn ("
                              << clamped << " with the ray a snap outside, " << outside
                              << " further), " << missing << " missing; worst point " << worst_point
                              << " of a footprint, barycentric " << worst_bary << ", uv "
                              << worst_uv << ", derivative " << worst_slope << " of its length");
      // Most of the picture: every case puts its triangle under much of the 128 x 128 pixels.
      CHECK(covered > k_size * k_size / 8);
      CHECK(missing == 0u);
      CHECK(outside == 0u);
      // A point a hundredth of a pixel from where the pixel looks, and derivatives a thousandth
      // of their own length from the ray's; measured, both are float rounding (the message above).
      CHECK(worst_point < 0.01);
      CHECK(worst_bary < 1e-4);
      CHECK(worst_uv < 1e-4);
      CHECK(worst_slope < 1e-3);
    }
  }
  rig.destroy();
  device.destroy();
}

TEST_CASE("material resolve: a pixel's point holds its precision far from the origin") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  Rig rig;
  REQUIRE_MESSAGE(rig.open(device, error), error);
  // The device's own raster path: the arithmetic under test is the same whoever drew the pixel.
  const Source source = rig.source(rig.source_count() > 1 && rig.source(1) == Source::Mesh ? 1 : 0);

  // The walker and his triangles moved along x, so the eye's other two coordinates stay small and
  // a millimetre's move across the ground is a float's own: the cameras below differ in z alone.
  // Turned 35 degrees, so that the eye's large coordinate is in every row of the view: looking
  // straight along -z it would be in the first alone, whose translation a move in z leaves alone.
  const f32 yaw = radians(35.0f);
  const f32 distances[] = {0.0f, 2000.0f, 10000.0f, 50000.0f};
  for (const f32 distance : distances) {
    REQUIRE_MESSAGE(rig.build(Vec3{distance, 0.0f, 0.0f}, error, yaw), error);
    const Camera camera = walker(Vec3{distance, 1.6f, 0.0f}, yaw);
    const Camera moved = walker(Vec3{distance, 1.6f, -0.001f}, yaw);
    // A float's step at the eye: a quarter of a millimetre at 2 km, a millimetre at 10, four at 50.
    const f64 step = float_step(std::max(static_cast<f64>(distance), 1.6));
    f64 worst_position = 0.0;  // the world position the resolve hands its readers, metres
    f64 worst_point = 0.0;     // the point its barycentrics name on the triangle, metres
    f64 worst_view = 0.0;      // the view vector, radians
    f64 worst_move = 0.0;      // the point's move under the camera's, less the camera's, metres
    u32 compared = 0;
    for (u32 ci = 0; ci < k_case_count; ++ci) {
      std::vector<ProbePixel> before(u64{k_size} * k_size);
      for (u32 pass = 0; pass < 2; ++pass) {
        const Camera& cam = pass == 0 ? camera : moved;
        const ref::Dvec3 eye = ref::dvec3(cam.eye);
        const Vec4* rows = rig.draw(ci, cam, source, error);
        REQUIRE_MESSAGE(rows != nullptr, error);
        for (u32 y = 0; y < k_size; ++y) {
          for (u32 x = 0; x < k_size; ++x) {
            const ProbePixel got = probe_pixel(rows, x, y);
            if (!got.covered) {
              if (pass == 0) before[y * k_size + x] = got;
              continue;
            }
            const ref::Dvec3 ray =
                ray_of(cam, static_cast<f64>(x) + 0.5, static_cast<f64>(y) + 0.5);
            const ref::TriangleHit hit = ref::pixel_on_triangle(eye, ray, got.corner);
            if (pass == 0) {
              before[y * k_size + x] = got;
              ++compared;
              worst_position = std::max(worst_position, length(got.position - hit.position));
              worst_point = std::max(worst_point, length(point_at(got, got.b) - hit.position));
              worst_view =
                  std::max(worst_view, angle(got.view, ref::normalize(eye - hit.position)));
            } else if (before[y * k_size + x].covered) {
              // How far the pixel's point moved on its triangle, against how far its ray's hit did.
              const ProbePixel& was = before[y * k_size + x];
              const ref::TriangleHit hit_was = ref::pixel_on_triangle(
                  ref::dvec3(camera.eye),
                  ray_of(camera, static_cast<f64>(x) + 0.5, static_cast<f64>(y) + 0.5), was.corner);
              const ref::Dvec3 shift = point_at(got, got.b) - point_at(was, was.b);
              worst_move = std::max(worst_move, length(shift - (hit.position - hit_was.position)));
            }
          }
        }
      }
    }
    MESSAGE(distance << " m from the origin, " << source_name(source) << ", " << compared
                     << " pixels: a float's step there " << step * 1000.0
                     << " mm; worst world position " << worst_position * 1000.0
                     << " mm, point on its triangle " << worst_point * 1000.0 << " mm, view vector "
                     << worst_view << " rad; a millimetre's move of the "
                     << "eye moves the point as it moves the ray's hit, to within "
                     << worst_move * 1000.0 << " mm");
    CHECK(compared > k_case_count * k_size * k_size / 8);
    // The world position is one rounding of the eye plus the point from it: under a float's step.
    CHECK(worst_position <= step + 1e-5);
    // Which point of its triangle a pixel shades, and where it is seen from, owe nothing to
    // where the camera stands.
    CHECK(worst_point <= 1e-4);
    CHECK(worst_view <= 1e-5);
    // Under a tenth of the float's step there, which the world-space arithmetic before 2026-10-03
    // missed by 66, 35 and 8 times at 2, 10 and 50 km (0.81, 3.4 and 3.1 mm; measured, RTX 5090),
    // and this meets with 5 micrometres at every distance.
    CHECK(worst_move <= std::max(step / 10.0, 1e-5));
  }
  rig.destroy();
  device.destroy();
}
