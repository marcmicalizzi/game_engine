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

// 48, up from 16, because the deformer became a **chain of stages** rather than one exclusive
// mode (gfx.md, "The deform chain"). What was added is exactly what is per *instance*: the stage
// mask took over the kind word, `weights` is the morph weights of the two morph stages (one array
// with the static half first, because two addresses would cost eight bytes on every deformed
// instance to say the same thing), `cache` is where the static stage's kept result lives, and
// `first_channel`/`channel_count`/`first_vertex` are what index the weights and the cache. What
// is per *mesh* — the channel records, the slice directory, the delta streams — is deliberately
// **not** here and not in `MeshDesc` either, which is full at 64 bytes and which every position
// read in the renderer holds: those are scene-wide arrays behind `DeformParams::morph`. One pad
// word is left, which is what a fifth stage's address would take.
ENGINE_EXPECT_SIZE(48, 8, gfx::DeformDesc);

// 16: what the static shape stage keeps per vertex of a cached instance's mesh — the displaced
// position and its octahedral normal. The normal is in it because a cache of positions alone
// would leave the stage's morph scan running every frame for the normals, which is the cost the
// cache exists to remove; sixteen bytes is also one aligned load rather than two.
ENGINE_EXPECT_SIZE(16, 4, gfx::DeformCacheVertex);

// 48: the scene's morph stream, one block behind `DeformParams::morph` the way `StreamParams`
// sits behind `CullParams::streaming`. Six addresses, read once per workgroup rather than once
// per vertex, which is why they are an indirection and the pool's own addresses are not.
ENGINE_EXPECT_SIZE(48, 8, gfx::MorphParams);

// 16: the frame's pool allocator, one record in a device buffer. Four counters, and it is copied
// into the per-slot statistics block and read one frame late like the visible counts.
ENGINE_EXPECT_SIZE(16, 4, gfx::DeformAlloc);

// 104, not 88: the chain gained the morph stream's block address and the deformed normal pool's.
// Both are null for a scene with no morph channels, so the pass then runs the instructions it ran
// before them. The 128-byte push limit is why the morph stream is one address and not six.
ENGINE_EXPECT_SIZE(104, 8, gfx::DeformParams);

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
// 432, not 416: the shadow casters' run and its counter (docs/subsystems/geometry.md, "Normal
// cones"). Both null keep the cone test dropping what it rejects, which is every caller but the
// renderer with ray-traced shadows on, so every picture without shadows is byte-identical.
// 448, not 432: the vertex path's indexed draw — the run's header the cull allocates each hardware
// survivor's triangles under, and the records it writes the survivor to (gfx::VertexDrawHeader).
// Only `cull_vertex_main` reads them; `cull_main` is compiled without that code, which is why the
// mesh path's cull costs what it did (docs/experiments/e1-pascal-rerun.md, "After").
// 464, not 448: the visibility id became the scene's pair instead of the visible entry, so that a
// depth tie is settled by the scene's order and not by the append's (gfx.md, "The tie rule"), and
// the resolve needs the way back: `pair_entries`, this view's pair-to-entry table the pass writes
// for every pair it draws, and the two run bases an entry is counted from.
ENGINE_EXPECT_SIZE(464, 8, gfx::CullParams);

// The vertex path's indexed draw, per run: the header is the draw's, the fallback's and the
// expansion's indirect arguments in one aligned block; a record is the entry the vertex stage reads
// instead of the visible list; the expansion's push block is five addresses.
ENGINE_EXPECT_SIZE(64, 4, gfx::VertexDrawHeader);
ENGINE_EXPECT_SIZE(16, 4, gfx::VertexDrawRecord);
ENGINE_EXPECT_SIZE(40, 8, gfx::VertexExpandParams);

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
// 288, not 272: `normal_pool` is the deformed shading normals the deform chain writes beside the
// positions, and it is an address because the resolve reads it per covered pixel. It is zero for
// every frame with no morph channels, and the shader then reads the rest attribute stream exactly
// as it did, which is what keeps those pictures byte-identical.
// 304, not 288: the visibility id became the scene's pair (docs/subsystems/gfx.md, "The tie
// rule"), so the resolve decodes it through the scene's pair table (`pairs`, one load, as
// `visible[entry]` was) and reaches the entry through the view's pair-to-entry table
// (`pair_entries`) only for a deformed instance's pool block. The old pad word went to
// `pair_entries`; `pairs` added the sixteen bytes after it.
ENGINE_EXPECT_SIZE(304, 8, gfx::ResolveParams);

// 256: the reference path tracer's block (docs/plan/04-renderer.md §4.8). It is not in a frame
// path — one dispatch per batch of samples, minutes per picture allowed — so it carries all
// eleven scene addresses outright rather than packing them, and the padding keeps it a whole
// number of float4 rows like every other addressed block.
ENGINE_EXPECT_SIZE(256, 8, gfx::PathTraceParams);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);
