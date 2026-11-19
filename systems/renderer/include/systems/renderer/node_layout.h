#pragma once

// **Nodes before instances** (R188 stage 1; docs/subsystems/renderer.md, "Nodes before instances";
// the design, docs/experiments/instance-hierarchy.md): which instance slots each node of the cull
// holds. A node is up to `gfx::k_cull_node_slots` (64) consecutive slots of one block of the layout
// — a tile's run, a hand-off cell's, or a run of the load's fixed prefix — and never spans two
// blocks, since two blocks adjacent in slot order may be kilometres apart. The prefix is cut once,
// in slot order, a node ended at 64 slots, where the next slot's 64 m cell is more than one cell
// from the node's first on any axis, or where its terrain level differs. A tail block's live slots
// are cut into runs of 64 from its first. Everything else a node holds — its sphere, its live
// count, its caster policy, its anchor — is measured on the GPU from the slots, so this layout says
// only where a node's slots are, and which nodes each table set has to measure again.
//
// The renderer keeps it from the scene's public surface (the prefix's records and
// `TileLayout::blocks`), so neither the GPU scene nor the tile layout knows nodes exist: after a
// change the tail's nodes are compared with the blocks by their first slot and count, a node whose
// run is the same keeps its id, and every other id is freed or given out (a free list, ids reused),
// and named in every table set's list of changed ids.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>
#include <string>

namespace engine::renderer {

struct TileBlock;

class NodeLayout {
 public:
  // An id's slots; `count` 0 is a free id, which the node pass rejects.
  struct Node {
    u32 first_slot = 0;
    u32 count = 0;
  };
  // What the prefix's cut reads of a slot: its cell on the 64 m grid and its terrain level.
  struct Slot {
    i32 cell[3] = {0, 0, 0};
    u32 terrain = 0;
  };

  // Empty, for `sets` table sets, with every set to be measured whole.
  void reset(u32 sets);
  // A new number of table sets: every set's list of changed ids emptied (the caller measures them
  // whole).
  void set_sets(u32 sets);
  // The load's prefix, slots [0, prefix.size()), cut in slot order. Called once, before any tail.
  void cut_prefix(std::span<const Slot> prefix);
  // The tail's live blocks as the tile layout holds them now, in any order.
  void place_tail(std::span<const TileBlock> blocks);

  // The table's length: ids in use and free.
  u32 node_count() const noexcept { return nodes_.size(); }
  std::span<const Node> nodes() const noexcept { return {nodes_.data(), nodes_.size()}; }
  // The node holding `slot`, or ~0.
  u32 node_of(u32 slot) const noexcept {
    return slot < slot_node_.size() ? slot_node_[slot] : ~u32{0};
  }
  // The ids whose slots changed since table set `set` last took them; the caller empties it.
  Vector<u32>& changed(u32 set) noexcept { return changed_[set]; }

  // The layout's invariants, for the tests: every node at most 64 slots, every slot in at most one
  // node and mapped back to it, no tail node outside its block. False with the first one broken.
  bool validate(std::span<const TileBlock> blocks, std::string* why = nullptr) const;

 private:
  struct Run {
    u32 first_slot = 0;
    u32 count = 0;
    u32 id = ~u32{0};
  };
  u32 give(u32 first_slot, u32 count);
  void take(u32 id);
  void mark(u32 id);

  Vector<Node> nodes_;
  Vector<u32> slot_node_;
  Vector<u32> free_;
  Vector<Run> tail_;   // the tail's nodes, by first slot
  Vector<Run> fresh_;  // scratch: the next tail's
  Vector<u32> dropped_;
  Vector<Vector<u32>> changed_;
};

}  // namespace engine::renderer
