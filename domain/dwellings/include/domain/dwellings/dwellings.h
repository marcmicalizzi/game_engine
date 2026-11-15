#pragma once

// dwellings capability (ADR-0027; docs/subsystems/dwellings.md; plan 14 §14.3, "The fillers").
//
// What it is: the plan of a small house. `interiors.h` lays a clear rectangle out as rooms a
// person recognises — a one-room cot, a hall and parlour, a cross-passage house, a two-up
// two-down — with inner walls, doorways, an enclosed straight stair, hearths and the places of
// the outer doors and windows, and validates it against the walker's numbers. `rural_filler.h`
// chooses that house from a plot, a household and a seed.
//
// What it is not: it meshes nothing and reads no ground. A plan is description data; the
// structures capability's mesher draws it.
//
// ADR-0027's registration points, each wired or deliberately not needed:
//
//   [-] schema types      none: a plan is plain records the description's own types carry
//   [-] scheduler entry   none: nothing here ticks
//   [-] render passes     none
//   [-] derived data      none of its own: a plan is a few hundred bytes, made when asked
//   [-] protocol methods  not yet: `structure.fill` is plan 14's, with the document records
//   [-] tunables          none: a caller's style is a struct, a plan is a pure function of it
//   [x] LOD policy        none of its own, by design: see k_lod below
//   [x] determinism       k_determinism below
//   [x] zero cost unused  nothing is registered and nothing runs until a caller asks for a plan
//   [x] docs, tests, size table

#include <domain/dwellings/interiors.h>
#include <domain/dwellings/rural_filler.h>

namespace engine::dwellings {

/// Determinism stance: a plan is a pure function of its parameters, in integer centimetres and
/// the engine's hash, with no floating point and no C library call. The same plan on every
/// toolchain, pinned by `tests/determinism_tests.cpp`.
inline constexpr const char* k_determinism = "pure function, integer, pinned";

/// LOD policy: the plan has none. It is description data of a few hundred bytes a house, made
/// once when a filler or an author asks; what is drawn at a distance is the mesher's interior
/// mesh and the cluster DAG's cut of it, and whether an interior is resident at all is the
/// streaming's (plan 14 §14.7).
inline constexpr const char* k_lod = "none: description data";

}  // namespace engine::dwellings
