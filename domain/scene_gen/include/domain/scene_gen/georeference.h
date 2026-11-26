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

// The map coordinate of a world position, and the world position of a map coordinate. Pure, and
// meaningful only for a block `validate_georeference` accepts.
//
// **The round trip from the map is exact** for a metre system (`unit_m` 1) wherever a coordinate is
// within a factor of two of the origin's on its axis, which every point of a scene on a national
// grid is (Sterbenz: the difference is exact, and adding it back gives the coordinate): the bits a
// map coordinate starts with are the bits it comes back with. From the world the round trip is
// within half an ulp of the map coordinate's magnitude (0.5 nm at five million metres).
MapCoord map_from_world(const scene::Georeference& g, WorldPos p) noexcept;
WorldPos world_from_map(const scene::Georeference& g, MapCoord m) noexcept;

}  // namespace engine::scene_gen
