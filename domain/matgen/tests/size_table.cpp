// Size table for domain/matgen (ADR-0019): the texel a build walks (one per texel of the set
// while it is built: 84 MB at 2,048 squared, freed when the four images are written) and the
// window a build starts at.
#include "../src/field.h"

#include <core/base/size_table.h>
#include <domain/matgen/matgen.h>

using namespace engine;

ENGINE_EXPECT_SIZE(20, 4, matgen::detail::Texel);
ENGINE_EXPECT_SIZE(8, 4, matgen::Window);
