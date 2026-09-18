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
// cluster's template, and `instantiate` replaced the pad word. Still 128 with multi-view: `views`
// took over the word that held the CLAS set's capacity, because a visible run's count is bounded
// by `pair_count` anyway (docs/plan/04-renderer.md §4.6).
ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterRecordParams);

ENGINE_EXPECT_SIZE(400, 8, gfx::CullParams);

// 48, not 40: `coverage` was appended when the resolve started skipping 32 x 32 tiles with
// nothing in them. The mask is this pass's by-product — the from_visibility dispatch reads every
// word of the visibility buffer and its workgroup is exactly one tile — so its address has to
// reach the shader, and every other word of the block is in use. The block is push constants and
// is nowhere near the 128-byte limit. Folding six mips per dispatch did *not* change the size:
// the destination mips are contiguous behind the source, so the shader walks their offsets the
// way hiz_layout() does instead of being handed one source and one destination.
ENGINE_EXPECT_SIZE(48, 8, gfx::HizParams);

// 256, not 224: one resolve runs per view of a `renderer::ViewSet`, so the block carries that
// view's rectangle of the color target — the origin it takes off SV_Position and the extent it
// bounds against, four words where one pad word used to be — and the three Panini parameters that
// resample a wide rectilinear source into the picture (docs/plan/04-renderer.md §4.6). Every one
// of them is zero for a single rectilinear view, and the shader's arithmetic then reduces to what
// it was, which is why the single-view pictures are byte-identical.
//
// 272, not 256: `sky_is_clear` took one of the two remaining pad words — the pass clears the
// target to `sky`, so an empty pixel is a fragment that would write what is already there and is
// discarded instead — and `coverage` plus its pitch added the sixteen bytes after it, which is
// the address of the Hi-Z build's per-tile mask and the only way to skip *reading* a 64-bit
// visibility word for a pixel with nothing in it. Both are zero for a caller that fills neither,
// and the shader then writes the sky and reads every word, exactly as it always did.
ENGINE_EXPECT_SIZE(272, 8, gfx::ResolveParams);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);
