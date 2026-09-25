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

}  // namespace

u32 tile_compact_pct_tunable() noexcept { return static_cast<u32>(tile_compact_pct.get()); }

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
  u32 at = 0;
  while (at < free_.size() && free_[at].pair_begin < run.pair_begin)
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
bool TileLayout::place(const Request& request, u32 source, TileBlock& out,
                       Vector<SlotWrite>& writes, TileLayoutChange& change) {
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

// Every block of `requests` again, in their order, from the load's end, at its class — or, `tight`,
// at exactly its instances and pairs, which is the layout a scene loaded with this tail would have.
void TileLayout::lay_out_again(std::span<const Request> requests, bool tight,
                               Vector<SlotWrite>& writes) {
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
    block.inst_begin = inst_end_;
    block.pair_begin = pair_end_;
    block.inst_cap =
        tight ? request.instances : block_capacity(request.instances, k_block_instance_granularity);
    block.pair_cap =
        tight ? request.pairs : block_capacity(request.pairs, k_block_pair_granularity);
    inst_end_ += block.inst_cap;
    pair_end_ += block.pair_cap;
    writes.push_back(
        SlotWrite{block.inst_begin, block.inst_cap, block.pair_begin, r, block.instances});
    live_.push_back(block);
  }
}

bool TileLayout::apply(std::span<const Request> requests, u32 compact_pct, bool force_compact,
                       u32 max_pairs, Vector<SlotWrite>& writes, TileLayoutChange& change,
                       std::string* error) {
  writes.clear();
  change = TileLayoutChange{};
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
      ++change.kept;
    } else if (action_[r] == 2) {
      block = live_[old_block_[r]];
      block.instances = request.instances;
      block.pairs = request.pairs;
      block.tail_first = request.first;
      writes.push_back(
          SlotWrite{block.inst_begin, block.inst_cap, block.pair_begin, r, block.instances});
      ++change.rewritten;
    } else {
      // Placed after every block that stays is known, so it cannot land on one: the free runs are
      // exactly what no staying block holds.
      continue;
    }
    next_.push_back(block);
  }
  for (u32 r = 0; r < count; ++r) {
    if (requests[r].instances == 0 || action_[r] != 0) continue;
    TileBlock block;
    place(requests[r], r, block, writes, change);
    next_.push_back(block);
  }
  live_.swap(next_);
  trim();
  live_instances_ = static_cast<u32>(live_instances);
  live_pairs_ = static_cast<u32>(live_pairs);

  // Too many holes, or asked: everything again, in the caller's order.
  const u64 holes = u64{pair_end_} - static_pairs_ - live_pairs_;
  if (force_compact ||
      (holes >= k_compact_min_holes && holes * 100 > u64{compact_pct} * pair_end_)) {
    lay_out_again(requests, false, writes);
    change.compacted = true;
  }
  // The classes' slack can take a layout past the visibility id's pairs that the tiles alone fit;
  // packed with none it is the layout a scene loaded with this tail has.
  if (pair_end_ > max_pairs) {
    lay_out_again(requests, true, writes);
    change.compacted = true;
    change.tight = true;
  }  // A write past the end names slots nothing reads any more.
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
  std::sort(runs.begin(), runs.end(),
            [](const Run& a, const Run& b) { return a.pair_begin < b.pair_begin; });
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
