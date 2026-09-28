// Size table for domain/terrain (ADR-0019). The hot types are the ones a tile evaluation walks per
// point and the ones the overlay stores: a gather's primitives (every point of a tile tests every
// one), the wind record's days (read per evaluation, 2,920 of them per field), a stamp (its stored
// form is its size), a tile's lag (four bytes of every tile's persistent state), and a crest line
// (a renderer's pass reads a few dozen a tile).
#include <core/base/size_table.h>
#include <domain/terrain/terrain.h>

using namespace engine;

// Centre (16), direction (8), height, half length, stoss, bend, reach, side, sharpness, lee (32),
// three reciprocals (12), the meander and its reciprocal (8), the cell's hash and the meander's
// phase (4), band and kind (2), and six bytes of padding. It grew from 56 when the reciprocals
// moved the inner loop's divisions into the gather, and from 72 to 88 for the band table's
// sinuous crests (terrain.md, "The band table"); a gather holds a few hundred.
ENGINE_EXPECT_SIZE(96, 8, terrain::Primitive);
// Flux x, z and magnitude, speed (16), direction and the calm flag (4).
ENGINE_EXPECT_SIZE(32, 8, terrain::WindDay);
// Time (8), centre (8), two radii and a yaw (6), depth and rim (4), kind and five spare bytes (6):
// exactly `k_stamp_bytes`, its record form.
ENGINE_EXPECT_SIZE(32, 8, terrain::Stamp);
// One lag unit per band and a spare byte.
ENGINE_EXPECT_SIZE(4, 1, terrain::TileLag);
// Five points (60), the lee (8), height, sharpness, celerity (12), the cell's hash, band, kind (4).
ENGINE_EXPECT_SIZE(84, 4, terrain::CrestLine);
