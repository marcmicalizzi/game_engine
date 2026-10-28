# The traced chain at the forest's width (2026-10-27)

Roadmap R107. **Question:** the forest proof of concept's v18 expanded forest at 11520×2160 under
traced shadows wanted 243,604 cluster structures a frame against the 1 GiB budget's 167,093, and
dropped instances' structures on every frame. What are those structures, what does a larger budget
buy, would cluster templates make them cheaper, and what should the default budget be?
**Answer:** they are the **swaying trees** — 98% of the chain is the pool-deformed plants of the
inner two rings and their off-frustum casters, the ground 1.7% — and at any budget that holds them
the frame is **29 ms faster** (90.3 → 61.0 ms), because the 35 ms the `rt` pass spent was the
one-thread drop rule, not the builds. The default budget now follows the device: a sixteenth of its
memory, 512 to 4,096 MiB, which is 2,025 MiB on the RTX 5090 and holds this frame with nothing
dropped. Templates cannot run on this scene (three refusals, below).

## Method

- **Scene and path.** The proof of concept's frozen `reports/quality-v18/runs/expanded-forest-rt-wide-leaf-wide-20261027-201626/scene.json`
  and `world/v18/expanded-forest-path.json`, read where they are; its derived-data cache
  (`world/v18/ddc`, 3.5 GB) **copied into this agent's scratch** and passed as `--ddc`, so nothing
  was written into the proof of concept. The flags are the proof of concept's run's
  (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --no-lights --time-of-day 15 --lod 1
  --deform-pool-mib 64 --tunable renderer.flora.motion=true --tunable renderer.flora.wind_strength=0.7
  --raster hw --shadows rt`) with `--offscreen` instead of its window, `--frames 600 --warmup 0
  --repeat 1`, `--wait-quiet 120`, each run under `tools/gpu-lock.ps1 run`.
- **Build.** `msvc-release` engine-view: dad55f90 for the 1,024 MiB row, this branch (the budget
  rule, otherwise the same code) for the rest; every row but `device` names its budget with
  `--rt-budget-mib`, so the rule is exercised only by that row.
- **The path holds the camera still**: every one of a run's 600 frames wanted the same structures,
  drew the same 2,576,680 pairs and kept the same 33,675 shadow casters, so the medians are of one
  frame drawn 600 times.
- **Machine.** RTX 5090 (32,404 MiB device-local), i9-10980XE, a machine shared with other agents'
  builds and tests; the harness's `machine_state` is quoted per row. Two rows were quiet by its
  rule; the others had other processes on up to 20% of the CPU at their end, and four ended with
  the GPU sampled 99% busy, so by the harness's rule those numbers are **upper bounds**. The GPU
  medians of the quiet `device` row and the busy 1,536 and 2,048 rows agree to 0.2 ms.

## The chain at 1,024, 1,536 and 2,048 MiB

| budget | capacity | wanted | built | overflow frames | dropped instances / casters (events) | `rt` median / p95 | `clas` | `blas` | `resolve` | GPU total median / p95 | chain bytes | machine state (start → end) |
|---|---:|---:|---:|---:|---|---|---:|---:|---:|---|---:|---|
| 1,024 MiB | 167,093 | 243,604 | 167,093 | 623 (every frame) | 206,836 / 6,853 | **43.55** / 44.99 | 3.47 | 1.32 | 31.62 | **90.29** / 93.52 | 1.77 GB | others 9.3 → 7.7% of the CPU, GPU 1 → 11%; quiet |
| 1,536 MiB | 250,640 | 243,604 | 243,604 | 0 | 0 / 0 | **8.16** / 8.83 | 5.51 | 0.85 | 37.46 | **61.03** / 61.99 | 2.31 GB | 7.6 → 14.9%, GPU 2 → 3% |
| 2,048 MiB | 334,186 | 243,604 | 243,604 | 0 | 0 / 0 | 8.10 / 8.68 | 5.50 | 0.84 | 37.52 | 61.01 / 62.64 | 2.85 GB | 3.1 → 19.8%, GPU 2 → 99% |
| **device rule, 2,025 MiB** | 330,433 | 243,604 | 243,604 | 0 | 0 / 0 | 8.06 / 8.69 | 5.39 | 0.85 | 37.71 | 61.15 / 62.47 | 2.83 GB | 6.1 → 2.8%, GPU 2 → 2%; quiet |
| 2,048 MiB, `--rt-templates` | 334,186 | 243,604 | 243,604 | 0 | 0 / 0 | 8.09 / 8.72 | 5.48 | 0.84 | 37.50 | 60.91 / 62.19 | 2.85 GB | 3.9 → 19.0%, GPU 4 → 99% |

`overflow frames` counts the run's frames and the ones a flight folds after it. The chain's bytes
are `rt_bytes()`: the per-frame capacity plus everything beside the budget (the static forms'
325 MB built once, the per-instance and scene-sized buffers).

**What the 35 ms was.** The chain's own builds are the smaller part at every budget: `clas` goes
*up* from 3.47 to 5.5 ms when 76,511 more structures are built, `blas` down from 1.32 to 0.85.
The 35 ms that disappears is `ranges_main`'s drop rule, which runs on one thread over every instance
whenever a frame wants more than its capacity ([renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory):
"a frame inside its capacity never takes it"), the same 34 ms R101's page found on v14. A frame that
fits never runs it. The resolve rises by 6 ms with the structures, because the rays meet the
leaves the dropped instances had taken out of the structure. **So the budget is worth 29 ms of this
frame** — more than any pass but the resolve — and the drop rule's cost, not the builds', is what
made the overflow expensive: past the budget it costs about 35 ms whatever is dropped. Parallelising
it is R106's (its owner tonight); this page only asks for the order the drops already have.

## What the 243,604 are

Measured on the 2,048 MiB configuration from three runs: the census (`--census`, 60 frames: the
visible list read back between frames and counted per mesh), `--no-shadow-casters` (the chain
without its caster run), and `--tunable renderer.flora.motion=false` (every plant rigid). The
deformed-vertex pool's own count (`Stats::deform_entries`, read with an instrument print that was not
committed) ties them together, because every pool entry is a deformed visible entry:

| part | structures | share | how it is known |
|---|---:|---:|---|
| swaying plants' drawn clusters (rings 0 and 1: full source meshes, pool-deformed, so never cone-tested, never a static form, never cached) | 223,795 | 91.9% | the census's 225,937 drawn pairs of the scene's own meshes, less the grass's 2,142 (pool drawn entries 229,972 − the chain's drawn 227,830; grass is untraced) |
| their off-frustum shadow casters | 15,741 | 6.5% | the pool's caster entries (245,740 − 229,972), less the ground's |
| the ground's drawn clusters (the world's tiles and far levels: the pool's terrain stage) | 4,035 | 1.7% | visible pairs the census does not attribute to a scene mesh; equal to the pool's drawn entries with the plants rigid, less their casters |
| the ground's casters | 27 | — | the pool's 4,062 entries with the plants rigid, less its drawn |
| rigid casters | 6 | — | `--no-shadow-casters` removes 15,774, the pool's caster entries are 15,768 |
| **total** | **243,604** | | `rt.wanted` |

Not in the chain at all: the rings 2 and 3, **2,346,694 drawn pairs of static forms** (49,436
instances of 171 rigid distance forms, R94's structures built once, 325 MB), and the 2,142 grass
pairs. The cull keeps 33,675 shadow casters a frame, but the static forms' are in their once-built
structures and only the deformed instances' 15,768 (and 6 rigid) are built per frame.

**This corrects R101's page**, which put the v14 expanded forest's 222,455 down to "its ground —
the world's tiles and far levels". On v18 the ground is 4,062 structures; what the chain builds is
the swaying trees, which is also what the context sheet's "the near trees sway" said. (The proof of concept's windowed report shows a `flora` block of one instance and no posed
frames, which makes the trees easy to miss; the offscreen flight's block counts 2,285 swaying
instances, 2,284 of them streamed, posed every frame.)

**With the plants rigid** (`renderer.flora.motion=false`): 171,918 structures, `rt` 6.42 ms,
`clas` 3.92, the frame 56.15 ms (others 6.8 → 16.4% of the CPU, GPU 3 → 99%). A rigid tree is
cone-tested, so its backfacing clusters become casters or nothing, and the inner rings' full meshes
are still built per frame (they are not forms, and the cluster cache is off by default since R101's
merge).

## The budget: a sixteenth of the device

The rule (`renderer::rt_device_budget_mib`, `resolve_rt_budget`): `RenderSettings::rt_budget_mib`
if set, else the `renderer.rt.budget_mib` tunable if not zero (its default is now 0), else **a
sixteenth of the device-local heaps, never under 512 MiB nor over 4,096**. On the cards this project
runs:

| card | device-local | budget | clusters at the RTX 5090's 6,426 B |
|---|---:|---:|---:|
| RTX 5090 (the owner's desktop) | 32,404 MiB | **2,025 MiB** | 330,433 |
| a 24 GB card | 24,576 MiB | 1,536 MiB | 250,640 |
| a 16 GB card | 16,384 MiB | 1,024 MiB (the old default) | 167,093 |
| TITAN X (Maxwell), TITAN Xp (the runners) | 12 GB | 768 MiB | — no cluster structures: the chain never exists there |
| RTX 4070 laptop | 8 GB | 512 MiB | — |
| a 6 GB card | 6 GB | 512 MiB (the floor) | — |

Why these numbers:

- **A sixteenth** is the smallest power-of-two share that holds this frame on the card it runs on,
  with the policy's headroom: 243,604 structures grow the capacity to 307,200 (25% over, rounded to
  4,096) = 1.84 GiB, which a sixteenth of the 5090 (2,025 MiB) holds and a thirty-second (1,012)
  does not. A larger share costs the scenes past the budget: a scene that wants more than its
  budget holds the whole budget from its first frame (`first_rt_capacity`), and the shrink rule
  never takes it back while the frames want more than half of it — this forest holds 330,433
  clusters (2.03 GiB of capacity) where its frames would have grown it to 307,200. An eighth would
  hold 4 GiB on the 5090 for the same frame.
- **The heaps' size, not `VK_EXT_memory_budget`'s budget.** The budget the driver reports moves
  with what other processes hold (on this shared machine, by gigabytes between runs), so a rule on
  it would give the same card a different capacity, and different drops, from run to run. The heaps
  are what the card is.
- **The floor, 512 MiB**, is the desert overlook's first capacity (65,536 clusters, 400 MB on the
  5090) with room: below it a small card would start every past-the-budget scene below the first
  capacity the policy was tuned on. It is still a twelfth of a 6 GB card, against the sixth the old
  default was — so **a small card's ceiling went down**, which is the direction a card that small
  wants.
- **The ceiling, 4,096 MiB**, is about 670,000 structures a frame at the 5090's cost: past that the builds' time
  (about 22 ns a structure in `clas` here) and not their memory is the limit.
- The budget is still a ceiling, never an allocation: `RtCapacity`'s headroom, growth and shrink
  rules are unchanged, and a scene that wants less allocates less. The desert overlook's chain
  (134 MB) and the endless desert's (a few thousand clusters) are sized by their frames as before.
- Explicit budgets are not clamped by the rule: `--rt-budget-mib 1` and `--rt-budget-mib 8192` are
  what the caller asked for.

## Cluster templates on the forest: they cannot run here

`--rt-templates` on this scene is **ignored** with `cluster templates ignored with world tiles`, and
the row above is the rebuild's to the byte in every number. Three refusals stand between the forest
and templates, and the first that fires is the one logged:

1. **World tiles** (`resolve_settings`): the ground's tiles are meshes laid out in slots the camera
   rewrites, and a template is built once per mesh cluster at load from a rest pose a slot does not
   have.
2. **A scene whose instances come and go** (the streamed tail): templates are built from the load's
   clusters and a tail's blocks are not laid out with instantiation records.
3. **Alpha-tested materials** (`GpuScene`: templates only where the scene has no masked material):
   an instantiated cluster takes its flags from its template, which is built opaque, so a leaf's
   mask would never be tested by a shadow ray. And with templates there are **no shadow casters**
   (`ResolvedSettings::casters` is off), because an instantiation cannot be marked non-opaque per
   frame; here that is the 15,768 casters.

What it would take, and what it could buy. The swaying trees are exactly the case templates were
made for (E25: a deforming mesh's clusters instantiated from the pool, 3× to 5× cheaper than a
rebuild, byte-identical pictures). Here the rebuild's `clas` is 5.4–5.5 ms for 243,604 structures;
at E25's ratio the 223,795 drawn plant clusters would cost about 1.0–1.7 ms instead, a saving of
3.5–4.5 ms of a 61 ms frame (6–7%), and the 15,741 casters would still be rebuilt. The costs:
templates for every cluster of the swaying meshes, packed at E25's 493–537 bytes a cluster (the
scene's meshes hold 581,900 clusters, of which the forms' are not needed; on the order of 100–300 MB),
built once at load; a mixed mode in `clas_records.slang` (templates for the plants, records for the
ground's slots and for casters), instantiation records for the tail's blocks as they arrive, and a
non-opaque template build for masked materials. That is `clas_records.slang`'s and the tail
layout's work (R106's area tonight), not R107's, and it was not tried. No default changes.

## The deformed-vertex pool's floor

`pool_budget_vertices` takes the largest of the setting (`--deform-pool-mib 64` here),
`renderer.terrain.pool_mib` (768 MiB) when the ground streams, and `renderer.flora.pool_mib`
(96 MiB) when plants sway, then clamps it down to the whole meshes over the views and cascades.
Under `--shadows rt` on this scene:

| | entries | vertices used (peak) | bytes used | pool allocated |
|---|---:|---:|---:|---:|
| the plants swaying (the run above) | 245,740 | 14,507,487 | 174.1 MB | 805.3 MB (67,108,864 vertices: the 768 MiB floor) |
| the plants rigid: the terrain stage alone | 4,062 | 192,118 | **2.3 MB** | 248.3 MB (the ground's whole meshes; the floor clamped down) |

**The terrain stage itself uses 2.3 MB of a 768 MiB floor** on this scene — a three-hundredth of
it — and the desert's budget scenes used at most 11,491 clusters of at most 64 vertices (8.8 MB) when
the floor was last measured (R84). On a 12 GB card the floor is 6.25% of the card held for that.
The pool is otherwise sized by what the frames want, so the recommendation is to **lower
`renderer.terrain.pool_mib`'s floor to the size the terrain's cut needs** (64 MiB holds seven times
the largest terrain use measured) **and let the scene's other tenants set their own** — the plants'
96 MiB floor and the explicit setting — rather than carry a ground floor ninety times the largest
ground use measured. That is not changed here: `pool_budget_vertices` is not R107's, and the cascaded frame's
overflow (1.59 million entries refused under `--shadows csm`) is the cascades' and the shadows
agent's tonight. Under the cascades the plants' demand is what fills any floor; on the 12 GB
runners, which draw the maps and have no chain, that is the number to size against.

## What holds it

- `rt capacity: the default budget is a sixteenth of the device, 512 to 4096 MiB` (CPU): the rule on
  the card classes above, the floor and the ceiling, whole MiB rounded down, and the precedence of
  the setting, the tunable and the device.
- `rt capacity: a scene with no budget named takes the device's, and says so` (GPU): a scene's
  `rt_budget_mib()`/`rt_budget_source()`, its capacity limit in clusters and `Stats::rt`'s
  `budget_mib`/`budget_source` with nothing named, with a setting, and with the tunable.
- The flythrough summary's `rt` block (`FlythroughRt` version 5: `budget_mib`, `budget_source`),
  engine-view's windowed summary and `engine.budgets` (`source: "device"` at the process, each
  scene's own budget and source) say which rule a run took.
