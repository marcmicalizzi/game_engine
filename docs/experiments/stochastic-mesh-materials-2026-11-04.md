# Stochastic mesh materials on the v29 ledge (R131, 2026-11-04)

**Question.** The forest proof of concept's v29 hand-off: a generated cliff repeats its one PBR
set — 1.8 m of `rock_boulder_dry` over a face tens of metres long — and the owner flagged the
repetition. Does R96's hex-tiled sampling, moved into a shared lattice and applied in a mesh
material's own UV plane ([renderer](../subsystems/renderer.md#stochastic-mesh-materials)), remove
the repeat without washing the rock out, and what does it cost the resolve?

**Method.** The v29 ledge repro (`reports/quality-v29/repro-ledge-wall/`, read as text; its
picture not opened): the scene, the five `foothill-ledge-*.rock.json` cliff recipes and
`ledge-close-path.json` copied into the agent's scratch, twice — once as they are (plain) and once
with `"sampling": "Stochastic"` in each recipe's `material` block (rocks schema `RockMaterial`
version 5) — and the terrain's tile cache copied too, so nothing was written under the proof of
concept. `engine-view --scene … --camera-path ledge-close-path.json --width 1920 --height 1080
--raster hw --shadows csm --offscreen --ddc <scratch> --frames 120 --benchmark … --capture …
--wait-quiet 120`, one process per variant under `tools/gpu-lock.ps1 run`, an `msvc-debug` build
(the GPU's timers time the same SPIR-V a release build runs; the CPU's numbers are not quoted). The
resolve's GPU milliseconds are the benchmark's per-frame `gpu_ms.resolve`, the first 20 records
dropped. The path holds one pose 38 m from the wall, where the wall fills about a third of the
picture.

## Cost

| variant | resolve median | p95 | min | frame GPU median | machine state (the harness's) |
|---|---|---|---|---|---|
| plain | 0.776 ms | 0.790 ms | 0.755 ms | 1.116 ms | others 81% CPU, GPU 70% busy: an upper bound |
| stochastic | 0.800 ms | 0.814 ms | 0.784 ms | 1.128 ms | others 64% CPU, GPU 0% busy |

**+0.02–0.03 ms on the resolve at 1920 × 1080 with a third of the picture on the cliff, +3%.** The
plain run shared the GPU with another job and the stochastic one did not, so the difference is if
anything understated by a little; neither was quiet (the box carried four other agents tonight). A
stochastic pixel reads colour, metallic-roughness and normal (and the variance chain and occlusion
where the material has them) three times where a tiled one reads them once, and computes the lattice
three times; on these BC-compressed maps at the footprint's mip the extra reads hit the texture cache
and cost little. Texture residency is unchanged: no lookup table, no second texture.

## The pictures

Captured to the agent's scratch (`b52/R131/ledge/plain.png`, `stochastic.png`, and a 2× crop of
both, `crop-compare.png`), and looked at:

- **The repeat is gone.** Plain, the wall is a lattice of the same dark blob and the same bump
  every 1.8 m — the grid reads from across the valley. Stochastic, the same set reads as irregular
  mottling, no direction and no period anywhere on the face or the ledge's top.
- **The contrast holds.** The light and dark patches are as strong as the plain set's; the wall
  reads neither flatter nor brighter (the operator keeps the mean and the variance — the GPU test
  holds the mean within 1% and the deviation above 0.8 of the plain one's; here the wall's average
  is the plain one's to the eye).
- **The normals at the seams.** No seam or crease shows: the slope pairs blend about zero with the
  same weights and gain, so the relief neither flattens between tiles nor steps at their edges. What
  does change is the relief's *pattern*: the plain set's one prominent bump, repeated, was the
  strongest thing on the wall; blended, it becomes many smaller irregular bumps.
- **At a few metres it reads as rock, not mush.** A second pose, 25 m along the same sightline —
  the wall a few metres off and about three quarters of the picture (`plain-near.png`,
  `stochastic-near.png`, side by side in `near-compare.png`): plain, the blob rows stand in
  columns a repeat apart; stochastic, the same blobs at the same sharpness and contrast, scattered.
  The blend's softening, where three tiles share a pixel near a lattice cell's middle, does not show
  as smear at this distance: each tile's texels are still the texture's own, read at the
  footprint's mip. Its cost there, the same flags and 40 frames:

| variant (near pose) | resolve median | min | machine state |
|---|---|---|---|
| plain | 0.544 ms | 0.532 ms | others 14% CPU, GPU 87% busy: an upper bound |
| stochastic | 0.635 ms | 0.621 ms | no warning recorded |

  **+0.09 ms, +17%, with three quarters of 1920 × 1080 on the stochastic material** — about
  0.06 ns more per stochastic pixel, so a full-width 11520 × 2160 picture wholly of such rock would
  pay on the order of 1.5 ms; a cliff in a forest view is a fraction of that.

## The plain kernel: the frame budgets' follow-up

The merged stack's frame budgets (2026-11-05, gate) put `endless-surround3-rt`'s resolve over
budget with **no stochastic material in the scene**. The first build put the dispatch and the
three-tile reads in every resolve kernel, so the plain path paid the stochastic one's registers.
Attributed in the agent's worktree, `msvc-release`, `tools/frame-budget.ps1 -Run
endless-surround3-rt -WaitQuiet 120` (budget 1.885 ms, limit 2.1235):

| build | resolve median | p99 | total median | machine state (the harness's) |
|---|---|---|---|---|
| efe7e9c5 (before R131) | 2.108 ms | 2.483 | 4.439 | busy: start 5.2% others, GPU 0%; end GPU 24% |
| R131 as first merged (dispatch in every kernel) | 2.240 ms | 2.631 | 4.550 | busy: start 3.3% others; end GPU 25% |
| R131 with the stochastic variant | 2.109 ms | 2.473 | 4.436 | quiet |

The fix compiles the stochastic path only into eight `*_stochastic` resolve modules that the
renderer selects when a material row carries the flag ([renderer](../subsystems/renderer.md#stochastic-mesh-materials)).
The four plain modules without terrain materials are now byte-identical to efe7e9c5's, which is
why the resolve is back to 2.108 ms to the microsecond. `erg-walk-surround3-rt` with the fix:
resolve 2.454 ms median (budget 2.318, limit 2.600), total 3.710 (budget 3.373, limit 3.760),
within budget, on a machine the harness called busy (the GPU 99% at the end).
## What was not measured

- **The lake-range camera with the full v29 scene** (`world/v29/lake-range-path.json`): not flown,
  for the agent's budget. The ledge is the repro the owner's complaint was about; the lake range
  shows the same material at a distance, where the per-pixel cost is the same per cliff pixel.
- A quiet machine. The numbers are the harness's upper bounds beside four other agents' work.
