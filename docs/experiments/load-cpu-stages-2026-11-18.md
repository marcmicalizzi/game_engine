# The load's two CPU stages, by zone (2026-11-18)

Roadmap W150. The forest staging's warm load (batch 69's staging, engine-view on main at 09ed028b, 3840×2160, `--raster rt --shadows rt`, the bounce, micromaps, `--terrain-async`, six frames; its startup trace and report kept beside the coordinator's staging, read here and run again by nothing) took 348 s from launch, of which the two stages that run before a byte reaches the device took 181 s: **`scene file` 92.7 s and `load scene` 88.3 s**. D40 had measured the scene file's JSON read at 6.25 s before its reader and 2.94 s after, so most of `scene file` was not parsing, and nobody had said what it was. This page names every zone of the two stages from that trace, says what each holds in memory, mends the two whose cause is the load's own code, and names the three later stages' largest zones for a later row.

Machine state for the trace: a busy machine (other agents' builds; the staging's own notes), session unlocked; the cores counter reads 0.75 to 0.78 through `scene file`, so the stage ran on one thread and was not starved. Stage times from it are upper bounds. The benches below give theirs.

## `scene file`, 92.7 s, every zone on the main thread

| zone | seconds | share | what it is |
|---|---:|---:|---|
| `json parse` | 3.85 | 4 % | the tree parse (before D40; D40's reader replaces this and `schema read`) |
| `schema read` | 1.27 | 1 % | the reflection into `engine.scene.Scene` |
| `scene expand` → `ground plans` (`ground plan` "channels" 16.51) | 16.79 | 18 % | the channels entry's plan for the ground (`plan_scene_ground`), reading 193 MB of tile records |
| `ground sampler` | 1.17 | 1 % | the `tiles` provider made |
| `mask level` (`ground masks`, a hit) | 0.95 | 1 % | `check_terrain_masks` |
| **unnamed** | **17.76** | **19 %** | **every file instance's ground**, below |
| `placements` (`placement` "channels" 14.86) | 14.94 | 16 % | the channels entry expanded; the other 21 entries are streamed and cost under 0.1 s together |
| `water` | 34.27 | 37 % | `read_scene_water` (26 bodies) and `read_scene_water_channels` (278 channels): 21.6 s of CPU and 1.39 GB of tile records read |
| stage's own, between zones | 1.70 | 2 % | the file read before the parse, the description's checks |

**The unnamed 17.8 s.** `read_scene_file` stands each of the file's 470,641 instances on the ground — every one, grounded or not, since a placed one is checked for floating (T74) — with `ground_at_world`: `GroundProvider::height_mm` once an instance, in the file's order. The `tiles` provider reads its ground through a cache of 64 MiB (`tileset.cache_mib`): 3,737 tiles of 64 × 64 cells at 250 mm, 17,956 bytes each, of a finest level of 151,425. The instances come in the order the file was written — a kind at a time, each kind over the whole 3.3 × 10.7 km region — so each kind's pass evicts what the last one read. A simulation of the cache over the instances' points gives **126,302 tile reads in the file's order, 2.268 GB**, which is the trace's `read_bytes` for the zone to the megabyte (the expand's 4.094 GB less its named children's), **for 53,086 distinct tiles under the instances**; asked in a row-major or a Morton order of 16 m cells, 53,086 and 53,161. 58 % of the zone's reads were tiles read again.

**The mend** (`scene_gen::heights_by_place`, [scene_gen](../subsystems/scene_gen.md#many-heights-at-once)): the instances' grounds are asked in a Morton order of 16.384 m cells and kept by index, so every height is the provider's answer to the bit and only the order of the questions changes. The instance array is reserved from the file's count (D40 found its growth to be the read's peak). Stage `instance heights` names it in the next trace.

Bench, `msvc-release`, `engine_tileset_bench` (`domain/tileset/bench/heights_bench.cpp`):

| input | `tileset.heights.file_order` | `tileset.heights.by_place` | ratio |
|---|---:|---:|---:|
| the staging's own: its tile set behind 64 MiB, its 470,641 instances' points (`ENGINE_BENCH_TILESET`, `ENGINE_BENCH_SCENE`, read-only) | 3.451 s | 1.536 s | 2.25× |
| synthetic: 2 km × 2 km at 1 m, 1,024 tiles behind 58, 200,000 points in a seeded order | 4.830 s | 0.134 s | 36× |

Both rows of a run are back to back in one process; no quiet window came (machine state below). The staging's row is the zone without the load around it and with the 2.7 GB archive in the OS's file cache, where a tile read is a copy: 2.25 times fewer reads (126,302 to 53,086), 1.9 s saved. In the trace the same zone took 17.8 s: the load ran with 4 GB of physical memory free on a full machine, so the archive's pages were not held and the reads went to the disk at about 140 µs a tile; there the saving is in proportion to the reads, about 10 s of the 17.8. The synthetic row, every point a random place, is the worst case a file's order can be.

The channels' three zones (`ground plan`, `placement`, `water`: 65.6 s, 71 % of the stage) are the channels' and the water's generators, the other coordinator's ground, and are measured here, not mended: see "Left for others" below.

## `load scene`, 88.3 s

| zone | seconds (main) | share | thread | what it is |
|---|---:|---:|---|---|
| `meshes` | 18.41 | 21 % | main waits; the pool works | 4,188 `mesh` jobs over 35 threads, 370 thread-seconds: `cluster read` 179, `generator inputs` 37, `source hash` 8.6; 5.76 GB read |
| `cache textures` | 5.58 | 6 % | main | of which `texture probe` 4.62 and `texture identities` 0.77 |
| `seats` | 1.96 | 2 % | pool | the grounded instances seated by their footprints |
| `authored forms` | 39.84 | 45 % | main | `stream_authored`: 29.47 s unnamed before `far forest`, `far forest` 7.32, 3.05 after it |
| `merge` | 3.51 | 4 % | main | `merge_cluster_meshes` |
| `release meshes` | 8.19 | 9 % | main | freeing the per-mesh DAGs (4.9 s of CPU) |
| `instances` and the stage's tail | 6.05 + 3.41 | 11 % | main | the 5,060 kept instances' table, then `SceneData` filled from the load's copy of the description, then that copy freed |
| before `meshes` | 0.72 | 1 % | main | `SceneDesc resolved = desc`: the description copied whole, 470,641 instances |

**`instances` and its tail.** `load_scene` filled `SceneData::streamed` with `out.streamed = resolved.streamed`, a copy, and `resolved` was freed as the function returned. The authored entry's parameters are `engine.scene.AuthoredSet` written into a `JsonValue` (`stream_authored`, `schema::to_json`): a tree of every instance it streams, about 465,000 in the staging, each an object of four members with three arrays. The copy was most of `instances` (it rose 0.40 GB of private bytes in that zone, the tree's size) and the free most of the tail. **The mend**: moved, not copied. Bench, `engine_renderer_bench` (`systems/renderer/bench/authored_params_bench.cpp`), a set of the staging's shape (465,000 instances, 3,849 form sets of 7 rings):

| row | median | what it is in the load |
|---|---:|---|
| `renderer.authored.params.write` | 2.415 s | `schema::to_json(set)` in `stream_authored` (now stage `authored params`) |
| `renderer.authored.params.copy` | 1.148 s | the copy `load_scene` made into `SceneData::streamed`: **gone** |
| `renderer.authored.params.free` | 0.858 s | the original's free as `load_scene` returned: **gone** (the moved tree is freed once, with the world's entries) |
| `renderer.authored.params.read` | 0.581 s | the world's `open` of the entry (`world create`) |

So the move takes about 2.0 s and 0.40 GB of private bytes out of `load scene`. The write and the read (3.0 s together) remain: a typed set handed to the generator instead of a tree is an interface change for a later row.

**`authored forms`' unnamed 29.5 s** is `stream_authored`'s serial work before the far forest: per used mesh, its sway padding (`flora_bounds_padding`), then a cut of its DAG per ring (`geometry::extract_lod_form`) and a copy of its `SourceMesh` for each form kept, then the walk of every instance into the set. It added 8.85 GB of private bytes that the load keeps (the forms). The trace has no zone inside it, so it is named now (`authored sway`, `authored rings`, `authored set`, `authored params`) and not mended blind: the per-mesh cuts are independent and could go on the load's pool, appended in mesh order afterwards, once the next trace says the cuts are what costs; the `SourceMesh` copies are W51-3's question (what a mesh keeps and when it lets go).

## What the zones hold

Private bytes from the trace's memory counter, at each zone's start, its peak and its end:

| zone | start GB | peak | end | kept |
|---|---:|---:|---:|---:|
| `json parse` | 0.56 | 1.04 | 1.04 | +0.48 |
| `ground plans` | 1.25 | 1.77 | 1.30 | +0.05 |
| instance heights (unnamed) | 1.31 | 1.51 | 1.51 | +0.20 |
| `placements` | 1.51 | 1.90 | 1.46 | −0.04 |
| `water` | 1.46 | 1.61 | 1.61 | +0.15 |
| `meshes` | 1.11 | 6.04 | 6.04 | +4.93 |
| `authored forms` | 5.88 | 14.74 | 14.73 | +8.85 |
| `merge` | 14.94 | 18.36 | 18.36 | +3.42 |
| `release meshes` | 18.36 | 18.28 | 15.56 | −2.80 |
| `instances` | 15.56 | 15.96 | 15.96 | +0.40 |

The heights' mend allocates 20 bytes an instance for the order and the coordinates (9 MB for the staging) and frees them at the zone's end; it does not move the stage's peak (1.90 GB, in `placements`). The streamed move takes the 0.40 GB copy out of `instances` and its free out of the tail; the load's peak (18.4 GB, at `merge`) is not in either zone, and nothing here changes what the load keeps.

## Left for others

- **The channels** (the other coordinator's): `ground plan` "channels" 16.5 s (`plan_scene_ground` asking the channels entry's plan: 193 MB of tile reads), `placement` "channels" 14.9 s (187 MB), and `water` 34.3 s, which read 1.39 GB — if all of it is tile records, 77,000 tiles at 17,956 bytes, more than the 53,086 under all the instances, which is the instances' pattern again: points asked in an order the cache cannot hold. The candidates are the channel generator's `open` and `expand` (`water_channel`), which sample the ground along 278 channels' stations; the reader's own part around them (`read_scene_water_channels` writing every `WaterChannel` into a `JsonValue` for the generator's `open` to read back, and copying each channel into `SceneWater::channel_source`) is small beside 34 s and was not measured apart. The mend described: the generator asks its stations' ground through `heights_by_place` or its own walk in place order.
- **W51-3** (each mesh's source images and index streams, and the upload's host copy): `release meshes` (8.2 s freeing the per-mesh DAGs on the main thread) and the `SourceMesh` copies of `authored forms` are lifetimes, and stay as they are here.
- **D40's description copy**: `load_scene` copies its `SceneDesc` whole (0.7 s, the 470,641 instances); taking it by value from a caller that has no further use of it is an interface change for a later row.

## The three later stages, named for a later row

| stage | seconds | largest zones |
|---|---:|---|
| `gpu scene` | 93.9 | `upload materials` 46.8 (3.15 GB read, 10.4 s of CPU: the textures, nothing named inside); `upload geometry` 31.3 (25.7 unnamed before `flora motion` 3.3); `terrain masks` 7.4 (416 MB read); tail 5.8; `ray tracing` 1.1 |
| `renderer` | 14.2 | `opacity micromaps` 12.1; `pipelines` 0.3 (warm cache) |
| `world create` | 29.9 | 27.6 s unnamed after `ground masks` 0.7: the world opening its placement entries, the authored one reading back its tree of 465,000 instances (`schema::from_json`, the bench's read row) among them |

`gpu scene` raises private bytes from 15.8 to 30.2 GB, the load's peak.

## Machine state of the benches

`msvc-release` of this branch, 2026-11-18 between 22:55 and 23:00, with the no-driver environment (`tools/lib/NoDriver.psm1`), `--wait-quiet` given and no quiet window found. The harness's lines: synthetic heights "cpu 99.8% (others 99.8%, own 2.8%), gpu 1%, session unlocked"; staging heights "cpu 99.0% (others 96.0%), gpu 3%, session locked", 48 GB of commit free at its start; authored params "cpu 100.0% (others 97.7%), gpu 0%, session locked". Every number is an upper bound; the ratios are the measurement.
