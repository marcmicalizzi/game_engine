# One residency manager for three page types (W47, a design, 2026-11-18)

**A design, nothing built, and nothing run.** The row was written while the batch's gate held the machine: every number here is read from a record that exists and is named where it is used (W51-2's reports of the forest's staging, [what the forest commits to start](start-commit-by-owner-2026-11-18.md)), or computed from one by a scratch script that only computes (the scene file and the report's mesh list, [what a place names](#what-a-place-names)), or is an estimate and says so. The published systems were read on this run through the fetch tool; what could not be read is marked. The decision it asks for is drafted at the end ([the decision this page asks for](#the-decision-this-page-asks-for)); its number is the coordinator's.

## The question

The owner, 2026-11-18: if the forest proof of concept were a game to be distributed, "there's no world in which a 60GB minimum commit to start ends up making sense, even on max settings", and larger, better looking games fit in much less. W51-2 measured the engine-side copy of that world the same day: 30.6 GB of commit at the peak of the load and 25.0 GB after a minute at 3840 × 2160 with traced primaries, shadows and the bounce; 24.0 and 21.0 GB at 1920 × 1080 rasterized. **The host's bytes follow the world's size and not the settings, and the whole region is loaded before the first frame.**

Four small stages are being built by other rows (the source images kept on the host, the generated meshes' streams, the traced chain's first capacity, the upload's double copy). They return what is held for nothing. **What is left after them is the part only residency fixes: what is on the device is everything the world holds, not what the camera can reach.** By W51-2's own table that remainder is larger than its closing sentence says: the four stages take the first frames from 27.5 to 15.0 GB (27.5 − 7.3 − 2.5 − 2.7) and the standing figure from 25.0 to 15.2 GB (25.0 − 7.3 − 2.5; the chain's first capacity is already gone from the minute's figure), of which **11.7 to 11.9 GB is the device's** and 3.3 to 3.8 GB the host's (the sampler's figure and the exit snapshot's). The page's "12.5 GB of commit, 10.5 on the device" subtracts the chain's 2.7 GB from a figure that had already lost it.

Roadmap W47, "one residency manager for three page types" ([04 §4.9](../plan/04-renderer.md#49-streaming-and-residency)), has been open since September. Since then the engine has grown the pieces one at a time, each with a bound of its own: geometry pages under `--stream` (R9), a tile's meshes by reference in a reserved room ([ADR-0061](../adr/0061-a-tiles-meshes-arrive-with-the-tile.md)), texture levels by a bound and a trim ([ADR-0065](../adr/0065-texture-levels-resident-by-bound-and-feedback.md), built and off), the traced chain's capacity ([ADR-0068](../adr/0068-the-traced-chain-holds-what-frames-asked-for.md)) and its cache. [Memory held by travel](streaming-by-travel.md) (D39) and [ADR-0063](../adr/0063-memory-is-held-under-one-budget-and-by-travel.md), proposed and not accepted, put a governor over those pools and an envelope round the observer. **This page is the layer under that governor**: what a unit of residency is for each of the three types, how one budget is shared between them, what a frame draws when a unit is not there, what the rays need that the rasterizers do not, and what a launch then commits. Where it differs from ADR-0063 it says so ([questions](#open-questions-for-the-owner), 1).

## The inventory: what is resident at the start, who made it, and what holds it

W51-2's two loads of the staging, standing after a minute (`GpuScene::place_bytes`, MiB; "top" is 4K traced with the bounce and micromaps, "low" 1080p rasterized with cascades). "Unit" is what the bytes are counted in; "streams today" is what already brings the class in and lets it go; "what holds it" is who keeps an index or an address into it, which is what an eviction has to respect.

**On the device.**

| Class | Top | Low | Made by | Unit | Streams today | What holds it |
|---|---:|---:|---|---|---|---|
| Geometry: cluster and LOD tables, 16-bit positions, attributes, triangles, and with the chain the float positions | 2,656 | 1,676 | `GpuScene::upload_geometry`, one set of buffers for the load's 13,064 meshes (3,933 scene meshes, 4,074 ring forms, 4,754 far-forest tiles, 302 waters) | 56.7 bytes a triangle top, about 36 low; 49.1 M triangles in 1,625,411 clusters at the top (the low load's form set differs a little) | Only under `--stream`, in a rigid scene read whole (R9's page pool, [renderer](../subsystems/renderer.md#geometry-streaming-the-gpu-half-of-pages-and-residency)); a tile's own meshes through the mesh room | Offsets baked into `geometry::ClusterDesc` and addresses every shader takes once; the chain's build records address the float buffer; the deformed-vertex pool indexes vertices scene-wide, which is why `--stream` refuses a deformed scene |
| Companions: 8-bit indices, skin, morphs, flora motion (359), coverage, the shadow proxy's table; with the chain 360 more | 721 | 361 | `upload_geometry`, `create_flora`, `create_ray_tracing` | per vertex, triangle or cluster, parallel to the geometry | No (the 8-bit indices ride a page under `--stream`) | Indexed by the scene-wide cluster and vertex index |
| Opacity micromaps | 369 | 0 | `GpuScene::bake_micromaps`, one blocking bake over the load's clusters | per triangle of an alpha-tested cluster | No | The cluster structures built with them; an admitted mesh has none ([renderer](../subsystems/renderer.md#meshes-that-come-and-go)) |
| Structures built once: the rigid forms' (352), the resident cache (530, of it 506 the slots) | 882 | 0 | `build_static_forms` at load, every form; `create_rt_cache`, a ceiling from the device | 8,832 bytes a cache slot (the driver's most); the forms packed to their size | The cache by use, least recently drawn first, under a fixed ceiling ([renderer](../subsystems/renderer.md#the-resident-cluster-structure-cache)); the forms never | Per-instance bottom-level records hold slot addresses; the top level holds the forms' addresses; every frame in flight shares the chain |
| The chain's per-frame part: sets, tables, bottom and top levels, scratch | 195 (2,977 at the first frames) | 0 | `make_rt_step`, `RtCapacity` | by the capacity | Yes, by what frames built (ADR-0068; the first capacity is W51-2's stage 3) | Rebuilt every frame |
| Textures: 5,440 built images, 462 MiB of it normal variance | 3,150 | 3,150 | one upload per distinct image at load | a level's bytes; a 128-texel tail is about 22 KB | The bound and the trim are built and off ([texture](../subsystems/texture.md#levels-resident-by-what-the-frame-samples)); a tile mesh's textures by reference (R147) | A bindless slot named by material records; the readers that name level 0 (the rays' mask tests, the micromap bake); the allocator keeps a trimmed image's block |
| Materials: the table (348), a material per cluster, the terrain's masks | 364 | 364 | `upload_materials` | per material and per cluster | No (the room has a material arena for admitted meshes) | `InstanceDesc`'s material base; the cluster-to-material table |
| Tiles: the terrain's ring slots (256 in one buffer) and fields, the arenas inside the geometry buffers (the rings' chunks, and a mesh room where a generator hands meshes), the staging ring | 1,215 | 865 | `create_terrain`, the room at `GpuScene::create` | a ring chunk's slot; a cluster of the arena at its worst case (2,632 bytes with the chain, 1,492 without) | Yes: slots round the eye; meshes by reference with a linger ([world](../subsystems/world.md#a-tiles-meshes-residency-by-reference)) | Reserved whether filled or not |
| The frame's own: instance and pair tables, the visible list's runs, draw arguments, the deformed-vertex pool, poses | 1,080 | 1,112 | `create_working_set` and the tables | the pair stride (684,483 pairs laid out, 400,960 of them ring slots) and the instances | Sized by the layout, not by place | Every pass of every frame |
| Outside `place_bytes`: the renderer's targets, the bounce's cache (144), the medium, the allocator's block slack | 1,113 | 883 | `SceneRenderer::create`, `Targets::create` | pixels, and fixed sizes | No | The frame |
| **Device-local blocks** | **11,744** | **8,411** | | | | |

Two residencies that exist held almost nothing in these loads: the flora's tail (the streamed trees' parts and poses, nearest first under a sixteenth of the device) stood at 2.3 MiB of its 2.1 GB budget with the camera still, and R9's page pool was off, since the scene deforms.

**What does not hold a payload, and what does.** A visibility id names (instance, cluster of its mesh, triangle) and never an address, so the pair table and every id in a frame survive a page's leaving and its return in another slot: the pair numbering pins a mesh's *cluster count*, which is its descriptors, and nothing of its bytes. What pins bytes is narrower: an offset a shader reads this frame (answered by retiring a slot for the frames in flight), a build record's vertex address (answered by a traced page carrying the builders' bytes), the deformed-vertex pool's scene-wide vertex index (E25's open item, stage 8), a structure's address in a per-instance record (answered by never moving a structure), and the two readers that reach past the frustum, the shadow casters and the bounce's rays, which pin nothing by address and are a question of priority ([the rays](#what-the-rays-need-resident-that-the-rasterizers-do-not)).

**On the host.** After a minute the process holds 13.7 GB that is not the driver's backing (top; 12.6 low):

| Class | MiB | Made by | Streams today | What holds it |
|---|---:|---|---|---|
| Each mesh's source image bytes, kept in `renderer::SourceMesh` | 7,330 | `load_scene` | No | Nothing after the textures are built: W51-2's stage 1, in hand |
| Generated meshes' container streams | 2,520 | `read_cluster_file_memory` | No | Nothing after the upload: stage 2, in hand |
| The merged geometry | 3,502 at the peak, 149 after | `merge_cluster_meshes` | Released after the upload (T138); the 149 are the cluster and LOD descriptors | The streamer patches a page's descriptors from them; collision keeps a copy of its own of what a proxy cuts |
| The scene's records: JSON trees (about 274), water (168), placement tiles' instances (68), authored instances (43), mesh records (20), the mask archive's index (20), terrain edits and motion (about 60) | about 650 | `load_scene`, `PlacementTiles::create` | The tiles' instances follow the rings; the rest is read whole | The plan a tile is built from; by region only when a world is ten times this one (D39's step 9) |
| Smaller sites: what `SourceMesh` keeps a mesh beyond its images (about 500), private bytes with no named owner (506), the device's host-visible blocks (128) | about 960 | | | |
| The C runtime's slack beyond live blocks; the executable and mapped files | about 1,650 + 400 | | | |

Collision is not in this table (the loads were offscreen with no walker): it is resident by its own ring of a tile and a half and reads its meshes from the derived-data cache on its own jobs ([world](../subsystems/world.md#a-tiles-meshes-residency-by-reference)). A document host's records are tile-partitioned on disk and materialized in the simulated rings; `engine-view` holds none.

**Three readings.** *The host has no residency problem left after the stages in hand*: about 3.8 GB at the top setting and 2.8 at the low one, against W51-2's proposed 8 and 6. What this design owes the host is to add nothing back. *On the device, 7.2 GB at the top and 5.2 at the low setting are classes bound to meshes and textures that are held whole* (geometry, companions, micromaps, the forms' structures, textures). *And 3.8 GB at the top and 3.2 at the low are sized by the settings and the layout* (the frame, the targets, the tiles' reserved room, the material table): no residency touches them, and at the low setting they are most of a 4.5 GB budget ([the budget](#the-budget-it-serves)).

### What a place names

W51-2's table could not say what share of the load a place names, because the scene holds everything. Two records that exist can: the staging's scene file (470,641 authored instances of 3,849 meshes, a world of 64 m tiles with rings at 1.25, 3, 10 and 20 tiles) and the report's list of every mesh with its clusters and triangles. A scratch script took, for a camera at four places, the distinct meshes with an instance inside each ring; counted the mesh itself in ring 0 and the ring's own form outside it ([a placement's form by ring](../subsystems/world.md#a-placements-form-by-ring)); and turned triangles into device bytes at the load's own 56.7 bytes a triangle. **Where the load built no form for a ring the script counts the whole source, so each figure is an upper bound.**

| Place (metres from the scene's origin) | Ring 0 (80 m) | Rings 1 to 3 (to 1,280 m) | All four rings | Share of the load's triangles |
|---|---:|---:|---:|---:|
| The origin (0, 0), the densest of the four | 262 MiB, 100 meshes | 770 MiB | 1,032 MiB | 38.9 % |
| The instances' centroid (−121, 631) | 82 MiB, 81 meshes | 728 MiB | 810 MiB | 30.5 % |
| (600, −900) | 174 MiB, 35 meshes | 305 MiB | 479 MiB | 18.1 % |
| A corner of the forest (−1,239, −1,493) | 62 MiB, 29 meshes | 201 MiB | 263 MiB | 9.9 % |

Beside them, wherever the eye is: the far forest's per-tile forms 156 MiB, the waters 164, the placement kits 6. So **the geometry the tiles in reach name is 0.59 to 1.36 GB of the 2.66 GB resident (22 to 51 %), counted by whole meshes**. The classes parallel to geometry (companions, micromaps, the forms' structures) are taken at the same share below; that is an assumption, since flora motion is heavier on the near trees.

Two things the records do not say, and the first stage measures. **How much of a named mesh a frame reaches**: after 1,200 standing frames the structure cache held 58,851 of its meshes' 1,243,216 clusters (4.7 %), but the swaying trees of the inner rings are not cached, so that is a floor on the question and not an answer. **Which textures a place names**: the load's 5,440 images are shared 147,184 times, species by species, so by-reference residency returns little there and the levels are what matter.

## What the published systems do

Read on this run through the fetch tool unless a row says otherwise; a summary returned by the tool was not checked against the raw page.

| Source | How far read | The shape or rule | What it buys this engine |
|---|---|---|---|
| Karis, Stubbe and Wihlidal, "Nanite: A Deep Dive", SIGGRAPH 2021 Advances in Real-Time Rendering | The streaming and compression slides with their notes, from the text R188's row extracted of the deck this morning | Fixed-size pages so the pool never fragments, filled at cluster granularity; a root page per mesh always resident so there is always something to draw; the whole culling hierarchy resident because it is small and lets a new object ask at once for every level it needs, where a streamed hierarchy would multiply the read latency by the levels; requests made during the cull as page ranges with a priority from LOD error, for the main view and for shadow views at a lower priority; requests re-emitted for resident pages to keep their priority fresh; the CPU adds missing dependencies, evicts low priority, installs and patches; a memory format that decodes at once and a disk format transcoded on arrival | The page pool this engine has is this design already (descriptors resident, offsets patched). What it adds here: **a second, smaller slot class for roots**, shadow views as a lower band, and "used" marks as the priority of what is resident |
| Epic Games staff on the Nanite streaming pool (developer forum thread on resizing the pool, staff replies of August and September 2026) | The thread, as a summary with quotes | Streaming pages are 128 KB and root pages 32 KB; the two pools share one maximum; a resize makes all geometry fall back to its root pages and stream again, with both allocations live for a moment; the refill is bounded by a maximum of page installs a frame and of pending pages; a pool pushed too low raises a quality scale (more pixels an edge) rather than thrashing; users should pick from a few sizes | **Pools grow and shrink by chunks, never by reallocation**; a per-frame install bound; a global quality scale as the answer to a budget the view does not fit; budgets as a few tiers |
| Epic Games, "Nanite Virtualized Geometry" (the page is titled for UE 5.9) | Whole page, as a summary with quotes | Geometry is streamed on demand so only visible detail is in memory; ray tracing uses a fallback mesh by default, native tracing of Nanite data is experimental | This engine traces the cut it draws ([renderer](../subsystems/renderer.md#one-cut-for-every-view)); the fallback-mesh route is what R94 measured and declined |
| Epic Games, "Nanite technical details" | **Not read**: the page returned no body | | |
| Epic Games, "Texture Streaming Configuration" (UE 5.9) | Whole page, as a summary with quotes | One pool in megabytes for every texture, the ones that never stream included; the pool may be limited to the card's memory; a wanted resolution per texture recomputed every few frames; a per-texture bias from the budget computation and a global bias under a low pool; a scale on the wanted resolution of what is not visible; a bound on temporary memory for updates | Non-streaming bytes count against the same budget; hidden things are wanted at a scale, not dropped; the texture half of the quality scale |
| Epic Games, "Streaming Virtual Texturing" (UE 5.9) | Whole page, as a summary with quotes | Tiles of every mip, only the visible ones loaded; at least two fetches a sample; reactive, with visible pop | Confirms ADR-0065's choice of whole textures by level: no indirection in the resolve |
| Unity, "Configure mipmap streaming" (manual 6000.6) | Whole page, as a summary with quotes | The budget covers streamed and non-streamed textures alike; over it, lower levels are loaded and unused ones removed; a maximum level reduction is both the floor and what is loaded first at startup | **The floor is the start**: what a launch loads first is what eviction may never go below |
| Intel, SamplerFeedbackStreaming (the repository's README; the project is no longer maintained) | Whole page, as a summary with quotes | Reserved resources committed by 64 KB tiles from shared heaps; the packed mips always resident and never evicted; sampling clamped by a residency map; an eviction delayed by the frames in flight and rescued if asked for again first; a tile not evicted while a finer one depends on it; loads coarse first; the feedback resolve bounded in time by a running GPU timer; performance degrading with fragmentation on some drivers | The same four rules the geometry pool has (floor, delay, dependency, coarse first) stated for textures; a time bound on feedback; a warning against sparse binding until it is measured on the three cards |
| NVIDIA, `vk_lod_clusters` (README, `docs/streaming.md`, `docs/hitbased_streaming.md`) | Three pages, as summaries with quotes; `clas_allocation.md` not read | Groups of clusters streamed; the lowest-detail group of every geometry always loaded and outside the limits; requests appended during traversal and read back at the frame's end; loads trimmed to per-frame limits; unload by age; cluster structures built into scratch when a group arrives and moved once to a place sized from the build, because host estimates run several times the built size; a scheme that moves resident structures on unload cannot keep cached bottom levels. For rays the traversal is the rasterizer's and culling only scales detail. **Hit-based streaming**: a word per resident group with the last frame a ray hit it and the ray cone's width, propagated one level a frame; shadow rays register, ambient and sky samples do not; 14 to 44 % fewer resident groups for 0.10 to 0.68 ms a frame; the feedback lags a frame and a round trip, and a path tracer never lets it settle | **Structures built at admission, never moved after**, so the cache's addresses stay good; what rays reach is told apart from what the camera draws; hit feedback is weighed and declined for now ([what it declines](#what-it-declines)) |
| GPUOpen, Vulkan Memory Allocator, "Staying within budget" | Whole page, as a summary with quotes | Usage and budget can be read every frame; past the budget the driver may move blocks to system memory or fail; an allocation flagged to stay within budget is refused instead, which the page recommends for what is not essential and not for render targets | The frame's own allocations are never budget-refused; the pools' are, on a worker |
| Microsoft "Residency", Khronos `VK_EXT_memory_budget`, Epic "World Partition", Cesium | Not re-read: [as D39 read them](streaming-by-travel.md#published-practice-and-how-far-each-source-was-read) | A budget that moves, a reservation, destroying to give back, making on background threads | The governor's rules, kept |
| Barrett 2008, van Waveren 2009, Chen 2015, sampler feedback streaming | Not re-read: as cited in [texture](../subsystems/texture.md#levels-resident-by-what-the-frame-samples) and ADR-0065 | | |
| The GDC talks on streaming in *Ghost of Tsushima*, *Spider-Man* and Decima | **Not read**, as D39 found them | | |

**What no source read gives**: a rule for sharing one budget between geometry and textures by a common priority. Every system read keeps a pool a type and lets an author size each. The sharing rule below is this page's own, and is built last for that reason.

## The manager

### Two budgets, in bytes, from the tier and the machine

**`engine.memory.device_budget_mib` and `engine.memory.host_budget_mib`.** A store page states memory and GPU memory apart, the two are not exchangeable once the host keeps no copy of what the device holds, and Linux charges no commit for device bytes; the commit a Windows launch asks for is their sum and is reported, never set. With nothing named:

- **The device's** is the settings tier's ceiling (W51-2's proposal: 10 GB at the top, 4.5 GB at the low setting), capped by what the card reports less a reserve of an eighth of it and never less than 1 GiB (`gfx::MemoryBudget`, the driver's own budget where `VK_EXT_memory_budget` gives one). The 32 GB RTX 5090 takes the top tier's 10 GB and leaves the rest of the card to whatever else the owner runs; the 12 GB Titan Xp, which has no ray queries, takes a rasterized tier and is capped at 10.5 GB; a 6 GB card is capped at 5.
- **The host's** is the tier's (8 and 6 GB), capped at half the physical memory.

A budget under the floor ([below](#the-floor-is-the-start)) is refused at load with the floor's bytes by kind, as ADR-0063 decides.

### Three page types, one account

A **unit** is the least thing brought in or let go: it has a type, a byte size on the device, a state (absent, wanted, reading, staged, resident, retiring), a band and a key ([one priority](#one-priority-from-what-the-frame-already-knows)), and at most one unit it depends on.

| Type | The unit | Depends on | What stands in when it is absent |
|---|---|---|---|
| **Geometry** | A page of clusters, a fixed-size slot of a pool ([R9](../subsystems/renderer.md#geometry-streaming-the-gpu-half-of-pages-and-residency)), carrying every stream parallel to its clusters: positions, attributes, triangles, and by the scene's settings the float positions, 8-bit indices, coverage, motion vertices, skin and micromap data | The page holding its clusters' parents; a mesh's root | The coarser parent cluster: `select_lod_streaming`'s fallback clause, which the cluster DAG always has |
| **Texture** | One step of a texture's top level (ADR-0065: whole textures by level) | The level under it | The resident top level through the same sampler |
| **Tile arena** | A tile's block: its instances and pair entries, its ground chunk's slot, its collision compound, and the references it holds on meshes and textures | The far tier | The far form: the ring's coarser form, the far forest, the coarser ground ([renderer](../subsystems/renderer.md#what-a-frame-waits-for)) |

**The third type is not the plan's.** 04 §4.9 wrote "geometry, texture, AS" in September. Since then the measurements put the structures' bytes in two places neither of which is a page: a per-frame capacity sized by what frames built (ADR-0068) and structures built once from geometry that is resident, which follow their geometry. What turned out to have a residency of its own with bytes on the device is the tile: its ground chunk, its instance block and what it holds by reference. So the structures are dependents here ([the rays](#what-the-rays-need-resident-that-the-rasterizers-do-not)), and the tile is the third type.

**The descriptors stay resident, as now**: 112 bytes a cluster and 48 a page for every mesh *a held tile names*, about 7 % of a paged mesh, because the cull tests every pair of every frame and must be able to say "not that one" about a cluster whose payload is absent. Nanite keeps its hierarchy resident for the same reason. A mesh no held tile names has no descriptors either: it is an entry in the table a tile's meshes already are (ADR-0061), keyed by identity.

**Two slot classes for geometry.** The staging's load is 13,064 meshes and most are small: the ring forms average 95 clusters, the far-forest tiles 30. One 128 KB slot a mesh for its root would pin 1.6 GB of mostly empty slots, and the mesh room's answer (every cluster at its worst case, 2,632 bytes) holds 102,000 clusters in 256 MiB where the load's own packing holds 157,000. So: a **root pool** of small slots in a few size classes (4 KB to 128 KB) holds each named mesh's root, admitted and retired by reference as the room's meshes are now, and a **page pool** of 128 KB slots holds everything under the roots, by feedback. A mesh that fits its root is all root and never asks for a page. The mesh room becomes the root pool; a paged scene and a streamed world stop being two cases.

**The companions arrive with their page or their mesh, or are measured at the cook.** What the renderer reads from host floats at `create` today (the crowns' priors, the sky-facing meshes, a swaying mesh's bound, a seat) becomes a section of the mesh's container, append-only like every other, so a mesh that arrives needs no host copy. Micromaps are baked a mesh at its first admission on a worker, kept in the derived-data cache, and ride its pages after ([opacity micromaps](../subsystems/renderer.md#opacity-micromaps)). A rigid form's structures are built when the form is admitted, into scratch and then once to a place sized from the build, in slices of the frame's allowance, where today they are three blocking submissions at load.

**Pools are made of chunks, and the budget moves a chunk at a time.** The page pool, the root pool, the textures' allocator blocks and the tile arena each stand in chunks of 64 MiB (a tunable). A pool grows by taking a chunk made on a worker and joined at a frame boundary, and shrinks by emptying one and destroying it: the only way device bytes are really given back, and never a reallocation that holds two copies. A slot freed inside a kept chunk is room for its own type and nothing else, and the account says so (`held`, `pinned`, `evictable`, `free in chunks` by type, in `engine.budgets` and the frame record). This closes what R172's second stage left open (a trimmed image's block stays with the allocator, so the driver's usage does not fall).

**What the account does not hold**: the frame's own buffers and targets, which are sized by the settings and the layout and allocated outside it, never refused; and the traced chain's per-frame capacity, which keeps its own policy (ADR-0068, and R174's keep rule on its branch) under a ceiling the account hands it. Both are subtracted from the device budget before the three types share the rest.

### The floor is the start

What may never be evicted is exactly what a launch loads first: the descriptors and roots of every mesh a held tile names (they go when the last tile naming them has gone and its linger has run, never before); the far tier; every texture's tail of 128 texels and smaller (ADR-0065's floor; 117 MiB for the staging's 5,440 images); the tile blocks of the observer's own ring and the collision under each observer; the material records of resident meshes. Unity states the same identity (its level-reduction floor is what it loads first), Nanite's root pages and NVIDIA's lowest-detail groups are the same rule for geometry. It is why the start needs no second mechanism ([the start](#the-start)).

### One priority, from what the frame already knows

**A band first, then a key.** The band is how the frame reached the unit, and it is commensurable across the three types; the key orders units inside a band.

| Band | Reached by | Source, already in the frame |
|---|---|---|
| 0 | The floor | pinned at admission |
| 1 | The camera's views drew it or asked for it this frame | the cull's page requests and "used" words; the texture feedback's finest level asked; the ring a tile is in |
| 2 | Only a shadow or the bounce reached it: a cascade's run, a caster volume's entry, a hit on it | the same cull, by which run asked; a "hit" word set where a ray's hit is decoded |
| 3 | Predicted: inside the lead of the travel envelope and not yet reached | the rings scored along the observer's segment (ADR-0063's decision 3, W53) |
| 4 | Held behind: reached within the hold and not since | the last frame it was in band 1 or 2 |

The key is what each type already carries. **Geometry**: the projected size in pixels of the cluster that asked, then the distance, then the page index, which is `PageResidencyManager`'s order unchanged, starvation step included (nothing waits longer than 16 frames). **Texture**: the levels it is short by, then the frame last asked. **Tile**: its ring, then its distance. Between types inside one band the tie is the error in pixels the picture carries while the unit is absent: a geometry request's projected parent error, and two to the power of a texture's missing levels. That last comparison is an assumption this page cannot test, and the build leaves it for stage 6, after stage 0 has measured how often two types compete inside one band at all.

**A used mark is the eviction order.** Whatever is resident and reached again has its band and its frame refreshed (Nanite re-emits requests for resident pages for this). Eviction takes the highest band first and inside it the least recently reached, restricted to what may go: a unit with no resident dependent, not in band 0, and older than the hold (8 s for a texture level by ADR-0065; the tiles' linger of 120 commits; the same 8 s proposed for a page).

### One request queue and one upload allowance

**Requests** come from four places that exist and one that is new: the cull's page feedback; the texture feedback pass (one pixel in sixteen, 0.044 ms at 4K); the world's ring updates and its commits; the rays' hit marks (new, a word a resident page set where a hit is decoded); and a covered travel's destination. They meet in one bounded queue, deduplicated a unit, read back a frame slot late as the page requests are, so **nothing in the frame waits on the device**.

**Reads** are ranged reads of the unit's container on the efficiency pool (`io::read_ranges_async`, `geometry::ClusterFileReader`, the `.tex` range read), a bounded number in flight, cancelled when the unit is dropped, the head of the dependency order always able to start ([admission never waits on a read it cannot start](../subsystems/renderer.md#admission-never-waits-on-a-read-it-cannot-start)). The derived-data cache is what a shipped game ships; nothing is cooked at run time.

**Uploads share one allowance a frame**, in bytes and in time, in place of today's three (the pages' 256 KiB, a commit's 4,096 KiB and two meshes, the textures' share of that) and of the one upload that has none: W51-2 saw the terrain's rings rebuilt every three seconds standing still, about 150 MB each. The allowance is spent band by band in the queue's order, through one staging ring; a unit larger than the allowance goes in slices over several frames and becomes resident only when whole, the old content drawn until then. **In steady state nothing is allocated**: the pools, the ring and the queue are sized once; a chunk or a texture's image is made on a worker and handed over at a frame boundary, as a relayout's tables are ([the frame loop allocates nothing](../subsystems/renderer.md#the-frame-loop-allocates-nothing)). Last week's fault was a stall of hundreds of milliseconds at a tile's arrival; the rule against its kind is that a frame's share is bounded in time whatever is due (ADR-0063's decision 5).

### Eviction never changes the picture inside a frame

- **A frame reads the residency it was recorded with.** A freed slot retires for the frames in flight before it is filled again; a texture's old image goes after the frames that could sample it; a tile's block as now.
- **A pair that names an absent page draws its coarser parent.** The cut with pages missing is still a cut of the DAG, every part of the surface drawn exactly once, because a group is never split across pages and residency is kept ancestor-closed on the manager and on the pool; a request whose parent was evicted while it waited is dropped, not admitted over a hole.
- **Where everything a frame reaches is resident, the picture is the unstreamed one, to the byte.** That is the existing test of the page pool against the CPU reference, extended to the root pool, the tail and the chain.
- **A settled picture is a function of the scene.** While units are arriving the picture depends on what has arrived, which depends on the disk; a capture, a test and a frame budget's flight ask for a settle (a whole commit, the texture bound's settle at a cut) and compare settled frames only. Content stays deterministic: residency reads containers and never writes one.
- **One residency for every view of a frame.** The camera, the cascades and the caster volumes test the same residency words, so the cut is one cut and a shadow is still cast by the geometry the picture draws.

### How the three types share one budget

Each type has a floor (above) and, with nothing else said, the budget's free chunks go to whoever asks first. Under pressure:

1. **A type evicts its own band 4, then its own band 3**, before it asks for a chunk.
2. **A chunk moves from the type whose least-needed chunk is in the highest band** (a type with a chunk's worth of band 4 gives before one holding only band 2). The giver empties the chunk by evicting or, for a pool of fixed slots, by letting it drain; nothing is moved between slots.
3. **Band 1 of one type is never evicted for band 2 or higher of another.** That is the starvation guard between types: textures cannot take the pages the camera is drawing, and geometry cannot take a level a pixel sampled this second.
4. **When band 1 alone does not fit**, no eviction can help and the manager stops trying: it raises one **quality scale** that multiplies the LOD threshold in pixels and adds a fraction of a level to every texture's wanted top together, a step at a time with a hold-off, logged with the budget's name, and lowers it when a chunk's worth has been free for the hold. Epic's pool does this for geometry and its texture pool for mips; here one scale drives both so that neither type is sharp over the other's blur.

The governor of ADR-0063 stands above this: it reads the account's sums and the process's own figures, sets the two budgets' effective values as the machine's free memory moves, and takes its own steps (the rings' radii, flight). The three types are one pool to it.

### What the rays need resident that the rasterizers do not

- **What a traced frame holds beyond the picture** is the picture's casters (clusters the cone test kept out, instances in the caster volumes outside the frustum) and whatever the bounce's rays reach, which is the same set: the world-space cache is fed by rays against the frame's own structures ([renderer](../subsystems/renderer.md#indirect-light-the-world-space-cache)). Those pairs ask for their pages through the cull run that keeps them, at the camera's own LOD as the cascades do, and land in band 2.
- **Under pressure band 2 goes before band 1**, and a caster whose fine page is gone casts from its coarser parent. That is not a second LOD for the rays, which R94 measured and declined (a coarser surface beside the picture's own receivers shadows them): it is the same one cut, coarser where nothing on screen draws that part of the surface. Its cost is a shadow's edge off by the parent's error, and the pair that proves the stage measures it.
- **A traced page carries the builders' bytes** (float positions, 8-bit indices: a fourth range of the same read, as R9 already has), so a scene with the chain pays 2,632 bytes a full cluster where a rasterized one pays 1,492. The low setting's geometry is 37 % smaller mostly for that reason (1,676 against 2,656 MiB).
- **A built structure does not hold its page.** A cluster's structure in the cache is self-contained once built and is keyed by (mesh, cluster), not by an address in the pool; it stays valid when its page leaves and returns in another slot. But a hit must be shaded and a traced primary resolved from the page's attributes, so **a cluster is in a frame's records only while its page is resident**, and a cached structure nobody references ages out by the cache's own rule. Structures are never moved after their one placement, so the per-instance records' addresses stay good (the constraint NVIDIA's sample states for its caching).
- **The cache becomes by use in bytes as well as in slots**: built into scratch, placed at its built size (the forms measured 18 % of the driver's reservation, R143), in chunks under the account; the 506 MiB reserved at the driver's most a slot whatever was built is what W49 was opened for.
- **A device without ray queries** has no band 2 from casters beyond the cascades' runs, no builders' bytes, no cache and no micromaps: the same manager with one type's payload smaller.

### What the host keeps

**Nothing a device copy can serve.** After an upload the host keeps a unit's identity and where its bytes are in the derived-data cache, and reads them again on a job when a reader wants them (collision's proxies, a pick, a static form's build), never on the frame's thread. What stays is small and counted against the host budget: the descriptors of named meshes (149 MiB for the whole staging today), the reads in flight (a bounded number of units), the queue, and the scene's records. The host budget's steps are the governor's; the manager's part is that an admission's host bytes are released when its unit is resident, and that the load's peak is the reads in flight and not a second copy of the scene (W51-2's stage 4 is the first cut of that).

### What it costs a scene that does not need it

The design keys on what a scene holds and what a frame reaches. **A scene whose whole load fits under half the device budget is loaded whole, as today, byte for byte**: no pools, no feedback pass, no residency test in the cull, a floor equal to the scene. A consumer with a handful of static assets pays the account's few counters and nothing else. A scene past that line gets the manager whether its meshes came from a generator, a glTF or a container named outright: an entry is keyed by a mesh's identity, a page by the container's own table, a texture by its `.tex` key. Nothing reads a generator's output or a world's layout; a scene with no world block has no tile units and no bands 3 and 4, and its pages and levels are managed by bands 0 to 2.

## The start

**A launch is the floor, then the first ring, in the queue's order with the allowance lifted**: ADR-0063's covered travel from nowhere. The first frame may be drawn when band 0 is in and every tile of the observer's own ring has its block; by default it is shown when the inner rings have settled, the startup trace's `nearby_stable`, because a first frame that sharpens while it is watched reads as a fault ([questions](#open-questions-for-the-owner), 5).

**In bytes**, derived from W51-2's table and [what a place names](#what-a-place-names), counted by whole meshes (so before pages return anything), with the textures at their floor at the least and at the tier's proposed ceiling at the most:

| MiB on the device | Top, today | Top, at the first frame | Low, today | Low, at the first frame |
|---|---:|---:|---:|---:|
| The frame, the targets, the tiles' room, the materials (not residency's) | 3,772 | 3,772 | 3,224 | 3,224 |
| The chain's per-frame part and the cache | 725 (3,507 before the first capacity is released) | 725 | 0 | 0 |
| Geometry, companions, micromaps, forms' structures: rings 0 and 1 and the far tier | 4,098 | 815 to 1,208 | 2,037 | 405 to 601 |
| Textures | 3,150 | 117 to 1,536 | 3,150 | 117 to 768 |
| **Device** | **11,744** | **5,400 to 7,200** | **8,411** | **3,700 to 4,600** |
| Host, after the stages in hand | 3,800 | 3,800 | 2,800 | 2,800 |
| **What a launch commits on Windows** | 15,200 after the four stages (30,600 at the peak on main) | **9,200 to 11,000** | 11,200 (24,000) | **6,500 to 7,400** |

The ranges are the corner and the origin of the four places. As rings 2 and 3 arrive the device stands at 6.9 to 8.1 GB at the top and 4.4 to 5.0 at the low setting ([the budget](#the-budget-it-serves)). The load's peak is the standing figure plus the reads in flight and the staging ring, since no unit is held on the host beside its device copy.

**A cold place entered inside a session** (a teleport, a load of a save) is the same call with a place the session came from: covered, release to the floor what the destination does not also name, then fill with the allowance lifted and the whole machine, arriving settled; or shown, a preload observer at the destination whose units enter band 3 and are admitted at the frame's allowance while the traversal plays (ADR-0063's decision 6, unchanged). Neither can hitch a frame: the covered form draws a cover, and the shown form's frames take only their allowance. What the walk rows would measure is the shown form's time to settle, which is the destination's bytes over the read and upload rates, said by `travel_begin`'s estimate before the move.

## The budget it serves

W51-2 proposed host at most 8 GB and device at most 10 GB at the top setting, 6 and 4.5 at the low one, from what shipped games of this scope ask (16 GB of memory, 12 to 16 GB of GPU memory at 4K with path tracing; its table has the sources).

| | Budget | Today (main) | After the four stages in hand | With this design, by whole meshes | Reached |
|---|---:|---:|---:|---:|---|
| Host, top | 8 GB | 13.7 GB | 3.8 GB | 3.8 GB | By the stages in hand, with 4.2 GB to spare |
| Device, top | 10 GB | 11.7 GB | 11.7 GB | 6.9 to 8.1 GB (textures at a 1.5 GB ceiling; 6.5 to 7.7 with the cache by use) | Yes, with 1.9 to 3.1 GB for the lead ahead and the hold behind |
| Host, low | 6 GB | 12.6 GB | 2.8 GB | 2.8 GB | By the stages in hand |
| Device, low | 4.5 GB | 8.4 GB | 8.4 GB | 4.4 to 5.0 GB (textures at 768 MiB) | At three of the four places; over by 0.5 GB at the densest |

**Why the low setting is the hard one.** 3.2 GB of its 4.5 are not residency's: the frame's pair-sized working set is 1.1 GB at 1080p as at 4K, because it follows the layout's 684,483 pairs and the cascades' runs and not the pixels (R4's design on its branch proposes sizing it by what survives the cull and finds targets nobody reads), and the tiles' reserved room and the material table are sized for the top tier. What closes the last 0.5 GB, in the order this page would take it: geometry by page for the inner rings' whole meshes (stage 8, unmeasured until stage 0), R4's rows, the room and the material table by what the tiles name.

**What the next tighter budget costs in picture**, class by class in the order they coarsen, which is the order of the least a viewer sees:

1. **Texture levels nobody sampled inside the hold**: nothing. R172 measured 56 % of a walk's resident texture bytes never sampled.
2. **Texture levels that were sampled**, least recently asked first: one level softer. On the village walk a ceiling a seventh of the set drew 0.0046 % of textured pixel-frames one level softer ([texture](../subsystems/texture.md#stage-2-the-trim-under-a-ceiling)).
3. **Geometry pages in bands 4, 3 and 2**: nothing on screen for the first two; for band 2 a shadow's edge or a bounce's occluder from the parent cluster.
4. **The quality scale**: every drawn cluster's error and every texture's top level a step coarser together, the first cost a viewer can see everywhere at once.
5. **The rings' radii and the chain's casters** (the governor's steps and R174's keep rule): far detail later, shadows missing from what was dropped. Last, because a missing shadow is the most visible of these.

An 8 GB card at the top tier's settings (a 7 GB device budget) fits at three of the four places with steps 1 and 2 (textures at about 1 GB) and the cache by use, and at the densest by a step of the scale or by stage 8; a 6 GB card with the chain on does not fit the frame's own 3.8 GB and the chain together with a world this size and belongs to the low tier.

## Stages, in build order

Each is a row one agent can build and prove; smallest first. "Returns" is device bytes on the staging at the start and standing, from the tables above; in flight a stage returns the same share of what the walk holds, less the lead and the hold. Every stage must keep: the frame budgets; no allocation in the steady frame loop; the picture to the byte wherever everything a frame reaches is resident; containers' bytes unchanged unless the stage says it adds a section. Stage letters a to j are [D39's](streaming-by-travel.md#the-build-in-stages).

| # | Stage | Size | Returns | Its case and its measure | Must not break |
|---|---|---|---|---|---|
| 0 | **What a frame reaches.** Counters, no policy: per frame and over a walk, the clusters drawn by the camera, kept only as casters or cascade entries, and hit by a ray, by mesh and by the page each would be in (the page table computed from the containers); the texture feedback run on the staging; the tiles' blocks by ring. D39's stage a, narrowed to reach | S | 0 | The staging standing at this page's four places and along the grounded walk, both settings: the table this page could not fill (a place's working set by page and by level) | The counters ride existing readback slots; their cost is stated before they are on |
| 1 | **The texture ceiling from the device budget, and blocks given back.** The textures' allocator in chunks of its own; `renderer.textures.ceiling_mib` set from the tier when nothing names it; a persistent staging for promotions. R172's remaining "not done" | S | 1.6 GB top, 2.4 GB low | The staging standing and walked: the driver's own usage falls by the trim; the share of pixel-frames a level softer reported and under a bound the row states first | A turn back inside the hold reads nothing again; a scene under its ceiling is main's bytes and picture |
| 2 | **One upload allowance and one staging ring** for pages, admissions, promotions and the ground's ring chunks, in bytes and in time, a unit larger than it in slices | M | 0 | The walk's worst frame at a tile's arrival and at a ring rebuild, as a pair against main; a ring rebuild standing still no longer uploads 150 MB in one frame | A whole commit (a capture's fill) still settles in one call |
| 3 | **The root pool**: the mesh room's arena by size class, packed as the load packs, with the room's free list, admission and retirement unchanged above it | M | The room holds 1.5 times the clusters it holds now, in the same 256 MiB | `tile_meshes_tests.cpp` and the admission case over the new pool; a scene whose tiles hand meshes walked (the rockfall deposits' walk) | Nothing drawn depends on the order meshes arrived in (ADR-0061's rule and its test) |
| 4 | **The load's meshes by reference**: a scene's own meshes and ring forms become entries a held tile names, read when first named, their descriptors and root admitted then, retired after the last with the linger. D39's stage e | L | 1.7 to 2.6 GB top, 1.0 to 1.6 GB low (geometry and companions less the named share); the load's `meshes` and `authored forms` stages by the share the start does not name | The staging at four places: device bytes against this page's table; the picture at marks equal to a whole load's once settled (0 id, 0 depth, 0 colour with the bounce off); time to the first frame | A scene with no world block; a scene under the whole-load line; pair numbering and the tie rule |
| 5 | **A mesh's companions arrive with it**: per-mesh scalars as container sections (a cook change, append-only); micromaps baked at first admission on a worker and cached; a form's structures built at admission in slices. D39's stage f | M | 0.35 to 0.56 GB top (micromaps and the forms' structures less the named share) | A streamed alpha-tested mesh's shadow rays cost what a loaded one's do; the load's blocking bake and three blocking submissions gone | The traced picture word for word against the rasterized cut; the forms' pair of structures built once against per frame |
| 6 | **The account, the bands and the exchange**: one device budget over the three types in chunks; bands 0 to 2 and 4 from the frame's marks, the hit word included; chunks moving between types; the quality scale; the floor's refusal at load; `engine.budgets` | M | Nothing by itself; it is what lets the tier's number be one number | The staging under 10, 7 and 4.5 GB budgets, standing and walked: no allocation refused by the device, the account within 3 % of the driver's usage for its types, the scale's steps logged, and a pair for a caster's coarser parent | Band 1 of one type never evicted for another's band 2 |
| 7 | **The start by priority**: the load as a covered travel from nowhere, floor then rings in the queue's order, ready at `nearby_stable`. D39's stage h, the first half | M | The launch's commit becomes the first frame's column above; the time to ready by the share not read | The staging cold and warm at three places: peak commit within 5 % of the standing figure; the picture on arrival equal to a whole load's | A capture's first frame is a settled one |
| 8 | **Pages in a streamed world**: the page pool under the tail, the deformed-vertex pool resolved through a page's slot (E25's suballocation), templates and the cache by page. D39's stage j | L | Stage 0 says; at most ring 0's and ring 1's whole meshes, 0.2 to 0.46 GB of geometry at the four places before companions | The staging's low setting under 4.5 GB at the densest place; D39's walks as they passed | `--stream` on a scene read whole, byte for byte; the streaming cut against the CPU reference |
| 9 | **The structure cache by use**: built into scratch, placed at its size, in chunks under the account (W49) | M | About 0.4 GB top | Second flight over the same cuts builds nothing; a full cache evicts and the picture holds, as now | The per-frame build's bytes |

The envelope, flight and the two forms of travel (D39's stages c, d, g, i) are unchanged and are not this row's; stage 6's band 3 is empty until the envelope exists.

## What it declines

- **Pages for textures and sparse binding.** ADR-0065 decided whole textures by level; the virtual-texture sources read here confirm the cost (two fetches a sample at the least) and Intel's sample reports sparse heaps degrading with fragmentation on some drivers. Revisit as that ADR says.
- **Hit-based requests.** NVIDIA's sample saves 14 to 44 % of resident groups by letting rays say what they reach. Here the chain's membership already comes from the cull's caster volumes, the hit word only refreshes what is resident, and the sample's per-group propagation passes cost 0.12 to 0.21 ms; it also reports that a path tracer never lets the residency settle, and the bounce is a stochastic reader. Revisit when stage 0 shows band 2 holding more than a few hundred MiB that no ray hits.
- **A second LOD for the rays**, and a fallback mesh for tracing: R94's measurement stands.
- **Streaming the descriptors or the hierarchy.** Seven percent held so that the cull never waits a level at a time.
- **Resizing a pool by making a larger one and copying**, compaction of slots, and per-mesh device buffers: the first is Epic's hitch and double peak, the second moves structures the cache points at, the third ADR-0061 weighed.
- **Sizing the frame's own buffers.** They are R4's ground and the pair cap's, and the largest single thing between the low tier and its budget; this design subtracts them and says so.
- **Decompression and prediction** (W52, W53): pages are read as they are stored, and band 3 waits for the envelope.
- **A host cache of what was evicted.** The operating system's file cache is that cache, and it is not charged to the process.

## Open questions for the owner

1. **Two budgets, or ADR-0063's one.** That ADR (proposed) sets one number for host plus device. *Recommendation: two, a device budget and a host budget, with commit reported as their sum.* It is how a store page is written and how W51-2 proposed its figures, the bytes are not exchangeable once the host keeps no copy, and one number means something different on Linux. ADR-0063's other decisions stand as written.
2. **The tiers' numbers.** *Recommendation: take W51-2's (device 10 and 4.5 GB, host 8 and 6) as the two tiers' ceilings now, capped by the card less an eighth*, and add a tier only when a card is measured under it. The design reaches the top one with room and the low one except at the densest place.
3. **What coarsens first.** *Recommendation: the order above*: unsampled texture levels, sampled ones, geometry nobody on screen draws, then everything a step together, then the rings and the casters. The owner's walk is the judge of step 2 against step 3.
4. **A shadow cast by a coarser parent when its caster is off screen and the budget is tight.** *Recommendation: yes.* It is one cut, not a bias, and it is what lets a 12 GB card hold a traced world; stage 6 flies the pair before it is on.
5. **The first frame: at the floor, or settled.** *Recommendation: settled by default for a launch and a covered move (shown at `nearby_stable`), with the earlier frame a game's choice.* A world that sharpens under the player's eyes in its first seconds is the pop-in this is meant to end.
6. **Accepting ADR-0063.** Stages 2, 6 and 7 lean on its decisions 2, 5 and 6. *Recommendation: accept it with decision 1 amended by question 1.*
7. **A cook change for the companions** (stage 5 adds container sections, so every mesh cooks again once). *Recommendation: yes, with the next change that bumps the cluster cache version anyway.*

## The decision this page asks for

Drafted for an ADR whose number the coordinator gives; status proposed.

*Context*: this page's question, inventory and sources.

*Decision*:

1. **Two budgets in bytes**, the device's and the host's, from the settings tier and capped by the machine; commit is reported.
2. **Three types of unit under one account**: a geometry page (a root in a pool of small slots, by reference; the rest in 128 KB slots, by feedback), a texture's level step, a tile's block. Descriptors of named meshes stay resident.
3. **The floor is what a launch loads first**, and nothing evicts below it; a budget under it is refused at load by kind.
4. **One priority**: a band from how the frame reached a unit (floor, camera, ray or shadow only, predicted, held), then the type's own key; eviction from the highest band, least recently reached, never a unit with a resident dependent or inside its hold.
5. **One request queue, ranged reads on workers, one upload allowance a frame** in bytes and time; pools in chunks, made and destroyed off the frame's thread; nothing allocated in a steady frame.
6. **A frame draws what its residency holds**: the coarser parent, the resident level, the far form; one residency for every view; where everything reached is resident the picture is the unstreamed one to the byte; comparisons are of settled frames.
7. **Between types**: own bands 4 and 3 first, then a chunk from the type with the least-needed chunk; band 1 never yields to another type's band 2; when band 1 does not fit, one quality scale for geometry and textures together.
8. **Rays**: casters and what the bounce reaches are band 2 and coarsen first, by the same cut; a traced page carries the builders' bytes; a structure is built at admission, placed once, keyed by content, and referenced only while its page is resident.
9. **The host keeps nothing a device copy can serve**, and reads again from the derived-data cache on a job.
10. **A scene under half its device budget is loaded whole**, as before; nothing reads a generator.

*Consequences*: the mesh room, the page pool and the texture table become pools of one account, and their tunables overrides; the load becomes a travel from nowhere; a mesh's companions move into its container; forbidden from here, a device residency outside the account, a pool resized by copy, a structure moved after placement, and a frame that waits for a unit.

*Revisit when*: stage 0 shows two types rarely compete inside a band (then decision 7's tie needs no formula); sparse binding is measured cheap on the three cards; a world ten times the staging makes the descriptors of named meshes or the scene's records the floor.

## What this page did not measure

- **Anything by running it.** The four places are a script over a scene file and a report; the staging's own view is not one of them by name, and the low setting's shares are the top's applied to its bytes.
- **What a frame reaches inside a named mesh**, and the textures a place names: stage 0. Every "returns" from stage 4 on is by whole meshes and an upper bound on what is held, a lower bound on what pages return.
- **The classes parallel to geometry at the geometry's share**: an assumption.
- **A cold file cache's read rate**, so no time to settle is given.
- **The owner's own world** (v51), which is larger than the staging.
- **Collision and a document host's records**, which W51-2's offscreen loads did not hold.
- **Sources not read**: Epic's Nanite technical details page (no body returned), `vk_lod_clusters`' `clas_allocation.md`, the GDC streaming talks; and the sources D39 and ADR-0065 read are cited through them, not re-read.

## Rows read and not edited

W51-3 (the host's start bytes: this page's "stages in hand"); the pass audit's mends on branch `b69-R186-1` (every buffer of the chain is one set for the frames in flight and is imported naming its last use: a pool's chunk joined at a frame boundary has to be imported the same way); R188's design on `b69-R188` (nodes over instance slots: a node's measure reads the mesh table, which stage 4 writes at every admission, so its "a mesh's sphere changed" rule covers an admitted mesh); R4's on `b69-R4` (the frame's targets and the working set, which this design subtracts and does not manage); R174 on `b69-R174` (what the chain keeps past its capacity, by size on screen: the chain's own rule under the ceiling the account hands it).
