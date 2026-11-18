# What the forest commits to start, by owner, and what a shipped game could ask (W51-2, 2026-11-18)

**The question.** The owner, 2026-11-18: if the forest proof of concept were a game to be distributed, a 60 GB minimum commit to start would make no sense, even at maximum settings; better looking and larger games fit in much less. Roadmap W51 last measured the v7 forest at 5.0 GiB before ready (2026-10-24); the world has grown since. This page measures where today's bytes are, by owner and by stage of the load, at the top settings and at a small machine's, and proposes the budget a distributed game could state and the stages that reach it. It designs only as far as the table lets it.

**The machine and its state.** RTX 5090 (32 GB), 64 GB of memory, commit limit 117.7 GB, Windows 11, driver 621.24. Shared: about sixteen agents building and testing, the processors at 100 % at every load's start, free commit 41 to 55 GB at each start and as low as 14 GB during (others' builds and loads); the session unlocked. **A commit figure does not move with the machine's load; a working set can** (Windows trims under pressure), so the working sets here are floors. Load times are long for the same reason and are not this page's subject (the staging's own warm load: 348 s, [DONE note of the staging agent], against b65's 130 s quiet).

**What ran.** The forest's staging (`<scratch>/v41/scene-staging.json`, re-migrated for 09ed028b; derived data warm, pipeline cache warm), `engine-view --offscreen`, the camera standing at the staging's own view (`--time-rate 0`), `--terrain-tiles --terrain-far 6 --terrain-async`, under the GPU lock, one load a lease:

- **Top**: 3840 × 2160, `--raster rt --shadows rt`, the bounce, opacity micromaps, flora motion (the owner's lab settings, as T129).
- **Low**: 1920 × 1080, `--raster hw --shadows csm`, no bounce, no micromaps: what a machine without ray queries draws.

Each run with `--memory-report` (`apps.md`, "--memory-report: where the commit is"), and from outside the process every two seconds the private bytes (the process's commit), the working set, and the GPU's dedicated and shared bytes for the process (`\GPU Process Memory(pid_*)`). The instrument this row added: the report's third snapshot, **`uploaded`**, taken once the scene's GPU copy exists and before its host copy is released, and **`--memory-report-floor-mib`**, which names the engine heap's and the device's blocks down to that floor and **samples the heap's smaller blocks** one per floor of bytes, each standing for that many bytes ([memory](../subsystems/memory.md#who-sized-the-largest-blocks)). The sampled estimate matched the heap's own count to 0.1 % (14,638 MiB estimated, 14,628 counted at `uploaded`) and 0.2 % at exit. Stacks were symbolized with `cdb` against the run's own PDB; a release build folds identical functions, so a frame's name can be a twin's (said where it matters).

## The three points

Commit is the process's private commit (what the commit limit is charged); on Windows it includes one byte of untouched commit per byte the engine puts on the GPU (T129: [memory-commit-accounting](memory-commit-accounting.md)).

| | Top (T3, 4K traced) | Low (T4, 1080p raster + cascades) |
|---|---|---|
| Free commit at the start | 54.5 GB | 57.0 GB |
| **Peak** (at the end of `gpu scene`, the `uploaded` moment) | **30.6 GB** commit; working set 15.0; GPU dedicated 13.7 | **24.0 GB**; working set 14.3; GPU 8.1 |
| **Ready** (first frames) | **27.5 GB**; working set 11.6; GPU 14.5 | **20.3 to 20.9 GB**; working set 10.8 to 11.1; GPU 8.2 |
| **After a minute standing** | **25.0 GB**; working set 11.7; GPU 11.9 | **21.0 GB**; working set 11.2; GPU 8.4 (exit snapshot: engine heap 10.6 GB, device-local 8.4) |
| The first measurement (T1, a binary without the sampler) | peak 30.7, after a minute 27.7, standing 25.2 | |

The staging agent's own loads of the same scene: a warm load peaked at 29.97 GB, a cold one (pipelines cold, meshes warm) at 32.22 GB, and the cook in the first cold leg at 30.1 GB: **about 30 GB is committed whether or not anything is cooked**; the cook adds 2 GB and the pipeline compile time, neither of which a shipped game pays (it ships its derived data). The 60 GB the owner saw were earlier builds' and the owner's larger world (T129's 76 to 79 GB had 7.7 GB of profiler heaps since removed, and the v43 world is larger than the staging).

## The curve: which stage brings which bytes

Top, T3 (private / working set / GPU dedicated, GiB, seconds from start):

| Stage ends at | Commit | Working set | GPU | What arrived |
|---|---|---|---|---|
| scene file read (~185 s on this loaded machine) | 1.5 → 2.5 | 1.3 | 0 | the scene's documents and plans |
| `scene loaded` (265 s) | **16.3** | 14.8 | 0 | every mesh's container read into host memory, the merged geometry built |
| `gpu scene` (383 s): **the peak** | **30.4 to 30.6** | 14.9 | 13.7 | the device copy (13.7 GB, and as much of commit), while the host copy is still whole |
| host copy released, renderer, world (≈ 405 s) | 27.2 | 11.8 | 14.5 | −3.4 GB host geometry; the world's tiles +0.3 |
| the traced chain's first capacity released (≈ 485 s, `renderer.rt.first_seconds`) | **25.0** | 11.7 | 11.9 | −2.6 GB: the chain's frame sets at their provisional first size (ADR-0068) |

**Before the first frame the load holds 16.3 GB on the host and 13.7 GB on the device; the frame adds almost nothing; a minute later 2.6 GB of the chain goes.**

## The 30 GB by owner

At `uploaded` (the peak) and after a minute (exit), MiB. Host owners are the engine heap's sites estimated by the sampler (2 MiB floor and interval); device owners are the buffers of 2 MiB or more by the line that made them, and `place` for the images.

| Owner (the line that sized it) | Peak | After a minute | Holds per unit | Needed |
|---|---:|---:|---|---|
| **Host: each mesh's `assets::MeshData` kept in `renderer::SourceMesh`** (`SourceMesh::data`, copied at `load_scene`; by the stack its `images` — the embedded image bytes; ICF may name a twin, to verify) | **7,330** | **7,330** | the source images of 4,236 meshes, ~1.7 MiB a mesh, 5.9 GB of it resident | never after the textures are built and uploaded (the container records its built `.tex`, R147/ADR-0036) |
| Host: `u32` streams of generated meshes' containers (`geometry::read_cluster_file_memory` ← `renderer::load_generated_mesh`), held after the load | 2,520 | 2,520 | ~2 MiB a generated mesh, all resident | not at the start once uploaded (a tile mesh re-reads by reference, ADR-0061) |
| Host: the merged geometry (`geometry::merge_cluster_meshes`: attributes 718, positions, triangles, sources, quantized) | 3,500 | 149 | 12 + 4 + 4 bytes a vertex/triangle, 94 M vertices | released after the upload today (T138); the descriptors stay |
| Host: the scene's records (`FlatMap<std::string,…>` JSON trees), water, authored instances, the world's tiles (`PlacementTiles::create` 68), masks (`tileset::MaskLevel` 20) | ~600 | ~600 | | before the first frame (the plan), the masks by tile |
| **Device: the traced chain's frame sets** (`make_rt_step` → `create_cluster_storage`, 6 chunks) | **2,787** | 69 | 8,832 bytes a cluster at the driver's most with micromaps | a first capacity before any frame has asked (ADR-0068); a minute later 69 MiB |
| **Device: textures** (images, `place.textures`) | **3,150** | 3,150 | the scene's built `.tex` at every level | the levels a frame samples (R172: 56 % never sampled on the village walk) |
| Device: the scene's geometry (`upload_geometry`: positions, attributes, triangles, tables 1,411 + 941 + 768 + 705 + 471 + 579 in pieces) | 2,656 + 721 companions | same | the whole load's meshes | the share the tiles in reach name (W47 step 4/5) |
| Device: tiles' arenas and the terrain's rings (`place.tiles`) | 1,215 | 1,215 | the mesh room, terrain rings | within the first ring |
| Device: the frame's own buffers and targets (`place.frame`) | 1,080 | 1,080 | the visible list, pair tables, deform pool, the bounce's cache | yes, sized by the frame and the resolution |
| Device: the structure cache (`create_rt_cache` 506 + slots 506) and `create_ray_tracing` (353) | 1,365 | 1,036 + 353 | a fixed ceiling from the device | by use, not at the start |
| Device: static forms (`build_static_forms`, 297 + 36) | 333 | 352 | | with their meshes |
| Device: materials (`upload_materials`, 348; masks within) and micromaps (0 at the peak, 369 after) | 364 + 0 | 364 + 369 | | materials yes; micromaps with their meshes |
| The driver's backing for every device-local byte (commit only, untouched) | 13,718 | 11,744 | one byte a byte | — |
| The C runtime heaps' slack beyond the engine heap; images; no named owner | ~2,400 | ~2,300 | | |

**Masks.** The coordinator asked what the mask levels cost against what they hold: on the host the mask level's archive index and windows are 20 MiB of sampled sites; on the device the terrain masks are inside the materials' 364 MiB. The nine layers the load drops ("under one quantization step") cost nothing this table can see; they are not where the bytes are.

**Reading it for a player's machine** (the owner's rule: memory first, how it scales down):

- **Scales with the world's size, so must stream**: the source images kept in `SourceMesh` (7.3 GB, by mesh count), the generated meshes' streams (2.5 GB), the device geometry and companions (3.4 GB), the textures (3.2 GB), the merged host copy at the peak (3.5 GB). These are 20 of the 30 GB, and all of them are held whole for the session wherever the eye is.
- **Scales with the settings, so must have a lower setting**: the frame (1.1 GB at 4K), the traced chain (2.8 GB first, 0.1 to 1.2 GB after), the structure cache (1.0 GB) and the micromaps (0.4 GB). A machine without ray queries pays none of the chain, the cache or the micromaps: at 1080p raster the device holds 8.4 GB (geometry 1.7, companions 0.4, textures 3.2, tiles 0.9, frame 1.1, materials 0.4, no chain or cache) and the commit 21.0 GB, of which the host is still 10.6 GB of engine heap: **the low setting saves the device 3.4 GB and the host nothing**, because the host is held by the world's size, not by the settings.
- **Fixed**: the executable and its libraries (0.4 GB), the long-lived pools' threads, the C runtime's slack.
- **The copy kept on the host after an upload**: the images' bytes (7.3 GB) are the largest; the floats the acceleration-structure builders read are already gone (T138; 149 MiB of descriptors remain); the masks are small. **A file read whole that could be mapped**: the generated meshes' containers (2.5 GB held after the read) and the source images.

## What a shipped game asks, read on this run

Each from the publisher's or the store's page, read through the fetch tool on 2026-11-18 (nothing downloaded):

| Game | Minimum | Recommended / top | Source |
|---|---|---|---|
| Cyberpunk 2077 (update 2.0) | 12 GB RAM, 6 GB VRAM (1080p low) | 20 GB RAM, 12 GB VRAM (2160p ultra); **24 GB RAM, 16 GB VRAM** (2160p, ray tracing overdrive: path traced) | CD Projekt support, "Cyberpunk 2077 system requirements" |
| Horizon Forbidden West Complete Edition | 16 GB RAM, 4 GB VRAM | 16 GB RAM (RTX 3060) | Steam store page |
| Monster Hunter Wilds | 16 GB RAM, 6 GB VRAM | 16 GB RAM, 8 GB VRAM | Steam store page |
| S.T.A.L.K.E.R. 2 | 16 GB RAM, 6 to 8 GB VRAM | 32 GB RAM (RTX 3070 Ti / 4070) | Steam store page |
| Microsoft Flight Simulator 2024 | 16 GB RAM | 32 GB RAM | Steam store page |
| Alan Wake 2 | 16 GB RAM, 6 GB VRAM | 16 GB RAM at every tier, path tracing at 4K included (RTX 4080, 16 GB) | press reports of Remedy's announcement (Remedy's own page not read: 404) |

Not read: Red Dead Redemption 2 (the store's age gate; Rockstar's support page timed out). A published "RAM" is physical memory, not commit: on Windows a game's commit is its host bytes plus the driver's backing of its GPU bytes, and the page file covers the latter, which nothing touches. **The open-world games of this scope ask 16 GB of memory at the top and 12 to 16 at the bottom, and 12 to 16 GB of GPU memory at 4K with path tracing.**

## The budget proposed

Stated the way a store page states it, with the commit it implies on Windows:

| Setting | Memory (host: working set and host commit) | GPU memory | Commit on Windows (host + the driver's backing) |
|---|---|---|---|
| Top (4K, traced, bounce) | **16 GB machine: host ≤ 8 GB** | **12 GB card: device ≤ 10 GB** | ≤ 18 GB |
| Low (1080p, raster, cascades) | **16 GB machine, host ≤ 6 GB** (12 GB at a stretch) | 6 GB card: device ≤ 4.5 GB | ≤ 10.5 GB |

Sources for the split: Cyberpunk's path-traced 4K tier (24 GB / 16 GB) is the upper bound of the genre; Alan Wake 2 and Horizon run their tops on 16 GB of memory. **The coordinator's working target, 12 GB of commit at the top settings, is right for the host and wrong for the commit as Windows counts it**: commit includes the GPU's bytes, and no 4K traced game asks for less than 12 GB of GPU memory alone. So: replace "12 GB of commit" with **host ≤ 8 GB and device ≤ 10 GB at the top (18 GB of commit), host ≤ 6 GB and device ≤ 4.5 GB at the low setting**; that is what runs on a 16 GB machine with a 12 GB card, and on a 16 GB machine with a 6 GB card.

## The stages, in order of bytes returned per unit of work

| # | Stage | Returns (top, from the table) | Work |
|---|---|---|---|
| 1 | **Do not keep the source image bytes in `SourceMesh` once the built textures exist** (or never copy them: the container names its `.tex`) | **−7.3 GB host commit, −5.9 GB working set, at the peak and standing** | small: one release after the texture pass, as T129's streams were |
| 2 | **Release the generated meshes' `u32` streams after the upload** (or read them by reference as a tile mesh is) | −2.5 GB host, all resident | small to medium |
| 3 | **The traced chain's first capacity from what the first frame asks**, not the provisional maximum (ADR-0068's revisit) | −2.7 GB device and commit at ready (the minute's figure arrives at once) | small, a policy in `RtCapacity` |
| 4 | **Upload without holding the whole host copy beside the device copy** (per-mesh upload and release, or a staged merge) | −3.5 GB at the peak only | medium |
| 5 | **Texture residency on** (R172, built and off): a ceiling from the budget | up to −1.7 GB device (R172's 56 % never sampled; not measured on the staging) | small: turn on with a governor's ceiling |
| 6 | **The residency manager (W47) over geometry, companions, tiles and the structure cache** | most of the 4.4 GB geometry and companions and part of the 1.4 GB cache | large (ADR-0063 stages b to j) |

After stages 1 to 3 the top setting stands at about **12.5 GB of commit with 2 GB on the host and 10.5 GB on the device** (25.0 − 7.3 − 2.5 − 2.7 at ready), and peaks at about 18 GB until stage 4. **So the host's 8 GB is one stage (stage 1 alone takes the host from 11.3 to 4 GB standing), the device's 10 GB is stages 3 and 5, and the peak's is stage 4: four small-to-medium stages before W47's residency manager, which is what the low setting and a larger world need.**

## W51-3: what the stages returned

**The owners, made sure of** (W51-3, 2026-11-18). A release build folds identical functions, so the
table's frame names were checked in a build that does not: the base's own debug `engine-view` with
the instrument (`--memory-report-floor-mib 1`) on `content/test-scenes/streamed-forest` (five
generated trees and their eight authored forms), its `uploaded` snapshot's sites symbolized with
`cdb` against its PDB.

- **Stage 1's 7,330 MiB are the image bytes copied into every form**: `Vector<u8>` copy-constructed
  inside `assets::ImageRef`'s copy, inside `Vector<ImageRef>`'s, inside `MeshData`'s, inside
  `SourceMesh`'s — the copy `stream_authored`, `make_placement_forms` and `offer_far_forms` make of
  a mesh for each form, embedded images and all.
- **Stage 2's 2,520 MiB are not `u32` streams**: the frame under `read_cluster_file_memory` is
  `Vector<u8>::resize`, the container's image bytes, which `adopt_container` moves into the original
  mesh's `ImageRef::bytes`. They are the same owner as stage 1 — the originals where stage 1 is the
  copies — and stage 1's release returns both, so **stage 2 is stage 1** and has no change of its
  own.
- A third owner the same check names: each form's copy of the per-vertex motion records and the
  part map, cleared and refilled with the form's own subset — `clear()` keeps the source's whole
  capacity, so every form of a tree held a whole tree's records (W51-2's `SourceMesh::SourceMesh`
  sites, about 500 MiB). Given back now (`Vector()` assigned, not cleared), in the same commit.

**Stage 1** (renderer.md, "The embedded image bytes go once the textures are built"): the load
empties a mesh's image bytes once every image its materials sample has its `.tex` and the mesh has
a container to read them back from; the readers that come late read the container again.

## Not measured

- The cold load with a cache of its own: a cold load of the whole staging needs a 20-minute lease and 40 GB free (the staging agent's DONE note), and its cook does not ship; its peak is the staging agent's 32.2 GB.
- The first-frame snapshot: a plain `--frames` run takes none (only the replay and path loop does); ready is read from the curve.
- The owner's own world (v51) and a walk: run with `--memory-report-floor-mib 2` and read the three snapshots.
- Linux, which charges no commit for the GPU's bytes.
- Standing still the terrain's rings were rebuilt every 3 s (1.1 to 2.1 s each, about 150 MB uploaded) and some 40 tiles activated and deactivated a tick: not memory, noted for the rows that own them.
