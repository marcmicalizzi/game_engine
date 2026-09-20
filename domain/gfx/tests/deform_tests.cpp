// The per-frame deformed-vertex pool (docs/plan/04-renderer.md §4.3, ADR-0026 decision 7,
// experiment E25). One scene, two instances of one mesh with the same world transform: instance
// 0 reads its positions out of the pool, instance 1 off the mesh's 16-bit grid. Three things are
// asserted here, and they are what makes the pool safe to build the rest of the engine on:
//
//   1. With the **identity** deformer — the pass writes the rest pose — the mesh, vertex, and
//      software rasterizers draw the deformed instance as they draw the rigid one: the same
//      coverage and the same id on every pixel, with the depth agreeing to an ulp (the rigid
//      path dequantizes in the rasterizer, the deformed one reads the float the pass stored, and
//      the two compilations round the same expression differently in the last bit). The pool's
//      slots outside the LOD cut keep the sentinel they were filled with, which is the cost rule
//      (`the cut, not the mesh`) written as an assertion.
//   2. With the **wave** deformer the rasterized picture and a KHR ray-traced picture built from
//      the same pool positions agree the way two rasterizers do, and both differ from the rigid
//      instance, so the deformation really reaches every position reader.
//   3. The cluster acceleration structure **templates** variant — templates built once from the
//      rest pose, instantiated per frame from the pool — traces the same picture as rebuilding
//      the cluster structures every frame, and costs less to do it.
//
// Test 1 runs on any device with 64-bit buffer atomics; 2 needs ray queries; 3 needs cluster
// acceleration structures (NVIDIA RTX).
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/cluster_sw_raster.spv.h>
#include <shaders/cluster_vertex.spv.h>
#include <shaders/deform.spv.h>
#include <shaders/ray_visibility.spv.h>
#include <string>

using namespace engine;

namespace {

constexpr u32 k_w = 320;
constexpr u32 k_h = 240;
constexpr u32 k_sentinel = 0x7fc00000u;  // a quiet NaN: no deformer ever writes it
// Pool vertices past the cut's blocks. The pass must leave them alone, which is what says the
// suballocation is packed and bounded rather than scattered over the mesh's whole vertex range.
constexpr u32 k_pool_slack = 1024;
constexpr f32 k_amplitude = 0.02f;  // of the mesh's grid box, as engine-view defaults

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
  u32 word_mismatch = 0;
  f32 max_depth_diff = 0.0f;
};

Comparison compare(const u64* a, const u64* b, u32 count) {
  Comparison c;
  for (u32 i = 0; i < count; ++i) {
    const bool in_a = a[i] != 0;
    const bool in_b = b[i] != 0;
    c.covered_a += in_a;
    if (a[i] != b[i]) ++c.word_mismatch;
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

// The scene every case here draws: one terrain mesh, its LOD cut from a fixed camera, and two
// instances of it in the same place — instance 0 deformed, instance 1 rigid. The two visible
// lists hold the same clusters in the same order under the two instances, so a visibility id is
// the same word in both pictures and the two can be compared directly.
struct DeformScene {
  geometry::ClusterLodMesh lod;
  Vector<u32> cut;  // global cluster indices, the LOD cut of the camera below
  u32 pool_vertices = 0;
  Mat4 view_proj;
  Vec3 eye{0.0f, 9.0f, 24.0f};
  f32 znear = 0.1f;

  gfx::BufferResource clusters;
  gfx::BufferResource triangles;
  gfx::BufferResource quantized;
  gfx::BufferResource attributes;
  gfx::BufferResource vertices;  // rest-pose floats, for the acceleration structure builders
  gfx::BufferResource meshes;
  gfx::BufferResource instances;
  gfx::BufferResource deform_table;
  gfx::BufferResource pool;
  gfx::BufferResource slots;  // u32 per visible entry: its block's first pool vertex
  gfx::BufferResource count_buffer;
  gfx::BufferResource visible[2];  // {0, cluster} and {1, cluster} per cut entry
  Vector<u32> slot_table;          // the CPU's copy of what the allocator would have written
  u32 cut_vertices = 0;            // what the cut's blocks take: the pool's occupancy

  bool create(const gfx::Device& device, u32 grid, u32 deform_kind, std::string* error) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_terrain(grid, 10.0f, positions, indices);
    if (!geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{}, lod, error))
      return false;

    const f32 fov_y = radians(60.0f);
    view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                look_at(eye, Vec3{}, Vec3{0, 1, 0});
    const Frustum frustum = frustum_from_view_proj(view_proj);
    geometry::LodView view;
    view.camera = eye;
    view.znear = znear;
    view.proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;
    view.threshold_px = 1.0f;
    for (u32 i = 0; i < lod.mesh.clusters.size(); ++i) {
      const geometry::ClusterDesc& c = lod.mesh.clusters[i];
      if (frustum_contains_sphere(frustum, c.center, c.radius) &&
          geometry::lod_selects(lod.lod[i], view)) {
        cut.push_back(i);
      }
    }
    if (cut.size() < 8) {
      if (error != nullptr) *error = "the camera selects too few clusters";
      return false;
    }

    // The pool is suballocated per **visible entry**, so its occupancy is the cut's vertices and
    // not the mesh's. The allocator that does this on the GPU (`deform_alloc.slang`) is a prefix
    // sum over exactly these numbers in exactly this order, and writing the same table here on the
    // CPU is what lets the cases below check the shaders' half of the contract on its own. The
    // slack past the cut is what the sentinel case reads: a block the frame did not hand out must
    // come back untouched.
    for (const u32 c : cut) {
      slot_table.push_back(cut_vertices);
      cut_vertices += lod.mesh.clusters[c].vertex_count;
    }
    pool_vertices = cut_vertices + k_pool_slack;
    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags k_pool_usage = k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            gfx::k_build_input_usage;
    if (!gfx::upload_buffer(device, lod.mesh.clusters.data(),
                            lod.mesh.clusters.size() * sizeof(geometry::ClusterDesc), k_storage,
                            clusters, error) ||
        !gfx::upload_buffer(device, lod.mesh.triangles.data(),
                            lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles, error) ||
        !gfx::upload_buffer(device, lod.mesh.quantized.data(),
                            lod.mesh.quantized.size() * sizeof(u16), k_storage, quantized, error) ||
        !gfx::upload_buffer(device, lod.mesh.attributes.data(),
                            lod.mesh.attributes.size() * sizeof(geometry::VertexAttributes),
                            k_storage, attributes, error) ||
        !gfx::upload_buffer(device, lod.mesh.vertices.data(),
                            lod.mesh.vertices.size() * sizeof(Vec3),
                            k_storage | gfx::k_build_input_usage, vertices, error) ||
        !gfx::create_buffer(device, u64{pool_vertices} * 3 * sizeof(f32), k_pool_usage, false, pool,
                            error)) {
      return false;
    }
    // What is left on the instance is which deformer it plays; where in the pool it writes is the
    // per-entry table above.
    gfx::DeformDesc desc{};
    desc.stages = gfx::k_deform_stage_procedural | deform_kind;
    gfx::MeshDesc mesh_desc{};
    mesh_desc.quant = Vec4{lod.mesh.quant_origin, lod.mesh.quant_scale};
    mesh_desc.cluster_count = lod.mesh.clusters.size();
    gfx::InstanceDesc table[2];
    for (gfx::InstanceDesc& instance : table)
      gfx::set_instance_transform(instance, Mat4::identity());
    table[0].deform = 0;  // the deformed one
    table[1].deform = gfx::k_invalid_deform;
    Vector<u32> entries[2];
    for (u32 k = 0; k < 2; ++k) {
      for (const u32 c : cut) {
        entries[k].push_back(k);
        entries[k].push_back(c);
      }
    }
    const u32 count = cut.size();
    if (!gfx::upload_buffer(device, &desc, sizeof(desc), k_storage, deform_table, error) ||
        !gfx::upload_buffer(device, table, sizeof(table), k_storage, instances, error) ||
        !gfx::upload_buffer(device, slot_table.data(), slot_table.size() * sizeof(u32), k_storage,
                            slots, error) ||
        !gfx::upload_buffer(device, &count, sizeof(count), k_storage, count_buffer, error)) {
      return false;
    }
    mesh_desc.quantized = quantized.address;
    mesh_desc.deform_pool = pool.address;
    mesh_desc.deform_slots = slots.address;
    if (!gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, error))
      return false;
    for (u32 k = 0; k < 2; ++k) {
      if (!gfx::upload_buffer(device, entries[k].data(), entries[k].size() * sizeof(u32), k_storage,
                              visible[k], error)) {
        return false;
      }
    }
    return true;
  }

  u32 cut_count() const noexcept { return cut.size(); }

  gfx::DeformParams params(f32 time) const noexcept {
    gfx::DeformParams p{};
    p.clusters = clusters.address;
    p.instances = instances.address;
    p.meshes = meshes.address;
    p.attributes = attributes.address;
    p.visible = visible[0].address;  // the deformed instance's list
    p.visible_count = count_buffer.address;
    p.pool = pool.address;
    p.deform = deform_table.address;
    p.slots = slots.address;
    p.time = time;
    p.amplitude = k_amplitude;
    p.max_entries = cut.size();
    p.visible_offset = 0;
    return p;
  }

  gfx::ClusterDrawParams draw(u32 instance) const noexcept {
    gfx::ClusterDrawParams d{};
    d.view_proj = view_proj;
    d.clusters = clusters.address;
    d.mesh = meshes.address;
    d.instances = instances.address;
    d.triangles = triangles.address;
    d.triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
    d.visible = visible[instance].address;
    d.width = k_w;
    d.height = k_h;
    return d;
  }

  void destroy(const gfx::Device& device) noexcept {
    for (gfx::BufferResource* b :
         {&clusters, &triangles, &quantized, &attributes, &vertices, &meshes, &instances,
          &deform_table, &pool, &slots, &count_buffer, &visible[0], &visible[1]}) {
      gfx::destroy_buffer(device, *b);
    }
  }
};

}  // namespace

TEST_CASE("deform: the identity deformer draws exactly what the rigid instance draws") {
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
  const bool have_mesh = device.features().mesh_shader;

  DeformScene scene;
  REQUIRE_MESSAGE(scene.create(device, 97, gfx::k_deform_identity, &error), error);
  const u32 cut_count = scene.cut_count();
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const VkBufferUsageFlags k_vis =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const u64 pool_bytes = u64{scene.pool_vertices} * 3 * sizeof(f32);
  gfx::BufferResource vis[6];  // vertex, software, mesh; deformed then rigid in each pair
  gfx::BufferResource host;
  for (gfx::BufferResource& v : vis)
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, v, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * 6 + pool_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             true, host, &error));

  gfx::FrameContext frames;
  gfx::BindlessSet bindless;
  REQUIRE(frames.create(device, 1, &error));
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  VkShaderModule vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  VkShaderModule sw_module = gfx::create_shader_module(
      device, shaders::k_cluster_sw_raster_spirv, shaders::k_cluster_sw_raster_spirv_size, &error);
  VkShaderModule deform_module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                           shaders::k_deform_spirv_size, &error);
  REQUIRE(vertex_module != VK_NULL_HANDLE);
  REQUIRE(sw_module != VK_NULL_HANDLE);
  REQUIRE(deform_module != VK_NULL_HANDLE);
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  VkPipeline vertex_pipeline = VK_NULL_HANDLE;
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error),
                  error);
  gfx::ComputePipeline sw_pipeline;
  gfx::ComputePipeline deform_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, sw_module, "sw_raster_main", {},
                                       sizeof(gfx::ClusterDrawParams), sw_pipeline, &error));
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, deform_module, "deform_main", {},
                                               sizeof(gfx::DeformParams), deform_pipeline, &error),
                  error);
  VkShaderModule mesh_module = VK_NULL_HANDLE;
  VkPipeline mesh_pipeline = VK_NULL_HANDLE;
  if (have_mesh) {
    mesh_module = gfx::create_shader_module(device, shaders::k_cluster_mesh_spirv,
                                            shaders::k_cluster_mesh_spirv_size, &error);
    REQUIRE(mesh_module != VK_NULL_HANDLE);
    gfx::MeshPipelineDesc mesh_desc;
    mesh_desc.mesh = mesh_module;
    mesh_desc.fragment = mesh_module;
    mesh_desc.fragment_entry = "fs_visibility";
    mesh_desc.layout = bindless.pipeline_layout();
    REQUIRE(gfx::create_mesh_pipeline(device, mesh_desc, mesh_pipeline, &error));
  }

  gfx::ClusterDrawParams draws[6];
  for (u32 i = 0; i < 6; ++i) {
    draws[i] = scene.draw(i % 2);  // even: the deformed instance, odd: the rigid one
    draws[i].visibility = vis[i].address;
  }
  const gfx::DeformParams deform_params = scene.params(0.0f);

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[6];
  for (u32 i = 0; i < 6; ++i)
    rg_vis[i] = graph.import_buffer("vis", vis[i]);
  const gfx::RgBuffer rg_pool = graph.import_buffer("pool", scene.pool);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.write(v, gfx::Access::TransferWrite);
        b.write(rg_pool, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (const gfx::BufferResource& v : vis)
          vkCmdFillBuffer(cb, v.buffer, 0, VK_WHOLE_SIZE, 0);
        // Every slot a sentinel: only the cut's may come back as a number.
        vkCmdFillBuffer(cb, scene.pool.buffer, 0, VK_WHOLE_SIZE, k_sentinel);
      });
  graph.add_pass(
      "deform", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(rg_pool, gfx::Access::ComputeWrite); },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, deform_pipeline.pipeline);
        vkCmdPushConstants(cb, deform_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(deform_params), &deform_params);
        vkCmdDispatch(cb, cut_count, 1, 1);
      });
  for (u32 i = 0; i < 6; ++i) {
    const bool software = i / 2 == 1;
    const bool mesh_path = i / 2 == 2;
    if (mesh_path && !have_mesh) continue;
    const gfx::ClusterDrawParams* params = &draws[i];
    if (software) {
      graph.add_pass(
          "software", gfx::PassKind::Compute,
          [&, i](gfx::PassBuilder& b) {
            b.write(rg_vis[i], gfx::Access::ComputeReadWrite);
            b.read(rg_pool, gfx::Access::ComputeRead);
          },
          [&, params](VkCommandBuffer cb, gfx::RenderGraph&) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sw_pipeline.pipeline);
            vkCmdPushConstants(cb, sw_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(*params), params);
            vkCmdDispatch(cb, cut_count, 1, 1);
          });
      continue;
    }
    graph.add_pass(
        "raster", gfx::PassKind::Raster,
        [&, i, mesh_path](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_vis[i], gfx::Access::FragmentReadWrite);
          b.read(rg_pool, mesh_path ? gfx::Access::MeshRead : gfx::Access::VertexRead);
        },
        [&, params, mesh_path](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mesh_path ? mesh_pipeline : vertex_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(*params), params);
          if (mesh_path) {
            vkCmdDrawMeshTasksEXT(cb, cut_count, 1, 1);
          } else {
            vkCmdDraw(cb, triangles_per_cluster * 3, cut_count, 0, 0);
          }
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.read(v, gfx::Access::TransferRead);
        b.read(rg_pool, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (u32 i = 0; i < 6; ++i) {
          const VkBufferCopy copy{0, vis_bytes * i, vis_bytes};
          vkCmdCopyBuffer(cb, vis[i].buffer, host.buffer, 1, &copy);
        }
        const VkBufferCopy pool_copy{0, vis_bytes * 6, pool_bytes};
        vkCmdCopyBuffer(cb, scene.pool.buffer, host.buffer, 1, &pool_copy);
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* host_bytes = static_cast<const u8*>(host.mapped);
  const char* names[3] = {"vertex", "software", "mesh"};
  for (u32 path = 0; path < 3; ++path) {
    if (path == 2 && !have_mesh) continue;
    const auto* deformed = reinterpret_cast<const u64*>(host_bytes + vis_bytes * (path * 2));
    const auto* rigid = reinterpret_cast<const u64*>(host_bytes + vis_bytes * (path * 2 + 1));
    const Comparison c = compare(deformed, rigid, k_w * k_h);
    CHECK(c.covered_a > k_w * k_h / 16);
    // Every pixel names the same surface: the same coverage and the same (entry, triangle) id.
    // The depth is not bit-identical everywhere, and cannot be: the rigid path dequantizes in
    // the rasterizer and the deformed path reads the pool slot the pass stored, so the two
    // compilations of the same expression round the multiply and the add differently in the last
    // bit. Measured worst case on the RTX 5090: 1.3e-8 of a reversed-Z depth around 0.005, which
    // is an ulp or two and a hundredth of what a silhouette pixel would cost.
    CHECK(c.coverage_mismatch == 0);
    CHECK(c.id_mismatch == 0);
    CHECK(c.max_depth_diff < 2.0e-7f);
    MESSAGE(names[path] << ": " << c.covered_a << " px covered, coverage mismatch "
                        << c.coverage_mismatch << ", id mismatch " << c.id_mismatch
                        << ", worst depth difference " << c.max_depth_diff << " over "
                        << c.word_mismatch << " pixels whose depth bits differ");
  }

  // Every slot the allocator handed out holds a number; every slot past the cut's blocks still
  // holds the sentinel. This is the cost rule *and* the suballocation as one assertion: the pass
  // wrote the cut's blocks, they are packed from the front of the pool, and the slack behind them
  // is untouched. The per-instance pool this replaced would have needed one block per instance's
  // whole mesh — 25,135 vertices here for a cut of 1,622.
  const auto* pool_words = reinterpret_cast<const u32*>(host_bytes + vis_bytes * 6);
  u32 in_cut_untouched = 0;
  u32 outside_written = 0;
  for (u32 v = 0; v < scene.pool_vertices; ++v) {
    bool sentinel = true;
    for (u32 k = 0; k < 3; ++k)
      sentinel = sentinel && pool_words[v * 3 + k] == k_sentinel;
    if (v < scene.cut_vertices) {
      in_cut_untouched += sentinel ? 1 : 0;
    } else {
      outside_written += sentinel ? 0 : 1;
    }
  }
  CHECK(scene.cut_vertices > 0);
  CHECK(scene.cut_vertices < scene.pool_vertices);  // or the check below proves nothing
  CHECK(in_cut_untouched == 0);
  CHECK(outside_written == 0);
  MESSAGE("pool: " << scene.cut_vertices << " of " << scene.pool_vertices
                   << " vertices written for a cut of " << cut_count << " of "
                   << scene.lod.mesh.clusters.size() << " clusters; the mesh has "
                   << scene.lod.mesh.vertices.size());

  graph.reset();
  if (mesh_pipeline != VK_NULL_HANDLE) gfx::destroy_pipeline(device, mesh_pipeline);
  if (mesh_module != VK_NULL_HANDLE) gfx::destroy_shader_module(device, mesh_module);
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, sw_pipeline);
  gfx::destroy_compute_pipeline(device, deform_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, sw_module);
  gfx::destroy_shader_module(device, deform_module);
  bindless.destroy();
  frames.destroy();
  scene.destroy(device);
  gfx::destroy_buffer(device, host);
  for (gfx::BufferResource& v : vis)
    gfx::destroy_buffer(device, v);
  device.destroy();
}

TEST_CASE("deform: the wave deformer reaches the rasterizer and the ray path alike") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().buffer_int64_atomics || !device.features().ray_query) {
    MESSAGE("no ray queries or 64-bit atomics on " << device.adapter().name);
    device.destroy();
    return;
  }
  DeformScene scene;
  REQUIRE_MESSAGE(scene.create(device, 97, gfx::k_deform_wave, &error), error);
  const u32 cut_count = scene.cut_count();
  const u32 triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const VkBufferUsageFlags k_vis =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  gfx::BufferResource vis[3];  // raster deformed, raster rigid, ray traced from the pool
  gfx::BufferResource host;
  gfx::BufferResource ray_params;
  gfx::BufferResource indices16;
  for (gfx::BufferResource& v : vis)
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, v, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host,
                             &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, ray_params,
                             &error));
  // One geometry per cut cluster, positions taken from the pool at the instance's own offset.
  Vector<u16> expanded;
  Vector<u32> expanded_offset;
  for (const u32 c : scene.cut) {
    const geometry::ClusterDesc& desc = scene.lod.mesh.clusters[c];
    expanded_offset.push_back(expanded.size());
    gfx::expand_packed_triangles(
        std::span<const u32>(scene.lod.mesh.triangles.data() + desc.triangle_offset,
                             desc.triangle_count),
        expanded);
  }
  REQUIRE(gfx::upload_buffer(device, expanded.data(), expanded.size() * sizeof(u16),
                             gfx::k_build_input_usage, indices16, &error));
  Vector<gfx::ClusterGeometry> geometries;
  for (u32 k = 0; k < cut_count; ++k) {
    const geometry::ClusterDesc& desc = scene.lod.mesh.clusters[scene.cut[k]];
    gfx::ClusterGeometry g;
    // The pool is packed per visible entry, so entry k's block is where the slot table says.
    g.vertices = scene.pool.address + u64{scene.slot_table[k]} * 12;
    g.vertex_count = desc.vertex_count;
    g.indices = indices16.address + u64{expanded_offset[k]} * sizeof(u16);
    g.triangle_count = desc.triangle_count;
    geometries.push_back(g);
  }
  gfx::AccelerationStructure blas;
  gfx::AccelerationStructure tlas;
  gfx::BufferResource scratch;
  gfx::BufferResource tlas_instances;
  REQUIRE_MESSAGE(gfx::create_blas(device, geometries, gfx::k_build_fast_trace, blas, &error),
                  error);
  REQUIRE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas, &error));
  REQUIRE(gfx::create_scratch(device,
                              blas.build_scratch_bytes > tlas.build_scratch_bytes
                                  ? blas.build_scratch_bytes
                                  : tlas.build_scratch_bytes,
                              scratch, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                             tlas_instances, &error));
  gfx::TlasInstance record;
  record.blas = blas.address;
  gfx::write_instances(std::span<const gfx::TlasInstance>(&record, 1), tlas_instances.mapped);

  gfx::FrameContext frames;
  gfx::BindlessSet bindless;
  REQUIRE(frames.create(device, 1, &error));
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  const u32 scene_slot = bindless.add_acceleration_structure(tlas.handle);
  REQUIRE(scene_slot != gfx::BindlessSet::k_invalid_slot);
  VkShaderModule vertex_module = gfx::create_shader_module(
      device, shaders::k_cluster_vertex_spirv, shaders::k_cluster_vertex_spirv_size, &error);
  VkShaderModule deform_module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                           shaders::k_deform_spirv_size, &error);
  VkShaderModule trace_module = gfx::create_shader_module(
      device, shaders::k_ray_visibility_spirv, shaders::k_ray_visibility_spirv_size, &error);
  REQUIRE(vertex_module != VK_NULL_HANDLE);
  REQUIRE(deform_module != VK_NULL_HANDLE);
  REQUIRE(trace_module != VK_NULL_HANDLE);
  gfx::GraphicsPipelineDesc vertex_desc;
  vertex_desc.vertex = vertex_module;
  vertex_desc.vertex_entry = "vs_cluster";
  vertex_desc.fragment = vertex_module;
  vertex_desc.fragment_entry = "fs_visibility";
  vertex_desc.layout = bindless.pipeline_layout();
  VkPipeline vertex_pipeline = VK_NULL_HANDLE;
  REQUIRE(gfx::create_graphics_pipeline(device, vertex_desc, vertex_pipeline, &error));
  gfx::ComputePipeline deform_pipeline;
  gfx::ComputePipeline trace_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, deform_module, "deform_main", {},
                                       sizeof(gfx::DeformParams), deform_pipeline, &error));
  const VkDescriptorSetLayout set_layout = bindless.layout();
  REQUIRE(gfx::create_compute_pipeline(device, trace_module, "trace_main",
                                       std::span<const VkDescriptorSetLayout>(&set_layout, 1),
                                       sizeof(u64), trace_pipeline, &error));

  gfx::ClusterDrawParams draw_deformed = scene.draw(0);
  draw_deformed.visibility = vis[0].address;
  gfx::ClusterDrawParams draw_rigid = scene.draw(1);
  draw_rigid.visibility = vis[1].address;
  const gfx::DeformParams deform_params = scene.params(0.75f);
  gfx::RayVisibilityParams ray{};
  ray.view_proj = scene.view_proj;
  ray.inv_view_proj = inverse(scene.view_proj);
  ray.camera = Vec4{scene.eye, 0.0f};
  ray.output = vis[2].address;
  ray.instance_base = 0;  // one geometry per cut entry, in cut order: the visible index outright
  ray.width = k_w;
  ray.height = k_h;
  ray.scene = scene_slot;
  std::memcpy(ray_params.mapped, &ray, sizeof(ray));
  const u64 ray_address = ray_params.address;

  // Frame A: fill the pool, then build the bottom-level structure from it. The build reads the
  // pool, so it cannot share a command buffer with the pass that writes it without the barrier
  // the graph would give; this keeps the two plainly separate.
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, deform_pipeline.pipeline);
        vkCmdPushConstants(cb, deform_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(deform_params), &deform_params);
        vkCmdDispatch(cb, cut_count, 1, 1);
      },
      &error));
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer cb) {
        gfx::build_blas(cb, blas, geometries, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb,
                                        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        gfx::build_tlas(cb, tlas, tlas_instances.address, 1, gfx::k_build_fast_trace, scratch);
        gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
      },
      &error));

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[3];
  for (u32 i = 0; i < 3; ++i)
    rg_vis[i] = graph.import_buffer("vis", vis[i]);
  const gfx::RgBuffer rg_pool = graph.import_buffer("pool", scene.pool);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host);
  graph.add_pass(
      "reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.write(v, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (const gfx::BufferResource& v : vis)
          vkCmdFillBuffer(cb, v.buffer, 0, VK_WHOLE_SIZE, 0);
      });
  for (u32 i = 0; i < 2; ++i) {
    const gfx::ClusterDrawParams* params = i == 0 ? &draw_deformed : &draw_rigid;
    graph.add_pass(
        "raster", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          b.render_area(k_w, k_h);
          b.write(rg_vis[i], gfx::Access::FragmentReadWrite);
          b.read(rg_pool, gfx::Access::VertexRead);
        },
        [&, params](VkCommandBuffer cb, gfx::RenderGraph&) {
          vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vertex_pipeline);
          bindless.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS);
          vkCmdPushConstants(cb, bindless.pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                             sizeof(*params), params);
          vkCmdDraw(cb, triangles_per_cluster * 3, cut_count, 0, 0);
        });
  }
  graph.add_pass(
      "trace", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) { b.write(rg_vis[2], gfx::Access::ComputeWrite); },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline.pipeline);
        bindless.bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdPushConstants(cb, trace_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                           &ray_address);
        vkCmdDispatch(cb, gfx::ray_visibility_group_count(k_w),
                      gfx::ray_visibility_group_count(k_h), 1);
      });
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (const gfx::RgBuffer& v : rg_vis)
          b.read(v, gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
      },
      [&](VkCommandBuffer cb, gfx::RenderGraph&) {
        for (u32 i = 0; i < 3; ++i) {
          const VkBufferCopy copy{0, vis_bytes * i, vis_bytes};
          vkCmdCopyBuffer(cb, vis[i].buffer, host.buffer, 1, &copy);
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  VkCommandBuffer commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto* deformed = static_cast<const u64*>(host.mapped);
  const u64* rigid = deformed + u64{k_w} * k_h;
  const u64* traced = rigid + u64{k_w} * k_h;
  const Comparison moved = compare(deformed, rigid, k_w * k_h);
  const Comparison rt = compare(deformed, traced, k_w * k_h);
  CHECK(moved.covered_a > k_w * k_h / 16);
  // The deformer has to have done something, or the comparison below proves nothing.
  CHECK(moved.word_mismatch > moved.covered_a / 8);
  // Raster against the ray path built from the same pool: the usual shared-edge tolerances.
  CHECK(rt.coverage_mismatch * 200 <= rt.covered_a);
  CHECK(rt.id_mismatch * 100 <= rt.both);
  CHECK(rt.max_depth_diff < 2e-3f);
  MESSAGE("wave: " << moved.covered_a << " px covered, " << moved.word_mismatch
                   << " differ from the rigid instance; raster vs rt coverage mismatch "
                   << rt.coverage_mismatch << ", id mismatch " << rt.id_mismatch << " of "
                   << rt.both << ", depth " << rt.max_depth_diff);

  graph.reset();
  gfx::destroy_pipeline(device, vertex_pipeline);
  gfx::destroy_compute_pipeline(device, deform_pipeline);
  gfx::destroy_compute_pipeline(device, trace_pipeline);
  gfx::destroy_shader_module(device, vertex_module);
  gfx::destroy_shader_module(device, deform_module);
  gfx::destroy_shader_module(device, trace_module);
  bindless.destroy();
  frames.destroy();
  gfx::destroy_acceleration_structure(device, blas);
  gfx::destroy_acceleration_structure(device, tlas);
  scene.destroy(device);
  for (gfx::BufferResource* b : {&host, &ray_params, &indices16, &scratch, &tlas_instances})
    gfx::destroy_buffer(device, *b);
  for (gfx::BufferResource& v : vis)
    gfx::destroy_buffer(device, v);
  device.destroy();
}

TEST_CASE("deform: instantiated cluster templates trace what the rebuilt clusters trace") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  if (!device.features().cluster_acceleration_structure || !device.features().ray_query ||
      !device.features().buffer_int64_atomics) {
    MESSAGE("no cluster acceleration structures, ray queries, or 64-bit atomics on "
            << device.adapter().name);
    device.destroy();
    return;
  }
  gfx::ClusterAsProperties props;
  REQUIRE(gfx::cluster_as_properties(device, props));
  MESSAGE("template alignment " << props.template_alignment << ", cluster alignment "
                                << props.cluster_alignment);
  DeformScene scene;
  REQUIRE_MESSAGE(scene.create(device, 97, gfx::k_deform_wave, &error), error);
  const u32 cut_count = scene.cut_count();
  const u32 cluster_count = scene.lod.mesh.clusters.size();

  constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const VkBufferUsageFlags k_vis =
      k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  gfx::BufferResource vis[2];  // rebuilt, instantiated
  gfx::BufferResource host;
  gfx::BufferResource ray_params[2];
  gfx::BufferResource indices8;
  gfx::BufferResource build_records;
  gfx::BufferResource template_records;
  gfx::BufferResource instantiate_records;
  gfx::BufferResource tlas_instances[2];
  for (gfx::BufferResource& v : vis)
    REQUIRE(gfx::create_buffer(device, vis_bytes, k_vis, false, v, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, host,
                             &error));
  for (gfx::BufferResource& p : ray_params)
    REQUIRE(
        gfx::create_buffer(device, sizeof(gfx::RayVisibilityParams), k_address, true, p, &error));
  Vector<u8> packed8;
  gfx::pack_cluster_indices(
      std::span<const u32>(scene.lod.mesh.triangles.data(), scene.lod.mesh.triangles.size()),
      packed8);
  REQUIRE(gfx::upload_buffer(device, packed8.data(), packed8.size(), gfx::k_build_input_usage,
                             indices8, &error));

  gfx::ClusterSetLimits limits;
  limits.max_clusters = cut_count;
  limits.max_triangles_per_cluster = geometry::ClusterLodOptions{}.max_triangles;
  limits.max_vertices_per_cluster = geometry::ClusterLodOptions{}.max_vertices;
  limits.max_geometry_index = cut_count - 1;
  gfx::ClusterSetLimits instantiate_limits = limits;
  instantiate_limits.instantiate = true;
  gfx::ClusterSetLimits template_limits = limits;
  template_limits.max_clusters = cluster_count;
  template_limits.max_geometry_index = 0;
  gfx::ClusterSet rebuilt;
  gfx::ClusterSet instantiated;
  gfx::ClusterTemplateSet templates;
  gfx::ClusterBlas blas[2];
  gfx::AccelerationStructure tlas[2];
  REQUIRE_MESSAGE(gfx::create_cluster_set(device, limits, rebuilt, &error), error);
  REQUIRE_MESSAGE(gfx::create_cluster_set(device, instantiate_limits, instantiated, &error), error);
  REQUIRE_MESSAGE(gfx::create_cluster_templates(device, template_limits, templates, &error), error);
  for (u32 i = 0; i < 2; ++i) {
    REQUIRE(gfx::create_cluster_blas(device, cut_count, blas[i], &error));
    REQUIRE(gfx::create_tlas(device, 1, gfx::k_build_fast_trace, tlas[i], &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_instance_record_bytes, gfx::k_build_input_usage, true,
                               tlas_instances[i], &error));
  }
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * cut_count,
                             gfx::k_build_input_usage, true, build_records, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_template_record_bytes * cluster_count,
                             gfx::k_build_input_usage, true, template_records, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_instantiate_record_bytes * cut_count,
                             gfx::k_build_input_usage, true, instantiate_records, &error));
  u64 scratch_bytes = rebuilt.build_scratch_bytes;
  for (const u64 bytes : {instantiated.build_scratch_bytes, templates.build_scratch_bytes,
                          blas[0].build_scratch_bytes, tlas[0].build_scratch_bytes}) {
    scratch_bytes = bytes > scratch_bytes ? bytes : scratch_bytes;
  }
  gfx::BufferResource scratch;
  REQUIRE(gfx::create_scratch(device, scratch_bytes, scratch, &error));

  // Templates: one per cluster of the mesh, from the rest pose, with cluster id and base
  // geometry index zero, so an instantiation's offsets are the visible entry outright.
  Vector<gfx::ClusterBuildInput> template_inputs;
  for (u32 c = 0; c < cluster_count; ++c) {
    const geometry::ClusterDesc& desc = scene.lod.mesh.clusters[c];
    gfx::ClusterBuildInput in;
    in.cluster_id = 0;
    in.triangle_count = desc.triangle_count;
    in.vertex_count = desc.vertex_count;
    in.vertices = scene.vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
    in.indices = indices8.address + u64{desc.triangle_offset} * 3;
    template_inputs.push_back(in);
  }
  gfx::write_cluster_template_records(
      std::span<const gfx::ClusterBuildInput>(template_inputs.data(), template_inputs.size()),
      template_records.mapped);

  // The cut's records, both ways: a rebuild from the pool, and an instantiation of the cluster's
  // template with the same pool positions.
  Vector<gfx::ClusterBuildInput> build_inputs;
  for (u32 k = 0; k < cut_count; ++k) {
    const geometry::ClusterDesc& desc = scene.lod.mesh.clusters[scene.cut[k]];
    gfx::ClusterBuildInput in;
    in.cluster_id = k;  // the visible entry
    in.triangle_count = desc.triangle_count;
    in.vertex_count = desc.vertex_count;
    in.vertices = scene.pool.address + u64{scene.slot_table[k]} * 12;
    in.indices = indices8.address + u64{desc.triangle_offset} * 3;
    build_inputs.push_back(in);
  }
  gfx::write_cluster_build_records(
      std::span<const gfx::ClusterBuildInput>(build_inputs.data(), build_inputs.size()),
      build_records.mapped);

  gfx::FrameContext frames;
  gfx::BindlessSet bindless;
  gfx::GpuTimer timer;
  REQUIRE(frames.create(device, 1, &error));
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  REQUIRE(timer.create(device, 1, 8, &error));
  VkShaderModule deform_module = gfx::create_shader_module(device, shaders::k_deform_spirv,
                                                           shaders::k_deform_spirv_size, &error);
  VkShaderModule trace_module = gfx::create_shader_module(
      device, shaders::k_ray_visibility_spirv, shaders::k_ray_visibility_spirv_size, &error);
  REQUIRE(deform_module != VK_NULL_HANDLE);
  REQUIRE(trace_module != VK_NULL_HANDLE);
  gfx::ComputePipeline deform_pipeline;
  gfx::ComputePipeline trace_pipeline;
  REQUIRE(gfx::create_compute_pipeline(device, deform_module, "deform_main", {},
                                       sizeof(gfx::DeformParams), deform_pipeline, &error));
  const VkDescriptorSetLayout set_layout = bindless.layout();
  REQUIRE(gfx::create_compute_pipeline(device, trace_module, "trace_main",
                                       std::span<const VkDescriptorSetLayout>(&set_layout, 1),
                                       sizeof(u64), trace_pipeline, &error));

  // Frame A: the pool, then the templates. Both are the "once" half of the templates variant.
  const gfx::DeformParams deform_params = scene.params(0.75f);
  f64 template_ms = 0.0;
  {
    VkCommandBuffer cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, deform_pipeline.pipeline);
    vkCmdPushConstants(cb, deform_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(deform_params), &deform_params);
    vkCmdDispatch(cb, cut_count, 1, 1);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    timer.begin(cb, "templates");
    gfx::build_cluster_templates(cb, templates, template_records.address, 0, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  u64 template_bytes = 0;
  const auto* template_sizes = static_cast<const u32*>(templates.sizes.mapped);
  const auto* template_addresses = static_cast<const u64*>(templates.addresses.mapped);
  for (u32 c = 0; c < cluster_count; ++c) {
    template_bytes += template_sizes[c];
    REQUIRE(template_addresses[c] != 0);
  }
  Vector<gfx::ClusterInstantiateInput> instantiate_inputs;
  for (u32 k = 0; k < cut_count; ++k) {
    gfx::ClusterInstantiateInput in;
    in.cluster_id = k;
    in.geometry_index = k;
    in.cluster_template = template_addresses[scene.cut[k]];
    in.vertices = scene.pool.address + u64{scene.slot_table[k]} * 12;
    instantiate_inputs.push_back(in);
  }
  gfx::write_cluster_instantiate_records(std::span<const gfx::ClusterInstantiateInput>(
                                             instantiate_inputs.data(), instantiate_inputs.size()),
                                         instantiate_records.mapped);

  // Frame B: the per-frame half, both ways, timed against each other.
  f64 rebuild_ms = 0.0;
  f64 instantiate_ms = 0.0;
  {
    VkCommandBuffer cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    template_ms = timer.ms("templates");
    timer.begin(cb, "rebuild");
    gfx::build_cluster_set(cb, rebuilt, build_records.address, 0, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    timer.begin(cb, "instantiate");
    gfx::instantiate_cluster_templates(cb, instantiated, instantiate_records.address, 0, scratch);
    timer.end(cb);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    gfx::build_cluster_blas(cb, blas[0], rebuilt.addresses.address, cut_count, scratch);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    gfx::build_cluster_blas(cb, blas[1], instantiated.addresses.address, cut_count, scratch);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    REQUIRE(frames.wait(frames.end_frame()));
  }
  u64 rebuilt_bytes = 0;
  u64 instantiated_bytes = 0;
  const auto* rebuilt_sizes = static_cast<const u32*>(rebuilt.sizes.mapped);
  const auto* instantiated_sizes = static_cast<const u32*>(instantiated.sizes.mapped);
  for (u32 k = 0; k < cut_count; ++k) {
    rebuilt_bytes += rebuilt_sizes[k];
    instantiated_bytes += instantiated_sizes[k];
  }

  u32 scene_slot[2];
  for (u32 i = 0; i < 2; ++i) {
    gfx::TlasInstance record;
    record.blas = blas[i].address;
    REQUIRE(record.blas != 0);
    gfx::write_instances(std::span<const gfx::TlasInstance>(&record, 1), tlas_instances[i].mapped);
    scene_slot[i] = bindless.add_acceleration_structure(tlas[i].handle);
    REQUIRE(scene_slot[i] != gfx::BindlessSet::k_invalid_slot);
    gfx::RayVisibilityParams ray{};
    ray.view_proj = scene.view_proj;
    ray.inv_view_proj = inverse(scene.view_proj);
    ray.camera = Vec4{scene.eye, 0.0f};
    ray.output = vis[i].address;
    ray.instance_base = 0;
    ray.width = k_w;
    ray.height = k_h;
    ray.scene = scene_slot[i];
    std::memcpy(ray_params[i].mapped, &ray, sizeof(ray));
  }
  const u64 ray_address[2] = {ray_params[0].address, ray_params[1].address};

  // Frame C: the two top-level builds and the two traces.
  {
    VkCommandBuffer cb = frames.begin_frame();
    timer.begin_frame(cb, frames.slot());
    rebuild_ms = timer.ms("rebuild");
    instantiate_ms = timer.ms("instantiate");
    for (u32 i = 0; i < 2; ++i) {
      gfx::build_tlas(cb, tlas[i], tlas_instances[i].address, 1, gfx::k_build_fast_trace, scratch);
      gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    for (const gfx::BufferResource& v : vis)
      vkCmdFillBuffer(cb, v.buffer, 0, VK_WHOLE_SIZE, 0);
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline.pipeline);
    bindless.bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
    for (u32 i = 0; i < 2; ++i) {
      vkCmdPushConstants(cb, trace_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u64),
                         &ray_address[i]);
      vkCmdDispatch(cb, gfx::ray_visibility_group_count(k_w), gfx::ray_visibility_group_count(k_h),
                    1);
    }
    gfx::acceleration_build_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    for (u32 i = 0; i < 2; ++i) {
      const VkBufferCopy copy{0, vis_bytes * i, vis_bytes};
      vkCmdCopyBuffer(cb, vis[i].buffer, host.buffer, 1, &copy);
    }
    REQUIRE(frames.wait(frames.end_frame()));
  }

  const auto* rebuilt_out = static_cast<const u64*>(host.mapped);
  const u64* instantiated_out = rebuilt_out + u64{k_w} * k_h;
  const Comparison c = compare(rebuilt_out, instantiated_out, k_w * k_h);
  CHECK(c.covered_a > k_w * k_h / 16);
  CHECK(c.coverage_mismatch * 1000 <= c.covered_a);
  CHECK(c.id_mismatch * 1000 <= c.both);
  CHECK(c.max_depth_diff < 2e-3f);
  MESSAGE("cut " << cut_count << " of " << cluster_count << " clusters; rebuild " << rebuild_ms
                 << " ms (" << rebuilt_bytes << " B), instantiate " << instantiate_ms << " ms ("
                 << instantiated_bytes << " B); " << cluster_count << " templates " << template_ms
                 << " ms (" << template_bytes << " B)");
  MESSAGE("rebuilt vs instantiated: coverage mismatch " << c.coverage_mismatch << ", id mismatch "
                                                        << c.id_mismatch << " of " << c.both
                                                        << ", depth " << c.max_depth_diff);

  gfx::destroy_compute_pipeline(device, deform_pipeline);
  gfx::destroy_compute_pipeline(device, trace_pipeline);
  gfx::destroy_shader_module(device, deform_module);
  gfx::destroy_shader_module(device, trace_module);
  timer.destroy();
  bindless.destroy();
  frames.destroy();
  for (u32 i = 0; i < 2; ++i) {
    gfx::destroy_cluster_blas(device, blas[i]);
    gfx::destroy_acceleration_structure(device, tlas[i]);
    gfx::destroy_buffer(device, tlas_instances[i]);
    gfx::destroy_buffer(device, ray_params[i]);
    gfx::destroy_buffer(device, vis[i]);
  }
  gfx::destroy_cluster_set(device, rebuilt);
  gfx::destroy_cluster_set(device, instantiated);
  gfx::destroy_cluster_templates(device, templates);
  scene.destroy(device);
  for (gfx::BufferResource* b :
       {&host, &indices8, &build_records, &template_records, &instantiate_records, &scratch})
    gfx::destroy_buffer(device, *b);
  device.destroy();
}
