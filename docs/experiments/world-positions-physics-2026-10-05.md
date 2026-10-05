# World positions in physics: what double precision costs, and what far tests hold (2026-10-05)

[ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md) decision 4 put the physics backend, Jolt 5.6, in its double-precision mode (`JPH_DOUBLE_PRECISION`), and made a body's position, a query's origin and a character's feet a `WorldPos` in `domain/physics`'s public surface ([physics](../subsystems/physics.md#far-from-the-origin)). The ADR says to revisit if double precision costs more than a tenth of a step on the reference scenes. This page has that number, what the far tests hold to, and one thing the ADR did not expect: **being far costs more than being double does**, and it grows with the distance.

## How it was measured

- `engine_physics_bench` in `msvc-release` (RelWithDebInfo, `/Ob2` on Jolt, x86-64-v3, cross-platform determinism on), on the owner's shared desktop.
- **Before** is main at `30c4ad70` (single precision), exported and built beside the change (`build/b` in the worktree); **after** is the change. The two executables were run interleaved, before then after for each group of rows, and the whole sequence twice, so a drift in the machine's load hits both sides alike. A third run of the after side added the far-site sweep.
- **Machine state.** Every run used `--wait-quiet` and started on a quiet machine after waiting (others' CPU 4.7–9.5% at the start, the GPU lock free). Most ended with the harness's warning — others' CPU 8–17% by the end, and in several runs another agent's GPU test holding the GPU lock (GPU 0–10% busy) — so **the absolute figures are upper bounds**; the before/after ratios, taken minutes apart under the same conditions, are what this page rests on. Spreads (median against minimum over the repeats) were under 2% except where noted.
- Median microseconds or milliseconds per step; each "before" and "after" cell is the mean of the runs' medians.

## By the origin: what double precision costs

| Row | Before (f32) | After (f64) | Change |
|---|---|---|---|
| 1,000 boxes, 1 worker | 2.212 ms | 2.370 ms (3 runs) | **+7.1%** |
| 1,000 boxes, 4 workers | 1.123 ms | 1.178 ms (3 runs) | +4.9% |
| 1,000 boxes, 8 workers | 786 µs | 782 µs (3 runs) | −0.5% (noise) |
| 1,000 boxes, no job system | 2.260 ms | 2.601 ms (3 runs) | **+15.1%** |
| 512-particle soft cube, 1 worker, 4 / 8 iterations | 558 / 967 µs | 562 / 975 µs | +0.7% / +0.8% |
| 8 soft cubes, 8 workers | 1.299 ms | 1.295 ms | −0.3% |
| E19 default cage tick (216 elements, 8 iterations), 1 / 8 workers | 475.6 / 734.6 µs | 473.2 / 742.3 µs | −0.5% / +1.0% |
| Character step (240 Hz, heightfield and 200 blocks) | 5.63 µs | 5.67 µs (3 runs) | +0.6% |

The rigid-body pile pays for double precision, the soft bodies and the character do not measurably. The pile's cost is the solver's position updates and the contact manifolds' base offsets in double; the soft bodies keep their particles in floats relative to the body, and the character's sweep is floats relative to its feet.

**The no-job-system pile is the one row past a tenth (+15%)**, and it is not the configuration a game runs: a world without a job system is a tool's or a test's. Why it pays twice what the one-worker row pays was not found; both rounds agree to a point. The one-worker figure, +7%, is the one a single-threaded game would see, and at four and eight workers the cost falls to 5% and nothing.

## Far from the origin: being far costs more than being double

The same scenes moved whole to the ADR's sites (each on x and z; the bench's `physics.step.boxes_1000_far` and `physics.character.step_far`), after the change:

| Row | Origin | 419 km | 10,000 km | 1e8 m |
|---|---|---|---|---|
| 1,000 boxes, 1 worker | 2.370 ms | 2.353 ms | 5.077 ms (3 runs) | 34.5 ms (spread 11%) |
| 1,000 boxes, 8 workers | 782 µs | 732 µs | 1.437 ms (3 runs) | — |
| Character step | 5.67 µs | 5.98 µs | 8.80 µs (3 runs) | 39.3 µs |

**At the owner's 419 km nothing changes** (the pile within noise, the character +5%). **At 10,000 km a step costs twice what it costs by the origin, and at 1e8 m seven to fifteen times.** The likely cause is the backend's broadphase, which keeps every body's bounds as float32 boxes in the world even in double mode and rounds them outwards to a float's step: 3.1 cm at 419 km, 1 m at 10,000 km, 8 m at 1e8 m. A pile of metre boxes a few centimetres apart then has every box's bounds overlapping its neighbours' neighbours, and every such pair goes to the narrow phase to find no contact; the character's sweep likewise gathers every block within a few metres. That cause is inferred from the backend's design and the way the cost grows with the step; it was not confirmed by counting pairs. The results do not change — the far tests below pass at 1e8 m — only the work.

## What decides the ADR's revisit clause

- **By the origin**, double precision costs at most **7% of a step** on the reference scenes as a game runs them (one to eight workers), and nothing measurable on the soft bodies, the cage and the character. Under the ADR's tenth: no revisit on that account.
- **Far from the origin**, the cost is the distance's, not the precision's: none at 419 km, **2× at 10,000 km and 7–15× at 1e8 m**. That is past the ADR's tenth for a world played thousands of kilometres out, and it is exactly the case the clause names: **islands with local origins** — a body's broadphase bounds kept relative to an island's origin, so that they round at the island's size and not the world's — under the same `WorldPos` types. Nothing in the engine walks there today; the endless desert's owner walked at 420 km, where the measurement says it costs nothing.

## What the far tests hold to

Each runs the same scene by the origin and moved whole to 419,072 m, 10,000 km and 1e8 m, every authored offset a whole 1024th of a metre so the site plus it is exact in f64, and compares positions from the site. f64 is not the same bits under translation: a sum rounds at the size of its result, 58 pm at 419 km, 1.9 nm at 10,000 km and 15 nm at 1e8 m. Measured on MSVC debug, 2026-10-05; each bound is the measurement with a margin, and a float32 build misses each by its whole step (3.1 cm, 1 m, 8 m).

| Test | 419 km | 10,000 km | 1e8 m | Held to |
|---|---|---|---|---|
| A ray onto a sloped heightfield: fraction, normal (`query_tests.cpp`) | same bits | same bits | same bits | equal |
| The ray's point, from the site | 0 | 0 | 0 | one f64 step at the site per axis |
| A sphere cast onto it: fraction; point | same bits; 0 | same bits; 0 | same bits; 0 | equal; two f64 steps |
| A sphere dropped on it, 150 steps rolling down | 0.14 nm | 0.31 µm | 3.8 µm | 0.1 mm |
| The hundred-box stack, 600 steps to sleep (`rigid_body_tests.cpp`) | 2.1 µm | 2.1 µm | 2.1 µm | 0.1 mm; rotations to 1e-4 of a unit dot; the same sleeping count |
| The scripted walk, 2,400 steps at 240 Hz (`character_tests.cpp`) | 78 pm | 0.88 µm | 6.6 µm | 0.1 mm; the same 963 steps in the air |
| The collision ground on and between lattice points (`scene_collision_tests.cpp`) | same bits | same bits | same bits | equal |
| A ray onto the collision's heightfield | 0 | 0 | 0 | 1 µm |
| A walk of ten seconds across a tile's edge on the collision ground | 0 | 1.4 µm | 85 µm | 1 mm |
| The dunes' tile source and point entry against the field (`far_tests.cpp`, 25 cm and 1 mm lattices) | same bits | same bits | same bits | equal |

The stack's 2.1 µm is the same at all three sites: the solver's floats meet a different last bit somewhere in 600 steps of a hundred contacts, and the stack carries it, whatever the distance. The walks grow with f64's step. The collision walk at 1e8 m (85 µm) is the largest: a capsule stepping up and down a field of 15° facets carries the rounding through the backend's step-up and stick-to-floor decisions.

## What changed in the hashes

The character's pinned walk hash moved from `4efdb83d1aa9a5c6` to `6b70c6f467ae6192` (MSVC): the backend's arithmetic is double now and the feet are hashed as three doubles. Every recorded walk's character hash changes with it.
