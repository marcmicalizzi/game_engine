// The terrain resolve with R136's four footprint rules built in and tiles by weight left out
// (terrain_material.slang, TERRAIN_FOOTPRINT_LEVERS): the rules the engine ships without, which
// terrain_footprint_tests.cpp holds to the near ground's bytes through a shader manifest.
#define TERRAIN_FOOTPRINT_LEVERS 15
#define MATERIAL_TERRAIN 1
#include "../../../../domain/gfx/shaders/visibility_resolve.slang"
