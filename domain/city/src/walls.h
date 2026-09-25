#pragma once

// Walls, openings and furniture zones from a building's spaces (docs/subsystems/city.md, "Walls,
// doors and windows"). Private to the module: the grammar calls it once the rooms of every floor
// are laid, and nothing else may, because it relies on the grammar's promise that a floor's spaces
// tile its footprint exactly.

#include <domain/city/building.h>
#include <domain/city/params.h>
#include <domain/city/plan.h>

namespace engine::city {

// The walls of every floor — one centre line per stretch between two spaces (or a space and the
// outside) — then the doors (the entrances by archetype, circulation onto the corridor, and each
// unit's rooms by the room-graph rule), then the windows of every habitable room.
void make_walls_and_openings(const Plan& plan, Building& b, const Rules& r);

// Where the furniture of each room goes, by its semantic: zones, not furniture.
void make_zones(Building& b, const Rules& r);

}  // namespace engine::city
