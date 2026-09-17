// Size table for domain/geometry (ADR-0019). ClusterDesc is read by the GPU through a device
// address, so its layout is part of the shader contract as well as the footprint budget.
#include <core/base/size_table.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterDesc);

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterLodDesc);

ENGINE_EXPECT_SIZE(8, 4, geometry::VertexAttributes);

// The .clusters container: these three are the file itself, so their size is the format.
ENGINE_EXPECT_SIZE(32, 8, geometry::ClusterFileHeader);

ENGINE_EXPECT_SIZE(24, 8, geometry::ClusterFileSection);

ENGINE_EXPECT_SIZE(32, 4, geometry::ClusterFileScalars);

ENGINE_EXPECT_SIZE(64, 4, geometry::ClusterFileMaterial);
