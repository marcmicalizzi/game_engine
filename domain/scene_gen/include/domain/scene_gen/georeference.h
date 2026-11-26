#pragma once

// Where on the Earth a scene is (W186; docs/subsystems/scene_gen.md, "Where on the Earth a scene
// is"): the scene's `georeference` block (`engine.scene.Georeference`), its form checked and its
// one mapping each way between a world position and a map coordinate in the scene's own systems.
//
// **Carried, not interpreted.** The names of the systems are strings the engine never reads: no
// projection, no datum shift, no lookup of a code, no geodesy library. The mapping is an origin,
// two signed engine axes for east and north, the engine's up for the vertical system's, and the
// horizontal unit's length in metres, all in f64. A tool that needs a geographic position takes
// the map coordinate this gives to a library of its own.

#include <core/base/types.h>
#include <core/math/world.h>

#include <schemas/scene.h>
#include <string>

namespace engine::scene_gen {

// A coordinate in the scene's own systems: easting and northing in the horizontal system's unit,
// height in metres in the vertical system. Named by meaning, never by the system's axis order.
struct MapCoord {
  f64 easting = 0.0;
  f64 northing = 0.0;
  f64 height = 0.0;
  constexpr bool operator==(const MapCoord&) const = default;
};

// Whether the block is well formed: both systems named, every number finite, the unit positive,
// east and north both stated and on two different engine axes. False with a sentence in `why`.
bool validate_georeference(const scene::Georeference& g, std::string* why);

// Whether the block's engine axes are the map's mirror image: east x north points down (-y) rather
// than up. Of the eight valid pairs four are rotations (east +x north -z, east -z north -x, east -x
// north +z, east +z north +x) and four mirrors. A mirrored world is the map's reflection: a tool that
// writes (easting, height, northing) into (x, y, z) makes one, and must also reverse its triangles'
// winding, since the engine's front faces are counter-clockwise; nothing in the engine (a sky's
// azimuth, for one) corrects for it.
bool georeference_mirrored(const scene::Georeference& g) noexcept;

// The map coordinate of a world position, and the world position of a map coordinate. Pure, and
// meaningful only for a block `validate_georeference` accepts. Heights are metres whatever the
// vertical system's unit: the block names that system and its unit is not applied.
//
// **From the map, for a metre system (`unit_m` 1):** an easting or a northing within a factor of
// two of the origin's (every point of a scene on a national grid) comes back to its bits — the
// difference is exact (Sterbenz) and adding it back gives the coordinate. A height comes back to
// its bits within a factor of two of `origin_height`, or when that is 0; any other comes back
// within one ulp of |height| + |origin_height|, since the difference and the sum each round. A foot
// grid's divide and multiply round once each, and its round trip is not claimed to return its bits.
// **From the world:** within half an ulp of the map coordinate where that is within a factor of two
// of the origin (at most 2^-31 m below 2^23 m, 2^-30 m to 2^24 m: a southern UTM northing), and
// within one ulp of |coordinate| + |origin| otherwise (a height, a local grid's origin near 0).
MapCoord map_from_world(const scene::Georeference& g, WorldPos p) noexcept;
WorldPos world_from_map(const scene::Georeference& g, MapCoord m) noexcept;

}  // namespace engine::scene_gen
