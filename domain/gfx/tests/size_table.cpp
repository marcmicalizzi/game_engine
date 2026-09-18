// Size table for domain/gfx (ADR-0019). These are the blocks the GPU reads: push constants, and
// parameter and descriptor structs behind device addresses, so their layouts are part of the
// shader contract as well as the footprint budget. Changing one means changing its mirror in
// every shader that reads it.
#include <core/base/size_table.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/path_trace.h>
#include <domain/gfx/visibility_resolve.h>

using namespace engine;

// 64, not 32: a MeshDesc also says where a *deformed* instance's positions come from — the
// frame's deformed-vertex pool, the per-entry allocation table into it, the mesh's cluster
// templates, and (the eighth word) its per-vertex skin bindings (docs/plan/04-renderer.md §4.3,
// docs/plan/05-simulation.md §5.11). The addresses ride on the mesh because the rasterizers'
// push block is full at its 128-byte limit and every position read already holds the MeshDesc;
// the binding stream in particular is per vertex, so it is the mesh's and not the instance's.
// **Still 64 with the per-frame pool suballocation**: `deform_slots` took over the word that held
// the `DeformDesc[]` address, because a position reader now needs the visible entry's block base
// and not the instance's deformer.
ENGINE_EXPECT_SIZE(64, 8, gfx::MeshDesc);

// 16, down from 24: a skinned instance carries its own bone-matrix address and joint count,
// because a pose is per instance while the mesh and its bindings are shared by a whole crowd —
// and *where in the pool* is no longer the instance's at all. The per-frame suballocation gives a
// block to each visible entry, so `pool_offset` and `vertex_count` went away and only the pool
// pass reads this record now (docs/subsystems/renderer.md, "The deformed-vertex pool").
ENGINE_EXPECT_SIZE(16, 8, gfx::DeformDesc);

// 16: the frame's pool allocator, one record in a device buffer. Four counters, and it is copied
// into the per-slot statistics block and read one frame late like the visible counts.
ENGINE_EXPECT_SIZE(16, 4, gfx::DeformAlloc);

// 88, not 80: the pool pass gained the per-entry allocation table's address and the run's first
// entry in it, because `visible` points at one run and the table spans the whole list.
ENGINE_EXPECT_SIZE(88, 8, gfx::DeformParams);

// 72: the allocator's push block. One dispatch per run of the visible list, one workgroup, every
// view inside it — so it carries the run and the view count rather than a per-view block.
ENGINE_EXPECT_SIZE(72, 8, gfx::DeformAllocParams);

// Unchanged at 96: `deform` took one of the three pad words and `bounds_padding` a second, so a
// deformed instance can say how far its vertices leave their rest positions without the record
// growing. One pad word is left.
ENGINE_EXPECT_SIZE(96, 4, gfx::InstanceDesc);

ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterDrawParams);

// 128, not 120: the emit pass reads the MeshDesc array to find a deformed instance's pool and a
// cluster's template, and `instantiate` replaced the pad word. Still 128 with multi-view: `views`
// took over the word that held the CLAS set's capacity, because a visible run's count is bounded
// by `pair_count` anyway (docs/plan/04-renderer.md §4.6).
ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterRecordParams);

// 416, not 400: geometry streaming appended the address of its own block plus the two counts that
// bound every index the shader writes — the page table's length and the request buffer's capacity
// (docs/plan/04-renderer.md §4.9, docs/subsystems/gfx.md). The address is null for a scene that is
// uploaded whole and the pass then runs the instructions it always ran, which is why a
// non-streamed picture is byte-identical; the two counts went beside it rather than into
// `StreamParams` because a bound has to be readable before the block it bounds is dereferenced.
ENGINE_EXPECT_SIZE(416, 8, gfx::CullParams);

// 64: eight addresses, no counts — the page table and the per-cluster tables the drawing rule
// reads, and the three feedback arrays it writes. It is read through a device address rather than
// pushed, because `CullParams` is itself addressed and one more indirection costs one load per
// dispatch while eight more words in the pushed block would cost every pass that never streams.
ENGINE_EXPECT_SIZE(64, 8, gfx::StreamParams);

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

// 256: the reference path tracer's block (docs/plan/04-renderer.md §4.8). It is not in a frame
// path — one dispatch per batch of samples, minutes per picture allowed — so it carries all
// eleven scene addresses outright rather than packing them, and the padding keeps it a whole
// number of float4 rows like every other addressed block.
ENGINE_EXPECT_SIZE(256, 8, gfx::PathTraceParams);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);
