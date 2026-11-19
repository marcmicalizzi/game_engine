#pragma once

// **Nodes before instances** (R188 stage 1; docs/subsystems/renderer.md, "Nodes before instances";
// the design, docs/experiments/instance-hierarchy.md): the GPU half. A node table a table set (the
// instance tables are flipped in turn, so each set's nodes are measured from that set's instances),
// a measure list a frame slot, a run's surviving node ids and their folded dispatch, and the
// guard's two counts. `CullInstances` owns one and records its passes in front of each instance
// pass, so `SceneRenderer::record_frame` grows by a call.
//
// **What keeps a node's sphere true**: the frame that flips to a set measures again every node of
// the slots that set's pending runs write (`GpuScene::TableUpdate::copies`, the record upload's
// instance regions for a scene read whole) and every id whose slots the layout changed since the
// set was last current; every node of a set made again or carried over (T167), of every set on a
// frame that measures the meshes' spheres, and of every set when the nodes are switched on. The
// measure runs after the frame's copies and before its first node pass.

#include <core/base/types.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/render_graph.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/node_layout.h>

#include <string>

namespace engine::renderer {

struct CullNodes {
  NodeLayout layout;
  Vector<gfx::BufferResource> tables;  // a node table a table set
  Vector<u8> whole;                    // per set: measure every node when it is next current
  Vector<gfx::BufferResource> lists;   // a measure list a frame slot, host-visible
  gfx::BufferResource items;           // per run: a u32 count, a pad word, `capacity` u32 ids
  gfx::BufferResource args;            // gfx::DispatchIndirectArgs per run, 16 bytes apart
  gfx::BufferResource guard;           // host-visible: instances not held, instances checked
  u32 capacity = 0;                    // nodes a table holds
  u32 runs = 0;
  u32 set = 0;  // this frame's
  u32 slot = 0;
  u32 node_count = 0;
  u32 list_count = 0;
  bool active = false;
  bool cut = false;
  bool measure_due = false;  // this frame measures `list_count` nodes before its first node pass
  bool guard_on = false;
  bool guard_due = false;
  Vector<u32> ids;                  // scratch: this frame's list
  Vector<NodeLayout::Slot> prefix;  // scratch: the prefix's cut
  gfx::RgBuffer rg_table, rg_list, rg_items, rg_args, rg_guard;
  const gfx::ComputePipeline* nodes_pipeline = nullptr;
  const gfx::ComputePipeline* instances_pipeline = nullptr;
  const gfx::ComputePipeline* measure_pipeline = nullptr;
  const gfx::ComputePipeline* guard_pipeline = nullptr;

  // Before anything is recorded: the layout brought to the scene's blocks, the buffers sized for
  // `run_count` runs, and this frame's measure list staged in frame slot `frame_slot`'s list. Off
  // (`want` false, or no node) leaves every run the instance pass over every instance.
  bool prepare(const gfx::Device& device, gfx::FrameContext& frames, bool want,
               const GpuScene& scene, const GpuScene::TableUpdate& table_update,
               const GpuScene::RecordUpdate& records, u32 frame_slot, u32 run_count,
               bool bounds_due, bool guard_wanted, std::string* error);
  // Run `run`'s fields of its block; 0 when off.
  void fill(gfx::CullParams& params, u32 run) const noexcept;
  // The frame's buffers in the graph and the runs' counts zeroed.
  void import_and_reset(gfx::RenderGraph& graph);
  u64 run_stride() const noexcept { return ((u64{capacity} + 2u) * 4u + 15u) & ~u64{15}; }
  u64 run_items(u32 run) const noexcept { return items.address + u64{run} * run_stride(); }
  // The guard's counts since the renderer was made; false when it never ran.
  bool read_guard(u32& failures, u32& checked) const noexcept;
  void destroy(const gfx::Device& device) noexcept;
};

}  // namespace engine::renderer
