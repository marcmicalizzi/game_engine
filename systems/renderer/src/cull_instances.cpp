#include <domain/gfx/resources.h>
#include <systems/renderer/scene_renderer.h>

#include <cull_instances.h>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_storage =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;

// A buffer of at least `bytes`, grown by half again so a world that fills its rings reallocates a
// handful of times; the old one goes to `frames`, since a frame in flight may still read it.
bool ensure(const gfx::Device& device, gfx::FrameContext& frames, gfx::BufferResource& buffer,
            u64 bytes, gfx::BufferUsage usage, std::string* error) {
  if (buffer.buffer.valid() && buffer.size >= bytes) return true;
  if (buffer.buffer.valid()) frames.defer_destroy(buffer);
  buffer = gfx::BufferResource{};
  return gfx::create_buffer(device, bytes + bytes / 2u, usage, false, buffer, error);
}

}  // namespace

void record_cull_fold(gfx::CommandList cb, const gfx::ComputePipeline& fold, u64 counts, u64 args,
                      u32 count_stride, u32 args_stride, u32 n) noexcept {
  gfx::FoldArgsParams params;
  params.counts = counts;
  params.args = args;
  params.count_stride = count_stride;
  params.args_stride = args_stride;
  params.n = n;
  cb.bind_pipeline(gfx::BindPoint::Compute, fold.pipeline);
  cb.push_constants(fold.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
  cb.dispatch((n + gfx::k_fold_args_workgroup - 1) / gfx::k_fold_args_workgroup, 1, 1);
}

bool CullInstances::prepare(const gfx::Device& device, gfx::FrameContext& frames, bool want,
                            u32 run_count, u32 instance_count, u32 pair_count, u64 table_bytes,
                            u64 meshes, bool meshes_written, std::string* error) {
  // Every entry of the table: until R173's fix this was the null mesh's index plus one, which a
  // scene read whole does not have (`null_mesh()` is 0 there), so only mesh 0's sphere was ever
  // measured and every other mesh's instances were tested by whatever the buffer held — a point at
  // the instance's origin — and dropped when that point was off the screen or behind a surface.
  const u32 meshes_now = static_cast<u32>(table_bytes / sizeof(gfx::MeshDesc));
  // The bounds pass is a workgroup a mesh in one row of a folded dispatch; a scene with more
  // meshes than that culls by pairs (none flown has a hundredth of it).
  active = want && run_count > 0 && instance_count > 0 && meshes_now > 0 &&
           meshes_now <= gfx::k_dispatch_pitch;
  bounds_due = false;
  if (!active) return true;
  mesh_count = meshes_now;
  runs = run_count;
  const u32 needed = gfx::cull_item_capacity(instance_count, pair_count);
  if (needed > capacity || !items.buffer.valid() ||
      items.size < u64{runs} * (u64{capacity} + 1u) * 8u) {
    if (needed > capacity) capacity = needed + needed / 2u;
    if (items.buffer.valid()) frames.defer_destroy(items);
    items = gfx::BufferResource{};
    if (!gfx::create_buffer(device, u64{runs} * (u64{capacity} + 1u) * 8u,
                            k_storage | gfx::BufferUsage::TransferDst, false, items, error)) {
      active = false;
      return false;
    }
  }
  if (!ensure(device, frames, args, u64{runs} * 16u, k_storage | gfx::BufferUsage::Indirect,
              error)) {
    active = false;
    return false;
  }
  // The spheres follow the mesh table: measured again when a frame writes it, when it is another
  // table (a relayout made it again), and when there are more meshes than they hold.
  if (!bounds.buffer.valid() || bounds.size < u64{mesh_count} * 16u) {
    if (!ensure(device, frames, bounds, u64{mesh_count} * 16u, k_storage, error)) {
      active = false;
      return false;
    }
    bound_meshes = 0;
  }
  bounds_due = meshes_written || meshes != bounded_table || mesh_count != bound_meshes;
  bounded_table = meshes;
  bound_meshes = mesh_count;
  return true;
}

void CullInstances::fill(gfx::CullParams& params, u32 run) const noexcept {
  params.items = active ? run_items(run) : 0u;
  params.item_args = active ? args.address + u64{run} * 16u : 0u;
  params.mesh_bounds = active ? bounds.address : 0u;
  nodes.fill(params, run);  // R188 stage 1: zero unless the nodes are on
}

void CullInstances::import_and_reset(gfx::RenderGraph& graph) {
  if (!active) return;
  rg_items = graph.import_buffer("cull items", items);
  rg_args = graph.import_buffer("cull item args", args);
  // Written only on the frames that measure them, while the frames in flight may still read the
  // last: imported as last read everywhere, so that write waits for them (as the record copies do).
  rg_bounds = graph.import_buffer("mesh bounds", bounds, gfx::PipelineStage::AllCommands,
                                  gfx::MemoryAccess::MemoryRead);
  graph.add_pass(
      "cull items reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_items, gfx::Access::TransferWrite); },
      [this](gfx::CommandList cb, gfx::RenderGraph&) {
        for (u32 r = 0; r < runs; ++r) {
          cb.fill_buffer(items.buffer, run_items(r) - items.address, sizeof(u32), 0u);
        }
      });
  nodes.import_and_reset(graph);  // R188 stage 1
}

void CullInstances::destroy(const gfx::Device& device) noexcept {
  nodes.destroy(device);
  gfx::destroy_buffer(device, items);
  gfx::destroy_buffer(device, args);
  gfx::destroy_buffer(device, bounds);
  capacity = 0;
  runs = 0;
  bound_meshes = 0;
  bounded_table = 0;
  active = false;
  bounds_due = false;
}

bool SceneRenderer::cull_node_guard(u32& failures, u32& checked) const noexcept {
  failures = checked = 0;
  return cull_instances_ != nullptr && cull_instances_->nodes.read_guard(failures, checked);
}

}  // namespace engine::renderer
