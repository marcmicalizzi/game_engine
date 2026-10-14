# muscle (systems)

**Purpose.** The line tier of the engine's muscles: a Hill-type **line actuator** on a routed path — activation dynamics, a fiber with active, passive and damping force, and a compliant tendon in series at a fixed pennation height — integrated at the scheduler's 60 Hz tick. A game drives up to 200 of them per character. This page is stage 1 of the capability ([roadmap](../roadmap.md) C53): tier 0 (near characters) as a batch kernel — the law, the scheme, the located slack/taut crossing, the whole-tick commit or rollback, the work ledger, static initialisation, the validator, the schema, the qualification corpus replayed as data, and the bench. Which tier an actuator is in, degrading to a rigid tendon after repeated failure, the inert animation tier, freeze accounting, promotion, the ECS system, document records and the path's source in the skeleton are stage 2 (C54, "Not yet" below). It does not do active volume (the tissue program's), and it owns passive fiber, damping and tendon force exclusively (D20).

**Status.** Landed: the schema, the law (`law.h`), the adopted scheme and its S probe (`scheme.h`), the schedule and the locator (`schedule.h`), the store, the tick, static initialisation, save and restore (`store.h`), the corpus replay (`content/muscle/`), with their tests. Following in C53's next commits: the validator; the bench.

**Why this shape.** The law and the scheme were not designed here. An authoring track (`astra_muscle`, the `06-muscle-activation` worktree) qualified them offline over ten revisions against a held-control Radau reference, under decisions D20–D23 that the engine owner confirmed, and those decisions are the contract ([ADR-0055](../adr/0055-the-muscle-line-tier-scheme-schedule-and-failure-semantics.md)): the r005 reference law, Alexander's SDIRK3 with the located-crossing locator (adopted at r009), the r010 schedule (four pieces per parent on the first tick, four remainder steps after a crossing), whole-tick rollback, the corpus as the only qualified domain. The engine's work is to carry them exactly and make them cheap and deterministic, not to improve them: a change of scheme, substep count or gate is a new authoring declaration, not an engine edit. Three engine choices sit on top. **Deterministic transcendentals** ([math](math.md), `engine::det`): D20 requires the committed state to be the same bytes on MSVC, GCC and Clang, and the C library's `exp` and `tanh` are not. **The path is a template parameter** of the tick, so a sample is an inlined call, never an indirect one per stage. **A failure is a value, not an exception**: every function that can fail returns `false` with the enumerated `ValidationTest` and the value it read, so a failed tick costs a branch and retains the committed bytes by never having written them.

**Owned data.** The committed line state of every actuator in a store: `(last_commit_tick, activation, fiber_length, startup_pending, the force cache)` with the identities (actuator, parameter set, path, model), and the per-actuator law rows derived from the parameters. Nothing else holds or mutates them.

**Invariants** (each tested; the files are under `systems/muscle/tests/`):
- The law reproduces the reference's forces, energies and residual at its archived initial states to 1e-9 relative (`law_tests.cpp`, the four embedded rows; `corpus_tests.cpp`, all 39).
- Every corpus case passes the D20 gates its class requires, a batch of all 39 equals each alone byte for byte, and the final states' hash is the pinned one; every corpus number round-trips through `core/json` (`corpus_tests.cpp`, "The corpus" below).
- The tableau is r009's pinned binary64 Alexander SDIRK3; its rational forms and the order-three conditions hold; the shared stage loop is third order on `y' = -y` with `y + w` conserved, and its stability function is `R(z)` with `R -> 0` in the stiff limit (`scheme_tests.cpp`).
- No `std::` transcendental is named in the kernel's sources (`law_tests.cpp` greps them).
- The schedule is fixed: 32 probes on a startup tick and 8 after, whatever the probe returns; parents restart at their canonical times (`schedule_tests.cpp`).
- The locator, on scalar and scripted fixtures: an exact endpoint zero is one crossing and one boundary query; same-phase ends with an opposite internal stage are `EventUnresolved`, a zero internal stage or a zero outgoing rate `EventTangentUnresolved`; the gap bound refuses a 100 m/s crossing the width bound accepts; a non-dyadic crossing in startup piece 2 costs exactly 28 probes; a second crossing in each of the four remainder steps and in each later startup piece is `EventBudgetExceeded` with no second search (`schedule_tests.cpp`).
- Static initialisation reproduces the archived static starts within 1e-9 m; a late invalid path fails the whole tick with the committed record unchanged, zero work and the test named, and the next tick retries from it; `startup_pending` survives save, the schema's JSON and restore, is cleared only by the first committed tick and re-armed only by a static reinitialisation; a record of another law or parameter set is refused (`lifecycle_tests.cpp`).

## The law, in the engine's words

The authoring track's r005 law, carried expression for expression (`law.h`). Its canonical text is `systems/muscle/tests/model.txt`, one formula or constant per line; the engine's model identity is that file's SHA-256 (`0238cd30…`), computed once with `Get-FileHash` and pinned as `k_model_sha256`, beside the reference's `3096e5ff…`, the identity of the law it implements.

- **Activation**: `da/dt = (u - a)((s + 1/2)/(tau_act z) + (1/2 - s) z / tau_deact)`, `z = 1/2 + 3a/2`, `s = tanh(10 (u - a))/2`; each stage's activation is a classical RK4 step from the probe's starting activation over `c_i d`, the excitation held. The three RK4 steps share their first slope, which is the same value each time.
- **Fiber**: `x = l / lopt` snapped to an endpoint within 4 ulp of it, else in `[0.5, 1.6]`; three Gaussians for active force-length; `asinh` force-velocity with `w = v / vmax` in `[-1, 1]` (a value in `(-1e-14, 0)` is zero); an exponential passive curve and its energy; damping `F0 beta v / vmax`.
- **Series**: a fixed height `lopt sin(alpha0)`; the belly's projection `sqrt(l^2 - h^2)` with a cosine floor; a tendon that is zero in compression and `0.2 F0 expm1(k strain)` in tension, `k = ln 6 / e1`, with `k strain > 100` a domain error; the residual `(F_a + F_p + F_d) cos - F_t`.
- **The implicit stage**: the fiber velocity whose length `base + gamma d v` zeroes the residual, bracketed by the length domain and the trial-extension cap (`L > Ls (1 + 2 e1)`) and by `[-vmax, vmax]`, the bracket moved one `nextafter` inward, solved by Brent exactly as scipy's `brentq` iterates (`xtol 2e-14`, `rtol 1e-14`, 64 iterations). "Empty bracket", "not bracketed" and "not converged" are failures.
- **The S probe** (`probe_s`): three stages, `base_i = l + d sum_{j<i} a_ij v_j` in increasing `j`, the end state `(t + d, a_3, l_3)`, stage 3's components the cache; the five work channels (`external = F_t dL/dt`, `active_output = -F_a v`, `dissipation = F_d v`, and the two absolute values) gain `(d b_i) P_i` in stage order with the signed weights kept (`b_2 < 0`), nothing clipped.

**Provenance**, file by file, all under `06-muscle-activation` (read as text; nothing there was run): `r005/model.py` (the fiber side), `r005/compliant.py` (the series side and `static_initial_length`), `r005/stepping.py` (`stage`, `sample_path`, `validate_state`), `r009/declarations/d001/ARM-S.md` and `TABLEAU.json` (the scheme), `r009/execution/shared/methods.py` (`stage_loop`, `probe_s`, `locate`), `r008/execution/a001/arm_a.py` (`gap`, `gap_rate`), `r010/declarations/d001/SCHEDULE.md`, `LOCATOR.md`, `BUDGET.md`, `METHOD.json` (the schedule), `r007/study.py` (the corpus's control and path kinds, the ledger), `r007/STATE-CONTRACT.md` and `r008/declarations/d001/muscle.schema` (the schema's proposal), and the decisions D20–D23 with the engine owner's four reviews.

## The schedule and the located crossing

`schedule.h`, r010's `SCHEDULE.md` and `LOCATOR.md` in the engine's words. Tick `k` advances `[(k-1)/60, k/60]` through eight **parents** of `H = 1/480 s` (`0x1.1111111111111p-9`). Parent `j` starts at its canonical time `((k-1) + j/8)/60` with the state carried from the parent before: the time is reset, never accumulated across parents. While `startup_pending` is set, a parent is four **pieces** of `h = H/4` (`0x1.1111111111111p-11`); otherwise one piece of `h = H`. A piece is one S probe from its incoming state; within a parent, pieces and remainder steps carry their propagated times.

After each probe the gap `g = L - projected(l) - Ls` is read at the start, both internal stages and the end. The start's phase is the sign of its gap, or at an exact zero the sign of the outgoing gap rate `dL/dt - v_eq / cos` (the equilibrium velocity's root), cached by the complete state; at most two such queries per parent, a zero or nonfinite rate `EventTangentUnresolved`, a third query `EventBudgetExceeded` before it is evaluated (r010's rule). An end of the start's phase with an internal stage of the other is `EventUnresolved`, a zero internal stage `EventTangentUnresolved`; an end of the other phase or an exact zero is a detected crossing. A detected crossing is located by 20 dyadic bisections of `tau` on the piece's **original-start** one-step map — the low side never advanced, the high side's probe kept, every candidate inspected — and must end with width `<= h/2^20` and `|g_high| <= 1e-8 m`, else `EventLocationUnresolved` (at 1 m/s the bracket leaves 2e-9 m; at 100 m/s 1.3e-7 m, refused: the two bounds agree up to about 5 m/s, and a faster path is a declared failure). The crossing's remainder `h - tau`, if positive, is **four equal S steps** of `r/4` from the event's time, each inspected as a remainder: any crossing or zero there is `EventBudgetExceeded`. **One crossing per parent**: a later piece of the same parent runs one full probe and is inspected the same way, so a second crossing is refused before any search. The ceilings fall out — 28 probes in a startup parent, 25 otherwise, 224 and 200 a tick — and an assertion holds the schedule to them. The located `tau` is a root of the one-step map, not the exact continuous crossing (the owner's r008 review, note (a)).

The schedule is a template over a *prober* — `validate`, `probe`, `gap`, `gap_rate` — so the tests drive it with fixtures whose crossings are known (a gap that is a function of time, or a script keyed by probe index for a second crossing at an exact remainder position) and the kernel with the law (`LineProber`), with no indirect call either way. The tick begins with the reference's `validate_state`: a finite time, the activation in range, the geometry, the path sample, and a committed state that admits the continuous model's equilibrium velocity.

## Commit or roll back, whole tick

A tick is a trial on locals. Success installs activation, fiber length, the cache (the last segment's final stage), `last_commit_tick = k` and `startup_pending = false`, and reports the tick's five work channels. Any failure anywhere — a domain test, a root not bracketed or not converged, an event code, `NotInitialized`, `TickOrder` — retains the committed record bit for bit, reports zero work and the enumerated test with the value it read, and the next tick retries from the retained state with its new inputs. There is no retry inside a tick, no clamp and no fabricated state (D20). Two places where the engine reports differently from the reference without changing what fails: a location failure's diagnostic gap rate, which the reference evaluated for its event record before raising, is not evaluated (it could replace the location code with a root failure of its own); and a tick at or before the last committed one, which the reference raised outside its rollback, is a structured `TickOrder` failure.

## The store and the tick

`store.h`: a structure-of-arrays `LineStore`, sized once by `reserve(capacity)` and never again (`add` refuses past it). The hot row a tick reads per actuator is the `LawRow` (28 doubles derived once from the parameters), with the activation, fiber length, last committed tick, startup marker and cache beside it; identities and stage 2's supervisory fields are cold arrays; each actuator has fixed slots for its last tick's outcome and counters. `tick<Path>(store, k, excitation, paths, samples)` runs every actuator; nothing in it allocates — the stage states, probe records (240 bytes each), the tick's work and the parent samples live on the stack. No early exit cannot be expressed as a mask: an actuator's failure ends that actuator's trial, which a later 4-wide pass across actuators would carry as a lane mask. `static_initial_length` is the reference's (the bracket and cap rule, `nextafter`, Brent with `xtol = rtol = 1e-14`, the restoring-slope check at ±1e-7 m) and `initialize_static` is the only way to arm `startup_pending`; `save` and `restore` move the committed record (`LineState`) in and out, keeping the marker as saved and refusing a record of another law or another parameter set.

## The corpus

[`content/muscle/`](../../content/muscle/README.md) holds the 39 trajectories the authoring track qualified against, written by `tools/muscle-corpus.ps1` from r010's `CASES.json` and r007's handoff `corpus.json` with every number copied as its source token (the script re-derives the 117 per-row hashes the sources carry and stops on any difference), and the labels from r009 S's summary. `corpus_tests.cpp` replays every case from its **archived** static start (the engine's own `static_initial_length` agreeing with it within 1e-9 m first, and the law's outputs there within 1e-9 relative), `startup_pending` set, for its 36 ticks (24 for `shortening` and `lengthening`, which run 0.4 s), the excitation sampled at each tick's start and held, the path the corpus's constant, linear or sine (`AnalyticPath`, through `engine::det`). The gates are D20's against the held oracle at t = 0 and every parent endpoint: max |F_t - oracle| <= `force_N`, max |l - oracle| <= `fiber_m`, and the final |R| <= `work_J` = 1e-3 D with `R = W_ext + W_active_output - (E_end - E_0) - W_diss` and `E` the passive fiber plus tendon energy. An unlabelled case must pass all three; a labelled case the two it is labelled as passing, with its failing metric at most 1.5 times r009 S's number. The test also runs all 39 in one store and requires each actuator's final bytes to equal its run alone, and pins a 64-bit hash (`core/hash`) of every case's final activation, fiber length and five work channels, taken on MSVC.

The result under the r010 schedule (2026-10-13, the hash the same on MSVC, GCC and Clang; N, mm, and the residual over its limit):

| Case | max \|F - oracle\| / limit | max \|l - oracle\| / limit | \|R\| / limit | events | class | result |
|---|---|---|---|---|---|---|
| step | 0.6640 / 7.9368 | 0.00240 / 0.5 | 0.6670 | 0 | qualified | pass |
| ramp | 0.3391 / 7.9151 | 0.00198 / 0.5 | 0.0514 | 0 | qualified | pass |
| shortening | 0.0025 / 6.1022 | 0.00000 / 0.5 | 0.0001 | 0 | qualified | pass |
| lengthening | 0.0027 / 6.4668 | 0.00000 / 0.5 | 0.0001 | 0 | qualified | pass |
| slack_cycles | 0.2432 / 5.0000 | 0.00149 / 0.5 | 0.1670 | 6 | qualified | pass |
| offset_pulse | 0.6640 / 7.9340 | 0.00240 / 0.5 | 0.6673 | 0 | qualified | pass |
| active_wave | 0.1138 / 7.2460 | 0.00034 / 0.5 | 0.0065 | 0 | qualified | pass |
| corner_00_step | 2.7566 / 8.1525 | 0.00452 / 0.5 | **2.8983** | 0 | known limit, work | within 1.5x of r009 (2.898) |
| corner_00_passive | 0.0552 / 5.0000 | 0.00014 / 0.5 | 0.0936 | 0 | qualified | pass |
| corner_01_step | 0.7728 / 8.0357 | 0.00607 / 0.5 | 0.4591 | 0 | qualified | pass |
| corner_01_passive | 0.0332 / 5.0000 | 0.00034 / 0.5 | 0.0723 | 0 | qualified | pass |
| corner_02_step | 2.7302 / 7.5012 | 0.00420 / 0.5 | **3.1471** | 0 | known limit, work | within 1.5x of r009 (3.147) |
| corner_02_passive | 0.0491 / 5.0000 | 0.00011 / 0.5 | 0.0881 | 0 | qualified | pass |
| corner_03_step | 0.7496 / 7.3778 | 0.00544 / 0.5 | 0.5432 | 0 | qualified | pass |
| corner_03_passive | 0.0309 / 5.0000 | 0.00028 / 0.5 | 0.0700 | 0 | qualified | pass |
| corner_04_step | 1.1636 / 8.1525 | 0.00153 / 0.5 | **1.6519** | 0 | known limit, work | within 1.5x of r009 (1.652) |
| corner_04_passive | 0.4952 / 5.0000 | 0.00107 / 0.5 | 0.2360 | 6 | known limit, force | **passes all three** |
| corner_05_step | 0.8371 / 8.0357 | 0.00731 / 0.5 | 0.2478 | 0 | qualified | pass |
| corner_05_passive | 0.2654 / 5.0000 | 0.00239 / 0.5 | 0.2443 | 6 | qualified | pass |
| corner_06_step | 1.2311 / 7.5012 | 0.00153 / 0.5 | **1.8463** | 0 | known limit, work | within 1.5x of r009 (1.846) |
| corner_06_passive | 0.2345 / 5.0000 | 0.00048 / 0.5 | 0.2577 | 6 | known limit, force | **passes all three** |
| corner_07_step | 0.7416 / 7.3777 | 0.00595 / 0.5 | 0.2908 | 0 | qualified | pass |
| corner_07_passive | 0.2694 / 5.0000 | 0.00226 / 0.5 | 0.2020 | 6 | qualified | pass |
| corner_08_step | 0.7282 / 7.9859 | 0.00736 / 0.5 | 0.3225 | 0 | qualified | pass |
| corner_08_passive | 0.0289 / 5.0000 | 0.00037 / 0.5 | 0.0672 | 0 | qualified | pass |
| corner_09_step | 0.6632 / 7.0018 | 0.03112 / 0.5 | 0.0367 | 0 | qualified | pass |
| corner_09_passive | 0.0078 / 5.0000 | 0.00039 / 0.5 | 0.0360 | 0 | qualified | pass |
| corner_10_step | 0.6785 / 7.3287 | 0.00633 / 0.5 | 0.3863 | 0 | qualified | pass |
| corner_10_passive | 0.0272 / 5.0000 | 0.00031 / 0.5 | 0.0655 | 0 | qualified | pass |
| corner_11_step | 0.6361 / 6.4064 | 0.02743 / 0.5 | 0.0458 | 0 | qualified | pass |
| corner_11_passive | 0.0079 / 5.0000 | 0.00037 / 0.5 | 0.0360 | 0 | qualified | pass |
| corner_12_step | 0.8199 / 7.9858 | 0.00919 / 0.5 | 0.1772 | 0 | qualified | pass |
| corner_12_passive | 0.1503 / 5.0000 | 0.00172 / 0.5 | 0.2318 | 6 | qualified | pass |
| corner_13_step | 0.3510 / 6.9984 | 0.01681 / 0.5 | 0.0200 | 0 | qualified | pass |
| corner_13_passive | 0.0043 / 5.0000 | 0.00019 / 0.5 | 0.0681 | 6 | qualified | pass |
| corner_14_step | 0.7649 / 7.3286 | 0.00788 / 0.5 | 0.2103 | 0 | qualified | pass |
| corner_14_passive | 0.1700 / 5.0000 | 0.00181 / 0.5 | 0.2010 | 6 | qualified | pass |
| corner_15_step | 0.3648 / 6.4051 | 0.01628 / 0.5 | 0.0256 | 0 | qualified | pass |
| corner_15_passive | 0.0053 / 5.0000 | 0.00022 / 0.5 | 0.0748 | 6 | qualified | pass |

35 of 39 pass all three gates, with no failed tick and 54 located crossings, every one committed. The four step rows reproduce r009 S's work ratios to four digits and stay order-limited, as D23 expected: their transient is the control step's, which the startup schedule does not touch. **The two first-sample rows pass**: r010's first-tick schedule takes the force error at t = 1/480 s from r009 S's 5.95 and 5.43 N to 0.056 and 0.038 N, and their largest error over the run is now 0.50 and 0.23 N against the 5 N floor. The labels stay as r009 left them until the owner's review of r010's own result changes them (D23 (4)); the test prints a labelled case that passes everything and accepts it.

## The schema

`systems/muscle/schemas/muscle.schema`, namespace `engine.muscle`: the r008 proposal with D21 (5) and the r010 review applied — the units `N`, `J`, `W` (added to schemac's table for this, [schemas](../../schemas/README.md)) so the name suffixes drop; `failed_test` an enumerated `ValidationTest` (every domain, root, event and budget test the law can fail) instead of a string; `FreezeAccountingStatus.Inert = 3`; `TickWork.transition_elastic_jump` the tick's sum; and `LineState.startup_pending`. Every struct is version 1: this is the engine's first registration, and the proposal's version 2 numbered review rounds. The supervisory fields (`tier`, `pending_transition`, the streaks, the freeze fields) are declared so the committed record is complete from the first commit, and are written by stage 2. No `materialize` yet.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stage 1 is a batch kernel with no ECS |
| Component and event types | `systems/muscle/schemas/muscle.schema` | registered |
| Tick scheduler entry | none in stage 1 | stage 2 (C54) |
| Render-graph passes | none | a muscle draws nothing |
| Content-build derived step | none | the corpus is test data |
| Protocol methods | none | not needed |
| Tunables | none | the schedule is fixed by D20–D23, never tuned at run time |
| LOD policy | tier 0 only (`k_lod_tiers`) | the tiers are the supervisor's, stage 2 |
| Determinism | `muscle::k_determinism` = `hashed` | the committed state is integration state the next tick reads; `engine::det` makes it the same bytes everywhere |
| Zero cost when unused | nothing outside the module names it, and an empty store runs no loop | works |
| Tests and size table | `tests/*_tests.cpp`, `tests/size_table.cpp` | |
| Bench | `bench/muscle_bench.cpp` | |
| Removal proof | `ENGINE_WITH_MUSCLE`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_MUSCLE=OFF`) drops the module, its schema library, its tests and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`.

**Public API.** `include/systems/muscle/muscle.h` (identities, paths), `law.h`, `scheme.h`, `schedule.h`, `store.h`.

**Depends on.** `base`, `containers`, `math`, `log`, `ids`, `hash`, `json`, `schema`, `muscle_schemas`.

**Testing.** `tools/dev.ps1 test -Filter muscle`. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'muscle.*'`.

## Not yet

Stage 2 (C54): the supervisor — eight consecutive failures degrade to tier 1 (rigid tendon) with `TierDegraded`, the pending projection at the next tick's start, an invalid projection entering the inert tier 2 with `TierInert`, promotion after 60 rigid successes through `static_initial_length` (D20, D21 (4)); stale forces and the freeze ledger (`W_freeze_ext`, `dE_freeze`, `FreezeAccountingStatus`); the scheduler entry and the ECS system; document records and `materialize`; the path's source in the skeleton (a `HermitePath` per tick from the routed path's endpoints); event delivery; widening the qualified domain with named-muscle calibrations.
