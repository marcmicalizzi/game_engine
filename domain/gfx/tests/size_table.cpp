// Size table for domain/gfx (ADR-0019). These are the blocks the GPU reads: push constants, and
// parameter and descriptor structs behind device addresses, so their layouts are part of the
// shader contract as well as the footprint budget. Changing one means changing its mirror in
// every shader that reads it.
#include <core/base/size_table.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>

using namespace engine;

// 56, not 32: a MeshDesc also says where a *deformed* instance's positions come from — the
// frame's deformed-vertex pool, the per-instance deform table, and the mesh's cluster templates
// (docs/plan/04-renderer.md §4.3). The three addresses ride on the mesh because the rasterizers'
// push block is full at its 128-byte limit and every position read already holds the MeshDesc.
ENGINE_EXPECT_SIZE(56, 8, gfx::MeshDesc);

ENGINE_EXPECT_SIZE(16, 4, gfx::DeformDesc);

ENGINE_EXPECT_SIZE(80, 8, gfx::DeformParams);

// Unchanged at 96: `deform` took one of the three pad words.
ENGINE_EXPECT_SIZE(96, 4, gfx::InstanceDesc);

ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterDrawParams);

// 128, not 120: the emit pass reads the MeshDesc array to find a deformed instance's pool and a
// cluster's template, and `instantiate` replaced the pad word.
ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterRecordParams);

ENGINE_EXPECT_SIZE(400, 8, gfx::CullParams);

ENGINE_EXPECT_SIZE(40, 8, gfx::HizParams);

ENGINE_EXPECT_SIZE(224, 8, gfx::ResolveParams);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);
