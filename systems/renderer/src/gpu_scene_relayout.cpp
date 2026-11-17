// What a tail's change makes again before the next frame (T152; renderer.md, "A relayout is swapped
// in"): `GpuScene::relayout` and the streamed trees' tables' growth, moved here from gpu_scene.cpp,
// and since T152 made beside the frames in flight rather than after waiting for them.

#include <systems/renderer/gpu_scene.h>

#include <core/profiling/startup_trace.h>  // stages only: timing, never logic
#include <domain/gfx/device.h>

#include <algorithm>
#include <string>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;

}  // namespace

bool GpuScene::rt_layout_outgrown() const noexcept {
  const u32 instance_slots =
      dynamic_ ? std::max(instance_capacity_, instance_count_) : instance_count_;
  return instance_slots != rt_instance_slots_ || pair_stride_ * view_count_ != rt_union_clusters_;
}

// The chain's half a swapped relayout makes beside the old: the per-instance tables and the
// top-level structure at the new capacity, the bottom-level set and the scratch it may need — at
// most half as much again as they hold, the most a capacity or a stride grows by (`fit_tables`).
// Asked of the device's budget as a swapped resize asks it (R116): past it the driver makes room
// by evicting, which made one swapped growth 524 ms where the waited one took 37.
bool GpuScene::rt_relayout_fits() const noexcept {
  gfx::MemoryBudget budget;
  if (!device_->memory_budget(budget) || !budget.valid) return true;
  const u64 held = rt_instances.size + slots.size + instance_counts.size + blas_records.size +
                   blas_dense.size + blas_slots.size + tlas.buffer.size + blas_set.data.size +
                   blas_set.addresses.size + rt_scratch.size;
  return budget.used_bytes + held + held / 2 <= budget.budget_bytes;
}

bool GpuScene::relayout(std::string* error, RelayoutRetired* retired) {
  if (!relayout_pending_) return true;
  ENGINE_STARTUP_STAGE("relayout");  // stages: a flight's growth frames named by their work (T167)
  const bool chain = ray_tracing_ && (rt_layout_outgrown() || rt_relayout_failed_);
  if (retired != nullptr) {
    retired->must_wait = chain && !rt_relayout_fits();
    if (retired->must_wait) return true;  // still pending: the caller waits, and asks without
  }
  relayout_pending_ = false;
  // **A swapped call that fails part way stays pending** (the review of T154, 2026-11-16): what it
  // replaced is in `retired` for the caller to free as on success, and the call the caller makes
  // next, waited and without `retired`, finishes the work. A pool buffer the swapped call failed to
  // make is empty, so the waited call makes it (its size test fails). A failure in the chain marks
  // it to be laid out whole: the swapped lay-out had already moved the instance slots and the union
  // to the new layout's, so `rt_layout_outgrown` alone would not see a chain half replaced. The
  // trees' tables' growth, swapped, puts everything back when a make fails (`grow_flora_tail`,
  // T163), so it stays pending and is retried with the rest.
  const auto failed = [&](bool in_chain) {
    if (retired != nullptr) {
      relayout_pending_ = true;
      rt_relayout_failed_ = rt_relayout_failed_ || in_chain;
    }
    return false;
  };
  Vector<gfx::BufferResource>* old = retired != nullptr ? &retired->buffers : nullptr;
  if (flora_grow_) {
    ENGINE_STARTUP_STAGE("relayout trees");
    if (!grow_flora_tail(error, old)) return failed(false);
  }
  // The pool's per-entry slots are laid out by the stride, and every mesh record names them.
  // The allocator's tile scratch follows the same stride (R104). Both are written by every frame
  // before it reads them, so the new ones start empty, as they always did.
  if (deform_ && deform_blocks.size < deform_block_bytes()) {
    retire(deform_blocks, old);
    if (!gfx::create_buffer(*device_, deform_block_bytes(), k_address, false, deform_blocks,
                            error)) {
      return failed(false);
    }
  }
  if (deform_ && deform_slots.size < u64{visible_entries()} * sizeof(u32)) {
    retire(deform_slots, old);
    if (!gfx::create_buffer(*device_, u64{visible_entries()} * sizeof(u32), k_address, false,
                            deform_slots, error)) {
      return failed(false);
    }
    for (gfx::MeshDesc& desc : mesh_descs_)
      desc.deform_slots = deform_slots.address;
    for (RingSlots& ring : ring_slots_) {
      for (TerrainSlot& slot : ring.slots)
        slot.desc.deform_slots = deform_slots.address;
    }
    if (old == nullptr) {
      gfx::destroy_buffer(*device_, meshes);
      if (!gfx::upload_buffer(*device_, mesh_descs_.data(),
                              mesh_descs_.size() * sizeof(gfx::MeshDesc), k_storage, meshes,
                              error)) {
        return false;
      }
    } else {
      // Swapped, the frame recorded next writes every mesh record in place through its copies, as
      // it writes the record of a mesh that arrives; the frames in flight read the old records,
      // which name the old slots, retired above and freed only once those frames have completed.
      for (u32 m = 0; m < mesh_descs_.size(); ++m)
        pending_mesh_writes_.push_back(m);
    }
  }
  // Until T152 the chain was laid out again on every relayout, a growth of the trees' tables alone
  // included — 70 to 83 ms of the frame's thread on the engine's streamed forest for buffers whose
  // sizes had not changed.
  if (chain) {
    ENGINE_STARTUP_STAGE("relayout chain");
    if (!lay_out_ray_tracing(error, retired)) return failed(true);
    rt_relayout_failed_ = false;
  }
  return true;
}

// The deform table, its per-frame copies and the pose table made again, half as much again as the
// records need, every record the CPU holds copied in. With `retired` (T152) the old ones go there
// and the copies are made again with the table whatever grew: the frames in flight read the ones
// they have, and writing a new record into those would write under them.
// **Swapped, it cannot fail part way** (T163): the old buffers are set aside rather than retired
// until every new one is made, so a make that fails destroys what was made, puts the old buffers
// and the capacities back, and leaves the growth pending — the scene exactly as before the call,
// for the waited call `relayout` is retried with. Until T163 the capacities had moved and the old
// buffers were retired by then, so a failure could not be resumed and stopped the host. Waited, the
// old buffers go first, so the device never holds both sets (it may be waiting for want of room),
// and a failure is returned as the waited chain's is.
bool GpuScene::grow_flora_tail(std::string* error, Vector<gfx::BufferResource>* retired) {
  const u32 deform_capacity = deform_capacity_;
  const u32 part_capacity = flora_part_capacity_;
  const u64 pose_bytes = flora_stats_.pose_table_bytes;
  bool grew = false;
  if (deform_descs_.size() > deform_capacity_) {
    deform_capacity_ = deform_descs_.size() + deform_descs_.size() / 2;
    grew = true;
  }
  const bool poses = flora_parts_ > flora_part_capacity_;
  if (poses) flora_part_capacity_ = flora_parts_ + flora_parts_ / 2;
  // The table is written whole again whether or not it grew: a record made past it was not.
  const bool frames = deform_frames.buffer.valid() && (grew || retired != nullptr);
  gfx::BufferResource old_poses;
  gfx::BufferResource old_table;
  gfx::BufferResource old_frames;
  const auto set_aside = [&](gfx::BufferResource& buffer, gfx::BufferResource& old) {
    if (retired != nullptr) {
      old = buffer;
      buffer = gfx::BufferResource{};
    } else {
      retire(buffer, nullptr);
    }
  };
  if (poses) set_aside(flora_poses_, old_poses);
  set_aside(deform_table, old_table);
  if (frames) set_aside(deform_frames, old_frames);
  bool ok = !poses || make_flora_poses(error);
  if (ok && take_fault(fail_flora_)) {
    if (error != nullptr) *error = "the trees' tables' growth failed (a test's fault)";
    ok = false;
  }
  ok = ok && make_deform_tables(frames, error);
  if (!ok) {
    if (retired == nullptr) return false;
    if (poses) gfx::destroy_buffer(*device_, flora_poses_);
    gfx::destroy_buffer(*device_, deform_table);
    if (frames) gfx::destroy_buffer(*device_, deform_frames);
    if (poses) flora_poses_ = old_poses;
    deform_table = old_table;
    if (frames) deform_frames = old_frames;
    deform_capacity_ = deform_capacity;
    flora_part_capacity_ = part_capacity;
    flora_stats_.pose_table_bytes = pose_bytes;
    return false;  // `flora_grow_` stands
  }
  flora_grow_ = false;
  retire(old_poses, retired);
  retire(old_table, retired);
  retire(old_frames, retired);
  flora_state_.reserve(flora_part_capacity_);
  deform_descs_.reserve(deform_capacity_);
  deform_instance_.reserve(deform_capacity_);
  deform_mesh_clusters_.reserve(deform_capacity_);
  tail_trees_.reserve(deform_capacity_ - load_deforms_);
  flora_counts();
  return true;
}

}  // namespace engine::renderer
