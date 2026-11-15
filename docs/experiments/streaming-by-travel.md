# Memory held by travel (D39, 2026-11-14)

**A design, nothing built.** The decision it asks for is [ADR-0063](../adr/0063-memory-is-held-under-one-budget-and-by-travel.md) (proposed). Every number here is either read from a page or a trace this page names, or derived from those on the page, and says which; what nobody has measured is listed at the end and is the first stage of the build.

## The question

The owner walked the forest proof of concept on batch 63 (2026-11-13: 3840 × 2160, traced primaries and shadows, the bounce; walking, running and flying) at **75.9 GB of private commit before the first frame and 79 GB later, against a working set of 22 GB**, and saw the frame time jump every couple of seconds while running, and while flying "as objects switch LOD en masse". Their ask, in their words:

> there should be fast enough background memory usage management that improves load times, as well as the amount of memory being held ... any normal move speed (walking, running, horse riding, driving) should be able to manage it

flying needs its own technique, and fast travel needs unloading and loading, "with support for a loading screen or support for an immersive fast travel that doesn't involve hiding the move".

So five things, and this page takes them in order: **a budget** the consumer sets and the engine stays under; **an envelope** that follows speed and heading so the ordinary speeds never outrun it; **flight**, which is what the engine does when the envelope is outrun; **fast travel** as an interface a game calls, in two forms; and **what has to be restructured first**, because today most of what a walk holds is held whole wherever the walker is.

It is the policy above two rows that are fixing the mechanics tonight and are not in this branch: T129 (where the commit is; its page is `docs/experiments/memory-commit-accounting.md` on branch `b64-T129`) and T131 (the world's tile work off the frame's thread and no trading at the pair cap; `docs/experiments/tile-work-off-the-frame-2026-11-13.md` and the world page's new sections on branch `b64-T131`). Both are cited here as text, by branch, because a link from this branch would not resolve; whoever merges the three should turn the citations into links.

## What the engine has, and what the plan already says

- **The plan asked for this shape.** [04 §4.9](../plan/04-renderer.md#49-streaming-and-residency): "One residency manager, three page types (geometry, texture, AS), one budget policy with per-type floors", with requests "from GPU feedback ... and from tile predictions based on observer velocity". What exists is the geometry page pool ([renderer](../subsystems/renderer.md#geometry-streaming-the-gpu-half-of-pages-and-residency)); there is no one budget and no prediction from velocity.
- **A tile ring over observers** ([world](../subsystems/world.md#the-rings-rules-and-defaults)): tiles scored by ground distance to the nearest observer, banded into rings with a 15 % hysteresis, consumers called in order, budgets on activations. Nothing in it reads a velocity.
- **Residencies, each with its own bound**: the mesh room for meshes a tile hands (256 MiB, resident by reference with a linger of 120 commits, [world](../subsystems/world.md#a-tiles-meshes-residency-by-reference), [ADR-0061](../adr/0061-a-tiles-meshes-arrive-with-the-tile.md)); their textures (1,024 MiB); the page pool of a scene read whole under `--stream`; the ray tracing chain's capacity (a sixteenth of the device, shrinking after `renderer.rt.shrink_seconds`, [renderer](../subsystems/renderer.md#the-budget-follows-the-device)); the structure cache (least recently drawn first, [renderer](../subsystems/renderer.md#the-resident-cluster-structure-cache)); the flora tail (a sixteenth of the device, nearest first, the rest withheld, [renderer](../subsystems/renderer.md#a-commit-past-the-tails-budget-withholds-its-farthest-tiles)); the pair cap. Seven bounds, seven tunables, no sum.
- **Far forms that are always there**: the far forest, one rigid mesh of coarsest forms per world tile, built at load ([renderer](../subsystems/renderer.md#the-far-forest), [ADR-0059](../adr/0059-the-far-forest-is-the-renderers-per-world-tile.md)); the ground's far levels and the catch-up rule "the coarser ground, already resident, never nothing" ([renderer](../subsystems/renderer.md#what-a-frame-waits-for)).
- **What is not streamed at all**: the scene's own meshes (4,070 of them on the forest's staging), their textures, the opacity micromaps, the CPU's merged copy of the geometry. They are read at load and held for the session.

## Published practice, and how far each source was read

Read on the night of 2026-11-13/14 through a fetching tool that returns the page's text or a summary with short quotes; where only a summary came back, the quotes below are the tool's and were not checked against the raw page. Nothing is taken from memory of a talk.

| Source | How far it was read | What it says that is used here |
|---|---|---|
| Epic Games, [World Partition in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine) (the page is titled for UE 5.9) | Whole page, as a summary with quotes | "Streaming sources are components that define a position in the world and trigger the loading of cells around them"; a grid has a cell size and a loading range (its example: 256 m cells, 768 m); a source's target state is "Loaded or Activated"; "a streaming source component can be activated at the location that a player will teleport to, so it can load the cells there"; "Is Streaming Completed" as the query; a grid option "Block on Slow Streaming" that "Blocks loading in situations where grid cells are not loading fast enough"; `wp.Runtime.MaxLoadingLevelStreamingCells` limits concurrent loads; a console variable weights a cell's importance by its angle to the source. It says nothing about velocity. The API reference for `FWorldPartitionStreamingSource` returned no body; a search summary says it lists a `Velocity` member "computed automatically", which is not relied on. |
| Epic Games, [World Partition: Hierarchical Level of Detail](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine) | Whole page, as a summary with quotes | "Content in unloaded cells is replaced with HLODs"; an HLOD layer has its own cell size and loading range, or is always loaded; a layer can have a parent layer. |
| Epic Games, [Streaming and HLODs in Unreal Editor for Fortnite](https://dev.epicgames.com/documentation/fortnite/streaming-and-hlods-in-unreal-editor-for-fortnite) | Whole page, as a summary with quotes | With streaming on, "only the cells surrounding the player's current position count toward the Current Memory Usage"; streaming on without HLODs: assets "will disappear at a distance". Nothing about speed or about outrunning the loads. |
| Microsoft, [Residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency) (Direct3D 12 programming guide, dated 2018-05-31) | Whole page, raw text | The process has a video memory **budget** that "can fluctuate noticeably"; "If an application doesn't stay within its budget, the process will be intermittently frozen ... and/or the creation APIs will return failure"; a **reservation** is "the amount of memory they cannot go without"; "destruction of heaps is a more reliable technique to free up residency"; "It's recommended to create heaps on background threads to avoid glitching the render thread"; "Applications should not rely on the over-budget behavior of the kernel"; watch "for negative budget changes and destroy unused API objects roughly once a frame". |
| Khronos, [VK_EXT_memory_budget](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html) | The description, as a summary with quotes | `heapBudget` is a guideline for how much of a heap the process can use "before allocations may start failing or causing performance degradation"; the values are implementation-dependent and change with the system's other activity; query "at some interval (once per frame, per few seconds, etc)". |
| Cesium, [`Cesium3DTileset`](https://cesium.com/learn/cesiumjs/ref-doc/Cesium3DTileset.html) (CesiumJS 1.147 by its source links) | The constructor's option table: the first 100,000 of 112,719 characters, as a summary with quotes | `cullRequestsWhileMoving` (default true): "Don't request tiles that will likely be unused when they come back because of the camera's movement"; `preloadFlightDestinations` (true): "Preload tiles at the camera's flight destination while the camera is in flight"; `foveatedTimeDelay` (0.2 s): how long "to wait after the camera stops moving before deferred tiles start loading in"; `cacheBytes` (512 MiB): the size the cache "will be trimmed" to when it holds tiles the view does not need, with `maximumCacheOverflowBytes` of headroom when the view needs more. |
| 80.lv, [Marvel's Spider-Man 2 fast travel hides no loading screens](https://80.lv/articles/marvel-s-spider-man-2-fast-travel-hides-no-loading-screens/) (2023-10-19) | The news article, as a summary with quotes | Insomniac's director of core technologies says the hold-to-confirm prompt hides no load and was kept because "having a confirmation window was important for player usability". A shipped game travels across its map without covering the move; **how** is not in the article. |
| GDC Vault: A. Bentley, "Zen of Streaming: Building and Loading 'Ghost of Tsushima'" (2021); E. Ruskin, "Marvel's Spider-Man: A Technical Postmortem" (2019) | **The abstracts only** (the second through 80.lv's write-up of its description); neither talk was watched and no slides were found | Nothing. They are named so the next reader knows where the detail on fine-grained streaming and on a city streamed at swinging speed is said to be, and that this page does not have it. |
| Guerrilla on flight in Horizon Forbidden West | **Not read at source**: the studio's "Streaming the World of Horizon Zero Dawn" page timed out twice, GameSpot's report refused the fetch; a search summary quotes Video Games Chronicle quoting a PlayStation Blog post | Only that the studio says flying "can see a lot at once" and needs "fast streaming technology", and that flight was nearly cut on the older console. No number, no technique. |
| Wikipedia, [Preferred walking speed](https://en.wikipedia.org/wiki/Preferred_walking_speed), [Horse gait](https://en.wikipedia.org/wiki/Horse_gait) | The speeds, as summaries with quotes | Walking 1.10 to 1.65 m/s; a gallop "averaging about 40 to 48 kilometres per hour" (11.1 to 13.3 m/s), a sprint record near 88.5 km/h. |
| Karis, Stubbe and Wihlidal, "Nanite: A Deep Dive" (SIGGRAPH 2021) | Not re-read; cited on the renderer page as known | The page pool it describes is the one this engine already has. |

**Where this design follows them.** A destination's observer that loads without drawing is Unreal's streaming source at a teleport's target with its "Loaded" state. Far stand-ins for whatever is not loaded are its HLODs, and a far tier with a coarser grid of its own is an HLOD layer's own cell size. A cap on work in flight is `MaxLoadingLevelStreamingCells`. Ordering by heading is its angle term. One budget watched every frame, a floor the application cannot go without, releasing by destroying, and making on background threads are the Direct3D 12 guide's four pieces of advice. Not asking for what the motion will have left behind before it arrives, loading the far end of a flight while the flight plays, and waiting for the camera to slow before the deferred detail comes in are Cesium's three options, which is the only published statement found of streaming that reads the camera's speed.

**Where it does not.** Unreal's "Block on Slow Streaming" stops the game when the cells are late; here a frame never waits for a load, and being outrun changes what is drawn instead (flight). Unreal's and Cesium's ranges and cache sizes are numbers an author sets per grid or per tileset; here the reach comes out of one budget and the rates the session measures, because the owner asked for the speeds to be managed and not tuned. And none of the sources read says how far ahead to load for a speed: the derivation below is this page's own, from this engine's traces.

## What is resident for a place today

T129's table (branch `b64-T129`; the forest's staging copy, the owner's renderer settings, offscreen, no game module; the hop flight F2 at its exit, MiB of commit). The last column is this page's reading of the engine pages: whether the bytes follow where the eye is.

| Kind | MiB | Follows the place today |
|---|---:|---|
| **On the device** (each byte also a byte of commit on Windows: 512 MiB uploaded raised commit by 514 MiB) | **16,237** | |
| The scene's geometry (positions, attributes, triangles, cluster tables) | 4,491 | No: one set of buffers at load |
| The traced chain's cluster storage (6 chunks, each cluster at the driver's most) | 2,848 | Yes: sized by recent frames, released after 120 s |
| The chain's bottom-level set, scratch, top level | 808 | Yes |
| The structure cache; `create_ray_tracing` | 506 + 353 | By use, not by place: least recently drawn first under a fixed ceiling |
| The pair tables | 896 | Yes: by the stride |
| The flora's parts, poses (host-visible) and relayout | 752 | The tail yes, the parts no |
| Materials | 348 | No |
| Opacity micromaps, the set and the bake | 610 | No: one bake over the load's clusters |
| Terrain | 256 | Yes: slots round the eye |
| Images and buffers under 16 MiB (textures, the frame's targets, the indirect light's cache) | 3,755 | Not split by T129 |
| **On the host** | | |
| The merged geometry's CPU copy | 4,672 (3,608 resident) | No |
| The rest of the engine heap | 11,158 | Not split by T129 |
| C runtime heaps beyond the engine heap's live blocks | 1,689 | |
| The device's host-visible blocks | 812 | |
| Private with no named owner (grows with frames, never touched) | 7,710 | No: it is not anyone's yet |
| Executable and mapped files | 364 | |
| **The process** | **42,376** | working set 16,313 |

Three readings decide the rest of the page. **About 10 GB is geometry and its companions held whole**: 4.5 GB on the device and 4.7 GB on the host for one merged mesh, 0.6 GB of micromaps, 0.35 GB of materials. **About 5 GB already follows the eye** (the chain, the pair tables, the terrain's slots, the flora's tail) and is released behind a walker by rules that exist. **And 7.7 GB belongs to nobody**: it is T129's second leg, not a streaming question, and a budget has to count it anyway. The staging is not the owner's world: v43 with its sea, islands and 416 animals committed 79 GB, and what the other 37 GB is has not been split.

## The budget

**One number, `engine.memory.budget_mib`: the most the process may hold, host bytes plus device bytes.** On Windows that sum is the process's private commit, because the driver backs every device-local byte with commit (T129 measured it: 514 MiB for 512); on Linux the device's bytes are not charged, and the engine counts them itself so the number means the same thing on both. One number and not two, because the consumer's question is "how much of this machine may the game take" and the split between host and device is the engine's to make: the device's part is capped a second time by what the device reports (`gfx::MemoryBudget`, the heap's size or `VK_EXT_memory_budget`'s `heapBudget` where it is present), and that cap moves with what other processes hold.

**The default, when nothing is named, follows the machine, as the chain's sixteenth follows the device.** Proposed: **half the physical memory** (32 GB on the development machine, against the staging's 42 GB and the walk's 79 GB today). Why a share of physical memory and not of the commit limit: commit past physical memory is a promise the page file keeps, the limit is a setting of the machine's and not a property of it, and a game is not alone on its machine (the owner's own case is an editor and another GPU job beside the walk). Why half: today's *touched* bytes on the staging are a 16.3 GB working set and 16.2 GB on the device, 32.5 GB, so half of 64 GB is what the forest already needs once nothing is held untouched, and less once the geometry's host copy is gone. This is the first of the owner's decisions; the alternative defaults are in the ADR.

**A floor, named at load.** What a place cannot go without: the descriptors every cull reads (112 bytes a cluster and 48 a page for paged geometry), the far tier, the frame's own targets, the coarsest level of every texture a far form samples, collision under each observer, the simulated rings' state. A budget under the floor is refused at load with the floor's bytes by kind, the Direct3D guide's "reservation" turned into a sentence: never a run that starts and then thrashes.

**Who stays under it: one governor over pools.** Each residency the engine has becomes a **pool** that reports, without a walk of the address space, four numbers and one function: bytes held on the host and on the device, bytes it could release now without changing a pixel, its floor, and `trim(target)`. The pools are the ones listed above (pages, the mesh room, textures, the chain, the structure cache, the flora tail, the tile sets, the terrain's slots) and the ones the restructure adds. The governor runs off the frame's thread a few times a second: it reads the process's commit from the system (one cheap query, not `--memory-report`'s walk) and the device's usage, takes the pools' sum from it, and **counts the difference as unaccounted, against the budget**, so a leak or a driver's pool shrinks what the pools may hold rather than the machine's free commit; it then gives each pool a ceiling. It is 04 §4.9's "one budget policy with per-type floors", with the types being the pools.

**What it does as the sum nears the budget, in this order**, each step counted and said once per change in the log with the bound's name, as the withholds are today:

1. **Release what nothing names**: lingering meshes and textures, the chain's free steps, cache slots not drawn recently, pages last used longest ago. Free: nothing drawn changes.
2. **Stop holding behind**: the band kept behind the direction of travel goes to the hysteresis alone.
3. **Coarsen from the far side**: the outer rings' radii come in (their tiles draw their far forms, which are resident), fine pages give way to their parents, a texture's top level goes.
4. **Withhold, nearest first**: the existing rule. A tile that does not fit keeps its far stand-in and is tried again; every tile drawn outranks every tile withheld.
5. **Never**: an allocation that fails in a frame, a hole in the picture, a release below the floor, or a trade (a tile let go and another drawn in its place within the hold-off, T131's rule).

Trimming frees commit only where the bytes are really given back: on the device by destroying the buffer (the Direct3D guide's advice, and what the chain's shrink already does), on the host by returning whole blocks. A pool that only marks bytes free inside a buffer it keeps has released nothing, and its "releasable" says so.

**What an imported asset gets and what a scene with none of it costs** is in [the general-purpose check](#the-general-purpose-check): the budget and the governor are not the world's, and a scene read whole with no world block has them over its page pool and its chain.

## The travel envelope

**The rule in one sentence: what is requested is what the observer will be near within the time it takes to bring it in, and what is held behind is what a turn would need back, while the budget has room.** Both are times, measured, and become distances by the speed.

### Demand: what a metre of travel asks for

The ring scores a tile by the distance from the observer to the tile's centre, so tiles cross a ring's radius one at a time, and a disc of radius *R* moving a metre sweeps 2*R* square metres of new ground. With tiles of side *T*, a ring boundary asks for **2*R* / *T*² tile builds a metre** at its leading edge. A tile that moves in a ring is built again in its nearer form, and one that moves out a ring behind the observer is built again in its coarser one (at 1.15 *R*, the hysteresis); only leaving the outer ring builds nothing.

For the forest's staging (`world.tile_size` 64 m, rings of 1.25, 3, 10 and 20 tiles: 80, 192, 640 and 1,280 m):

| Boundary | Leading, builds a metre | Trailing, builds a metre |
|---|---:|---:|
| 80 m | 0.039 | 0.045 |
| 192 m | 0.094 | 0.108 |
| 640 m | 0.313 | 0.359 |
| 1,280 m | 0.625 | 0 (a release) |
| **Sum** | **1.07** | **0.51** |

**1.58 builds a metre of straight travel, three fifths of them at the two outer boundaries.** Turning asks for nothing (a turn changes no distance), so heading matters to the order and the prediction, not to the count. The count is the scene's: it scales with the sum of the radii over the tile's area, and a consumer with one ring of 8 tiles of 32 m asks for 0.5 a metre.

### Supply: what the traces say a thing costs

| Thing | Measured | Run |
|---|---|---|
| A tile's build (placements) | 1.3 ms | v23, 64 m tiles, the flight of 2026-10-31 ([world](../subsystems/world.md#performance-notes), W81) |
| | 2.6 to 3.5 ms | v24, the owner's dash, 2026-11-01 (W82) |
| | 4.5 to 10 ms (18 to 40 ms of `world.tiles_ms` a frame at four tiles a frame) | v43, the owner's walk of 2026-11-13, at 242 to 251 s (T131's reading of its `frames.jsonl`) |
| | "15 to 60 ms" | T131's page, said of a forest tile without naming the frames |
| A commit's hand-over of a job's tiles on the frame's thread, the most | 1.36 and 2.79 ms | T131's branch, its cap-walk scenes |
| Meshes admitted | at most 2 and 4,096 KiB a commit (240 MiB/s at 60 commits a second); a 2K texture set is three commits and its mesh the fourth | the allowance, [world](../subsystems/world.md#a-tiles-meshes-residency-by-reference) |
| Ground tiles staged | 4 MiB a frame | `renderer.terrain.tile_upload_mib` |
| Geometry pages staged | 256 KiB a frame (15 MiB/s at 60 frames a second) | `--upload-budget`'s default |
| The load's meshes read | 4,070 meshes in 8.43 s, 5,500 MiB read by the process in that time: 1.35 MiB a mesh, 650 MiB/s over the load's pool, **from a warm file cache** | the warm load W1 of the staging on 61ccce67, its startup trace (`<scratchpad>\b64\T129\loads\W1-warm-main\startup.json`) |
| Materials uploaded | 14.2 s, 3,000 MiB read: 210 MiB/s, upload included | W1 |
| Geometry uploaded | 4.5 GB in 3.3 s: 1.4 GB/s | W1 |
| Merge on the host | 6.4 s for 4.6 GB | W1 |
| Opacity micromaps baked | 8.1 s for the load's clusters | W1 |
| The whole warm load | 179 s; `scene_loaded` at 106 s, `gpu_scene` at 131 s | W1 |

A stage's "read" in that trace is the process's during the stage, so with stages in parallel it is an upper bound for the stage's own. **Not measured: a read from a cold file cache**, which is what a walker crossing new ground gets, and a tile's build by ring.

### Whether a speed outruns it

One build worker makes 1 / *t* tiles a second; a speed *v* asks for 1.58 *v*. So the workers a speed needs are **1.58 · *v* · *t***, and the speed one worker holds is **1 / (1.58 · *t*)**.

| Travel | m/s | Builds a second | Workers at 10 ms a tile | Workers at 60 ms a tile |
|---|---:|---:|---:|---:|
| Walking (1.10 to 1.65 m/s preferred) | 1.4 | 2.2 | 0.02 | 0.13 |
| Running (the owner's recorded run was 2.5 to 4.5 m/s) | 4.5 | 7.1 | 0.07 | 0.43 |
| Riding at a gallop (40 to 48 km/h) | 13.3 | 21 | 0.21 | 1.3 |
| Driving (144 km/h, a design value, not a source's) | 40 | 63 | 0.63 | 3.8 |
| The flight of 2026-10-31 | 99 | 156 | 1.6 | 9.4 |
| The owner's flight of 2026-11-13, at its fastest | 366 | 578 | 5.8 | 35 |

**So with T131's one build job at a time, walking and running are held at any tile cost on record; a gallop is held at 10 ms a tile and is not at 60 (one worker holds 10.5 m/s there); driving needs four workers at the worst cost and one at the walk's.** The ordinary speeds are therefore within reach, and what stands between the engine and them is not a rate but two things on record: tiles built where the frame waits (T131), and a placement generator's `tile` not being callable from several threads at once ([world](../subsystems/world.md#not-yet): an audit and a contract change over eight generators). The envelope's first stage after the budget is that contract.

### Ahead: lead is a time

A tile is wanted in its form when it crosses the radius; it is drawn **τ** later: its wait in the queue, its build, a commit to hand it over, a frame to flip the tables, and, when it names something not resident, the admissions (and, once geometry and textures arrive by place, their reads). With everything a tile names resident, τ is the build and two or three frames: at 40 m/s and 60 ms a tile, 3.7 m of travel, inside the 12 m band the inner ring's hysteresis already is. **So while supply holds, the lead a ground speed needs is metres, and the existing band covers it; the lead grows only with what has to be read.**

The envelope therefore does not carry a table of distances. **It measures τ**: per kind (tile, mesh, texture, page), the time from wanted to drawn over the last few hundred arrivals, and takes a high percentile. Each observer is then scored not from its point but from **the segment from where it is to where it will be τ from now at its present velocity** (a tile's score is its distance to the nearest point of that segment, and a ring is banded as it is today). At rest the segment is the point and nothing changes; walking, it is a metre long and nothing changes; driving past ground whose meshes must be read, it reaches as far as the reads take. It needs no constant per speed, it is right on a slow disk without being told, and it is right for a world of imported meshes whose sizes nobody tuned for. A velocity that changes direction faster than τ is not predicted at all: the segment is capped at the distance the observer has actually kept its heading.

**Order within the reach**: innermost ring first, then nearest to the segment, then tile order, which is the pair budget's order today with the segment in place of the point. No published source read gives an order by speed; Unreal weights by angle to the source, and the segment is that with a length.

### Behind: a turn's worth, while there is room

What is behind is held for a **time**, `t_back`, the seconds of travel a turn back would find still resident: the default is the mesh linger's two seconds (120 commits at 60 a second), and it applies to every pool. At a walk that is three metres, less than the hysteresis band, so nothing more is held than today; at 40 m/s it is 80 m. It is the first thing the governor gives up (step 2 above), so under a tight budget the engine holds nothing behind but the band, and a consumer with room to spare can set it long.

### Where it must be outrun

**The envelope is outrun when demand passes supply: *v*\* = *W* / (1.58 · *t*)** for the staging's rings, 10.5 to 63 m/s for one worker by the tile costs on record and four times that for four. The engine does not pin it: the governor sees it as a backlog whose time to drain is longer than the time its tiles will stay in the ring that asked for them. Past that speed, more workers do not help for long (the owner flew at 366 m/s, which is 35 workers at the worst cost), and building faster is not the point: at 100 m/s a tile of the inner ring is in that ring for at most 1.6 s. **That is where flight begins.**

## Flight

**Flight is not a mode the game sets; it is what the engine draws while the envelope is outrun.** A game may say so in advance (a mount takes off), which only saves the seconds the backlog would take to show it.

**What is drawn.** The far tier, everywhere, because it is resident everywhere: the far forest's per-tile forms and the ground's far levels, with the catch-up rule the ground already has (the coarser level, never nothing). Of the world's own rings, **only those whose tiles will be drawn long enough to be worth their cost**: a ring of radius *R* holds a tile for at most 2*R* / *v* seconds, and it is requested only while that passes its measured τ and a time worth showing (a second or two; a tunable, and the one number here nobody can derive). This is Cesium's "don't request tiles that will likely be unused when they come back" with the test written down. At 100 m/s the 80 m ring holds a tile for 1.6 s, which fails a two-second showing whatever τ is, and the 1,280 m ring holds one for 25 s and passes; at 366 m/s the 192 m ring (1.0 s) fails too and the 640 m ring holds one for 3.5 s. What supply is left goes to the rings that pass, nearest first, and since even they ask for more than the builders make, their radii come in until demand meets supply (the governor's step 3). So at speed the picture is far forms with coarser rings under the eye; the near field is not asked for.

**Height is part of it.** The ring scores by distance on the ground, by design (a camera 300 m up must activate the tiles under it for the simulation, [world](../subsystems/world.md)). For *forms* that is wrong at height: the tile under an eye 300 m up is 300 m away and is asked for its 80 m form. In flight a tile's drawn form takes the eye's distance in three dimensions; the simulated rings keep the ground's.

**The near field is deferred until the speed falls**, and then comes in from the eye outwards, at the frame's allowance, after a delay (Cesium's `foveatedTimeDelay`, 0.2 s there) so a pause in a turn does not start a fill the next second abandons.

**Why objects must not change form en masse in one frame.** Three measurements of exactly that, all on this engine: the owner's dash of 2026-11-01 activated 577 tiles in one update and built them in 1.5 to 2.9 s of one frame; with four builds a commit the same burst's backlog of up to 984 tiles drained over 250 updates at 19 to 43 ms a frame ([world](../subsystems/world.md#performance-notes)); and on 2026-11-13 a move of 1.1 km at 300 m/s left 955 tiles due and the run that followed paid nine seconds of 50 to 165 ms frames (T131's page). A form change is not a flag: it is a tile's build, its slots and pair entries written, at the pair cap a tight compaction of the whole layout (15 to 56 ms across the three runs) or a relayout that waits for the device (51 to 145 ms), new structures for every newly drawn cluster the chain has not cached, and new pool blocks. Each is priced per tile, so a thousand in a frame is a thousand times one. And a staggered change is hidden where a simultaneous one is seen: nothing here cross-fades between a tile's two instance sets yet ([world](../subsystems/world.md#not-yet), "The handover pops"). **The rule**: a frame hands over at most what fits a time allowance on the frame's thread (the stage names it from T131's measured 1.4 to 2.8 ms a job; a tile count, not a hope), nearest first; a tile that changed form holds it for the hold-off; the layout keeps room across a flight so the regrowth meets no relayout; and where a form has a cross-fade ([renderer](../subsystems/renderer.md#the-cross-fade)) the change goes through it.

## Fast travel

An interface of the world's (`systems/world`), reached by a game module through its interface table and by a client through the protocol (`world.travel.*`; every method a write but the status, which is `read_only`). A sketch of its shape, not a schema:

| Call | Carries | Gives back |
|---|---|---|
| `travel_begin` | the destination (a `WorldPos` and a heading), `cover` (true: the game covers the picture; false: the move is shown), `ready_ring` (how much of the destination must be settled: by default the simulated rings), and for a shown move the path the observer will take or the seconds the game's own move will last, and `on_late` (`hold` or `cover`) | a handle and an **estimate** from the session's measured rates: tiles to build, bytes to read by kind, the seconds it will take, the least duration a shown move can have, the peak the process will reach, and whether the budget holds it (and, if not, what will be released at departure) |
| `travel_status` | the handle | the phase (`releasing`, `loading`, `ready`, `arrived`, `refused`), a progress from 0 to 1 that never goes back (work done over work estimated, in seconds at the measured rates), the counts by kind, the seconds left |
| `travel_arrive` | the handle | the observer is at the destination; refused before `ready` |
| `travel_cancel` | the handle | the destination's share is released, the observer stays |

**Behind a loading screen** (`cover` true). The picture is the game's: the engine draws no world passes and its frames go on, so the game's screen animates and can show the progress. In order: **release** every pool to its floor except what the destination also names (resident by reference makes that a count, not a comparison); **load** the destination as a first fill, with every worker and no per-frame allowance, since no world frame is at stake; **ready** when the `ready_ring` has nothing waiting; `travel_arrive` moves the observer, and the first frame at the destination is drawn settled, as an offscreen capture's whole commit is today. **The peak is the floor plus the larger of the two places, never their sum**, which is why this form fits any budget that fits one place, and why it is the fallback of the other. **The first load of a session is this call from nowhere**: time to the first frame becomes the start's bytes at the measured rates, not the world's (today 131 s to `gpu_scene` on the staging, warm).

**In the world, the move not hidden** (`cover` false). A second observer is placed at the destination with the role **preload**: its tiles are built and what they name is read and admitted, but they are not drawn and hold no pairs (Unreal's "Loaded" against "Activated"). Meanwhile the traversal plays in the world: the eye leaves, and whatever the game shows (a flight over the ground, a ride, a climb to a height and a descent) is drawn by the rules above, which at travelling speeds means the far tier, resident already. As the eye comes within sight of the destination its tiles enter the scene **at the frame's allowance, nearest the arrival point first**, so that arriving changes nothing in one frame. Three things make it honest:

- **The least duration is the engine's to say.** `travel_begin` returns it: the destination's bytes and builds at the measured rates. A game stretches its traversal to at least that.
- **Late is never hidden.** If the traversal ends before `ready`, `hold` keeps the eye in the flight's form (the game circles, slows its descent, lets the horse walk the last stretch) and the status says how late; `cover` hands the remainder to the game's screen. The engine never holds a frame for it.
- **Two places at once is a statement about the budget.** While both are held the process is the floor, what the two share, the origin's near field and as much of the destination's as has arrived. If the estimate says that passes the budget, the origin's near field is released **at departure** (the ground coarsens behind a rising eye, which a flight does anyway) and the peak is again the larger of the two; if the destination's near field alone does not fit beside the floor and the flight's own forms, the shown move is **refused with the bytes**, and the covered one is the answer.

**What budget makes the shown move possible** is therefore *floor + flight's forms + the larger near field*, and *+ the smaller one* to leave the origin intact until the eye has gone. A place's near field in bytes is the number this page cannot give: T129's table does not split what a place names from what the scene holds, because today the scene holds everything. It is the first measurement of the build.

## What must be restructured first

In order. "Buys" is from T129's table and is the **most** the step can return; the share a place actually names is not measured.

| # | Step | Buys | Why here |
|---|---|---|---|
| 1 | **T129's second leg** (in hand): the 7.7 GB nobody owns; the chain's cluster storage sized by the bytes built and not the driver's most a cluster; the heaps' slack | up to 7.7 GB; part of 2.8 GB; 0.9 GB | Not streaming, but the largest bytes held for nothing, and a governor that counts them as unaccounted would otherwise spend the budget on them |
| 2 | **The host's copy of the geometry released after upload**, read back per mesh from the derived-data cache by the few readers that want floats (the static forms, collision's proxies, the pickers) | 4.7 GB of commit, 3.6 GB of it resident: the one step that lowers the working set | It is whole, it is touched, and nothing per frame reads it |
| 3 | **The pools and the governor** | nothing by itself | Everything after it needs a sum to stay under and a place to report to |
| 4 | **The load's meshes resident by reference**: the scene's own meshes become entries of the table a tile's meshes already are (ADR-0061's first "revisit when"), built or read when a held tile first names one, retired after the last | the share of 4.5 GB on the device (and of the load's 47 s) that the tiles in reach do not name | It is the mechanism that exists; it pays most where regions differ (an island, a town, a biome) and least in a forest whose every species is everywhere |
| 5 | **Geometry by page in a streamed scene**: the page pool joined with the tail, the deformed-vertex pool and the chain, so a mesh's fine pages are resident only where an instance is near enough to want them | most of the rest of the 4.5 GB: the descriptors are about 7 % of a paged mesh | The step that pays in a homogeneous world. It is also the longest: `--stream` today refuses a deformed scene and cluster templates, and the mesh room is refused to a paged scene |
| 6 | **Textures by reference for the load's own**, and by level | the materials' 14 s of the load; the device's share is inside the 3.8 GB T129 did not split | R147's table is already keyed for it |
| 7 | **Micromaps and the proxy's coverage per mesh**, baked when a mesh is admitted, kept in the derived-data cache, released with it | 0.6 GB and 8 s of the load | It is "what does not arrive, yet" on the renderer's page ([renderer](../subsystems/renderer.md#meshes-that-come-and-go)), and step 4 without it loses the micromaps for every streamed mesh |
| 8 | **The structure cache's backing by use** | part of 0.86 GB | It already evicts by use; what it lacks is giving the buffer back |
| 9 | **The far tier by region**, and the plans the load expands (54 s of `scene expand`, 21 s of `world create` on the staging) cooked and read by region | load time, and the floor of a world much larger than this one | Not needed at the staging's size (its sward's bounds are 56 by 168 tiles); it is what the floor becomes at ten times it |

Steps 2 to 4 are what a 32 GB budget on the development machine needs for the staging; step 5 is what a 16 GB machine needs.

## The general-purpose check

The owner's rule of 2026-11-13: the forest is a stress test, not the product; a consumer with only imported assets and no solver must work as well.

- **Nothing above reads a generator.** The budget is the process's. The envelope reads the scene's `world` block (a tile size and rings any scene declares), observers' positions and velocities, and times it measures. Flight's test is a radius over a speed against a measured time. No constant on this page is a solver's default: the 64 m tile and the four rings are the staging scene's own, and the tables above are worked for them as an example.
- **What an imported asset gets.** A scene file's own `instances` already stream per tile through the `authored` placement generator ([world](../subsystems/world.md#an-authored-world), item 5), each in the distance form its ring allows, and the far tier takes "every streamed instance" by its coarsest form, which any mesh with the content build's LOD DAG has; an authored heightfield is a tile set with its far levels built. So an imported world gets the envelope, flight and both travel forms as they stand. What it does not get until step 4 is its *meshes* by place: a file a scene lists is read at load whatever tile names it, exactly as a generator's resident list is. Step 4's entry is keyed by the mesh's identity (a file's bytes or a recipe's), so the imported case is the same code.
- **One finding against the rule.** The far tier's name, its page and its measurements are a forest's ("the far forest", `far_forest.cpp`, `RenderSettings::far_forest`). Read on the page, the mechanism merges any streamed instances; whether it has a test over meshes that are not trees was not checked here, and flight leans on it for every world.
- **A scene read whole, with no world block** (a level, an interior, a model viewer) has no observers and no envelope and pays nothing for them. It has the budget: the governor over its page pool (`--stream`), its chain and its cache. Fast travel between two such scenes is the covered form with a scene as the destination; the shown form needs both resident and is the same budget statement.
- **What a scene with none of it costs.** The governor is a few counters read a few times a second off the frame's thread; a pool's report is four numbers it already keeps. A scene that sets no budget gets the default and, where it fits with room, never sees a step taken.

## The build, in stages

Rows for the roadmap, proposed; the ids are the coordinator's to give. Each closes on a measurement.

| Stage | What | Closes when |
|---|---|---|
| a | **What a place names.** Counters, no policy: per kind, the bytes the held tiles name against the bytes resident; a tile's build time by ring; τ by kind; one read rate from a cold file cache | A table on this page for the staging at three places and along one walk, and the same for an engine scene of imported meshes; the near field of a place in bytes |
| b | **The pools and the governor**, `engine.memory.budget_mib`, the floor's refusal, the five steps, `engine.budgets` and the frame record carrying the sum | The staging's hop flight and a ten-minute walk at 4.5 m/s under a ceiling stage a's table names, no allocation refused by the device, and the governor's sum plus its unaccounted within 3 % of the system's figure for the process |
| c | **A placement generator's `tile` callable from several threads**, and the build workers a tunable | The eight generators audited and tested from four threads at once with the bytes of one; the staging walked at 40 m/s with tiles at their worst recorded cost and no backlog older than its ring's band |
| d | **The envelope**: τ measured by kind, the segment in place of the point, `t_back` | Straight paths over the staging at 1.4, 4.5, 13.3 and 40 m/s, a minute each, in a window: no tile of the inner two rings drawn later than its band, and no more frames over 25 ms than the same path flown with every tile already resident (a pair, the machine's state stated) |
| e | **The host's geometry released; the load's meshes by reference** (steps 2 and 4) | Commit at the staging's start under the figure stage a derives, the first frame sooner than today's 131 s by the share the start does not name, and the picture at the marks the stage names equal to a whole load's once settled (0 id, 0 depth, 0 colour with the bounce off) |
| f | **Micromaps and textures with their mesh** (steps 6 and 7) | A streamed alpha-tested mesh's shadow rays cost what a loaded one's do; the bake's 8 s gone from the load |
| g | **Flight**: the dwell test, forms by the eye's distance, the hand-over's time allowance, the layout's room kept, the delay before the near field | The staging flown at 100 and 366 m/s, at 30 m and at 300 m up, then stopped: no frame over 25 ms beyond the resident pair's, commit never above the walk's ceiling, no pixel of ground uncovered in a capture a second, and after the stop the near field settled within the estimate with no frame over 25 ms |
| h | **Travel behind a screen**, and the first load through it | Two places of the staging 5 km apart: the peak within 5 % of the larger place's, progress never going back, the time within a quarter of the estimate, the picture on arrival equal to a fresh load's there |
| i | **Travel in the world** | The same two places with a 20 s flight between: no frame over 25 ms, the peak under the budget, nothing waiting at arrival, the picture equal to a fresh load's; and with the budget set under two near fields, the departure's release said in the log and the same result |
| j | **Geometry by page in a streamed scene** (step 5) | The staging under a 16 GB budget: stage d's walks and stage g's flight pass as they did |

## What this page did not measure

- A place's near field in bytes, and what share of the load's meshes, textures and micromaps the tiles in reach name. Every "buys" above is a ceiling until stage a.
- A tile's build by ring, and why T131's page says 15 to 60 ms where the walk's frames give 4.5 to 10.
- Any read from a cold file cache. Every rate above is the warm load's.
- The owner's own world: the staging holds 42 GB where v43 with its animals held 79.
- The talks. The two GDC sessions and Guerrilla's streaming page are named and unread; if one of them gives a rule for lead by speed, it should be checked against the derivation here.
- Nothing here was run. The derivations are arithmetic over other rows' measurements, each named where it is used.
