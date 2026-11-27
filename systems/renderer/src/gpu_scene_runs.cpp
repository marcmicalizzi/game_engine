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
  // `create_working_set`), and behind its last entry the frame's two drop counts and the resolve's
  // capped pixels (R213) and its smooth surfaces' reflection rays (R71), which the frame zeroes and
  // reads back with its statistics (`run_drops_offset`).
  constexpr u64 k_entry_bytes = 2 * sizeof(u32);
  visible_run_bytes_ = u64{run_capacity_} * k_entry_bytes;
  return gfx::create_buffer(
      device, u64{visible_entries()} * k_entry_bytes + 4 * sizeof(u32),
      k_address | gfx::BufferUsage::TransferSrc | gfx::BufferUsage::TransferDst, false, visible,
      error);
}

bool GpuScene::fit_runs(u32 capacity, Vector<gfx::BufferResource>* retired, std::string* error) {
  if (capacity > pair_stride_) capacity = pair_stride_;
  if (capacity == 0) capacity = 1;
  runs_fit_ = true;
  if (capacity == run_capacity_) return true;
  // **All or nothing** (W51-5b; renderer.md, "The visible list's runs are sized by the frame"):
  // every buffer the runs size is made at the new capacity while the old ones stand, and only then
  // are the old ones let go. Until W51-5b the old list went first, so a make that failed left the
  // frame a list it had already retired (leaked: the caller returned before freeing it) and a
  // fallback whose own failure left it none, and the pool's slots followed in a relayout whose
  // waited form destroyed the slots every mesh record named before making their replacement.
  // Made beside the old, the peak is what a swapped growth always held: the old ones live on in
  // `retired` until the frames in flight that read them have completed.
  const u32 before = run_capacity_;
  const u64 before_run_bytes = visible_run_bytes_;
  const bool indexed = vertex_records.buffer.valid();
  gfx::BufferResource old_visible = visible;
  gfx::BufferResource old_records = vertex_records;
  visible = gfx::BufferResource{};
  vertex_records = gfx::BufferResource{};
  run_capacity_ = capacity;
  bool ok = make_run_buffer(false, error);
  if (ok && take_fault(fail_runs_)) {
    if (error != nullptr) *error = "the visible list's runs could not be made (a test's fault)";
    ok = false;
  }
  ok = ok && (!indexed || make_run_buffer(true, error));
  // The pool's per-entry slots, one word an entry of the whole list, and the allocator's tile
  // scratch (R104) follow the runs. Both are written by every frame before it reads them, so the
  // new ones start empty, as they always did.
  gfx::BufferResource new_blocks;
  gfx::BufferResource new_slots;
  if (ok && deform_) {
    ok = gfx::create_buffer(*device_, deform_block_bytes(), k_address, false, new_blocks, error) &&
         gfx::create_buffer(*device_, u64{visible_entries()} * sizeof(u32), k_address, false,
                            new_slots, error);
  }
  // A caller that waited for the device rewrites the mesh records whole, into a buffer of their
  // own beside the old; a swapped one has the next frame write them in place through its copies.
  Vector<gfx::MeshDesc> descs;
  gfx::BufferResource new_meshes;
  if (ok && deform_ && retired == nullptr) {
    descs = mesh_descs_;
    for (gfx::MeshDesc& desc : descs)
      desc.deform_slots = new_slots.address;
    ok = gfx::upload_buffer(*device_, descs.data(), descs.size() * sizeof(gfx::MeshDesc), k_storage,
                            new_meshes, error);
  }
  if (!ok) {
    // Nothing a frame has seen: destroyed at once, and the scene is as it was.
    gfx::destroy_buffer(*device_, visible);
    gfx::destroy_buffer(*device_, vertex_records);
    gfx::destroy_buffer(*device_, new_blocks);
    gfx::destroy_buffer(*device_, new_slots);
    gfx::destroy_buffer(*device_, new_meshes);
    visible = old_visible;
    vertex_records = old_records;
    run_capacity_ = before;
    visible_run_bytes_ = before_run_bytes;
    return false;
  }
  retire(old_visible, retired);
  retire(old_records, retired);
  if (deform_) {
    retire(deform_blocks, retired);
    retire(deform_slots, retired);
    deform_blocks = new_blocks;
    deform_slots = new_slots;
    for (gfx::MeshDesc& desc : mesh_descs_)
      desc.deform_slots = deform_slots.address;
    for (RingSlots& ring : ring_slots_) {
      for (TerrainSlot& slot : ring.slots)
        slot.desc.deform_slots = deform_slots.address;
    }
    if (retired == nullptr) {
      gfx::destroy_buffer(*device_, meshes);
      meshes = new_meshes;
    } else {
      // The frames in flight read the old records, which name the old slots, retired above and
      // freed only once those frames have completed.
      for (u32 m = 0; m < mesh_descs_.size(); ++m)
        pending_mesh_writes_.push_back(m);
    }
  }
  return true;
}

}  // namespace engine::renderer
