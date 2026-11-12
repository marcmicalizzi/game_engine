// Size table for domain/landform (ADR-0019). The solver keeps nothing resident and mirrors nothing
// on the GPU; what it lays over bytes on disk are raw grids of u8, u32 and i32. The one type worth
// pinning is the result a host holds while it writes the files: eight arrays (the sediment's and
// the resistance's joined the six with deposition, W95 stage B), the lattice, the routing seed and
// the run's budget of eight volumes. Stage C (W104) added where the solve's seconds went, eight
// doubles, and the count of the network's levels (336 bytes), then the divided discharge and the
// channel's width, two arrays, and the count of the banks lateral erosion took (376 bytes), then
// the ice and the till, two arrays more, with the count of the ice's steps beside the banks': 408.
// Stage F (W127) added what the ice leaves, five arrays (the thickest ice that ever stood, the
// moraines and their crests, a lake's fill, the rock's floor) and three volumes of the budget
// (the ice's load delivered, laid as ridges, lodged): 512.
#include <core/base/size_table.h>
#include <domain/landform/landform.h>

using namespace engine;

ENGINE_EXPECT_SIZE(512, 8, landform::Landform);
