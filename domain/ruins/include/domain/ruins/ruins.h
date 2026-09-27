#pragma once

// ruins capability (ADR-0027; docs/subsystems/ruins.md; plan 07 §7.6, the direction note of
// 2026-09-24 "ruined desert structures are assembled from the kit, never one asset").
//
// A seeded assembler that turns a kit — wall sections, corners, openings and debris, described as
// data beside their meshes — into ruined buildings: a footprint grammar lays the walls at right
// angles with a corner member at every join, a ruin rule brings walls down by their exposure,
// openings go where the wall still stands, debris settles beside the walls, and each wall declares
// the sand drift it asks the terrain for. The output is instances of the kit's meshes; the module
// builds no geometry, touches no GPU, and does not build the sand.
//
//   kit.h            the kit, converted to integer centimetres and Q10 once (read_kit_file)
//   assembler.h      (kit, world seed, tile, height query) -> instances, drifts, sites
//   blocks.h         the same building laid block by block from a block kit: the other
//                    representation, every block an instance of one of a dozen block meshes
//   fragment.h       either output as an engine.scene.Scene fragment (engine-content ruins)
//   synthetic_kit.h  a kit of boxes and a synthetic block kit at three fidelities, for the tests
//                    and the measurements
//
// Registration points (ADR-0027 decision 2), each wired or deliberately not needed; the docs page
// says why:
//
//   [x] schema types      engine.scene.RuinKit and friends in schemas/scene.schema, beside the
//                         Scatter they sit with
//   [-] scheduler entry   none: nothing ticks; a building is a function of its seed and tile
//   [-] render passes     none: the output is instances the renderer already draws
//   [x] scene generators  the placement generator "ruins" (scene_generator.h, ADR-0046): what the
//                         scene reader expands a scene's `ruins` entries through, and the world
//                         ring streams them through a tile at a time
//   [x] derived data      `engine-content ruins` writes a scene fragment for an authored place
//   [-] protocol methods  none yet
//   [-] tunables          none: every parameter is the kit's, and the kit is content
//   [x] LOD policy        detail_for_distance(): a far tile keeps its walls and drops its debris
//   [x] determinism       k_determinism below
//   [x] zero cost unused  a scene without a `ruins` entry never reaches this module; switched off,
//                         it is not linked
//   [x] docs, tests, size table, bench

#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/scene_generator.h>

namespace engine::ruins {

// Determinism stance (ADR-0010): "derived". A building is content made from its seed and tile,
// never state the simulation ticks; it is the ruin's *initial* state, and what happens to it
// afterwards is the persistent store's (plan 13 §13.1, plan 03 §3.5). It is also bit-for-bit
// reproducible on every toolchain, which is a stronger promise than "derived" needs, and the one
// the endless desert needs: a tile dematerialized and materialized again is the same ruin.
inline constexpr const char* k_determinism = "derived";

}  // namespace engine::ruins
