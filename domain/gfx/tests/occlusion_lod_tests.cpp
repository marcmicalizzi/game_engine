// The occlusion term of the LOD threshold end to end (R118; renderer.md, "Occlusion-aware LOD"). A
// crown — a bumpy sphere with a real LOD DAG — stands behind a screen pierced by square holes, the
// way a crown stands behind the gaps of a nearer one. The cull pass runs as a frame without
// two-pass occlusion culling runs it (the ray path, ray-traced shadows): one pass, reading last
// frame's Hi-Z for the term alone (`gfx::k_cull_hiz_lod_only`), the Hi-Z rebuilt from each frame's
// visibility buffer for the next. With the term off the crown's cut is the same behind the screen
// and alone; with it on, the crown behind the screen is cut coarser than at k 0 and coarser than
// alone, the screen's cut does not move, and every pixel the term changes is one where the crown
// shows — in the holes — and nowhere else. A static scene's cut settles: its last two frames draw
// the same pairs. Skips only without 64-bit buffer atomics.
#include "raster_path.h"

#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/cluster_cull.spv.h>
#include <shaders/hiz_build.spv.h>
#include <string>

using namespace engine;

namespace {

// A sphere of radius about `radius` at `center`, its surface pushed in and out so simplification
// has real error to measure: the DAG has several levels, like a crown's.
void make_crown(Vec3 center, f32 radius, u32 rings, u32 segments, Vector<Vec3>& positions,
                Vector<u32>& indices) {
  const u32 first = positions.size();
  positions.push_back(center + Vec3{0.0f, radius, 0.0f});
  for (u32 r = 1; r < rings; ++r) {
    const f32 theta = k_pi * static_cast<f32>(r) / static_cast<f32>(rings);
    for (u32 s = 0; s < segments; ++s) {
      const f32 phi = 2.0f * k_pi * static_cast<f32>(s) / static_cast<f32>(segments);
      const f32 bump = 1.0f + 0.07f * std::sin(5.0f * theta) * std::cos(7.0f * phi) +
                       0.04f * std::sin(11.0f * phi + 3.0f * theta);
      const f32 rr = radius * bump;
      positions.push_back(center + Vec3{rr * std::sin(theta) * std::cos(phi), rr * std::cos(theta),
                                        rr * std::sin(theta) * std::sin(phi)});
    }
  }
  const u32 bottom = positions.size();
  positions.push_back(center - Vec3{0.0f, radius, 0.0f});
  const auto ring = [&](u32 r, u32 s) { return first + 1 + (r - 1) * segments + s % segments; };
  for (u32 s = 0; s < segments; ++s) {
    indices.push_back(first);
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

// A screen across the view at depth `z`, facing +z, with a two-by-two-cell hole in every six by
// six: a ninth of it open.
void make_screen(f32 z, Vector<Vec3>& positions, Vector<u32>& indices) {
  constexpr u32 k_cells_x = 42;
  constexpr u32 k_cells_y = 32;
  constexpr f32 k_cell = 0.4f;
  const f32 x0 = -0.5f * k_cells_x * k_cell;
  const f32 y0 = -0.5f * k_cells_y * k_cell;
  const u32 first = positions.size();
  for (u32 j = 0; j <= k_cells_y; ++j) {
    for (u32 i = 0; i <= k_cells_x; ++i) {
      positions.push_back(
          Vec3{x0 + static_cast<f32>(i) * k_cell, y0 + static_cast<f32>(j) * k_cell, z});
    }
  }
  const auto at = [&](u32 i, u32 j) { return first + j * (k_cells_x + 1) + i; };
  for (u32 j = 0; j < k_cells_y; ++j) {
    for (u32 i = 0; i < k_cells_x; ++i) {
      if ((i % 6 == 2 || i % 6 == 3) && (j % 6 == 2 || j % 6 == 3)) continue;  // a hole
      indices.push_back(at(i, j));
      indices.push_back(at(i + 1, j));
      indices.push_back(at(i + 1, j + 1));
      indices.push_back(at(i, j));
      indices.push_back(at(i + 1, j + 1));
      indices.push_back(at(i, j + 1));
    }
  }
}

}  // namespace

TEST_CASE("occlusion lod: a crown behind a screen is cut coarser, and only its slivers change") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;

  // Two meshes merged into one scene: the crown first, so its pairs are [0, crown_pairs).
  Vector<geometry::ClusterLodMesh> meshes(2);
  {
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_crown(Vec3{0.0f, 0.0f, -8.0f}, 2.5f, 64, 128, positions, indices);  // 16,128 triangles
    REQUIRE_MESSAGE(geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{},
                                                meshes[0], &error),
                    error);
    positions.clear();
    indices.clear();
    make_screen(-3.0f, positions, indices);
    REQUIRE_MESSAGE(geometry::build_cluster_lod(positions, indices, geometry::ClusterLodOptions{},
                                                meshes[1], &error),
                    error);
  }
  REQUIRE(meshes[0].level_cluster_counts.size() >= 4);  // a DAG with somewhere to go
  geometry::ClusterLodMesh lod;
  Vector<geometry::ClusterMeshPart> parts;
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(meshes, lod, parts, &error), error);
  const u32 cluster_count = lod.mesh.clusters.size();
  const u32 crown_pairs = parts[0].cluster_count;
  const u32 pair_count = crown_pairs + parts[1].cluster_count;
  Vector<gfx::InstanceDesc> instance_descs;
  for (u32 m = 0; m < 2; ++m) {
    gfx::InstanceDesc instance{};
    gfx::set_instance_placement(instance, Mat4::identity(), WorldPos::origin());
    instance.mesh = m;
    instance.first_pair = m == 0 ? 0u : crown_pairs;
    instance_descs.push_back(instance);
  }

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
  constexpr gfx::BufferUsage k_args = k_address | gfx::BufferUsage::Indirect |
                                      gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  gfx::BufferResource clusters;
  gfx::BufferResource lods;
  gfx::BufferResource quantized;
  gfx::BufferResource mesh_buffer;
  gfx::BufferResource instances;
  gfx::BufferResource triangles;
  REQUIRE(gfx::upload_buffer(device, lod.mesh.clusters.data(),
                             cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.lod.data(),
                             cluster_count * sizeof(geometry::ClusterLodDesc), k_storage, lods,
                             &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.quantized.data(),
                             lod.mesh.quantized.size() * sizeof(u16), k_storage, quantized,
                             &error));
  Vector<gfx::MeshDesc> mesh_descs;
  for (const geometry::ClusterMeshPart& part : parts) {
    gfx::MeshDesc desc{};
    desc.quant = Vec4{part.quant_origin, part.quant_scale};
    desc.quantized = quantized.address;
    desc.first_cluster = part.first_cluster;
    desc.cluster_count = part.cluster_count;
    mesh_descs.push_back(desc);
  }
  REQUIRE(gfx::upload_buffer(device, mesh_descs.data(), mesh_descs.size() * sizeof(gfx::MeshDesc),
                             k_storage, mesh_buffer, &error));
  REQUIRE(gfx::upload_buffer(device, instance_descs.data(), 2 * sizeof(gfx::InstanceDesc),
                             k_storage, instances, &error));
  REQUIRE(gfx::upload_buffer(device, lod.mesh.triangles.data(),
                             lod.mesh.triangles.size() * sizeof(u32), k_storage, triangles,
                             &error));

  constexpr u32 k_w = 384;
  constexpr u32 k_h = 256;
  const Vec3 eye{0.0f, 0.5f, 6.0f};
  const f32 znear = 0.1f;
  const f32 fov_y = radians(50.0f);
  const Mat4 view_proj = perspective_reversed_z(fov_y, static_cast<f32>(k_w) / k_h, znear) *
                         look_at(eye, Vec3{0.0f, 0.0f, -8.0f}, Vec3{0, 1, 0});
  const f32 proj_scale = 1.0f / std::tan(fov_y * 0.5f) * static_cast<f32>(k_h) * 0.5f;

  const u64 vis_bytes = u64{k_w} * k_h * sizeof(u64);
  const u64 visible_bytes = u64{pair_count} * 8;
  u32 hiz_offsets[gfx::k_hiz_max_mips];
  const u32 hiz_elements = gfx::hiz_layout(k_w, k_h, hiz_offsets);
  const u32 hiz_mips = gfx::hiz_mip_count(k_w, k_h);
  gfx::BufferResource vis;
  gfx::BufferResource hiz;
  gfx::BufferResource visible;
  gfx::BufferResource args;
  gfx::BufferResource params;
  gfx::BufferResource host_vis;
  gfx::BufferResource host_list;
  gfx::BufferResource host_args;
  const gfx::BufferUsage k_copyable =
      k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc;
  REQUIRE(gfx::create_buffer(device, vis_bytes, k_copyable, false, vis, &error));
  REQUIRE(gfx::create_buffer(device, u64{hiz_elements} * 4, k_copyable, false, hiz, &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, k_copyable, false, visible, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_draw_args_bytes, k_args, false, args, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::CullParams), k_address, true, params, &error));
  REQUIRE(
      gfx::create_buffer(device, vis_bytes, gfx::BufferUsage::TransferDst, true, host_vis, &error));
  REQUIRE(gfx::create_buffer(device, visible_bytes, gfx::BufferUsage::TransferDst, true, host_list,
                             &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_draw_args_bytes, gfx::BufferUsage::TransferDst, true,
                             host_args, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ShaderModuleHandle cull_module = gfx::create_shader_module(
      device, shaders::k_cluster_cull_spirv, shaders::k_cluster_cull_spirv_size, &error);
  gfx::ShaderModuleHandle hiz_module = gfx::create_shader_module(
      device, shaders::k_hiz_build_spirv, shaders::k_hiz_build_spirv_size, &error);
  REQUIRE(cull_module.valid());
  REQUIRE(hiz_module.valid());
  gfx::ComputePipeline cull_pipeline;
  gfx::ComputePipeline hiz_pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, cull_module, "cull_main", {}, sizeof(u64),
                                               cull_pipeline, &error),
                  error);
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, hiz_module, "hiz_build_main", {},
                                               sizeof(gfx::HizParams), hiz_pipeline, &error),
                  error);
  gfx_test::ClusterRaster raster;
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                geometry::ClusterLodOptions{}.max_triangles, &error),
                  error);

  gfx::ClusterDrawParams draw{};
  draw.view_proj = view_proj;
  draw.clusters = clusters.address;
  draw.mesh = mesh_buffer.address;
  draw.instances = instances.address;
  draw.triangles = triangles.address;
  draw.visibility = vis.address;
  draw.extent = gfx::draw_extent(k_w, k_h);
  draw.visible = visible.address;
  draw.visible_offset = 0;
  const u32 hiz_dispatches = gfx::hiz_dispatch_count(hiz_mips);
  Vector<gfx::HizParams> hiz_params(hiz_dispatches);
  for (u32 d = 0; d < hiz_dispatches; ++d) {
    gfx::HizParams& p = hiz_params[d];
    const u32 src_mip = gfx::hiz_dispatch_src_mip(d);
    p.from_visibility = d == 0 ? 1u : 0u;
    p.src = d == 0 ? vis.address : hiz.address + u64{hiz_offsets[src_mip]} * 4;
    p.pyramid = hiz.address;
    p.width = k_w;
    p.height = k_h;
    p.src_mip = src_mip;
    p.src_offset = hiz_offsets[src_mip];
    p.levels = gfx::hiz_dispatch_levels(hiz_mips, d);
  }
  const u64 block_address = params.address;
  auto* block = static_cast<gfx::CullParams*>(params.mapped);
  const auto* args_out = static_cast<const u32*>(host_args.mapped);
  const auto* list_out = static_cast<const u32*>(host_list.mapped);

  struct Run {
    Vector<u64> picture;
    u32 crown_triangles = 0;
    u32 screen_triangles = 0;
    u32 drawn = 0;
    u32 drawn_before = 0;  // the frame before the last
  };
  // One configuration, four frames: the Hi-Z starts empty (nothing hidden, the plain cut), and each
  // frame's cull reads the pyramid the frame before it built.
  constexpr u32 k_frames = 4;
  const auto run = [&](f32 k, bool with_screen, Run& out) {
    gfx::CullParams cull{};
    gfx::set_frustum(cull, frustum_from_view_proj(view_proj));
    cull.view_proj = view_proj;
    cull.camera = Vec4{eye, znear};
    cull.lod = Vec4{proj_scale, 0.5f, 1.0f, 1.0f};
    cull.raster = Vec4{0.0f, gfx::k_raster_hardware, 0.0f, 0.0f};
    cull.cluster_count = cluster_count;
    cull.count_index = raster.count_index();
    cull.clusters = clusters.address;
    cull.lods = lods.address;
    cull.instances = instances.address;
    cull.meshes = mesh_buffer.address;
    cull.instance_count = with_screen ? 2u : 1u;
    cull.pair_count = with_screen ? pair_count : crown_pairs;
    cull.visible = visible.address;
    cull.draw_args = args.address;
    cull.hiz = hiz.address;
    cull.hiz_width = k_w;
    cull.hiz_height = k_h;
    cull.hiz_mips = hiz_mips;
    std::memcpy(cull.hiz_offsets, hiz_offsets, sizeof(hiz_offsets));
    cull.hiz_flags = gfx::k_cull_hiz_lod_only | (1u << gfx::k_cull_hiz_bias_shift);
    cull.occluded_scale = k;
    frames.wait_idle();
    *block = cull;
    const u32 dispatch_pairs = cull.pair_count;
    for (u32 frame = 0; frame < k_frames; ++frame) {
      const bool last = frame + 1 == k_frames;
      gfx::RenderGraph graph(device);
      const gfx::RgBuffer rg_vis = graph.import_buffer("vis", vis);
      const gfx::RgBuffer rg_hiz = graph.import_buffer("hiz", hiz);
      const gfx::RgBuffer rg_visible = graph.import_buffer("visible", visible);
      const gfx::RgBuffer rg_args = graph.import_buffer("args", args);
      const gfx::RgBuffer rg_host_vis = graph.import_buffer("host vis", host_vis);
      const gfx::RgBuffer rg_host_list = graph.import_buffer("host list", host_list);
      const gfx::RgBuffer rg_host_args = graph.import_buffer("host args", host_args);
      const bool clear_hiz = frame == 0;
      graph.add_pass(
          "reset", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            b.write(rg_vis, gfx::Access::TransferWrite);
            b.write(rg_args, gfx::Access::TransferWrite);
            if (clear_hiz) b.write(rg_hiz, gfx::Access::TransferWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.fill_buffer(vis.buffer, 0, gfx::k_whole_size, 0);
            raster.reset_args(cb, args.buffer);
            if (clear_hiz) cb.fill_buffer(hiz.buffer, 0, gfx::k_whole_size, 0);
          });
      graph.add_pass(
          "cull", gfx::PassKind::Compute,
          [&](gfx::PassBuilder& b) {
            b.write(rg_args, gfx::Access::ComputeReadWrite);
            b.write(rg_visible, gfx::Access::ComputeWrite);
            b.read(rg_hiz, gfx::Access::ComputeRead);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.bind_pipeline(gfx::BindPoint::Compute, cull_pipeline.pipeline);
            cb.push_constants(cull_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &block_address);
            cb.dispatch(gfx::cull_group_count(dispatch_pairs), 1, 1);
          });
      graph.add_pass(
          "draw", gfx::PassKind::Raster,
          [&](gfx::PassBuilder& b) {
            b.render_area(k_w, k_h);
            b.write(rg_vis, gfx::Access::FragmentReadWrite);
            b.read(rg_args, gfx::Access::IndirectRead);
            b.read(rg_visible, raster.geometry_read());
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            raster.draw_indirect(cb, bindless, draw, args.buffer);
          });
      for (u32 d = 0; d < hiz_dispatches; ++d) {
        const gfx::HizParams* level = &hiz_params[d];
        const u32 src_w = gfx::hiz_mip_extent(k_w, level->src_mip);
        const u32 src_h = gfx::hiz_mip_extent(k_h, level->src_mip);
        graph.add_pass(
            "hiz", gfx::PassKind::Compute,
            [&, d](gfx::PassBuilder& b) {
              if (d == 0) b.read(rg_vis, gfx::Access::ComputeRead);
              b.write(rg_hiz, gfx::Access::ComputeReadWrite);
            },
            [&, level, src_w, src_h](gfx::CommandList cb, gfx::RenderGraph&) {
              cb.bind_pipeline(gfx::BindPoint::Compute, hiz_pipeline.pipeline);
              cb.push_constants(hiz_pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(*level),
                                level);
              cb.dispatch(gfx::hiz_group_count(src_w), gfx::hiz_group_count(src_h), 1);
            });
      }
      graph.add_pass(
          "readback", gfx::PassKind::Transfer,
          [&](gfx::PassBuilder& b) {
            b.read(rg_vis, gfx::Access::TransferRead);
            b.read(rg_visible, gfx::Access::TransferRead);
            b.read(rg_args, gfx::Access::TransferRead);
            b.write(rg_host_vis, gfx::Access::TransferWrite);
            b.write(rg_host_list, gfx::Access::TransferWrite);
            b.write(rg_host_args, gfx::Access::TransferWrite);
          },
          [&](gfx::CommandList cb, gfx::RenderGraph&) {
            cb.copy_buffer(vis.buffer, host_vis.buffer, gfx::BufferCopy{0, 0, vis_bytes});
            cb.copy_buffer(visible.buffer, host_list.buffer, gfx::BufferCopy{0, 0, visible_bytes});
            cb.copy_buffer(args.buffer, host_args.buffer,
                           gfx::BufferCopy{0, 0, gfx::k_draw_args_bytes});
          });
      REQUIRE_MESSAGE(graph.compile(&error), error);
      gfx::CommandList commands = frames.begin_frame();
      graph.execute(commands);
      REQUIRE(frames.wait(frames.end_frame()));
      graph.reset();
      if (frame + 2 == k_frames) out.drawn_before = raster.survivors(args_out);
      if (!last) continue;
      out.drawn = raster.survivors(args_out);
      out.crown_triangles = 0;
      out.screen_triangles = 0;
      for (u32 e = 0; e < out.drawn; ++e) {
        const u32 instance = list_out[e * 2];
        const u32 cluster = list_out[e * 2 + 1];
        (instance == 0 ? out.crown_triangles : out.screen_triangles) +=
            lod.mesh.clusters[cluster].triangle_count;
      }
      const auto* picture = static_cast<const u64*>(host_vis.mapped);
      out.picture.assign(picture, picture + u64{k_w} * k_h);
    }
  };

  constexpr f32 k_scale = 4.0f;
  Run alone_off;
  Run alone_on;
  Run behind_off;
  Run behind_on;
  run(0.0f, false, alone_off);
  run(k_scale, false, alone_on);
  run(0.0f, true, behind_off);
  run(k_scale, true, behind_on);
  MESSAGE(std::string(raster.name())
          << " path; crown triangles: alone " << alone_off.crown_triangles << " (k 0) and "
          << alone_on.crown_triangles << " (k " << k_scale << "), behind the screen "
          << behind_off.crown_triangles << " and " << behind_on.crown_triangles
          << "; screen triangles " << behind_off.screen_triangles << " and "
          << behind_on.screen_triangles);
  // Off, the cut does not depend on what stands in front.
  CHECK(alone_off.crown_triangles == behind_off.crown_triangles);
  CHECK(alone_off.crown_triangles > 0);
  // On, the crown behind the screen is coarser than at k 0 and than alone, and the screen, which
  // nothing hides, keeps its cut.
  CHECK(behind_on.crown_triangles < behind_off.crown_triangles);
  CHECK(behind_on.crown_triangles < alone_on.crown_triangles);
  CHECK(behind_on.screen_triangles == behind_off.screen_triangles);
  // Alone, the crown's far side is hidden by its near side, so the term coarsens it a little too.
  CHECK(alone_on.crown_triangles <= alone_off.crown_triangles);
  // The cut settles on a static scene: the last two frames draw the same number of pairs.
  CHECK(behind_on.drawn == behind_on.drawn_before);
  CHECK(alone_on.drawn == alone_on.drawn_before);

  // The picture: every pixel the term changed is one where the crown shows, with it or without it.
  const auto pair_of = [](u64 word) { return static_cast<u32>(word & 0xFFFFFFFFu) >> 8; };
  u32 changed = 0;
  u32 changed_elsewhere = 0;
  u32 crown_visible = 0;
  u32 screen_pixels = 0;
  for (u32 i = 0; i < k_w * k_h; ++i) {
    const u64 a = behind_off.picture[i];
    const u64 b = behind_on.picture[i];
    const bool crown_a = a != 0 && pair_of(a) < crown_pairs;
    const bool crown_b = b != 0 && pair_of(b) < crown_pairs;
    crown_visible += crown_a ? 1u : 0u;
    screen_pixels += a != 0 && !crown_a ? 1u : 0u;
    if (a == b) continue;
    ++changed;
    if (!crown_a && !crown_b) ++changed_elsewhere;
  }
  MESSAGE("behind the screen: " << crown_visible << " px of crown show through the holes, "
                                << screen_pixels << " px of screen; the term changed " << changed
                                << " px, " << changed_elsewhere
                                << " where the crown shows in neither");
  CHECK(crown_visible > 0);
  CHECK(screen_pixels > k_w * k_h / 2);
  CHECK(changed_elsewhere == 0);
  CHECK(changed <= crown_visible + crown_visible / 4);

  raster.destroy(device);
  gfx::destroy_compute_pipeline(device, hiz_pipeline);
  gfx::destroy_compute_pipeline(device, cull_pipeline);
  gfx::destroy_shader_module(device, hiz_module);
  gfx::destroy_shader_module(device, cull_module);
  bindless.destroy();
  for (gfx::BufferResource* b :
       {&host_args, &host_list, &host_vis, &params, &args, &visible, &hiz, &vis, &triangles,
        &instances, &mesh_buffer, &quantized, &lods, &clusters}) {
    gfx::destroy_buffer(device, *b);
  }
  frames.destroy();
  device.destroy();
}
