// Size table for domain/landform (ADR-0019). The solver keeps nothing resident and mirrors nothing
// on the GPU; what it lays over bytes on disk are raw grids of u8, u32 and i32. The one type worth
// pinning is the result a host holds while it writes the files: eight arrays (the sediment's and
// the resistance's joined the six with deposition, W95 stage B), the lattice, the routing seed and
// the run's budget of eight volumes.
#include <core/base/size_table.h>
#include <domain/landform/landform.h>

using namespace engine;

ENGINE_EXPECT_SIZE(264, 8, landform::Landform);
