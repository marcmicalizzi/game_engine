#pragma once

// The terrain capability (ADR-0027; ADR-0043, proposed; docs/subsystems/terrain.md; plan 05 §5.13,
// 07 §7.6, 13 §13.1): **the desert's procedural terrain generator**. The endless desert's dunes as
// a pure function of (world seed, tile, game time) — parametric dune primitives in lattices the
// seeded wind record carries downwind (dunes.h, wind.h) — and, beside it, the only state the player
// can add to it: a per-tile deformation overlay of fixed size that the wind fills back in
// (overlay.h), and a saturating per-tile lag that is the one way large obstacles may feed back into
// the dunes (feedback.h). A tile of it at a time, with its crest lines, wind and sand flux for a
// renderer's blowing-sand pass, and a sampler that answers exactly what the tile's mesh has
// (tile.h).
//
// It owns the field, the wind record, the overlay's rules and format, and the lag. It does not own
// what a tile is to the world (the world capability's consumer builds a tile's field when the ring
// activates it and keeps its overlay in the store), what the renderer draws (the scene reader
// serves its `TerrainSampler` from this when a scene's terrain names the generator), or any GPU
// work: the deformation map's refinement and the blowing sand are the renderer's, from the outputs
// here.
//
// **Why `domain`, and why "terrain".** It is pure CPU arithmetic over core modules, read by the
// renderer (L3), the world (L3) and the content tool (L4), and it must not depend on any of them —
// the ruins capability is its closest sibling and sits here for the same reason. It is called
// terrain rather than desert because what it owns is the ground's function of time and the ground's
// deformation, which snow and mud will want (plan 05 §5.13) with other primitives and other rules;
// the dunes are its first field, not its name.
//
// Scaffolded by tools/new-capability.ps1. ADR-0027 decision 2, and where each point stands:
//
//   [x] schema types      schemas/terrain.schema: the overlay's projection kind and the
//                         `engine-content terrain` report; the scene's fields in scene.schema
//   [-] scheduler entry   none: the field is a function, evaluated where a tile is built; the
//                         overlay advances where its tile is held (the world consumer)
//   [-] render passes     none here: the outputs are what a future pass reads (terrain.md); the
//                         terrain rings (rings.h) are cluster meshes the renderer draws like any
//                         other
//   [x] derived data      the field is `derived`: a tile is a function of its inputs (field_hash)
//   [-] protocol methods  none yet; `engine-content terrain` is the agents' surface
//   [x] tunables          terrain.overlay.*, terrain.feedback.* (below), terrain.rings.*
//   (rings.cpp) [x] LOD policy        `Detail` and `detail_for_distance` (dunes.h); the overlay is
//   the coarse
//                         CPU grid of 05 §5.13
//   [x] determinism       k_determinism below
//   [x] zero cost unused  no linked code; a scene that does not name the generator reads as before
//   [x] docs, tests, size table, bench

#include <core/base/types.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/feedback.h>
#include <domain/terrain/overlay.h>
#include <domain/terrain/rings.h>
#include <domain/terrain/stats.h>
#include <domain/terrain/tile.h>
#include <domain/terrain/wind.h>

namespace engine::terrain {

// Determinism stance (ADR-0010): `derived`. The base field is a function of its description and
// the game time, integer throughout, the same bits on every toolchain and at either CPU baseline
// (golden hashes in tests/terrain_tests.cpp), and nothing about it is stored. The overlay is
// persistent state the world keeps in the store, where the persistent-state hash sees it; its
// evolution is integer too and independent of when it is advanced (overlay.h).
inline constexpr const char* k_determinism = "derived";

// The overlay's and the feedback's rules from the tunables (`terrain.overlay.*`,
// `terrain.feedback.*`), read once by whoever holds tiles. A record written under other rules is
// refused on read (the rules' hash is in its header), so a change to one of these is a change to
// the world, not a quiet reinterpretation of a save.
OverlayRules overlay_rules_from_tunables() noexcept;

}  // namespace engine::terrain
