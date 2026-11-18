// Timber and furniture (docs/subsystems/dwellings.md, "Timber and furniture"): every piece is
// made of members; no piece of any house the filler plans stands in the tour's way, in a door's
// approach, before a hearth or in another piece; the rooms get what their use asks for; the timber
// is where an upper floor and a stair are; and the same furnishing comes from the same seed on
// every toolchain.

#include <core/hash/hash.h>
#include <domain/dwellings/furnish.h>
#include <domain/dwellings/rural_filler.h>
#include <domain/dwellings/tour.h>

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::dwellings;

namespace {

// Taken on MSVC 19.44 (msvc-debug) on 2026-11-15 at k_furnish_version 1, k_interior_version 2 and
// k_rural_filler_version 2.
// And at k_rural_filler_version 3 (G208; 0x1aa941cf067715ce before): the same furnisher over the
// filler's houses, fewer of which are two-up two-downs.
// And at k_furnish_version 2 (G219, 2026-11-17; 0x0da968d6a8cc2b6e before, MSVC msvc-debug): the
// timber over a house of one storey, a hall's bed where a plan has no chamber, and the houses'
// new inner rooms and kitchens' fires.
constexpr u64 k_furnishing_pin = 0xcfc74546599b4f1full;

u32 count_kind(const Furnishing& f, PieceKind kind, u32 room) {
  u32 n = 0;
  for (const Piece& p : f.pieces)
    n += (p.kind == kind && p.room == room) ? 1u : 0u;
  return n;
}

// How far a point stands from a rectangle, centimetres along the worse axis (0 inside).
i32 clear_of(const RectCm& r, i32 x, i32 z) {
  const i32 dx = x < r.x0 ? r.x0 - x : (x > r.x1 ? x - r.x1 : 0);
  const i32 dz = z < r.z0 ? r.z0 - z : (z > r.z1 ? z - r.z1 : 0);
  return dx > dz ? dx : dz;
}

}  // namespace

TEST_CASE("furnish: a piece is made of members, and a wrong size is refused") {
  Furnishing f;
  REQUIRE(
      add_piece(f, PieceKind::TableSet, RectCm{0, 0, 180, k_table_set_out_cm}, Side::Front, 0, 0));
  REQUIRE(add_piece(f, PieceKind::Settle, RectCm{0, 200, k_settle_out_cm, 380}, Side::Left, 0, 0));
  REQUIRE(add_piece(f, PieceKind::Bed, RectCm{300, 0, 447, k_bed_out_cm}, Side::Front, 0, 0));
  REQUIRE(add_piece(f, PieceKind::Chest, RectCm{300, 300, 410, 300 + k_chest_out_cm}, Side::Back, 0,
                    0));
  REQUIRE(f.pieces.size() == 4);
  const u32 members[4] = {20, 5, 10, 4};
  const u16 heights[4] = {75, 105, 110, 52};
  for (u32 i = 0; i < 4; ++i) {
    const Piece& p = f.pieces[i];
    CHECK(p.member_count == members[i]);
    CHECK(p.height_cm == heights[i]);
    u32 solids = 0;
    for (u32 m = p.first_member; m < p.first_member + p.member_count; ++m) {
      const Member& member = f.members[m];
      // Inside the footprint, on the floor or over it, no taller than the piece.
      CHECK(member.x0 >= p.footprint.x0);
      CHECK(member.x1 <= p.footprint.x1);
      CHECK(member.z0 >= p.footprint.z0);
      CHECK(member.z1 <= p.footprint.z1);
      CHECK(member.x0 < member.x1);
      CHECK(member.z0 < member.z1);
      CHECK(member.y0 >= 0);
      CHECK(member.y0 < member.y1);
      CHECK(member.y1 <= i32{p.height_cm});
      solids += member.solid ? 1u : 0u;
    }
    CHECK(solids >= 1);
  }
  // The table's top stands 75 cm up and the settle's back against its wall.
  CHECK(f.pieces[1].against == Side::Left);

  CHECK_FALSE(add_piece(f, PieceKind::Chest, RectCm{0, 0, 60, k_chest_out_cm}, Side::Front, 0, 0));
  CHECK_FALSE(add_piece(f, PieceKind::Bed, RectCm{0, 0, 147, 180}, Side::Front, 0, 0));
  CHECK_FALSE(add_piece(f, PieceKind::Joists, RectCm{0, 0, 400, 400}, Side::Front, 0, 0));
  CHECK(f.pieces.size() == 4);
}

TEST_CASE("furnish: no piece of any house stands in the tour's way, and rooms get their use") {
  const RuralStyle style;
  const RuralPlot plots[3] = {RuralPlot{2400, 1500}, RuralPlot{760, 1500}, RuralPlot{1200, 520}};
  u64 pin = k_hash_seed;
  u32 halls = 0, tables = 0, heated = 0, settles = 0, sleeping = 0, beds = 0, chests = 0;
  u32 houses = 0, most_members = 0;
  for (const RuralPlot& plot : plots) {
    for (u8 persons = 1; persons <= 9; ++persons) {
      for (u8 means = 0; means <= 2; ++means) {
        for (u64 seed = 0; seed < 8; ++seed) {
          RuralChoice choice;
          InteriorPlan plan;
          std::string error;
          const u64 house_seed = seed * 977 + persons;
          INFO("plot " << plot.frontage_cm << " persons " << int{persons} << " means " << int{means}
                       << " seed " << seed);
          REQUIRE(
              plan_rural(plot, Household{persons, means}, style, house_seed, choice, plan, &error));
          Furnishing f;
          timber_interior(plan, choice.ridge_along_front, f);
          const u32 timber_pieces = f.pieces.size();
          furnish_interior(plan, house_seed, f);
          std::string why;
          const bool valid = validate_furnishing(plan, f, &why);
          INFO(why);
          REQUIRE(valid);

          // The tour's line, read again here: every point of it at least the tour's clearance
          // from every piece on its storey.
          Vector<TourPoint> tour;
          REQUIRE(tour_interior(plan, 150, tour));
          for (u32 i = timber_pieces; i < f.pieces.size(); ++i) {
            const Piece& p = f.pieces[i];
            for (const TourPoint& t : tour) {
              if ((t.y_mm == 0 ? 0 : 1) != int{p.storey}) continue;
              CHECK(clear_of(p.footprint, t.x_cm, t.z_cm) >= k_tour_clear_cm);
            }
          }

          for (u32 r = 0; r < plan.rooms.size(); ++r) {
            const Room& room = plan.rooms[r];
            const bool cot = plan.params.type == PlanType::Cot;
            const bool sat_in =
                room.kind == RoomKind::Parlour && plan.params.type == PlanType::TwoUpTwoDown;
            if (room.kind == RoomKind::Hall || room.kind == RoomKind::Kitchen || sat_in) {
              ++halls;
              tables += count_kind(f, PieceKind::TableSet, r);
              if ((room.flags & k_room_heated) != 0) {
                ++heated;
                settles += count_kind(f, PieceKind::Settle, r);
              }
            }
            if (cot || (room.kind == RoomKind::Parlour && !sat_in) ||
                room.kind == RoomKind::Inner || room.kind == RoomKind::Chamber) {
              ++sleeping;
              beds += count_kind(f, PieceKind::Bed, r);
              chests += count_kind(f, PieceKind::Chest, r);
            }
            // A stair's compartment, a passage and a service room stay bare.
            if (room.kind == RoomKind::Stair || room.kind == RoomKind::Passage ||
                room.kind == RoomKind::Service) {
              for (u32 i = timber_pieces; i < f.pieces.size(); ++i)
                CHECK(f.pieces[i].room != r);
            }
            CHECK(count_kind(f, PieceKind::TableSet, r) <= 1);
            CHECK(count_kind(f, PieceKind::Bed, r) <= 1);
          }
          if (f.members.size() > most_members) most_members = f.members.size();
          pin = hash_combine(pin, hash_furnishing(f));
          ++houses;
        }
      }
    }
  }
  std::printf(
      "dwellings furniture over %u houses: %u of %u halls a table, %u of %u heated a "
      "settle, %u beds and %u chests in %u sleeping rooms; most members %u\n",
      houses, tables, halls, settles, heated, beds, chests, sleeping, most_members);
  // What the rooms' sizes allow: a table in nine halls of ten, a bed in nine sleeping rooms of ten.
  CHECK(tables * 10 >= halls * 9);
  CHECK(beds * 10 >= sleeping * 9);
  CHECK(chests * 10 >= sleeping * 8);
  CHECK(settles * 10 >= heated * 6);
  if (pin != k_furnishing_pin) {
    std::printf("dwellings furnishing pin: 0x%016llxull\n", static_cast<unsigned long long>(pin));
  }
  CHECK(pin == k_furnishing_pin);
}

TEST_CASE("furnish: the timber is under an upper floor and on the stair") {
  const RuralStyle style;
  RuralChoice choice;
  InteriorPlan plan;
  std::string error;
  REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{5, 2}, style, 7, choice, plan, &error));
  REQUIRE(plan.params.storeys == 2);
  REQUIRE(plan.stairs.size() == 1);
  Furnishing f;
  timber_interior(plan, choice.ridge_along_front, f);
  std::string why;
  REQUIRE(validate_furnishing(plan, f, &why));
  const i32 ceiling = i32{plan.params.storey_cm} - i32{plan.params.floor_cm};
  u32 nosings = 0, newels = 0;
  for (const Piece& p : f.pieces) {
    const Room& room = plan.rooms[p.room];
    CHECK(room.storey == 0);
    for (u32 m = p.first_member; m < p.first_member + p.member_count; ++m) {
      const Member& member = f.members[m];
      CHECK_FALSE(member.solid);
      if (p.kind == PieceKind::Joists || p.kind == PieceKind::Beam) {
        // Hung from the ceiling, never in a stair's compartment, never under a door's head.
        CHECK(room.kind != RoomKind::Stair);
        CHECK(member.y1 == ceiling + 1);
        CHECK(member.y0 >= i32{plan.params.head_cm});
      }
    }
    if (p.kind == PieceKind::Joists) {
      // Joists at 52 cm centres: one for every 52 cm of the room along the ridge.
      const i32 length =
          choice.ridge_along_front ? room.rect.x1 - room.rect.x0 : room.rect.z1 - room.rect.z0;
      CHECK(p.member_count == static_cast<u32>(length / 52));
    }
    if (p.kind == PieceKind::Nosings) nosings += p.member_count;
    if (p.kind == PieceKind::Newel) ++newels;
  }
  CHECK(nosings == plan.stairs[0].risers);
  CHECK(newels == 2);
  for (u32 r = 0; r < plan.rooms.size(); ++r) {
    const Room& room = plan.rooms[r];
    if (room.storey != 0) continue;
    CHECK(count_kind(f, PieceKind::Joists, r) == (room.kind == RoomKind::Stair ? 0u : 1u));
  }

  // **What is overhead in a house of one storey** (need 153 (b)): a cot's one room is open to its
  // roof and shows tie beams at the plate, 20 cm deep, across it and clear of its stack; a house of
  // rooms on one floor is ceiled at the plate on joists, a set a room but the passage's too.
  REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{2, 0}, style, 7, choice, plan, &error));
  REQUIRE(open_to_roof(plan));
  f.clear();
  timber_interior(plan, choice.ridge_along_front, f);
  REQUIRE_FALSE(f.pieces.empty());
  for (const Piece& p : f.pieces) {
    CHECK(p.kind == PieceKind::TieBeam);
    REQUIRE(p.member_count == 1);
    const Member& m = f.members[p.first_member];
    CHECK(m.y1 == i32{plan.params.storey_cm});
    CHECK(m.y0 == i32{plan.params.storey_cm} - 20);
    CHECK(m.y0 >= k_headroom_cm);
    for (const Hearth& h : plan.hearths)
      CHECK_FALSE(
          (m.x0 < h.stack.x1 && h.stack.x0 < m.x1 && m.z0 < h.stack.z1 && h.stack.z0 < m.z1));
  }
  CHECK(room_clear_cm(plan, plan.rooms[0]) == i32{plan.params.storey_cm} - 20);
  REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{4, 0}, style, 7, choice, plan, &error));
  REQUIRE(plan.params.storeys == 1);
  REQUIRE_FALSE(open_to_roof(plan));
  f.clear();
  timber_interior(plan, choice.ridge_along_front, f);
  for (u32 r = 0; r < plan.rooms.size(); ++r)
    CHECK(count_kind(f, PieceKind::Joists, r) == 1u);
  for (const Member& m : f.members) {
    CHECK(m.y1 == i32{plan.params.storey_cm} + 1);
    CHECK(m.y0 >= i32{plan.params.head_cm});
  }
}

// **A household sleeps in a bed** (need 153 (c): two cross-passage houses of six persons and one
// storey had none): over every household on a generous plot and four seeds, every house the
// filler plans has a bed, and one of six with no means for an upper floor sleeps in its inner
// room.
TEST_CASE("furnish: every house the filler plans has a bed") {
  const RuralStyle style;
  for (u8 persons = 1; persons <= 9; ++persons)
    for (u8 means = 0; means <= 2; ++means)
      for (u64 seed = 1; seed <= 4; ++seed) {
        RuralChoice choice;
        InteriorPlan plan;
        std::string error;
        REQUIRE_MESSAGE(plan_rural(RuralPlot{2400, 1500}, Household{persons, means}, style, seed,
                                   choice, plan, &error),
                        error);
        Furnishing f;
        furnish_interior(plan, seed, f);
        u32 beds = 0;
        for (const Piece& p : f.pieces)
          beds += p.kind == PieceKind::Bed ? 1u : 0u;
        CHECK_MESSAGE(beds > 0, plan_type_name(plan.params.type)
                                    << " of " << u32{plan.params.storeys} << " storeys, "
                                    << u32{persons} << " persons, means " << u32{means});
        if (persons >= 5 && persons <= 6 && means == 0) {
          CHECK(plan.params.type == PlanType::CrossPassage);
          CHECK(plan.params.inner_room);
        }
      }
  // A hand-made plan with no room beyond its hall sleeps in the hall.
  InteriorParams p;
  p.type = PlanType::CrossPassage;
  p.storeys = 1;
  p.width_cm = 1100;
  p.depth_cm = 520;
  p.seed = 3;
  InteriorPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_interior(p, plan, &error), error);
  Furnishing f;
  furnish_interior(plan, 3, f);
  u32 hall_beds = 0;
  for (const Piece& piece : f.pieces)
    hall_beds +=
        piece.kind == PieceKind::Bed && plan.rooms[piece.room].kind == RoomKind::Hall ? 1u : 0u;
  CHECK(hall_beds == 1);
}

// **The kitchen has the fire** (need 153 (d): a two-up two-down of one hearth had it in the
// parlour and none in its kitchen): of one hearth, the kitchen's; of two, both rooms'.
TEST_CASE("furnish: a two-up two-down's one hearth is its kitchen's") {
  for (u8 hearths = 1; hearths <= 2; ++hearths)
    for (u64 seed = 1; seed <= 4; ++seed) {
      InteriorParams p;
      p.type = PlanType::TwoUpTwoDown;
      p.storeys = 2;
      p.hearths = hearths;
      p.width_cm = 520;
      p.depth_cm = 900;
      p.seed = seed;
      p.mirror = seed % 2 == 0;
      InteriorPlan plan;
      std::string error;
      REQUIRE_MESSAGE(plan_interior(p, plan, &error), error);
      REQUIRE(plan.hearths.size() == hearths);
      u32 kitchen = 0, parlour = 0;
      for (const Hearth& h : plan.hearths) {
        kitchen += plan.rooms[h.room].kind == RoomKind::Kitchen ? 1u : 0u;
        parlour += plan.rooms[h.room].kind == RoomKind::Parlour ? 1u : 0u;
      }
      CHECK(kitchen == 1);
      CHECK(parlour == hearths - 1u);
    }
}

// **A kit chest has a lid** (need 153 (a): no kit chest of twelve houses had one): a chest a kit
// row stands in for is a place to stand a thing on, its foot at its height; a kit table, settle or
// bed states no surface.
TEST_CASE("furnish: a kit chest's lid is a place") {
  const RuralStyle style;
  const KitRow rows[2] = {KitRow{0, PieceKind::Chest, 95, 50, 55},
                          KitRow{1, PieceKind::Bed, 145, 205, 105}};
  const Kit kit{rows, 2};
  u32 kit_chests = 0;
  for (u64 seed = 1; seed <= 4; ++seed) {
    RuralChoice choice;
    InteriorPlan plan;
    std::string error;
    REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{5, 2}, style, seed, choice, plan, &error));
    Furnishing f;
    furnish_interior(plan, seed, f, kit);
    Vector<NamedPlace> named;
    house_places(plan, f, choice.outer_wall_cm, named);
    for (u32 i = 0; i < f.pieces.size(); ++i) {
      const Piece& p = f.pieces[i];
      if (p.kit == 0) continue;
      u32 lids = 0, mattresses = 0;
      for (const NamedPlace& n : named) {
        if (n.index != i) continue;
        if (n.kind == PlaceKind::ChestLid) {
          ++lids;
          CHECK(n.y0_mm == i32{p.height_cm} * 10);
          CHECK(n.y1_mm == n.y0_mm);
          CHECK(n.rect.x0 == p.footprint.x0);
          CHECK(n.rect.z1 == p.footprint.z1);
          CHECK(n.room == p.room);
        }
        mattresses += n.kind == PlaceKind::Mattress ? 1u : 0u;
      }
      CHECK(lids == (p.kind == PieceKind::Chest ? 1u : 0u));
      CHECK(mattresses == 0);
      kit_chests += p.kind == PieceKind::Chest ? 1u : 0u;
    }
  }
  CHECK(kit_chests > 0);
}

// **Kit pieces** (dwellings.md, "Kit pieces"): over every household's house on three plots and
// four seeds, a kit's rows stand in the box pieces' place wherever they fit, with no members, at
// the furnisher's own clearances (the validator reads every one back, and the tour's line is
// measured again here), never into the joists; where a row fits nowhere the box stands; and with
// no kit the furnishing is the same bytes.
TEST_CASE("furnish: a kit's pieces stand in the box pieces' place at the furnisher's clearances") {
  const RuralStyle style;
  const KitRow rows[5] = {KitRow{0, PieceKind::TableSet, 170, 110, 80},
                          KitRow{1, PieceKind::Bed, 145, 205, 105},
                          KitRow{2, PieceKind::Chest, 95, 50, 55},
                          KitRow{3, PieceKind::Settle, 160, 60, 300},  // under no ceiling
                          KitRow{4, PieceKind::Chest, 1500, 50, 55}};  // longer than any wall
  const Kit kit{rows, 5};
  const RuralPlot plots[3] = {RuralPlot{2400, 1500}, RuralPlot{760, 1500}, RuralPlot{1200, 520}};
  u32 houses = 0, kit_pieces = 0, by_row[5] = {0, 0, 0, 0, 0};
  for (const RuralPlot& plot : plots)
    for (u8 persons = 1; persons <= 9; ++persons)
      for (u8 means = 0; means <= 2; ++means)
        for (u64 seed = 1; seed <= 4; ++seed) {
          RuralChoice choice;
          InteriorPlan plan;
          std::string error;
          if (!plan_rural(plot, Household{persons, means}, style, seed, choice, plan, &error))
            continue;
          ++houses;
          Furnishing plain, with;
          furnish_interior(plan, seed, plain);
          Furnishing plain_again;
          furnish_interior(plan, seed, plain_again, Kit{});
          CHECK(hash_furnishing(plain) == hash_furnishing(plain_again));
          furnish_interior(plan, seed, with, kit);
          std::string why;
          CHECK_MESSAGE(validate_furnishing(plan, with, &why), why);
          Vector<TourPoint> tour;
          tour_interior(plan, 150, tour);
          for (const Piece& p : with.pieces) {
            if (p.kit == 0) continue;
            ++kit_pieces;
            REQUIRE(p.kit <= 5);
            ++by_row[p.kit - 1];
            const KitRow& row = rows[p.kit - 1];
            CHECK(p.kind == row.replaces);
            CHECK(p.member_count == 0);
            CHECK(i32{p.height_cm} + k_kit_head_gap_cm <= room_clear_cm(plan, plan.rooms[p.room]));
            const bool along_x = p.against == Side::Front || p.against == Side::Back;
            CHECK((along_x ? p.footprint.x1 - p.footprint.x0 : p.footprint.z1 - p.footprint.z0) ==
                  row.along_cm);
            CHECK((along_x ? p.footprint.z1 - p.footprint.z0 : p.footprint.x1 - p.footprint.x0) ==
                  row.out_cm);
            for (const TourPoint& t : tour) {
              if (t.room != p.room) continue;  // a room is on one storey
              CHECK(clear_of(p.footprint, t.x_cm, t.z_cm) >= k_tour_clear_cm);
            }
          }
        }
  MESSAGE(houses << " houses, " << kit_pieces << " kit pieces: " << by_row[0] << " tables, "
                 << by_row[1] << " beds, " << by_row[2] << " chests");
  CHECK(by_row[0] > houses / 3);
  CHECK(by_row[1] > houses / 3);
  CHECK(by_row[2] > 0);
  CHECK(by_row[3] == 0);
  CHECK(by_row[4] == 0);
}

// **The places a plan knows** (dwellings.md, "The places a plan knows"): over every household's
// house on a generous plot and four seeds, each hearth has its recess inside its stack and its
// mouth on the stack's face, every table set its top at 75 cm and two bench seats, every bed a
// mattress, every chest a lid, every joist an underside, and every door, doorway and window its
// floor.
TEST_CASE("furnish: the places a plan knows are named, one a thing the plan holds") {
  const RuralStyle style;
  u32 houses = 0, places = 0;
  for (u8 persons = 1; persons <= 9; ++persons)
    for (u8 means = 0; means <= 2; ++means)
      for (u64 seed = 1; seed <= 4; ++seed) {
        RuralChoice choice;
        InteriorPlan plan;
        std::string error;
        REQUIRE_MESSAGE(plan_rural(RuralPlot{2400, 1500}, Household{persons, means}, style, seed,
                                   choice, plan, &error),
                        error);
        Furnishing f;
        timber_interior(plan, choice.ridge_along_front, f);
        furnish_interior(plan, seed, f);
        Vector<NamedPlace> named;
        house_places(plan, f, choice.outer_wall_cm, named);
        ++houses;
        places += named.size();
        u32 count[16] = {};
        for (const NamedPlace& n : named) {
          ++count[static_cast<u32>(n.kind)];
          CHECK(n.rect.x0 <= n.rect.x1);
          CHECK(n.rect.z0 <= n.rect.z1);
          CHECK(n.y0_mm <= n.y1_mm);
          if (n.kind == PlaceKind::HearthRecess) {
            const RectCm& st = plan.hearths[n.index].stack;
            CHECK(n.rect.x0 >= st.x0);
            CHECK(n.rect.x1 <= st.x1);
            CHECK(n.rect.z0 >= st.z0);
            CHECK(n.rect.z1 <= st.z1);
            CHECK(n.y0_mm == k_raised_hearth_mm);
            CHECK(n.y1_mm == i32{plan.hearths[n.index].lintel_cm} * 10);
          }
          if (n.kind == PlaceKind::StackOutside) {
            // Outside the house, its stack's wall's outer face and 8 cm more away, in no room.
            const RectCm& st = plan.hearths[n.index].stack;
            const i32 out_cm = i32{choice.outer_wall_cm} + k_stack_proud_cm;
            CHECK(n.room == k_no_room);
            CHECK(n.y0_mm == 0);
            CHECK(n.y1_mm >= i32{plan.params.storey_cm} * 10);
            const bool thin_x = n.rect.x0 == n.rect.x1, thin_z = n.rect.z0 == n.rect.z1;
            CHECK(thin_x != thin_z);
            if (thin_z) CHECK((n.rect.z0 == -out_cm || n.rect.z0 == plan.params.depth_cm + out_cm));
            if (thin_x) CHECK((n.rect.x0 == -out_cm || n.rect.x0 == plan.params.width_cm + out_cm));
            CHECK((thin_z ? n.rect.x1 - n.rect.x0 == st.x1 - st.x0
                          : n.rect.z1 - n.rect.z0 == st.z1 - st.z0));
          }
          if (n.kind == PlaceKind::TableTop) CHECK(n.y0_mm == 750);
          if (n.kind == PlaceKind::BenchSeat) CHECK(n.y0_mm == 450);
          if (n.kind == PlaceKind::Mattress || n.kind == PlaceKind::ChestLid ||
              n.kind == PlaceKind::TableTop) {
            const RectCm& fp = f.pieces[n.index].footprint;
            CHECK(n.rect.x0 >= fp.x0);
            CHECK(n.rect.x1 <= fp.x1);
            CHECK(n.rect.z0 >= fp.z0);
            CHECK(n.rect.z1 <= fp.z1);
          }
        }
        u32 tables = 0, beds = 0, chests = 0, settles = 0, joists = 0, doors = 0, windows = 0;
        u32 ties = 0, outer_stacks = 0;
        for (const Hearth& h : plan.hearths)
          outer_stacks += (h.stack.x0 == 0 || h.stack.z0 == 0 ||
                           h.stack.x1 == plan.params.width_cm || h.stack.z1 == plan.params.depth_cm)
                              ? 1u
                              : 0u;
        for (const Piece& p : f.pieces) {
          ties += p.kind == PieceKind::TieBeam ? p.member_count : 0u;
          tables += p.kind == PieceKind::TableSet ? 1u : 0u;
          beds += p.kind == PieceKind::Bed ? 1u : 0u;
          chests += p.kind == PieceKind::Chest ? 1u : 0u;
          settles += p.kind == PieceKind::Settle ? 1u : 0u;
          joists += p.kind == PieceKind::Joists || p.kind == PieceKind::Beam ? p.member_count : 0u;
        }
        for (const OuterOpening& o : plan.openings)
          (o.door() ? doors : windows) += 1;
        CHECK(count[static_cast<u32>(PlaceKind::HearthRecess)] == plan.hearths.size());
        CHECK(count[static_cast<u32>(PlaceKind::HearthMouth)] == plan.hearths.size());
        CHECK(count[static_cast<u32>(PlaceKind::HearthApron)] == plan.hearths.size());
        CHECK(count[static_cast<u32>(PlaceKind::TableTop)] == tables);
        CHECK(count[static_cast<u32>(PlaceKind::BenchSeat)] == 2 * tables);
        CHECK(count[static_cast<u32>(PlaceKind::SettleSeat)] == settles);
        CHECK(count[static_cast<u32>(PlaceKind::Mattress)] == beds);
        CHECK(count[static_cast<u32>(PlaceKind::ChestLid)] == chests);
        CHECK(count[static_cast<u32>(PlaceKind::JoistUnderside)] == joists);
        CHECK(count[static_cast<u32>(PlaceKind::TieBeamUnderside)] == ties);
        // Something overhead to hang a thing from in every house (need 153 (b)).
        CHECK(joists + ties > 0);
        // One face outside a stack in an outer wall (a gable's or a side wall's: no plan here
        // puts a stack in a corner).
        CHECK(count[static_cast<u32>(PlaceKind::StackOutside)] == outer_stacks);
        CHECK(count[static_cast<u32>(PlaceKind::DoorApproach)] == doors);
        CHECK(count[static_cast<u32>(PlaceKind::WindowLight)] == windows);
        CHECK(count[static_cast<u32>(PlaceKind::DoorwayApproach)] == plan.doorways.size());
      }
  MESSAGE(houses << " houses, " << places << " named places");
}
