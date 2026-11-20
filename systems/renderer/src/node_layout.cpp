#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/node_layout.h>
#include <systems/renderer/tile_layout.h>

#include <algorithm>
#include <cstdlib>

namespace engine::renderer {

void NodeLayout::reset(u32 sets) {
  nodes_.clear();
  slot_node_.clear();
  free_.clear();
  tail_.clear();
  fresh_.clear();
  dropped_.clear();
  set_sets(sets);
}

void NodeLayout::set_sets(u32 sets) {
  changed_.resize(sets);
  for (Vector<u32>& list : changed_)
    list.clear();
}

void NodeLayout::mark(u32 id) {
  for (Vector<u32>& list : changed_)
    list.push_back(id);
}

u32 NodeLayout::give(u32 first_slot, u32 count) {
  u32 id;
  if (!free_.empty()) {
    id = free_[free_.size() - 1];
    free_.pop_back();
  } else {
    id = nodes_.size();
    nodes_.push_back(Node{});
  }
  nodes_[id] = Node{first_slot, count};
  if (slot_node_.size() < first_slot + count) slot_node_.resize(first_slot + count, ~u32{0});
  for (u32 s = first_slot; s < first_slot + count; ++s)
    slot_node_[s] = id;
  mark(id);
  return id;
}

void NodeLayout::take(u32 id) {
  const Node node = nodes_[id];
  for (u32 s = node.first_slot; s < node.first_slot + node.count && s < slot_node_.size(); ++s) {
    if (slot_node_[s] == id) slot_node_[s] = ~u32{0};
  }
  nodes_[id] = Node{};
  free_.push_back(id);
  mark(id);
}

void NodeLayout::cut_prefix(std::span<const Slot> prefix) {
  u32 first = 0;
  const auto far_from = [](const Slot& a, const Slot& b) {
    for (u32 k = 0; k < 3; ++k) {
      if (std::abs(i64{a.cell[k]} - i64{b.cell[k]}) > 1) return true;
    }
    return a.terrain != b.terrain;
  };
  for (u32 s = 1; s <= prefix.size(); ++s) {
    if (s == prefix.size() || s - first == gfx::k_cull_node_slots ||
        far_from(prefix[first], prefix[s])) {
      (void)give(first, s - first);
      first = s;
    }
  }
}

void NodeLayout::place_tail(std::span<const TileBlock> blocks) {
  fresh_.clear();
  for (const TileBlock& block : blocks) {
    for (u32 k = 0; k < block.instances; k += gfx::k_cull_node_slots)
      fresh_.push_back(
          Run{block.inst_begin + k, std::min(gfx::k_cull_node_slots, block.instances - k)});
  }
  std::sort(fresh_.begin(), fresh_.end(),
            [](const Run& a, const Run& b) { return a.first_slot < b.first_slot; });
  // A run the tail had keeps its id; every other id of the tail's is freed before any is given, so
  // a slot ends up named by the node that holds it now.
  dropped_.clear();
  u32 a = 0;
  u32 f = 0;
  while (a < tail_.size()) {
    while (f < fresh_.size() && fresh_[f].first_slot < tail_[a].first_slot)
      ++f;
    if (f < fresh_.size() && fresh_[f].first_slot == tail_[a].first_slot &&
        fresh_[f].count == tail_[a].count) {
      fresh_[f].id = tail_[a].id;
    } else {
      dropped_.push_back(tail_[a].id);
    }
    ++a;
  }
  for (const u32 id : dropped_)
    take(id);
  for (Run& run : fresh_) {
    if (run.id == ~u32{0}) run.id = give(run.first_slot, run.count);
  }
  tail_.swap(fresh_);
}

bool NodeLayout::validate(std::span<const TileBlock> blocks, std::string* why) const {
  const auto fail = [&](const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
  };
  Vector<u32> seen;
  seen.resize(slot_node_.size(), ~u32{0});
  for (u32 id = 0; id < nodes_.size(); ++id) {
    const Node node = nodes_[id];
    if (node.count > gfx::k_cull_node_slots)
      return fail("node " + std::to_string(id) + " holds more than 64 slots");
    for (u32 s = node.first_slot; s < node.first_slot + node.count; ++s) {
      if (s >= slot_node_.size() || slot_node_[s] != id)
        return fail("slot " + std::to_string(s) + " does not name its node " + std::to_string(id));
      if (seen[s] != ~u32{0}) return fail("slot " + std::to_string(s) + " is in two nodes");
      seen[s] = id;
    }
  }
  for (u32 s = 0; s < slot_node_.size(); ++s) {
    if (slot_node_[s] != ~u32{0} && seen[s] != slot_node_[s])
      return fail("slot " + std::to_string(s) + " names a node that does not hold it");
  }
  for (const Run& run : tail_) {
    bool inside = false;
    for (const TileBlock& block : blocks) {
      inside = inside || (run.first_slot >= block.inst_begin &&
                          run.first_slot + run.count <= block.inst_begin + block.instances);
    }
    if (!inside)
      return fail("tail node at slot " + std::to_string(run.first_slot) + " spans blocks");
  }
  for (const TileBlock& block : blocks) {
    for (u32 s = block.inst_begin; s < block.inst_begin + block.instances; ++s) {
      if (node_of(s) == ~u32{0})
        return fail("live slot " + std::to_string(s) + " of a block is in no node");
    }
  }
  return true;
}

}  // namespace engine::renderer
