#pragma once

// flora capability (ADR-0027; docs/subsystems/flora.md, docs/plan/05-simulation.md §5.15): the
// deterministic tree solver — a species' parameters and a seed in, a mesh out — that makes the
// engine's vegetation instead of modelled or image-to-3D assets, which E10 found lose their detail
// past a person's size (docs/experiments/e10-generated-props.md). Its first species is the date
// palm (palm.h). It owns the species' parameter types (schemas/flora.schema) and the solver; it
// does not own placement (a scatter layer or a scene's instances place what it makes), wind
// (roadmap S66), or the textures its materials will sample (G60).
//
// The registration points, ADR-0027 decision 2:
//
//   [x] schema types      schemas/flora.schema: engine.flora.Palm, engine.flora.FloraFile
//   [-] scheduler entry   none: the solver runs at content-build time and has no tick
//   [-] render passes     none: a palm is a cluster mesh like any other
//   [x] derived data      the solver's container, keyed by the parameters (flora.md, "The build")
//   [-] protocol methods  none yet
//   [-] tunables          none: everything the solver reads is a parameter of the file
//   [x] LOD policy        the cluster DAG's (flora.md, "LOD")
//   [x] determinism       the same parameters give the same bytes on every toolchain (palm.h)
//   [x] zero cost unused  nothing runs unless a mesh names a `.flora.json`
//   [x] docs, tests, size table
//   [-] bench             no hot path: the solver runs once per parameter set

#include <core/base/types.h>

namespace engine::flora {

// The solver's version: part of every build key over a flora file, so a change to what the solver
// makes from the same parameters — a new rung on the budget's ladder, a different leaflet — is a
// new key and never an old container read as the answer. Bump it with any change that moves a
// byte of the default palm's mesh (the determinism test says when one did).
inline constexpr u32 k_flora_version = 1;

// The species `engine.flora.FloraFile.species` names.
inline constexpr const char* k_species_date_palm = "date_palm";

}  // namespace engine::flora
