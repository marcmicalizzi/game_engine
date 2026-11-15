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
constexpr u64 k_furnishing_pin = 0x1aa941cf067715ceull;

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

  // A house of one storey shows no joists: its rooms are ceiled under the roof or open to it.
  REQUIRE(plan_rural(RuralPlot{2400, 1500}, Household{2, 0}, style, 7, choice, plan, &error));
  f.clear();
  timber_interior(plan, choice.ridge_along_front, f);
  CHECK(f.pieces.empty());
}
