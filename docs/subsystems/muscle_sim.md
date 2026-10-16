# muscle_sim (systems)

**Purpose.** The line tier's engine wiring ([roadmap](../roadmap.md) C54, stage 2 of the muscle line tier): `engine.muscle.LineActuator` document records materialize into actuators that one scheduler entry ticks through [muscle](muscle.md)'s supervisor every tick, each routed through its character's posed skeleton, with each actuator's force, tier and stale flag written into a `LineOutput` component and the tick's validation events into a buffer the rest of the engine reads. It owns the world's `muscle::LineStore` and the per-actuator routing; it does not apply the force to anything (no consumer exists yet: a ragdoll's physics will read it), drive the excitation (`LineExcitation` is a property until a controller writes it), or write anything back to the document.

**Why this shape.** **A second capability, not more of `systems/muscle`**: the kernel and the supervisor are a batch API over their own store with no ECS, so their corpus and supervisor tests run in the no-ECS build (`linux-clang-no-ecs`); everything here needs the entity store, the scheduler or a skeleton, and is switched off with them through the capability graph. **The records and components are declared in `systems/muscle/schemas/muscle.schema`** and registered here ("declared in one module, registered by another", [schemas](../../schemas/README.md)): the record carries `FiberParameters` and `SeriesParameters`, and a second capability's schema cannot import the first's (imports resolve against the shared `schemas/` directory and the listed files). With this capability off the mapping is still registered and the driver skips its records, naming the components no world registered. **The store is the capability's, not the ECS's**, as animation's pose pool is: the committed line state is SoA integration state the supervisor reads and writes in place, and an entity holds only its slot (`LineOutput::slot`). **The path is routed in model space**: an entity's world transform is rigid (`world::Transform` has no scale), so the polyline through the joints' model-space points is the world-space length, and no world coordinate is formed ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)). **The phase is `Physics`**, the one after the phase kinematics and animation run in: the path reads the pose `animation.sample_poses` writes in `Systems`, a phase boundary is the order the scheduler guarantees (under `ScheduledTick` each phase is its own pipeline run), where two capabilities' systems in one phase would be ordered by registration, and the force is a physics input, computed in the phase physics runs in before `PostPhysics` reads it.

**Owned data.** The world's `muscle::LineStore` (every actuator's committed line state, supervisory record and per-tick slots), each slot's route (up to `k_max_route_points` = 8 joint-and-offset points, resolved once by bone name), its path history (the routed length and rate at the last tick boundary), the record-to-slot map, the refusals, and the last tick's event buffer. The `LineOutput` component's values are written only here.

## Records and materialization

`LineActuator` (`@kind(record)`): `name`, `parameter_sha256` (64 hex digits naming a parameter set the validator's qualified domain holds), `fiber` and `series` (the values that identity was qualified with), `origin` and `insertion` (a `LineAttachment`: a bone by name and an offset in its frame, metres) and `via` (fixed via points between them, in order), and `excitation` (in [0, 1]). Its document parent is the character whose skeleton the attachments name, and the mapping makes it the entity's `ChildOf`. It materializes at LOD0 and LOD1 only (`@tiers(0, 1)`) into `LineActuatorSpec` (the authored half) and `LineExcitation`.

**The validator gates it.** `MuscleSimSystem::hooks()` is a materialization row registered after the entity store's (`ecs::RecordMaterializer`), which makes the entity and writes the authored components; this row reads `LineActuatorSpec`, runs `muscle::validate` (model, parameter-set identity, the law's domain), checks the route's point count and the store's capacity, and only then gives the record a store slot and the `LineOutput` that makes it an actuator. A refused record is a **document-level validation failure**: it is listed in `refusals()` with the validator's code, field, value, unit and rule (or this capability's reason), logged, and never ticks. Its entity exists — the hooks table cannot veto the entity store's hook, every row runs and the first to give a record an existence names it ([sim](sim.md), "Which name each hook carries") — but it holds only the authored components and matches no system. An update of a record this world holds keeps the actuator's integration state when its law (identity and parameters) is unchanged and resolves its route again; another parameter set starts it over in the same slot; an update the validator refuses releases it. Dematerialization releases the slot to a LIFO free list, so slot assignment is a function of the materialization sequence.

**Nothing is written back.** The committed line state is runtime integration state, not an authored property: `save(record, LineState&, PathHistory*)` and `restore` move it, with the path's history, in and out of a world (a restore in another world ticks the same bytes, tested), and a host that saves a world saves those.

## The tick

`muscle_sim.tick`, one `sim::SystemDesc` registered with `ecs::register_system` in `TickPhase::Physics`, reads `LineActuatorSpec` and `LineExcitation`, writes `LineOutput`, declares the pose source's resources (`animation.pose_pool`), and is **not** multi-threaded: one pass in slot order is the tick.

1. **Gather**: every actuator's excitation, character (its `ChildOf` parent) and output row into its slot, resolving a route not yet resolved by bone name through the pose source.
2. **Route**: per slot in order, the routed length at the tick's end from the character's model-space joint transforms, composed once for consecutive actuators of one character (a character's muscles materialize together, so they hold consecutive slots).
3. **Start**: an actuator with no static start yet gets one, as of the tick before, at the length routed now and the activation its held excitation settles at (`initialize_static`, which arms the startup schedule); one with no route or no pose yet waits and outputs nothing.
4. **Path**: the tick's `HermitePath` from the path history at the last boundary to the length at this one (below).
5. **Batch**: every ready actuator through `muscle::supervise_actuator` in slot order.
6. **Outputs**: `LineOutput` (`tendon_force` — the committed cache's, stale when `forces_stale`, zero when `force_output` is false — `tier`, `path_length`, `failed_test`) and the tick's events copied into the buffer `events()` returns.

**Events.** Nothing in the engine delivers simulation events yet (the only other `@kind(event)` type, `WriteBack`, goes to the store's log). The tick copies every actuator's `ValidationEvent`s out of the store's slots into a buffer sized once (capacity × 4, so none is ever dropped); a consumer reads `events()` between ticks or from a later phase, and each tick replaces it. `TickWork` stays in the store's per-actuator slots (`store().work`).

## The path

`path.h`: `route_length(model, route)` is the polyline through the route's points, each a joint's model-space transform applied to an offset, in f64 from the f32 matrices. `next_path(history, length)` is the tick's Hermite from `(L_{k-1}, rate_{k-1})` to `(L_k, (L_k - L_{k-1})/dt)`: the rate at a boundary is the backward difference over the tick that ends there, first order, lagging the true rate by `(dt/2) L''`. The first tick, and the first after a length that is not a number, has no history and is constant. Measured on a synthetic two-bone skeleton bent 0.4 rad at 1 Hz (`path_tests.cpp`): the routed length matches the closed form to 8.5e-10 m (bound 1e-7 m, an f32 ulp at 0.3 m); against the kernel on the analytic path over 2 s, the Hermite's mid-tick length is within 4.6e-5 m on the first tick (which starts from rate zero against a true 0.021 m/s) and 6.9e-7 m after (bounds 5e-5 and 2e-6 m); the fiber lengths agree to 2e-6 m and the tendon forces to 1.14 N, which is the rate lag through force-velocity's slope at zero velocity — 0.2% of the 600 N active force at `a = 0.6`. **A velocity-aware rate** (the joints' angular velocities through the route's Jacobian, which animation does not publish) is the improvement when that matters.

`animation_pose_source(AnimationSystem&)` is the posed skeleton of [animation](animation.md): a character's `SkeletonInstance`, its skeleton's joint names from the clip library (`anim::Skeleton::find`, once per attachment), and the pose pool's local transforms composed into model space by `anim::local_to_model` — what `AnimationSystem::joint_model_transform` composes, once per character a tick rather than once per joint. A `PoseSource` is two function pointers and a resource mask, so a test (and a game with its own rig) can route through anything that poses joints.

## Invariants (each tested)

- One system, in `Physics`, reading the spec and the excitation and writing the output; four `LineActuator` records under two `Node` characters make six entities, of which three are actuators and the one with an identity outside the qualified domain is refused with `domain_unqualified`, holds its authored components and has no `LineOutput`; the via point and the excitation arrive in the components (`muscle_sim_tests.cpp`).
- Sixty ticks through the engine's scheduler: the two healthy actuators output force from their routed, moving paths and emit no event; the stretched one fails eight ticks after its bone lengthens at tick 20, `TierDegraded` arrives at tick 27 and `TierChanged` at 28, and its `LineOutput` says `Rigid` with force; the bytes are the same at one worker and at four (`muscle_sim_tests.cpp`).
- A saved actuator restored with its path history into another world ticks the same bytes for thirty ticks (`muscle_sim_tests.cpp`).
- The routed length matches the closed form; the tick's Hermite runs from the last boundary's length and rate and restarts after a non-number; the kernel on it follows the kernel on the analytic path within the bounds above (fiber 1e-5 m, force 1.5 N); animation's pose source names joints by name and gives the pool's pose composed into model space, bit for bit what the library's clip at the playhead gives (`path_tests.cpp`).

**Public API.** `include/systems/muscle_sim/muscle_sim.h` (`MuscleSimSystem`, `MuscleSimConfig`, `MuscleSimStats`, `RecordRefusal`, `PoseSource`, `animation_pose_source`, `k_phase`, `k_determinism`, `k_lod_tiers`), `path.h` (`RoutePoint`, `route_length`, `PathHistory`, `next_path`, `k_max_route_points`). The components come from `systems/muscle/schemas/muscle.schema` and arrive as `<schemas/muscle.h>`.

**Depends on.** `base`, `containers`, `math`, `log`, `ids`, `sim`, `ecs`, `schema`, `schemas`, `muscle`, `muscle_schemas`, `animation`, `anim`.

**Testing.** `tools/dev.ps1 test -Filter muscle_sim`.

**Performance notes.** No allocation in the tick: every per-actuator array (the store's, the routes, the histories, the outputs' pointers, the event buffer) and the model-transform scratch are sized at construction from `MuscleSimConfig`, and materialization past the capacity is refused with that said. The tick's cost is the supervisor's per actuator ([muscle](muscle.md), "Performance") plus one skeleton composition per character and one polyline per actuator. The gather walks the entity store's tables once and writes into slots, which is what makes the order — and so the bytes — independent of table layout and worker count.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(muscle_sim muscle)`, `(... ecs)`, `(... animation)` | declared; off when any is off (the no-ECS build switches it off) |
| Component and event types | `LineActuator`, `LineActuatorSpec`, `LineExcitation`, `LineOutput` and the mapping in `systems/muscle/schemas/muscle.schema` | registered here |
| Tick scheduler entry | `muscle_sim.tick`, `TickPhase::Physics` | registered |
| Materialization hooks | `MuscleSimSystem::hooks()`, after the entity store's | the validator's gate |
| Render-graph passes | none | a muscle draws nothing |
| Content-build derived step | none | |
| Protocol methods | none | records reach the world through materialization |
| Tunables | none | the schedule is fixed by D20–D23; the capacity is a config struct |
| LOD policy | `k_lod_tiers` = LOD0 and LOD1, the mapping's `@tiers(0, 1)`: a farther character's muscles are not entities | works; the supervisor's own tiers are failure-driven |
| Determinism | `muscle_sim::k_determinism` = `hashed` | one worker and four give the same bytes |
| Zero cost when unused | no linked code (`ENGINE_WITH_MUSCLE_SIM=OFF`) and no instances: an empty store's tick walks nothing | works |
| Tests and size table | `tests/muscle_sim_tests.cpp`, `tests/path_tests.cpp`, `tests/size_table.cpp` | |
| Bench | none of its own | the tick's cost is the supervisor's, `muscle`'s bench |
| Removal proof | `ENGINE_WITH_MUSCLE_SIM`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_MUSCLE_SIM=OFF`) drops the module and its tests; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`.
