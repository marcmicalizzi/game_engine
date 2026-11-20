// **The visible list's runs are sized by the frame, not the scene** (W51-5; renderer.md, the
// section of that name): `GpuScene::fit_runs` and the buffers laid out by a run's capacity, in a
// file of their own so that the hub `gpu_scene.cpp` does not grow for them.
//
// Every run of the visible list was as long as the scene's pair stride, because no draw can append
// more than every pair; a streamed world's stride is its tail's pairs and half as much again, and
// the list holds three runs a view and one a cascade. Standing in the forest staging at the low
// setting that was 7 runs of 15.8 million entries — 842 MiB of list and 421 MiB of the pool's
// per-entry slots — for frames that drew a few hundred thousand entries a run. The tables indexed
// by a pair (`pair_entries`, `flags`, `fade`, the pair table) are the scene's and stay by the
// stride; what is indexed by an entry follows the run.

#include <domain/gfx/device.h>
#include <systems/renderer/gpu_scene.h>

#include <string>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;

}  // namespace

bool GpuScene::make_run_buffer(bool vertex_path, std::string* error) {
  const gfx::Device& device = *device_;
  if (vertex_path) {
    // The vertex path's indexed draw: a record per entry of each of its runs, two hardware runs a
    // view and one region per shadow cascade behind them.
    const u64 runs = u64{2} * view_count_ + cascade_count_;
    return gfx::create_buffer(device, runs * run_capacity_ * sizeof(gfx::VertexDrawRecord),
                              k_address, false, vertex_records, error);
  }
  // One visible list for the whole frame, in three runs per view and one per shadow cascade (see
  // `create_working_set`), and behind its last entry the frame's two drop counts, which the frame
  // zeroes and reads back with its statistics (`run_drops_offset`).
  constexpr u64 k_entry_bytes = 2 * sizeof(u32);
  visible_run_bytes_ = u64{run_capacity_} * k_entry_bytes;
  return gfx::create_buffer(
      device, u64{visible_entries()} * k_entry_bytes + 2 * sizeof(u32),
      k_address | gfx::BufferUsage::TransferSrc | gfx::BufferUsage::TransferDst, false, visible,
      error);
}

bool GpuScene::fit_runs(u32 capacity, Vector<gfx::BufferResource>* retired, std::string* error) {
  if (capacity > pair_stride_) capacity = pair_stride_;
  if (capacity == 0) capacity = 1;
  runs_fit_ = true;
  if (capacity == run_capacity_) return true;
  const u32 before = run_capacity_;
  const bool indexed = vertex_records.buffer.valid();
  retire(visible, retired);
  retire(vertex_records, retired);
  run_capacity_ = capacity;
  if (!make_run_buffer(false, error) || (indexed && !make_run_buffer(true, error))) {
    // Nothing to draw with at the new capacity: back to the old one, which a frame could draw
    // with (its buffers are retired, so they are made again).
    retire(visible, nullptr);
    retire(vertex_records, nullptr);
    run_capacity_ = before;
    std::string ignored;
    (void)make_run_buffer(false, &ignored);
    if (indexed) (void)make_run_buffer(true, &ignored);
    return false;
  }
  // The pool's per-entry slots and the allocator's scratch follow the runs, and every mesh record
  // names the slots: the relayout makes them again before the next frame, swapped in beside the
  // frames in flight as a growth of the stride is (`relayout`).
  if (deform_) relayout_pending_ = true;
  return true;
}

}  // namespace engine::renderer
