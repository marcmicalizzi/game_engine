# Experiment write-ups

The measurements the decisions rest on. [10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) lists the experiments and what each one is meant to settle; a row there that reports **Done** or **Measured** must link a page here, and `tools/docs-check.ps1` fails a push where it does not.

A write-up is not a benchmark log. It is the page a future contributor reads instead of re-running the experiment, so it has to say what was measured, on what, **beside what**, and what the numbers do not cover.

| Experiment | Page | Settles |
|---|---|---|
| E1 | [e1-raster-crossover.md](e1-raster-crossover.md) | hardware against software rasterization of small clusters ([ADR-0024](../adr/0024-hardware-rasterization-first.md)) |
| E1 on Pascal | [e1-pascal-rerun.md](e1-pascal-rerun.md) | the same sweep on a GPU without mesh shaders (TITAN Xp, 2026-09-24): the vertex path against the software rasterizer, occlusion culling and the Hi-Z on the baseline tier, and a same-build control on the RTX 5090 |
| E2 | [e2-cluster-acceleration.md](e2-cluster-acceleration.md) | cluster acceleration structures against one bottom-level structure per cut ([ADR-0025](../adr/0025-cluster-acceleration-structures.md)) |
| E6 | [e6-ecs-store.md](e6-ecs-store.md) | flecs at 10^5 entities and SQLite at 10^6 projection records ([ADR-0028](../adr/0028-ecs-and-persistent-store.md)) |
| E9 | [e9-multi-view.md](e9-multi-view.md) | a three-view surround against a Panini projection at 11520×2160, and what a `ViewSet` costs |
| E10 | [e10-generated-props.md](e10-generated-props.md) | how many generated props the engine takes as they come, and which validators and repairs the rest need (twenty props through Meshy twice, Tripo and TRELLIS.2 from one image set; the collapse check calibrated on the Khronos samples) |
| Flythrough | [flythrough-desert-overlook.md](flythrough-desert-overlook.md) | the first benchmark-corpus scene of [09 §9.4](../plan/09-testing-profiling.md#94-benchmark-scene-corpus) along its 2,401-frame path (RTX 5090 at 1080p to the 11520×2160 surround, TITAN Xp at 1080p and 1440p; Khronos substitutes and the owner's landmarks): what the frame costs per pass, whether occlusion culling pays once there are real occluders, streaming under a budget, and the occlusion invariant over a whole path |
| Content-build determinism | [content-build-determinism.md](content-build-determinism.md) | whether a `.clusters` container is a function of its source or also of the toolchain that built it: eleven sources on MSVC, GCC 13 and 14 and Clang 18 at x86-64-v2 and v3, the stage that diverged (meshoptimizer under FMA contraction), why a per-module flag leaked, and what turning contraction off costs ([ADR-0035](../adr/0035-no-floating-point-contraction.md)) |
| E19 | [e19-lattice-cage.md](e19-lattice-cage.md) | what a lattice cage costs against ADR-0026's per-tick budget |
| E25 | [e25-deformed-clusters.md](e25-deformed-clusters.md) | the deformed-vertex pool, and cluster templates for deforming geometry |

## The skeleton

```markdown
# E<n>: <the question in a phrase>

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** what this decides, and which plan section asked.
- **Date:** YYYY-MM-DD. **Machine:** CPU, cores, RAM, OS; **GPU:** model, driver, API version. **Build:** the preset.
- **Machine state:** what else the machine was doing. See below — this line is mandatory.
- **Decision:** the ADR or the plan status note this produced, and its status.

## Setup
What was built, what was measured, and the exact command lines that reproduce it.

## Results
Tables. Medians, and the spread when it matters.

## What surprised me
The numbers that did not match the plan's guess, and why. This section is the reason to read the page.

## What it decides
And what it explicitly does not.

## Caveats
One machine, one driver, one corpus; what a second data point would likely change.
```

## The "Machine state" line

**Every write-up says what else the machine was doing while its numbers were taken.** This project's development box is shared with GPU diffusion workloads and with several agents compiling and testing at once, so "otherwise idle" is a claim that has to be *checked*, not assumed — and a number taken beside a full GPU or a parallel build is an upper bound on the cost rather than the cost. Attributing a loaded environment's numbers to poor performance is how a budget gets set against a figure nobody can reproduce.

The harness records it (`foundation/bench`, [bench.md](../subsystems/bench.md#measuring-on-a-shared-machine)): a measured run samples the CPU, GPU and session-lock state before and after itself, writes both into the JSON header, and prints one WARNING line when other processes used more than 10% of the CPU or the GPU was more than 20% busy. So the line has one of three honest forms:

- **`Machine state: quiet.`** Measured with `--require-quiet`, or with `--wait-quiet=<n>` reporting a quiet machine, and no WARNING. Say which flag, and say it if you widened a threshold.
- **`Machine state: <the numbers>, WARNING raised.`** Quote the warning. The numbers are upper bounds; ratios measured in the same run survive, comparisons against another day's numbers do not.
- **`Machine state: not recorded at the time.`** For a page written before the harness knew how to look. Say what the machine is normally doing, and re-measure when a decision turns on the number.

A GPU measurement adds the renderer's own `gpu_memory` block (`budget_mib`, `used_mib`, `device_local_total_mib`), which is what says whether another process was holding the card's memory while the frame was timed.
