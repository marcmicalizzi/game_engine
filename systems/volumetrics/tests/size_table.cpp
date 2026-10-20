// Size table for systems/volumetrics (ADR-0019): the GPU-mirrored blocks shaders/medium.slang reads
// through device addresses, so a field added on one side and not the other fails the build rather
// than the picture.
#include <core/base/size_table.h>
#include <systems/volumetrics/volumetrics.h>

using namespace engine;

ENGINE_EXPECT_SIZE(512, 8, volumetrics::MediumParams);
// One fire of the frame (R85): seven vectors and its ids, as `MediumFire` in the shader.
ENGINE_EXPECT_SIZE(128, 4, volumetrics::MediumFire);
