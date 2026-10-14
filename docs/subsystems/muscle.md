# muscle (systems)

**Purpose.** The line tier of the engine's muscles: a Hill-type **line actuator** on a routed path — activation dynamics, a fiber with active, passive and damping force, and a compliant tendon in series at a fixed pennation height — integrated at the scheduler's 60 Hz tick. A game drives up to 200 of them per character. This page is stage 1 of the capability ([roadmap](../roadmap.md) C53): tier 0 (near characters) as a batch kernel — the law, the scheme, the located slack/taut crossing, the whole-tick commit or rollback, the work ledger, static initialisation, the validator, the schema, the qualification corpus replayed as data, and the bench. Which tier an actuator is in, degrading to a rigid tendon after repeated failure, the inert animation tier, freeze accounting, promotion, the ECS system, document records and the path's source in the skeleton are stage 2 (C54, "Not yet" below). It does not do active volume (the tissue program's), and it owns passive fiber, damping and tendon force exclusively (D20).

**Status.** Landed: the schema, the law (`law.h`), the adopted scheme and its S probe (`scheme.h`), with their tests. Following in C53's next commits: the schedule and locator, the store, static initialisation and the lifecycle; the corpus replay; the validator; the bench.

**Why this shape.** The law and the scheme were not designed here. An authoring track (`astra_muscle`, the `06-muscle-activation` worktree) qualified them offline over ten revisions against a held-control Radau reference, under decisions D20–D23 that the engine owner confirmed, and those decisions are the contract ([ADR-0055](../adr/0055-the-muscle-line-tier-scheme-schedule-and-failure-semantics.md)): the r005 reference law, Alexander's SDIRK3 with the located-crossing locator (adopted at r009), the r010 schedule (four pieces per parent on the first tick, four remainder steps after a crossing), whole-tick rollback, the corpus as the only qualified domain. The engine's work is to carry them exactly and make them cheap and deterministic, not to improve them: a change of scheme, substep count or gate is a new authoring declaration, not an engine edit. Three engine choices sit on top. **Deterministic transcendentals** ([math](math.md), `engine::det`): D20 requires the committed state to be the same bytes on MSVC, GCC and Clang, and the C library's `exp` and `tanh` are not. **The path is a template parameter** of the tick, so a sample is an inlined call, never an indirect one per stage. **A failure is a value, not an exception**: every function that can fail returns `false` with the enumerated `ValidationTest` and the value it read, so a failed tick costs a branch and retains the committed bytes by never having written them.

**Owned data.** The committed line state of every actuator in a store: `(last_commit_tick, activation, fiber_length, startup_pending, the force cache)` with the identities (actuator, parameter set, path, model), and the per-actuator law rows derived from the parameters. Nothing else holds or mutates them.

**Invariants** (each tested; the files are under `systems/muscle/tests/`):
- The law reproduces the reference's forces, energies and residual at its archived initial states to 1e-9 relative (`law_tests.cpp`, the four embedded rows; `corpus_tests.cpp`, all 39).
- The tableau is r009's pinned binary64 Alexander SDIRK3; its rational forms and the order-three conditions hold; the shared stage loop is third order on `y' = -y` with `y + w` conserved, and its stability function is `R(z)` with `R -> 0` in the stiff limit (`scheme_tests.cpp`).
- No `std::` transcendental is named in the kernel's sources (`law_tests.cpp` greps them).

## The law, in the engine's words

The authoring track's r005 law, carried expression for expression (`law.h`). Its canonical text is `systems/muscle/tests/model.txt`, one formula or constant per line; the engine's model identity is that file's SHA-256 (`0238cd30…`), computed once with `Get-FileHash` and pinned as `k_model_sha256`, beside the reference's `3096e5ff…`, the identity of the law it implements.

- **Activation**: `da/dt = (u - a)((s + 1/2)/(tau_act z) + (1/2 - s) z / tau_deact)`, `z = 1/2 + 3a/2`, `s = tanh(10 (u - a))/2`; each stage's activation is a classical RK4 step from the probe's starting activation over `c_i d`, the excitation held. The three RK4 steps share their first slope, which is the same value each time.
- **Fiber**: `x = l / lopt` snapped to an endpoint within 4 ulp of it, else in `[0.5, 1.6]`; three Gaussians for active force-length; `asinh` force-velocity with `w = v / vmax` in `[-1, 1]` (a value in `(-1e-14, 0)` is zero); an exponential passive curve and its energy; damping `F0 beta v / vmax`.
- **Series**: a fixed height `lopt sin(alpha0)`; the belly's projection `sqrt(l^2 - h^2)` with a cosine floor; a tendon that is zero in compression and `0.2 F0 expm1(k strain)` in tension, `k = ln 6 / e1`, with `k strain > 100` a domain error; the residual `(F_a + F_p + F_d) cos - F_t`.
- **The implicit stage**: the fiber velocity whose length `base + gamma d v` zeroes the residual, bracketed by the length domain and the trial-extension cap (`L > Ls (1 + 2 e1)`) and by `[-vmax, vmax]`, the bracket moved one `nextafter` inward, solved by Brent exactly as scipy's `brentq` iterates (`xtol 2e-14`, `rtol 1e-14`, 64 iterations). "Empty bracket", "not bracketed" and "not converged" are failures.
- **The S probe** (`probe_s`): three stages, `base_i = l + d sum_{j<i} a_ij v_j` in increasing `j`, the end state `(t + d, a_3, l_3)`, stage 3's components the cache; the five work channels (`external = F_t dL/dt`, `active_output = -F_a v`, `dissipation = F_d v`, and the two absolute values) gain `(d b_i) P_i` in stage order with the signed weights kept (`b_2 < 0`), nothing clipped.

**Provenance**, file by file, all under `06-muscle-activation` (read as text; nothing there was run): `r005/model.py` (the fiber side), `r005/compliant.py` (the series side and `static_initial_length`), `r005/stepping.py` (`stage`, `sample_path`, `validate_state`), `r009/declarations/d001/ARM-S.md` and `TABLEAU.json` (the scheme), `r009/execution/shared/methods.py` (`stage_loop`, `probe_s`, `locate`), `r008/execution/a001/arm_a.py` (`gap`, `gap_rate`), `r010/declarations/d001/SCHEDULE.md`, `LOCATOR.md`, `BUDGET.md`, `METHOD.json` (the schedule), `r007/study.py` (the corpus's control and path kinds, the ledger), `r007/STATE-CONTRACT.md` and `r008/declarations/d001/muscle.schema` (the schema's proposal), and the decisions D20–D23 with the engine owner's four reviews.

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

**Public API.** `include/systems/muscle/muscle.h` (identities, paths), `law.h`, `scheme.h`.

**Depends on.** `base`, `containers`, `math`, `log`, `ids`, `hash`, `json`, `schema`, `muscle_schemas`.

**Testing.** `tools/dev.ps1 test -Filter muscle`. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'muscle.*'`.

## Not yet

Stage 2 (C54): the supervisor — eight consecutive failures degrade to tier 1 (rigid tendon) with `TierDegraded`, the pending projection at the next tick's start, an invalid projection entering the inert tier 2 with `TierInert`, promotion after 60 rigid successes through `static_initial_length` (D20, D21 (4)); stale forces and the freeze ledger (`W_freeze_ext`, `dE_freeze`, `FreezeAccountingStatus`); the scheduler entry and the ECS system; document records and `materialize`; the path's source in the skeleton (a `HermitePath` per tick from the routed path's endpoints); event delivery; widening the qualified domain with named-muscle calibrations.
