# What the forest commits to start, by owner, and what a shipped game could ask (W51-2, 2026-11-18)

**The question.** The owner, 2026-11-18: if the forest proof of concept were a game to be distributed, a 60 GB minimum commit to start would make no sense, even at maximum settings; better looking and larger games fit in much less. Roadmap W51 last measured the v7 forest at 5.0 GiB before ready (2026-10-24); the world has grown since. This page measures where today's bytes are, by owner and by stage of the load, at the top settings and at a small machine's, and proposes the budget a distributed game could state and the stages that reach it. It designs only as far as the table lets it.

**The machine and its state.** RTX 5090 (32 GB), 64 GB of memory, commit limit 117.7 GB, Windows 11, driver 621.24. Shared: about sixteen agents building and testing, the processors at 100 % at every load's start, free commit 41 to 55 GB at each start and as low as 14 GB during (others' builds and loads); the session unlocked. **A commit figure does not move with the machine's load; a working set can** (Windows trims under pressure), so the working sets here are floors. Load times are long for the same reason and are not this page's subject (the staging's own warm load: 348 s, [DONE note of the staging agent], against b65's 130 s quiet).

**What ran.** The forest's staging (`<scratch>/v41/scene-staging.json`, re-migrated for 09ed028b; derived data warm, pipeline cache warm), `engine-view --offscreen`, the camera standing at the staging's own view (`--time-rate 0`), `--terrain-tiles --terrain-far 6 --terrain-async`, under the GPU lock, one load a lease:

- **Top**: 3840 × 2160, `--raster rt --shadows rt`, the bounce, opacity micromaps, flora motion (the owner's lab settings, as T129).
- **Low**: 1920 × 1080, `--raster hw --shadows csm`, no bounce, no micromaps: what a machine without ray queries draws.

Each run with `--memory-report` (`apps.md`, "--memory-report: where the commit is"), and from outside the process every two seconds the private bytes (the process's commit), the working set, and the GPU's dedicated and shared bytes for the process (`\GPU Process Memory(pid_*)`). The instrument this row added: the report's third snapshot, **`uploaded`**, taken once the scene's GPU copy exists and before its host copy is released, and **`--memory-report-floor-mib`**, which names the engine heap's and the device's blocks down to that floor and **samples the heap's smaller blocks** one per floor of bytes, each standing for that many bytes ([memory](../subsystems/memory.md#who-sized-the-largest-blocks)). The sampled estimate matched the heap's own count to 0.1 % (14,638 MiB estimated, 14,628 counted at `uploaded`) and 0.2 % at exit. Stacks were symbolized with `cdb` against the run's own PDB; a release build folds identical functions, so a frame's name can be a twin's (said where it matters).

## The three points

**Correction (W51-3, 2026-11-18): these loads were not standing.** An offscreen `engine-view` with no `--camera-path` flies `renderer::orbit_camera`, which on the staging swept about 300 m a frame (185 tiles in and out a tick, 48 ground rebuilds, 7.4 GB uploaded over 3,600 frames; found by roadmap row W149), so the peak and its owners (before any frame) stand, "ready" is nearly right, and "after a minute standing" was taken on that orbit and is not a standing figure; [W51-3's](#w51-3-what-the-stages-returned) after-run flies the scene's one-place path and gives the first standing one.

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

**How it was measured.** As W51-2's T3 and T4, one load a lease, warm (derived data and pipeline
cache), 1,200 frames standing, `--memory-report-floor-mib 2`, the process sampled from outside every
two seconds; the binary is the branch's own `msvc-release` build of each stage. The machine: the
processors at 99 % from other agents' builds at every start, the session unlocked, free commit
written beside each run. **The working sets after the peak are floors**: with free memory this low
Windows trims a process's working set (W51-2's caveat), and these runs had less free memory than
W51-2's (14.6 GB of commit at the lowest against 41 to 55 at its starts).

| Top (4K, traced, bounce) | W51-2's T3 (the base) | Stage 1 (S1-top) |
|---|---:|---:|
| Free commit at the start / lowest during | 54.5 / — GB | 40.0 / 14.6 GB |
| `scene loaded`: commit | 16.3 GB | **5.96 GB** |
| **Peak** (end of `gpu scene`): commit / working set / GPU | 30.6 / 15.0 / 13.7 GB | **20.5** / 6.1 / 14.0 GB |
| **Ready** (first frames): commit / working set / GPU | 27.5 / 11.6 / 14.5 GB | **17.4** / 2.6 / 14.5 GB |
| After a minute **on the orbit** (not standing; see the correction above): commit / working set / GPU | 25.0 / 11.7 / 11.9 GB | 15.0 / 2.8 / 11.9 GB |
| `uploaded` snapshot: commit / engine heap | 30,560 / 14,628 MiB | 19,966 / **4,708** MiB |
| exit snapshot: commit / engine heap | 25,561 / 11,410 MiB | 15,150 / **1,471** MiB |
| The image bytes in the forms' `SourceMesh` copies | 7,330 MiB | **0** |
| The originals' image bytes (W51-2's "`u32` streams") | 2,520 MiB | **0** |
| The forms' copies of the motion records and part maps | ~500 MiB | **0** (the originals' 50 MiB remain) |
| The merged host copy at the peak | 3,502 MiB | 3,502 MiB (stage 4's) |

**Stage 1 returns 10.1 GB of commit at every point of the load** — before the first frame, at its
peak, and a minute on (both runs on the orbit) — and 9.9 GB of the engine heap; the device is unchanged. The engine scenes say
the same in small, with pictures byte-identical (the captures of `streamed-forest` and `grove`,
1280 × 720, six frames, base and stage 1, compared by hash): `streamed-forest`'s engine heap at
`uploaded` 156 → 132 MiB and at exit 58 → 35, `grove`'s 17 → 13 and 9 → 5; the load's log names
what went (`embedded image bytes released`: 6 meshes, 40 images, 7.9 MB; 4, 18, 3.9 MB).

**Stage 4** (renderer.md, "The upload-only streams go inside `create`"): of the merged host copy
that stood beside the device copy at the peak (3,502 MiB: the float positions 1,077, the attributes
718, the 16-bit grid 538, three `u32` streams of 359, 359 and 302), the attributes, the grid and the
vertex ids now go right after the geometry's upload, before the textures (3.1 GB), the working set
and the chain's first capacity (2.8 GB) are made; the floats and the triangles stay for the chain's
8-bit indices and the collision's copy, as T138 left them. On the engine scenes, pictures
byte-identical to the base again: `streamed-forest`'s engine heap at `uploaded` 132 → 89 MiB (the log:
19.5 MB of grid and 26.0 MB of attributes given back inside `create`, the 61 MB of floats at the
host's release), commit 2,274 → 2,231 MiB; `grove`'s heap 13 → 9 MiB. On the staging the same rows
fall by 1.26 GB at the peak and more if the 302 MiB stream is the ids (the owner rows above).
**Not flown on the staging yet**: the one after-run with both stages, standing on the scene's
one-place path (`--camera-path`, W149's `standing-path.json`), was asked five times between 19:23
and 20:11 and refused each time at its start for free commit (19 to 32 GB against the 30 GB its
20.5 GB peak needs with room), the machine full of other rows' loads. So the staging has stage 1's
peak and ready (above, on the orbit), stage 4's share by its owner rows, and **no standing figure
yet, before or after**: the first standing minute is owed by whoever next flies the staging, with
`--camera-path` and `--memory-report-floor-mib 2`.

## W51-4: a launch standing, and the next stages

**How it was flown** (2026-11-19). Every load stands at the staging's one-place path (`--camera-path` with both keys at one camera, W149's), so the frames after the first are a player standing still; the rest of the command is the staging's own (DONE note: `--terrain-tiles --terrain-far 6 --terrain-async --time-rate 0`, flora motion), at the **top** setting (3840 × 2160, `--raster rt --shadows rt`, the bounce, micromaps) and the **low** one (1920 × 1080, `--raster hw --shadows csm`, no bounce, no micromaps), with `--memory-report-floor-mib 2` where the binary has it and the process sampled from outside every two seconds. Three binaries, the same evening: **b68** (main before any of W51's stages, 09ed028b), **b69** (main with stage 1 and stage 4, 1f5ff68f) and **the tip** (this row's, b69 plus stage A below). The derived data is a copy of the staging's own read set under the row's scratch (the clusters, textures and far-forest entries the 09ed028b loads read; everything else linked), because batch 69's main rekeys about forty broadleaf trees and a load of it cooks them: the first load of b69 (a declared 20-minute lease) cooked them and compiled its pipelines into that copy, and is not in the table. "GB" here is 2³⁰ bytes, as the page's other tables. The machine: processors at 80 to 100 % from other rows' builds at every start, the session unlocked, free commit 50.9 to 52.6 GB at each start and as low as 16 GB during; the working sets are floors, as above.

**Where the load's maximum is now.** Until stage 1 the commit peaked at the end of `gpu scene`, where the host copy stood beside the device copy. After stages 1 and 4 it peaks **at the first frame**: the world's first commit builds every mesh its tiles name (`world::TileMeshes`, below) and the traced chain resizes its first capacity, which the device's own usage shows (16.8 GB at that moment against 14.4 at ready and 13.4 after a minute). So a stage that lowers the end of `gpu scene` lowers the stretch from there to the first frame and not the maximum.

| Top (4K traced, bounce), standing | b68 | b69 (stages 1, 4) | tip (stage A) |
|---|---:|---:|---:|
| End of `gpu scene` (`uploaded`): commit / engine heap / host geometry | — (no snapshot) | 18.0 GB / 3,120 / 1,887 MiB | **16.3 GB / 1,381 / 149 MiB** |
| Maximum, and when | 32.4 GB at the first frame (with a cook, below) | 22.7 GB at the first frame | 22.7 GB at the first frame |
| Ready (first frame): commit / device | 30.5 / 13.9 GB | 19.8 / 14.4 GB | 19.0 / 14.4 GB |
| One minute standing | 28.9 GB | 19.1 GB | 19.1 GB |
| Standing at the run's end | 28.7 GB (+2 min) | 18.9 GB (+4.7 min) | 18.8 GB (+3.4 min) |
| Exit snapshot: commit / engine heap / C runtime heaps (committed / used) / device-local | 29,405 / 13,613 / 14,839 / 14,365 / 13,384 MiB | 19,302 / 3,707 / 4,886 / 4,368 / 13,311 | 19,300 / 3,707 / 4,878 / 4,353 / 13,311 |

The standing curve is flat from the first minute in every run (within 0.3 GB), so the run's end stands for five minutes; the runs ended earlier than five minutes where the machine drew faster than the first calibration (28 ms a frame at b69's start, 19 at the tip's). **b68's top run is not clean**: it was the first through the row's cache copy, which lacked 349 of the channels' clast meshes, and its world built them in the first minute (its maximum and its first minute carry that cook; its end does not). W51-2's T3 is the clean figure for b68's peak (30.6 GB, on the orbit).

| Low (1080p raster, cascades), standing | b68 | b69 (stages 1, 4) | tip (stage A) |
|---|---:|---:|---:|
| End of `gpu scene` (`uploaded`): commit / engine heap / host geometry | not flown standing (W51-2's T4 on the orbit: peak 24.0 GB) | 12.2 GB / 3,074 / 1,854 MiB | **10.5 GB / 1,366 / 146 MiB** |
| Maximum, and when | — | 15.8 GB at the first frame | 15.6 GB at the first frame |
| One minute standing / at the run's end (+5 min) | 21.0 GB after a minute (T4, orbit) | 15.8 / 15.6 GB | 15.6 / 15.3 GB |
| Exit snapshot: commit / engine heap / C runtime heaps / device-local / the frame's own | — | 15,955 / 3,408 / 4,702 / 10,451 / 2,923 MiB | 15,693 / 3,408 / 4,677 / 10,451 / 2,923 |

The low pair was flown 40 minutes apart (b69's on a quieter machine, 8 % against 40 % of the processors); the two pictures at the run's end, 12,000 standing frames each, are the same file (SHA-256 `FD8CF17D6291F45E…`, 8,295,480 raw bytes, none differing). **At the low setting the device holds 10.5 GB standing, of which the frame's own is 2.9 GB** (1.1 at the end of `gpu scene` and on W51-2's orbit): standing at this view the visible runs and the pair tables grow with the world's tail to nearly three times their load size, which is the largest single thing between the low setting and a 4.5 GB card after the textures.

**Stage A: the floats go inside `create`** (renderer.md, "The floats go inside `create`"). Of the merged host copy stage 4 kept through `create`, nothing in `create` read the float positions or the per-vertex source indices after `measure_host_reads`, and the chain reads only the triangles, to pack its 8-bit indices; the collision's copy came after `create` only because engine-view made it there. engine-view makes it first now, and `create` gives the floats and the source indices back with the upload-only streams and the triangles after the pack: on the staging the log says 3.20 GB inside `create` (1.51 GB of it floats and source indices) and 0.32 GB of triangles after the pack, and the `uploaded` snapshot's host geometry falls from 1,887 to 149 MiB (the descriptors). The commit from the end of `gpu scene` to the first frame stands 1.7 GB lower (16.1 GB against 17.1 to 19.0); the maximum and the standing figures do not move, because neither held the floats any more. Pictures: the low pair's last frames are the same file (above); `streaming: a whole scene draws the same bytes when create gives its host geometry back` holds the cascaded and the traced pictures of a scene that gave everything back to one that kept it, byte for byte and on both tiers.

**What the host holds standing, by owner** (b69 and the tip alike, exit snapshot, heap sites symbolized with `cdb`): the engine heap's 3.7 GB is

| Owner | MiB | Who reads it later | Released? |
|---|---:|---|---|
| **Meshes the tiles name that the mesh room withholds**: their built textures read for the admission (`load_placement_textures`, 1,352) and their merged streams (`load_placement_mesh`, about 820) | **~2,170** | the admission, when the room has space (`TileMeshes::admit`; the room was full: 52 meshes resident, 99,902 of 101,989 clusters, 189 withheld, `renderer.meshes.budget_mib` 256) | No: the reader is real; W47's stage 3 (the root pool) and the room's budget are where it goes |
| The `authored` entry's instances as a JSON tree (`schema::to_json<AuthoredSet>`, scene.cpp) beside the generator's typed copy (43) | ~280 | the world's and the collision's opens of the entry | No (two readers); a typed hand-off instead of the tree is the next cut |
| The waters' depth maps (`SceneWater`) | 168 | the swash's numbers when the wind turns (`water_swash_numbers`) | No |
| The forms' and originals' motion records (`stream_authored`, `load_generated_mesh`, `flora::motion_records`) | ~250 | the tail's forms | Not measured further |
| The cluster and LOD descriptors | 149 | every frame's cull | No |
| The world's tiles' instances (`PlacementTiles`) | 132 | the world | No |

Beside the engine heap: **1.2 GB of the C runtime heaps is not the engine's** (4.4 GB used against 3.7), and **0.5 GB is free but committed**. The free part does not come back by asking: a scratch program that frees six of every seven of 1.5 GB of mixed blocks keeps 1.45 GB private, and `_heapmin` and `HeapCompact` return none of it (fragmentation; freed whole, the heap gives it back by itself). An allocator that returns whole spans (ADR-0012's mimalloc) or load-scoped arenas is the cure, not a call.

**What a launch commits, after each stage** (standing, GB of commit; the stages are this page's):

| | Top: maximum / standing | Low: maximum / standing |
|---|---:|---:|
| Main before W51's stages (b68) | 30.6 (W51-2 T3) / 28.7 | 24.0 / 21.0 (W51-2 T4, orbit) |
| Stage 1 (W51-3, on the orbit) | 20.5 / 15.0 (orbit) | — |
| Stages 1 and 4 (b69) | 22.7 / 18.9 | 15.8 / 15.6 |
| Stage A (the tip) | 22.7 / 18.8 (16.3 at the end of `gpu scene`, was 18.0) | 15.6 / 15.3 (10.5 at the end of `gpu scene`, was 12.2) |
| Next: the chain's first capacity from the first frame (stage 3) and no tile mesh built that the room cannot take | about 20 / 16.6 (estimates: the 2.4 GB device transient and the 2.2 GB of withheld meshes) | about 13.4 / 13.1 (no chain at this setting) |

**What is left for the residency manager** (W47), on the tip at the top setting standing (exit, MiB): device-local 13,311 — textures 3,150, geometry 2,656, the frame's own 2,272 (twice the orbit's 1,080: standing at this view the visible runs and pair tables grow to it), structures 1,346 (the cache 1,036, static forms 352), tiles 1,218, companions 721, micromaps 369, materials 364 — which Windows charges once more as the driver's backing, all of it in the commit; and on the host the withheld meshes' 2.2 GB, which are the mesh room's. Of the 18.8 GB standing 13.0 is the device's and 5.8 the host's, and every large host owner left has a reader after the upload: what nothing reads again is the C runtime heap's free 0.5 GB, an allocator's matter.

## W51-5: the withheld meshes, and the runs sized by the frame

**How it was flown** (2026-11-19, the evening after W51-4): as W51-4 flew it — the staging standing at W149's one-place path, `--memory-report-floor-mib 2`, sampled from outside every two seconds, one load a lease — against batch 69's main (`b69`, 1f5ff68f) the same evening, with the row's own copy of the derived data (W51-4's read set, linked, and a copy of its pipeline cache), and the walk of R152 (`walk-path.json`, 3,596 frames, about 1.5 km in a minute through the staging's middle rings) at the low setting with `--benchmark` for the arrivals' frame times. **c1** is the row's first commit alone (the withheld meshes); **the tip** is both stages.

**Stage B: a withheld mesh is let go and built again** (world.md, "A withheld mesh is let go and built again"). W51-4 found the host's largest owner standing to be the mesh room's withheld tile meshes: 189 meshes the room refused (52 resident, `renderer.meshes.budget_mib` 256), each keeping its merged streams and its textures read for the admission — about 2.2 GB that nothing drew. The room's refusal now shelves a built mesh: its streams and its texture references go, its counts stay (the tile's pairs are counted from its clusters, and the room is asked with its clusters and materials before it is built again), and it is built again, off the committing thread and out of the derived-data cache, only when the room can take it. **Why that and not never building it**: the counts need a build, and the room frees in an order the world decides a commit at a time. **What the admission costs when it comes**: a second build on a worker, so a mesh that was withheld arrives a few commits later than it would have, drawn as its tile drew meanwhile; the committing thread collects and never reads, so no arrival stalls a frame (the walk's pair, below).

| Top (4K traced, bounce), standing | b69 | c1 (stage B) |
|---|---:|---:|
| End of `gpu scene` (`uploaded`): commit / engine heap | 18.0 GB / 3,120 MiB | 16.3 GB / 1,381 MiB |
| Maximum, and when | 22.7 GB at the first frame | 20.4 GB at the first frame |
| Ready (first frame): commit / device | 19.7 / 13.4 GB | 17.5 / 14.4 GB |
| One minute standing | 19.1 GB | 16.8 GB |
| Exit snapshot: commit / engine heap / C runtime heaps (committed / used) / device-local | 19,278 / 3,707 / 4,891 / 4,368 / 13,311 MiB | **16,912 / 1,516 / 3,385 / 2,161 / 13,311** |

The machine: b69's at 21:12 (free commit 47.6 GB at its start and as low as 4.1 GB during, from other loads; the processors at 68 %), c1's at 21:44 (51.8 GB free, 20.8 at the lowest; 48 %); the session locked. The device's own blocks are the same to the MiB, and the first frame's device transient (16.8 GB against 14.4 at ready, the chain's resize: W51-6's) is in both maxima. Of the 2.37 GB the exit commit fell by, the engine heap's share is 2.19 GB, the rest the C runtime heaps' committed slack that went with them. The room held the same meshes on both sides (top: 52 resident, 99,902 clusters, 189 tiles withheld, their 297 meshes shelved with no host bytes; low: 89, 178,965 and 169), and the low setting's last frames are the same bytes (below).

**The arrivals, on the walk** (low setting, three repeats of 3,596 frames each side; b69 at 22:47 with the processors at 12 %, c1 at 23:13 at 2 %; the room full along the whole walk, 239 changes of what it withholds on b69's side and 491 on c1's — a shelved mesh's tile reads as waiting rather than refused during its second build, so it is said again when it is refused next — with 329 meshes shelved and no host bytes at c1's end):

| Walk, per repeat | b69 | c1 |
|---|---|---|
| Frame p99, ms | 15.5 / 11.7 / 10.8 | 10.5 / 9.3 / 8.8 |
| Longest frame, ms | 183.7 / 50.4 / 46.9 | 103.0 / 26.8 / 97.3 |
| Frames over 33 ms | 6 / 3 / 3 | 2 / 0 / 1 |
| The admissions' longest commit (`world.admit_ms`), ms | 0.40 / 3.91 / 3.19 | 0.80 / 0.75 / 0.18 |

No arrival stalls a frame: the admissions' time on the committing thread stayed under a millisecond, and c1's two long frames follow a 101 ms commit of the ground's consumer and a 96 ms world update, neither the room's (b69's longest follows a 182 ms update). What the second build costs is when a withheld mesh arrives, a few commits later, and the frames do not see it.

**Stage C: the visible list's runs are sized by the frame** (renderer.md, "The visible list's runs are sized by the frame"). The device's own bytes standing were 2.3 GB at the top setting and 2.9 at the low one, against 1.1 at the end of `gpu scene`. By owner (the device's block log at exit, symbolized; low setting, W51-4's tip, whose device blocks are b69's to the MiB):

| Buffer | MiB | Indexed by | Sized by, before | Now |
|---|---:|---|---|---|
| The visible list (`visible`): 3 runs a view and one a cascade, 8 bytes an entry | 842 | entry | runs × pair stride (15.8 M) | runs × the run capacity |
| The pool's per-entry slots (`deform_slots`) | 421 | entry | the same | the same as the list |
| The table sets' pair tables (`pairs`, two sets) | 240 | pair | pair stride | unchanged: the scene's numbering |
| The cross-fade's tables (`fade`), a view and the cascades | 240 | pair | pair stride | unchanged |
| The table sets' instance tables (two) | 108 | instance slot | instance capacity | unchanged |
| The occlusion flags (two) and the pair-to-entry table | 120 + 60 | pair | pair stride | unchanged |
| The frame's tables' staging and the instance pass's items | 56 + 50 | — | the change, the instances | unchanged |

What a pair indexes is the scene's and stays by the stride: a pair's flag, fade split, entry or place in the table is read by its number from any frame. What an entry indexes is the frame's: a run is filled by one atomic per draw, and the frames standing filled at most 1,100,232 entries a run at the top setting against a stride of 16,777,216. The runs now hold a headroom above the fullest run (half as much again, in steps of 65,536 entries), grown at once on a frame that turns an append away and counted. **The first flight held twice the fullest run**, and at the low setting it never shrank: there the fullest run standing is the farthest cascade's (4,419,461 entries; the camera's own runs hold under half a million), 28 % of the stride, and the arithmetic shrinks only when a window's peak with its headroom leaves more than half unused, so the list stood at the stride (`c2` below). At half again it gives the stride back in one step two seconds in. Standing, no frame of either setting dropped an entry.

| Standing, the same hour | Top: b69 (23:45) | Top: c2 (23:28) | Low: b69 (00:39) | Low: c2, twice (00:12) | Low: the tip (01:34) |
|---|---:|---:|---:|---:|---:|
| Maximum (at the first frame) | 22.5 GB | 20.4 GB | 15.8 GB | 13.8 GB | 13.7 GB |
| One minute standing | 19.1 GB | 16.3 GB | 15.8 GB | 13.7 GB | 13.0 GB |
| At the run's end | 18.9 GB (+2.9 min) | 16.0 GB (+2.7 min) | 15.6 GB (+5 min) | 13.7 GB (+5 min) | 12.8 GB (+5 min) |
| Exit snapshot: commit / engine heap / device-local / the frame's own | 19,319 / 3,707 / 13,311 / 2,272 MiB | 16,440 / 1,516 / 12,786 / 1,772 | 15,953 / 3,408 / 10,451 / 2,923 | 13,739 / 1,504 / 10,451 / 2,923 | **13,068 / 1,504 / 9,724 / 2,195** |
| The run capacity / the fullest run / entries dropped | the stride (16,777,216) | 2,228,224 / 1,100,232 / 0 | the stride (15,757,312) | the stride / 4,419,461 / 0 | 6,684,672 / 4,419,461 / 0 |

`c2` is stage C with the first headroom (twice the fullest run); the tip's half again holds less, so `c2`'s top column is the tip's upper bound (the tip's own top flight was given up for the night's budget). The last frames are the same bytes on every side: at the top b69, c1 and c2 (SHA-256 `6E9CBCD2FACDFE99…`, 33,179,760 raw bytes, none differing), at the low setting b69 (twice), c1, c2 and the tip (`0071CF243EAF4394…`, 8,295,480). The machine at every start 63 to 65 GB of free commit, the processors at 4 to 19 % except b69's second low (91 %), the session locked. **On the walk** the tip's runs went from the walk's stride (5,857,280) to 2,031,616 two seconds in and back to the stride 22 seconds later, with no entry dropped: the reach-ahead over a rising demand (`slope_frames`) met a frame whose fullest run jumped as the cascades' boxes moved, and a growth goes to the larger of that and the peak held; the walk's frames were as c1's (p99 11.0 / 9.5 / 8.7 ms, the admissions at most 0.22 ms). A demand fed run by run, or a slope over a steadier number than the maximum over runs, is the next refinement.

**What a launch commits now** (standing, GB of commit; the stages are this page's):

| | Top: maximum / standing | Low: maximum / standing |
|---|---:|---:|
| Stages 1, 4 and A (b69 with W51-4) | 22.7 / 18.8 | 15.6 / 15.3 |
| Stage B (c1) | 20.4 / 16.5 | 13.7 / 13.6 |
| Stages B and C (the tip; the top from `c2`) | 20.4 / 16.0 | 13.7 / 12.8 |

What is left at the top setting's maximum is the first frame's device transient, the chain's (W51-6's); standing, of the tip's 16.0 GB at the top about 12.5 is the device's (textures 3.2, geometry 2.7, the frame's own 1.8, structures 1.3, tiles 1.2) and 3.5 the host's, of which the engine heap is 1.5 and the C runtime's committed slack 1.2 — the residency manager's (W47) and an allocator's.

## W51-6: the peak at the first frame

**How it was flown** (2026-11-19, 21:33 to 23:10): W51-4's command and standing path at the top setting (3840 × 2160, `--raster rt --shadows rt`, the bounce, micromaps), 4,000 frames, `--memory-report-floor-mib 2`, `--benchmark` for each frame's traced counts, W51-4's derived-data copy, one lease each and ten minutes clear between; the process sampled from outside every two seconds. The binaries: **b69** (main as merged, 1f5ff68f, copied out), **the rule before** (this row's first commit with `renderer.rt.initial_clusters=16777216`, which starts the chain at the budget and the references at their estimate exactly as b69 does), and the tip. The machine: the session locked, free commit 50.7 to 62.8 GB at each start, the processors 18 to 100 % busy with other rows' builds and device runs.

**What happens between the end of `gpu scene` and the tenth frame, in order** (the rule before, the device's blocks from the large-buffer log's events, seconds from the log's start; every buffer of 2 MiB or more):

| t (s) | what | MiB |
|---:|---|---:|
| at `create` (W51-4: 193 s) | the first capacity, **the budget** (330,906 clusters, the estimate 710,947 being past it): six chunks, the records and arrays, a bottom-level set and its scratch, and the references at their estimate | 2,787 + 114 + 157 + 20 |
| 100.41 | the world's first commit: the frame's working set and its table sets for 387,108 instances (W51-5's ground), and the deformed-vertex pool's slots | + 1,194, + 192 |
| 100.43 | the chain laid out again, swapped: the top-level structure, the per-instance tables, a bottom-level set and scratch **beside** the old ones | + 127, + 100, + 690, + 183 |
| 100.45 to 100.57 | the first capacity taken again before any frame (the budget as it reads after the micromaps, 337,597, and the references' new estimate): a **waited** resize, so the six chunks and the bottom-level set just made are freed first | − 2,787, − 690, − 183, − 20 |
| 100.58 to 100.60 | … and made again: six chunks, a bottom-level set, scratch, references | + 2,844, + 701, + 200, + 21 |
| 100.60 to 100.64 | the old bottom-level set and scratch the relayout retired, freed: **the only blocks held twice, 272 MiB for 0.2 s** | − 114, − 157 |
| 100.68 | frame 0: the device at 17.2 GiB, the commit at its maximum, 22.8 GiB | |
| 110.96 | ten seconds of frames that built 6,306 structures and referenced 50,016: the guess given back, the new one chunk made on the worker beside the old six | + 69, + 81, + 55 |
| 111.00 to 111.06 | the old chain freed | − 3,765 |

**So the maximum is not a block held twice.** The chain's double holds are a frame's worth (272 MiB at the relayout, 205 at the give-back); the maximum is **the guess**: 3.7 GiB of chain made at a share of the card for frames that wanted 0.2 of it, held from `create` through the rest of the load (it is most of why the end of `gpu scene` stood at 16.3 GiB) and through the first ten seconds of frames, so that the world's first commit (1.4 GiB of the frame's own on the device, 2 to 3 GiB on the host: W51-5's) lands on top of it. And the first capacity, taken again at the world's arrival, freed the whole chain and made it again, chunks and all, for nothing: the same budget, a few thousand clusters more.

**The mend, at its cause** (renderer.md, "A guess past the budget is not made at it"): a scene whose estimate passes the budget starts at `renderer.rt.initial_clusters` (65,536 clusters, one chunk) and grows by what its frames ask, and the cache's references start at the same floor. Then the first capacity taken again at the world's arrival asks for what the chain already holds, and nothing is made again; the first frames record into one chunk; the give-back at ten seconds takes one chunk to its fit. A frame before the first read-back that wanted more than the floor would keep whole instances and count what it dropped; on the staging none did.

**The pair, and the stages between** (GiB of commit unless said; the device is the process's dedicated usage; "ready" is 20 seconds after the first frame, past the give-back):

| Top, standing | b69 (main) | the rule before (tip, `initial_clusters` 2^24) | the chain at the floor | **the tip** (and the references at the floor) |
|---|---:|---:|---:|---:|
| Flown at; free commit, processors | 22:08; 50.7 GB, 68 % | 22:35; 62.8 GB, 18 % | 21:33; 55.0 GB, 100 % | 22:59; 63.9 GB, 11 % |
| End of `gpu scene` (`uploaded`) | 17.97 | 16.45 | 14.07 | **14.11** |
| **Maximum**, and when | **22.71**, the first frame | 22.80, the first frame | 20.35, the first frame | **19.66**, the first frame |
| The device at the maximum | 16.79 | 16.84 | 14.47 | **13.92** |
| The chain at the first frame (MiB: chunks, bottom-level set, scratch, references) | 3,765 | 3,765 | 1,324 | **752** |
| Ready: commit / device | 19.13 / 13.38 | 19.11 / 13.38 | 19.06 / 13.38 | **19.13 / 13.38** |
| At the run's end (+4 to +5 min) | 17.53 | 17.50 | 17.48 | **17.50** |
| The first hundred frames: short of their demand; instances dropped in the run | 0; 0 | 0; 0 | 0; 0 | **0; 0** |

**The maximum falls 3.05 GiB against b69 (22.71 to 19.66) and stands 0.53 above ready, where it stood 3.58 above**; ready and standing do not move, because the guess was given back after ten seconds in every binary. The device at the first frame is 2.87 GiB lower. Every frame of every run built what it wanted (6,306 structures, 50,016 references, from frame 0), so the floor costs this scene nothing; a scene whose first frames want more than 65,536 records two frames short and counts them, which the staging could not show. The end of `gpu scene` is 2.3 GiB lower than the rule before (the guess no longer made at `create`; b69's 1.5 more is W51-4's stage A). What is left at the first frame above ready is the floor itself (one chunk of 552 MiB and its bottom-level set, given back to 206 MiB after ten seconds). The world's first commit, 1.4 GiB of the frame's working set on the device and the mesh room's 2 to 3 GiB on the host, now makes most of what remains between the end of `gpu scene` and the maximum; it is W51-5's.
**The frame budgets** (`tools/frame-budget.ps1` on the tip's `msvc-release`, 23:47 to 01:11, the session locked, others at 9 to 30 % of the processors). The erg and endless runs' GPU totals do not move: `erg-walk-1080p-rt` 0.692 / 0.736 ms (median / p99) against 0.724 / 1.214, `erg-walk-surround3-rt` 3.773 / 4.161 against 3.771 / 4.127, `endless-1080p-csm` 2.089 / 4.661 against 3.161 / 5.545, `endless-surround3-rt` 4.317 / 5.213 against 4.392 / 5.287; their chains now start at one chunk instead of six, and none of their frames was short. `erg-walk-surround3-rt`'s CPU median read 0.793 and then 0.683 ms against a limit of 0.420, where b69 read 0.356 the same hour; flown again in one lease as tip, the rule before in the tip's binary, b69 and tip, it read 0.765, 0.498, 0.360 and **0.309**: the same binary 0.31 to 0.79 ms from one process to the next, every part of the frame's CPU (begin, passes, record, submit) scaled alike, so the line measures where the process ran on this shared machine and not the chain's first capacity. `overlook-1080p` and `static-1080p-rt` did not run: they read Khronos samples (`content/samples/`) this checkout has not fetched.

## W51-5b: the memory rows together, mended

An independent read of W51-5 and W51-6 (batch 71) found four things on W51-5 a suite did not catch; each was made to fail on the joined tree first and mended at its cause.

- **A farther tile shelved a mesh a nearer one was admitting** (world.md, "A withheld mesh is let go and built again"). Mended: a mesh kept from being asked is let go only when no ticket asked for it this commit and it holds no texture yet.
- **Every shelved mesh that fitted alone was built again in one commit** (the same section). Mended: a build again reserves the room it was asked for until the mesh is in or let go.
- **A frame somebody waits for returned with pairs dropped** (renderer.md, "The visible list's runs are sized by the frame"): W51-5's own case measured it (runs half the fullest: 1,615 appends turned away and 4,610 ids differing in the capture). Mended: `render_offscreen` grows the runs and draws again, as it does for the chain; the case now asserts the capture is the loaded scene's.
- **A refused lay-out of the runs leaked the old list or left the frame none**, and the pool's slots followed in a relayout that destroyed what the mesh records named before making its replacement (the same section). Mended: `fit_runs` makes every buffer before any old one goes and leaves the scene as it was on a refusal, counted; a test seam (`GpuScene::Fault::runs`) proves each path.
- Lows: a pair a full run turned away is no longer counted or flagged drawn; the large-buffer log records a make only with its table entry; two stale sentences on the first frame past the budget; two stray control bytes.

**The memory rows together, mended** (2026-11-20, the forest staging standing at W149's one-place path, W51-5's command; `b70\bin-main` is main as merged, 2de3230c, before W51-5 and W51-6; the tip is batch 71's join with W51-5, W51-6 and these mends, a63534141, in `msvc-release`; one load a lease, ten minutes clear between; free commit 54 to 62 GB at each start, the processors at 9 to 10 %, the session unlocked; ready is 20 seconds after the first frame):

| Standing | Top: main (20:30) | Top: the tip (20:07) | Low: main (20:47) | Low: the tip (21:26) |
|---|---:|---:|---:|---:|
| Maximum, and when | 22.77 GiB, the first frame | **20.21 GiB**, the first frame | 15.95 GiB, the first frame | **13.73 GiB**, the first frame (13.85 GiB for one 3 s sample at 508 s) |
| The device at the maximum | 17,189 MiB | **11,787 MiB** | 10,615 MiB | **10,575 MiB** |
| Ready / one minute standing | 19.16 / 19.16 GiB | **15.73 / 16.33 GiB** | 15.90 / 15.90 GiB | **13.08 / 13.10 GiB** |
| The first hundred frames: chain short; run entries dropped | 0; 0 (runs at the stride) | 0; 0 (runs 16.8 M to 1,703,936, fullest 1,100,232) | 0; 0 (runs at the stride) | 0; 0 (runs 15.8 M to 6,684,672, fullest 4,419,461) |
| The mesh room: resident / tiles withheld / meshes shelved / host bytes waiting | 52 / 189 / — / (kept) | 52 / 189 / 297 / 0 MiB | 89 / 169 / — / (kept) | 89 / 169 / 260 / 0 MiB |

**Together** the two rows take the top setting's maximum from 22.77 to 20.21 GiB (W51-6 alone measured 19.66 and W51-5 alone 20.4, each against 22.7) and the device at that moment from 16.8 to 11.5 GiB; standing a minute, 19.16 to 16.33 GiB (W51-5 alone about 16.0, an upper bound on a busier machine). **What H1's and M1's mends cost of it: nothing measurable standing** — the room held the same 52 meshes and shelved the same 297, with no host bytes waiting (a mesh kept from shelving because a nearer tile asked for it, or for its textures, would show there), and a room that stays full starts no build again, so M1's reservation never binds standing; what M1 changes is a walk's freeing of the room, one build per mesh's worth of room instead of one per shelved mesh. H2's redraws and M2's all-or-nothing lay-out cost no bytes standing: the runs were sized once and never refused.

**At the low setting** (1080p, `--shadows csm`, no bounce; flown after the top pair, the same night, ten minutes clear between): the tip takes the maximum from 15.95 to 13.73 GiB and a standing minute from 15.90 to 13.10 GiB, with the device at the maximum unchanged (10.37 against 10.33 GiB: this setting builds no ray-tracing chain, so W51-6's capacity does not reach it, and what is gone is W51-5's working buffers and withheld tiles). The machine was not the same for the two: main started at 6 % of the processors with no other `engine-view`, the tip at 71 % with two others running, so its upload took 37 s longer and its first frame came at 234 s against 118 s; both columns are upper bounds and the tip's the more loaded. The tip's one 3 s sample at 14.18 GiB, 274 s after its first frame, is past the standing minute and fell back at the next sample. Read from `<scratch>\b71\W51-5b\loads\{main,tip}-low\` with the row's `summarize.ps1`, the first frame taken from the log's first sizing of the runs where the log carries no "first frame" line.

## W156: what a launch commits after it is ready

**How it was flown** (2026-11-21 23:54 to 2026-11-22 01:30): W51-5b's command and standing path (W149's one-place path, `--memory-report-floor-mib 2`, `--benchmark`, the process sampled from outside every two to three seconds), at the top setting (3840 × 2160, `--raster rt --shadows rt`, the bounce, micromaps) and the low one (1920 × 1080, `--raster hw --shadows csm`, no bounce, no micromaps), 4,000 and 3,000 frames, one load a lease and ten minutes clear between; `main` is batch 71's merged release (2964d252, copied out), the tip this row's; the staging's derived data is its own (batch 71's recooked flora), read through a root of links with a copy of the pipeline cache, so every load here is **warm**. "Ready" in the forest session's table, which this row was asked about, is the `gpu_scene` milestone; this page's earlier tables read it as twenty seconds after the first frame, and both are given.

**The warm launch's timeline** (main, top setting, seconds from launch; the commit at the stage's end): the scene file 0.8 to 57.6 (its expansion: ground plans, instance heights, placements, waters); the load 57.7 to 112.1 (`scene_loaded`, 8.8 GiB); the upload 115.9 to 138.2 (`gpu_scene`, the `uploaded` snapshot 13.95 GiB); **then 27 s to the first frame**: the renderer 7.5 s (its pipelines 0.5 s from the cache, the opacity micromaps' bake 6.7 s: 16,508,008 triangles), the ground's start 2.1, the world's creation 10.8, its first update 4.9 (the placements' commit 2.8); the first frame at about 165 s. The low setting: `scene_loaded` 82.6, `gpu_scene` 105.6 (10.1 GiB), the renderer 1.1, the world 11.6 and 4.1, the first frame at about 126 s. **The bake is not keyed**: `GpuScene::bake_micromaps` runs on every load, blocking, as every upload of a scene's creation does, and a window's first frame waits for it exactly as a flight's does (both create the renderer before their loop); caching it is W47's stage 5 (micromaps baked at first admission and cached), not a small cache beside the bake.

**The cold launch's gap is one pipeline.** On the first load of a binary whose shaders changed (batch 71's merged binary at 18:00, and W51-5b's first load), the time from `gpu_scene` to the first frame is about 350 s, and 341 s of it (285 s in W51-5b's) is the driver compiling **one** pipeline on a pipeline-cache miss: `visibility_resolve_rt_water_terrain_stochastic`'s `fs_resolve`, 5.1 MB of SPIR-V; the next longest is the bounce's `gi_sample_main` at 13 s, and the bake is the same 6.7 s. **That compile is also the launch's largest transient after ready**: in W51-5b's first load the commit climbed from 14.7 GiB at `gpu_scene` to 20.7 GiB during the pipelines with the device flat (11.8 GB), and fell to 15.4 GiB between the samples on either side of the moment the resolve's pipeline was made (396.5 s) — **+6.0 GiB for about five minutes, the driver's compiler**, in the process. It is what the forest session's table shows (+6.6 GiB at the top and +6.4 at the low setting after `gpu_scene`, two thirds of it gone by the exit, the low setting with no micromaps rising the same): every resolve variant compiles this way the first time a binary, a driver or a player's machine meets it, and a warm cache makes it 54 ms. Nothing in the engine's own memory moves during it. It is the next row's ([below](#what-is-next-after-w156)).

**What arrives after `gpu_scene` on a warm launch, by owner** (main; MiB of commit unless said; the device's large blocks from the large-buffer log's events, 16 MiB and over, symbolized against the binary that ran):

| | Top | Low |
|---|---:|---:|
| `uploaded` snapshot (`gpu_scene`) | 13,952 | 10,345 |
| The maximum, and when | **17,477 at 167 s** (the first frame), the device at 13,713 MB | 13,763 at 202 s (a path repeat's world restart, below); about 12,900 at the first frame |
| Standing (a minute after the first frame, and at the exit snapshot) | 16,403 / 16,134 | 13,008 / 13,001 |
| The renderer's creation: the micromaps (369 standing; the bake's own scratch of about 0.9 GB, made and gone inside its 6.7 s, under the maximum), the rigid forms' structures built again, the targets | about +2,600 by the curve | about +0 |
| The world's arrival (the first commit's hand-over, `fit_tables` and `relayout`): **the visible list at the pair stride** and **the pool's per-entry slots at the stride** | **+384 and +192, for 4 s** (16,777,216 entries a run; the frames' fullest 1,100,232) | **+841.5 and +420.8, for 6 s** (15,757,312; the fullest 4,419,461, fit 6,684,672: 357 + 178.5) |
| The arrival's table sets and working tables (standing) | +53 + 128 twice, +64 + 256 + 64 + 64 | +54 + 120 twice, +60 + 240 + 60 + 60 |
| The chain's first capacity, at its floor (ADR-0078) until the give-back at ten seconds | **+552 + 144, for 10 s** (69 + 81 after) | — |
| The host (commit less the device's) | 3,764 MiB at the maximum, 3,773 standing: none | flat at the first frame; **+0.76 GiB for one sample at each path repeat's restart** (the world cleared and filled again: 1,256 tiles out and in, the mesh room newly withholding 169) |

So on a warm launch the maximum stands **1.05 GiB above the standing figure at the top setting (17,477 against 16,403 MiB), and it is the device's** (13,713 against 12,630 MB): the runs and slots laid out at the stride (576 MiB) and the chain's floor (696 MiB, less the 150 it fits to) beside the frame's first tables; the host's share is nothing. At the low setting the arrival's runs (1,262 MiB at the stride, 727 above their fit) are the largest of the engine's own, and the low setting's maximum is the flight's restart, which a player standing still never meets: a restart is the timed flight's repeat (`fly_camera_path`), the whole world's fill again, and its host share is the mesh room's withheld meshes loaded and shelved, which T213's row is reading tonight and this row did not edit. **Neither setting shows tiles streaming in after the first frame while the camera holds still**: standing, the curve is flat from 177 s (top) and 141 s (low) to the end, except the restarts. **W21** ("one staging ring for tile uploads"), a candidate, is not an owner: the ring has staged every chunk and admission since 2026-10-04 (renderer.md, "Staging allocates nothing in steady state"), and the process's shared GPU memory (where host-visible blocks are charged) stood at 425 MiB at the top's maximum and 425 standing.

**The mend: a tail's runs start at a first bound** (renderer.md, "The visible list's runs are sized by the frame"). The stride is laid out at the arrival before any frame has drawn, which is why it held for the first seconds of every launch; now the unsized runs are the smaller of the stride and `renderer.runs.first_entries` (4,194,304 entries, 32 MiB of list a run), the frames grow them as any filled run is grown, and a frame somebody waits for is drawn again. **Why 4,194,304 and not twice that**: the guess is given back to the frames' fit only when the fit is at most half of it, so 8,388,608 would stand at the low setting (fit 6,684,672) 137 MiB above the stride's fit; at 4,194,304 the top's frames (1,100,232) fit and the guess goes to their fit, and the low setting's farthest cascade fills it on the first frame and is grown to the same fit (drawn again offscreen; in a window that run's overflow is dropped for the read-back's frames, counted). The chain's floor, the other half of the top's maximum, is ADR-0078's decision and was left.

**The pair** (GiB of commit unless said; "the first frame" is the commit's largest sample within six seconds of the device's own maximum, which is the first frame's; "ready" is `gpu_scene`; the device is the process's dedicated usage; the session locked on every flight):

| | Top: main (23:54 / 00:30) | Top: the tip (00:40) | Low: main (00:14) | Low: the tip (00:56) |
|---|---:|---:|---:|---:|
| Processors at the start; free commit | 79 % / 74 %; 53.1 / 57.8 GB | 100 %; 52.7 GB | 34 %; 54.3 GB | 4 %; 52.9 GB |
| Ready (`gpu_scene`), s; the first frame, s | 138 / 151; 165 / 179 | 117; 139 | 106; 126 | 105; 120 |
| The arrival's list and slots, MiB (the runs) | 384 + 192 (16,777,216) | **96 + 48 (4,194,304)** | 841.5 + 420.8 (15,757,312) | **224 + 112 (4,194,304)** |
| **At the first frame**: commit / the device, MB | 17.07 / 17.01; 13,713 | **16.62; 13,281** | 13.38; 10,039 | **12.8 (by the curve); 9,352** |
| The run's maximum, and when | 17.07 / 17.01, the first frame | 16.65, a path repeat's restart at 209 s | 13.44, a restart at 202 s | 13.41, a restart at 276 s |
| A minute after the first frame / at the run's end | 16.02 / 15.81 and 16.01 / 15.79 | 16.0 (the sample at a minute is the restart's 16.65) / 15.75 | 12.70 / 12.48 | 12.64 / 12.51 |
| The runs' fit, and entries dropped | 1,703,936; 0 | 1,703,936; 0 | 6,684,672; 0 | 6,684,672; **225,157 in the first frame's read-back** (the farthest cascade, grown 0.2 s later) |
| Frames over 33 / 50 / 100 ms, of 12,000 (top) and 9,000 (low) | 36 / 7 / 0 and 0 / 0 / 0 | 1 / 0 / 0 | 3 / 2 / 0 | 1 / 0 / 0 |
| The last frame (SHA-256 of the capture) | `49F09EDFEEFB6379…` | the same | `B7BF17FA7C2D07CA…` | the same |

(The commit here is GiB, 2³⁰ bytes, as the page's other tables; the flight's sampler logs MiB: 17,477 and 17,418 against 17,023 at the top's first frame.) **At the first frame the top setting's commit falls 0.43 GiB and the device's 432 MB, and at the low setting the device's 687 MB, where the device's maximum is now its standing figure**; ready, the minute after, the runs' fit and the last frame do not move. The low setting's run maximum stays the flight's restart (the room's, above), and the top's moves there too, 0.4 GiB under the first frame it was. The long frames are the machine's, not the bound's: main's two top flights read 36 and 0 over 33 ms with the same binary. What it costs: at the low setting the first frame's farthest cascade drops 225,157 of its 4.4 million casters for the frames until its growth lands (0.2 s), which a capture or any frame somebody waits for draws again; a window starts behind the readiness cover.

### What is next after W156

- **The resolve's first compile** (a row): 285 to 341 s and +6 GiB of commit the first time a machine meets a binary's resolve variant, which on a player's machine is the first launch and every driver update. In the order this page would try them: (1) **compile it before the load**, while the process holds a few hundred MiB instead of 14 GiB, so the compiler's +6 GiB lands under the standing figure instead of on top of `gpu_scene` (the variant is a function of the settings and four scene flags — water, terrain materials, stochastic materials, foliage — that the scene file names before its meshes load; the pipeline's layout is the bindless set's and its colour format the target's, both known then), then the load's own creation finds it in the cache; (2) **compile it beside the load** on a worker, which takes the gap off the time to the first frame but not the peak; (3) **a smaller module**: the resolve by pixel class (R151) compiles a module a class, and what each costs the driver's compiler is unmeasured. Measure first: the compiler's peak per variant, cold (`ENGINE_PIPELINE_CACHE=0`), from a small scene of the tree's that selects the same variant, which takes seconds to load.
- **The bake's cache**: W47's stage 5 (micromaps baked at first admission, cached by the mesh and its materials); 6.7 s of every load at the top setting.
- **The chain's floor at the first frame** (696 MiB for ten seconds): a give-back after the first read-back rather than after ten seconds would shorten it, not lower it; lowering it is a smaller `renderer.rt.initial_clusters` that the staging's first frames (6,306 structures) would allow and other scenes might not, ADR-0078's to revisit.
- **A first-frame and a peak snapshot in the timed flight** (`fly_camera_path` takes no `first_frame` snapshot, so the host's share at the maximum is read from the curve less the device's usage): a snapshot taken when the commit passes its maximum after the first frame would name the host's owners there.

## W167: the texture ceiling gives its bytes back

**What was built** (W47's stage 1, [the design](residency-manager-design.md#stages-in-build-order) row 1; [texture](../subsystems/texture.md#stage-3-the-bytes-go-back-to-the-device)): the images the texture residency holds in chunks of their own that go back to the device, a sparse chunk drained by moving its textures, the ceiling from `renderer.textures.device_budget_mib` where no `ceiling_mib` is named, and promotions through the staging ring. **How it was flown**: W156's command and paths (`--memory-report-floor-mib 2`, `--benchmark`, the process sampled from outside every two to three seconds; the standing path, and the grounded walk's 2,400 frames), `--audio off`, at the top setting (3840 × 2160, `--raster rt --shadows rt`, the bounce, micromaps) and the low one (1920 × 1080, `--raster hw --shadows csm`, no bounce, no micromaps); "base" is batch 73's merged release (0f91fd9d, copied out), "tip" this row's `msvc-release` copied out; the session unlocked, one load a lease, the other agents' loads on the card before and after but never beside. "Standing" is the sample a minute after the device's maximum (the first frame), as W156's table reads it. The bound stated before any flight: the share of textured pixel-frames drawn a level or more softer than asked **at most 0.01 % standing at either ceiling, at most 0.1 % on the walk at the top ceiling and 0.5 % at the low one, and two levels or more at most 0.01 % anywhere** (the village walk measured 0.0046 % at a seventh of its set).

**What the staging holds of textures**: 3.30 GB in 5,440 images; **1.80 GB of it in 3,874 images the residency holds by level**, the other 1.50 GB not — an alpha-tested base colour (the rays' mask tests and the micromap bake read level 0), a texture of a deformed or open mesh, the terrain's layers and the water's (how it splits between those four was not counted). So any ceiling under about 1.6 GB trims every held texture to its floor (125 MB for the 3,874) and stops there: **at the top tier's 1,536 MiB and the low tier's 768 MiB the trim is the same trim**, and what it can give back is 1.68 GB.

**Base, with main's ceiling** (top, standing, 1,536 MiB, with the feedback measurement on): the trim released 1.70 GB in 3,820 demotions and **the driver's usage rose**: 12,887 MB standing against 12,630 with no ceiling, the shared 681 MiB against 425. And the world's first activation of its 1,256 tiles dispatched for 101.8 s against 4.3 s, while the process's commit rose to **49.9 GB** (the machine's free commit to 0.2 GB): the load's held textures were not offered to tile meshes, so each tile mesh naming one read and uploaded its own whole copy. Both are mended in the tip ([texture](../subsystems/texture.md#stage-3-the-bytes-go-back-to-the-device), items 1 and 5); the base's ceiling is not flown again at the low setting for that reason.

| Top setting, standing | Base, no ceiling | Tip, no ceiling | Tip, 1,536 MiB |
|---|---:|---:|---:|
| Processors at the start; free commit | 61 %, 46.7 GB | 23 %, 54.1 GB | 11 %, 66 GB |
| Ready (`gpu_scene`), s | 148.9 | 78.3 | 83.2 |
| The first frame: commit, MiB; the device's maximum, MB | 17,123; 13,281 | 17,275; 13,281 | 17,132; 13,281 |
| **Standing: commit, MiB; the device, MB** | 16,462; 12,630 | 16,489; 12,630 | **15,004; 11,094** |
| The held textures' chunks at the end: live, MiB of them used | — | — | 4 × 64 MiB, 124.8 MB used (28 made, 24 given back, at most 1,792 MiB) |
| Demotions; moves; frames under pressure | — | — | 3,820; 2,210 (75 MB); 731 |
| Frame median / p95 / p99, ms | 14.77 / 15.32 / 15.90 | 14.79 / 15.01 / 15.13 | 14.77 / 15.74 / 20.35 |
| The frame loop: allocations (frames) | 2 (1) | 2 (1) | 25 (5, the last at frame 633) |
| The last frame (SHA-256 of the capture) | `2ABEDD4F38F31357…` | the same | `A54038A2A9CA42BC…` (the bounce reads a trimmed image's top) |

**At the top setting the ceiling returns 1,536 MB of the device and 1,458 MiB of commit standing** — the design's 1.6 GB, less the floor the held textures keep (125 MB) and the 144 MB of holes in the four live chunks (1,680 MB released by the trim, 1,536 back to the driver). With no ceiling the tip is main to the byte: the same standing figures and the same last frame. **The first frame does not move**: the trim waits the hold (8 s) after the load, which uploads every held texture whole, so the launch's maximum is main's; a launch that should start under the ceiling is the bound's (`renderer.textures.bound`, which uploads from the floor) or stage 7's. The ceiling costs a frame its feedback pass (0.04 ms at 4K, R172's measure) and nothing the medians show; the p99 is the trim's first seconds, when 3,820 demotions and 2,210 moves pass through 4 MiB a frame, and the 25 allocations are that same stretch growing the frame's pending lists (none after frame 633).

| Low setting, standing | Base, no ceiling | Tip, 768 MiB |
|---|---:|---:|
| Processors at the start; free commit | 6 %, 51.9 GB | 0 %, 54 GB |
| Ready, s; the first frame's commit, MiB; the device's maximum, MB | 82.5; 13,319; 9,352 | 94.0; 13,409; 9,352 |
| **Standing: commit, MiB; the device, MB** | 13,131; 9,352 | **11,697; 7,816** |
| Demotions; moves; chunks live at the end | — | 3,870; 2,843; 4 (115 MB used) |
| Frame median / p95 / p99, ms | 22.37 / 22.93 / 23.24 | 22.40 / 22.98 / 23.29 |
| The frame loop: allocations (frames) | 0 | 29 (5, the last at frame 559) |
| The last frame against the base's | — | 38 of 2,073,600 pixels differ, by at most 2 of 255 |

**At the low setting the same 1,536 MB comes back** (the device 9,352 to 7,816 MB, the commit 13,131 to 11,697 MiB), since the low tier's 768 MiB trims to the same floor; the design's 2.4 GB needed the 1.50 GB the residency does not hold. With no bounce the last frame is nearly the base's: 38 pixels a step of 2 at most, which is a level the sparse feedback had not asked within the hold, drawn a level softer.

**The ceiling from a device budget** (`renderer.textures.device_budget_mib=10240`, the design's top tier, no `ceiling_mib`; the tip, top, standing): the ceiling was worked out at 816 MiB (10 GiB less what the rest of the process held on the device) and the trim and the standing figures are the 1,536 MiB ceiling's — the device 11,094 MB, the commit 15,111 MiB.

**On the grounded walk** (the top setting, the tip; the share a level softer from runs with the feedback measurement on, the rest from runs without it, the same hour, the processors at 1 to 23 %):

| Top setting, the walk's 2,400 frames | No ceiling | 1,536 MiB |
|---|---:|---:|
| The device at the end, MB; the commit at the end and the run's most, MiB | 13,844; 18,195 and 18,890 | **12,372; 16,328 and 17,706** |
| Promotions; demotions; moves; refused inside the hold; frames that asked late | — | 315; 4,199; 3,083; 33; 412 |
| **Textured pixel-frames a level softer** (two or more) | — | **0.0157 %** (0.0009 %) |
| Frame median / p95 / p99, ms | 17.91 / 21.73 / 22.58 | 18.09 / 21.96 / 22.79 |
| The frame loop: allocations (frames); device buffers (frames) | 5,731 (662); 39 (10) | 5,655 (665); 46 (17) |

And at the low setting under 768 MiB (with the measurement on): **0.0007 %** of textured pixel-frames a level softer (two or more 0.00001 %), 403 promotions, the device 7,398 MB a minute after the first frame. **Both walks are inside the bound stated before the flights** (0.1 % at the top ceiling, 0.5 % at the low one, two levels or more 0.01 %). The walk with no ceiling allocates as much as the walk with one: those are the walk's own (its tiles arriving, the first flight over a path), which `engine_view.frame_loop`'s second flight holds to zero; the ceiling adds seven device buffers in seven frames, promotions the ring had no room for beside the ground's chunks, which a promotion makes as before. The median moves 0.18 ms; the feedback's pass is 0.04 ms of it at 4K (R172's measure), and the rest was not split between the swaps, the moves and the run.

**The default I would set** (the design's question 2, the owner's): none fixed in MiB. A ceiling in MiB means a different thing beside a different scene; the device budget names what a player's card has, and on the staging 10 GiB at the top setting leaves the textures 816 MiB, which is the same trim as the tiers' 1,536 and 768 MiB. So `renderer.textures.device_budget_mib` from the tier (10,240 at the top, 4,608 at the low one, capped by the card less an eighth as the design says), once ADR-0063's form is settled; until then the default stays main's, nothing trimmed, because a ceiling under the textures the residency does not hold trims every held texture to its floor and a launch should not do that unasked.

## Not measured

- The cold load with a cache of its own: a cold load of the whole staging needs a 20-minute lease and 40 GB free (the staging agent's DONE note), and its cook does not ship; its peak is the staging agent's 32.2 GB.
- The first-frame snapshot: a plain `--frames` run takes none (only the replay and path loop does); ready is read from the curve.
- The owner's own world (v51) and a walk: run with `--memory-report-floor-mib 2` and read the three snapshots.
- Linux, which charges no commit for the GPU's bytes.
- Standing still the terrain's rings were rebuilt every 3 s (1.1 to 2.1 s each, about 150 MB uploaded) and some 40 tiles activated and deactivated a tick: not memory, noted for the rows that own them.
