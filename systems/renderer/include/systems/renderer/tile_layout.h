#pragma once

// Where a streamed world's tiles live in the instance table and in the pair space
// (docs/subsystems/renderer.md, "Instances that come and go"). No GPU: `GpuScene` asks this how a
// change to its tail of instances lays out, writes the instance slots it names, and the frame
// uploads them; the tests ask it the same questions with no device at all.
//
// **A tile is a block, and a block does not move.** Each tile's instances occupy a run of instance
// slots and a **reserved** run of pairs, both rounded up to a size class (`block_capacity`), in the
// same order in both spaces — so the instances' `first_pair`s stay a sorted prefix sum and the cull
// pass's binary search over them is the formula it always was. Another tile arriving or leaving
// touches neither run, which is what makes a change cost the tile and not the tail: until this,
// the tail was one prefix sum after the load's instances, and every change rewrote every tile's
// instances and pairs behind the first one that moved (E35: 3.2 ms a change for a kit of boxes and
// 15–17 ms for E33's ashlar kit, whose tail is eight million pairs).
//
// What a block does not use is a **hole**: the slack its class leaves past its own pairs, and a
// freed block. The cull pass dispatches one thread per pair of the whole space, holes included,
// and rejects a hole's thread right after the binary search (the instance it lands on has no
// cluster there), so a hole costs a search and nothing else. Freed blocks are reused by the
// smallest free block both of whose runs hold the tile, split when what is left is worth keeping,
// and merged with a free neighbour; a free block at the end is given back. When the holes pass a
// declared share of the dispatch the next change **compacts**: every tile is laid out again in the
// caller's order, as a first fill would lay it, and the whole table is written once.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>

#include <span>
#include <string>

namespace engine::renderer {

// A caller's name for a run of the tail it hands over: a streamed world's tile. The tail's
// instances `first .. first + count` are the block's, in its order. A block that comes back under
// the same key with the same instances is the same block and costs nothing; under the same key with
// different ones it is rewritten, in place when it still fits.
struct DynamicBlock {
  u64 key = 0;
  u32 first = 0;
  u32 count = 0;
};

// The granularities a block's reserved runs are rounded to: sixteen pairs, four instance slots.
// Sixteen is small beside a tile of E33's ashlar kit (tens of thousands of pairs) and not much
// beside a tile of one-cluster boxes, whose sections are a few dozen pairs.
inline constexpr u32 k_block_pair_granularity = 16;
inline constexpr u32 k_block_instance_granularity = 4;
// The share of the cull dispatch the holes may take before a change compacts, in percent, and the
// fewest holes worth a compaction at all (a small scene's few hundred slack pairs cost nothing).
inline constexpr u32 k_default_compact_pct = 25;
inline constexpr u32 k_compact_min_holes = 4096;
// `renderer.tiles.compact_pct`: the share above, read once when a dynamic scene is created.
u32 tile_compact_pct_tunable() noexcept;

// The run a block of `count` is given: `granularity` times the smallest of 1..8 at or above it, and
// past eight granules the smallest of eight steps an octave (9, 10, ..., 16, 18, 20, ..., 32, 36,
// ...), so at most an eighth of a large block is slack and blocks come in few enough sizes that a
// freed one fits the next tile of its kind. Zero for zero; clamped to 2^32 - 1.
u32 block_capacity(u32 count, u32 granularity) noexcept;

// A tile's block: its slots and its pairs, reserved, and how much of each its instances use.
struct TileBlock {
  u64 key = 0;
  u32 inst_begin = 0;
  u32 inst_cap = 0;
  u32 pair_begin = 0;
  u32 pair_cap = 0;
  u32 instances = 0;   // live instances, in the first slots
  u32 pairs = 0;       // their pairs, from `pair_begin`
  u32 tail_first = 0;  // where its instances were in the tail last handed over
  u32 inst_end() const noexcept { return inst_begin + inst_cap; }
  u32 pair_end() const noexcept { return pair_begin + pair_cap; }
};

// A run of slots a change wrote, in the order it wrote them (a later write of a slot wins): the
// first `live` slots are request `source`'s instances, placed from `pair_begin` on, and the rest
// are null instances whose `first_pair` is where the live ones' pairs end — so the slots stay
// sorted by `first_pair` and every pair past the live ones lands on a null instance. `source` is ~0
// for a run of nulls alone (a freed block, what a split left).
struct SlotWrite {
  u32 inst_begin = 0;
  u32 inst_count = 0;
  u32 pair_begin = 0;
  u32 source = ~u32{0};
  u32 live = 0;
};

// What one change did, block by block.
struct TileLayoutChange {
  u32 kept = 0;       // the same key with the same instances: nothing written
  u32 rewritten = 0;  // the same key with other instances that still fit its block
  u32 reused = 0;     // placed in a free block
  u32 appended = 0;   // placed at the end
  u32 freed = 0;      // let go: gone from the tail, or moved to a block it fits
  bool compacted = false;
  bool tight = false;  // compacted with no slack at all, because the classes would pass the cap
};

class TileLayout {
 public:
  // One request per block of the new tail, in the tail's order. `same` says the caller found the
  // live block of this key holding exactly these instances (`find`), which the layout cannot see.
  struct Request {
    u64 key = 0;
    u32 first = 0;
    u32 instances = 0;
    u32 pairs = 0;
    bool same = false;
  };

  // Empty, behind the load's own instances and pairs, which never move.
  void reset(u32 static_instances, u32 static_pairs) noexcept;

  // The live block of `key`, or null.
  const TileBlock* find(u64 key) const noexcept;

  // Lays out the change from what is live now to `requests`: a request whose block is `same` keeps
  // it, one whose instances still fit its block is rewritten there, and every other one is placed —
  // in the smallest free block both of whose runs hold it, or at the end — after every live block
  // no request keeps has been freed. Then, when the holes pass `compact_pct` percent of the pairs
  // the cull dispatches (and `k_compact_min_holes`), or `force_compact`, everything is laid out
  // again in the requests' order. `writes` are the slots to write, in order. False, with `error`
  // and nothing changed, when the layout would pass `max_pairs` even packed with no slack, or a key
  // repeats.
  bool apply(std::span<const Request> requests, u32 compact_pct, bool force_compact, u32 max_pairs,
             Vector<SlotWrite>& writes, TileLayoutChange& change, std::string* error = nullptr);

  // The instance slots and pairs the layout covers, the load's included: what the binary search
  // runs over and what the cull dispatches.
  u32 instance_end() const noexcept { return inst_end_; }
  u32 pair_end() const noexcept { return pair_end_; }
  u32 static_instances() const noexcept { return static_instances_; }
  u32 static_pairs() const noexcept { return static_pairs_; }
  u32 live_instances() const noexcept { return live_instances_; }
  u32 live_pairs() const noexcept { return live_pairs_; }
  // Pairs the cull dispatches that no instance has: slack and free blocks.
  u32 hole_pairs() const noexcept { return pair_end_ - static_pairs_ - live_pairs_; }
  // Of those, the pairs of free blocks; the rest is the blocks' own slack.
  u32 free_pairs() const noexcept {
    u64 pairs = 0;
    for (const FreeRun& run : free_)
      pairs += run.pair_cap;
    return static_cast<u32>(pairs);
  }
  u32 block_count() const noexcept { return live_.size(); }
  u32 free_count() const noexcept { return free_.size(); }
  std::span<const TileBlock> blocks() const noexcept { return {live_.data(), live_.size()}; }
  // The block holding slot `slot`, or null for a slot of the load, of a free block, or past the
  // end.
  const TileBlock* block_of_slot(u32 slot) const noexcept;

  // The layout's own invariants, for the tests: the blocks and the free runs tile both spaces from
  // the load's end in one order, no two free runs touch, none is at the end, and every block holds
  // what it says. False with the first one broken.
  bool validate(std::string* why = nullptr) const;

 private:
  struct FreeRun {
    u32 inst_begin = 0;
    u32 inst_cap = 0;
    u32 pair_begin = 0;
    u32 pair_cap = 0;
  };
  void free_block(const TileBlock& block, Vector<SlotWrite>& writes);
  bool place(const Request& request, u32 source, TileBlock& out, Vector<SlotWrite>& writes,
             TileLayoutChange& change);
  void trim() noexcept;
  void lay_out_again(std::span<const Request> requests, bool tight, Vector<SlotWrite>& writes);
  void index() noexcept;

  u32 static_instances_ = 0;
  u32 static_pairs_ = 0;
  u32 inst_end_ = 0;
  u32 pair_end_ = 0;
  u32 live_instances_ = 0;
  u32 live_pairs_ = 0;
  Vector<TileBlock> live_;    // in the order the last change's requests named them
  Vector<FreeRun> free_;      // sorted by position
  HashMap<u64, u32> by_key_;  // key -> index into live_
  // A change's scratch, kept so that a change allocates only when it outgrows the last one.
  Vector<u8> action_;
  Vector<u32> old_block_;
  Vector<u8> stays_;
  Vector<TileBlock> next_;
  HashMap<u64, u32> seen_;
};

}  // namespace engine::renderer
