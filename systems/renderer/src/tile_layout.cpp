#include <core/base/assert.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/tile_layout.h>

#include <algorithm>

namespace engine::renderer {

namespace {

tunables::Int tile_compact_pct{
    "renderer.tiles.compact_pct", k_default_compact_pct, 1, 100,
    "Share of a streamed world's cull dispatch, in percent, its holes — "
    "tiles' slack and freed blocks — may take before a change lays every "
    "tile out again"};

tunables::Int tile_move_slots{
    "renderer.tiles.move_slots", k_default_move_slots, 0, 1 << 24,
    "Most instance slots one change of a streamed world moves from the end "
    "of its table into free runs, once free slots pass half the compaction "
    "share; 0 moves none and lays every tile out again past the share"};

}  // namespace

u32 tile_compact_pct_tunable() noexcept { return static_cast<u32>(tile_compact_pct.get()); }
u32 tile_move_slots_tunable() noexcept { return static_cast<u32>(tile_move_slots.get()); }

u32 block_capacity(u32 count, u32 granularity) noexcept {
  if (count == 0) return 0;
  const u64 g = granularity > 0 ? granularity : 1;
  u64 m = (u64{count} + g - 1) / g;
  if (m > 8) {
    // Eight steps an octave above eight granules: the octave's base and an eighth of it at a time.
    u64 base = 8;
    while (base * 2 < m)
      base *= 2;
    const u64 step = base / 8;
    m = (m + step - 1) / step * step;
  }
  const u64 capacity = m * g;
  return capacity > 0xffffffffull ? 0xffffffffu : static_cast<u32>(capacity);
}

void TileLayout::reset(u32 static_instances, u32 static_pairs) noexcept {
  static_instances_ = static_instances;
  static_pairs_ = static_pairs;
  inst_end_ = static_instances;
  pair_end_ = static_pairs;
  live_instances_ = 0;
  live_pairs_ = 0;
  live_.clear();
  free_.clear();
  by_key_.clear();
  move_slots_ = tile_move_slots_tunable();
}

const TileBlock* TileLayout::find(u64 key) const noexcept {
  const u32* index = by_key_.find_value(key);
  return index != nullptr ? &live_[*index] : nullptr;
}

const TileBlock* TileLayout::block_of_slot(u32 slot) const noexcept {
  for (const TileBlock& block : live_) {
    if (slot >= block.inst_begin && slot < block.inst_end()) return &block;
  }
  return nullptr;
}

void TileLayout::index() noexcept {
  by_key_.clear();
  for (u32 i = 0; i < live_.size(); ++i)
    by_key_.insert_or_assign(live_[i].key, i);
}

// A block given back: its slots become null instances (a live one's slots would otherwise still
// name its instances, and a pair of its range would find them), and its runs join the free list,
// merged with a free neighbour on either side. Neighbours in one space are neighbours in the other,
// because blocks and free runs tile both spaces in the same order.
void TileLayout::free_block(const TileBlock& block, Vector<SlotWrite>& writes) {
  if (block.inst_cap > 0) {
    writes.push_back(SlotWrite{block.inst_begin, block.inst_cap, block.pair_begin, ~u32{0}, 0});
  }
  FreeRun run{block.inst_begin, block.inst_cap, block.pair_begin, block.pair_cap};
  // By slot: the same order as by pair, and still strict where a free run holds no pairs (T137).
  u32 at = 0;
  while (at < free_.size() && free_[at].inst_begin < run.inst_begin)
    ++at;
  free_.insert(at, run);
  // With the one after it, then with the one before.
  if (at + 1 < free_.size() &&
      free_[at].pair_begin + free_[at].pair_cap == free_[at + 1].pair_begin &&
      free_[at].inst_begin + free_[at].inst_cap == free_[at + 1].inst_begin) {
    free_[at].inst_cap += free_[at + 1].inst_cap;
    free_[at].pair_cap += free_[at + 1].pair_cap;
    free_.erase_at(at + 1);
  }
  if (at > 0 && free_[at - 1].pair_begin + free_[at - 1].pair_cap == free_[at].pair_begin &&
      free_[at - 1].inst_begin + free_[at - 1].inst_cap == free_[at].inst_begin) {
    free_[at - 1].inst_cap += free_[at].inst_cap;
    free_[at - 1].pair_cap += free_[at].pair_cap;
    free_.erase_at(at);
  }
}

// A free run at the end of both spaces is not a hole worth dispatching: the ends come back to where
// the last block ends. Free runs are merged, so there is at most one.
void TileLayout::trim() noexcept {
  while (!free_.empty()) {
    const FreeRun& last = free_.back();
    if (last.pair_begin + last.pair_cap != pair_end_ ||
        last.inst_begin + last.inst_cap != inst_end_)
      break;
    pair_end_ = last.pair_begin;
    inst_end_ = last.inst_begin;
    free_.pop_back();
  }
}

// A new block for `request`: the smallest free run both of whose runs hold it — by pairs, then by
// slots — split when what is left is at least a granule of each, or a new block at the end, of the
// request's class. The live slots and the slack are written whole, and so is what a split left,
// because its null slots still carry the `first_pair` of the free run they were part of and the
// block now in front of them has larger ones.
//
// **A block that fits no free run by both may take one by its slots** (T137 at the cap; T154 under
// it too, below): the smallest free run that holds its *slots*, split by slots alone, and the pairs
// are numbered in behind it (`number_again`), which costs no slot. With no such run it goes to the
// end: with exactly its pairs, numbered again, when its class would pass `max_pairs`; at its class
// otherwise. Without this a layout whose free runs hold no pairs — every one after a numbering —
// would only ever append, and the table would grow by every tile that came until free slots past
// the share laid everything out again.
bool TileLayout::place(const Request& request, u32 source, u32 max_pairs, TileBlock& out,
                       Vector<SlotWrite>& writes, TileLayoutChange& change, bool& number_again) {
  const u32 want_inst = block_capacity(request.instances, k_block_instance_granularity);
  const u32 want_pairs = block_capacity(request.pairs, k_block_pair_granularity);
  u32 best = ~u32{0};
  for (u32 i = 0; i < free_.size(); ++i) {
    const FreeRun& run = free_[i];
    if (run.inst_cap < request.instances || run.pair_cap < request.pairs) continue;
    if (best == ~u32{0} || run.pair_cap < free_[best].pair_cap ||
        (run.pair_cap == free_[best].pair_cap && run.inst_cap < free_[best].inst_cap)) {
      best = i;
    }
  }
  out = TileBlock{};
  out.key = request.key;
  out.instances = request.instances;
  out.pairs = request.pairs;
  out.tail_first = request.first;
  out.version = request.version;
  // By its slots when the pairs are numbered again anyway (at the cap, or a block of this change
  // already took a run by its slots), or when the free slots pass half the share of the table:
  // short of that it goes to the end, which moves no pair. A numbering moves every pair after the
  // run, and each table set then writes those pairs again on the GPU, so a tile that arrives
  // beside free slots of no consequence does not pay that (T154: the engine's streamed forest,
  // taking every such run, numbered its pairs on 592 of 900 frames).
  const u64 open = free_slots();
  const bool at_cap = u64{pair_end_} + want_pairs > max_pairs;
  const bool by_slots =
      number_again || at_cap ||
      (open >= k_compact_min_free_slots && open * 200 > u64{compact_pct_} * inst_end_);
  u32 slots = ~u32{0};
  if (best == ~u32{0} && by_slots) {
    for (u32 i = 0; i < free_.size(); ++i) {
      if (free_[i].inst_cap < request.instances) continue;
      if (slots == ~u32{0} || free_[i].inst_cap < free_[slots].inst_cap) slots = i;
    }
  }
  if (best == ~u32{0} && (slots != ~u32{0} || at_cap)) {
    number_again = true;
    if (slots == ~u32{0}) {
      out.inst_begin = inst_end_;
      out.pair_begin = pair_end_;
      out.inst_cap = want_inst;
      out.pair_cap = request.pairs;
      inst_end_ += want_inst;
      pair_end_ += request.pairs;
      writes.push_back(
          SlotWrite{out.inst_begin, out.inst_cap, out.pair_begin, source, out.instances});
      ++change.appended;
      return true;
    }
    FreeRun& run = free_[slots];
    const u32 inst = want_inst < run.inst_cap ? want_inst : run.inst_cap;
    const bool split = run.inst_cap - inst >= k_block_instance_granularity;
    out.inst_begin = run.inst_begin;
    out.pair_begin = run.pair_begin;
    out.inst_cap = split ? inst : run.inst_cap;
    out.pair_cap = request.pairs;
    writes.push_back(
        SlotWrite{out.inst_begin, out.inst_cap, out.pair_begin, source, out.instances});
    if (split) {
      run.inst_begin += inst;
      run.inst_cap -= inst;
      run.pair_cap = 0;
      writes.push_back(SlotWrite{run.inst_begin, run.inst_cap, run.pair_begin, ~u32{0}, 0});
    } else {
      free_.erase_at(slots);
    }
    ++change.reused;
    return true;
  }
  if (best != ~u32{0}) {
    FreeRun& run = free_[best];
    const u32 inst = want_inst < run.inst_cap ? want_inst : run.inst_cap;
    const u32 pairs = want_pairs < run.pair_cap ? want_pairs : run.pair_cap;
    const bool split = run.inst_cap - inst >= k_block_instance_granularity &&
                       run.pair_cap - pairs >= k_block_pair_granularity;
    out.inst_begin = run.inst_begin;
    out.pair_begin = run.pair_begin;
    out.inst_cap = split ? inst : run.inst_cap;
    out.pair_cap = split ? pairs : run.pair_cap;
    if (split) {
      run.inst_begin += inst;
      run.inst_cap -= inst;
      run.pair_begin += pairs;
      run.pair_cap -= pairs;
    } else {
      free_.erase_at(best);
    }
    writes.push_back(
        SlotWrite{out.inst_begin, out.inst_cap, out.pair_begin, source, out.instances});
    if (split) {
      const FreeRun& left = free_[best];
      writes.push_back(SlotWrite{left.inst_begin, left.inst_cap, left.pair_begin, ~u32{0}, 0});
    }
    ++change.reused;
    return true;
  }
  out.inst_begin = inst_end_;
  out.pair_begin = pair_end_;
  out.inst_cap = want_inst;
  out.pair_cap = want_pairs;
  inst_end_ += want_inst;
  pair_end_ += want_pairs;
  writes.push_back(SlotWrite{out.inst_begin, out.inst_cap, out.pair_begin, source, out.instances});
  ++change.appended;
  return true;
}

// **The pairs numbered again where the slots stand** (T137): the blocks and the free runs in slot
// order from the load's end, a block exactly its pairs and a free run none. Blocks and free runs
// tile the slots, so walking them by slot is walking the pair space in the order the cull's search
// needs; nothing here reads a pair field, which is what lets `place` leave one stale.
//
// **What moved, as runs** (T154): a block's slots all move by its distance (its live slots keep
// their offsets from its first pair, its slack the live pairs' end), and a free run's null slots
// all take its one number, so the change is a few runs a block and not a record a slot. The writes
// are given their new numbers here — a block's by its distance, a null run's its free run's — so
// the caller writes final values and needs no pass over the table after; `shifts` gets every run
// that moved, an add for a block and a set for a free run.
//
// **A block keeps its class's pairs when they all fit** (T154): under the cap each block is given
// `block_capacity` of its pairs, as a first fill gives it, so a tile rewritten with a few more
// clusters still fits where it is; only when the classes would pass `max_pairs` is each block
// exactly its pairs (T137's, the pair space a scene loaded with this tail has).
void TileLayout::number_pairs_again(Vector<SlotWrite>& writes, Vector<PairShift>* shifts,
                                    u32 max_pairs) {
  order_.clear();
  u64 classes = static_pairs_;
  for (u32 i = 0; i < live_.size(); ++i) {
    order_.push_back(i);
    classes += block_capacity(live_[i].pairs, k_block_pair_granularity);
  }
  const bool slack = classes <= max_pairs;
  std::sort(order_.begin(), order_.end(),
            [&](u32 a, u32 b) { return live_[a].inst_begin < live_[b].inst_begin; });
  renumbered_.clear();
  u32 pair = static_pairs_;
  u32 f = 0;
  const auto free_run = [&](FreeRun& run) {
    renumbered_.push_back(
        Renumbered{run.inst_begin, run.inst_begin + run.inst_cap, run.pair_begin, pair, false});
    run.pair_begin = pair;
    run.pair_cap = 0;
  };
  for (const u32 i : order_) {
    TileBlock& block = live_[i];
    for (; f < free_.size() && free_[f].inst_begin < block.inst_begin; ++f)
      free_run(free_[f]);
    renumbered_.push_back(
        Renumbered{block.inst_begin, block.inst_end(), block.pair_begin, pair, true});
    block.pair_begin = pair;
    block.pair_cap = slack ? block_capacity(block.pairs, k_block_pair_granularity) : block.pairs;
    pair += block.pair_cap;
  }
  for (; f < free_.size(); ++f)
    free_run(free_[f]);
  pair_end_ = pair;
  trim();
  // The writes' numbers: by the run each starts in. A null write that runs on past its run into
  // another is always followed by that one's own write, which wins.
  for (SlotWrite& write : writes) {
    if (write.inst_count == 0) continue;
    u32 lo = 0;
    u32 hi = renumbered_.size();
    while (lo < hi) {
      const u32 mid = (lo + hi) / 2;
      if (renumbered_[mid].inst_end <= write.inst_begin) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo >= renumbered_.size()) continue;  // past the end: trimmed, nothing reads it
    const Renumbered& run = renumbered_[lo];
    write.pair_begin = run.block ? run.new_pair + (write.pair_begin - run.old_pair) : run.new_pair;
  }
  if (shifts == nullptr) return;
  shifts->clear();
  for (const Renumbered& run : renumbered_) {
    if (run.inst_begin >= inst_end_) break;  // trimmed
    if (run.block && run.new_pair == run.old_pair) continue;
    push_shift(*shifts, PairShift{run.inst_begin, std::min(run.inst_end, inst_end_),
                                  run.block ? run.new_pair - run.old_pair : run.new_pair,
                                  run.block ? 0u : 1u});
  }
}

// Every block of `requests` again, in their order, from the load's end, at its class. (Until T137
// it also laid them out with no slack at the cap; `number_pairs_again` does that without a write.)
void TileLayout::lay_out_again(std::span<const Request> requests, Vector<SlotWrite>& writes) {
  writes.clear();
  live_.clear();
  free_.clear();
  inst_end_ = static_instances_;
  pair_end_ = static_pairs_;
  for (u32 r = 0; r < requests.size(); ++r) {
    const Request& request = requests[r];
    if (request.instances == 0) continue;
    TileBlock block;
    block.key = request.key;
    block.instances = request.instances;
    block.pairs = request.pairs;
    block.tail_first = request.first;
    block.version = request.version;
    block.inst_begin = inst_end_;
    block.pair_begin = pair_end_;
    block.inst_cap = block_capacity(request.instances, k_block_instance_granularity);
    block.pair_cap = block_capacity(request.pairs, k_block_pair_granularity);
    inst_end_ += block.inst_cap;
    pair_end_ += block.pair_cap;
    writes.push_back(
        SlotWrite{block.inst_begin, block.inst_cap, block.pair_begin, r, block.instances});
    live_.push_back(block);
  }
}

// **The blocks at the end move into free runs before them** (T164): while the free slots pass half
// the share, the block that ends the table is given back (its slots written null) and placed by its
// slots in the smallest free run before it that holds it — `place`'s rule for a block that fits no
// run by its pairs — and what it left at the end goes back, so the table shrinks by its slots. Its
// new slots are written whole from the change's request of its key (every live block has one: a
// block no request names was freed before this), and its pairs are left for the numbering after,
// which the caller runs (true: at least one block moved). It stops at `move_slots_` slots moved, at
// a block no free run before it holds, and under half the share. Only slots move, so a numbering
// needs no slot it did not write; a block keeps its key, so its age (`GpuScene::block_stamp`) and
// its place in the requests' order stay its own.
bool TileLayout::move_last_blocks(u32 compact_pct, Vector<SlotWrite>& writes,
                                  TileLayoutChange& change) {
  u32 moved = 0;
  bool any = false;
  u64 open = free_slots();
  if (open < k_compact_min_free_slots || open * 200 <= u64{compact_pct} * inst_end_) return false;
  // The live blocks by slot, once: the last is the one that ends the table (`trim` leaves no free
  // run at the end), and each move takes the next from the back.
  order_.clear();
  for (u32 i = 0; i < live_.size(); ++i)
    order_.push_back(i);
  std::sort(order_.begin(), order_.end(),
            [&](u32 a, u32 b) { return live_[a].inst_begin < live_[b].inst_begin; });
  u32 next = order_.size();
  for (; next > 0; --next) {
    if (open < k_compact_min_free_slots || open * 200 <= u64{compact_pct} * inst_end_) break;
    // A block moved earlier in this change may now end the table instead: then it stops.
    TileBlock& block = live_[order_[next - 1]];
    if (block.inst_end() != inst_end_) break;
    if (u64{moved} + block.inst_cap > move_slots_) break;
    const u32* source = seen_.find_value(block.key);
    if (source == nullptr) break;
    u32 best = ~u32{0};
    for (u32 i = 0; i < free_.size(); ++i) {
      const FreeRun& run = free_[i];
      if (run.inst_begin >= block.inst_begin || run.inst_cap < block.instances) continue;
      if (best == ~u32{0} || run.inst_cap < free_[best].inst_cap) best = i;
    }
    if (best == ~u32{0}) break;
    // Given back first: its run merges with a free neighbour, which may be `best` itself (it is
    // before the block, so its index stands).
    const TileBlock old = block;
    free_block(old, writes);
    open += old.inst_cap;
    FreeRun& run = free_[best];
    const u32 want = block_capacity(old.instances, k_block_instance_granularity);
    const u32 inst = want < run.inst_cap ? want : run.inst_cap;
    const bool split = run.inst_cap - inst >= k_block_instance_granularity;
    block.inst_begin = run.inst_begin;
    block.pair_begin = run.pair_begin;
    block.inst_cap = split ? inst : run.inst_cap;
    block.pair_cap = block.pairs;
    writes.push_back(
        SlotWrite{block.inst_begin, block.inst_cap, block.pair_begin, *source, block.instances});
    open -= block.inst_cap;
    if (split) {
      run.inst_begin += inst;
      run.inst_cap -= inst;
      run.pair_cap = 0;
      writes.push_back(SlotWrite{run.inst_begin, run.inst_cap, run.pair_begin, ~u32{0}, 0});
    } else {
      free_.erase_at(best);
    }
    // What it left is at the end, by its slots: given back by them, with a free run it touches that
    // a split earlier in the change left with no pairs and so did not merge (the numbering after
    // sets every pair, `pair_end_` included).
    while (!free_.empty() && free_.back().inst_begin + free_.back().inst_cap == inst_end_) {
      inst_end_ = free_.back().inst_begin;
      open -= free_.back().inst_cap;
      free_.pop_back();
    }
    moved += block.inst_cap;
    ++change.moved;
    any = true;
  }
  ENGINE_ASSERT(open == free_slots(), "the free slots counted move by move are the recount's");
  return any;
}

bool TileLayout::apply(std::span<const Request> requests, u32 compact_pct, bool force_compact,
                       u32 max_pairs, Vector<SlotWrite>& writes, TileLayoutChange& change,
                       std::string* error, Vector<PairShift>* shifts) {
  writes.clear();
  if (shifts != nullptr) shifts->clear();
  change = TileLayoutChange{};
  compact_pct_ = compact_pct;
  // Everything that can refuse a change is checked before anything moves, so a refusal leaves the
  // layout exactly as it was: a key named twice, and tiles whose pairs pass the cap however they
  // are packed. Past these the change cannot fail — packed tight it is the layout a scene loaded
  // with this tail has, which the second check proved fits.
  //
  // What each request does with the block its key has now: 0 place, 1 keep, 2 rewrite.
  const u32 count = static_cast<u32>(requests.size());
  action_.clear();
  action_.resize(count, u8{0});
  old_block_.clear();
  old_block_.resize(count, ~u32{0});
  u64 live_pairs = 0;
  u64 live_instances = 0;
  seen_.clear();
  for (u32 r = 0; r < count; ++r) {
    const Request& request = requests[r];
    if (!seen_.try_emplace(request.key, r).second) {
      if (error != nullptr) {
        *error = "two blocks of one tail share the key " + std::to_string(request.key);
      }
      return false;
    }
    live_pairs += request.pairs;
    live_instances += request.instances;
    if (request.instances == 0) continue;
    const u32* index = by_key_.find_value(request.key);
    if (index == nullptr) continue;
    const TileBlock& block = live_[*index];
    old_block_[r] = *index;
    if (request.same && block.instances == request.instances) {
      action_[r] = 1;
    } else if (request.instances <= block.inst_cap && request.pairs <= block.pair_cap) {
      action_[r] = 2;
    }
  }
  if (u64{static_pairs_} + live_pairs > max_pairs) {
    if (error != nullptr) {
      *error = "the instances would take the scene past " + std::to_string(max_pairs) +
               " (instance, cluster) pairs";
    }
    return false;
  }

  // Every block no request keeps in place is given back first, so a tile that left frees room for
  // one that arrives in the same change.
  stays_.clear();
  stays_.resize(live_.size(), u8{0});
  for (u32 r = 0; r < count; ++r) {
    if (action_[r] != 0) stays_[old_block_[r]] = 1u;
  }
  for (u32 b = 0; b < live_.size(); ++b) {
    if (stays_[b] == 0u) {
      free_block(live_[b], writes);
      ++change.freed;
    }
  }
  // And a free run left at the end goes back before anything is placed, so a block that fits no
  // hole lands right behind the last one that stays rather than behind a hole of its own making.
  trim();
  // The blocks that stay, then the new ones, in the requests' order.
  next_.clear();
  for (u32 r = 0; r < count; ++r) {
    const Request& request = requests[r];
    if (request.instances == 0) continue;
    TileBlock block;
    if (action_[r] == 1) {
      block = live_[old_block_[r]];
      block.tail_first = request.first;
      block.version = request.version;
      ++change.kept;
    } else if (action_[r] == 2) {
      block = live_[old_block_[r]];
      block.instances = request.instances;
      block.pairs = request.pairs;
      block.tail_first = request.first;
      block.version = request.version;
      const u32 keep = request.keep < block.instances ? request.keep : 0u;
      writes.push_back(SlotWrite{block.inst_begin + keep, block.inst_cap - keep,
                                 block.pair_begin + (keep != 0 ? request.keep_pairs : 0u), r,
                                 block.instances - keep, keep});
      ++change.rewritten;
    } else {
      // Placed after every block that stays is known, so it cannot land on one: the free runs are
      // exactly what no staying block holds.
      continue;
    }
    next_.push_back(block);
  }
  bool number_again = false;
  for (u32 r = 0; r < count; ++r) {
    if (requests[r].instances == 0 || action_[r] != 0) continue;
    TileBlock block;
    place(requests[r], r, max_pairs, block, writes, change, number_again);
    next_.push_back(block);
  }
  live_.swap(next_);
  live_instances_ = static_cast<u32>(live_instances);
  live_pairs_ = static_cast<u32>(live_pairs);
  trim();
  // Free slots past half the share: the blocks at the end move into free runs before them, a few a
  // change (T164), and the pairs are numbered behind them.
  if (!force_compact && move_slots_ != 0 && move_last_blocks(compact_pct, writes, change))
    number_again = true;

  // **The pairs numbered again where the slots stand, and not one slot moved or made again**
  // (T137, T154): every block exactly its pairs, in slot order — the pair space a scene loaded
  // with this tail has. When a block took a run by its slots alone (it has no pairs of its own
  // yet); when the classes' slack takes the layout past the visibility id's pairs that the tiles
  // alone fit (T137: until then this laid every tile out again with no slack, 52 to 86 ms of the
  // frame's thread at the forest's 300,000 slots, 22 times along its 360 m walk; W81 tried laying
  // it out at the classes first, and at the ceiling that left less than a tile of room — 467 whole
  // rewrites along the forest's flight against tight's 60, docs/experiments/forest-load-in-
  // 2026-10-31.md); and when the holes pass the share (T154: until then that laid every tile out
  // again, 59 ms of the frame's thread and 20 ms a table set at the forest staging's 349,000 slots,
  // five times along its 2,400-frame walk).
  const u64 holes = u64{pair_end_} - static_pairs_ - live_pairs_;
  const bool too_many_holes =
      holes >= k_compact_min_holes && holes * 100 > u64{compact_pct} * pair_end_;
  if (!force_compact && (number_again || too_many_holes || pair_end_ > max_pairs)) {
    number_pairs_again(writes, shifts, max_pairs);
    change.tight = true;
  }
  // Asked, or free slots past the share once the pairs are numbered — twice the share while blocks
  // may move, since the moves close what one change cannot (T164) — everything again, in the
  // caller's order: the one whole rewrite left. Free runs hold no pairs then, so it is their slots
  // that are counted.
  const u64 open = change.tight ? u64{free_slots()} : 0;
  const u64 bar = u64{compact_pct} * (move_slots_ != 0 ? 2u : 1u);
  if (force_compact || (open >= k_compact_min_free_slots && open * 100 > bar * inst_end_)) {
    lay_out_again(requests, writes);
    if (shifts != nullptr) shifts->clear();
    change.compacted = true;
    change.tight = false;
    // Laid out at the classes, a tail near the cap can pass it: numbered where they now stand.
    if (pair_end_ > max_pairs) {
      number_pairs_again(writes, nullptr, max_pairs);
      change.tight = true;
    }
  }
  // A write past the end names slots nothing reads any more.
  for (SlotWrite& write : writes) {
    if (write.inst_begin >= inst_end_) {
      write.inst_count = 0;
    } else if (write.inst_begin + write.inst_count > inst_end_) {
      write.inst_count = inst_end_ - write.inst_begin;
    }
    if (write.live > write.inst_count) write.live = write.inst_count;
  }
  index();
  return true;
}

bool TileLayout::validate(std::string* why) const {
  auto fail = [&](const std::string& what) {
    if (why != nullptr) *why = what;
    return false;
  };
  struct Run {
    u32 inst_begin, inst_cap, pair_begin, pair_cap;
    bool live;
  };
  Vector<Run> runs;
  u64 instances = 0;
  u64 pairs = 0;
  for (const TileBlock& block : live_) {
    if (block.instances > block.inst_cap || block.pairs > block.pair_cap) {
      return fail("block " + std::to_string(block.key) + " holds more than it reserved");
    }
    if (block.instances == 0) return fail("an empty block is live");
    runs.push_back(Run{block.inst_begin, block.inst_cap, block.pair_begin, block.pair_cap, true});
    instances += block.instances;
    pairs += block.pairs;
  }
  for (const FreeRun& run : free_)
    runs.push_back(Run{run.inst_begin, run.inst_cap, run.pair_begin, run.pair_cap, false});
  // By slot, which is by pair too and still strict where a free run holds no pairs (T137).
  std::sort(runs.begin(), runs.end(),
            [](const Run& a, const Run& b) { return a.inst_begin < b.inst_begin; });
  u32 inst = static_instances_;
  u32 pair = static_pairs_;
  for (u32 i = 0; i < runs.size(); ++i) {
    if (runs[i].inst_begin != inst || runs[i].pair_begin != pair) {
      return fail("the runs do not tile the spaces at pair " + std::to_string(pair));
    }
    if (!runs[i].live && i + 1 < runs.size() && !runs[i + 1].live) {
      return fail("two free runs touch at pair " + std::to_string(runs[i + 1].pair_begin));
    }
    inst += runs[i].inst_cap;
    pair += runs[i].pair_cap;
  }
  if (inst != inst_end_ || pair != pair_end_) return fail("the ends are not where the runs end");
  if (!runs.empty() && !runs.back().live) return fail("a free run is at the end");
  if (instances != live_instances_ || pairs != live_pairs_) return fail("the live counts drifted");
  if (by_key_.size() != live_.size()) return fail("the key index drifted");
  return true;
}

}  // namespace engine::renderer
