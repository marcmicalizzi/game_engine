// Size table for domain/gfx (ADR-0019). These are the blocks the GPU reads: push constants, and
// parameter and descriptor structs behind device addresses, so their layouts are part of the
// shader contract as well as the footprint budget. Changing one means changing its mirror in
// every shader that reads it.
#include <core/base/size_table.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>

using namespace engine;

// 64, not 32: a MeshDesc also says where a *deformed* instance's positions come from — the
// frame's deformed-vertex pool, the per-instance deform table, the mesh's cluster templates, and
// (the eighth word) its per-vertex skin bindings (docs/plan/04-renderer.md §4.3,
// docs/plan/05-simulation.md §5.11). The addresses ride on the mesh because the rasterizers'
// push block is full at its 128-byte limit and every position read already holds the MeshDesc;
// the binding stream in particular is per vertex, so it is the mesh's and not the instance's.
ENGINE_EXPECT_SIZE(64, 8, gfx::MeshDesc);

// 24, not 16: a skinned instance carries its own bone-matrix address and joint count, because a
// pose is per instance while the mesh and its bindings are shared by a whole crowd. The word
// that was `pad` became `joint_count`.
ENGINE_EXPECT_SIZE(24, 8, gfx::DeformDesc);

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
