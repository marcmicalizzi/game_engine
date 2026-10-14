# What the muscle line tier costs (2026-10-13)

The line tier's stage-1 kernel ([muscle](../subsystems/muscle.md), [ADR-0055](../adr/0055-the-muscle-line-tier-scheme-schedule-and-failure-semantics.md)) measured as time, which no authoring-side count could give. D20 names **2 µs per actuator per tick** for tier 0; the owner's estimate of this scheme's cost — eight parents, three implicit stages each, Alexander SDIRK3 under the located-crossing schedule — was 10 to 50 µs. The deliverable is the measured number and where it goes, not a met target: neither the scheme nor the substep count changed to hit anything.

## Setup

`systems/muscle/bench/muscle_bench.cpp`, `build/msvc-release` (x86-64-v3, `RelWithDebInfo`, Tracy zones compiled in but none in the kernel), the harness's default seven repeats, `--require-quiet` (it ran: the machine was quiet). Two corpus cases replicated 64 and 256 times on the runtime's path, a cubic Hermite per tick from the analytic path's length and rate at the tick's two ends (`HermitePath`):

- **step**: the default parameters, excitation 0 then 0.8 from 0.05 s to 0.25 s, a constant taut path, no crossing.
- **passive**: `corner_04_passive`, the stiffest tendon (e1 = 0.02) with heavy damping (0.2), no excitation, a sine path that goes slack and taut again: one located crossing every six ticks.

Every actuator is initialised at its static equilibrium and runs the startup tick (four pieces a parent) outside the timing; an iteration restores that state (untimed) and runs ticks 2 to 61, the 60 ticks after it; an item is one actuator's tick. Each benchmark also runs those ticks once untimed with the kernel's counters and prints what a tick spends.

**Machine state**: i9-10980XE (36 threads), RTX 5090; at the start of the run other processes used 8.1% of the CPU and the GPU 0% (4.5 GB of 32 GB in use), at the end 6.2% and 0%; no GPU lock held, session unlocked.

## Results

Nanoseconds per actuator per tick at the median (the median iteration over 3,840 items at n = 64 and 15,360 at n = 256; spread under 0.4%):

| Case | Stage root | n = 64 | n = 256 | Residuals a tick (all roots) | over stages | Root iterations over stages |
|---|---|---|---|---|---|---|
| step | Brent (the reference's iteration) | 51.7 µs | 51.7 µs | 256.2 | 10.68 | 9.63 |
| step | **safeguarded Newton (adopted)** | **41.6 µs** | **41.6 µs** | 199.0 | 8.29 | 6.25 |
| passive | Brent | 60.6 µs | 60.6 µs | 323.4 | 8.98 | 7.95 |
| passive | **safeguarded Newton (adopted)** | **46.7 µs** | **46.5 µs** | 234.6 | 6.52 | 4.49 |

The counts a tick, the same for both roots: **step** 8 probes, 24 implicit stages, 80 activation right-hand sides (each an RK4's three slopes per stage plus the shared first), 1 equilibrium solve (the tick's `validate_state`), no crossing; **passive** 12 probes, 36 stages, 120 activation right-hand sides, 1 equilibrium solve, 0.167 crossings and 3.3 bisection probes a tick (a crossing costs 20 bisections and 4 remainder steps in its parent). One S probe from the step case's static start costs 6.3 µs (Brent).

**Where it goes.** The time is the residual. A residual evaluation is six transcendentals — three `exp` in the active force-length Gaussians, one `exp` in the passive curve, an `expm1` in a taut tendon, a `log1p` or `log` inside the force-velocity `asinh` — plus two square roots (the belly's projection and the `asinh`'s), and an activation right-hand side is one more (`tanh`, through `expm1`). The step case's 256 residuals and 80 rates are about 1,620 transcendental calls an actuator a tick; at the cost of a libm-class `exp` (fdlibm's kernels are a reduction and a degree-5 to degree-7 polynomial, a few tens of cycles) that is most of the 52 µs, though the transcendentals' own share was not measured separately here. The rest is the residual's arithmetic, the root's bookkeeping and the schedule. Per residual, the whole tick costs about 200 ns with Brent and 209 ns with Newton (whose slope is arithmetic on the residual's own exponentials: no transcendental more).

**The two roots against the gates.** Under the Newton root every corpus case's three metrics equal Brent's to the four digits the replay prints (35 of 39 pass all three gates either way, the same four labelled step rows within 1.5 times r009, the two first-sample rows passing), and both final-state hashes are pinned in `corpus_tests.cpp` and reproduced by GCC and Clang. Newton takes 20% off the step case and 23% off the passive one, from fewer residual evaluations: 6.3 iterations a stage against Brent's 9.6, 4.5 against 8.0. It is the kernel's root (`k_stage_root`); Brent stays in the module as the reference's own iteration, and the test runs both.

## What it decides

The adopted kernel costs **42 to 47 µs per actuator per tick** on this machine, 21 to 23 times D20's 2 µs and inside the owner's 10–50 µs estimate, near its top. At 200 actuators a character that is 8.3 to 9.3 ms of one core per 60 Hz tick: a near character's muscles at tier 0 take half a frame of a core. The first concrete reductions, in the order they would pay:

1. **Four lanes across actuators** (AVX2, 4 × f64). The kernel was written so that it can: no early exit a lane mask cannot express (a failure ends one actuator's trial), the path a template, the schedule fixed. The residual's transcendentals vectorise as they stand — `engine::det`'s functions are branch-light polynomial kernels on the argument's bits — and the iteration counts of neighbouring actuators are close (a root's iterations vary by a few between lanes, and a masked lane idles for them). How much it buys is the next measurement, not a promise: four lanes are the ceiling, and masking and the gathers of per-actuator rows take from it.
2. **Fewer transcendentals a residual.** Along a stage's root the fiber length moves by `gamma d v`, micrometres, while the Gaussians and the passive exponential are evaluated afresh at every iterate: a Newton iterate could carry the length terms' derivative instead of re-evaluating them (the force-velocity term alone depends on `v` at fixed length), and the equilibrium solve's residual already does exactly that. The four length-dependent transcendentals of six are the target.
3. **A better Newton start and stop.** The iteration starts at the bracket's secant point and stops on a step under Brent's tolerance (`(2e-14 + 1e-14 |v|)/2`), which costs a confirming iteration after quadratic convergence. Starting from the previous stage's velocity and stopping on the residual's own rounding level would take one or two iterations of 4.5 to 6.3.
4. **The tick's `validate_state`** (one equilibrium root a tick, about a dozen residuals, counted in the table's residuals) is the reference's check and stays; it is about 5% of the step case.

None of these changes the law, the tableau, the schedule or a gate, so each is the engine's to make under D23 (5)'s rule: the corpus gates hold and the bytes agree across the three compilers, which the pinned hashes check.

## Caveats

- One machine, one compiler (MSVC); GCC and Clang were run for their bytes, not their time. A Linux GCC release build at v3 is often faster on scalar floating-point code; it was not measured here.
- The two cases are replicated, not mixed: every lane of a future 4-wide kernel would take the same branch here, which flatters it.
- The Hermite path through the tick's endpoints is not the corpus's analytic path; the passive case's crossings land at slightly different times, which does not change the per-tick counts by more than the crossing's own share.

The raw output: `muscle_bench.exe --require-quiet --json=...`, the four `muscle.tick.*` rows and `muscle.probe`, with the harness's JSON header holding the machine state quoted above.
