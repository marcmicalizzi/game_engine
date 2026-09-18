// Size table for domain/geometry (ADR-0019). ClusterDesc is read by the GPU through a device
// address, so its layout is part of the shader contract as well as the footprint budget.
#include <core/base/size_table.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterDesc);

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterLodDesc);

// A page descriptor travels to the GPU with the cluster it names, so its layout is the streaming
// contract as well as the footprint: twelve words, no padding, a multiple of sixteen bytes.
ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterPageDesc);

// One page request, which the cull pass will write to a feedback buffer once §4.9's GPU half
// exists: three words, so a lane emits one and the CPU reads it back without a conversion.
ENGINE_EXPECT_SIZE(12, 4, geometry::PageRequest);

ENGINE_EXPECT_SIZE(8, 4, geometry::VertexAttributes);

// The skin binding is the same eight bytes a vertex as the attributes, and it is read by the
// deform pass through a device address, so its layout is the shader contract too: four u8 joint
// indices then four u8 weights, no padding, alignment 1 so the stream is a plain byte array.
ENGINE_EXPECT_SIZE(8, 1, geometry::SkinBinding);

// The .clusters container: these three are the file itself, so their size is the format.
ENGINE_EXPECT_SIZE(32, 8, geometry::ClusterFileHeader);

ENGINE_EXPECT_SIZE(24, 8, geometry::ClusterFileSection);

ENGINE_EXPECT_SIZE(32, 4, geometry::ClusterFileScalars);

ENGINE_EXPECT_SIZE(64, 4, geometry::ClusterFileMaterial);
