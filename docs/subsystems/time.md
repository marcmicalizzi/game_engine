# time (core)

**Purpose.** The engine's time model (docs/plan/02-architecture.md §2.4, ADR-0010, ADR-0017): a monotonic wall clock for measurement, 64-bit `SimTick` and integer `GameTime` for everything the simulation reasons about, the fixed-step accumulator that decouples the sim from rendering, and the game clock that advances game time by whole ticks.

**Owned data.** None global; `FixedStepClock` and `GameClock` are owned by whoever runs the loop.

**Invariants.**
- `SimTick` and `GameTime` are integers; no floating point accumulates over a session. Floats appear only in `interpolation_alpha()` and the time-scale multiplication of a single frame's delta.
- `FixedStepClock::step()` yields exactly one step per `step_ns` of (scaled) real time; the backlog is capped at `max_steps_per_advance` steps and the excess is counted in `dropped_steps()`, never simulated later.
- `GameClock` advances by an integer number of microseconds per tick (`us_per_tick`, rounded once from the rate), so the same tick sequence produces the same `GameTime` on every machine. Fast-forward is an explicit `jump_by`/`jump_to`.
- `time::monotonic_ns()` never decreases.

**Public API.** `core/time/time.h`: `time::monotonic_ns`, `wall_unix_ms`, `wall_unix_us`, `Stopwatch`; `SimTick`; `GameTime` (constructors from us/ms/seconds/minutes/hours/days, accessors, arithmetic); `FixedStepClock`; `GameClock`.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter time`.

**Performance notes.** Trivial. The fixed-step loop is the shape every system's `update` will be called from; the scheduler's timing wheel (plan 05 §5.3) keys on `GameTime`.
