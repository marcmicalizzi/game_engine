// Size table for domain/gfx (ADR-0019). These are the blocks the GPU reads: push constants, and
// parameter and descriptor structs behind device addresses, so their layouts are part of the
// shader contract as well as the footprint budget. Changing one means changing its mirror in
// every shader that reads it.
#include <core/base/size_table.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>

using namespace engine;

ENGINE_EXPECT_SIZE(32, 8, gfx::MeshDesc);

ENGINE_EXPECT_SIZE(96, 4, gfx::InstanceDesc);

ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterDrawParams);

ENGINE_EXPECT_SIZE(400, 8, gfx::CullParams);

ENGINE_EXPECT_SIZE(40, 8, gfx::HizParams);

ENGINE_EXPECT_SIZE(208, 8, gfx::ResolveParams);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);
