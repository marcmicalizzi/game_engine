# ecs (domain)

**Purpose.** The runtime entity store ([03 §3.4](../plan/03-data-model.md#34-the-runtime-world)) and the tick that drives it ([05 §5.2](../plan/05-simulation.md#52-sim-scheduler)). The store is [flecs](https://github.com/SanderMertens/flecs) 4.1 (MIT), an archetype ECS with first-class relationships, a query language, reflection, and an explorer. It is **exposed directly and not wrapped**, and the engine owns **five narrow seams** through which everything that makes this engine what it is passes. Evaluated by experiment **E6** ([results](../experiments/e6-ecs-store.md)); decided by [ADR-0028](../adr/0028-ecs-and-persistent-store.md).

## The shape of the module, in one paragraph

Write flecs for the things flecs is good at: **system bodies and queries**. A system iterates `Position` as a plain C++ struct through a plain `flecs::query`, uses relationships, wildcards, `cascade()`, staging and the query DSL, and nothing stands in the way. Go through the engine for the five things that are the engine's: **where a component type comes from**, **how a system declares itself**, **what an entity is called across a save**, **how something that is not a system changes the world**, and **who is allowed to see flecs at all**. Everything else is flecs'.

That split is a cost decision, and it is worth stating as one. A full wrapper was rejected — it would have to re-expose relationships, wildcards, staging and the query language to be useful, it would cost an indirection on every query forever, and it would lag the library. But "unwrapped" must not mean "flecs owns the engine's guarantees". With the seams in place, the price of ever replacing flecs is *rewrite the system bodies and queries* — which no wrapper avoids either, because query semantics differ between ECS libraries — and nothing else. The type system, the scheduling contract, identity, persistence and the module graph all survive the swap because none of them is flecs'.

## Seam 1: components come from the schema IDL

A component is declared **once**, in a `.schema` file, as a struct carrying `@kind(component)`:

```
struct Standing @version(1) @kind(component) {
  position: vec3
  wealth: i64 = 0
  scratch: u32 @transient
}

struct Perception @version(1) @kind(component) @transient { ... }
```

`schemac` emits the C++ struct as it always has, plus `<schemas/<stem>_ecs.h>`, whose one function registers every component that file declares:

```cpp
#include <schemas/ecs_demo_ecs.h>
engine::ecs::demo::register_ecs_demo_components(sim.world());
```

What the registration does, and why each part of it matters:

- **Under the schema's qualified name.** `engine.ecs.demo.Standing` becomes the flecs path `engine::ecs::demo::Standing`, so the schema namespace is a flecs scope and the explorer's tree is the schema's tree. `ecs::lookup_component(world, "engine.ecs.demo.Standing")` resolves the dotted IDL spelling directly. One name, wherever it is read from: the protocol, a save file, a migration, a log line and the explorer all say the same string.
- **With member reflection where the schema type maps onto flecs meta.** Scalars, enums (as their underlying integer), `id128` (two `u64`), `vec2/3/4` and `quat` (two to four `f32`), and fixed arrays of scalars are described to flecs at the offset the schema descriptor gives, so flecs points at exactly the bytes the C++ type has. A `string`, a `bytes`, a nested struct, an array, a map, an optional or a `json` payload is **left out rather than described wrongly**, and flecs treats the component as a partial type. That is the honest answer: `core/schema` is the authority on those fields and can render all of them, and duplicating half of its walker in flecs' meta language would give the engine two serializers that have to agree.
- **With `@transient` carried as a tag.** A `@transient` component gets `ecs::Transient` on its **component entity**, so "what would a save file contain" is a question a walk of the world's components can answer. The same marking is on the schema descriptor as `schema::TypeFlag::transient`, which is how persistence, the protocol and migrations see it without linking an ECS.

**The one exception.** `ecs::register_private_component<T>(world, name)` registers a flecs component for state that is private to a module, never persisted and never visible to the protocol — a per-world cache, a scratch tag. It is marked `Transient` and has no `TypeInfo`, so `WorldCommands` cannot name it and a persistence walk skips it. Saying so at the call site is the point; the distinction is then visible in the world rather than only in a comment.

**The index space.** `sim::ComponentMask` addresses components by a small integer and nothing assigned those integers until now. `ecs::ComponentRegistry` does, in registration order, per world: `ecs::component_index<T>(world)` and `ecs::mask_of<A, B>(world)` are how a system's declared sets and the world's components come to share a vocabulary. A component registered past `sim::k_max_components` gets `k_invalid_component_index` and a logged warning rather than silently aliasing another component's bit ([ADR-0017](../adr/0017-no-hidden-limits.md)).

**`ComponentType::flat`** records whether every field, recursively, is a scalar or a fixed array of scalars. A flat component *is* its bytes, which is what lets `WorldCommands::set_bytes` copy one in without a serializer, and what a future binary protocol encoder will key on.

## Seam 2: systems register through the engine's descriptor

```cpp
ecs::register_system(sim, k_move_desc, [](flecs::world& w, flecs::entity phase) {
  return w.system<Position, const Velocity>("move").kind(phase).multi_threaded()
          .each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
});
```

One call, two halves. `k_move_desc` is a `sim::SystemDesc` — phase, read and write component sets, LOD tiers, determinism stance — declared to the **engine**; the lambda builds the flecs system with flecs' own builder, flecs' terms and flecs' `each`/`run`. Neither half is wrapped and neither is optional. The callable takes the phase rather than returning a system the caller already built, so `.kind(phase)` cannot be forgotten and cannot disagree with `desc.phase`.

**`sim::TickPhase` is the one phase enum, and `ecs::TickPhase` is an alias of it.** Not "the same enumerators in the same order" — the same type, so a capability's `SystemDesc::phase` and the flecs phase entity its body attaches to cannot drift apart. The dependency goes `domain/ecs` → `domain/sim` and never the other way: the scheduler has to stay free of flecs for the executor to be replaceable at all. `ecs::phase_entity_name()` is the flecs entity's name (`sim_lod`), distinct from `sim::phase_name()`'s short name (`lod`), and prefixed so a phase entity can never collide with a system named after its phase.

**The debug check.** In debug builds, registration reads the built query's terms back and compares them with the declaration. A term the system has and the descriptor does not is an **assert**: the scheduler would run that system in parallel with one that writes the component, and nothing downstream could tell. A declaration the query does not use is **not** an error, because that is how a system tells the schedule about accesses its terms cannot mention — a singleton it reads, a pool it writes through an index, a component it adds. Over-declaring costs parallelism and is visible in `schedule_hash()`; under-declaring costs correctness and is invisible, so only one of the two is worth an assert. `ecs::query_access(world, system)` is the same reading, exposed, so a test can ask what the check asks.

A non-`const` term is flecs' `InOutDefault`, which means the system may write, and the check reads it that way. That is deliberate: a declaration that omits the write is exactly the bug worth finding.

**Resources are not query terms, and the check knows it.** `sim::SystemDesc` now also carries `reads_resources`/`writes_resources` — named data outside the ECS, which is what [03 §3.4](../plan/03-data-model.md#34-the-runtime-world) tells a hot system to own ([sim](sim.md), "Named resources"). A query has no term for a pose pool, so there is nothing to compare a resource declaration against and the check does not try; that is the *point* of them, because they are how a system declares the accesses the paragraph above says its terms cannot mention. `sim::ComponentMask` and `sim::ResourceMask` are distinct types, so a resource id cannot land in a component mask by accident. What the check does add is that every declared resource id is one `sim::ResourceRegistry` handed out, which catches a mask built from a stale constant rather than from `sim::resource_mask()` — the failure that would otherwise claim another capability's pool silently.

**What v1 does not check.** Relationship terms — `(Likes, *)`, `(ChildOf, parent)` — have no place in a mask that addresses plain components by index, so they are counted (`QueryAccess::pair_terms`) and nothing is claimed about them. Closing that gap means deciding what a relationship's bit means, which is the scheduler's design and not this seam's.

**Who runs the systems.** `SystemDesc` goes into `ecs::SystemRegistry`, a per-world table in registration order; the flecs system is created with `.kind(phase)`. `SimWorld::step()` runs it with flecs' own pipeline; under `ecs::ScheduledTick` the engine's scheduler owns the tick and runs each phase's systems as a per-phase flecs pipeline inside its phases ([below](#the-tick-under-the-engines-scheduler), [ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md), proposed). A system is registered the same way for both and cannot tell which is driving it — the capabilities' tests run each system under both executors and compare the output byte for byte.

## Seam 3: persistent identity is `Id128`

A flecs entity id is a world-local handle with a generation counter: the right thing to hold inside a tick and the wrong thing to hold anywhere else. It does not survive a save, a reload, a tile unload and rematerialization, a replay on another machine, or a second process looking at the same world. `Id128` is the engine's stable identity ([03 §3.1](../plan/03-data-model.md#31-identity)) and it is what the document, the store and the protocol already speak.

**Nothing outside `domain/ecs` stores a flecs entity id in anything that outlives a tick's working set.** Inside a tick, iterate entities; across a tick, across a save, across the wire, name an `Id128`. `ecs::Identity` is the component and `ecs::IdentityMap` is the one place the two meet: `entity_for(world, id)`, `create_entity(world, id)`, `id_of(entity)`.

**One map, not two.** `Id128 → flecs::entity` needs a hash map. The reverse does not: the entity carries the component, so `id_of()` is a component read — the same O(1) with none of the memory and nothing second to keep in agreement.

**Maintained by observers, not by the call sites.** The map has to be right even when an entity is destroyed by something that never heard of it — a `delete_with`, a tile teardown, a system that removes `Identity`. So `OnSet` and `OnRemove` observers on the component maintain it, which means a raw `world.entity().set<ecs::Identity>({id})` joins the map too. That is what makes this a seam rather than a convention reviews have to enforce. Two entities claiming one id is a content or protocol bug rather than an engine one, so it is counted (`IdentityMap::collisions()`) and logged, the last writer holds the row, and the loser leaving does not evict the winner.

`SimWorld` installs the observers in its constructor rather than on first use, because "first use" can be from inside a system body, and **an observer created inside a deferred region does not exist until the merge — by which time the commands it was meant to see have already been applied.** That failure is silent and it cost an afternoon; the module's `detail::Undeferred` exists for the same reason and every per-world accessor here uses it.

**The bridge to the materialization contract lives here, and only here.** [sim](sim.md)'s hooks table names a persistent record with an `Id128` and a live entity with a `sim::EntityHandle` — opaque, valid only inside the call that carries it. `ecs::handle_of(entity)`, `ecs::entity_of(world, handle)` and `ecs::handle_for(world, id)` are the three functions that turn a flecs entity id into one and back, and they are the **only** place in the tree that knows an `EntityHandle` holds a `flecs::entity_t`. That is what makes this seam's rule checkable rather than reviewable: outside `domain/ecs` the runtime name is an opaque struct, so storing one past a tick is not something anybody writes by accident. `entity_of` answers with entity 0 for a handle that names nothing live, the same "not found" `entity_for` returns and for the same reason — and it tests with `is_valid()`, because `is_alive()` **asserts** on entity 0 rather than returning false, which is how the first version of this crashed.

## Seam 4: mutation from outside a system is guarded

A system body gets the raw `flecs::world&` and writes it directly. Everything else that wants to change the world — the protocol answering `world.apply`, the persistence layer materializing a tile, a save being loaded, a network packet, an agent's edit — arrives on another thread or between ticks, names entities by `Id128` because that is what survived the trip, and must not land in the middle of a phase. `ecs::WorldCommands` is the queue those callers use:

```cpp
ecs::WorldCommands commands(sim.world());
commands.install(sim, ecs::TickPhase::EventsIn);   // drained at a phase boundary, every tick

commands.create(id);
commands.set_json(id, "engine.ecs.demo.Standing", payload, &diagnostics);
commands.remove(id, "engine.ecs.demo.Perception");
commands.destroy(other);
```

- **A phase boundary.** `install()` puts the drain in a phase, so no system ever sees half of an external edit. `EventsIn` is the default because that is what an external edit is: something that happened since the last tick, delivered at the top of this one ([05 §5.2](../plan/05-simulation.md#52-sim-scheduler)).
- **`Id128`, not entity handles.**
- **Schema types, by name.** `set_json` goes through the reflection schemac already generated, so the protocol needs no per-component code and a type the schema does not declare cannot be set at all. `set_bytes` is the flat-component path.
- **Failure where the mistake is.** The payload is parsed **when it is queued**, so a caller learns its JSON was wrong while it still has somewhere to report it, with `core/schema`'s field path. `apply()` cannot fail on a parse.

**It is not a transaction.** `apply()` walks the queue in order and counts what it could not do; a failed command does not roll back the ones before it. The event log is what makes an edit undoable ([ADR-0003](../adr/0003-event-sourced-persistent-state.md)), not this queue. It is also not thread-safe: queue from one thread, apply from the thread that owns the world.

**Why `apply()` plays by flecs' deferring rules.** `ecs_progress` makes the world **readonly** while the pipeline runs, so a drain inside a tick cannot suspend deferring and mutate directly — that is an assert, not a race. Every command therefore goes through flecs the way a system's would. The consequence is that an entity created in a batch is not in the world's identity map until the batch merges, so `apply()` remembers its own creates: `create` followed by `set` in one request is the shape every caller will write, and it works.

**Where the protocol attaches.** `world.query`, `world.apply` and the rest of [06 §6.2](../plan/06-agent-tooling.md#62-engine-protocol)'s method table are **not wired up**. When they are: `apply`'s parameters map onto `create`/`set_json`/`remove`/`destroy` one for one, the dispatcher owns one `WorldCommands` per session, and `install()` is what makes a session's edits land on a tick boundary. Nothing in `world_commands.h` has to change for that. `world.query` is the other half and is flecs' query DSL over the schema names seam 1 registered. The read half by name already exists: `ecs::component_json(world, id, "engine.world.Transform", out)` renders one entity's component through `core/schema`, which is what `session.run_headless`' predicate reads live entities with ([protocol](protocol.md#sessionrun_headless-the-materialized-world)).

**The materialization hook is seam 4's other client.** `ecs::RecordMaterializer` changes the world from outside a system too, and keeps the same promises — `Id128` names, schema types only, between ticks — but writes *fields*, not whole components ([below](#materialization-the-entity-stores-hook)).

## Seam 5: include hygiene

`flecs.h` may be included **only** by `domain/ecs`, `systems/*`, `game/*`, and their tests and benches. `tools/lint.ps1` enforces it as a confinement rule (`flecs-include`), checked under CTest as `lint.banned_patterns`, with its own tests in `tools/lint.Tests.ps1` (`tools.lint`). Unlike the container and exception rules, the confinement applies to tests and benches too: a module whose *test* reaches flecs has an ECS dependency in its build.

So `physics`, `nav`, `anim`, `gfx`, `geometry`, `sim`, `store` and everything in `core/` and `foundation/` stay ECS-free, and bridging code between an ECS-free module and the world lives in `systems/`. This is what makes seam 2's "the cost of replacing flecs is bounded" a fact about the build rather than an intention: the set of modules that would have to change is the set the linter names.

The generated `<schemas/<stem>_ecs.h>` is a header precisely because of this rule. The generated schema module sits in the core layer and must not link flecs; the registration must. A header is compiled only where it is included, and the only places that may include it are the places that may see flecs — which is also what keeps it out of a build with `ENGINE_WITH_ECS` off.

## Two traps E6 measured, as guard rails

**1. Relationships multiply archetypes as a cross product.** E6 gave 10,000 children an independent faction as well as a parent: 1,330 tables became **9,096**, the tick went from 2.79 ms to **4.71 ms (1.7×)** and memory from 162 to **773 bytes an entity**, with no change to the data. Nothing failed; it just got slower, which is the kind of regression that reaches a release.

So a world says what its component design expects and a debug build says so in the log when it is badly wrong:

```cpp
ecs::SimWorldConfig config;
config.table_watch.expected_archetypes = 1500;   // 0, the default, switches it off
config.table_watch.warn_multiple = 4.0f;
```

`SimWorld::step()` checks it every `check_every_ticks` ticks in debug builds only, and `SimWorld::table_watch()` is the reading. It warns rather than asserts because the number is a design estimate, not an invariant: a world legitimately grows tables while it loads, and a game that wants 20,000 of them is allowed to. What it is not allowed to do is get there without noticing. `ecs::TableWatch` is usable on its own against any world.

**2. Where things are, not what they are.** An archetype ECS indexes by *what an entity is*, never by where it is. E6 measured the same coarse spatial grid three ways on the same query (entities of faction F within radius r):

| Implementation | Latency | Against the scan |
|---|---|---|
| Scan the faction, distance test | 5.53 µs | 1.0× |
| Scan the faction, reject on a `Cell` **component** first | 10.04 µs | **1.8× slower** |
| One cached query per intersecting cell, cell as an `(InCell, cell)` **relationship** | 0.417 µs | **13× faster** |

The rule that generalizes past flecs: **a component the query reads and rejects on is not an index.** It adds a column to the iteration and saves nothing, because the distance test it replaced was already two multiplies. The only way to make a query *skip* entities is to put the discriminator in the archetype — a relationship — or to keep the spatial structure **outside** the ECS entirely, as a grid or a BVH the system owns, with the ECS holding an index into it ([03 §3.4](../plan/03-data-model.md#34-the-runtime-world)'s "hot systems own their own data").

Choose between them by the cost of trap 1. A relationship is the right answer when the discriminator is coarse and slowly changing: `(InCell, cell)` over an 8×8 grid took E6's world from 1,330 tables to 8,158, and that bought 13×. It is the wrong answer when the discriminator is fine-grained or changes every tick, because then every movement is an archetype move *and* the table count is the cell count multiplied by every other archetype. A per-entity cell **component** is never the answer. A capability that adds a relationship **says in its docs page what it does to the archetype set** — that sentence is the review item, and `table_watch` is what catches the ones that were not written down.

## The tick

**The phases.** `TickPhase` is [05 §5.2](../plan/05-simulation.md#52-sim-scheduler)'s list in its order, as flecs phase entities chained with `depends_on`, which is what lets the built-in pipeline pick up a system that names one without the pipeline knowing the system exists — [ADR-0027](../adr/0027-additive-capabilities.md)'s registration point for anything that ticks. The chain hangs off `flecs::PostFrame`, flecs' last built-in phase: the engine uses none of the built-in phases, but a system that forgets `.kind()` defaults to `OnUpdate`, and anchoring at the end means such a system runs *before* the engine's `input` phase instead of somewhere in the middle of the tick — a visible mistake rather than an invisible one.

**The singletons are core/time's own types.** `world.get<SimTick>()` and `world.get<GameTime>()`, not an `ecs::Tick` that has to be kept in agreement with the clock. They are published *before* the pipeline runs, so every phase of a tick sees that tick's numbers, and they exist from construction, so a tool inspecting a world that has never stepped does not read a missing component.

`SimWorld::step()` runs exactly one fixed step and reads no clock at all, which is what makes a replay, a headless fast-forward and a live session take the same path ([05 §5.10](../plan/05-simulation.md#510-determinism-and-replay)). `advance(real_ns)` feeds the `FixedStepClock` accumulator and runs whole steps, at most `max_steps_per_advance` of them. The accumulator's own tick counter is not the world's — `step()` advances the world without touching it — so `SimWorld::tick()` is the one to read.

**How the workers are hosted, and the measurement that chose it.** flecs offers long-running worker threads (`ecs_set_threads`, through `ecs_os_api_t::thread_new_`/`thread_join_`) or per-update tasks (`ecs_set_task_threads`, through `task_new_`/`task_join_`), and both exist precisely so an engine can host them on its own job system. The engine already pins one worker per logical CPU and splits them into pools ([11 §11.5](../plan/11-performance-principles.md#115-threads-cores-and-memory-locality)), so a second unpinned set of threads would oversubscribe every core. `ecs::JobOsApi` fills in **both** pairs of hooks and schedules every flecs worker as a job on `Pool::Performance`; `ecs::set_workers` chooses between them and **`WorkerHosting::Threads` — one long-running job per worker, for the world's lifetime — is the default** ([ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md)).

It is the default because the per-tick form was measured, on 2026-09-18, to cost **1.6–2.2 ms a tick** on E6's 100,000-entity world — enough that every worker count was slower than one worker — and long-running hosting adds nothing measurable *to the tick*: the same tick runs within a few percent of flecs' own OS threads at 2, 4, 8 and 16 workers and keeps scaling instead of turning back up. It has a cost, but the cost is pool workers rather than milliseconds, and it is paid once per world; see below. The tables are in [E6](../experiments/e6-ecs-store.md#worker-hosting-re-measured-2026-09-18).

**Why per-tick hosting cost that.** Two mechanisms, both measured, both a property of `ecs_set_task_threads` rather than of `core/jobs`:

1. **flecs creates and joins the whole worker set inside every `ecs_progress`.** The main thread then spins in `flecs_wait_for_workers` — a bare lock/check/unlock loop on one critical section — until every new worker has announced itself. Counted: **5,400 to 50,000 critical-section round trips a tick**, against **4 to 18 in total** for a tick with `ecs_set_threads`, and **0.32 ms (two workers) to 2.5 ms (sixteen)** of main-thread time a tick spent outside the pipeline, against 4–6 µs.
2. **Every tick's worker is a fresh job**, and the pool worker that ran the previous tick's has gone to sleep on the pool's eventcount in between, so every tick pays a wake per worker and every single-threaded stretch of a tick leaves a pinned core idle mid-tick.

Neither is the job system's fault, and this is worth being precise about because the obvious fix would have been aimed at the wrong place: submitting and joining the same number of empty jobs on the same pool costs **1.16–5.7 µs** (`ecs.task.roundtrip`), three orders of magnitude below the cost, and the pool's spin-then-sleep policy is the right one for the short jobs it exists for. What was wrong was asking the pool to re-dispatch a *long-running* worker every tick. `core/jobs` is unchanged.

**What long-running hosting trades, and the number the application picks.** A flecs worker is not a short job: it blocks on a condition variable between sync points and returns only when the world gives its workers back. As one job it therefore **occupies** a pool worker for the world's lifetime rather than using it. That cost is paid once instead of every tick, and it is bounded:

- **`JobOsApiConfig::hosted_worker_budget`** is how many performance-pool workers may be lent to flecs worlds at once. **The default lends all but one**, so the pool always keeps a worker for everything else; an application that wants more of its pool back sets the number. `JobOsApi::hosted_workers()` is how many are out and `pool_workers_kept()` is what is left.
- **A count the budget cannot give is refused**, with a log line naming the request, the budget and what is already out, and the world is left single-threaded but ticking. That is not politeness: a flecs worker with no thread to run on does not make the tick slow, it makes `flecs_wait_for_workers` spin forever on a worker that never announces itself.
- **Two live worlds share one budget.** A world takes its share at `set_workers` and gives it back when it is reconfigured, dropped to one worker, or destroyed.
- **Nothing long-running may be scheduled on the performance pool beside a ticking world.** Short jobs are fine; they run on the workers the budget kept back.
- **There is no work stealing *into* the tick**: flecs splits each system's matched entities evenly across its workers, so an uneven system is as slow as its slowest range. That is flecs' scheduling model, not something this adapter can improve, and it is one of the two reasons [ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7 is still open.
- **Join blocks rather than helps**: `JobSystem::wait` would run other performance-pool jobs on the joining thread, and one of those could be another flecs worker, which would put the thread inside flecs' own sync protocol and deadlock it.

**Lifetime, which is the sharp edge of holding a pool thread for a world's life.** A world must give its workers back before it is destroyed — `SimWorld`'s destructor does, and it is required anyway because flecs' own `ecs_fini` does not join a world's workers, it asserts that they are gone. If the `JobOsApi` is destroyed while a world still holds workers, the adapter takes them back and logs an error, and that world keeps ticking single-threaded; the alternative is a pool thread parked inside flecs that `JobSystem`'s destructor cannot join, which is a hang and not a diagnosable one. A hosted worker also **renames its pool thread** to `ecs-worker-<n>` for as long as it holds it, and back to `perf-<n>` when it lets go, so a profiler and a debugger show what is actually on those threads.

**[ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 3 is wrong in one clause and right in the rest, and it is not edited because it is Accepted.** It says the adapter runs flecs' workers "through `ecs_set_task_threads` and the `ecs_os_api_t` task hooks"; that mechanism is replaced by `ecs_set_threads` and the thread hooks, by [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md). Everything else in decision 3 stands — the workers are still the engine's pinned pool threads, there is still no second thread set, and the reason to host them at all is unchanged. Decision 7 is untouched and still open; if anything this makes it easier to judge, because flecs' pipeline now costs what flecs' pipeline costs.

The hooks are assigned on flecs' public `ecs_os_api` global rather than through `ecs_os_set_api()`, which is one-shot and refuses once anything has claimed the API — and what claims it first depends on whether a `flecs::world` or the adapter was constructed first. Assigning the fields is idempotent and order-independent. Everything else — heap, time, mutexes, condition variables — stays as flecs shipped it: they are thin platform wrappers with nothing engine-specific to gain, and the workers that wait on those condition variables are flecs' own state machine.

## The tick under the engine's scheduler

`ecs::ScheduledTick` (`scheduled_tick.h`) makes `sim::SimScheduler` the executor of a `SimWorld` ([ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md), proposed; it closes [ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7 if accepted). It installs itself as the scheduler's `sim::TickExecutor` and, every tick:

1. publishes the scheduler's tick and game time as the world's `SimTick` and `GameTime` (`SimWorld::sync_clock`) and opens a flecs frame (`ecs_frame_begin`, with the step as the delta);
2. in each phase, runs a flecs pipeline that selects the systems carrying that phase — in creation order, flecs' default for a pipeline, which is the order `step()` runs them in within a phase — through `ecs_run_pipeline`, so flecs' workers (ADR-0030), sync points, merges and deferred-command rules are exactly what they were; a phase with no system is skipped on a cached query's `is_true` without touching flecs;
3. closes the frame (`ecs_frame_end`) and runs the debug table watchdog.

The scheduler runs its own table's waves after flecs' systems in each phase ([sim](sim.md#the-executor-seam)). **One clock, one executor**: the world and the scheduler must have the same step (asserted), and a scheduled world asserts if `step()` or `advance()` is called on it. Detaching (destroying the `ScheduledTick`) hands the world back to its own pipeline. flecs' `OnStart` systems are not run under the scheduler; nothing in the engine declares one.

What it deliberately does **not** do yet is run flecs' systems in the scheduler's waves and jobs: inside a phase, flecs still splits a system's rows evenly across its workers and hierarchy propagation still cannot be `multi_threaded()`. Taking that on is a change inside `ScheduledTick` alone — `SystemRegistry` is the table it would read — and the ADR says when it is worth it. `ecs.tick.four_systems.scheduled` is E6's four-system tick under it, beside `ecs.tick.four_systems` under `step()`.

## Materialization: the entity store's hook

`ecs::RecordMaterializer` (`materialize.h`) is the entity store's row of `sim::SimScheduler`'s materialization hooks and the `sim::MaterializeTarget` the driver asks what the world can hold ([sim](sim.md#the-driver-a-document-through-the-hooks)). Register its hooks **first**: the first hook to give a record a runtime existence names it, and dematerialization walks the table backwards, so the entity store creates first and lets go last.

**Identity.** Handed a document record, it finds the entity the record's `Id128` names in the identity map, or creates one keyed by it. The record's id *is* the entity's key, so a record and its entity are one identity across saves, reloads and rematerialization, and nothing keeps a pairing that could drift.

**The mapping table.** Which components a record type becomes, and which record property fills which component field, is data: the IDL's `materialize` declarations, compiled by schemac into `schema::MaterializeInfo` rows ([schema](schema.md#materialization-type-mapping-as-data)). The hook resolves a row against this world once — each component's flecs id through `ComponentRegistry`, each field's `FieldInfo` — and a row naming a component this world has not registered is refused whole (the driver reports it as `missing component`, with the name) rather than half made. There is no per-type code anywhere.

**A new entity is one table move, into a table found by type.** Its whole archetype — `Identity`, every component the mapping names, and the `(ChildOf, parent)` pair — is sorted into one type, the table for it found or made with `ecs_table_find`, and the new entity committed to it with `ecs_commit` (the whole type as the added ids, so `OnAdd` observers see what they would have). Not `ecs_entity_init` with `add` and `parent`, which was the first version: flecs walks the table graph from the parent's `ChildOf` table one id at a time, and every step of that walk is a table it creates and keeps, so each new parent cost its children four tables where one holds them (below). Then each component's mapped fields are written in place through `schema::read_mapped` — a direct copy or the unit conversion the schema declared — and each component is marked modified once, so observers and change detection see one write per component. `Identity` is written and marked modified first, so the identity map binds it at once and a later hook of the same record finds it.

**An update writes only mapped fields.** A changed record is handed over again; the hook writes its mapped fields and leaves every other field alone — a velocity a system integrated, a cache a system filled — which is why this is not `WorldCommands::set_json`, which replaces a component whole. A property value that does not fit its field leaves the field as it was and is counted (`stats().invalid_values`); the document's own validation names the property.

**The parent is `ChildOf`, and it never cascades.** A mapping that says `parent = ChildOf` makes the document's composed parent the entity's `(ChildOf, parent)` when the parent has an entity, and keeps it in step when the document's parent changes. flecs deletes an entity's `ChildOf` children with it; the document does not delete a record's children with it — so `dematerialize` first moves the entity's children to the root, and the driver re-links them when their parent comes back. **What that costs the archetype set** (ADR-0028's guard rail): every distinct parent is a distinct archetype for its children, so a document with *n* parents that have children makes about *n* more tables; the 100,000-record bench document, one market per sixteen stalls, is 5,883 parents and 6,137 tables (it was 23,787 while creation walked the table graph, above; [sim](sim.md#the-materialization-driver) has the timings). That is flecs' hierarchy and what `cascade()` queries need; the table watchdog is what notices one nobody sized.

**Write-back.** For every entity whose mapping has `@writeback` rows the hook keeps a shadow of those fields' bytes as it wrote them, in one slab per mapping. `collect_writeback` compares each watched entity's fields against the shadow, reports the ones that differ as `sim::WriteBackChange`s in the record's units (`schema::write_mapped`, the inverse of the row) sorted by (record id, row), and takes the new bytes as the baseline. No allocation when nothing changed; a JSON value only for a field that did. It is a scan of the watched entities, once per write-back flush and not once per tick — the flush's cadence is what bounds it ([sim](sim.md#write-back)).

**Between ticks only.** A call while the world is deferred or readonly is refused with a warning: an entity created there would not be in the identity map until the merge.

## Owned data

One `flecs::world` per `SimWorld`, its eight phase entities, the `SimTick`/`GameTime` singletons, and four per-world singletons the seams keep: `ComponentRegistry`, `SystemRegistry`, `IdentityMap`, and the marker that says the identity observers are installed. A `ScheduledTick` owns one flecs pipeline and one cached query per phase; a `RecordMaterializer` owns its resolved mappings and the write-back shadow slabs. The adapter owns the mapping from flecs' workers to `core/jobs` jobs for the process, and the budget of pool workers that may be lent to worlds. Nothing else; components belong to whoever declares them.

**Hold a singleton reference only for the call that obtained it.** `components(world)`, `systems(world)` and `identity_map(world)` return references into flecs' storage; a world mutation may move them.

## Invariants

- Every phase is a distinct named entity carrying flecs' `Phase` tag, and the eight run in plan order whatever order their systems were created in.
- The `SimTick` and `GameTime` singletons exist from construction and advance exactly once per `step()`.
- A component registered from a schema is findable by its dotted qualified name and by its flecs path, and registering it twice is the first registration.
- A `@transient` schema struct's component entity carries `ecs::Transient`; a hand-registered private component does too.
- Every registered component has a distinct `ComponentMask` index below `sim::k_max_components`, or `k_invalid_component_index` and a counted overflow.
- A live `Identity` is in the identity map, and an entity that is destroyed or loses the component is not — however it was destroyed.
- `WorldCommands::apply()` leaves the queue empty and every parsed payload freed, whether or not the command succeeded.

## Public API

- `domain/ecs/sim_world.h`: `TickPhase` (an alias of `sim::TickPhase`) and `phase_entity_name`; `Phases`; `TableWatchConfig` and `TableWatch`; `SimWorldConfig`; `SimWorld` (`world`, `phases`, `phase`, `step`, `advance`, `tick`, `game_time`, `step_seconds`, `clock`, `game_clock`, `table_watch`, and for an external executor `sync_clock`, `watch_tables`, `scheduled`/`set_scheduled`). Includes `<flecs.h>`; that is the point.
- `domain/ecs/scheduled_tick.h`: `ScheduledTick` (`step`, `advance`, `world`, `scheduler`, `phases_run`, `phases_skipped`, `pipeline`).
- `domain/ecs/materialize.h`: `RecordStats`, `RecordMaterializer` (`hooks`, `target`, `stats`, `watched`).
- `domain/ecs/components.h`: `Transient`, `ComponentType`, `ComponentRegistry`, `components`/`components_if_present`, `register_schema_component<T>`, `register_private_component<T>`, `component_index`, `mask_of<Ts...>`, `lookup_component`, `component_json`.
- `domain/ecs/systems.h`: `RegisteredSystem`, `SystemRegistry`, `systems`/`systems_if_present`, `register_system`, `QueryAccess`, `query_access`.
- `domain/ecs/identity.h`: `Identity`, `IdentityMap`, `identity_map`/`identity_map_if_present`, `entity_for`, `create_entity`, `id_of`, and the materialization bridge `handle_of`, `entity_of`, `handle_for`.
- `domain/ecs/world_commands.h`: `CommandKind`, `CommandStats`, `WorldCommands`.
- `domain/ecs/os_api.h`: `install_log_sink`, `set_flecs_log_level`, `WorkerHosting`, `JobOsApiConfig`, `JobOsApi` (`hosted_worker_budget`, `hosted_workers`, `pool_workers_kept`, `max_workers`, `job_system`, `workers_started`, `tasks_started`), `set_workers`, `stop_workers`.
- Generated per schema file: `<schemas/<stem>_ecs.h>`'s `register_<stem>_components(flecs::world&)`.

**Determinism stance ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)):** `hashed`. The step is fixed, tick and game time are integers, nothing reads a wall clock, and flecs orders systems within a phase by creation order whatever the worker count — the conflicting-write-set test pins that at 1, 2 and 4 workers, and a second test runs a small world sixteen ticks at 1, 4 and 8 workers and compares every component **bit for bit**, because a tolerance there would hide exactly the bug worth finding.

**Zero-cost-when-unused ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)):** no linked code. `ENGINE_WITH_ECS=OFF` (and the minimal presets) skip the module, its tests, its bench, its generated test components, *and* the flecs fetch, so a minimal configure does not even download it. That configuration has presets of its own — `msvc-no-ecs` and `linux-clang-no-ecs`, the latter in CI — because this module is the one everything that ticks depends on, and switching only it off is what exercises the capability graph: `systems/animation` has to follow it off through `engine_capability_requires` and the rest of the tree has to build without either. The minimal build cannot catch a missing edge, because it switches everything off for a different reason. The generated `<stem>_ecs.h` is written for every schema in every configuration and compiled in none of them where the capability is off, because nothing may include it there.

**LOD policy.** The module provides the phase (`TickPhase::Lod`), the tick the policy runs on, and `SystemDesc::tiers` as the place a system declares where it runs; the policy itself belongs to each capability, as ADR-0027 requires. The reference implementation — minimum over observers of f(distance, importance, weight) with a two-tick hysteresis dwell — is in `domain/sim`'s `TierAssignment`, and E6's measured version is in `bench/ecs_bench.cpp`.

**Depends on.** `base`, `containers`, `ids`, `json`, `jobs`, `log`, `memory`, `platform`, `schema`, `sim`, `time`, `doc` (the materialization driver's types reach it through `sim`), and flecs (`flecs::flecs_static`, included SYSTEM, pinned in `cmake/EngineEcs.cmake`).

**The explorer is compiled out, and why that is not caution for its own sake.** flecs' default addon set includes REST and the HTTP server under it, and `ecs_init()` imports the REST module on every world — which registers the `EcsRest` component and an observer, and opens nothing. The socket opens when something *sets* that component, and then flecs binds `INADDR_ANY` unless the caller names an address (`src/addons/http/http.c`). A listener on a non-loopback address is one Windows Firewall prompt per executable *path*, each prompt is a `PickerHost.exe` process holding a GPU context, and enough of them stop the NVIDIA driver surviving `vkCreateDevice` — which is how a night went on 2026-09-18 ([profiling](profiling.md)). This tree makes executable paths cheaply: every preset and every agent worktree has its own copy of every test, bench and app. So `cmake/EngineEcs.cmake` builds flecs with `FLECS_NO_HTTP` and `FLECS_NO_REST` rather than leaving the socket one `ecs_singleton_set` away, and nothing here has to remember not to call it. `-DENGINE_WITH_ECS_EXPLORER=ON` compiles them back in for anyone who wants the explorer over a running world; whoever turns it on binds loopback — `ecs_singleton_set(world, EcsRest, {.ipaddr = "127.0.0.1"})` — because the explorer is a tool for the machine it runs on, and that is AGENTS.md's convention for anything that listens. `tools/ci/check-no-listeners.ps1` is the check ([self-hosted runners](../ci/self-hosted-runners.md)).

## Testing

`tools/dev.ps1 test -Preset msvc-debug -Filter ecs`:

- **The tick** (`tests/ecs_tests.cpp`): the eight phases run in plan order when the systems are registered back to front; each phase is a distinct named entity carrying flecs' `Phase` tag; the singletons exist before the first step, advance once per step, and advance by `us_per_tick` of game time, with `advance()` running whole steps only, honouring the step cap and the time scale; two systems that both write one component run in creation order at 1, 2 and 4 workers, in both creation orders; and a relationship query written as plain flecs matches what it should, which is the evidence that a consumer writes flecs and not a wrapper.
- **Worker hosting** (same file): a multi-threaded system over 4,096 entities runs on exactly `workers` threads of which `workers - 1` are workers of the engine's job system, and the dispatch happens **once** and not once a tick; a count above the budget is refused rather than hung and leaves a world that still ticks; the per-tick hosting is still selectable and does create its workers every step; eight worlds created and destroyed in a row leave the pool with nothing occupied; two live worlds share one budget and the second is refused what the first holds; an adapter destroyed while a world still holds workers takes them back, leaving a world that ticks single-threaded and refuses workers afterwards; and sixteen ticks of a small world produce bit-identical components at 1, 4 and 8 workers.
- **Seam 1** (`tests/component_tests.cpp`): components declared in `tests/ecs_demo.schema` and compiled by schemac register under their qualified names and at the matching flecs path; a `@kind(record)` struct in the same file does not; registration is idempotent; `@transient` reaches both the component entity and the schema descriptor, and a field-level `@transient` still works inside a component; member reflection describes `i64`, `f32`, a `vec3` as three floats and an `id128` as two `u64` at the schema's offsets, and leaves a `std::string` out; `flat` is true for the two scalar components and false for the one with a string; the index space is distinct per component and `mask_of` builds from it; a private component is `Transient`, has no descriptor and cannot be named; and the same component still round-trips through `core/schema`'s JSON with its transient field omitted.
- **Seam 2** (`tests/system_tests.cpp`): `ecs::TickPhase` *is* `sim::TickPhase`; a registration lands the flecs system in the declared phase, runs it, and keeps the descriptor whole in `SystemRegistry`; `query_access` reads `const T` as a read and a non-const term as a write; a relationship term is counted rather than masked; a resource declaration survives registration and is not read as a query term; and over-declaring is accepted while the under-declaring case the assert fires on is detected.
- **Seam 3** (`tests/identity_tests.cpp`): an `Id128` names an entity in both directions and creating one twice is the same entity; the map follows the component when the entity is destroyed through flecs, when the component is removed, and when a raw `set<Identity>` created it; deferred creation and destruction reach the map at the merge; two entities claiming one id are counted, with the loser's departure leaving the winner's row alone; and the materialization bridge round-trips an entity through `handle_of`/`entity_of`, answers a handle whose entity is gone with entity 0 rather than asserting, and resolves a persistent id to a handle.
- **Seam 4** (`tests/world_command_tests.cpp`): a batch creates, sets from JSON, removes and destroys by `Id128`, and nothing happens before `apply()`; what flecs then holds round-trips back through `core/schema`; an unknown type, a non-component and JSON that does not fit are all refused where they were queued, with the field path; bytes are accepted for a flat component and refused for the wrong size and for one holding a string; commands for an entity that is gone are counted rather than fatal and a queue that is never applied still frees what it parsed; and an installed drain makes a whole batch visible to a system in a later phase of the same tick.
- **The scheduler as executor** (`tests/scheduled_tick_tests.cpp`, ADR-0038): flecs systems registered back to front run in plan order inside the scheduler's phases, flecs' before the table's in the one phase that has both, each handed the scheduler's step as its delta; the world reads the scheduler's tick and game time from the moment it is attached, empty phases are skipped, and a detached world steps itself again; a 1,024-entity world ticked sixteen times by the scheduler at one worker and at three gives the same bits as flecs' own pipeline; and a `WorldCommands` drain installed in `EventsIn` still lands before a later phase's system. The capabilities repeat the byte-for-byte comparison over their own systems (animation's matrices and playheads, the commands `audio_system` sends, kinematics' transforms).
- **Materialization** (`tests/materialize_tests.cpp`, over `Stall` and `Market` records declared with their mappings in `tests/ecs_demo.schema`): a document becomes entities keyed by its records' ids, a document-only record is skipped, a stall's parent market is its `ChildOf`, its fields are the record's (a type default where the record is silent, and 100 percent as a ratio of 1 through the declared units), and a market gets only the component its mapping names; an unchanged document is handed to no hook and writes no component; a changed property updates its field and leaves an unmapped field a system set; a deleted market dematerializes and its stall keeps its entity, moved to the root, and is relinked when the market comes back; two runs make the same entities in the same order with the same flecs ids, and a hash of the world is pinned; and a flecs system moving and paying the stalls under `ScheduledTick` writes position and wealth — not the sign, which is not a write-back row — back to the document at `Persist`, one transaction a tick attributed to `system`, after which the next pass calls no hook.

The size table pins `flecs::entity` at 16 bytes, `Phases` at 128, `SimWorldConfig` at 40, `Identity` at 16 and `ComponentType` at 32.

`tools/dev.ps1 bench -Preset msvc-release -Filter ecs.*` runs experiment E6's half: the four-system tick at 1/2/4/8/16 workers as the engine hosts them, the same tick with the three other hostings the finding rests on — `ecs.tick.four_systems.tasks` (the per-tick form ADR-0030 replaced), `.os_threads` (flecs' own threads, the number to beat), and `.tasks_never_sleep` (the per-tick form on a pool that does not sleep, which is the control that splits its cost in two) — a bare submit-and-join round trip as the other control (`ecs.task.roundtrip`), each system alone, three implementations of a faction-and-radius query, churn with and without deferring, and the working set of a 100,000-entity world. Numbers and what they mean: [docs/experiments/e6-ecs-store.md](../experiments/e6-ecs-store.md). Two more families sit beside E6's: `ecs.tick.four_systems.scheduled` is the same tick under `ScheduledTick` at 1, 4 and 16 workers (its numbers are in [ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md)'s consequences), and `ecs.materialize.full`/`.nochange`/`.incremental_1pct` run the materialization driver over this module's hook at 10^4 and 10^5 records (numbers on [sim](sim.md#the-materialization-driver)).

**Run one hosting at a time, in its own process.** Each (hosting, worker count) pair keeps a `Scene` — a 100,000-entity world and its own job system — alive for the process, so a run that covers several measures the later ones with every earlier configuration's threads resident; a full `--filter=ecs.*` run reads about 13% slow on the 1-worker row for that reason alone. The filter is a substring match unless it contains `*`, in which case it must match the whole name, so `--filter=*four_systems` selects the shipped hosting alone and `--filter=*.tasks` the per-tick one. Two runs of each with `--wait-quiet`, for any number that will be quoted.

## Performance notes

Measured on an i9-10980XE ([E6](../experiments/e6-ecs-store.md)), on a machine verified quiet by the harness ([bench](bench.md#measuring-on-a-shared-machine)), one bench executable at a time, every figure taken twice:

| | Value | Note |
|---|---|---|
| Four systems over 100,000 entities, 1 worker | **2.42 ms** | the figure to plan against |
| The same tick, 4 workers, as the engine hosts them | **1.25 ms** (1.94×) | flecs' own threads: 1.33 ms |
| The same tick, 16 workers | **1.02 ms** (2.37×) | flecs' own threads: 1.12 ms |
| The same tick, 4 workers, per-tick hosting (ADR-0030 replaced it) | 2.79 ms (0.87×) | slower than one worker |
| `lod_assign` alone, single-threaded | 1.65 ms | two thirds of the tick |
| Memory | 162 B an entity | 1,346 tables |
| Create and destroy an eight-component entity | 1.13 µs, 0.83 µs deferred | |

**Plan against the single-threaded figure anyway.** The parallel rows are what the tick costs when the pool has nothing else to do; a frame that is also rendering and streaming shares the pool, and a world holding `workers - 1` of it makes that sharing sharper rather than softer. Three things to know before writing a system here:

1. **`query_builder<...>().build()` is uncached and re-matches every table on every iteration.** A system's query is cached; a hand-built one is not unless it says `.cached()`. E6 measured 328 µs against 0.417 µs for the same query — 790× — on that difference alone.
2. **A relationship narrows a query; a component does not.** See "where things are, not what they are" above.
3. **Relationships multiply archetypes as a cross product.** See the watchdog above.

The seams themselves are cold by construction: registration happens at world setup, `mask_of` is called once per system, and `WorldCommands` is the external-edit path and not a per-frame one. The one thing in a tick is the table watchdog, which is a world-info read every 64th tick in debug builds and absent in release.

And one flecs limit worth designing around: **hierarchy transform propagation cannot be `multi_threaded()`**. `cascade()` orders tables so parents come before children, but multi-threading splits each table's rows across workers with no barrier between tables, so a worker can read a parent's transform before the worker owning that row has written it. Propagation stays single-threaded until the engine's own scheduler can put a barrier per depth.

## Not yet

- **A budget a real application has chosen.** `hosted_worker_budget` defaults to "all but one", which is a sensible default and not a measured one: nothing yet runs a renderer, a streamer and a world on the same pool, so nobody has had to say how the performance pool is actually divided. The first app that does should set it deliberately and record what it found.
- **flecs' systems in the scheduler's waves.** The scheduler owns the tick (above), but inside a phase flecs' pipeline still runs flecs' systems: it splits a system's matched entities evenly instead of letting the pool steal into the tick, and hierarchy propagation cannot be `multi_threaded()` at all (below). Those were ADR-0028 decision 7's two live limits and [ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md) leaves them for a measured change inside `ScheduledTick`. `SystemDesc::tiers` and `Determinism` are still recorded and not enforced: nothing filters a system's rows by tier yet.
- **engine-view on the scheduler.** Its animated world still steps itself (`SimWorld::step()`); moving it is the change that lets `step()` retire.
- **The protocol.** `world.query`, `world.apply` and the rest of [06 §6.2](../plan/06-agent-tooling.md#62-engine-protocol)'s table; see seam 4 for where they attach. `component_json` is the read half, by name.
- **The persistence flush to the store.** `TickPhase::Persist` now holds the materialization write-back, which writes `@writeback` fields to the *document* and logs an event per record in the store ([sim](sim.md#write-back)). What it does not do is write *projections*: walking the non-`Transient` components of changed entities into `foundation/store` clustered by tile ([store](store.md)) — the `flat` flag and the `Transient` tag are what that walk will read — is still not built.
- **Write-back by change detection.** `collect_writeback` compares every watched entity's writable bytes once per flush; flecs' change detection could skip the tables no system wrote, and at 10^5 watched entities flushing every tick that would matter. Measured with the npc capability's residents ([E38](../experiments/e38-scheduled-npcs.md#the-write-back-scan)): 6.3 ms a flush at 10^5 residents with nothing changed, 0.1 ms a tick at the default cadence of 60 and 38% of a tick at a cadence of one, while ~11 residents a second actually change. The proposal there: collect only from tables a watched component was written in since the last collect, or let the one capability that writes a component hand the write-back the entities it wrote.
- **Relationships in the mask.** `sim::ComponentMask` cannot express a pair, so the debug check counts them and says nothing about them.
- **flecs' REST explorer**, which is an introspection surface the protocol layer should eventually expose rather than a port the engine opens by itself.
