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

## The controllers in f64

The second half of the change (2026-10-05, walker-2): `apps/engine_view`'s fly camera and walker hold `WorldPos` and integrate each 240 Hz tick's float32 displacement in f64 ([apps](../subsystems/apps.md#far-from-the-origin)), and `systems/scene_collision` reads a scene's instances from their cells. These are correctness measurements, not timings: MSVC debug, and the same bits on any compiler that keeps IEEE arithmetic unfused (ADR-0035). Each test compares a run from a far site with the same run from the origin, positions taken from the site (`apps/engine_view/tests/far_tests.cpp`, `systems/scene_collision/tests/scene_collision_tests.cpp`).

| What | 419 km | 10,000 km | 1e8 m | Held to |
|---|---|---|---|---|
| Two seconds of flight (keys, pointer, fast, slow, lift), at its end | 44 pm | 81 nm | 0.50 µm | 1 µm (and every tick's angles the same bits) |
| The same flight across a 64 m cell's corner at 419 km | 44 pm | | | 1 µm |
| Two and a half seconds walked (a turn, a sprint, a jump, a strafe, back), at its end | 0.21 nm | 0.48 µm | 0.55 µm | 1 µm |
| The same walk, the worst tick | 0.21 nm | 7.5 µm (tick 543) | 7.3 µm (tick 53) | 1 µm at 419 km, 20 µm beyond |
| `walk.max_ground_error_m` over that walk (the collision's 1 m heightfield against a 25 cm tile lattice of the same source) | 0.748 mm | 0.748 mm | 0.748 mm | under 5 mm, within 0.1 mm of the origin's (0.748 mm) |
| A second's walk along x at 1.5 m/s | | 1.49876 m | | 1.5 m to 5 mm, the origin's to 1 µm (the same 1.49876 m) |
| A box of a scene read whole: its west face, by a ray | 0 | 0 | 0 | 1 µm |

The walk rows are the physical walker's (`msvc-debug`). The ground follower's, the same walk in `msvc-minimal` where physics is off: at its end 36 pm, 0.11 µm and 0.74 µm; the worst tick 36 pm, 0.26 µm and 1.5 µm; its ground against the drawn lattice 0.069 mm at every site; the x walk exactly 1.5 m at both sites; and the owner's sprint 0.231804 m along x and 0.557928 m along z, his heading at 5 m/s to the micrometre.

The flight's f64 sums round at the coordinate's own step (58 pm, 1.9 nm, 15 nm), so 480 ticks could at the very worst add to 14 nm, 0.45 µm and 3.6 µm; what was measured is under that. The walk's worst ticks at the two farther sites come back within half a micrometre by its end: that is the character backend's own double-precision stepping, which its own far walk above measured at 0.88 µm and 6.6 µm over ten seconds; it does not grow with the controller.

**The owner's sprint.** His frame log of 2026-10-04 (`endless-2026-10-04T1710-frames.jsonl`, frames 5256 to 5266: walking at (−419,055.125, 81.7, −66,781.67), sprinting at yaw −2.7478, 22° off +z towards +x, for 0.1224 s) shows x at −419,055.125 on every frame and z advancing 0.461 m — 3.77 m/s of a sprint whose z share is 4.62: the float32 sum rounded each tick's 8 mm along x away and each 19 mm along z to two of z's 7.8 mm steps. The walker put at that pose on the test's swell and sprinting at that yaw for the 29 ticks the frames span now moves **0.2317 m along x and 0.5580 m along z** (5 m/s along his heading would be 0.2318 and 0.5579). The brief's "0.19 m" is the x his frames should have shown for the z they did show (0.461 × tan 22.6°); at full speed it is 0.232.

**An old session.** The committed synthetic session as version 1 wrote it (two seconds over the procedural heightfield, 22 m from the origin) replays under version 2 and ends **31.5 µm** from where version 1 ended it (`fly_tests.cpp`); by the origin version 1's float32 sums rounded at 1.9 µm, and 480 of them moved it that far. Far out it moves by what version 1 lost, which is the point of the change.

**What changed in the hashes.** The committed synthetic trajectory moved from `81f482098316f782` (version 1) to `b245b4d4b3e6dcdd` (version 2): positions in f64 and hashed as doubles. Every walk's hash changes too: the ground follower's feet are hashed as doubles, and the physical walker is dropped at the ray's hit plus 2 cm in f64 where that sum was float32, so its character starts a few nanometres elsewhere.
