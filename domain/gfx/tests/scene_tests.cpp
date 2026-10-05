// A scene of instances of meshes, end to end (docs/plan/04-renderer.md §4.2). Two meshes — a
// terrain and a sphere — go into one set of buffers, each keeping its own 16-bit position grid,
// and five instances place them: one scaled three times uniformly, one scaled unevenly, one
// rotated a quarter turn, and one hidden behind another. The cull pass runs one thread per
// (instance, cluster) pair and its survivors must equal a CPU reference that does every test in
// the instance's frame; the mesh-shader and vertex paths must fill the visibility buffer
// identically; the ray-traced picture must match the rasterized one; and two-pass occlusion
// culling must drop the hidden instance without changing the picture. The vertex path is the
// reference every device draws; the mesh half needs VK_EXT_mesh_shader and the ray half
// VK_KHR_ray_query, and each is skipped by name where it is missing, while the cull, the
// instances and the occlusion half run on the baseline tier too.
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/hiz_build.spv.h>
#include <shaders/ray_visibility.spv.h>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_w = 384;
constexpr u32 k_h = 288;

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

// A unit sphere wound counter-clockwise seen from outside.
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
      indices.push_back(ring(r, s));
      indices.push_back(ring(r, s + 1));
      indices.push_back(ring(r + 1, s));
      indices.push_back(ring(r, s + 1));
      indices.push_back(ring(r + 1, s + 1));
      indices.push_back(ring(r + 1, s));
    }
  }
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(ring(rings - 1, s));
    indices.push_back(ring(rings - 1, s + 1));
    indices.push_back(bottom);
  }
}

// The CPU reference of the pair cull, done exactly as cluster_cull.slang does it: every test in
// the instance's frame, the cone only where the scale is uniform and the instance is rigid.
Vector<u32> reference_cut(const geometry::ClusterLodMesh& lod,
                          std::span<const geometry::ClusterMeshPart> parts,
                          std::span<const gfx::InstanceDesc> instances, const Frustum& frustum,
                          const geometry::LodView& view, bool cone_cull) {
  Vector<u32> pairs;
  for (u32 i = 0; i < instances.size(); ++i) {
    const gfx::InstanceDesc& instance = instances[i];
    const Mat4 world = gfx::instance_matrix(instance, WorldEye{});
    const geometry::ClusterMeshPart& part = parts[instance.mesh];
    for (u32 local = 0; local < part.cluster_count; ++local) {
      const u32 index = part.first_cluster + local;
      const geometry::ClusterDesc& c = lod.mesh.clusters[index];
      const Vec3 center = transform_point(world, c.center);
      const f32 radius = c.radius * instance.scale_max;
      if (!frustum_contains_sphere(frustum, center, radius)) continue;
      if (cone_cull && (instance.flags & gfx::k_instance_uniform_scale) != 0 &&
          instance.deform == gfx::k_invalid_deform) {
        geometry::ClusterDesc moved = c;
        moved.cone_apex = transform_point(world, c.cone_apex);
        const geometry::NormalCone cone = geometry::decode_cone(c.cone);
        const Vec3 axis = normalize(transform_direction(world, cone.axis));
        moved.cone = geometry::encode_cone(axis, cone.cutoff);
        // encode_cone rounds the cutoff up, which would cull less than the shader does; the
        // shader reads the stored byte, so put the original byte back.
        moved.cone = (moved.cone & 0x00ffffffu) | (c.cone & 0xff000000u);
        if (geometry::cluster_backfacing(moved, view.camera)) continue;
      }
      geometry::ClusterLodDesc scaled = lod.lod[index];
      const Vec3 own = transform_point(world, Vec3{scaled.own.x, scaled.own.y, scaled.own.z});
      const Vec3 parent_center =
          transform_point(world, Vec3{scaled.parent.x, scaled.parent.y, scaled.parent.z});
      scaled.own = Vec4{own, scaled.own.w * instance.scale_max};
      scaled.parent = Vec4{parent_center, scaled.parent.w * instance.scale_max};
      scaled.own_error *= instance.scale_max;
      if (scaled.parent_error < geometry::k_lod_terminal_error)
        scaled.parent_error *= instance.scale_max;
      if (!geometry::lod_selects(scaled, view)) continue;
      pairs.push_back(instance.first_pair + local);
    }
  }
  return pairs;
}

struct Scene {
  geometry::ClusterLodMesh lod;
  Vector<geometry::ClusterMeshPart> parts;
  Vector<gfx::InstanceDesc> instances;
  u32 pair_count = 0;
  u32 hidden_instance = 0;  // the one behind another
};

// Two meshes and five instances: the terrain at the origin, a sphere three times bigger, a
// sphere scaled unevenly, a terrain rotated a quarter turn, and a sphere hidden behind the big
// one from the camera.
bool build_scene(Scene& out, std::string& error) {
  Vector<Vec3> terrain_positions;
  Vector<u32> terrain_indices;
  make_terrain(65, 6.0f, terrain_positions, terrain_indices);  // 8,192 triangles
  Vector<Vec3> sphere_positions;
  Vector<u32> sphere_indices;
  make_sphere(24, 48, sphere_positions, sphere_indices);  // 2,208 triangles
  Vector<geometry::ClusterLodMesh> meshes(2);
  if (!geometry::build_cluster_lod(terrain_positions, terrain_indices,
                                   geometry::ClusterLodOptions{}, meshes[0], &error) ||
      !geometry::build_cluster_lod(sphere_positions, sphere_indices, geometry::ClusterLodOptions{},
                                   meshes[1], &error)) {
    return false;
  }
  if (!geometry::merge_cluster_meshes(meshes, out.lod, out.parts, &error)) return false;

  struct Placement {
    u32 mesh;
    Transform3 transform;
  };
  const Placement placements[5] = {
      {0, Transform3{Vec3{0.0f, -2.0f, 0.0f}, Quat::identity(), Vec3::one()}},
      {1, Transform3{Vec3{-6.0f, 1.0f, 2.0f}, Quat::identity(), Vec3{3.0f, 3.0f, 3.0f}}},
      {1, Transform3{Vec3{5.0f, 1.0f, 3.0f}, Quat::identity(), Vec3{2.2f, 0.8f, 1.5f}}},
      {0, Transform3{Vec3{0.0f, 4.0f, -9.0f}, quat_from_axis_angle(Vec3{0, 1, 0}, k_pi * 0.5f),
                     Vec3::one()}},
      // Twice as far along the camera ray through the big sphere's centre, and small enough to
      // sit inside its shadow: the occlusion pass must drop it.
      {1, Transform3{Vec3{-12.0f, -4.0f, -18.0f}, Quat::identity(), Vec3{1.6f, 1.6f, 1.6f}}},
  };
  for (const Placement& placement : placements) {
    gfx::InstanceDesc instance{};
    gfx::set_instance_transform(instance, mat4_from_transform(placement.transform));
    instance.mesh = placement.mesh;
    instance.first_pair = out.pair_count;
    out.pair_count += out.parts[placement.mesh].cluster_count;
    out.instances.push_back(instance);
  }
  out.hidden_instance = 4;
  return true;
}

f32 depth_of(u64 word) {
  const u32 bits = static_cast<u32>(word >> 32);
  f32 depth = 0.0f;
  std::memcpy(&depth, &bits, 4);
  return depth;
}

}  // namespace

TEST_CASE("scene: the pair cull, the two rasterizers, ray tracing, and occlusion over instances") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  // The vertex path is the reference every device draws; the mesh path is compared with it where
  // the device has mesh shaders, and the ray-traced picture where it has ray queries.
  const bool have_mesh =
      gfx_test::part(device, "the mesh path against the vertex path", {gfx_test::Need::MeshShader});
  const bool have_rt = gfx_test::part(device, "the ray-traced picture", {gfx_test::Need::RayQuery});

  Scene scene;
  REQUIRE_MESSAGE(build_scene(scene, error), error);
  const u32 cluster_count = scene.lod.mesh.clusters.size();
  const u32 instance_count = scene.instances.size();
  const u32 pair_count = scene.pair_count;
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
  MESSAGE("scene: " << scene.parts.size() << " meshes, " << cluster_count << " clusters, "
                    << instance_count << " instances, " << pair_count << " pairs");
  CHECK(scene.parts[0].quant_scale != scene.parts[1].quant_scale);  // one grid per mesh

  // The camera and the CPU reference cut it sees.
  const Vec3 eye{0.0f, 6.0f, 22.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(60.0f);
  const Mat4 projection = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear);
  const Mat4 eye_view = look_at(eye, Vec3{0.0f, 0.5f, 0.0f}, Vec3{0, 1, 0});
  const Mat4 view_proj = projection * eye_view;
  const Frustum frustum = frustum_from_view_proj(view_proj);
  geometry::LodView view;
  view.camera = eye;
  view.znear = znear;
  view.proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;
  view.threshold_px = 0.35f;  // a cut of a few hundred pairs, not a handful
  Vector<u32> expected =
      reference_cut(scene.lod, scene.parts, scene.instances, frustum, view, true);
  REQUIRE(expected.size() > 16);
  REQUIRE(expected.size() < pair_count);
  // Every instance must contribute, or the scene is not testing what it claims to.
  Vector<u32> per_instance(instance_count, 0u);
  for (const u32 pair : expected) {
    for (u32 i = instance_count; i-- > 0;) {
      if (pair >= scene.instances[i].first_pair) {
        ++per_instance[i];
        break;
      }
    }
  }
  for (u32 i = 0; i < instance_count; ++i)
    CHECK(per_instance[i] > 0);

  // Geometry and the scene on the GPU.
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  constexpr gfx::BufferUsage k_args = k_address | gfx::BufferUsage::Indirect |
                                      gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  Vector<gfx::MeshDesc> mesh_descs;
  gfx::BufferResource clusters;
  gfx::BufferResource lods;
  gfx::BufferResource quantized;
  gfx::BufferResource meshes;
  gfx::BufferResource instances;
  gfx::BufferResource triangles;
  gfx::BufferResource vertices;
  REQUIRE(gfx::upload_buffer(device, scene.lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, scene.lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));
  REQUIRE(gfx::upload_buffer(device, scene.lod.mesh.quantized.data(),
                             scene.lod.mesh.quantized.size() * sizeof(u16), k_storage, quantized,
                             &error));
  for (const geometry::ClusterMeshPart& part : scene.parts) {
    gfx::MeshDesc desc{};
    desc.quant = Vec4{part.quant_origin, part.quant_scale};
    desc.quantized = quantized.address;
    desc.first_cluster = part.first_cluster;
    desc.cluster_count = part.cluster_count;
    mesh_descs.push_back(desc);
  }
  REQUIRE(gfx::upload_buffer(device, mesh_descs.data(), mesh_descs.size() * sizeof(gfx::MeshDesc),
                             k_storage, meshes, &error));
  REQUIRE(gfx::upload_buffer(device, scene.instances.data(),
                             instance_count * sizeof(gfx::InstanceDesc), k_storage, instances,
                             &error));
  REQUIRE(gfx::upload_buffer(device, scene.lod.mesh.triangles.data(),
                             scene.lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));
  REQUIRE(gfx::upload_buffer(device, scene.lod.mesh.vertices.data(),
                             scene.lod.mesh.vertices.size() * sizeof(Vec3),
                             k_storage | gfx::k_build_input_usage, vertices, &error));

  // Frame resources. The visible list holds two runs, one per occlusion pass, so an id names an
  // entry of the whole list whichever pass drew the pixel.
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const u64 run_bytes = u64{pair_count} * 8;
  u32 hiz_offsets[gfx::k_hiz_max_mips];
  const u32 hiz_elements = gfx::hiz_layout(k_w, k_h, hiz_offsets);
  const u32 hiz_mips = gfx::hiz_mip_count(k_w, k_h);
  const gfx::BufferUsage k_vis =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  gfx::BufferResource vis[4];  // mesh, vertex, ray, occlusion
  gfx::BufferResource visible;
  gfx::BufferResource args[2];       // {vertexCount, survivors, 0, 0} for vkCmdDrawIndirect
  gfx::BufferResource mesh_args[2];  // {survivors, 1, 1}, copied from the word above
  gfx::BufferResource flags[2];
  gfx::BufferResource hiz;
  gfx::BufferResource params;  // three CullParams: single pass, occlusion pass 1, pass 2
  gfx::BufferResource ray_params;
  gfx::BufferResource host;
  for (gfx::BufferResource& v : vis)
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, v, &error));
  REQUIRE(gfx::create_buffer(device, run_bytes * 2, k_vis, false, visible, &error));
  for (u32 i = 0; i < 2; ++i) {
    REQUIRE(gfx::create_buffer(device, 16, k_args, false, args[i], &error));
    REQUIRE(gfx::create_buffer(device, 12, k_args, false, mesh_args[i], &error));
    REQUIRE(gfx::create_buffer(device, u64{pair_count} * 4,
                               k_address | gfx::BufferUsage::TransferDst, false, flags[i], &error));
  }
  REQUIRE(gfx::create_buffer(device, u64{hiz_elements} * 4,
                             k_address | gfx::BufferUsage::TransferDst, false, hiz, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams) * 3, k_address, true, params, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, ray_params,
                             &error));
  const u64 host_vis_offset[4] = {0, vis_bytes, vis_bytes * 2, vis_bytes * 3};
  const u64 host_list_offset = vis_bytes * 4;
  const u64 host_args_offset = host_list_offset + run_bytes * 2;
  REQUIRE(gfx::create_buffer(device, host_args_offset + 32, gfx::BufferUsage::TransferDst, true,
                             host, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ShaderModuleHandle cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  gfx::ShaderModuleHandle vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  gfx::ShaderModuleHandle hiz_module = gfx::create_shader_module(
      device, shaders::k_hiz_build_spirv, shaders::k_hiz_build_spirv_size, &error);
  REQUIRE(cull_module.valid());
  REQUIRE(vertex_module.valid());
  REQUIRE(hiz_module.valid());
  gfx::ComputePipeline cull_pipeline;
  gfx::ComputePipeline hiz_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               cull_pipeline, &error),
                  error);
  REQUIRE(gfx::create_compute_pipeline(device, hiz_module, "hiz_build_main", {},
                                       sizeof(gfx::HizParams), hiz_pipeline, &error));
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  gfx::PipelineHandle vertex_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
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

  gfx::CullParams base{};
  gfx::set_frustum(base, frustum);
  base.view_proj = view_proj;
  base.camera = Vec4{eye, znear};
  base.lod = Vec4{view.proj_scale, view.threshold_px, 1.0f, 1.0f};
  base.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
  base.cluster_count = cluster_count;
  base.cone_cull = 1;
  base.clusters = clusters.address;
  base.lods = lods.address;
  base.instances = instances.address;
  base.meshes = meshes.address;
  base.instance_count = instance_count;
  base.pair_count = pair_count;
  base.hiz_width = k_w;
  base.hiz_height = k_h;
  base.hiz_mips = hiz_mips;
  std::memcpy(base.hiz_offsets, hiz_offsets, sizeof(hiz_offsets));
  auto* blocks = static_cast<gfx::CullParams*>(params.mapped);
  gfx::CullParams single = base;
  single.count_index = 1;  // vkCmdDrawIndirect's instance count; the mesh path reads word 0 too
  single.visible = visible.address;
  single.draw_args = args[0].address;
  blocks[0] = single;

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = meshes.address;
  draw.instances = instances.address;
  draw.triangles = triangles.address;
  draw.visible = visible.address;
  draw.extent = gfx::draw_extent(k_w, k_h);
  // One block per visibility buffer: mesh, vertex, ray (unused), occlusion.
  gfx::ClusterDrawParams draw_to[4];
  for (u32 v = 0; v < 4; ++v) {
    draw_to[v] = draw;
    draw_to[v].visibility = vis[v].address;
  }

  const u64 block_address[3] = {params.address, params.address + sizeof(gfx::CullParams),
                                params.address + 2 * sizeof(gfx::CullParams)};

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[4];
  gfx::RgBuffer rg_visible;
  gfx::RgBuffer rg_args[2];
  gfx::RgBuffer rg_mesh_args[2];
  gfx::RgBuffer rg_flags[2];
  gfx::RgBuffer rg_hiz;
  gfx::RgBuffer rg_host;
  auto import_all = [&]() {
    for (u32 v = 0; v < 4; ++v)
      rg_vis[v] = graph.import_buffer("vis", vis[v]);
    rg_visible = graph.import_buffer("visible", visible);
    rg_hiz = graph.import_buffer("hiz", hiz);
    rg_host = graph.import_buffer("host", host);
    for (u32 i = 0; i < 2; ++i) {
      rg_args[i] = graph.import_buffer("args", args[i]);
      rg_mesh_args[i] = graph.import_buffer("mesh_args", mesh_args[i]);
      rg_flags[i] = graph.import_buffer("flags", flags[i]);
    }
  };
  // The cull counts survivors into vkCmdDrawIndirect's instance count; the mesh path wants the
  // same number as a task-group count, so it is copied across rather than culled twice.
  auto add_mesh_args = [&](u32 list) {
    graph.add_pass(
        "mesh args", gfx::PassKind::Transfer,
        [&, list](gfx::PassBuilder& b) {
          b.read(rg_args[list], gfx::Access::TransferRead);
          b.write(rg_mesh_args[list], gfx::Access::TransferWrite);
        },
        [&, list](gfx::CommandList cb, gfx::RenderGraph&) {
          const gfx::BufferCopy copy{sizeof(u32), 0, sizeof(u32)};
          cb.copy_buffer(args[list].buffer, mesh_args[list].buffer, copy);
        });
  };
  auto add_cull = [&](u32 block, u32 list) {
    graph.add_pass(
        "cull", gfx::PassKind::Compute,
        [&, list](gfx::PassBuilder& b) {
          b.write(rg_args[list], gfx::Access::ComputeReadWrite);
          b.write(rg_visible, gfx::Access::ComputeWrite);
          if (block != 0) {
            b.read(rg_hiz, gfx::Access::ComputeRead);
            for (u32 i = 0; i < 2; ++i)
              b.write(rg_flags[i], gfx::Access::ComputeReadWrite);
          }
        },
        [&, block](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, cull_pipeline.pipeline);
          cb.push_constants(cull_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &block_address[block]);
          cb.dispatch(gfx::cull_group_count(pair_count), 1, 1);
        });
  };
  auto add_draw = [&](u32 target, u32 list, bool use_mesh,
                      const gfx::ClusterDrawParams* draw_params) {
    graph.add_pass(
        "draw", gfx::PassKind::Raster,
        [&, target, list, use_mesh](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_vis[target], gfx::Access::FragmentReadWrite);
          b.read(use_mesh ? rg_mesh_args[list] : rg_args[list], gfx::Access::IndirectRead);
          b.read(rg_visible, use_mesh ? gfx::Access::MeshRead : gfx::Access::VertexRead);
        },
        [&, list, use_mesh, draw_params](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, use_mesh ? mesh_pipeline : vertex_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0,
                            sizeof(*draw_params), draw_params);
          if (use_mesh) {
            cb.draw_mesh_tasks_indirect(mesh_args[list].buffer, 0, 1, sizeof(u32) * 3);
          } else {
            cb.draw_indirect(args[list].buffer, 0, 1, sizeof(u32) * 4);
          }
        });
  };
  const u32 hiz_dispatches = gfx::hiz_dispatch_count(hiz_mips);
  Vector<gfx::HizParams> hiz_params(hiz_dispatches);
  auto add_hiz = [&](u32 source) {
    for (u32 d = 0; d < hiz_dispatches; ++d) {
      gfx::HizParams* level = &hiz_params[d];
      const u32 src_mip = gfx::hiz_dispatch_src_mip(d);
      *level = gfx::HizParams{};
      level->from_visibility = d == 0 ? 1u : 0u;
      level->src = d == 0 ? vis[source].address : hiz.address + u64{hiz_offsets[src_mip]} * 4;
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
          [&, d, source](gfx::PassBuilder& b) {
            if (d == 0) b.read(rg_vis[source], gfx::Access::ComputeRead);
            b.write(rg_hiz, gfx::Access::ComputeReadWrite);
          },
          [&, level, src_w, src_h](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, hiz_pipeline.pipeline);
            cb.push_constants(hiz_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(*level),
                              level);
            cb.dispatch(gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
          });
    }
  };
  auto add_reset = [&](bool clear_hiz, u32 clear_flags) {
    graph.add_pass(
        "reset", gfx::PassKind::Transfer,
        [&, clear_hiz, clear_flags](gfx::PassBuilder& b) {
          for (u32 v = 0; v < 4; ++v)
            b.write(rg_vis[v], gfx::Access::TransferWrite);
          for (u32 i = 0; i < 2; ++i) {
            b.write(rg_args[i], gfx::Access::TransferWrite);
            b.write(rg_mesh_args[i], gfx::Access::TransferWrite);
          }
          if (clear_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
          if (clear_flags < 2) b.write(rg_flags[clear_flags], gfx::Access::TransferWrite);
        },
        [&, clear_hiz, clear_flags](gfx::CommandList cb, gfx::RenderGraph&) {
          for (const gfx::BufferResource& v : vis)
            cb.fill_buffer(v.buffer, 0, gfx::k_whole_size, 0);
          for (u32 i = 0; i < 2; ++i) {
            // {vertexCount, instanceCount, firstVertex, firstInstance} and {groups, 1, 1}.
            cb.fill_buffer(args[i].buffer, 0, 4, triangles_per_cluster * 3);
            cb.fill_buffer(args[i].buffer, 4, 12, 0);
            cb.fill_buffer(mesh_args[i].buffer, 0, 4, 0);
            cb.fill_buffer(mesh_args[i].buffer, 4, 8, 1);
          }
          if (clear_hiz) cb.fill_buffer(hiz.buffer, 0, gfx::k_whole_size, 0);
          if (clear_flags < 2) cb.fill_buffer(flags[clear_flags].buffer, 0, gfx::k_whole_size, 0);
        });
  };
  auto add_readback = [&]() {
    graph.add_pass(
        "readback", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          for (u32 v = 0; v < 4; ++v)
            b.read(rg_vis[v], gfx::Access::TransferRead);
          b.read(rg_visible, gfx::Access::TransferRead);
          for (u32 i = 0; i < 2; ++i)
            b.read(rg_args[i], gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          for (u32 v = 0; v < 4; ++v) {
            const gfx::BufferCopy copy{0, host_vis_offset[v], vis_bytes};
            cb.copy_buffer(vis[v].buffer, host.buffer, copy);
          }
          const gfx::BufferCopy list_copy{0, host_list_offset, run_bytes * 2};
          cb.copy_buffer(visible.buffer, host.buffer, list_copy);
          for (u32 i = 0; i < 2; ++i) {
            const gfx::BufferCopy args_copy{0, host_args_offset + i * 16, 16};
            cb.copy_buffer(args[i].buffer, host.buffer, args_copy);
          }
        });
  };
  auto run_frame = [&]() {
    gfx::CommandList cb = frames.begin_frame();
    graph.execute(cb);
    REQUIRE(frames.wait(frames.end_frame()));
  };

  // Frame 1: one cull pass, then the vertex path and (where there are mesh shaders) the mesh
  // path from the same visible list.
  import_all();
  add_reset(true, 2);
  add_cull(0, 0);
  add_draw(1, 0, false, &draw_to[1]);
  if (have_mesh) {
    add_mesh_args(0);
    add_draw(0, 0, true, &draw_to[0]);
  }
  add_readback();
  REQUIRE_MESSAGE(graph.compile(&error), error);
  run_frame();

  // Later frames read back over the same host buffer, so this frame's pictures and its visible
  // list are copied out before anything else runs.
  const auto* host_bytes = static_cast<const u8*>(host.mapped);
  const auto* args_out = reinterpret_cast<const u32*>(host_bytes + host_args_offset);
  const u32 drawn = args_out[1];
  Vector<u64> mesh_pixels(k_w * k_h);
  Vector<u64> vertex_pixels(k_w * k_h);
  Vector<u32> list_entries(pair_count * 4);
  std::memcpy(mesh_pixels.data(), host_bytes + host_vis_offset[0], vis_bytes);
  std::memcpy(vertex_pixels.data(), host_bytes + host_vis_offset[1], vis_bytes);
  std::memcpy(list_entries.data(), host_bytes + host_list_offset, run_bytes * 2);
  const u64* mesh_out = mesh_pixels.data();
  const u64* vertex_out = vertex_pixels.data();
  const u32* list_out = list_entries.data();

  // The GPU's survivors are exactly the CPU reference's pairs.
  CHECK(drawn == expected.size());
  Vector<u32> got;
  for (u32 i = 0; i < drawn && i < pair_count; ++i) {
    const u32 instance_index = list_out[i * 2];
    const u32 cluster = list_out[i * 2 + 1];
    REQUIRE(instance_index < instance_count);
    const gfx::InstanceDesc& instance = scene.instances[instance_index];
    const geometry::ClusterMeshPart& part = scene.parts[instance.mesh];
    REQUIRE(cluster >= part.first_cluster);
    REQUIRE(cluster < part.first_cluster + part.cluster_count);
    got.push_back(instance.first_pair + (cluster - part.first_cluster));
  }
  std::sort(got.begin(), got.end());
  std::sort(expected.begin(), expected.end());
  u32 mismatched = 0;
  for (u32 i = 0; i < got.size() && i < expected.size(); ++i)
    mismatched += got[i] != expected[i];
  CHECK(mismatched == 0);
  MESSAGE("cull: " << drawn << " of " << pair_count << " pairs, reference " << expected.size());

  // The two rasterizers write the same words.
  u32 covered = 0;
  u32 coverage_mismatch = 0;
  u32 id_mismatch = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const bool in_vertex = vertex_out[i] != 0;
    covered += in_vertex;
    if (have_mesh) {
      const bool in_mesh = mesh_out[i] != 0;
      if (in_mesh != in_vertex) ++coverage_mismatch;
      if (in_mesh && in_vertex && static_cast<u32>(mesh_out[i]) != static_cast<u32>(vertex_out[i]))
        ++id_mismatch;
    }
  }
  CHECK(covered > k_w * k_h / 16);
  if (have_mesh) {
    CHECK(coverage_mismatch == 0);
    CHECK(id_mismatch == 0);
  }
  MESSAGE("covered " << covered << " px; mesh vs vertex coverage mismatch " << coverage_mismatch
                     << ", id mismatch " << id_mismatch);
  // Every instance in the reference set put pixels on the screen, so the transforms reach the
  // rasterizer and not only the cull pass.
  // A pixel's id is the scene's pair (gfx.md, "The tie rule"), whose instance is the last one whose
  // first pair is at or below it.
  auto instance_of_id = [&](u32 id) {
    const u32 pair = id >> 8;
    u32 k = 0;
    while (k + 1 < instance_count && scene.instances[k + 1].first_pair <= pair)
      ++k;
    return k;
  };
  Vector<u32> pixels_per_instance(instance_count, 0u);
  for (u32 i = 0; i < k_w * k_h; ++i) {
    if (vertex_out[i] == 0) continue;
    const u32 id = static_cast<u32>(vertex_out[i]);
    if ((id >> 8) < pair_count) ++pixels_per_instance[instance_of_id(id)];
  }
  for (u32 i = 0; i < instance_count; ++i) {
    if (i != scene.hidden_instance) CHECK(pixels_per_instance[i] > 0);
  }

  // The ray-traced picture: one bottom-level structure per instance, one geometry per visible
  // cluster of that instance in visible-list order, and a top-level instance carrying the
  // world transform, so GeometryIndex plus the instance's base is the visible entry.
  if (have_rt) {
    Vector<u32> instance_geometries(instance_count, 0u);
    Vector<u32> instance_base(instance_count, 0u);
    Vector<Vector<u32>> entries_of(instance_count);
    for (u32 i = 0; i < drawn; ++i)
      entries_of[list_out[i * 2]].push_back(i);
    Vector<u16> expanded;
    Vector<u32> expanded_offset;
    Vector<gfx::ClusterGeometry> geometries;
    // A bottom-level structure's geometries are numbered from 0, so the instances have to be
    // grouped: geometry g of instance k is the (base[k] + g)-th of the whole scene. The cull's
    // atomics put the entries in another order, so that index is a permutation of the visible
    // entry rather than the entry itself, and `geometry_entry` is the map back. (The cluster
    // acceleration structure path has no such step: every record carries its own base geometry
    // index, and engine-view sets it to the visible entry outright.)
    Vector<u32> geometry_entry;  // the visible entry each geometry stands for
    for (u32 k = 0; k < instance_count; ++k) {
      instance_base[k] = geometry_entry.size();
      instance_geometries[k] = entries_of[k].size();
      for (const u32 entry : entries_of[k]) {
        const geometry::ClusterDesc& desc = scene.lod.mesh.clusters[list_out[entry * 2 + 1]];
        expanded_offset.push_back(expanded.size());
        gfx::expand_packed_triangles(
            std::span<const u32>(scene.lod.mesh.triangles.data() + desc.triangle_offset,
                                 desc.triangle_count),
            expanded);
        geometry_entry.push_back(entry);
      }
    }
    gfx::BufferResource indices16;
    REQUIRE(gfx::upload_buffer(device, expanded.data(), expanded.size() * sizeof(u16),
                               gfx::k_build_input_usage, indices16, &error));
    for (u32 g = 0; g < geometry_entry.size(); ++g) {
      const geometry::ClusterDesc& desc =
          scene.lod.mesh.clusters[list_out[geometry_entry[g] * 2 + 1]];
      gfx::ClusterGeometry geometry;
      geometry.vertices = vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
      geometry.vertex_count = desc.vertex_count;
      geometry.indices = indices16.address + u64{expanded_offset[g]} * sizeof(u16);
      geometry.triangle_count = desc.triangle_count;
      geometries.push_back(geometry);
    }
    Vector<gfx::AccelerationStructure> blas(instance_count);
    gfx::AccelerationStructure tlas;
    u64 scratch_bytes = 0;
    u32 first = 0;
    for (u32 k = 0; k < instance_count; ++k) {
      const std::span<const gfx::ClusterGeometry> range(geometries.data() + first,
                                                        instance_geometries[k]);
      REQUIRE_MESSAGE(gfx::create_blas(device, range, gfx::k_build_fast_trace, blas[k], &error),
                      error);
      scratch_bytes = std::max(scratch_bytes, blas[k].build_scratch_bytes);
      first += instance_geometries[k];
    }
    REQUIRE(gfx::create_tlas(device, instance_count, gfx::k_build_fast_trace, tlas, &error));
    scratch_bytes = std::max(scratch_bytes, tlas.build_scratch_bytes);
    gfx::BufferResource scratch;
    gfx::BufferResource tlas_instances;
    gfx::BufferResource base_buffer;
    REQUIRE(gfx::create_scratch(device, scratch_bytes, scratch, &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes * instance_count,
                               gfx::k_build_input_usage, true, tlas_instances, &error));
    REQUIRE(gfx::upload_buffer(device, instance_base.data(), instance_count * sizeof(u32),
                               k_storage, base_buffer, &error));
    Vector<gfx::TlasInstance> records;
    for (u32 k = 0; k < instance_count; ++k) {
      gfx::TlasInstance record;
      record.transform = gfx::instance_matrix(scene.instances[k], WorldEye{});
      record.custom_index = k;
      record.blas = blas[k].address;
      records.push_back(record);
    }
    gfx::write_instances(std::span<const gfx::TlasInstance>(records.data(), records.size()),
                         tlas_instances.mapped);
    first = 0;
    REQUIRE(gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          for (u32 k = 0; k < instance_count; ++k) {
            if (k != 0) {
              gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                              gfx::MemoryAccess::AccelerationStructureRead |
                                                  gfx::MemoryAccess::AccelerationStructureWrite);
            }
            const std::span<const gfx::ClusterGeometry> range(geometries.data() + first,
                                                              instance_geometries[k]);
            gfx::build_blas(cb, blas[k], range, gfx::k_build_fast_trace, scratch);
            first += instance_geometries[k];
          }
          gfx::acceleration_build_barrier(cb, gfx::PipelineStage::AccelerationBuild,
                                          gfx::MemoryAccess::AccelerationStructureRead);
          gfx::build_tlas(cb, tlas, tlas_instances.address, instance_count, gfx::k_build_fast_trace,
                          scratch);
          gfx::acceleration_build_barrier(cb, gfx::PipelineStage::ComputeShader,
                                          gfx::MemoryAccess::AccelerationStructureRead);
        },
        &error));
    const u32 scene_slot = bindless.add_acceleration_structure(tlas.handle);
    REQUIRE(scene_slot != gfx::BindlessSet::k_invalid_slot);
    // What each geometry stands for, as the ray pass reads it: GeometryIndex plus the instance's
    // base names geometry g, and `geometry_list[g]` is the {instance, cluster} of its entry, whose
    // pair is the id the pass writes (gfx.md, "The tie rule").
    Vector<u32> geometry_pairs;
    for (const u32 entry : geometry_entry) {
      geometry_pairs.push_back(list_out[entry * 2]);
      geometry_pairs.push_back(list_out[entry * 2 + 1]);
    }
    gfx::BufferResource geometry_list;
    REQUIRE(gfx::upload_buffer(device, geometry_pairs.data(), geometry_pairs.size() * sizeof(u32),
                               k_storage, geometry_list, &error));
    gfx::RayVisibilityParams ray{};
    ray.view_proj = view_proj;
    ray.clip_to_ray = gfx::clip_to_ray(projection, eye_view);
    ray.camera = Vec4{eye, 0.0f};
    ray.output = vis[2].address;
    ray.instance_base = base_buffer.address;
    ray.width = k_w;
    ray.height = k_h;
    ray.scene = scene_slot;
    ray.visible = geometry_list.address;
    ray.instances = instances.address;
    ray.meshes = meshes.address;
    std::memcpy(ray_params.mapped, &ray, sizeof(ray));
    const u64 ray_address = ray_params.address;
    gfx::ShaderModuleHandle trace_module = gfx::create_shader_module(
        device, shaders::k_ray_visibility_spirv, shaders::k_ray_visibility_spirv_size, &error);
    REQUIRE(trace_module.valid());
    const gfx::DescriptorSetLayoutHandle set_layout = bindless.layout();
    gfx::ComputePipeline trace_pipeline;
    REQUIRE(gfx::create_compute_pipeline(
        device, trace_module, "trace_main",
        std::span<const gfx::DescriptorSetLayoutHandle>(&set_layout, 1), sizeof(u64),
        trace_pipeline, &error));
    graph.reset();
    import_all();
    graph.add_pass(
        "trace", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) { b.write(rg_vis[2], gfx::Access::ComputeWrite); },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, trace_pipeline.pipeline);
          bindless.bind(cb, gfx::BindPoint::Compute);
          cb.push_constants(trace_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &ray_address);
          cb.dispatch(gfx::ray_visibility_group_count(k_w), gfx::ray_visibility_group_count(k_h),
                      1);
        });
    graph.add_pass(
        "readback rt", gfx::PassKind::Transfer,
        [&](gfx::PassBuilder& b) {
          b.read(rg_vis[2], gfx::Access::TransferRead);
          b.write(rg_host, gfx::Access::TransferWrite);
        },
        [&](gfx::CommandList cb, gfx::RenderGraph&) {
          const gfx::BufferCopy copy{0, host_vis_offset[2], vis_bytes};
          cb.copy_buffer(vis[2].buffer, host.buffer, copy);
        });
    REQUIRE_MESSAGE(graph.compile(&error), error);
    run_frame();

    const auto* ray_out = reinterpret_cast<const u64*>(host_bytes + host_vis_offset[2]);
    // What must agree is the surface: the (instance, cluster, triangle) each id leads to, and both
    // pictures name it the same way, by the scene's pair (gfx.md, "The tie rule").
    auto raster_surface = [&](u64 word) { return static_cast<u32>(word); };
    auto ray_surface = [&](u64 word) { return static_cast<u32>(word); };
    u32 rt_coverage_mismatch = 0;
    u32 rt_id_mismatch = 0;
    u32 both = 0;
    f32 max_depth_diff = 0.0f;
    for (u32 i = 0; i < k_w * k_h; ++i) {
      const bool in_raster = vertex_out[i] != 0;
      const bool in_rt = ray_out[i] != 0;
      if (in_raster != in_rt) ++rt_coverage_mismatch;
      if (in_raster && in_rt) {
        ++both;
        if (raster_surface(vertex_out[i]) != ray_surface(ray_out[i])) {
          ++rt_id_mismatch;
        } else {
          const f32 diff = std::fabs(depth_of(vertex_out[i]) - depth_of(ray_out[i]));
          max_depth_diff = diff > max_depth_diff ? diff : max_depth_diff;
        }
      }
    }
    CHECK(rt_coverage_mismatch * 200 <= covered);
    CHECK(rt_id_mismatch * 100 <= both);
    CHECK(max_depth_diff < 2e-3f);
    MESSAGE("raster vs rt: coverage mismatch " << rt_coverage_mismatch << ", id mismatch "
                                               << rt_id_mismatch << " of " << both << ", depth "
                                               << max_depth_diff);

    graph.reset();
    gfx::destroy_compute_pipeline(device, trace_pipeline);
    gfx::destroy_shader_module(device, trace_module);
    gfx::destroy_acceleration_structure(device, tlas);
    for (gfx::AccelerationStructure& b : blas)
      gfx::destroy_acceleration_structure(device, b);
    for (gfx::BufferResource* b :
         {&scratch, &tlas_instances, &base_buffer, &indices16, &geometry_list})
      gfx::destroy_buffer(device, *b);
  }

  // Two-pass occlusion culling over the scene: the instance behind the big sphere must drop out,
  // and the picture must not change. The two passes append to their own runs of one list. They
  // draw through the device's own path — the mesh path where there are mesh shaders, the vertex
  // path (the baseline tier) where there are not — and are held to that path's single-pass
  // picture, since "culling never changes the picture" has to hold on both tiers.
  {
    const u64* single_out = have_mesh ? mesh_out : vertex_out;
    gfx::CullParams pass1 = base;
    pass1.count_index = 1;
    pass1.pass = 1;
    pass1.hiz = hiz.address;
    pass1.visible = visible.address;
    pass1.draw_args = args[0].address;
    gfx::CullParams pass2 = pass1;
    pass2.pass = 2;
    pass2.visible = visible.address + run_bytes;
    pass2.draw_args = args[1].address;
    gfx::ClusterDrawParams draw2 = draw_to[3];
    draw2.visible = visible.address + run_bytes;
    draw2.visible_offset = pair_count;
    u32 pass_counts[3][2] = {};
    for (u32 frame = 0; frame < 3; ++frame) {
      const u32 cur = frame % 2;
      const u32 prev = 1 - cur;
      pass1.prev_flags = flags[prev].address;
      pass1.flags = flags[cur].address;
      pass2.prev_flags = flags[prev].address;
      pass2.flags = flags[cur].address;
      frames.wait_idle();
      blocks[1] = pass1;
      blocks[2] = pass2;
      graph.reset();
      import_all();
      add_reset(frame == 0, cur);
      if (frame == 0) {
        graph.add_pass(
            "clear prev", gfx::PassKind::Transfer,
            [&, prev](gfx::PassBuilder& b) { b.write(rg_flags[prev], gfx::Access::TransferWrite); },
            [&, prev](gfx::CommandList cb, gfx::RenderGraph&) {
              cb.fill_buffer(flags[prev].buffer, 0, gfx::k_whole_size, 0);
            });
      }
      add_cull(1, 0);
      if (have_mesh) add_mesh_args(0);
      add_draw(3, 0, have_mesh, &draw_to[3]);
      add_hiz(3);
      add_cull(2, 1);
      if (have_mesh) add_mesh_args(1);
      add_draw(3, 1, have_mesh, &draw2);
      add_hiz(3);
      add_readback();
      REQUIRE_MESSAGE(graph.compile(&error), error);
      run_frame();
      pass_counts[frame][0] = args_out[1];
      pass_counts[frame][1] = args_out[5];
      MESSAGE("occlusion frame " << frame << ": pass 1 drew " << pass_counts[frame][0]
                                 << ", pass 2 drew " << pass_counts[frame][1]);
    }
    CHECK(pass_counts[0][0] == 0);  // no history
    CHECK(pass_counts[0][1] == expected.size());
    const u32 steady = pass_counts[2][0] + pass_counts[2][1];
    CHECK(steady < expected.size());  // the hidden instance drops out
    CHECK(pass_counts[1][0] + pass_counts[1][1] == steady);

    // The hidden instance contributes nothing once the Hi-Z has it, and the picture is the same
    // surface everywhere: an id names the scene's pair, not an entry of that frame's list
    // (gfx.md, "The tie rule"), so the ids agree word for word.
    const auto* occl_out = reinterpret_cast<const u64*>(host_bytes + host_vis_offset[3]);
    const auto* occl_list = reinterpret_cast<const u32*>(host_bytes + host_list_offset);
    u32 hidden_pairs = 0;
    for (u32 i = 0; i < steady && i < pair_count * 2; ++i) {
      const u32 entry = i < pass_counts[2][0] ? i : pair_count + (i - pass_counts[2][0]);
      if (occl_list[entry * 2] == scene.hidden_instance) ++hidden_pairs;
    }
    CHECK(hidden_pairs == 0);
    u32 occl_coverage_mismatch = 0;
    u32 occl_id_mismatch = 0;
    for (u32 i = 0; i < k_w * k_h; ++i) {
      const bool a = single_out[i] != 0;
      const bool b = occl_out[i] != 0;
      if (a != b) ++occl_coverage_mismatch;
      if (a && b && static_cast<u32>(single_out[i]) != static_cast<u32>(occl_out[i]))
        ++occl_id_mismatch;
    }
    CHECK(occl_coverage_mismatch == 0);
    CHECK(occl_id_mismatch * 1000 <= covered);
    MESSAGE("occlusion, " << std::string(have_mesh ? "mesh" : "vertex") << " path: "
                          << expected.size() << " pairs become " << steady << ", coverage mismatch "
                          << occl_coverage_mismatch << ", id mismatch " << occl_id_mismatch);
  }

  graph.reset();
  if (mesh_pipeline.valid()) gfx::destroy_pipeline(device, mesh_pipeline);
  if (mesh_module.valid()) gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_compute_pipeline(device, hiz_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, cull_module);
  gfx::destroy_shader_module(device, hiz_module);
  bindless.destroy();
  frames.destroy();
  for (gfx::BufferResource* b :
       {&vis[0],  &vis[1],       &vis[2],       &vis[3],   &visible,  &args[0],
        &args[1], &mesh_args[0], &mesh_args[1], &flags[0], &flags[1], &hiz,
        &params,  &ray_params,   &host,         &clusters, &lods,     &quantized,
        &meshes,  &instances,    &triangles,    &vertices}) {
    gfx::destroy_buffer(device, *b);
  }
  device.destroy();
}
