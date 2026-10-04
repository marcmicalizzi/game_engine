// Size table for domain/gfx (ADR-0019). These are the blocks the GPU reads: push constants, and
// parameter and descriptor structs behind device addresses, so their layouts are part of the
// shader contract as well as the footprint budget. Changing one means changing its mirror in
// every shader that reads it.
#include <core/base/size_table.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/ground_detail.h>
#include <domain/gfx/path_trace.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>
#include <domain/gfx/sky.h>
#include <domain/gfx/visibility_resolve.h>

#include <type_traits>

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
// 112, not 104: the terrain stage's table of levels (`terrain`), null for a scene with none, so
// every other scene's pool pass runs the instructions it ran before (renderer.md, "The dunes in
// time-lapse").
ENGINE_EXPECT_SIZE(112, 8, gfx::DeformParams);

// A terrain level's height field and its frame (gfx.md, "The deform chain"; renderer.md, "The dunes
// in time-lapse"): 24 bytes a field — the heights' address, the window's lattice origin and its
// size — and 96 a level: two fields, the lattice's origin and spacing, the blend, the padding its
// spheres take while its fields have moved away from its rest heights, and the square the next
// finer level draws. Written per frame into a host-visible table the cull pass and the pool pass
// read through one address each.
ENGINE_EXPECT_SIZE(24, 8, gfx::TerrainField);
ENGINE_EXPECT_SIZE(96, 8, gfx::TerrainLevelDesc);

// 72: the allocator's push block. One dispatch per run of the visible list, one workgroup, every
// view inside it — so it carries the run and the view count rather than a per-view block.
ENGINE_EXPECT_SIZE(72, 8, gfx::DeformAllocParams);

// Unchanged at 96: `deform` took one of the three pad words and `bounds_padding` a second, so a
// deformed instance can say how far its vertices leave their rest positions without the record
// growing. `terrain` took the last: the frame's terrain level an instance draws, for the cull
// pass's hole test and the pool pass's terrain stage. None is left.
ENGINE_EXPECT_SIZE(96, 4, gfx::InstanceDesc);

ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterDrawParams);

// 128, not 120: the emit pass reads the MeshDesc array to find a deformed instance's pool and a
// cluster's template, and `instantiate` replaced the pad word. Still 128 with multi-view: `views`
// took over the word that held the CLAS set's capacity, because a visible run's count is bounded
// by `pair_count` anyway (docs/plan/04-renderer.md §4.6).
ENGINE_EXPECT_SIZE(128, 8, gfx::ClusterRecordParams);
// tlas_references.slang's push block: two addresses and the count (2026-10-04).
ENGINE_EXPECT_SIZE(24, 8, gfx::TlasReferenceParams);

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
// 480, not 464: the terrain levels' table (`terrain`), so a cluster of a terrain level wholly
// inside the square a finer level draws is dropped before any test that costs; eight bytes of pad
// keep the block a whole number of float4 rows.
ENGINE_EXPECT_SIZE(480, 8, gfx::CullParams);

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
// Still 304 with cascaded shadow maps: `shadow_maps`, the address of the block below, took the pad
// word behind `pairs`. It is zero for every frame that draws no maps.
// 320, not 304: `ground`, the ground's albedo, beside `sky` and `sun` (2026-09-25). The resolve's
// hemisphere ambient took a lower half of its own — the ground lit by the frame's sun and sky —
// so a face turned towards the ground is lit by it (docs/subsystems/renderer.md, "The sky above,
// the ground below"). Three floats with no pad word left to take them, so the block grew a row.
// 336, not 320: `ground_detail`, the address of the ground's detail block below (2026-09-29), and a
// pad word to keep the block a whole number of float4 rows. Zero for every frame that draws none.
// Still 336 with the sky (2026-09-30): `sky_params` took that pad word and `sky_view` the one
// beside `coverage_pitch`.
// 352, not 336: `dither_steps`, the colour target's code steps the output encode dithers by
// (2026-10-04, display.h, ADR-0052), with no pad word left to take it, and three pad words to keep
// the block a whole number of float4 rows. Zero is no dither, the encode as it was.
ENGINE_EXPECT_SIZE(352, 8, gfx::ResolveParams);

// The sky (sky.h; docs/subsystems/gfx.md, "The sky"): the air, the lights, the eye, the celestial
// frame, the exposure, the tables' addresses, and a view's inverse projection and pixel angle for
// each of up to eight views — one block a frame, read by the tables' passes, the resolve and the
// reference. The frame's sums the sky pass writes are nine ambient coefficients and four rows.
ENGINE_EXPECT_SIZE(80, 4, gfx::SkyView);
ENGINE_EXPECT_SIZE(1008, 8, gfx::SkyParams);
ENGINE_EXPECT_SIZE(208, 4, gfx::SkyFrame);

// The ground's detail (ground_detail.h, docs/subsystems/gfx.md "The ground's detail"): the wind,
// the ripples' shape and the kernels' lattice, the filter's slope variance, the slope fade and the
// grain, read through an address once per shaded pixel of a material that carries it. One block a
// frame, shared by every view and by the reference.
// 560, not 224 (2026-10-04, gfx.md "Far from the origin"): the frame the pattern is evaluated in —
// its origin near the eye, and where the ripples', the grain's, the patches' and the streaks'
// lattices stand at it (16 bytes each), then the grainflow's sixteen lane directions (16 each) —
// so no lattice, hash or phase is ever taken of a float32 world coordinate, which has a 3.1 cm step
// 420 km out. The shader's struct is the first 304; it reads the one lane a pixel needs from the
// table behind it. Still one block a frame, and a pixel reads the 80 new head bytes and one lane.
ENGINE_EXPECT_SIZE(16, 4, gfx::GroundLattice);
ENGINE_EXPECT_SIZE(16, 4, gfx::GroundLane);
ENGINE_EXPECT_SIZE(560, 4, gfx::GroundDetailParams);

// The cascaded shadow maps (docs/subsystems/renderer.md, "Shadows"): per cascade its world-to-tile
// matrix and three numbers the filter and the bias need; per frame four cascades, the light's
// frame and the atlas. Read through `ResolveParams::shadow_maps`, once per shaded pixel, so it is
// a device-address block beside the resolve's own rather than more words in it.
ENGINE_EXPECT_SIZE(80, 4, gfx::ShadowCascade);
ENGINE_EXPECT_SIZE(400, 4, gfx::ShadowMapParams);

// 256: the reference path tracer's block (docs/plan/04-renderer.md §4.8). It is not in a frame
// path — one dispatch per batch of samples, minutes per picture allowed — so it carries all
// eleven scene addresses outright rather than packing them, and the padding keeps it a whole
// number of float4 rows like every other addressed block.
// 272, not 256: `ground`, the same albedo `ResolveParams::ground` carries, so an escaped ray below
// the horizon sees the ground the resolve's hemisphere term puts there (2026-09-25).
// 288, not 272: `ground_detail`, the resolve's block, which the reference draws unfiltered, and a
// pad (2026-09-29). Still 288 with the sky: `sky_params` took the pad (2026-09-30).
ENGINE_EXPECT_SIZE(288, 8, gfx::PathTraceParams);

// 112, not 64: the occlusion and emissive textures, the occlusion strength, a sampler for each
// slot (two 16-bit halves per word, the base colour keeping `sampler`) and one UV transform (a
// 2x2 matrix and an offset, KHR_texture_transform). The table is read per pixel through the
// cluster's index, so it is a handful of cache lines a frame whatever its size; one transform per
// material rather than one per slot is what kept it from 208 (gfx.md, "The material table").
ENGINE_EXPECT_SIZE(112, 4, gfx::ResolveMaterial);

ENGINE_EXPECT_SIZE(64, 4, gfx::ResolveLight);

// The RHI's own vocabulary (rhi.h, docs/subsystems/gfx.md "The RHI surface and the backend
// surface"). A handle carries the bits of the Vulkan handle it replaced, so it is that handle's 8
// bytes and nothing more, and every struct that holds one kept its size: a BufferResource and an
// ImageResource are the same 40 bytes the Vulkan-typed structs were, which is why no GPU scene or
// frame structure moved when the public headers stopped naming Vulkan. The small descriptions
// that reach a Vulkan call as they are (a buffer copy region, the indirect argument records the
// GPU reads) are pinned against Vulkan's own layouts in src/rhi_vulkan.cpp as well.
ENGINE_EXPECT_SIZE(8, 8, gfx::BufferHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::ImageHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::ImageViewHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::SamplerHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::PipelineHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::AccelerationStructureHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::SemaphoreHandle);
ENGINE_EXPECT_SIZE(8, 8, gfx::CommandList);
ENGINE_EXPECT_SIZE(40, 8, gfx::BufferResource);
ENGINE_EXPECT_SIZE(40, 8, gfx::ImageResource);
ENGINE_EXPECT_SIZE(16, 8, gfx::ComputePipeline);
ENGINE_EXPECT_SIZE(24, 8, gfx::BufferCopy);
ENGINE_EXPECT_SIZE(16, 4, gfx::DrawIndirectArgs);
ENGINE_EXPECT_SIZE(20, 4, gfx::DrawIndexedIndirectArgs);
ENGINE_EXPECT_SIZE(12, 4, gfx::DispatchIndirectArgs);
static_assert(std::is_trivially_copyable_v<gfx::BufferHandle> &&
              std::is_trivially_copyable_v<gfx::CommandList> &&
              std::is_trivially_copyable_v<gfx::BufferResource>);
