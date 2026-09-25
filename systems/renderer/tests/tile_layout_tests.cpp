// Where a streamed world's tiles live in the instance table and the pair space (TileLayout,
// systems/renderer/tile_layout.h; docs/subsystems/renderer.md, "Instances that come and go"). No
// device: the layout is arithmetic, and what the GPU scene does with it — write the slots it names,
// copy them into a table set, expand their pairs — is `dynamic_instances_tests.cpp`'s.
//
// The invariant that matters is the one the cull pass's binary search needs: every instance slot's
// `first_pair` at or above the one before it, every live slot where its block says, and every pair
// a block does not use landing on nothing. `apply_writes` below does what `GpuScene::write_slots`
// does to a table of (key, index, first_pair) and the randomized case checks it after every change.
#include <core/containers/vector.h>
#include <systems/renderer/tile_layout.h>

#include <doctest/doctest.h>

#include <random>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

using Request = TileLayout::Request;

Request tile(u64 key, u32 first, u32 instances, u32 pairs, bool same = false) {
  Request r;
  r.key = key;
  r.first = first;
  r.instances = instances;
  r.pairs = pairs;
  r.same = same;
  return r;
}

// One slot of a simulated instance table: which tile's instance it holds (a null one has none) and
// the first pair it would carry.
struct Slot {
  u64 key = 0;
  u32 index = 0;
  u32 first_pair = 0;
  bool live = false;
};

// `GpuScene::write_slots`, with each live instance `pairs_per[instance]` pairs long.
void apply_writes(std::span<const SlotWrite> writes, std::span<const Request> requests,
                  std::span<const Vector<u32>> pairs_of, u32 end, Vector<Slot>& table) {
  table.resize(end);
  for (const SlotWrite& write : writes) {
    u32 pair = write.pair_begin;
    for (u32 k = 0; k < write.inst_count; ++k) {
      Slot& slot = table[write.inst_begin + k];
      if (write.source != ~u32{0} && k < write.live) {
        slot.key = requests[write.source].key;
        slot.index = k;
        slot.live = true;
        slot.first_pair = pair;
        pair += pairs_of[write.source][k];
      } else {
        slot = Slot{};
        slot.first_pair = pair;
      }
    }
  }
}

}  // namespace

TEST_CASE("tile layout: block capacities are a granule, then eight steps an octave") {
  CHECK(block_capacity(0, 16) == 0);
  CHECK(block_capacity(1, 16) == 16);
  CHECK(block_capacity(16, 16) == 16);
  CHECK(block_capacity(17, 16) == 32);
  CHECK(block_capacity(128, 16) == 128);
  CHECK(block_capacity(129, 16) == 144);  // nine granules
  CHECK(block_capacity(161, 16) == 176);
  CHECK(block_capacity(256, 16) == 256);
  CHECK(block_capacity(257, 16) == 288);  // seventeen round to eighteen
  // An ashlar tile in sections and in walls (E35): sections to walls is rewritten in place, since
  // the walls fit the sections' block, and walls to sections moves.
  CHECK(block_capacity(25726, 16) == 26624);
  CHECK(block_capacity(24377, 16) == 24576);
  CHECK(block_capacity(6, 4) == 8);
  // At most an eighth of a large block is slack.
  for (u32 n = 129; n < 200000; n += 97) {
    const u32 c = block_capacity(n, k_block_pair_granularity);
    CHECK(c >= n);
    CHECK(u64{c} * 8 <= u64{n} * 9 + 8 * k_block_pair_granularity);
  }
}

TEST_CASE("tile layout: tiles are placed in order, kept, rewritten, freed, reused and trimmed") {
  TileLayout layout;
  layout.reset(2, 100);  // the load's own two instances and hundred pairs
  Vector<SlotWrite> writes;
  TileLayoutChange change;
  std::string error;

  // A first fill lays the tiles out in the order given, each at its class, from the load's end.
  Vector<Request> first = {tile(10, 0, 6, 60), tile(20, 6, 3, 30), tile(30, 9, 5, 200)};
  REQUIRE(layout.apply(std::span<const Request>(first.data(), first.size()), 25, false, 1u << 24,
                       writes, change, &error));
  CHECK(change.appended == 3);
  CHECK(layout.validate(&error));
  const TileBlock* a = layout.find(10);
  const TileBlock* b = layout.find(20);
  const TileBlock* c = layout.find(30);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c != nullptr);
  CHECK(a->inst_begin == 2);
  CHECK(a->pair_begin == 100);
  CHECK(a->pair_cap == 64);
  CHECK(b->inst_begin == a->inst_end());
  CHECK(b->pair_begin == a->pair_end());
  CHECK(c->pair_begin == b->pair_end());
  CHECK(layout.pair_end() == c->pair_end());
  CHECK(layout.hole_pairs() == layout.pair_end() - 100 - 290);
  // Copied out: a change replaces the blocks the pointers point into.
  const u32 a_end = a->pair_end();
  const u32 c_begin = c->pair_begin;

  // The same tiles again, the same: nothing moves and nothing is written.
  Vector<Request> again = {tile(10, 0, 6, 60, true), tile(20, 6, 3, 30, true),
                           tile(30, 9, 5, 200, true)};
  REQUIRE(layout.apply(std::span<const Request>(again.data(), again.size()), 25, false, 1u << 24,
                       writes, change, &error));
  CHECK(change.kept == 3);
  CHECK(writes.empty());

  // Tile 20 leaves: its block is a hole, and tile 30 keeps its pairs.
  Vector<Request> without_b = {tile(10, 0, 6, 60, true), tile(30, 6, 5, 200, true)};
  REQUIRE(layout.apply(std::span<const Request>(without_b.data(), without_b.size()), 25, false,
                       1u << 24, writes, change, &error));
  CHECK(change.freed == 1);
  CHECK(layout.free_count() == 1);
  CHECK(layout.find(30)->pair_begin == c_begin);
  CHECK(layout.find(30)->tail_first == 6);
  CHECK(layout.validate(&error));

  // A tile that fits the hole takes it; one that does not goes to the end.
  Vector<Request> with_d = {tile(10, 0, 6, 60, true), tile(30, 6, 5, 200, true),
                            tile(40, 11, 2, 20), tile(50, 13, 9, 400)};
  REQUIRE(layout.apply(std::span<const Request>(with_d.data(), with_d.size()), 25, false, 1u << 24,
                       writes, change, &error));
  CHECK(change.reused == 1);
  CHECK(change.appended == 1);
  CHECK(layout.find(40)->pair_begin == a_end);
  CHECK(layout.find(50)->pair_begin == layout.find(30)->pair_end());
  CHECK(layout.validate(&error));

  // Tile 10 changes but still fits its block: rewritten there. Tile 30 grows past its: moved.
  Vector<Request> changed = {tile(10, 0, 5, 50), tile(30, 5, 9, 900), tile(40, 14, 2, 20, true),
                             tile(50, 16, 9, 400, true)};
  REQUIRE(layout.apply(std::span<const Request>(changed.data(), changed.size()), 25, false,
                       1u << 24, writes, change, &error));
  CHECK(change.rewritten == 1);
  CHECK(change.freed == 1);
  CHECK(layout.find(10)->pair_begin == 100);
  CHECK(layout.find(30)->pair_begin != c_begin);
  CHECK(layout.validate(&error));

  // Everything but the first leaves: the end comes back to it.
  Vector<Request> only_a = {tile(10, 0, 5, 50, true)};
  REQUIRE(layout.apply(std::span<const Request>(only_a.data(), only_a.size()), 25, false, 1u << 24,
                       writes, change, &error));
  CHECK(layout.free_count() == 0);
  CHECK(layout.pair_end() == layout.find(10)->pair_end());
  CHECK(layout.validate(&error));

  // And nothing at all: back to the load.
  REQUIRE(layout.apply({}, 25, false, 1u << 24, writes, change, &error));
  CHECK(layout.pair_end() == 100);
  CHECK(layout.instance_end() == 2);
  CHECK(layout.block_count() == 0);
}

TEST_CASE("tile layout: holes past the share compact, and the cap packs tight or refuses") {
  TileLayout layout;
  layout.reset(1, 1000);
  Vector<SlotWrite> writes;
  TileLayoutChange change;
  std::string error;
  // Twenty tiles of 1,000 pairs, then every other one leaves: 10,000 pairs of holes in a dispatch
  // of about 22,000, past a quarter.
  Vector<Request> all;
  for (u32 t = 0; t < 20; ++t)
    all.push_back(tile(t + 1, t * 4, 4, 1000));
  REQUIRE(layout.apply(std::span<const Request>(all.data(), all.size()), 90, false, 1u << 24,
                       writes, change, &error));
  Vector<Request> half;
  for (u32 t = 0; t < 20; t += 2)
    half.push_back(tile(t + 1, static_cast<u32>(half.size()) * 4, 4, 1000, true));
  // Under a share of 90 percent nothing compacts...
  REQUIRE(layout.apply(std::span<const Request>(half.data(), half.size()), 90, false, 1u << 24,
                       writes, change, &error));
  CHECK_FALSE(change.compacted);
  CHECK(layout.hole_pairs() >= 9000);
  CHECK(layout.validate(&error));
  // ...and at 25 percent the same change does: every tile again, in the order given, and every
  // slot of the table written.
  REQUIRE(layout.apply(std::span<const Request>(half.data(), half.size()), 25, false, 1u << 24,
                       writes, change, &error));
  CHECK(change.compacted);
  CHECK(layout.free_count() == 0);
  CHECK(layout.hole_pairs() == 10 * (block_capacity(1000, 16) - 1000));
  CHECK(layout.find(1)->pair_begin == 1000);
  CHECK(layout.find(3)->pair_begin == layout.find(1)->pair_end());
  CHECK(writes.size() == 10);
  CHECK(layout.validate(&error));
  // Asked for, it compacts whatever the holes.
  REQUIRE(layout.apply(std::span<const Request>(half.data(), half.size()), 100, true, 1u << 24,
                       writes, change, &error));
  CHECK(change.compacted);

  // Past the cap with their classes' slack but not without it: packed tight, exactly the layout a
  // scene loaded with these tiles would have.
  TileLayout tight;
  tight.reset(0, 0);
  Vector<Request> big = {tile(1, 0, 3, 1100), tile(2, 3, 2, 1100)};
  REQUIRE(tight.apply(std::span<const Request>(big.data(), big.size()), 25, false, 2300, writes,
                      change, &error));
  CHECK(change.tight);
  CHECK(tight.pair_end() == 2200);
  CHECK(tight.find(2)->pair_begin == 1100);
  // Past it even tight: refused, and the layout is what it was.
  Vector<Request> bigger = {tile(1, 0, 3, 1100, true), tile(2, 3, 2, 1100, true),
                            tile(3, 5, 1, 300)};
  CHECK_FALSE(tight.apply(std::span<const Request>(bigger.data(), bigger.size()), 25, false, 2300,
                          writes, change, &error));
  CHECK(error.find("past 2300") != std::string::npos);
  CHECK(tight.pair_end() == 2200);
  CHECK(tight.block_count() == 2);
  CHECK(tight.validate(&error));
  // Two blocks under one key are refused the same way.
  Vector<Request> twice = {tile(7, 0, 1, 10), tile(7, 1, 1, 10)};
  CHECK_FALSE(tight.apply(std::span<const Request>(twice.data(), twice.size()), 25, false, 2300,
                          writes, change, &error));
  CHECK(error.find("share the key") != std::string::npos);
  CHECK(tight.block_count() == 2);
}

TEST_CASE("tile layout: a random world keeps the table sorted and every tile where it says") {
  std::mt19937 rng(20260925);
  TileLayout layout;
  layout.reset(3, 500);
  // Forty tiles, each at most present, of one of a few kinds, changing kind now and then.
  constexpr u32 k_tiles = 40;
  struct Tile {
    bool present = false;
    u32 kind = 0;
  };
  Tile tiles[k_tiles];
  const u32 kinds[4][2] = {{3, 1}, {12, 25}, {40, 3}, {7, 700}};  // instances, pairs each
  Vector<Slot> table(3);
  for (u32 s = 0; s < 3; ++s) {
    table[s].first_pair = s * 100;
    table[s].live = true;
  }
  Vector<SlotWrite> writes;
  TileLayoutChange change;
  std::string error;
  u32 compactions = 0;
  u32 reused = 0;
  for (u32 step = 0; step < 400; ++step) {
    for (Tile& t : tiles) {
      const u32 roll = rng() % 16;
      if (roll == 0) t.present = !t.present;
      if (roll == 1) t.kind = rng() % 4;
    }
    Vector<Request> requests;
    Vector<Vector<u32>> pairs_of;
    u32 first = 0;
    for (u32 k = 0; k < k_tiles; ++k) {
      if (!tiles[k].present) continue;
      const u32 instances = kinds[tiles[k].kind][0];
      const TileBlock* held = layout.find(k + 1);
      Vector<u32> pairs(instances, kinds[tiles[k].kind][1]);
      // The same key with the same kind is the same tile.
      const bool same = held != nullptr && held->instances == instances &&
                        held->pairs == instances * kinds[tiles[k].kind][1];
      requests.push_back(tile(k + 1, first, instances, instances * kinds[tiles[k].kind][1], same));
      pairs_of.push_back(std::move(pairs));
      first += instances;
    }
    REQUIRE(layout.apply(std::span<const Request>(requests.data(), requests.size()), 25, false,
                         1u << 24, writes, change, &error));
    compactions += change.compacted ? 1u : 0u;
    reused += change.reused;
    REQUIRE_MESSAGE(layout.validate(&error), error);
    apply_writes(std::span<const SlotWrite>(writes.data(), writes.size()),
                 std::span<const Request>(requests.data(), requests.size()),
                 std::span<const Vector<u32>>(pairs_of.data(), pairs_of.size()),
                 layout.instance_end(), table);
    // Sorted by first pair, which is all the binary search asks.
    for (u32 s = 1; s < layout.instance_end(); ++s)
      REQUIRE(table[s].first_pair >= table[s - 1].first_pair);
    // Every live tile's instances where its block says, in its order, on its pairs; and no slot
    // outside a block holds anything.
    u32 live = 0;
    for (const TileBlock& block : layout.blocks()) {
      u32 pair = block.pair_begin;
      for (u32 k = 0; k < block.instances; ++k) {
        const Slot& slot = table[block.inst_begin + k];
        REQUIRE(slot.live);
        REQUIRE(slot.key == block.key);
        REQUIRE(slot.index == k);
        REQUIRE(slot.first_pair == pair);
        pair += kinds[tiles[block.key - 1].kind][1];
      }
      live += block.instances;
    }
    u32 named = 0;
    for (u32 s = 3; s < layout.instance_end(); ++s)
      named += table[s].live ? 1u : 0u;
    REQUIRE(named == live);
    // A pair of the dispatch, found by the search: a block's own pairs lead to its own instance and
    // anything else to nothing (a null slot, or a live one the pair is past the end of).
    for (u32 pair = 500; pair < layout.pair_end(); pair += 7) {
      u32 lo = 0;
      u32 hi = layout.instance_end() - 1;
      while (lo < hi) {
        const u32 mid = (lo + hi + 1) / 2;
        if (table[mid].first_pair <= pair) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      const Slot& found = table[lo];
      const u32 length = found.live && lo >= 3 ? kinds[tiles[found.key - 1].kind][1] : 0u;
      bool owned = false;
      for (const TileBlock& block : layout.blocks())
        owned = owned || (pair >= block.pair_begin && pair < block.pair_begin + block.pairs);
      REQUIRE((found.live && pair - found.first_pair < length) == owned);
    }
  }
  MESSAGE("400 random changes: " << compactions << " compactions, " << reused
                                 << " blocks reused, last " << layout.block_count() << " blocks, "
                                 << layout.hole_pairs() << " hole pairs of " << layout.pair_end());
  CHECK(reused > 0);
}
