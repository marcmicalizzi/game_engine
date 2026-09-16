// Size table for domain/geometry (ADR-0019). ClusterDesc is read by the GPU through a device
// address, so its layout is part of the shader contract as well as the footprint budget.
#include <core/base/size_table.h>
#include <domain/geometry/cluster.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 4, geometry::ClusterDesc);
