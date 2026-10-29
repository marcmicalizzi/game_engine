# Untraced species under a traced picture (2026-10-28)

Roadmap R110. **Question:** the forest proof of concept's v20 report found its grass absent from
the picture under `--raster rt` and present under `--raster hw --shadows rt`, in an isolated
grass-only scene with no drop and no overflow (`reports/quality-v20/ENGINE-AND-ASSET-ROADMAP.md`).
The cause was the caster policy: `traced = false` was implemented as "not in the chain". What does
the fix — the two contracts, drawn by camera rays and never met by a light's ray ([renderer](../subsystems/renderer.md#the-caster-policy),
[gfx](../subsystems/gfx.md#the-ray-mask)) — draw, and what does it cost?
**Answer:** the ray path now draws the grass **word for word as it draws it with the policy off**
(0 of 1,440,000 id words differ at 1600×900), and still casts no shadow from it. It costs what
tracing the grass costs: on the grass-only scene the chain wants 67,934 structures instead of
2,308 and the `rt` pass goes from 0.54 to 2.00 ms; on the v20 forest at 1600×900 it wants 1,146
more (99,811 → 100,957, `rt` 3.58 → 3.78 ms); at 11520×2160 it wants 247,302, which the device
default budget (2,025 MiB) holds with nothing dropped and the proof of concept's pinned 1,024 MiB
does not. The frame budgets' traced runs, which have no untraced species, are where they were.

## Method

`msvc-release` engine-view, `--offscreen`, RTX 5090 (driver as installed on 2026-10-28), i9-10980XE,
each run under the machine-wide GPU lock (`tools/gpu-lock.ps1 run`) with `--wait-quiet 120`, 180
frames along the scene's camera path after the harness's 240-frame warm-up and three repeats; the
proof of concept's arguments (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 0
--time-of-day 15 --no-lights --lod 1`), its scenes read as data, its derived-data cache copied to
scratch. **Before** is this branch's base (fa41167c) built in the same tree, **after** is 5a91b9f2.
The machine was shared with other agents' builds and the owner's sessions; `quiet` is the
harness's judgement, and every row not quiet is an upper bound. GPU milliseconds are medians.

## The grass-only scene, 1600×900

`grass-diagnosis/grass-only.json` along `world/v19/grass-clearing-path.json` (15,704 sward
instances, all untraced; `--rt-budget-mib 1024`).

| Run | wanted / built | overflow | total | rt | resolve | trace | quiet (start → end: CPU others, GPU) |
|---|---|---|---|---|---|---|---|
| before, `hw` + `rt` | 2,308 / 2,308 | 0 | 1.577 | 0.543 | 0.363 | — | yes (6%, 0% → 8%, 10%) |
| before, `rt` + `rt` | 2,308 / 2,308 | 0 | 1.377 | 0.540 | 0.488 | 0.099 | no (4%, 0% → 5%, 99%) |
| after, `hw` + `rt` | 2,308 / 2,308 | 0 | 1.574 | 0.543 | 0.364 | — | yes (4%, 0% → 4%, 0%) |
| after, `rt` + `rt` | **67,934 / 67,934** | 0 | 6.520 | 2.003 | 0.366 | 3.929 | no (8%, 0% → 19%, 100%) |
| after, `rt` + `rt`, policy off | 72,952 / 72,952 | 0 | 7.835 | 2.128 | 1.285 | 4.149 | no (8%, 1% → 25%, 7%) |
| after, `hw` + `rt`, policy off | 72,952 / 72,952 | 0 | 4.082 | 2.121 | 1.290 | — | no (8%, 0% → 10%, 17%) |

The 2,308 are the ground's and the water's; the before rows reproduce the proof of concept's
count in both modes. After, the ray path builds the grass's drawn clusters (65,626 more) and none
of its casters — the policy-off rows' 5,018 more are those casters — and its resolve stays the
grass-free 0.37 ms where the policy-off frame's shadow rays meet the grass for 1.29 ms. The
rasterized frame with traced shadows is unchanged: the same 2,308 structures, and its id capture
**identical to the before build's at every one of the 1,440,000 pixels**.

**The ids where the grass stands** (`--capture-channels ids`, the path's last frame):

| Comparison | id words that differ |
|---|---|
| after `rt` + `rt` against after `rt` + `rt` with the policy off | **0** |
| after `hw` + `rt` against after `hw` + `rt` with the policy off | 0 |
| after `hw` + `rt` against after `rt` + `rt` (policy on) | 91,106 |
| after `hw` + `rt` against after `rt` + `rt` (policy off) | 91,106 |
| before `hw` + `rt` against before `rt` + `rt` | 627,978 |

Of the 627,650 pixels where the before build's ray path named a different instance from the
rasterizer — the grass it did not draw, the ground behind it showing — the after build's ray path
names the rasterizer's instance at 554,394 and the same id word at 539,339; 228 still show the
instance behind. The rest are what they would be for any traced grass: the 91,106 words in which
the ray path and the rasterizer disagree are the same 91,106 with the policy off, the thin
alpha-tested blades' edges, where a ray through a pixel centre and a rasterized coverage test part
ways. So the ray path's picture of the grass is the one it would have drawn had the grass always
been traced, and the shadows are the policy's.

## The v20 forest

`world/v20/base.json` along `world/v20/expanded-forest-path.json`, `--raster rt --shadows rt`
(6,489 untraced sward instances live at the path's end; the forest's trees and rocks traced).

| Run | budget | wanted / built | overflow frames | instances dropped | total | rt | resolve | trace | clas | quiet |
|---|---|---|---|---|---|---|---|---|---|---|
| before, 1600×900 | 1,024 MiB (pinned) | 99,811 / 99,811 | 0 | 0 | 8.823 | 3.580 | 2.917 | 0.927 | 2.046 | no (5%, 0% → 11%, 0%) |
| before, 1600×900 | 2,025 MiB (device) | 99,811 / 99,811 | 0 | 0 | 8.721 | 3.591 | 2.891 | 0.927 | 2.053 | no (5%, 0% → 8%, 78%) |
| after, 1600×900 | 1,024 MiB (pinned) | 100,957 / 100,957 | 0 | 0 | 9.367 | 3.781 | 2.846 | 1.388 | 2.059 | no (9%, 1% → 11%, 100%) |
| after, 1600×900 | 2,025 MiB (device) | 100,957 / 100,957 | 0 | 0 | 9.329 | 3.794 | 2.844 | 1.391 | 2.067 | yes (5%, 0% → 6%, 0%) |
| after, 11520×2160 | 1,024 MiB (pinned) | 247,302 / 169,226 | 1,262 | 904,854 | 60.893 | 5.507 | 40.433 | 11.358 | 3.302 | yes (9%, 0% → 4%, 0%) |
| after, 11520×2160 | 2,025 MiB (device) | **247,302 / 247,302** | 0 | 0 | 63.519 | 6.850 | 40.444 | 12.616 | 4.999 | no (5%, 0% → 11%, 30%) |

At 1600×900 the grass in view adds 1,146 structures, 0.2 ms of `rt` and 0.46 ms of primary tracing
— the trace is the ray path's own pass, now meeting grass it used to pass through — and nothing
of the resolve, whose shadow rays still pass it. **At 11520×2160 no before run was flown here**
(one or two wide runs a night); the proof of concept's own run of this path on 4726acd7 (this base
plus its HUD branch, windowed, 180 frames, `runs/expanded-forest-rt-rt-20261028-181825`) wanted
**245,146** and built 169,226 at the pinned 1,024 MiB, with 40 overflow frames, `rt` 5.50 ms,
resolve 43.7 ms and trace 15.5 ms on a busy machine. After, the frame wants **247,302**: 2,156 more,
the grass the wide view sees. At 1,024 MiB it still drops (the overflow and drop counts here are
summed over the harness's warm-up and three repeats, so they are not the proof of concept's
per-flight 40); **at the device default it builds every one of them, with nothing dropped**, for
1.3 ms more of `rt` (the builds the 1,024 MiB run dropped) and 1.3 ms more of trace. That is the
proof of concept's budget setting to change (R107), not an engine limit; the 40 ms resolve is the
alpha-tested casters' shadow loop that R109 is about.

## The frame budgets' traced runs

`tools/frame-budget.ps1 -Preset msvc-release -WaitQuiet 120 -Run
erg-walk-1080p-rt,erg-walk-surround3-rt,endless-surround3-rt` on 5a91b9f2, all three quiet by the
harness's rule (`endless-surround3-rt` ended with the GPU at 20%). None of the three scenes has an
untraced species, so their records are the bytes they were and every top-level mask the 0xff it
was; the numbers say so:

| Run | total median / p99 (budget) | rt median (budget) | resolve median (budget) | verdict |
|---|---|---|---|---|
| `erg-walk-1080p-rt` | 0.722 / 0.921 (0.724 / 1.214) | 0.366 (0.368) | 0.204 (0.200) | pass |
| `erg-walk-surround3-rt` | 3.466 / 4.016 (3.373 / 4.127) | 0.603 (0.632) | 2.410 (2.318) | pass |
| `endless-surround3-rt` | 4.172 / 5.088 (4.436 / 5.418) | 0.866 (0.951) | 1.997 (1.885) | pass |

The erg surround's resolve sits 4% over its median budget and inside its limit (2.600); the only
change in the resolve is the mask argument of its two shadow queries, an immediate, so it is read as
the run's noise rather than a cost.

## What holds it

The tests are named on the [renderer page](../subsystems/renderer.md#the-caster-policy) and the
[gfx page](../subsystems/gfx.md#the-ray-mask). Each was run against the change with a part taken
out: with the old records filter, the renderer's cube-as-sward draws 0 of its 1,874 pixels on the
ray path (5,615 id words differ, 5,692 deformed) and the reference's camera ray misses all 1,874;
with the light queries' masks back at 0xff, the ray path's ground is shadowed at 1,133 pixels
(1,481 deformed) and the reference's shadowed ground falls to 0.021 from 0.22.
