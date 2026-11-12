// `FootprintList` (footprint_list.h; scene_gen.md, "The shapes a tile occupies"): a polygon added
// to the list reads its own points whatever happens to the list after — the store growing under a
// thousand more, the list copied, the list moved, the caller's scratch overwritten — and a
// hand-over's `clear` empties it with the placements and the meshes.
#include <domain/scene_gen/footprint_list.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <utility>

using namespace engine;
using namespace engine::scene_gen;

namespace {

// A square of side `2 * half` about (x, z), into the caller's scratch.
Footprint square(FootprintPoint scratch[4], i64 x, i64 z, i64 half) {
  scratch[0] = {x - half, z - half};
  scratch[1] = {x - half, z + half};
  scratch[2] = {x + half, z + half};
  scratch[3] = {x + half, z - half};
  return Footprint::polygon(std::span<const FootprintPoint>(scratch, 4));
}

// Every polygon `k` of the list is the square about (1000 k, -500 k) of half 400 + k: its centre
// covered, a point 2 mm past its edge not.
void check_squares(const FootprintList& list, u32 first, u32 count) {
  for (u32 k = 0; k < count; ++k) {
    const Footprint& f = list[first + k];
    REQUIRE(f.shape == FootprintShape::polygon);
    REQUIRE(f.point_count == 4);
    const i64 x = 1000 * static_cast<i64>(k), z = -500 * static_cast<i64>(k);
    const i64 half = 400 + static_cast<i64>(k);
    CHECK(f.covers_mm(x, z));
    CHECK(f.covers_mm(x + half, z));
    CHECK_FALSE(f.covers_mm(x + half + 2, z));
    CHECK(f.points[0].x_mm == x - half);
    CHECK(f.points[2].z_mm == z + half);
  }
}

}  // namespace

TEST_CASE("scene_gen: a footprint list owns its polygons' points") {
  FootprintList list;
  CHECK(list.empty());
  list.add_disc(5, 6, 1.5);
  list.add(Footprint::band(0, 0, 4000, 0, 300));
  FootprintPoint scratch[4];
  const u32 count = 1000;
  for (u32 k = 0; k < count; ++k) {
    // One scratch for every polygon, overwritten each time: the list's copy is its own.
    list.add(square(scratch, 1000 * static_cast<i64>(k), -500 * static_cast<i64>(k),
                    400 + static_cast<i64>(k)));
  }
  scratch[0] = scratch[1] = scratch[2] = scratch[3] = FootprintPoint{};
  REQUIRE(list.size() == count + 2);
  CHECK(list.point_count() == 4 * count);
  CHECK(list[0].shape == FootprintShape::disc);
  CHECK(list[0].radius == 1.5);
  CHECK(list[1].shape == FootprintShape::band);
  CHECK(list[1].covers_mm(2000, 300));
  check_squares(list, 2, count);

  // Copied: the copy reads its own store, so the source may go.
  FootprintList copy = list;
  FootprintList assigned;
  assigned = list;
  list.clear();
  CHECK(list.empty());
  CHECK(list.point_count() == 0);
  check_squares(copy, 2, count);
  check_squares(assigned, 2, count);
  CHECK(copy[5].points != assigned[5].points);

  // Moved, and added to after: still each its own.
  FootprintList moved = std::move(copy);
  CHECK(copy.empty());
  for (u32 k = 0; k < 300; ++k)
    moved.add_disc(k, k, 1.0);
  check_squares(moved, 2, count);
  // One list added to another, through its span.
  FootprintList both;
  both.add(assigned.span());
  both.add(moved.span());
  check_squares(both, 2, count);
  check_squares(both, count + 4, count);

  // A polygon with no points is nowhere, and names none.
  Footprint none;
  none.shape = FootprintShape::polygon;
  FootprintList empty_polygon;
  empty_polygon.add(none);
  CHECK(empty_polygon[0].points == nullptr);
  CHECK_FALSE(empty_polygon[0].covers_mm(0, 0));
}

TEST_CASE("scene_gen: a hand-over's clear empties its shapes with its placements and meshes") {
  Placements placed;
  FootprintPoint scratch[4];
  placed.shapes.add(square(scratch, 0, 0, 400));
  placed.instances.push_back(Placement{});
  placed.meshes.push_back(PlacementMesh{"a.glb", "a"});
  placed.things = 3;
  placed.clear();
  CHECK(placed.shapes.empty());
  CHECK(placed.shapes.point_count() == 0);
  CHECK(placed.instances.empty());
  CHECK(placed.meshes.empty());
  CHECK(placed.things == 0);
  // The hook is the descriptor's last entry and absent by default: a generator that plans nothing
  // is opened once, as it always was.
  const PlacementGeneratorDesc plain;
  CHECK(plain.plan_ground == nullptr);
  CHECK(GroundPlan{}.empty());
}
