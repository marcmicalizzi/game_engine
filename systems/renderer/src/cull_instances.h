#pragma once

// **The cull culls instances first** (R16, R173; docs/subsystems/renderer.md, "The cull culls
// instances first"): the buffers and passes `SceneRenderer::record_frame` puts in front of each
// cull run, kept here so the hub grows by its calls alone. A run's instance pass
// (`cull_instances_main`, cluster_cull.slang) writes its survivors' items and their count, a fold
// turns the count into the folded dispatch, and the run's pair pass (`cull_items_main`) is that
// dispatch, indirect. The meshes' spheres the instance pass tests are measured on the GPU from the
// cluster table (`mesh_bounds_main`) on the frames that write the mesh table, and on the first.

#include <core/base/types.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/dispatch.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/render_graph.h>

#include <cull_nodes.h>
#include <string>

namespace engine::renderer {

struct CullInstances {
  gfx::BufferResource items;   // per run: a u32 count, a pad word, `capacity` u32x2 items
  gfx::BufferResource args;    // gfx::DispatchIndirectArgs per run, 16 bytes apart
  gfx::BufferResource bounds;  // float4 per mesh
  u32 capacity = 0;            // items a run
  u32 runs = 0;
  u32 bound_meshes = 0;   // meshes `bounds` holds
  u64 bounded_table = 0;  // the mesh table they were measured from
  bool active = false;    // this frame culls instances first
  bool bounds_due =
      false;           // this frame measures the meshes' spheres, before its first instance pass
  u32 mesh_count = 0;  // this frame's
  gfx::RgBuffer rg_items, rg_args, rg_bounds;
  // The renderer's, bound once (`bind`): they outlive every frame.
  const gfx::ComputePipeline* instances_pipeline = nullptr;
  const gfx::ComputePipeline* fold_pipeline = nullptr;
  const gfx::ComputePipeline* bounds_pipeline = nullptr;
  const gfx::DeviceCaps* caps = nullptr;
  gfx::GpuTimer* timer = nullptr;
  // **Nodes before instances** (R188 stage 1, `renderer.cull.nodes`; src/cull_nodes.h): when
  // active, each instance pass is the node pass, its fold and the instance pass over the surviving
  // nodes' slots, in the same zone.
  CullNodes nodes;

  void bind(const gfx::ComputePipeline& instances, const gfx::ComputePipeline& fold,
            const gfx::ComputePipeline& mesh_bounds, const gfx::DeviceCaps& device_caps,
            gfx::GpuTimer& gpu_timer) noexcept {
    instances_pipeline = &instances;
    fold_pipeline = &fold;
    bounds_pipeline = &mesh_bounds;
    caps = &device_caps;
    timer = &gpu_timer;
  }
  void bind_nodes(const gfx::ComputePipeline& node_pass, const gfx::ComputePipeline& node_instances,
                  const gfx::ComputePipeline& measure, const gfx::ComputePipeline& guard) noexcept {
    nodes.nodes_pipeline = &node_pass;
    nodes.instances_pipeline = &node_instances;
    nodes.measure_pipeline = &measure;
    nodes.guard_pipeline = &guard;
  }
  // After `prepare`: the nodes for this frame, when the instance pass is on and `want` (src/
  // cull_nodes.h). `bounds_due` is read before the first instance pass clears it.
  bool prepare_nodes(const gfx::Device& device, gfx::FrameContext& frames, bool want,
                     const GpuScene& scene, const GpuScene::TableUpdate& tables,
                     const GpuScene::RecordUpdate& records, u32 frame_slot, bool guard,
                     std::string* error) {
    return nodes.prepare(device, frames, want && active, scene, tables, records, frame_slot,
                         active ? runs : 0u, bounds_due, guard, error);
  }
  // Sizes the buffers for a frame of `run_count` cull runs (a view's two blocks each, then a block
  // per cascade, in the order of the frame's parameter blocks) over `instance_count` instances,
  // `pair_count` pairs and the mesh table, `meshes` its address and `table_bytes` its size — every
  // `gfx::MeshDesc` in it is a mesh an instance may name, the scene's, a ring's or the room's slots
  // and a dynamic scene's null mesh alike — and `meshes_written` whether this frame writes it. Off
  // (`want` false, or more meshes than a row of a folded dispatch) leaves every run the pass over
  // pairs. A buffer outgrown goes to `frames`, to be freed when the frames in flight are done.
  bool prepare(const gfx::Device& device, gfx::FrameContext& frames, bool want, u32 run_count,
               u32 instance_count, u32 pair_count, u64 table_bytes, u64 meshes, bool meshes_written,
               std::string* error);
  // Run `run`'s fields of its block: the items, their arguments, the spheres; 0 when off.
  void fill(gfx::CullParams& params, u32 run) const noexcept;
  // The frame's buffers in the graph and their count words zeroed, before any pass that names them.
  void import_and_reset(gfx::RenderGraph& graph);
  // Before a cull pass: the meshes' spheres if the frame measures them and has not yet, the
  // instance pass of `count` runs, `first_run` on and `run_stride` apart, whose blocks are
  // `sizeof(gfx::CullParams)` apart from `params` on, and the fold of their counts into their
  // dispatches, the last two timed as `zone`. `read_tables` is a callable `(gfx::PassBuilder&)`
  // declaring the reads of the instance, mesh and cluster tables a frame may write; `hiz`, when
  // valid, the Hi-Z the runs test against.
  template <class ReadTables>
  void add_instance_pass(gfx::RenderGraph& graph, const char* zone, u64 params, u32 first_run,
                         u32 run_stride, u32 count, u32 instance_count, gfx::RgBuffer hiz,
                         ReadTables& read_tables);
  // With nodes: the frame's measure and guard if not yet recorded, then the node pass of the same
  // runs, its fold and the instance pass over the survivors' slots (R188 stage 1).
  template <class ReadTables>
  void add_node_passes(gfx::RenderGraph& graph, const char* zone, u64 params, u32 first_run,
                       u32 run_stride, u32 count, gfx::RgBuffer hiz, ReadTables& read_tables);
  // A cull pass's reads of the items and their dispatch.
  void declare_items(gfx::PassBuilder& b) const {
    if (!active) return;
    b.read(rg_items, gfx::Access::ComputeRead);
    b.read(rg_args, gfx::Access::IndirectRead);
  }
  // Where run `run`'s count and items start.
  u64 run_items(u32 run) const noexcept {
    return items.address + u64{run} * (u64{capacity} + 1u) * 8u;
  }
  // In a cull pass's body: run `run`'s pair pass, indirect.
  void dispatch_items(gfx::CommandList cb, u32 run) const noexcept {
    cb.dispatch_indirect(args.buffer, u64{run} * 16u);
  }
  void destroy(const gfx::Device& device) noexcept;
};

void record_cull_fold(gfx::CommandList cb, const gfx::ComputePipeline& fold, u64 counts, u64 args,
                      u32 count_stride, u32 args_stride, u32 n) noexcept;

template <class ReadTables>
void CullInstances::add_instance_pass(gfx::RenderGraph& graph, const char* zone, u64 params,
                                      u32 first_run, u32 run_stride, u32 count, u32 instance_count,
                                      gfx::RgBuffer hiz, ReadTables& read_tables) {
  if (!active || count == 0) return;
  if (bounds_due) {
    bounds_due = false;
    const u64 block = params + sizeof(gfx::CullParams) * first_run;
    graph.add_pass(
        "mesh bounds", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          read_tables(b);
          b.write(rg_bounds, gfx::Access::ComputeWrite);
        },
        [this, block](gfx::CommandList cb, gfx::RenderGraph&) {
          timer->begin(cb, "mesh bounds");
          cb.bind_pipeline(gfx::BindPoint::Compute, bounds_pipeline->pipeline);
          cb.push_constants(bounds_pipeline->layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &block);
          cb.dispatch(mesh_count, 1, 1);
          timer->end(cb);
        });
  }
  if (nodes.active) {
    add_node_passes(graph, zone, params, first_run, run_stride, count, hiz, read_tables);
  } else {
    graph.add_pass(
        "cull instances", gfx::PassKind::Compute,
        [&, hiz](gfx::PassBuilder& b) {
          read_tables(b);
          b.read(rg_bounds, gfx::Access::ComputeRead);
          if (hiz.valid()) b.read(hiz, gfx::Access::ComputeRead);
          b.write(rg_items, gfx::Access::ComputeReadWrite);
        },
        [this, zone, params, first_run, run_stride, count, instance_count](gfx::CommandList cb,
                                                                           gfx::RenderGraph&) {
          timer->begin(cb, zone);
          cb.bind_pipeline(gfx::BindPoint::Compute, instances_pipeline->pipeline);
          const u64 groups =
              (u64{instance_count} + gfx::k_cull_workgroup_size - 1) / gfx::k_cull_workgroup_size;
          for (u32 k = 0; k < count; ++k) {
            const u64 block = params + sizeof(gfx::CullParams) * (first_run + k * run_stride);
            cb.push_constants(instances_pipeline->layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                              &block);
            gfx::dispatch_folded(cb, groups, *caps);
          }
          timer->end(cb);
        });
  }
  graph.add_pass(
      "cull items fold", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(rg_items, gfx::Access::ComputeRead);
        b.write(rg_args, gfx::Access::ComputeWrite);
      },
      [this, zone, first_run, run_stride, count](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, zone);
        record_cull_fold(
            cb, *fold_pipeline, run_items(first_run), args.address + u64{first_run} * 16u,
            static_cast<u32>((u64{capacity} + 1u) * 8u * run_stride), run_stride * 16u, count);
        timer->end(cb);
      });
}

template <class ReadTables>
void CullInstances::add_node_passes(gfx::RenderGraph& graph, const char* zone, u64 params,
                                    u32 first_run, u32 run_stride, u32 count, gfx::RgBuffer hiz,
                                    ReadTables& read_tables) {
  const u64 first_block = params + sizeof(gfx::CullParams) * first_run;
  // The frame's measure, after its table copies and its meshes' spheres (both declared reads), and
  // before any node pass reads the table.
  if (nodes.measure_due) {
    nodes.measure_due = false;
    graph.add_pass(
        "cull node measure", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          read_tables(b);
          b.read(rg_bounds, gfx::Access::ComputeRead);
          b.read(nodes.rg_list, gfx::Access::ComputeRead);
          b.write(nodes.rg_table, gfx::Access::ComputeWrite);
        },
        [this, zone, first_block](gfx::CommandList cb, gfx::RenderGraph&) {
          timer->begin(cb, zone);
          cb.bind_pipeline(gfx::BindPoint::Compute, nodes.measure_pipeline->pipeline);
          cb.push_constants(nodes.measure_pipeline->layout, gfx::ShaderStage::Compute, 0,
                            sizeof(u64), &first_block);
          gfx::dispatch_folded(cb, nodes.list_count, *caps);
          timer->end(cb);
        });
  }
  // The guard, once a frame, untimed: it is the debug builds' and the tests'.
  if (nodes.guard_due) {
    nodes.guard_due = false;
    graph.add_pass(
        "cull node guard", gfx::PassKind::Compute,
        [&](gfx::PassBuilder& b) {
          read_tables(b);
          b.read(rg_bounds, gfx::Access::ComputeRead);
          b.read(nodes.rg_table, gfx::Access::ComputeRead);
          b.write(nodes.rg_guard, gfx::Access::ComputeReadWrite);
        },
        [this, first_block](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Compute, nodes.guard_pipeline->pipeline);
          cb.push_constants(nodes.guard_pipeline->layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &first_block);
          gfx::dispatch_folded(cb, nodes.node_count, *caps);
        });
  }
  graph.add_pass(
      "cull nodes", gfx::PassKind::Compute,
      [&, hiz](gfx::PassBuilder& b) {
        read_tables(b);
        b.read(nodes.rg_table, gfx::Access::ComputeRead);
        if (hiz.valid()) b.read(hiz, gfx::Access::ComputeRead);
        b.write(nodes.rg_items, gfx::Access::ComputeReadWrite);
      },
      [this, zone, params, first_run, run_stride, count](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, zone);
        cb.bind_pipeline(gfx::BindPoint::Compute, nodes.nodes_pipeline->pipeline);
        const u64 groups =
            (u64{nodes.node_count} + gfx::k_cull_workgroup_size - 1) / gfx::k_cull_workgroup_size;
        for (u32 k = 0; k < count; ++k) {
          const u64 block = params + sizeof(gfx::CullParams) * (first_run + k * run_stride);
          cb.push_constants(nodes.nodes_pipeline->layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &block);
          gfx::dispatch_folded(cb, groups, *caps);
        }
        timer->end(cb);
      });
  graph.add_pass(
      "cull nodes fold", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(nodes.rg_items, gfx::Access::ComputeRead);
        b.write(nodes.rg_args, gfx::Access::ComputeWrite);
      },
      [this, zone, first_run, run_stride, count](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, zone);
        record_cull_fold(cb, *fold_pipeline, nodes.run_items(first_run),
                         nodes.args.address + u64{first_run} * 16u,
                         static_cast<u32>(nodes.run_stride() * run_stride), run_stride * 16u,
                         count);
        timer->end(cb);
      });
  graph.add_pass(
      "cull node instances", gfx::PassKind::Compute,
      [&, hiz](gfx::PassBuilder& b) {
        read_tables(b);
        b.read(rg_bounds, gfx::Access::ComputeRead);
        b.read(nodes.rg_table, gfx::Access::ComputeRead);
        b.read(nodes.rg_items, gfx::Access::ComputeRead);
        b.read(nodes.rg_args, gfx::Access::IndirectRead);
        if (hiz.valid()) b.read(hiz, gfx::Access::ComputeRead);
        b.write(rg_items, gfx::Access::ComputeReadWrite);
      },
      [this, zone, params, first_run, run_stride, count](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, zone);
        cb.bind_pipeline(gfx::BindPoint::Compute, nodes.instances_pipeline->pipeline);
        for (u32 k = 0; k < count; ++k) {
          const u32 run = first_run + k * run_stride;
          const u64 block = params + sizeof(gfx::CullParams) * run;
          cb.push_constants(nodes.instances_pipeline->layout, gfx::ShaderStage::Compute, 0,
                            sizeof(u64), &block);
          cb.dispatch_indirect(nodes.args.buffer, u64{run} * 16u);
        }
        timer->end(cb);
      });
}

}  // namespace engine::renderer
