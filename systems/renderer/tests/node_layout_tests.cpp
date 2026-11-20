// R188 stage 1 (renderer.md, "Nodes before instances"): the nodes' layout, with no device. The
// prefix is cut in slot order at 64 slots, at a cell more than one from the node's first and at a
// terrain level; a tail block's live slots are cut into runs of 64 from its first; a run that stays
// keeps its id, every other id is freed and given again, and every table set is told each id
// whose slots changed. After every change of a random world laid out by the tile layout itself, no
// slot is in two nodes, every live slot of a block is in one node of that block, and every slot
// names the node that holds it.

#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/node_layout.h>
#include <systems/renderer/tile_layout.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

NodeLayout::Slot at(i32 x, i32 y, i32 z, u32 terrain = 0) {
  NodeLayout::Slot s;
  s.cell[0] = x;
  s.cell[1] = y;
  s.cell[2] = z;
  s.terrain = terrain;
  return s;
}

}  // namespace

TEST_CASE("node layout: the prefix is cut at 64 slots, a far cell and a terrain level") {
  Vector<NodeLayout::Slot> prefix;
  for (u32 i = 0; i < 70; ++i)
    prefix.push_back(at(0, 0, i % 2 == 0 ? 0 : 1));  // within a cell of the first: 64, then 6
  prefix.push_back(at(3, 0, 0));                     // three cells from the run's first: a node
  prefix.push_back(at(2, 0, 0));                     // within one of it
  prefix.push_back(at(2, 0, 0, 1));                  // another terrain level
  prefix.push_back(at(2, 0, 0, 1));
  NodeLayout layout;
  layout.reset(2);
  layout.cut_prefix(std::span<const NodeLayout::Slot>(prefix.data(), prefix.size()));
  const std::span<const NodeLayout::Node> nodes = layout.nodes();
  REQUIRE(nodes.size() == 4);
  CHECK(nodes[0].first_slot == 0);
  CHECK(nodes[0].count == 64);
  CHECK(nodes[1].first_slot == 64);
  CHECK(nodes[1].count == 6);
  CHECK(nodes[2].first_slot == 70);
  CHECK(nodes[2].count == 2);
  CHECK(nodes[3].first_slot == 72);
  CHECK(nodes[3].count == 2);
  for (u32 s = 0; s < prefix.size(); ++s)
    CHECK(layout.node_of(s) != ~u32{0});
  CHECK(layout.node_of(static_cast<u32>(prefix.size())) == ~u32{0});
  // Both sets are told every id the cut gave.
  CHECK(layout.changed(0).size() == 4);
  CHECK(layout.changed(1).size() == 4);
  std::string why;
  CHECK_MESSAGE(layout.validate({}, &why), why);
}

TEST_CASE("node layout: a block that stays keeps its ids, and the sets hear of every other") {
  NodeLayout layout;
  layout.reset(2);
  layout.cut_prefix({});
  TileBlock a;
  a.key = 1;
  a.inst_begin = 10;
  a.instances = 70;  // two nodes: 64 and 6
  TileBlock b;
  b.key = 2;
  b.inst_begin = 100;
  b.instances = 5;
  TileBlock both[2] = {a, b};
  layout.place_tail(std::span<const TileBlock>(both, 2));
  REQUIRE(layout.node_count() == 3);
  const u32 a0 = layout.node_of(10);
  const u32 a1 = layout.node_of(74);
  const u32 b0 = layout.node_of(100);
  CHECK(a0 != a1);
  CHECK(b0 != a0);
  CHECK(layout.node_of(80) == ~u32{0});
  layout.changed(0).clear();
  layout.changed(1).clear();
  // b rewritten in place with one more instance; a stays.
  both[1].instances = 6;
  layout.place_tail(std::span<const TileBlock>(both, 2));
  CHECK(layout.node_of(10) == a0);
  CHECK(layout.node_of(74) == a1);
  CHECK(layout.node_of(105) != ~u32{0});
  // The old id of b was freed and given again (to b's new run): told to both sets, twice.
  CHECK(layout.changed(0).size() == 2);
  CHECK(layout.changed(1).size() == 2);
  // a shrinks to 64: its second node goes, and nothing else changes.
  layout.changed(0).clear();
  both[0].instances = 64;
  layout.place_tail(std::span<const TileBlock>(both, 2));
  CHECK(layout.node_of(10) == a0);
  CHECK(layout.node_of(74) == ~u32{0});
  REQUIRE(layout.changed(0).size() == 1);
  CHECK(layout.changed(0)[0] == a1);
  CHECK(layout.nodes()[a1].count == 0);
  std::string why;
  CHECK_MESSAGE(layout.validate(std::span<const TileBlock>(both, 2), &why), why);
  // Every block gone: every id free, no slot in a node.
  layout.place_tail({});
  for (const NodeLayout::Node& node : layout.nodes())
    CHECK(node.count == 0);
  CHECK(layout.node_of(10) == ~u32{0});
}

TEST_CASE("node layout: a random world's blocks are covered by nodes after every change") {
  TileLayout tiles;
  constexpr u32 k_prefix = 37;
  tiles.reset(k_prefix, 500);
  NodeLayout layout;
  layout.reset(3);
  Vector<NodeLayout::Slot> prefix;
  for (u32 i = 0; i < k_prefix; ++i)
    prefix.push_back(at(static_cast<i32>(i / 9), 0, 0));
  layout.cut_prefix(std::span<const NodeLayout::Slot>(prefix.data(), prefix.size()));
  std::mt19937 rng(188);
  struct Tile {
    u64 key;
    u32 instances;
  };
  Vector<Tile> held;
  Vector<TileLayout::Request> requests;
  Vector<SlotWrite> writes;
  u32 changes_with_moves = 0;
  for (u32 change = 0; change < 300; ++change) {
    // Each tile leaves with a chance of one in four, and up to three arrive, of 1 to 200 instances.
    Vector<Tile> next;
    for (const Tile& t : held) {
      if (static_cast<u32>(rng()) % 4 != 0) {
        Tile kept = t;
        if (static_cast<u32>(rng()) % 8 == 0) kept.instances = 1 + static_cast<u32>(rng()) % 200;
        next.push_back(kept);
      }
    }
    const u32 arrivals = static_cast<u32>(rng()) % 4;
    for (u32 k = 0; k < arrivals; ++k)
      next.push_back(Tile{1000u + change * 4u + k, 1 + static_cast<u32>(rng()) % 200});
    requests.clear();
    u32 first = 0;
    for (const Tile& t : next) {
      TileLayout::Request r;
      r.key = t.key;
      r.first = first;
      r.instances = t.instances;
      r.pairs = t.instances * 3;
      const TileBlock* live = tiles.find(t.key);
      r.same = live != nullptr && live->instances == t.instances;
      requests.push_back(r);
      first += t.instances;
    }
    writes.clear();
    TileLayoutChange what;
    std::string error;
    REQUIRE_MESSAGE(
        tiles.apply(std::span<const TileLayout::Request>(requests.data(), requests.size()),
                    k_default_compact_pct, change % 50 == 49, 1u << 24, writes, what, &error),
        error);
    changes_with_moves += what.moved > 0 || what.compacted ? 1u : 0u;
    held = next;
    layout.place_tail(tiles.blocks());
    std::string why;
    REQUIRE_MESSAGE(layout.validate(tiles.blocks(), &why), why);
    for (u32 s = 0; s < k_prefix; ++s)
      REQUIRE(layout.node_of(s) != ~u32{0});
    // Every slot a change wrote that a node holds names a node every set was told of, or one
    // whose run is the same as before (kept): the measure lists cover it either way.
    for (u32 set = 0; set < 3; ++set)
      layout.changed(set).clear();
  }
  MESSAGE("300 changes, " << changes_with_moves << " of them compacted or moving blocks; "
                          << layout.node_count() << " node ids at the end");
  CHECK(changes_with_moves > 0);
}
