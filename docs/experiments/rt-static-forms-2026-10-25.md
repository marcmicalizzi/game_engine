# The ray tracing chain sized by what moves (R94, 2026-10-25)

**The question.** The forest proof of concept's dense clearing wanted 580,000–633,000 cluster
acceleration structures a frame against a 1 GiB budget of about 118,000, so every frame of it
dropped instances' structures, and at an 8 GiB diagnostic budget the first frames still did while
the capacity grew. The chain builds a structure per visible pair every frame, the ring-1-and-beyond
distance forms included, whose geometry never moves. What does the frame have to build, and how
much of it can be built once?

**What was built** ([renderer](../subsystems/renderer.md#static-structures-for-rigid-forms)):
rigid distance forms drawn at their frontier — a fixed cut — and traced against cluster and
bottom-level structures built once per form, referenced per instance from the top-level structure.

## The scenes

The proof of concept's scenes as read-only data (`D:/workspace/forest_wildlife_poc`, never written
or run), their derived-data caches copied to scratch, through this worktree's own `msvc-release`
`engine-view`, offscreen, the proof of concept's own flags (`--terrain-tiles --terrain-far 6
--time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1 --deform-pool-mib 64
--rt-budget-mib 1024`, flora motion on at wind 0.7), `--wait-quiet 120`:

- **clearing** — v10's `clearing-wide-rt` scene (97,662 file instances, 32 m tiles in rings of
  1.5, 6 and 40) on its own `grass-clearing-path.json`, at the case's 11520×2160 single view,
  `--raster rt --shadows rt`, 180 frames. At main the scene is refused under traced shadows with
  their casters (its 5.0 M prefix pairs, the rings' slots and a tile's floor are 5.78 M of the
  5.59 M the chain's index space names over three runs), so every run here passes
  `--no-shadow-casters`, which the case's own run had been given by `resolve_settings` in its day.
- **hardwoods** — v8's `base.json` with W73's `world` block (32 m tiles, rings 1.5, 6 and 40) at
  `hardwoods-path.json`, 1600×900, `--raster hw --shadows rt --no-shadow-casters`, 120 frames.
  Without `--no-shadow-casters` the world's placements consumer withholds 1,184 tiles to stay under
  the same 5,592,405 pairs and the renderer then refuses the rest of the tail outright (2,949,035
  pairs that "would take the scene past 5592405"), so the frame draws the prefix alone — a defect of
  the consumer's budget against the layout's, not of the chain, and not fixed here.
- **crossing** — the same scene on a straight 60 s flight 40 m over the ground from (−950, −700)
  to (950, 2550), 1,800 frames, the heights read from the region's heightfield.

## Stage 1: static structures for rigid forms

GPU medians in ms over the run; "wanted" is the structures the frame asked the chain for, "built"
what it built; the chain is the records passes, the cluster, bottom-level and top-level builds.

| scene | run | visible pairs | wanted (max) | built (max) | overflow frames | dropped instances | chain | hw raster | trace | resolve | frame | machine_state (CPU by others, GPU busy, start / end) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clearing | before | 475,281 | 475,281 | 118,214 | 216 | 11,688,402 | 18.17 | — | 5.70 | 15.19 | 43.38 | 34% / 1% → 32% / 100% |
| clearing | static forms | 1,194,040 | 110,282 | 110,282 | 2 | 14,284 | 3.90 | — | 8.81 | 19.80 | 39.73 | 28% / 0% → 30% / 2% |
| hardwoods | before | 56,510 | 56,510 | 56,510 | 0 | 0 | 2.30 | 0.20 | — | 1.77 | 5.16 | 17% / 6% → 17% / 99% |
| hardwoods | static forms | 234,563 | 36,884 | 36,884 | 0 | 0 | 1.76 | 0.47 | — | 1.75 | 5.32 | 41% / 15% → 47% / 1% (another agent's engine-view held the GPU lock) |

**What it says.** At the clearing the frame wanted four times what the budget holds, and the forms
were more than three quarters of it: with them built once it wants 110,282 and fits, and the two
overflow frames left are the first ones, before the capacity's one growth lands (stage 2). The
chain's 18.2 ms was mostly the records pass's one-thread drop path walking 60,000 instances every
frame; complete, the chain is 3.9 ms. The trace and the resolve grew by 3.1 and 4.6 ms because the
picture is now whole: the 11.7 million instances' structures dropped before were surfaces the ray
path did not draw. The cost is the raster's: the frontier is the cut a camera at a ring's inner
edge asks for, so the hardwoods' raster cut quadrupled and the hardware raster went from 0.20 to
0.47 ms, while the chain fell from 2.30 to 1.76 ms. Every number is an upper bound by the
harness's rule: the machine was shared throughout.

## Stages 2 and 3: the first capacity, the drop order, the caster horizon

The same scenes and flags. "s1" is static forms alone; "all" is every stage of R94 that was built —
static forms, the first capacity from the scene's estimate, the drop order by distance, and the
caster horizon, which is off by default and inert here: all three scenes run with
`--no-shadow-casters` (above), so they have no casters for it to bound. Peak bytes are the chain's
largest `rt_bytes`, the end bytes its last.

| scene | run | wanted max / mean | built max | overflow frames | dropped instances | growths (hitch ms) | capacity end / peak | chain bytes end / peak | chain | hw raster | frame median / p95 | machine_state |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clearing | before | 475,281 / 475,281 | 118,214 | 216 | 11,688,402 | 1 (43.4) | 118,214 / 118,214 | 1.24 / 1.24 GB | 18.17 | — | 43.38 / 44.47 | 34% / 1% → 32% / 100% |
| clearing | s1 | 110,282 / 110,282 | 110,282 | 2 | 14,284 | 1 (49.0) | 118,214 / 118,214 | 1.38 / 1.38 GB | 3.90 | — | 39.73 / 42.23 | 28% / 0% → 30% / 2% |
| clearing | all | 110,282 / 110,282 | 110,282 | **0** | **0** | **0** | 118,214 / 150,047 | 1.38 / 1.38 GB | 4.34 | — | 41.20 / 43.85 | 55% / 4% → 72% / 4% |
| hardwoods | before | 56,510 / 56,510 | 56,510 | 0 | 0 | 0 | 65,536 / 65,536 | 0.65 / 0.65 GB | 2.30 | 0.20 | 5.16 / 5.87 | 17% / 6% → 17% / 99% |
| hardwoods | all | 36,884 / 36,884 | 36,884 | 0 | 0 | 0 | 49,152 / 150,447 | 0.80 / 1.39 GB | 1.86 | 0.47 | 5.57 / 6.17 | 40% / 3% → 40% / 14% |
| crossing | before | 81,720 / 33,607 | 81,720 | 2 | 16,968 | 5 | 28,672 / 98,304 | 0.49 / 0.88 GB | 1.51 | 0.08 | 4.06 / 7.11 | 45% / 5% → 69% / 3% |
| crossing | all | 38,279 / 16,865 | 38,279 | **0** | **0** | **0** | 20,480 / 150,447 | 0.61 / 1.40 GB | 1.29 | 0.27 | 4.54 / 9.91 | 35% / 2% → 23% / 3% |

**The new critical number is the structures built per frame**: at the clearing 110,282 (was 475,281),
at the hardwoods 36,884 (56,510), along the crossing at most 38,279 and 16,865 on average (81,720
and 33,607). What is built once is per form, not per instance: three forms of 53 clusters in the
test grove, and on the forest the forms' frontiers (a few tens of thousands of clusters over 90–148
forms) for all their tens of thousands of instances.

**What the stages bought.** The clearing's first frame is complete and nothing grows: before, frame 0
dropped 60,530 instances and the one growth took 43–49 ms. The crossing's five growths and two
overflow frames are gone. **What they cost**: memory up front — a scene past the budget now holds
the budget from its first frame (1.38–1.40 GB of chain at the peak on all three, where the hardwoods
and the crossing had peaked at 0.65 and 0.88 GB) until the first shrink window passes, which the
hardwoods' and the crossing's end bytes show — and the raster's finer far trees (0.20 → 0.47 ms at
the hardwoods, 0.08 → 0.27 ms along the crossing, where the visible cut is 5.5 times larger). The
crossing's frame p95 rose from 7.1 to 9.9 ms with the machine as shared as it was; its median moved by
0.5 ms. Every time is an upper bound.

**The caster horizon** is measured on the test grove, since the forest's scenes run without casters:
800 trees on 16 m tiles in rings of 4 and 8, 640×360, traced shadows, `--shadow-distance 30`: 7,051
casters without the horizon, 1,283 within it and 5,768 counted past it, and no pixel of the traced
shadows changed (`engine-view: a shadow caster past the caster horizon casts nothing and is
counted`).

## What was not built

**The rays' LOD bias** (`renderer.rt.lod_bias_px`, a coarser cut for the structures than for the
picture) was not built. A structure cut coarser than the picture puts a coarser surface beside the
picture's own receivers, and where it stands above them a shadow ray leaving the receiver meets it
within a few centimetres — the shards [One cut for every view](../subsystems/renderer.md#one-cut-for-every-view)
describes for two views' cuts, and the self-shadowing the cascades showed at a coarser cut
([cascaded shadow maps](cascaded-shadow-maps.md)): the cost is not a traced shadow's edge moving
by the cut's error but its receivers shadowing themselves. Avoiding that needs either a ray offset as
large as both cuts' errors (which loses every contact shadow smaller than the error) or a second cull
run for the structures and a per-receiver test, and with the forms built once the clearing's frame
already fits its budget. It is left for a measured case that needs it.

**The forest's casters.** None of the three scenes can run with shadow casters on this engine: the
chain's index space over three runs (5.59 M pairs) is smaller than the clearing's fixed pairs, and
on the v8 forest the placements consumer's budget and the renderer's layout disagree by the blocks'
slack, so the tail is refused whole. Static forms change neither, because the forms' pairs are still
pairs of the visible list.