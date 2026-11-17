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
// declared share of the dispatch the pairs are **numbered again where the slots stand** (T154):
// every block exactly its pairs, a free run none, no slot written, and what moved said as runs of
// slots (`PairShift`). Until T154 that laid every tile out again and wrote the whole table — 59 ms
// of the frame's thread at the forest's 349,000 slots, then 20 ms a table set to stage it. **Free
// slots past half the share close a few blocks a change** (T164): the block that ends the table
// moves into the smallest free run before it that holds it, as long as the change has moved fewer
// than `move_slots` slots, and the pairs are numbered again behind it — so a change writes the
// slots it moved and no others. Until T164 free slots past the share laid every tile out again (the
// owner's walk of the forest: ten such changes of 14,000 to 253,000 slots, 5 to 60 ms of the
// frame's thread each and 1 to 18 ms a table set). Only a caller's request, and free slots past
// twice the share that the moves have not closed, still lay everything out again.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <systems/renderer/pair_shift.h>

#include <span>
#include <string>

namespace engine::renderer {

struct SceneInstance;

// A caller's name for a run of the tail it hands over: a streamed world's tile. The tail's
// instances `first .. first + count` are the block's, in its order. A block that comes back under
// the same key with the same instances is the same block and costs nothing; under the same key with
// different ones it is rewritten, in place when it still fits.
struct DynamicBlock {
  u64 key = 0;
  u32 first = 0;
  u32 count = 0;
  // The ring the block's tile is drawn in, 0 for the inner one or for none: past the budget the ray
  // tracing chain drops whole instances farthest ring first (renderer.md, "The ray tracing chain's
  // memory").
  u32 ring = 0;
  // **The block's own instances** (W81): when set, the block's `count` instances are read from here
  // and not from the tail handed over beside the blocks, so a caller whose tiles keep their own
  // instances hands them over without writing a tail; `first` is still the block's place in the
  // tail those blocks make in order, which `GpuScene::tail_index` answers in. Null: the tail's.
  const SceneInstance* instances = nullptr;
  // **The caller's version of the key's instances** (W81): a number the caller changes whenever the
  // block's instances change, never reusing one for a key. A block that comes back under its key
  // with the version and the count the renderer holds is the same block, and its instances are not
  // read at all; 0 says nothing, and the renderer compares them as it always did.
  u64 version = 0;
  // **What changed since a version the caller handed before** (G202): when the renderer holds the
  // key at `from_version` with the same count, the block is that one with exactly the instances
  // `changed` names (ascending indices) different, so nothing is compared and the slots before the
  // first of them are kept as they are; any other held block is read as above. 0: nothing said.
  u64 from_version = 0;
  const u32* changed = nullptr;
  u32 changed_count = 0;
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
// At the cap the holes are slots, not pairs: the fewest free slots worth laying everything out
// again for (a slot is 96 bytes of the table and a step of the cull's search).
inline constexpr u32 k_compact_min_free_slots = 4096;
// `renderer.tiles.compact_pct`: the share above, read once when a dynamic scene is created.
u32 tile_compact_pct_tunable() noexcept;
// The most slots one change moves to close free slots at the end (T164): about 4 ms of the frame's
// thread to write and 1 ms a table set to stage at the forest's cost a slot, so a change that moves
// stays under a frame. 0 moves none, and free slots then lay everything out again past the share,
// as before T164. `renderer.tiles.move_slots`, read by `TileLayout::reset`.
inline constexpr u32 k_default_move_slots = 16384;
u32 tile_move_slots_tunable() noexcept;

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
  u64 version = 0;     // the caller's version of these instances (`DynamicBlock::version`), 0 none
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
  // The source's instances before the first written (G202: a block rewritten from its first
  // changed instance on), so slot `inst_begin + k` holds instance `skip + k`.
  u32 skip = 0;
};

// What one change did, block by block.
struct TileLayoutChange {
  u32 kept = 0;       // the same key with the same instances: nothing written
  u32 rewritten = 0;  // the same key with other instances that still fit its block
  u32 reused = 0;     // placed in a free block
  u32 appended = 0;   // placed at the end
  u32 freed = 0;      // let go: gone from the tail, or moved to a block it fits
  bool compacted = false;
  // **The pairs were numbered again, and no slot moved** (T137 at the cap, T154 under it too;
  // renderer.md, "The pairs are numbered again, and no slot moves"): every block now holds exactly
  // its pairs, in slot order from the load's end, and a free run holds none — because the classes'
  // slack would have passed the cap, a tile fitted no free run but by its slots, or the holes
  // passed the share. The writes carry the new numbers, and the slots that moved and were not
  // written are the runs `apply` returns. Until T137 at the cap and T154 under it this laid every
  // tile out again and wrote every slot, 52 to 86 ms of the frame's thread at the forest's 300,000.
  bool tight = false;
  // Blocks moved from the end of the table into a free run before them, to close free slots past
  // half the share (T164): each one's old slots written null and its new ones written whole.
  u32 moved = 0;
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
    u64 version = 0;  // carried into the block (`DynamicBlock::version`)
    // Rewritten in place, its first `keep` instances and their `keep_pairs` pairs are the block's
    // own already (G202, `DynamicBlock::changed`): only the slots after them are written.
    u32 keep = 0;
    u32 keep_pairs = 0;
  };

  // Empty, behind the load's own instances and pairs, which never move; the most slots a change
  // moves read from `renderer.tiles.move_slots`.
  void reset(u32 static_instances, u32 static_pairs) noexcept;
  // The most slots a change moves to close free slots at the end (T164); 0 moves none.
  void set_move_slots(u32 slots) noexcept { move_slots_ = slots; }

  // The live block of `key`, or null.
  const TileBlock* find(u64 key) const noexcept;

  // Lays out the change from what is live now to `requests`: a request whose block is `same` keeps
  // it, one whose instances still fit its block is rewritten there, and every other one is placed —
  // in the smallest free block both of whose runs hold it; failing that the smallest that holds its
  // slots, its pairs numbered in behind it; failing that at the end — after every live block no
  // request keeps has been freed. `writes` are the slots to write, in order. False, with `error`
  // and nothing changed, when the layout would pass `max_pairs` even packed with no slack, or a key
  // repeats.
  //
  // **No slot moves to close a hole** (T137 at the cap, T154 under it): when a block took a run by
  // its slots, the classes' slack would pass `max_pairs`, or the holes pass `compact_pct` percent
  // of the pairs the cull dispatches (and `k_compact_min_holes`), the pairs alone are numbered
  // again (`TileLayoutChange::tight`), the writes carry the new numbers, and `shifts` (when given)
  // gets the runs of slots whose first pair that moved — a block's by its distance, a free run's
  // set to its one number — written or not; the caller drops the ones it writes. Free slots past
  // half of `compact_pct` percent of the table (and `k_compact_min_free_slots`) move the blocks at
  // its end into free runs before them, up to `move_slots` slots a change (T164,
  // `TileLayoutChange::moved`), and the pairs are numbered again. Only `force_compact`, or free
  // slots past `compact_pct` percent of the table after a numbering — twice that while blocks may
  // move — lay everything out again in the requests' order (`TileLayoutChange::compacted`, every
  // slot written, `shifts` empty).
  bool apply(std::span<const Request> requests, u32 compact_pct, bool force_compact, u32 max_pairs,
             Vector<SlotWrite>& writes, TileLayoutChange& change, std::string* error = nullptr,
             Vector<PairShift>* shifts = nullptr);

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
  // The slots of free blocks: what holds the table open once the pairs are numbered tight.
  u32 free_slots() const noexcept {
    u64 slots = 0;
    for (const FreeRun& run : free_)
      slots += run.inst_cap;
    return static_cast<u32>(slots);
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
  bool place(const Request& request, u32 source, u32 max_pairs, TileBlock& out,
             Vector<SlotWrite>& writes, TileLayoutChange& change, bool& number_again);
  void number_pairs_again(Vector<SlotWrite>& writes, Vector<PairShift>* shifts, u32 max_pairs);
  bool move_last_blocks(u32 compact_pct, Vector<SlotWrite>& writes, TileLayoutChange& change);
  void trim() noexcept;
  void lay_out_again(std::span<const Request> requests, Vector<SlotWrite>& writes);
  void index() noexcept;

  u32 static_instances_ = 0;
  u32 static_pairs_ = 0;
  u32 inst_end_ = 0;
  u32 pair_end_ = 0;
  u32 live_instances_ = 0;
  u32 live_pairs_ = 0;
  u32 compact_pct_ = k_default_compact_pct;  // the change's share, for `place`
  u32 move_slots_ = k_default_move_slots;    // the most slots a change moves (T164)
  Vector<TileBlock> live_;                   // in the order the last change's requests named them
  Vector<FreeRun> free_;                     // sorted by position
  HashMap<u64, u32> by_key_;                 // key -> index into live_
  // A change's scratch, kept so that a change allocates only when it outgrows the last one.
  Vector<u8> action_;
  Vector<u32> old_block_;
  Vector<u8> stays_;
  Vector<TileBlock> next_;
  Vector<u32> order_;  // the live blocks by slot, for `number_pairs_again`
  // Every run of slots, by slot, with its first pair before and after a numbering.
  struct Renumbered {
    u32 inst_begin = 0;
    u32 inst_end = 0;
    u32 old_pair = 0;
    u32 new_pair = 0;
    bool block = false;
  };
  Vector<Renumbered> renumbered_;
  HashMap<u64, u32> seen_;
};

}  // namespace engine::renderer
