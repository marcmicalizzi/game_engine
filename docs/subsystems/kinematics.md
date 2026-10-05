# kinematics (systems)

**Purpose.** Motion without forces: an entity with a `world::Transform` and a `kinematics::Velocity` moves by its velocity every tick — a translation, and a turn about its own origin. It is what a thing that moves but is not simulated needs: a cart on a rail, a drifting buoy, a patrol a scheduled event will stop, a prop a script sets going. It is also **the first capability whose state a document authors and the world writes back**: `engine.kinematics.Mover` records materialize into a `Transform` and a `Velocity` through the IDL's `materialize` declaration, and where the mover has got to comes back to the document at `Persist` ([sim](sim.md#the-driver-a-document-through-the-hooks)). It does not collide, accelerate, follow a path or know about anything but the entity's own two components: physics is `domain/physics`', a path is navigation's.

**Why this shape.** It exists because the materialized world needed a system that changes a component a document authored — the end-to-end proof that a record becomes an entity, a system moves it, and the move comes back as a commit attributed to `system` — and motion is the smallest such thing that is also useful on its own. It was built through `tools/new-capability.ps1` and ADR-0027's contract, so it touches no shared module: its components and record type are in its own schema, `Transform` is the world's (`schemas/world.schema`) because every capability that places something reads it, and it attaches through `ecs::register_system` and the schema's registration header alone. Being optional, a build without it (or without the ECS) simply has no `Mover` type, and engine-host's `session.materialize` reports such records as `unknown type`.

**Owned data.** None beyond the two components' fields on each entity that has both; the system keeps a counter (`KinematicsStats::moved`, stage 0's share of the last tick).

**The integration.** `integrate(transform, velocity, seconds)`: position += linear × seconds, affine and **in f64** — `Transform::position` is a `WorldPos` ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)) and the float32 velocity and step widen exactly, so a cart moves as far 10,000 km out as by the origin, where the float32 sum it replaced moved a 1.5 m/s cart 1.875 m a second 420 km out and nothing at all at 240 Hz; orientation takes one explicit first-order step of dq/dt = ½ (0, ω) q and is renormalized — well inside what a thing that is not simulated needs at a 60 Hz step. **An entity that does not turn has its orientation left alone to the bit**, which matters for write-back: write-back compares bytes, so a translating mover writes back its position every flush and never its orientation. `integrate` is also the analytic step over a frozen interval, the same function the tick calls.

**The `Mover` mapping** (`schemas/kinematics.schema`): `Transform.position = position @writeback`, `Transform.orientation = orientation @writeback`, `Velocity.linear = velocity`, `Velocity.angular = spin`, `parent = ChildOf`. A mover is authored in the units a designer types — metres per second, and **degrees per second** for `spin` — and the component holds radians per second: the row converts, from the `@unit`s both sides declare, with no code here. Velocity is the author's and is not written back; where the mover got to is.

**LOD policy: every tier, on purpose.** Its whole cost is two multiply-adds a component and, when it turns, a quaternion product, so there is nothing to coarsen and a band would cost more than it saved. A record at LOD3 is not an entity and is not ticked; bringing it back is `integrate()` over the gap, which meets `domain/sim`'s summarizer contract exactly for the linear part (an affine step is additive over a partition) and to float rounding for the angular part.

**Determinism: hashed.** Per-entity arithmetic over the entity's own two components, no shared accumulator, no wall clock, floating-point contraction off tree-wide (ADR-0035). The engine's scheduler and flecs' own pipeline produce the same transforms bit for bit (tested).

**Invariants.**
- A mover at 1 m/s moves 0.5 m in thirty 60 Hz ticks, and one with no velocity does not move.
- An entity with zero angular velocity keeps its orientation's exact bytes.
- The same world ticked by `ecs::ScheduledTick` and by `SimWorld::step()` ends in byte-identical transforms.
- A `Mover` record materializes with its spin converted from degrees to radians per second and its orientation the identity.

**Public API.** `include/systems/kinematics/kinematics.h`: `k_determinism`, `k_phase`, `integrate`, `KinematicsStats`, `KinematicsSystem` (`install`, `stats`, `descriptor`). Types from `schemas/kinematics.schema`: `Velocity` (component), `Mover` (record) and its mapping.

**Depends on.** `base`, `containers`, `math`, `log`, `ids`, `sim`, `ecs`, `schema`, `schemas`, `kinematics_schemas`.

**Testing.** `tools/dev.ps1 test -Filter kinematics`: the integration (translation, a quarter turn in sixty steps, an untouched orientation, and a second at 60 Hz 419,072 m, 10,000,000 m and 100,000,000 m out ending displaced as by the origin within 60 roundings of the far position, 60 × site × 2^-53), the registration (the descriptor's phase and masks, one system in `Systems`, a moving and a still entity after thirty ticks), sixty-four movers ticked forty-five times by the scheduler and by flecs' pipeline compared byte for byte, and a `Mover` record through the driver and the entity store's hook — its velocity, its spin converted, and a second of ticks moving it three metres. The end-to-end case is engine-host's (`apps/engine_cli/tests/ops_tests.cpp`, [protocol](protocol.md#sessionrun_headless-the-materialized-world)): a cart moving in a yard, written back to the document. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'kinematics.*'`.

**Performance notes.** `kinematics.tick` runs the real tick (flecs' pipeline, one worker) over 10^3, 10^4 and 10^5 movers, every second one turning. On the i9-10980XE, release, 2026-09-24, two runs on a machine that was not quiet (others' CPU 7.7% rising to 21.5% over the first, 13.4–15.3% over the second, GPU idle): **10.6–12.9 µs** at 10^3, **93.8–94.8 µs** at 10^4, **933–941 µs** at 10^5 — 9.3 ns a mover, linear from 10^4 up, the 10^3 row carrying the tick's fixed cost. Most of that is the turning half: a quaternion product and a square root per turning mover per tick. **The branch between the two halves is data-dependent** ([11](../plan/11-performance-principles.md)): the bench alternates movers, which the predictor learns, and a population that mixes turning and still movers at random would pay a mispredict on a good share of them. When a real scene shows it, the fix is data layout, not code — the angular velocity in a component of its own, so still and turning movers are different tables and two branch-free loops.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(kinematics ecs)` | declared; off when ecs is off |
| Component and record types | `systems/kinematics/schemas/kinematics.schema`: `Velocity`, `Mover` and its `materialize` declaration | built |
| Tick scheduler entry | `kinematics.integrate`, `SystemDesc` phase `Systems`, reads `Velocity`, writes `Transform`, registered with `ecs::register_system` | built |
| Render-graph passes | none: it moves transforms, whoever draws reads them | not needed |
| Content-build derived step | none | not needed |
| Protocol methods | none: its records reach the world through materialization | not needed |
| Tunables | none: there is no parameter to tune | not needed |
| LOD policy | every tier (above) | decided |
| Determinism | `kinematics::k_determinism` = `hashed` | tested |
| Zero cost when unused | no linked code (`ENGINE_WITH_KINEMATICS=OFF`) and no instances: a world with no `Velocity` matches no query | built |
| Tests and size table | `tests/kinematics_tests.cpp`, `tests/size_table.cpp` (`Velocity` 24, `Transform` 40: a `worldpos` and a quat) | built |
| Bench | `bench/kinematics_bench.cpp` | built |
| Removal proof | `ENGINE_WITH_KINEMATICS`, off in the minimal build and with the ECS | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_KINEMATICS=OFF`, or `msvc-no-ecs`) drops the module, its tests, its bench and its schema library; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else still builds and passes; engine-host's runtime world then has no `Mover` type and says so.
