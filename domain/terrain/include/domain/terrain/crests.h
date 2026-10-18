#pragma once

// **A tile's crest lines and wind, and nothing else** (docs/subsystems/terrain.md, "What the effect
// needs"): the read-only query a renderer's blowing-sand pass makes for every tile round the eye
// (systems/volumetrics' crest plumes). `evaluate_tile` answers the same crest lines and wind, but
// with the tile's whole grid of heights and normals, and a gather and an apron allocated on every
// call; this answers them alone, into a scratch the caller keeps, so once the scratch has grown a
// call allocates nothing — which a frame's pass needs.
//
// **The same numbers as `evaluate_tile`, bit for bit**: the crest lines are the gathered
// primitives' crests in the gather's order with the same fixed-point arithmetic, and the wind is
// `WindRecord::wind_at`'s with the saltation share counted over the tile's (cells + 1)^2 vertices'
// materials, as the tile does. systems/volumetrics/tests/plume_tests.cpp holds the two equal at
// several tiles and times.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/tile.h>

namespace engine::terrain {

struct CrestScratch {
  Gather gather;
};

// The crest lines and wind `evaluate_tile(field, tile, time_us, options, lag, nullptr, out)` writes
// into `out.crests` and `out.wind` (the crests only when `options.crests`). `crests` is cleared and
// refilled, keeping its capacity.
void tile_crests(const DuneField& field, TileCoord tile, i64 time_us, const TileOptions& options,
                 const LagField* lag, CrestScratch& scratch, Vector<CrestLine>& crests,
                 TileWind& wind);

}  // namespace engine::terrain
