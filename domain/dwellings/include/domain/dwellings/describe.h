#pragma once

// A planned house as a building description (docs/subsystems/dwellings.md, "The description a plan
// becomes"): the shell its plan stands in and everything the plan holds, written as the structures
// capability's `engine.structure.Description`, which its one mesher draws and its walk records
// read. This is where the rural filler ends: `fill_rural` is plan 14's filler, a plot, a household
// and a seed to a description its author then owns.
//
// The description's frame is the plan's moved by half the outer wall's thickness on both axes: the
// plan is laid out between the outer walls' inner faces from (0, 0), the description on the walls'
// centre lines from (0, 0), the front wall along z = 0 and the lane on its -z side.

#include <domain/dwellings/rural_filler.h>

#include <core/base/types.h>

#include <schemas/structures.h>
#include <string>
#include <string_view>

namespace engine::dwellings {

// What a plan's coordinate gains to become the description's, centimetres.
inline i32 description_offset_cm(const RuralChoice& choice) noexcept {
  return choice.outer_wall_cm / 2;
}

// The largest clear width or depth a house may have in the description's reach (every coordinate
// of a description, the roof's outer faces and a gable stack's proud face among them, lies within
// 15 m of its origin): the reach less a wall and a half and the stack's 8 cm, on the 10 cm module.
i32 max_clear_cm(u16 outer_wall_cm) noexcept;

// The description of a plan in the shell the filler chose: the footprint, the storeys, four
// exterior walls a storey, the inner walls, every door, window and doorway, the rooms, the stair,
// a chimney a hearth with its hearth stone and bressumer, one gable roof with its ridge along the
// longer side, and the filler's provenance. `out` is replaced.
void describe(const InteriorPlan& plan, const RuralChoice& choice, std::string_view name,
              u64 seed, structure::Description& out);

// The rural filler: a plot, a household and a seed to a description that passes the mesher's
// validator. False with a sentence when the plot holds no house.
bool fill_rural(const RuralPlot& plot, const Household& household, const RuralStyle& style,
                u64 seed, std::string_view name, structure::Description& out,
                std::string* error);

}  // namespace engine::dwellings
