// Size table for domain/landform (ADR-0019). The solver keeps nothing resident and mirrors nothing
// on the GPU; what it lays over bytes on disk are raw grids of u8, u32 and i32. The one type worth
// pinning is the result a host holds while it writes the files: six arrays, the lattice and the
// routing seed.
#include <core/base/size_table.h>
#include <domain/landform/landform.h>

using namespace engine;

ENGINE_EXPECT_SIZE(168, 8, landform::Landform);
