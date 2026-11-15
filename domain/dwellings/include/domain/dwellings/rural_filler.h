#pragma once

// The rural filler (docs/subsystems/dwellings.md, "The rural filler"; plan 14 §14.3,
// "The fillers"; roadmap G72): from a plot, a household and a seed, a house — its plan type, its
// storeys, its clear inner rectangle, which end its hearth stands at — and from that the interior
// (interiors.h) and the building description the mesher takes.
//
// A filler is a starting point, never the source (plan 14 §14.1): what it writes is a description
// its author then owns, and nothing runs it again behind the author's back.
//
// What is fixed and what varies. The household and the plot fix the type, the storeys and the
// hearths: `choose_rural` draws nothing for them, so two households of one size and means on plots
// of one size live in houses of one type. The seed varies what one type's houses differ in on a
// lane: the length and the depth on a 10 cm module inside the type's measured range, which end the
// hearth is at, where the hall ends and the second room begins, the passage's width, and the door's
// place in a cot's front. The type fixes the rest: the order of the rooms, the room the door opens
// into, where the stack stands, where the stair goes.
//
// Determinism: integer centimetres and the engine's hash, one draw a statement; the same plot,
// household, style and seed give the same choice on every toolchain.

#include <core/base/types.h>
#include <domain/dwellings/interiors.h>

#include <string>

namespace engine::dwellings {

inline constexpr u32 k_rural_filler_version = 2;

// The rectangle of a plot a house may stand in: its length along the lane and its depth away from
// it, centimetres. The filler never looks at the ground: whoever places the house (a village
// filler, an author) cuts or fills its pad.
struct RuralPlot {
  i32 frontage_cm = 0;
  i32 depth_cm = 0;
};

// Who lives there. `means` is 0 for a labourer's household, 1 for a husbandman's, 2 for a
// yeoman's: it buys an upper storey and a second hearth, never a different order of rooms.
struct Household {
  u8 persons = 4;
  u8 means = 1;
};

// What the house is built of, as far as its plan needs to know: the description's style row fills
// it (rural_filler.cpp, `style_from`).
struct RuralStyle {
  u16 outer_wall_cm = 50;
  u16 inner_wall_cm = 10;
  u16 storey_cm = 240;         // floor to floor, under an upper storey
  u16 single_storey_cm = 240;  // the floor to the wall plate of a house of one storey
  // The upper storey's floor to its plate: high enough that a window's head, 2.0 m, stands clear
  // of a thatch's eave, which hangs half a metre under the plate.
  u16 upper_clear_cm = 250;
  u16 floor_cm = 20;
  u16 door_cm = 95;
  u16 doorway_cm = 85;
  u16 head_cm = 200;
  u16 window_cm = 90;
  u16 sill_cm = 90;
  // What the description states besides the shell and the plan (furnish.h): the timber a room
  // shows (joists and a beam under an upper floor, a stair's nosings and newels) and its
  // furniture. Each is a caller's to leave out: a bare house is the same plan.
  bool timber = true;
  bool furniture = true;
  // What one house's shell differs from the next one's in (describe.h): door leaves, shutters,
  // mullions, walls that wander and batter, rounded corners, a proud plinth, drawn from the seed.
  // False: the plain shell, every house's joinery and walls the mesher's defaults.
  bool dressed = true;
};

// What the filler chose, before any description is written.
struct RuralChoice {
  InteriorParams interior;
  i32 outer_width_cm = 0;  // the footprint, outer faces
  i32 outer_depth_cm = 0;
  u16 outer_wall_cm = 0;
  bool ridge_along_front = true;  // the ridge runs along the longer side
  bool demoted = false;           // the plot was too small for what the household asked
  bool timber = true;             // the style's: what `describe` states besides the plan
  bool furniture = true;
  bool dressed = true;
};

// The type, storeys and hearths a household asks for, on a plot with room for anything.
void household_asks(const Household& household, InteriorParams& out) noexcept;

// The choice. False with a sentence when the plot holds no house at all (a cot needs about 5 m of
// frontage and 4.3 m of depth at the default walls).
bool choose_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                  u64 seed, RuralChoice& out, std::string* error);

// The choice and its interior, in one call.
bool plan_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                u64 seed, RuralChoice& choice, InteriorPlan& plan, std::string* error);

}  // namespace engine::dwellings
