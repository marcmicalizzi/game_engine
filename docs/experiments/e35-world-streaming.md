# E35: a thousand ruins streamed tile by tile along the desert flythrough

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), E35; [05 §5.5](../plan/05-simulation.md#55-reconciliation-when-a-tile-activates), [04 §4.9](../plan/04-renderer.md#49-streaming-and-residency), [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content)):** E34 said the desert's ruins should be blocks near and sections (or a slab) far, decided per tile, and left the switching unbuilt. With the world capability's tile ring ([world](../subsystems/world.md)) and a renderer that takes instances between frames ([renderer](../subsystems/renderer.md#instances-that-come-and-go)), what does a thousand-building desert cost when it is streamed round the camera rather than read whole — per frame in tiles, events, pairs and GPU time; per event in CPU (the ring's update, assembling or laying a tile's building, handing the renderer its instances, reading and writing a tile in the store); and how much does a building **pop** when its tile moves between representations with nothing to cross-fade it?
- **Date:** 2026-09-25. **Machine:** Intel Core i9-10980XE (18 cores, 36 logical CPUs), 64 GB, Windows 11 Pro 26200; **GPU:** RTX 5090, driver 610.88, Vulkan 1.4. **Build:** `msvc-release`, the world streaming branch (`worktree-agent-ae7fb01b5595bfdd7`) as its commits record it. The rows marked *v0* were taken with the renderer's first insertion, which wrote both instance tables whole into buffers made for them on every change; the others with the committed one, which writes only the tail into kept buffers, and with the pair budget decided by ring and distance (below).
- **Machine state:** shared. Every group of runs held the GPU lock on its own (`ENGINE_GPU_LOCK_OWNER=agent-world`, released between groups), and every run waited up to 60 s for a quiet machine (`--wait-quiet 60`) and measured either way; three of the thirteen rows below found one. Each row quotes the summary's `machine_state` as other processes' share of the CPU at the run's start and end: 3–18% at every start but one (38%, *v0* boxes with mid blocks), 5–49% at the ends; the GPU ours at every start but two (17% and 14% busy at the start of the two ashlar rows with mid blocks), with 7,490–9,285 MiB of its 32,607 MiB held by other processes. The bench numbers ran beside another agent's full test suite, which held the GPU lock (11.9% of the CPU, the GPU idle).
- **Decision:** none of its own: status notes in [05 §5.5](../plan/05-simulation.md#55-reconciliation-when-a-tile-activates), [04 §4.9](../plan/04-renderer.md#49-streaming-and-residency) and [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content), and the rules and defaults [ADR-0040](../adr/0040-the-tile-ring.md) (proposed) records. It changed two things before it was written up: the renderer's insertion (from rewriting both tables whole to writing the tail in place) and the ruins consumer's pair budget (from first come in tile order to nearest first).

## Setup

**What was built** ([world](../subsystems/world.md)): the tile ring — `sim::TierAssignment` over 32 m tiles round the camera, with hysteresis and a budget of 8 activations and 16 deactivations an update — and its ruins consumer, which assembles a tile's building when the ring takes the tile in (`ruins::Assembler` for sections, `ruins::BlockAssembler` for blocks), assembles it again when the tile moves to a ring that draws it differently, drops it when the ring lets it go, and after every update that changed a tile hands the renderer every drawn tile's instances in tile order as the scene's **tail**, behind the terrain. The renderer takes the tail between frames after the frames in flight finish (`SceneRenderer::set_dynamic_instances`). engine-view updates the ring before every frame from that frame's camera, and restarts it from an empty ring, filled without a budget, at the start of every repeat, so the three repeats fly the same world.

**The rings.** The defaults: blocks inside 1.5 tiles (48 m), sections with their debris inside 8 (256 m), sections without debris (`Detail::walls`) inside 24 (768 m), nothing beyond; hysteresis 0.15 (a tile leaves a ring 15% past its radius). E33's ashlar kit is 33,000 pairs a building in sections, so under the default rings — about 970 buildings along this path — it would need twice the visibility id's 2^24 pairs; its main rows use rings of **1.5, 6 and 10 tiles** (48, 192 and 320 m) instead, and one row runs it under the defaults to see the budget at work.

**The buildings**: E34's — world seed 2026, 32 m tiles, the 1,000 tiles of the square (−16, −16)–(15, 31) the seed ranks first, wind from 30°, on the desert overlook's terrain (2,049² vertices, 187,660 clusters; the square covers the camera path). Two section kits: the **kit of boxes** at E33's sizes with an inside corner (`engine-content ruins-kit --e33-sizes --inside-corner`: one cluster a member) and **E33's ashlar kit** (nineteen textured members of 641–5,863 clusters). Two block kits, E34's: **low** (a chamfered box of 44 triangles, one cluster) and **mid** (about 5,000 triangles, 105–111 clusters).

**The flythrough**, E34's: `engine-view --offscreen` at 1920 × 1080, the desert overlook's path (40 s, 1,037 m over the ground, 26 m/s on average and 47 m/s at its start) resampled to 600 frames — so **1.7 m of camera movement between two updates**, about seventeen times what a running player covers in a 60 Hz tick — three repeats, occlusion on, shadows off, the hardware rasterizer. The streamed rows add `--world-log`; the handover rows fly the path once more afterwards with `--world-handover`. Scenes and scripts are under `game_engine_local\world\e35\` (never committed):

```powershell
engine-content ruins-kit D:\workspace\game_engine_local\world\e35\kit-boxes --e33-sizes --inside-corner
pwsh D:\workspace\game_engine_local\world\e35\make-scenes.ps1     # scene-<row>.json; a streamed one has "world": {...}
$env:ENGINE_GPU_LOCK_OWNER = 'agent-world'
pwsh tools/gpu-lock.ps1 run -Purpose "E35 ..." -Exec "pwsh -NoProfile -File D:\workspace\game_engine_local\world\e35\run-group.ps1 -Scenes boxes-world,ashlar-world-mid"
#   each: engine-view --scene scene-<row>.json --camera-path content\test-scenes\desert-overlook\camera-path.json
#     --offscreen --benchmark out\<row>.jsonl --frames 600 --repeat 3 --width 1920 --height 1080
#     --shadows off --ddc <local ddc> --log info --wait-quiet 60 [--world-log out\<row>.world.jsonl]
#   -Handover: --repeat 1 --world-handover out\handover-<row>.handover.jsonl
pwsh D:\workspace\game_engine_local\world\e35\analyze.ps1 -Names <rows>      # the tables below
pwsh D:\workspace\game_engine_local\world\e35\handovers.ps1 -Names <rows>
build\msvc-release\systems\world\engine_world_bench.exe --wait-quiet=120 --repeats=5
```

## Results

### Read whole against streamed

Pairs are the scene's (instance, cluster) pairs — one cull thread each — over the path's frames; the terrain alone is 187,660. *Frame* is the wall time from one frame's submission to the next, which for a streamed row includes the ring's update and handing the renderer its tail; the renderer's own CPU time is 0.06–0.17 ms a frame in every row.

| scene | buildings drawn | pairs, min–max (median) | visible pairs, median (p95) | cull ms, median (p95) | all passes ms, median (p95) | frame ms, median (p95) | GPU memory | others' CPU |
|---|---|---|---|---|---|---|---|---|
| the terrain alone | — | 187,660 | 804 (1,244) | 0.026 (0.028) | 0.116 (0.129) | 0.14 (0.16) | 432 MiB | 7–26% |
| 1,000 boxes in sections, read whole | 1,000 | 235,886 | 4,018 (9,582) | 0.030 (0.032) | 0.131 (0.143) | 0.15 (0.18) | 432 MiB | 6–17% |
| 1,000 boxes in low blocks, read whole | 1,000 | 914,700 | 38,834 (114,169) | 0.077 (0.081) | 0.219 (0.313) | 0.24 (0.34) | 498 MiB | 10–10% (quiet) |
| **boxes streamed**, default rings, low blocks near | 555–972 | 202,274–211,167 (208,937) | 2,855 (3,980) | 0.030 (0.032) | 0.128 (0.143) | 3.64 (4.21) | 432 MiB | 6–5% (quiet) |
| *v0* | | | | 0.030 (0.032) | 0.128 (0.143) | 4.62 (5.22) | | 4–15% |
| **boxes streamed**, default rings, mid blocks near | 555–972 | 319,536–826,003 (577,532) | 2,876 (3,980) | 0.036 (0.039) | 0.132 (0.149) | 4.23 (4.90) | 434 MiB | 7–15% |
| *v0* | | | | 0.036 (0.039) | 0.132 (0.149) | 6.35 (7.36) | | 38–32% |
| 100 ashlar buildings in sections, read whole | 100 | 3,414,823 | 2,924 (5,908) | 0.088 (0.096) | 0.184 (0.197) | 0.21 (0.23) | 2,220 MiB\* | 3–6% |
| 1,000 ashlar buildings in sections, read whole | 1,000 | 33,245,802 | refused: more than 2^24 pairs | | | | | |
| **ashlar streamed**, rings 1.5/6/10, mid blocks near | 146–248 | 5,115,395–9,145,824 (8,472,877) | 20,568 (32,204) | 0.206 (0.219) | 0.333 (0.356) | 14.6 (16.7) | 2,631 MiB\* | 8–30% |
| *v0* | | | | 0.260 (1.704) | 0.400 (2.145) | 43.2 (56.8) | 2,604 MiB | 4–49% |
| **ashlar streamed**, rings 1.5/6/10, low blocks near | 146–248 | 4,786,936–8,670,127 (8,113,835) | 20,566 (32,204) | 0.196 (0.209) | 0.322 (0.346) | 16.1 (20.7) | 2,595 MiB\* | 18–45% |
| *v0* | | | | 0.243 (1.607) | 0.381 (1.987) | 41.7 (54.3) | 2,572 MiB | 5–27% |
| **ashlar streamed**, default rings, mid blocks near | 487–518 of 555–972 | 16,667,786–16,777,147 (16,760,401) | 30,340 (49,154) | 0.401 (0.413) | 0.550 (0.572) | 34.2 (53.6) | 2,847 MiB\* | 7–10% (quiet) |

\* E34's caveat: each textured ashlar member uploads its own copy of the kit's atlas, so most of these rows' memory over the terrain is duplicated images. Since 2026-09-25 the renderer uploads one texture per distinct image ([renderer](../subsystems/renderer.md#one-upload-per-distinct-image)): the kit's six images are 80 MiB instead of 1,440, and the streamed row with mid blocks near, flown for 60 frames (1920 × 1080, `msvc-debug`, other processes at 3–9% of the CPU, quiet), holds 1,098 MiB at its last frame where the build before holds 2,378 MiB at the same frame. The world handed the renderer 62 tails in those frames and not one uploaded a texture; the kit's six were uploaded with the scene's meshes, once.

The ashlar rows with rings of 1.5/6/10 report two frames of 600 whose visible pairs differ between repeats (`deterministic` false): the frames at which the first repeat's tail grew the pair stride, which makes the per-pair buffers again and clears the occlusion history, so that frame's first pass drew nothing and its second pass tested everything (44,418 visible pairs against 21,257 in the later repeats, whose stride was already grown). The pictures are the same; the count is the cull's.

**Per path frame, the ring** (recorded frames only, 3 × 600):

| rings | active tiles, min–max (median), per ring at the median frame | frames with an activation / a move / a deactivation | activations (most in one update) | moves | deactivations | activations the budget deferred | ring update µs, median (p95; p99; max) |
|---|---|---|---|---|---|---|---|
| default, 1.5 / 8 / 24 tiles | 1,810–2,090 (2,036): 6, 220, 1,810 | 1,296 / 1,197 / 1,125 of 1,800 | 4,542 (8) | 3,546 | 3,720 | 471, over 153 frames | 81.6 (244; 258; 428) |
| ashlar's, 1.5 / 6 / 10 tiles | 312–365 (354): 6, 124, 224 | 900 / 1,116 / 828 | 1,929 (6) | 2,823 | 1,794 | none | 21.0 (48; 62; 97) |

The events follow the camera's speed: the first hundred path frames (the fast run through the dunes) take 490 activations under the default rings, the saddle's slow climb 94. The bench agrees with the flythrough on the ring: **77 µs** an update of the default rings round one moving observer, 173 µs round two and 367 µs round four (the candidates are enumerated per observer and merged), and **303 µs** for a first fill from nothing, 1,810 tiles at once.

### What an event costs on the CPU

Per frame that had any event (1,545 of the 1,800 under the default rings, 1,401 under the ashlar's), milliseconds, median (p95; p99; max). *Calls* are the ruins consumer's activations, moves and deactivations — assembling a tile's building, laying it in blocks, dropping it — and *commit* is deciding which tiles the budget draws, concatenating their instances into the tail and the renderer taking it.

| row | calls | commit | the renderer taking the tail, mean a hand-over (worst) |
|---|---|---|---|
| boxes, low blocks near | 0.048 (0.275; 0.473; 1.60) | 3.46 (7.43; 10.2; 11.4) | 3.24 (10.1) |
| *v0* | 0.051 (0.278; 0.472; 3.61) | 4.38 (7.44; 11.3; 13.6) | 4.19 (12.4) |
| boxes, mid blocks near | 0.046 (0.272; 0.486; 1.30) | 4.05 (4.95; 11.2; 12.2) | 3.76 (10.9) |
| *v0* | 0.056 (0.268; 0.434; 0.58) | 6.15 (7.13; 7.68; 9.42) | 5.56 (8.8) |
| ashlar, mid blocks near | 0.052 (0.311; 0.593; 1.31) | 14.9 (26.0; 31.3; 37.5) | 15.3 (37.3) |
| *v0* | 0.055 (0.318; 0.602; 1.76) | 45.1 (65.3; 82.8; 254.8) | 47.3 (254.6) |
| ashlar, low blocks near | 0.052 (0.301; 0.537; 1.43) | 16.4 (22.4; 31.5; 39.5) | 16.8 (39.3) |
| *v0* | 0.057 (0.333; 0.588; 1.36) | 42.7 (60.6; 90.1; 958.6) | 48.1 (958.3) |
| ashlar, default rings | 0.068 (0.347; 0.743; 1.82) | 34.0 (114; 193; 383) | 45.3 (382) |

Over each whole run (warm-ups and restarts included): **a building assembled in sections costs 17 µs** with the kit of boxes (5,007 in 85.2 ms) and **22 µs** with the ashlar kit (3,300 in 71.4 ms); **a building laid in blocks 235–238 µs** (186 in 43.7 and 44.2 ms), which includes the section assembler the block layer runs first and the scene instances it makes. A deactivation drops a tile's instances and costs nothing measurable beside them. **The ring's update is 20–80 µs** a frame at the median. **The renderer taking the tail is the cost**, two orders of magnitude above everything else: every change rewrites every drawn tile's instances and pair-table entries after the prefix, because the pairs are one prefix sum — 21,000 instances and pairs for the boxes, and for the ashlar kit 9,300 instances and 8.3 million pairs, whose 66 MB of pair table is most of its 15 ms. At the budget's 16.7 million pairs the table is 134 MB and a change costs 34 ms at the median.

**The store** (`world.store.tile`, in-memory SQLite): writing a tile of 64 records — projections and snapshot — and reading it back costs **0.51 ms**, 8 µs a record; E6's file-backed numbers ([store](../subsystems/store.md)) are the ones a save on disk pays. engine-view's world has no store consumer; engine-host's headless run does, and its per-call `store_ms` is where a session's number is.

### The pair budget under the default rings

Under the default rings the ashlar kit wants 555–972 buildings and 20–33 million pairs; the tail holds 487–518 buildings and stays within 110,000 pairs of 2^24 at every frame. The first version decided the budget per event, in tile order, and a first fill gives the tiles to the renderer in tile order, so the budget went to the western tiles whatever their distance: **132 tiles of the inner ring and 315 of the middle one were refused** while tiles at the horizon held the pairs. The committed version decides at the commit, over every tile held, by ring and then by distance, and draws none after the first that does not fit: across 1,149 changes of what it withheld (63–463 tiles at a time), **every tile withheld was in the outer ring**. It costs a sort of the tiles held on each commit, which does not show beside the tail.

### The handover pop

The handover pass flies the path once more, one frame at a time and untimed, and for every tile an update moves to a ring that draws it differently draws the frame twice: as the update left it, and with only that tile's instances put back as they were. Everything else the update did is in both pictures, so what differs is the one tile. Each picture is drawn once before it is captured, so its visible pairs are its own. *Changed* is the pixels covered in one picture and not the other plus those covered in both at depths more than a thousandth apart; *moved*, those whose depth moved by more than a hundredth (half a metre at 50 m) — geometry that moved rather than relief on a surface that stayed; *colour*, those that changed by more than 8 of 255 in any channel. On-screen tiles only; 1080p is 2,073,600 pixels.

| kit | change | at | tiles (on screen) | changed px, median (p95; max) | moved px, median (max) | colour px, median (p95; max) | the tile's pixels, before → after (median) | visible pairs, after − before (median) |
|---|---|---|---|---|---|---|---|---|
| boxes | sections → blocks | 45–48 m | 58 (33) | 13,924 (44,077; 80,690: 3.9%) | 12,422 (57,078) | 18,247 (64,221; 82,929) | 4,372 → 8,205 | +433 |
| ashlar | sections → blocks | 45–48 m | 58 (34) | 11,572 (44,930; 75,248: 3.6%) | 8,060 (54,265) | 20,843 (65,132; 79,039) | 2,183 → 6,626 | −236 |
| boxes | walls → sections (debris appears) | 253–256 m | 327 (136) | 35 (116; 154) | 0 (60) | 38 (134; 208) | — | +25 |
| ashlar | walls → sections | 189–192 m | 248 (119) | 40 (130; 207) | 0 (94) | 45 (158; 251) | — | +18 |
| both | blocks → sections, sections → walls | 55–58 m, 221–298 m | 635 (0) | 0 | | | | |

### After the rubble rule

The table above found the two forms of a building agreeing on everything but the ground, so the rubble was made one rule for both ([ruins](../subsystems/ruins.md#the-rubble-rule)): a field of sites per wall, each a block's worth of what came down, on which the block layer lays one fallen block and the assembler a heap of debris members as large — `debris_per_module` (3) blocks' worth per module of wall brought down, about a tenth of what falls, where the block layer had kept every fallen block and the assembler a few pieces of its own. The handover pass was then flown again, before and after, with a pass that also splits each pop by how far its pixels moved (`depth_bins`) and by whether either picture shows the tile's rubble (`rubble_changed`; [apps](../subsystems/apps.md#engine-view---world-a-streamed-scene)).

- **Runs:** 2026-09-25, `msvc-release`, the same scenes, path, rings and settings as the rows above (`game_engine_local\world\e35\rubble\run-handover.ps1 -Bin <bin> -Tag before|after`, `--repeat 1 --world-handover`, and `analyze-handover.ps1` for the tables; the outputs beside them), each pair of scenes in one hold of the GPU lock (`ENGINE_GPU_LOCK_OWNER=agent-rubble`); *before* is the branch's first commit, which changed only the measurement, and reproduces the table above to the pixel (13,924 / 44,077 / 80,690 and 11,572 / 44,930 / 75,248). **Machine state:** before, 3–14% of the CPU in other processes and the GPU ours (0–1% busy at the start; 33% at the end of the ashlar run); after, 31–37% of the CPU in other processes (another agent's work and this agent's bench), the GPU ours and idle. The pixel counts do not depend on the load; the milliseconds below do, and the *after* ones are upper bounds.

Sections → blocks at 45–48 m, the 33 (boxes) and 34 (ashlar) tiles on screen:

| kit | | changed px, median (p95; max) | moved > 1%, median (max) | colour px, median (p95; max) | rubble's share of the changed px | the tile's px, before → after (median) | instances, before → after (median) |
|---|---|---|---|---|---|---|---|
| boxes, low blocks | before | 13,924 (44,077; 80,690: 3.9%) | 12,422 (57,078) | 18,247 (64,221; 82,929) | 65% | 14,491 → 25,452 | 44 → 640 |
| | **after** | **7,791 (27,933; 30,737: 1.5%)** | 4,897 (20,539) | 12,481 (36,904; 50,929) | 27% | 14,568 → 18,090 | 73 → 377 |
| ashlar, mid blocks | before | 11,572 (44,930; 75,248: 3.6%) | 8,060 (54,265) | 20,843 (65,132; 79,039) | 56% | 11,379 → 20,858 | 37 → 565 |
| | **after** | **9,070 (30,017; 30,712: 1.5%)** | 3,556 (15,999) | 14,915 (50,502; 52,500) | 23% | 12,433 → 14,711 | 87 → 298 |

The displacement histogram — every on-screen tile's changed pixels summed, by how far the depth moved as a share of the distance (at 47 m: 5 cm, 14 cm, 0.47 m, 1.4 m, 4.7 m), coverage being a pixel one picture covers and the other does not:

| kit | | changed px, all tiles | coverage | 0.1–0.3% | 0.3–1% | 1–3% | 3–10% | > 10% |
|---|---|---|---|---|---|---|---|---|
| boxes | before | 586,449 | 0.2% | 5.0% | 18.3% | 48.1% | 16.7% | 11.7% |
| | after | 297,793 | 0.3% | 13.6% | 17.2% | 19.9% | 27.2% | 21.8% |
| ashlar | before | 539,513 | 0.3% | 13.8% | 17.9% | 44.1% | 12.9% | 11.1% |
| | after | 316,292 | 0.1% | 26.0% | 22.4% | 17.3% | 16.1% | 18.2% |

**What it says.** The pop halved (the sum over every tile −49% for the boxes, −41% for the ashlar; the worst tile from 3.9% and 3.6% of the frame to 1.5%), and **the rubble's part of it fell by four fifths** — from 380,000 to 81,000 pixels (boxes) and from 300,000 to 72,000 (ashlar) — while the walls' part stayed where it was (206,000 → 217,000 and 240,000 → 245,000). So the 1–3% band, a stone's height on the sand, is what went: 48% and 44% of the pop before, 20% and 17% after. **What is left is the walls**, three quarters of it: a section building stands at its members' height variants (100%, 55%, 30% of the wall in the kit of boxes; the ashlar's h100/h50/h25), the tallest not above the ruin line, and sunk where none is low enough, while the block building stands a block wherever the line is above its top give or take half a course — so the two agree on the line and not on the steps down to it, and a wall top that differs by a course or a variant is what the > 3% bands are (the sky or the sand behind a wall the other form does not have). The ashlar's remaining pop leans to relief (48% under 1%, where its sections' scan-quality faces meet the synthetic blocks' flat ones); the boxes' to wall tops (49% over 3%). The heaps are on the blocks' sites but are not blocks, which is the 23–27% still rubble: a heap of two to four stones round a block's centre covers its footprint but not its shape.

Walls → sections at 190–256 m (debris appears), on screen: 35 (116; 154) → 44 (148; 204) changed pixels for the boxes and 40 (130; 207) → 67 (224; 333) for the ashlar — three times the ashlar debris members, still under 0.02% of the frame. Promotions only, as before: none of the 635 demotions drew a pixel.

**What it costs.** The tail (the path's median frame) went from 208,937 to 212,351 pairs for the boxes and from 8,472,877 to 8,560,310 for the ashlar (+1.6% and +1.0%): the middle ring's section buildings carry more debris and the inner ring's block buildings fewer fallen blocks (21,277 → 24,691 and 9,259 → 12,777 ruin instances in the tail; the ashlar tile that switched at the median, 27,625 → 30,171 pairs in sections and 67,209 → 35,111 in mid blocks). Over each run, a building assembled in sections cost 25.0 µs for the boxes (24.7 before) and 34.3 µs for the ashlar (23.5 before: three times the debris members, each with its own terrain query), and one laid in blocks 140–142 µs where it cost 240–246 (the fallen blocks it no longer places were most of its ground queries) — upper bounds, from the loaded after runs. The renderer taking the tail is still the cost that matters (4.3 and 19.4 ms a change against 6.6 and 17.1 before, at a third of the CPU busy).

**Would a cross-fade hide what is left?** A median 8,000–9,000 pixels and at worst 1.5% of the frame, three quarters of it wall tops a course or a variant apart and relief on faces that stayed, at 45–48 m — a change a dissolve over a quarter to half a second would hide where a one-frame pop does not; before the rule the same fade would have dissolved heaps of rubble in and out. What it would need, since a visibility buffer has one surface per pixel and so no alpha: **both instance sets of the tile in the tail for N frames**, and a **screen-space dither** — a fragment of the old set drawn where a hashed threshold of the pixel is above the fade, one of the new set where it is below, the same pattern for both so every pixel shows exactly one — in every rasterizer (the mesh-shader, software and both vertex-path draws) and the ray path, pixel-identical among them as the visibility tests require. `gfx::InstanceDesc` has a spare word (`pad`) where a fade's start frame and direction fit without changing its 96 bytes, and the fade then follows from the frame index with no per-frame write; Hi-Z stays conservative (a dithered hole shows something farther); the cascaded shadows would dither with it for those frames. With the tail rewritten whole on every change it costs one more insertion per handover (3–45 ms each today), which the incremental insertion E35 asked for would remove. That is renderer work across four rasterizers, the ray path and their tests, and the consumer holding two forms of a tile at once: several days, not one, so it is not built here; making the walls agree (a section kit whose variants are whole courses, or a switch farther out where a course is a pixel) is the cheaper next step.

## What surprised me

- **The renderer taking the tail is the whole cost of streaming, and the ring is noise.** 20–80 µs for the ring, 17–22 µs a building assembled and 0.24 ms laid, against 3.2–45 ms for the renderer to take a tail — on six frames of seven along this path. The ring was the part expected to need care.
- **Rewriting both tables whole also cost the GPU.** With the first insertion the ashlar rows' p95 cull was 1.70 ms against a 0.26 ms median; writing the tail into kept buffers took it to 0.219. Each change had made the 70 MB pair table and the instance table in new allocations; the cause inside the driver was not isolated.
- **A streamed world of boxes costs the GPU what the whole desert does.** 0.128 ms against 0.131: the tail is as many pairs as a thousand buildings in sections, because the default outer ring holds most of them. Streaming pays where a whole read is refused — the ashlar kit, 33 million pairs whole, streams at 5–9 million — not where it fits.
- **Walls only is not a far representation, for pairs.** An ashlar tile moving from sections to walls goes from 25,726 pairs to 24,377 (the debris stones are small DAGs; the members are the pairs), so a far ring of walls costs the cull nearly what sections do. The ring's representation must cut *pairs*, and nothing but E33's slab (or dropping the building) does.
- **The budget in tile order refused buildings beside the camera.** Obvious once seen; invisible in every test with a budget that fit.
- **Only promotions are ever on screen.** A camera that moves forward leaves tiles behind it: of 635 demotions (blocks to sections, sections to walls), none drew a pixel; every visible pop is a tile coming *closer*.
- **The blocks-for-sections pop is mostly the rubble, not the relief.** The block building covers two to three times the pixels its section building did (4,372 → 8,205 and 2,183 → 6,626 at the median): the block layer drops every block that did not stand beside its wall (E34's `debris_kept` 1), where the section assembler scatters a few debris members. So 70–90% of the changed pixels *moved* by more than half a metre — stones appearing on the sand — rather than a wall's face changing depth by centimetres. The two representations are one ruin in footprint, walls and openings, and not in what lies on the ground. (Since fixed: both lay one rubble field, and the pop halved; [After the rubble rule](#after-the-rubble-rule).)

## What it decides

- **The ring's rules and defaults stand** for the kit this project measures near (the boxes and low blocks): the ring is 80 µs a frame, the budget defers 471 of 4,542 activations at a flythrough's speed and none at the ashlar rings', and the tail stays under the budget with the nearest tiles drawn first. [ADR-0040](../adr/0040-the-tile-ring.md) records them, proposed.
- **The insertion has to become incremental before a heavy kit streams at a game's frame rate.** A tail rewritten whole costs 3 ms for one-cluster buildings and 15–34 ms for E33's; at a runner's speed there are about seventeen times fewer changes than here, which makes it a hitch every second or so and not every frame, and still a hitch. The shape of the fix is known ([renderer](../subsystems/renderer.md#instances-that-come-and-go)): stable per-tile blocks, the pair entries expanded on the GPU from the instance table, and double-buffered tables so a change does not wait for the device.
- **The budget is decided over the tiles held, nearest first**, never per event.
- **The far ring needs a far representation that cuts pairs**, or the ashlar kit's outer ring stays at 10 tiles (320 m).

## What it does not decide

- **The cross-fade.** The pop is measured, not hidden: up to 4% of the frame at 47 m for one tile, mostly rubble, and since the rubble rule up to 1.5%, mostly wall tops ([After the rubble rule](#after-the-rubble-rule), which says what a dither between the two instance sets would need). Whether the rest is taken by a dither, by walls whose variants agree with the blocks' courses, or by a farther switch is the next question for [ruins](../subsystems/ruins.md#the-block-layer).
- **The far tier** (E33's slab) and the outer ring's radius with it.
- **A game's tile size and speed.** 32 m tiles at 1.7 m an update is a stress case; the plan's 64–128 m tiles at a player's speed change a tile far less often, and a tile's building would be larger.
- **The baseline tier.** One GPU (RTX 5090, mesh shaders); the TITAN Xp's vertex path pays per pair differently and was not run.
- **Shadows and ray tracing.** Shadows were off, and a dynamic scene has no ray tracing chain yet; a cascade runs the visible list again, so it multiplies what the visible pairs cost.
- **The document and the store along a path.** engine-view streams ruins only; the document and store consumers are measured by their tests and the bench, and a headless session that walks observers through a partitioned document is where their cost at scale will be measured.

## Caveats

- **A busy machine**, as the header says: every flythrough row is an upper bound on its CPU numbers, and the insertion's cost at 16.7 million pairs varied by half between two runs of the same code (29 and 45 ms a hand-over).
- **One path, one seed, one tile size**, and a camera that moves seventeen times as far between updates as a runner does between ticks.
- **The handover pass** measures one tile against the frame the update left, with occlusion on; a tile hidden by a nearer one pops nothing, which is the truth for that frame and not for the next.
- **Synthetic blocks and synthetic ruin rules**, as E34's: a block kit that keeps less rubble would change the pop more than anything in the renderer.
