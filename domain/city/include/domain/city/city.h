#pragma once

// city capability (ADR-0027; docs/subsystems/city.md; plan 13 §13.2, plan 07 §7.6, ADR-0044).
//
// Island City's generator, in two levels. The **plan** is a pure function of an island's
// parameters, made once and cached as a derived node: the coastline, the districts (downtown,
// mixed use, residential, old town, industry and port, waterfront, campus), the street graph by
// class, the blocks and lots, the parks (central, district, pocket, squares, waterfront,
// greenbelt) and the civic reservations. Every **building** is a pure function of (plan, lot): the
// hierarchical grammar of plan 13 §13.2 from footprint to furniture zones, output as a description
// with an occupancy summary, never as meshes. The validators are the review loop, and the share of
// buildings they pass without repair is E18's yield. The proxy fragment is how a city is looked at
// before any kit exists.
//
//   params.h     an island's parameters and style tables, converted to integers once
//   plan.h       the whole-island plan, its per-tile query, its key and its file
//   building.h   the grammar: one lot to one building description and its occupancy summary
//   validate.h   the validators, and E18's yield
//   fragment.h   the proxy scene fragment
//
// Registration points (ADR-0027 decision 2), each wired or deliberately not needed; the docs page
// says why:
//
//   [x] schema types      domain/city/schemas/city.schema (engine.city), the capability's own
//   [-] scheduler entry   none: nothing ticks; a plan and a building are functions of their inputs
//   [-] render passes     none: the proxies are instances the renderer already draws
//   [x] derived data      `engine-content city plan` caches the plan under ddc/city/<key>/;
//                         `city building`, `city fragment` and `city yield` read it
//   [-] protocol methods  none yet
//   [-] tunables          none: every parameter is the island's, and the island is content
//   [x] LOD policy        stage_for_distance(): massing far, cores and floors in the middle, rooms
//                         near
//   [x] determinism       k_determinism below
//   [x] zero cost unused  nothing calls the module unless a command or a consumer asks for a city;
//                         switched off, it is not linked
//   [x] docs, tests, size table, bench

#include <domain/city/building.h>
#include <domain/city/fragment.h>
#include <domain/city/params.h>
#include <domain/city/plan.h>
#include <domain/city/validate.h>

namespace engine::city {

// Determinism stance (ADR-0010): "derived". The plan and every building are content made from
// the island's parameters, never state the simulation ticks; what happens to a building afterwards
// (a door left open, a wall brought down) is the persistent store's. And bit for bit on every
// toolchain, which the finite island needs as the endless desert does: a tile dropped and
// materialized again holds the same buildings, and an apartment named by its id is the same
// apartment on every machine.
inline constexpr const char* k_determinism = "derived";

}  // namespace engine::city
