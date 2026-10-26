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
