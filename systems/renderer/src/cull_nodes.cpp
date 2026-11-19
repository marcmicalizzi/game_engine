#include <domain/gfx/resources.h>

#include <algorithm>
#include <cstring>
#include <cull_nodes.h>

namespace engine::renderer {

namespace {

constexpr gfx::BufferUsage k_storage =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;

// The slots a copy region of instance records writes.
void list_written(const NodeLayout& layout, std::span<const gfx::BufferCopy> regions,
                  Vector<u32>& ids) {
  for (const gfx::BufferCopy& region : regions) {
    const u32 begin = static_cast<u32>(region.dst_offset / sizeof(gfx::InstanceDesc));
    const u32 end = static_cast<u32>((region.dst_offset + region.size) / sizeof(gfx::InstanceDesc));
    u32 last = ~u32{0};
    for (u32 s = begin; s < end; ++s) {
      const u32 id = layout.node_of(s);
      if (id == ~u32{0} || id == last) continue;
      ids.push_back(id);
      last = id;
    }
  }
}

}  // namespace

bool CullNodes::prepare(const gfx::Device& device, gfx::FrameContext& frames, bool want,
                        const GpuScene& scene, const GpuScene::TableUpdate& table_update,
                        const GpuScene::RecordUpdate& records, u32 frame_slot, u32 run_count,
                        bool bounds_due, bool guard_wanted, std::string* error) {
  // A list the last frame staged and no pass of it measured: that set is measured whole next.
  if (measure_due && set < whole.size()) whole[set] = 1;
  measure_due = false;
  guard_due = false;
  const bool was_active = active;
  active = false;
  if (!want || run_count == 0 || scene.instance_count() == 0) return true;
  const bool dynamic = scene.dynamic();
  const u32 sets = dynamic ? std::max(1u, scene.table_sets()) : 1u;
  if (!cut) {
    // The load's prefix, cut once: a scene read whole is all prefix.
    const u32 prefix_end =
        dynamic ? scene.tile_layout().static_instances() : scene.instance_count();
    prefix.clear();
    for (u32 i = 0; i < prefix_end; ++i) {
      const gfx::InstanceDesc* record = scene.instance_record(i);
      NodeLayout::Slot info;
      if (record != nullptr) {
        info.cell[0] = record->cell.x;
        info.cell[1] = record->cell.y;
        info.cell[2] = record->cell.z;
        info.terrain = record->terrain;
      }
      prefix.push_back(info);
    }
    layout.reset(sets);
    layout.cut_prefix(std::span<const NodeLayout::Slot>(prefix.data(), prefix.size()));
    cut = true;
  }
  const bool fresh = !was_active || whole.size() != sets;
  if (fresh) {
    layout.set_sets(sets);
    whole.resize(sets);
    for (u8& w : whole)
      w = 1;
  }
  // The tail's nodes follow its blocks; they change only with slots a change wrote, which flips
  // the frame's tables.
  if (dynamic && (fresh || table_update.write)) layout.place_tail(scene.tile_layout().blocks());
  node_count = layout.node_count();
  if (node_count == 0) return true;
  runs = run_count;
  set = dynamic ? table_update.set : 0u;
  if (set >= sets) set = 0;
  slot = frame_slot;
  // The tables, one a set, grown by half again: a new table is measured whole.
  if (node_count > capacity) {
    capacity = node_count + node_count / 2u;
    for (gfx::BufferResource& table : tables) {
      if (table.buffer.valid()) frames.defer_destroy(table);
      table = gfx::BufferResource{};
    }
    for (u8& w : whole)
      w = 1;
  }
  if (tables.size() < sets) tables.resize(sets);
  for (u32 s = 0; s < sets; ++s) {
    if (tables[s].buffer.valid()) continue;
    if (!gfx::create_buffer(device, u64{capacity} * sizeof(gfx::CullNodeDesc), k_storage, false,
                            tables[s], error)) {
      return false;
    }
    whole[s] = 1;
  }
  // What this frame measures in its set: every node of a set to be measured whole — a new one,
  // one carried over, every set after the meshes' spheres were measured again — else the ids the
  // layout changed since the set was last current and the nodes of the slots it writes now.
  if (bounds_due) {
    for (u8& w : whole)
      w = 1;
  }
  if (table_update.carry_instances.buffer.valid()) whole[set] = 1;
  ids.clear();
  Vector<u32>& changed = layout.changed(set);
  if (whole[set] != 0) {
    for (u32 id = 0; id < node_count; ++id)
      ids.push_back(id);
    whole[set] = 0;
  } else {
    ids.append(std::span<const u32>(changed.data(), changed.size()));
    if (dynamic && table_update.write) {
      list_written(
          layout,
          std::span<const gfx::BufferCopy>(table_update.copies.data(), table_update.copies.size()),
          ids);
    }
    list_written(
        layout,
        std::span<const gfx::BufferCopy>(records.instances.data(), records.instances.size()), ids);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }
  changed.clear();
  list_count = ids.size();
  measure_due = list_count > 0;
  // The list, in this frame slot's own host-visible buffer (its last frame is done).
  if (lists.size() <= frame_slot) lists.resize(frame_slot + 1);
  gfx::BufferResource& list = lists[frame_slot];
  const u64 list_bytes = 16u + u64{std::max(list_count, 1u)} * sizeof(gfx::CullNodeListEntry);
  if (list.size < list_bytes) {
    gfx::destroy_buffer(device, list);
    list = gfx::BufferResource{};
    if (!gfx::create_buffer(device, list_bytes + list_bytes / 2u + 4096u, k_storage, true, list,
                            error)) {
      return false;
    }
  }
  auto* base = static_cast<u8*>(list.mapped);
  const u32 header[4] = {list_count, 0u, 0u, 0u};
  std::memcpy(base, header, sizeof(header));
  auto* entries = reinterpret_cast<gfx::CullNodeListEntry*>(base + 16);
  const std::span<const NodeLayout::Node> nodes = layout.nodes();
  for (u32 k = 0; k < list_count; ++k) {
    const NodeLayout::Node node = nodes[ids[k]];
    entries[k] = gfx::CullNodeListEntry{ids[k], node.first_slot, node.count, 0u};
  }
  // A run's surviving ids and their dispatch.
  const u64 item_bytes = u64{runs} * run_stride();
  if (!items.buffer.valid() || items.size < item_bytes) {
    if (items.buffer.valid()) frames.defer_destroy(items);
    items = gfx::BufferResource{};
    if (!gfx::create_buffer(device, item_bytes, k_storage | gfx::BufferUsage::TransferDst, false,
                            items, error)) {
      return false;
    }
  }
  if (!args.buffer.valid() || args.size < u64{runs} * 16u) {
    if (args.buffer.valid()) frames.defer_destroy(args);
    args = gfx::BufferResource{};
    if (!gfx::create_buffer(device, u64{runs} * 16u * 2u, k_storage | gfx::BufferUsage::Indirect,
                            false, args, error)) {
      return false;
    }
  }
  guard_on = guard_wanted;
  if (guard_on && !guard.buffer.valid()) {
    if (!gfx::create_buffer(device, 16u, k_storage, true, guard, error)) return false;
    std::memset(guard.mapped, 0, 16u);
  }
  guard_due = guard_on;
  active = true;
  return true;
}

void CullNodes::fill(gfx::CullParams& params, u32 run) const noexcept {
  params.node_count = active ? node_count : 0u;
  params.nodes = active ? tables[set].address : 0u;
  params.node_items = active ? run_items(run) : 0u;
  params.node_list = active ? lists[slot].address : 0u;
  params.node_guard = active && guard_on ? guard.address : 0u;
}

void CullNodes::import_and_reset(gfx::RenderGraph& graph) {
  if (!active) return;
  // Written on the frames that measure, while a frame in flight may read the same set's table (one
  // that does not flip reads the set its predecessor did): imported as last read everywhere, so the
  // measure waits for those reads, as the meshes' spheres do.
  rg_table = graph.import_buffer("cull nodes", tables[set], gfx::PipelineStage::AllCommands,
                                 gfx::MemoryAccess::MemoryRead);
  rg_list = graph.import_buffer("cull node list", lists[slot]);
  rg_items = graph.import_buffer("cull node items", items);
  rg_args = graph.import_buffer("cull node args", args);
  if (guard_on) {
    rg_guard = graph.import_buffer("cull node guard", guard, gfx::PipelineStage::AllCommands,
                                   gfx::MemoryAccess::MemoryRead);
  }
  graph.add_pass(
      "cull node items reset", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) { b.write(rg_items, gfx::Access::TransferWrite); },
      [this](gfx::CommandList cb, gfx::RenderGraph&) {
        for (u32 r = 0; r < runs; ++r)
          cb.fill_buffer(items.buffer, run_items(r) - items.address, sizeof(u32), 0u);
      });
}

bool CullNodes::read_guard(u32& failures, u32& checked) const noexcept {
  failures = checked = 0;
  if (!guard.buffer.valid() || guard.mapped == nullptr) return false;
  u32 words[2];
  std::memcpy(words, guard.mapped, sizeof(words));
  failures = words[0];
  checked = words[1];
  return true;
}

void CullNodes::destroy(const gfx::Device& device) noexcept {
  for (gfx::BufferResource& table : tables)
    gfx::destroy_buffer(device, table);
  for (gfx::BufferResource& list : lists)
    gfx::destroy_buffer(device, list);
  gfx::destroy_buffer(device, items);
  gfx::destroy_buffer(device, args);
  gfx::destroy_buffer(device, guard);
  tables.clear();
  lists.clear();
  whole.clear();
  capacity = 0;
  active = false;
  cut = false;
  measure_due = false;
}

}  // namespace engine::renderer
