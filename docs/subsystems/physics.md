# physics (domain)

**Purpose.** The simulated world: rigid bodies, collision shapes, queries, contact events, a
debris pool, and XPBD soft bodies, on a fixed step ([05 §5.11](../plan/05-simulation.md#511-integration-notes),
[§5.13](../plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies),
[ADR-0026](../adr/0026-deformable-volumes-first-class.md)). [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
5.6 (MIT) is the backend and does not appear in the public surface. An optional capability
([ADR-0027](../adr/0027-additive-capabilities.md)): `ENGINE_WITH_PHYSICS=OFF`, or a `*-minimal`
preset, leaves the module, its tests, its bench, and the Jolt download out of the configuration
entirely.

**Owned data.** The `physics::World` owns everything: the backend's body table, the shape table
(shapes are immutable and shared by handle), the soft-body table with its attachments, the
debris ring, and the contact buffer of the last step. Nothing else may hold a backend object.
Transforms read out of a `World` are copies; the world is the source of truth between steps and
the only thing that advances it is `step()`.

**Invariants.**
- No public header names a `JPH` type, includes a Jolt header, or is affected by Jolt's build
  flags. The backend lives behind `World::Impl` in `src/`.
- A handle is `SlotMap`-shaped: a stale or null `BodyId`, `ShapeId`, or `SoftBodyId` fails every
  lookup rather than aliasing a live object.
- `layers_collide` is symmetric, and `Layer::Query` collides with nothing.
- A shape that bodies still reference cannot be destroyed (`destroy_shape` returns false).
- The step is fixed and reads no wall clock. `tick()` counts steps and nothing else.
- Contact events are drained by the caller after `step()`; no caller code ever runs on a
  backend worker thread.
- The contact buffer carries at most one event per (body pair, phase) per step, ordered
  deterministically, with `a < b`.
- Spawning past `debris_cap` destroys the oldest piece rather than growing the pool.
- A `step()` that has returned has left nothing of its own in the job system's queues
  (`WorldStats::backend_jobs_pending` is 0), so the number of live backend jobs is bounded by
  one step and never by how fast the pool drains.
- Two worlds built by the same sequence of calls and stepped the same number of times produce
  bit-identical transforms, whatever worker count each was given.
- A cage with a `max_strain` holds no edge outside that limit once `step()` has returned, to
  within one part in a thousand of the edge's rest length, unless both its ends are pinned or the
  clamp ran out of sweeps (`SoftBodyBudget::strain_clamp_saturated`). A cage with no limit runs no
  clamp and keeps no extra state.
- `WorldStats::soft_body_budget` describes the step that has just returned and nothing else: it is
  per step, unsmoothed, and wall clock rather than a sum of per-worker time
  ([ADR-0029](../adr/0029-deformable-volume-budgets.md)).

**Public API.** `domain/physics/types.h`: `Status`/`status_name`, `Layer`/`layer_name`/
`layers_collide`/`LayerMask`, `MotionType`, and the three handle types.
`domain/physics/soft_body.h`: `SoftEdge`, `SoftVolumeConstraint`, `AttachmentKind`,
`SoftAttachment`, `SoftBodyDesc`, `k_soft_body_constraint_batch` and `soft_body_solve_width`,
and the two cage builders `build_cloth_sheet` and `build_lattice_volume` with
their `ClothSheet` and `LatticeVolume` results.
`domain/physics/deformable.h` ([ADR-0029](../adr/0029-deformable-volume-budgets.md)): the budget
constants `k_deformable_ambient_budget_ms` / `k_deformable_hero_budget_ms`, the cage sizing
constants and `cage_size_verdict`, `volume_compliance_for`, and `SoftBodyBudget`. `domain/physics/physics.h`: `WorldOptions`,
`WorldStats`, `temp_allocator_size_for`, `CompoundChild`, `HeightfieldDesc`, `BodyDesc`,
`RayHit`, `ShapeHit`, `ContactPhase`, `ContactEvent`, and `World` itself — shape creation
(box, sphere, capsule, convex hull, triangle mesh, heightfield, compound), body creation and
destruction, transform and velocity access, `move_kinematic`, activation and sleep, the batch
`read_transforms`, `spawn_debris`, soft-body creation and `read_soft_body_vertices`, `cast_ray`,
`cast_shape`, `step`, `contact_events`, and `stats`.

**Depends on.** `base`, `containers`, `math`, `time`, `jobs`, `log`.

---

## Why nothing from Jolt is public

[ADR-0026](../adr/0026-deformable-volumes-first-class.md) decision 3 says replacing the solver
must not change the asset format, the binding data, the LOD policy, or the renderer path. A
header that exposed `JPH::BodyID` or `JPH::Vec3` would make every consumer a Jolt consumer and
that decision unenforceable — the abstraction would exist on paper and be violated at the first
call site that found it convenient.

There is a second, more immediate reason. Jolt's headers change shape with the flags its library
was compiled with (`JPH_DOUBLE_PRECISION`, `JPH_OBJECT_LAYER_BITS`, the `JPH_USE_SSE4_2` family,
`JPH_CROSS_PLATFORM_DETERMINISTIC`), and a translation unit that sees a different set than the
library did produces link errors or, worse, silently different layouts; Jolt guards this with a
version id that fails at startup. Keeping every `#include <Jolt/...>` inside this module's `src/`
means exactly one set of translation units has to agree, and `engine_physics` is the only target
that links `Jolt`.

The public surface is therefore core/math (`Vec3`, `Quat`, `Transform3`, `Aabb3`), core/time's
`SimTick`, core/containers handles, `std::span`, and a `Status` enum.

## The layer table

Five layers ([05 §5.11](../plan/05-simulation.md#511-integration-notes)). Each is one Jolt object
layer, so the object-layer pair filter is `layers_collide()` from `types.h` verbatim and there is
no second table to keep in sync.

| | Static | Moving | Debris | Kinematic | Query |
|---|---|---|---|---|---|
| **Static** | — | yes | yes | — | — |
| **Moving** | yes | yes | yes | yes | — |
| **Debris** | yes | yes | — | yes | — |
| **Kinematic** | — | yes | yes | — | — |
| **Query** | — | — | — | — | — |

The reasoning per empty cell: two static bodies never move, so testing them is pure cost. Two
kinematic bodies would both refuse to respond, so a contact between them can only produce jitter.
**Debris never collides with debris** — that is the line in plan 05 §5.11 that makes a hard-capped
debris pool affordable, because rubble piling on rubble is quadratic in the pile for an effect
nobody inspects. Static against kinematic is off for the same reason as static against static: a
moving platform sliding along a wall resolves nothing. `Layer::Query` is collision geometry that
exists only to be cast against — triggers, cover volumes, aim probes — and simulating it would be
a bug, not a feature; it is still found by `cast_ray` and `cast_shape` when the mask includes it.

Broadphase layers are coarser, because each one is a bounding-volume tree and the point of having
more than one is that the tree holding the world's static geometry is not rebuilt because a crate
moved. There are four: `Static` and `Query` share one (neither moves), and `Moving`, `Debris`, and
`Kinematic` get one each because they move in bulk and are queried by different sets of layers.
The object-vs-broadphase filter is derived from `layers_collide()` at compile time rather than
written out, so the two can never disagree.

Queries take a `LayerMask` and ignore the collision table entirely: what a body is *simulated*
against and what a raycast is *allowed to find* are different questions.

## Determinism

Two things are called determinism here and they are not the same.

**The build property.** Jolt's `CROSS_PLATFORM_DETERMINISTIC` is on. It sets `/fp:precise` on
MSVC and `-ffp-contract=off` elsewhere and disables the FMA paths, so the same source produces the
same simulation on a different compiler, configuration, OS, or architecture. [05 §5.10](../plan/05-simulation.md#510-determinism-and-replay)
budgets about 8% for it; measured here on `msvc-release` it is **1.6% to 3.6%** — 2.43 ms against
2.39 ms for a step of the 1000-box bench, 942 µs against 909 µs for the soft cube at eight
iterations. It is on now rather than later because turning it on changes every hash a replay or a
lockstep session has ever recorded, and the day that becomes expensive is the day it is needed
([ADR-0016](../adr/0016-multiplayer-readiness.md)).

**The per-world flag.** `WorldOptions::deterministic` is Jolt's `mDeterministicSimulation`: it
sorts contacts and islands so the result does not depend on the order jobs happened to finish in.
Leaving it on is what makes the 1-worker and 8-worker runs agree bit for bit.

Neither of them covers the *callbacks*. Jolt calls the contact listener from whichever worker
solved the island, so the order events arrive in is a scheduling artefact. The module records them
into a per-worker bucket (lock-free: one bucket per performance worker plus one for every other
thread, since the stepping thread executes jobs too while it waits on the barrier) and then, on
the stepping thread, merges, sorts by the backend's own body and sub-shape indices, and collapses
each (pair, phase) run to its first entry. That is what makes "begin is reported once per pair" a
property of the buffer rather than of the caller's bookkeeping, and it is why no game code ever
runs on a physics worker.

The determinism test builds the same 64-body scene twice on one `jobs::JobSystem`, gives one world
a concurrency of 1 and the other 8, steps both 600 times, and compares the transforms with
`memcmp`. It also checks that the two worlds really did split their work differently, because
otherwise the comparison would prove nothing.

## Jolt's build flags, and what goes wrong without each

Jolt's CMake is written to be the top-level project. Every option in [`cmake/EnginePhysics.cmake`](../../cmake/EnginePhysics.cmake)
is there because the default breaks something:

| Option | Default | Set to | What the default does |
|---|---|---|---|
| `OVERRIDE_CXX_FLAGS` | ON | OFF | Replaces `CMAKE_CXX_FLAGS_DEBUG`/`RELEASE` wholesale, discarding the preset's flags and Debug's iterator checking. |
| `USE_STATIC_MSVC_RUNTIME_LIBRARY` | ON | OFF | Builds Jolt against `/MT` while the engine is `/MD`: two C runtimes, two heaps, `LNK2038`. |
| `CPP_RTTI_ENABLED` | OFF | **ON** | Compiles Jolt `-fno-rtti`. Our `JobSystemWithBarrier` subclass is compiled with RTTI, and under the Itanium ABI a polymorphic base with no typeinfo symbol is an undefined reference at link time — the exact case AGENTS.md's "RTTI stays on everywhere" rule exists for. |
| `USE_AVX2`, `USE_AVX`, `USE_LZCNT`, `USE_TZCNT`, `USE_F16C` | ON | **`ENGINE_CPU_BASELINE`** | `target_compile_options(Jolt PUBLIC /arch:AVX2)` — **PUBLIC**, so whatever Jolt decides reaches every target that links physics. So it does not decide: these follow the tree's own baseline ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md)), which is on at `v3` and off at `v2`. `USE_SSE4_1`/`USE_SSE4_2` stay on at both. |
| `USE_AVX512` | ON | OFF | AVX-512 is never a baseline ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md)): it is dispatch-only, and Jolt has no dispatch. |
| `USE_FMADD` | ON | OFF | Contraction is what `CROSS_PLATFORM_DETERMINISTIC` costs, and Jolt ignores this option when that one is on. It stays off at every baseline rather than following it, because a switch that reads as if it did something and does not is worse than one that is off. |
| `INTERPROCEDURAL_OPTIMIZATION` | ON | OFF | LTO on one static library in a tree that does not use it produces objects the other half's archiver cannot read. |
| `ENABLE_ALL_WARNINGS` | ON | OFF | Judges Jolt's code with `-Wall -Werror`. Its headers are SYSTEM here; its sources are not ours to fix. |
| `GENERATE_DEBUG_SYMBOLS` | ON | OFF | Adds a second `/Zi` on top of the preset's and an `/DEBUG` exe-linker flag in Jolt's directory scope. |
| `FLOATING_POINT_EXCEPTIONS_ENABLED` | ON | OFF | Unmasks FP exceptions during the step *and* switches on `Vec3`'s "keep W equal to Z" bookkeeping in every vector operation. |
| `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE`, `PROFILER_IN_DEBUG_AND_RELEASE` | ON | OFF | Compile a debug renderer and a profiler into every build that is not "Distribution". The engine draws with its own renderer and profiles with Tracy. |
| `USE_ASSERTS` | OFF | ON in Debug | Jolt ships with asserts off everywhere, including the configuration developers actually run. |
| `JPH_USE_DX12`, `JPH_USE_VK`, `JPH_USE_MTL`, `JPH_USE_CPU_COMPUTE` | ON | OFF | Jolt 5.6 ships a GPU hair solver whose shaders are compiled with `dxc` at build time, and the Vulkan path adds `find_package(Vulkan)`. Requiring the Vulkan SDK to build physics would break every machine without one. |
| `ENABLE_OBJECT_STREAM` | ON | OFF | Compiles Jolt's text/binary object stream for its own sample assets. The engine serializes through schemas ([ADR-0007](../adr/0007-schema-code-generation.md)). |

Two things bit hard enough to be worth naming:

- **`/Ob1`.** With `OVERRIDE_CXX_FLAGS` off, Jolt is compiled with the preset's flags, and CMake's
  `RelWithDebInfo` uses `/Ob1` — inline only what is marked `inline`. Jolt's hot loops are built
  out of small unmarked helpers, and the difference is not marginal: a step of the 1000-box bench
  with no job system cost **6.70 ms with `/Ob1` and 2.43 ms with `/Ob2`**, and the soft cube went
  from 7.9 ms to 0.94 ms. The fix is `/Ob2` on the `Jolt` target alone, in `RelWithDebInfo`; the
  preset is left as it is for the rest of the tree.
- **Include order.** `<Jolt/Jolt.h>` must precede every other Jolt header (it selects the SIMD
  width and defines `JPH_NAMESPACE_BEGIN`), and the engine's clang-format sorts includes inside a
  block, where `Jolt/Core/...` comes before `Jolt/Jolt.h`. The rule therefore lives in one header,
  `src/jolt.h`, which everything under `src/` includes first, instead of in a comment that the
  next `tools/dev.ps1 format` would quietly undo.

Also worth knowing: shape geometry inside Jolt is stored relative to the shape's centre of mass,
so `Shape::GetLocalBounds()` on a hull built from points in [0, 1] reports [-0.25, 0.75].
`World::shape_bounds` adds the centre of mass back, so bounds come out in the frame the caller
gave the points in.

## Memory, and the one limit that is not a limit

`WorldOptions` carries the hard caps ([ADR-0017](../adr/0017-no-hidden-limits.md): the limits that
exist are visible, in one struct). The per-step scratch arena is derived from them by
`temp_allocator_size_for()` — the dominant term is one 480-byte Jolt `ContactConstraint` per
configured contact constraint — because it is a number nobody can guess and getting it wrong used
to abort the process: Jolt's plain `TempAllocatorImpl` asserts on overflow. The module uses
`TempAllocatorImplWithMallocFallback` instead, so an underestimate costs an allocation in the
middle of a step rather than a crash. `temp_allocator_bytes` of 0 means "derive it"; anything else
overrides.

## The job adapter, and why the step drains the queue

Jolt's jobs run on `core/jobs`' performance pool through `src/job_adapter.h`, a
`JPH::JobSystemWithBarrier` subclass of about sixty lines. Jolt asks for four things — allocate a
job, free a job, queue one or many, and say how many ways it may split its work — and the
interesting part, the barrier, is `JobSystemWithBarrier`'s: the barrier owns the completion
semaphore, executes ready jobs on the thread that is waiting, and is the only place that blocks.
So no pool worker ever blocks; the thread that called `step()` is the one that waits. The
alternative was Jolt's own `JobSystemThreadPool`, which would stand a second set of threads
beside the engine's pinned pools and let the OS decide which of the two gets a P-core
([11 §11.5](../plan/11-performance-principles.md)).

**A backend job lives in a fixed-size free list, and two things hold references to it.** The
queue holds one from `QueueJob` until the job has run, and the barrier holds one until
`WaitForJobs` retires it. That is Jolt's design and the adapter follows it. What Jolt's own
thread pool also does, and what the adapter missed, is *empty the queue*: because the barrier
executes ready jobs on the waiting thread, a job is routinely finished while a queue entry for
it is still outstanding, and that entry keeps the `Job` object alive. `JobSystemThreadPool`
sweeps its own queue when it stops its threads; this adapter's queue belongs to
`jobs::JobSystem`, which outlives the world, so nothing ever swept it.

The consequence was not a slow leak but a **machine-dependent** one. The live-job count became a
function of how fast the pool drained relative to how fast steps were queued, and the stepping
thread does much of a step's work itself inside the barrier wait, so it finishes steps faster
than an oversubscribed pool retires the leftovers. On an 18-core box the backlog never grew; on
a four-vCPU CI runner it exhausted the 2,048-job list inside half a second (`assertion failed:
index != cInvalidObjectIndex`) and, when it did not, left the free list short at shutdown
(Jolt's `mNumFreeObjects == mNumPages * mPageSize` assert, which is what the Debug bench smoke
run tripped on). In Release the second one is worse than an assert: a leftover job runs against
a deleted adapter.

**So `World::step` drains.** The adapter counts the jobs it hands to the pool on a
`jobs::Counter`, and `drain()` is one `JobSystem::wait` on it, called after
`PhysicsSystem::Update` returns and again from the adapter's destructor. `wait` from the
stepping thread *helps* the pool rather than spinning on it, so the drain pops the leftovers
itself instead of waiting for a worker to get round to them, and the jobs it waits for have
already run — the wait is microseconds. The live-job count is now bounded by one step's worth,
which is what the free list is sized for. `WorldStats::backend_jobs_pending` is that counter, and
it exists so that "a step leaves nothing behind" is a number a test reads rather than an outcome
it hopes for.

**Reproducing it takes taking the race away.** The regression test holds the pool's only worker
in a spin job for the duration of the steps, so every entry a step queues is guaranteed to still
be there when `Update` returns. Without the drain that is an immediate, machine-independent
failure — 17 jobs outstanding after one step of the 64-body scene at a concurrency of 1, and
Jolt's free-list assert on the way out. On an 18-core box neither the exhaustion nor the
shutdown assert reproduces at all, with or without four-core affinity, which is precisely why
this reached CI. A second, looser case runs 300 steps through a deliberately small pool at 1, 2,
and 4 workers, which is the shape the failure actually took.

One corollary: a job system with **no** performance workers is now treated as no job system at
all. Before the drain, such a world limped along because the barrier ran everything on the
stepping thread; with the drain it would wait forever for a queue nothing can empty. `init`
substitutes inline execution and the behaviour is the documented one.

The lesson generalizes past this module: **a fixed-size pool whose occupancy depends on how fast
another component drains is not a pool with a limit, it is a race with a limit.** Bound the
occupancy at a point you control — here, the end of the step — rather than sizing for the worst
scheduling you have happened to see.

`WorldOptions::max_backend_jobs` makes the size visible in the options struct with the other
caps ([ADR-0017](../adr/0017-no-hidden-limits.md)); 0 means the backend's own worst case
(`JPH::cMaxPhysicsJobs`, 2,048) and is what every caller should use. It exists as an option so
that the regression test can make the pool small enough — 256 — for exhaustion to be reachable
in 300 steps, at 1, 2 and 4 workers. A step needs roughly `10 + 8 * worker_count` jobs, so a
pool below that is not a small pool but a broken one, and running out is a hard failure rather
than Jolt's own sleep-and-retry: it means the world was configured with fewer jobs than the
backend needs, which is a configuration bug and not something to spin on.

## Soft bodies, and how they map onto ADR-0026

`SoftBodyDesc` is the *backend surface* that ADR-0026's `DeformableVolume` is expected to sit on,
not the abstraction itself. A cage arrives as particles plus constraints, with no statement about
where it came from, how it is bound to a render mesh, or which LOD tier it is running at:

| ADR-0026 concept | Here |
|---|---|
| cage topology | `vertices`, `edges`, `volumes`, `faces` |
| material parameters (stiffness, damping) | per-constraint `compliance`, per-body `linear_damping`, `friction`, `restitution` |
| attachment: free, bone, rigid body, static collider | `SoftAttachment{vertex, body, local_point, kind, follow_rate}`; a null `body` is a fixed world point |
| closed volume with pressure | `faces` plus `pressure` (refused without a surface to measure over) |
| per-tick solve against a contact set | `World::step`, with `iterations` as the cost knob the tier table turns |
| particle-and-shell cage (v1 kind) | `build_cloth_sheet` |
| adaptive lattice (v1 default kind) | `build_lattice_volume` — the generated version differs only in element size |

The word "compliance" is the only XPBD-shaped thing in the interface, and every position-based or
finite-element solver can honour it (0 is a hard constraint; larger is softer, in metres per
newton).

### The two attachment kinds, and why there are two

Attachment is implemented without a Jolt constraint kind at all: the step writes the attached
particle's velocity and nothing else, so a new kind is a new formula in `World::step` rather than
a new backend feature.

| Kind | Inverse mass | Velocity each step | What it is |
|---|---|---|---|
| `Rigid` | forced to 0 | covers the whole gap to the anchor | the particle form of `MoveKinematic` |
| `Spring` | the caller's | steered `clamp(follow_rate * dt, 0, 1)` of the way towards that | a stiff spring, not a weld |

`Rigid` was the only kind at first, and it is the right one for a cage element that is *inside*
the rigid structure it hangs on — a lattice cell that falls inside a bone must not be pushable
out of the bone. It is the wrong one for everything else, for a reason [05 §5.14](../plan/05-simulation.md#514-deformable-volumes)
states outright: "'bound' is a stiff spring rather than a weld, so flesh lags a fast bone instead
of tracking it exactly". A zero-inverse-mass particle does not lag, does not carry momentum, and
cannot be pushed by contact — which also means that "no element passes through the core", E19's
own pass criterion, would have been true by construction rather than measured. That is what made
the second kind necessary rather than merely nice, and E19's fixture uses both: `Rigid` for the
cage vertices inside a bone capsule, `Spring` for the shell just outside it.

`Spring` steers the velocity rather than adding a force to it, so it cannot ring the way an
accumulating spring would: at `follow_rate * dt >= 1` it degenerates exactly to the `Rigid`
formula with the mass kept, and below that it is a critically damped position servo. A rate is
in inverse seconds because the thing being chosen is "how fast does this close the gap", which is
what a content-side stiffness in kPa ([07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets))
has to be converted into anyway; converting it is the deformation system's job, a level up.

Two properties are worth knowing before reading a lag number. A `Rigid` attachment on a
constant-velocity anchor sits **exactly one step of travel behind it**, because the velocity is
computed from where the anchor was when the step began and the anchor moves during that same
step; that is `move_kinematic`'s own behaviour and not a defect. And a `Spring` attachment's lag
is *not* monotone in `follow_rate` once the cage's inertia is in play — a gentler servo keeps
more of the particle's previous velocity, which in steady state is closer to the anchor's than a
"cover the whole gap now" correction the solver then partly undoes. Pick a rate by what the
motion looks like, not by assuming stiffer tracks better.

**Deliberately not here, because ADR-0026 puts it a level up:** cage generation from a signed
distance field, layers and their boundary behaviour, adhesion, damage as constraint edits,
simulation LOD, strain output, and the binding to render vertices. **Not here because the backend
does not do it yet:** soft-against-soft contact (deferred by ADR-0026 — the pair count is
quadratic in the interacting set), skinning a cage to a skeleton (`SoftBodySharedSettings`'s
skinned constraints), per-region materials, and soft-body contact events (the rigid contact
listener does not see them).

### How wide one cage's solve can go, and why it is not the worker count

`SoftBodySharedSettings::Optimize()` runs when a soft body is created, and it is not optional:
the backend asserts on settings with no update groups, and without it the whole cage would be one
serial group. What it does is partition the cage's **vertices** greedily and spatially into
batches of at most `SoftBodyUpdateContext::cVertexConstraintBatch` — 256 — and put each batch's
constraints in its own update group, with the constraints that straddle two batches in a single
trailing group. The solve then takes one group per thread and runs the trailing group once the
parallel ones are done.

So **the width of a cage's constraint solve is `ceil(vertices / 256)`, a property of the cage,
and adding workers past that buys nothing.** A 512-particle cage is a two-wide solve on a
36-thread machine. That number is the one thing to know before sizing a cage, and it is the
reason [E19](../experiments/e19-lattice-cage.md) recommends more small cages over one large one;
`soft_body_solve_width(vertex_count)` reports it, and the batch size is mirrored in
`soft_body.h` and pinned to Jolt's by a static assertion in `src/soft_body.cpp`, because the
group array itself is private to the backend and a mirror that drifted would be worse than no
mirror at all.

Two consequences that are easy to get backwards:

- **The backend's solve jobs are not per cage.** `PhysicsSystem` creates `GetMaxConcurrency()`
  of them and each takes the next available constraint group from *any* active cage, so eight
  one-batch cages fill eight threads where one eight-batch cage would not. `Optimize` is what
  makes that possible and the reason it is called at creation rather than lazily.
- **A worker with nothing to claim spins.** A solve job that finds no group yields and retries
  until the step's iterations are done, so workers past the width are not idle, they are busy
  doing nothing. On a pinned pool that is worse than not asking for them.

`WorldStats::soft_body_solve_jobs` and `soft_body_solve_workers` are what turn "does it actually
spread?" into a number: the job adapter tags the backend job that runs the constraint solve
(matched by its name, since Jolt keeps job names only with its profiler compiled in) and records
the set of performance workers that have executed one. The parallelism test asserts the count is
non-zero, which is also what pins the job name against a future Jolt renaming it.

Vertices are never reordered by `Optimize` — Jolt says so where it sorts the constraints, that
reordering vertices "would be much more of a burden to the end user" — which is why attachment
indices, face indices, and `read_soft_body_vertices` indices all stay valid across it.

`build_lattice_volume` uses the six-tetrahedron Kuhn decomposition — the one Jolt's own cube
fixture uses — rather than the five-tetrahedron alternative. The five-tetrahedron split has to be
mirrored in every other cell to stay conformal, and a cell whose orientation is wrong inverts
under compression and turns the cage inside out instead of pushing it back; that failure took a
while to recognise because it looks like a solver explosion rather than a topology bug.

### `iterations` is a sub-step count, not an iteration count

`SoftBodyDesc::iterations` is Jolt's `mNumIterations`, and Jolt's soft body **integrates positions
inside each one**: it applies gravity and damping, moves every particle by `v * dt_sub`, solves
the constraints, and closes with `v = (x - x_prev) / dt_sub`, where
`dt_sub = collision step dt / iterations`. So it is an XPBD sub-step, not a Gauss-Seidel sweep,
and `World::step(dt, sub_steps)`'s own `sub_steps` multiplies it: eight iterations at two
sub-steps is **sixteen position solves a tick**, at a sub-step of 1/960 s.

The name is Jolt's and this module keeps it, but three things follow that the name hides, and all
three cost time to rediscover:

- **A constraint's compliance is divided by `dt_sub²`, not by the step's `dt²`**, so raising
  `iterations` makes every constraint *effectively stiffer per solve* rather than merely
  better-converged. That is why E19 measured raising iterations turning recoveries into failures
  while raising sub-steps did not, and why 16 iterations at 2 sub-steps diverges with a compliance
  that 8 at 2 is comfortable with ([E19](../experiments/e19-lattice-cage.md)).
- **Anything the engine does to a cage per `World::step` is once per `iterations × sub_steps`
  solves**, which is the trap the strain clamp's velocity correction fell into (below).
- **Cost is linear in it**, exactly, which is what makes E19's cost model one multiplication.

### The strain clamp, and why it is not a constraint

[ADR-0029](../adr/0029-deformable-volume-budgets.md) decision 4 makes `SoftBodyDesc::max_strain`
— [plan 07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)'s
`limits.max_strain`, which that table has always described as "a per-element clamp the solver
never exceeds" — into something that is actually enforced. Before it, E19 measured held stretch of
1.76 to 2.41 against an authored limit of 1.5, a transient peak of 2.81, three cages taking a
permanent set at two thirds of their volume, and one diverging outright.

**It is a clamp and not a constraint, and that is the whole design.** A constraint is a thing the
solver negotiates with compliance; the cage already has one of those per edge and E19 shows it
losing the argument with a kinematic plate by a factor of two. A limit is a thing that is true
when the step is over. So it runs in `World::step`, after `PhysicsSystem::Update` has returned, on
the stepping thread, over the backend's own `mEdgeConstraints` — the backend's rest lengths rather
than a copy, because a copy is 16 bytes an edge of duplicate state that can drift from what the
solver actually used.

Four properties, each of which was measured rather than assumed, and each of which went against
the first guess at least once:

| Property | Why |
|---|---|
| **Symmetric in strain.** A limit of 0.5 means a length between half and one and a half times rest. | The compression half is the one that stops a tetrahedron going inside out, which is the mechanism behind every failure E19 recorded. Measured against a stretch-only clamp on the whole press grid: stretch-only is cheaper (2–3 sweeps against 7–15) and recovers the same eighteen of twenty configurations, but it lets **1 to 21** free particles through the rigid core against the symmetric clamp's **0 to 5**, and that is a pass criterion while sweep count is not. |
| **Position-only, velocity-consistent at the *step's* dt.** `v += dx / dt`, not `dx / dt_sub`. | Matching the solver's own per-sub-step velocity identity looks right and is wrong: the correction is up to 32 times larger, overshoots, and 343 elements at 16 iterations and 2 sub-steps goes from recovering to diverging. The clamp is not inside a sub-step — it runs once a step and removes what a whole step accumulated — so the step is the interval its correction belongs to. At the step's dt and with no velocity correction at all the grid is indistinguishable; the correction is kept because it is the one that cannot leave the stretch's energy in the velocity. |
| **Sweeps until the worst edge is inside the limit to one part in a thousand**, capped at 16. | Projecting one edge moves the particles its neighbours share (a lattice particle carries eighteen edges), so one sweep is not enough. Exiting on "a sweep corrected nothing" never fires under load — the solver re-stretches what the last sweep pulled in, forever — and burned all sixteen sweeps on every hold tick of E19's press, 370 µs a tick at 512 elements. The tolerance is the number the module's strain test asserts, so the guarantee the code makes and the one the test checks are the same sentence. |
| **A cage that asks for no limit pays nothing.** | No rest lengths are kept and no pass runs ([plan 11 §11.10](../plan/11-performance-principles.md)). |

**Cost: `edges × sweeps × 4.3 ns` a step, and the sweep count is reported.** Under E19's press
that is **17 µs for ADR-0029's 216-element default cage (2 sweeps) and 326 µs for a 512-element
one (14.6 sweeps)** — 4% and 27% of their respective ticks, which is a second and independent
reason for the small default. A settled cage exits on its first sweep and costs nothing measurable
(E19's 20-second sustained load is within 0.3% of the unclamped run).

**What the clamp cannot do.** It keeps a cage inside a press a material could survive; it does not
make a cage survive a press it cannot. A kinematic plate is infinitely heavy, so a
displacement-controlled press driven past what the limit allows leaves the projection with two
demands it cannot satisfy at once, and it is the clamp that loses. Measured on a stiff 64-element
cage: driven to 70% of its height it leaves **0** inverted cells against 63 unclamped, at 60% it
leaves 50, and at 50% and 40% it leaves slightly *more* than no clamp at all.
`SoftBodyBudget::strain_clamp_saturated` is how a caller sees that happening.

### Volume preservation is authored, and the conversion knows the cell size

`volume_compliance_for(preservation, cell_size, stiffness)` in `domain/physics/deformable.h` is
[ADR-0029](../adr/0029-deformable-volume-budgets.md) decision 3. The header carries the
derivation; the reason it exists is E19's least expected result, and it is worth stating plainly
because it will catch the next person too:

> **A volume constraint's compliance is not an edge constraint's.** XPBD weighs compliance against
> the sum of inverse masses times the squared constraint gradient. An edge's gradient is a unit
> vector; a tetrahedron's volume gradient is an **area**. At a 6 cm cell those are four orders of
> magnitude apart, so the same 1e-4 that makes a stiff edge switches the volume constraint off,
> and the cage takes a permanent set at two thirds of its volume with no way back.

The conversion comes from the energy rather than from that ratio, which is what makes it a
material property instead of a tuning constant: the backend's constraint is on six times the
tetrahedron's volume, so equating `C²/2α` with a bulk modulus K's `K(ΔV)²/2V₀` gives
`α = 36·V₀/K`, and for a cubic cell of side h that is `6h³/K`. **Compliance therefore goes as the
cube of the cell size**, and E19's own rule of thumb — "half the element size wants half the
compliance" — is the wrong law; it came from the conditioning ratio rather than from the energy.
The authored 0..1 weight scales the material's own bulk modulus, `K = (E/3)·p/(1−p)`, so p = ½ is
a material with Poisson's ratio 0, p → 1 is incompressible and a hard constraint, and p → 0 has no
volume preservation at all.

The sanity check that says the model is describing reality: E19 found 1e-8 by hand, and at its
5.71 cm cell that is a preservation of 0.63 at plan 07's default 200 kPa stiffness, while its
hand-picked edge compliance of 1e-4 is a Young's modulus of 175 kPa at the same cell. The
hand-tuned fixture and the plan's authored defaults were already the same material; nothing knew
it because nothing had written the conversion down.

### What a tick spends on deformables

`WorldStats::soft_body_budget` is [ADR-0029](../adr/0029-deformable-volume-budgets.md) decision 1:
**wall clock on the performance pool per tick, not a sum of per-worker CPU time**. The distinction
is not pedantry — E19 measured eight cages running 6.45× faster together than one after another,
so a CPU-time sum would price the arrangement the engine wants as if it were the worst one.

Three things go into the number: the world's own attachment pre-pass, the backend's soft-body
phase, and the strain clamp. The middle one is the interesting one to measure, because it happens
on threads this module does not own.

**How the phase is timed.** The job adapter already tags each backend job with its stage
(`src/job_adapter.h`). The window **opens** when the first job of a soft-body stage is queued and
**closes** when the job that kicks the next collision sub-step is queued, or when `Update` returns
for the last one. Those two events bracket the phase because the soft-body jobs are the last thing
in a collision step and it is the soft-body finalize job that removes the next step's last
dependency. **Measuring the queue rather than the execution is the point**: a queue is something
the adapter owns and an execution is not — `JobSystemWithBarrier`'s barrier runs ready jobs on the
stepping thread without passing through the adapter's `run_job`, so a span built from execution
timestamps would silently lose whatever the stepping thread did, and how much that is depends on
the pool. The cost is one relaxed load per job queued and two clock reads per sub-step. Measured
against the whole step on E19's fixture, the window is 96–98% of it, which is the expected answer
for a world whose only body of consequence is a cage.

**How ambient and hero are split.** The total is measured; the split is modelled, and saying which
is which is the honest way to report it. The backend's solve jobs take the next available
constraint group from *any* active cage — that interleaving is exactly why eight cages cost less
together than one costs alone — so there is no per-cage wall clock to be had. Each active cage's
share is therefore proportional to its elements times its iterations, which is E19's cost model
and held to within 5% across its whole grid. A sleeping cage is left out, because the backend does
no work for it.

The report carries `ambient_ms` and `hero_ms`, how far each is past its budget, how many volumes
are flagged `hero` (more than one at a time is an authoring error, not something this module
refuses — refusing would break a tier transition that legitimately overlaps two for a few ticks),
and the strain clamp's sweep count. **The ambient overrun is the actionable number**: the tiers
above this module demote ambient volumes farthest-first until it is zero, and they never demote
the hero to fit a budget. A hero overrun is a content problem and is reported separately so it
cannot invite the wrong response.

The module holds no opinion about tiers and assigns none; it reports the number the tier logic
acts on. Per step and unsmoothed, because smoothing is the tier logic's decision and a number
that has already been smoothed cannot be un-smoothed.

## LOD policy and determinism stance

[ADR-0027](../adr/0027-additive-capabilities.md) asks every capability for both, in writing.

**LOD.** Rigid bodies exist for LOD0 and LOD1 entities only ([05 §5.11](../plan/05-simulation.md#511-integration-notes));
an entity demoted to LOD2 destroys its body and is reconstructed from its projection when it is
promoted, which is why `create_body`/`destroy_body` are cheap and handles are generation-checked
rather than pointers. Debris carries its own cap, which is a residency policy rather than a tier.
Soft bodies take their tiers from [ADR-0026](../adr/0026-deformable-volumes-first-class.md)'s
table (full cage, reduced cage, authored secondary motion, skinning only, none); this module
provides the knob those tiers turn — `iterations`, and the cage the caller hands it — and holds
no opinion about which tier an entity is in. The module never assigns a tier itself: tier
assignment is the deformation system's, above this layer. What it does provide, since
[ADR-0029](../adr/0029-deformable-volume-budgets.md), is the number that decides: the per-step
wall clock the deformable set cost, split between the ambient volumes the tiers may demote and
the one flagged `hero` that they may not, with how far each is past its budget.

**Determinism.** The world is part of the fixed-step sim and enters the sim hash. It reads no
wall clock, sorts its contacts and islands, and produces bit-identical results whatever worker
count it is given, on any compiler, configuration, OS, or architecture (see *Determinism* above).
The GPU is not involved at all: there is no derived GPU state here to read back.

## What is not wrapped yet

Constraints and motors, character controllers (plan 05 §5.11 wants Jolt's, which is its own
wrapper with its own tests), ragdolls, vehicles, sensors and triggers, `SaveState`/`RestoreState`
for rollback ([ADR-0016](../adr/0016-multiplayer-readiness.md)), scaled shape instances,
collision groups and sub-shape filtering, overlap and collide-shape queries (only ray and shape
casts are here), soft-body contact events, and Jolt's GPU hair solver. None of them needs the
public surface to change shape; each is an addition.

**Testing.** `tools/dev.ps1 test -Preset msvc-debug -Filter physics`. Thirty-six cases: a sphere
dropped on a static box comes to rest within the penetration slop and falls asleep; a hundred
boxes in ten towers of ten are still standing after 600 steps, with bounds on sideways drift and
on how far anything sank; a kinematic box pushes a dynamic one and stays behind it; contact
`Begin` is reported exactly once per pair with the normal pointing the documented way and the
user data carried along; the debris pool recycles its oldest piece at the cap; shapes are shared
and refuse to be destroyed underneath a body; a mesh shape refuses to be dynamic; the layer table
is symmetric and `Query` simulates against nothing; ray and shape casts report fraction, body,
position, and normal, and respect a `LayerMask`; a sphere rests at exactly one radius above a
flat heightfield and stays one radius off the surface while it rolls down a sloped one;
heightfields and hulls refuse grids and point sets the backend cannot build; a step leaves no
job of its own in the queue even when the pool's only worker is held busy, three hundred steps
through a deliberately small backend job pool at 1, 2, and 4 workers recycle it many times over
and hand every job back by the time the world is destroyed, and a job system with no performance
workers behaves as none at all (the regression tests for the drain above, and the reason the pool
size is an option); the cloth and
lattice builders produce exactly the constraint counts they promise; a cloth pinned at two
corners sags, settles, and stays finite; a lattice cube pressed to 70% of its height by a
kinematic plate recovers more than 90% of it; a `Rigid` attachment holds its particle against a
hanging cage's own weight and sits exactly one step of travel behind a sweeping anchor while a
`Spring` one sags and lags further, and a `Spring` with no follow rate is refused; the
arithmetic of `soft_body_solve_width` matches the batch rule for the cage sizes E19 sweeps;
eight cages stepped on eight workers put the backend's constraint solve on more than one of
them, a world with no job system puts it on none, and two cages stepped 120 times with 1 and
with 8 workers give bit-identical particle positions; soft-body descriptions are validated
before the backend sees them; 600 steps with 1 and with 8 workers are bit-identical; the job
adapter runs backend jobs on `jobs::JobSystem` workers; and a world with no job system produces
the same answer as one with four.

[ADR-0029](../adr/0029-deformable-volume-budgets.md) adds seven more, in
`tests/deformable_tests.cpp`: the volume-compliance conversion scales as the cube of the cell size
and reproduces E19's hand-tuned working point at E19's cell; the cage size verdicts are the ones
the content validator applies (the tissue definition's `region.cage_size` row reads
`cage_size_verdict()` directly, [tissue](tissue.md)) and the default cage is exactly one backend
solve group; **a cage
pressed to 70% of its height and released recovers to within 1% of its rest volume at three cell
sizes**, each built with the compliance the conversion returns for its own cell; a cage hung under
thirty gravities stretches past the authored limit without the clamp and sits at or under it with
it; **a stiff cage crushed 30% leaves inverted cells without the clamp and none with it** (that
test also records, in a comment, the press depth past which the clamp stops being able to help,
because a kinematic plate always wins); the budget report splits ambient from hero the way the
flags say and its overruns are the budget arithmetic; and a strain limit that is negative or at or
above 1 is refused. The divergence itself is measured where it happened — the E19 press grid, in
`msvc-release` — rather than in a unit test, because a test that waits for a NaN is a test tuned
to one machine's rounding.

A single 100-high tower is *not* in the suite, and not because it was awkward to write: it falls
over inside two seconds. That is a property of sequential-impulse solvers rather than of this
wrapper — the bottom box carries a hundred times its own weight and the residual error at the
base is amplified all the way up — and a test that asserted otherwise would be asserting a bug.
Ten towers of ten is the shape a game builds anyway, and it puts ten islands in front of the
solver instead of one.

The size table pins `BodyId`/`ShapeId`/`SoftBodyId` at 8 bytes, `ContactEvent` at 64 (one cache
line, which is why the user data rides inline instead of being looked up per event), `RayHit`,
`ShapeHit`, and the three cage-element structs.

**The bench.** `bench/physics_bench.cpp` is the step cost of the module's own fixtures; beside it,
`bench/e19_bench.cpp` is [experiment E19](../experiments/e19-lattice-cage.md) — a lattice cage
around a rigid two-bone core pressed to 30% of its depth and released, and the same cage under a
sustained load — which prints its own JSON lines so the whole experiment re-runs on another
machine from one command. Both live in `engine_physics_bench`; a benchmark argument is one
integer, so the E19 sweeps pack their dimensions into a decimal key
(`mode * 1e9 + elements * 1e6 + workers * 1e4 + iterations * 10 + sub_steps`). `mode` is the
control ADR-0029 added: 0 has both of its fixes in, 1 is the fixture exactly as the experiment
first ran, and 2 and 3 turn the strain clamp and the compliance conversion on one at a time. The
failing configurations run in all four **in the same session**, because a re-run on a different
day cannot otherwise tell "the fix did this" from "the machine was quieter this time" — which on
a shared desktop is a real alternative and not a rhetorical one. Under CTest the bench runs with
`--smoke`, and the E19 fixture reads that flag (`bench::smoke_mode()`) rather than the build type:
a four-a-side cage and phases twelve times shorter, because the release grid registers 54
variants and at full length they cost a billed CI runner a minute per run (11 s now).

**Performance notes.** `tools/dev.ps1 bench -Preset msvc-release -Filter "physics.*"`, measured on
an i9-10980XE (18 cores), `RelWithDebInfo`, cross-platform determinism on, **SSE4.2 baseline —
which is what `msvc-release` was until 2026-09-19 and is what `msvc-release-v2` is now**
([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md)). One
machine settles layout and traversal decisions, not cross-machine defaults
([11 §11.8](../plan/11-performance-principles.md)). **This desktop is shared** — the owner runs GPU
jobs on it and other agents compile on it — so a measurement is worth what its record of the
machine's state is worth; the table below was re-taken on 2026-09-18 at a CPU load of 6–10% of 36
threads with the GPU idle, and it agrees with the 2026-09-17 run (in brackets) to within 1%:

**Machine state:** the rigid-body and single-cage rows were re-taken on 2026-09-18 with
`--require-quiet`, twice, on a machine that started and ended both runs below 10% others' CPU
([bench](bench.md#measuring-on-a-shared-machine)). The eight-cage row is the original and was
measured before the harness recorded anything.

| benchmark | 1 worker | 4 workers | 8 workers |
|---|---|---|---|
| 1,000 boxes | 2,367 | 1,070 | 750 |
| 1,000 boxes, no job system | 2,409 | — | — |
| 512-particle soft cube, 4 iterations | 489 | 598 | 641 |
| 512-particle soft cube, 8 iterations | 945 | 1,156 | 1,226 |
| 512-particle soft cube, 16 iterations | 1,857 | 2,277 | 2,401 |
| **8** × 512-particle soft cube, 8 iterations | 7,475 | 2,236 | **1,159** |

**The v3 build moves three of these rows, and not all the same way** ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md),
2026-09-19; the table above is the v2/SSE4.2 measurement and the rest of it has not been re-taken).
The 1,000-box step is **2,396 → 2,234 µs (−6.7%) at one worker** and **731 → 781 µs (+6.8%) at
eight**, which is not a contradiction but Intel's AVX frequency licensing on this part: one core
running 256-bit code boosts and eighteen do not. [E19](../experiments/e19-lattice-cage.md)'s
default 216-element cage tick goes **428 → 484 µs (+13.1%)**, and the 512-particle soft cube is
within noise (+1.7%). Jolt's own `USE_AVX2`/`USE_LZCNT`/`USE_TZCNT`/`USE_F16C` are what changed
with the baseline; `CROSS_PLATFORM_DETERMINISTIC` and `USE_FMADD` did not, so none of this changes
a simulation result.

Every row here but one is within 2% of the figure first recorded on this page, which is the
answer to "were those numbers taken on a loaded machine?" for the rigid-body path and for eight
of the nine cage variants: no, or at least not enough to matter. **The exception is the one-worker,
four-iteration cage, which was recorded as 878 µs and measures 489** — two quiet runs three
minutes apart agreeing to 0.6%, against eight neighbouring cells that reproduce. A single
outlying cell in an otherwise reproducing table is what a measurement taken beside somebody
else's work looks like, and it is the cell that carried the only apparent *speedup* the
single-cage rows had.

Microseconds per step. The box benchmark measures a *settled* pile with sleeping switched off: a
pile that is allowed to sleep costs nothing after a second and would make the number a measure of
the sleep heuristic. Scaling from one to eight workers is 3.2x, which is what a contact solver
that has to sort its islands for determinism looks like — the 1-worker and no-job-system numbers
being equal says the adapter itself, and the drain it does at the end of every step, cost nothing
measurable.

**The soft-body rows are the interesting ones, and they go the wrong way.** One cage is *slower*
on eight workers than on one **at every iteration count** — 489 → 641, 945 → 1,226, 1,857 → 2,401
— because a 512-particle cage is a two-wide solve (above) and the other six solve jobs spin
rather than help. (Until the 2026-09-18 re-measurement the four-iteration row appeared to gain
from workers; it was the one cell that did not reproduce, and the corrected number makes the
conclusion uniform rather than qualified.) Eight cages are
**6.45× faster** on eight workers than on one, and eight of them together cost **less than one of
them does** on the same pool — 145 µs a cage against 1,226. So the way to spend a pool on
deformables is *more cages*, not *bigger* ones, and a deformation system has to step its volumes
in one world for the backend to be able to interleave them at all.

Against the budget: E19 measures the T0 cage at **0.225 µs per element per iteration per
sub-step** — twice, five weeks apart, agreeing to 3% — so the 512-element cage at eight iterations
and two sub-steps costs **1.8 ms, more than ADR-0026's whole 1.5 ms budget for one volume**,
against the 40–120 µs plan 05 §5.14 estimated. That is what
[ADR-0029](../adr/0029-deformable-volume-budgets.md) acted on: the default T0 cage is now **at
most 256 elements** — one solve group — which costs **0.80 ms at one worker and 1.37 ms at eight**
at the same iteration and sub-step counts, and the budget is **1.5 ms ambient plus a 2.5 ms hero
allowance**, wall clock, reported per step in `WorldStats::soft_body_budget`. The strain clamp is
a further `edges × sweeps × 4.3 ns` — 17 µs on the default cage, 326 µs on a 512-element one. The
full grid, what it decides, and what still fails is in [E19](../experiments/e19-lattice-cage.md).

Other hot-path decisions: contact recording is lock-free (a per-worker bucket, merged and sorted
once per step); `read_transforms` takes one pass over the caller's id array against the no-lock
body interface, so the renderer does not pay a lock per body; shapes are shared by handle with a
reference count, so a thousand instances of a mesh hold one copy of its triangles; and the debris
pool is a ring of handles, so recycling is one destroy at the head rather than a scan.
