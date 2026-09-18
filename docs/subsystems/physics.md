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
- Two worlds built by the same sequence of calls and stepped the same number of times produce
  bit-identical transforms, whatever worker count each was given.

**Public API.** `domain/physics/types.h`: `Status`/`status_name`, `Layer`/`layer_name`/
`layers_collide`/`LayerMask`, `MotionType`, and the three handle types.
`domain/physics/soft_body.h`: `SoftEdge`, `SoftVolumeConstraint`, `SoftAttachment`,
`SoftBodyDesc`, and the two cage builders `build_cloth_sheet` and `build_lattice_volume` with
their `ClothSheet` and `LatticeVolume` results. `domain/physics/physics.h`: `WorldOptions`,
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
| `USE_AVX2`, `USE_AVX`, `USE_LZCNT`, `USE_TZCNT`, `USE_F16C` | ON | OFF | `target_compile_options(Jolt PUBLIC /arch:AVX2)` — **PUBLIC**, so it reaches every target that links physics, and the resulting binary crashes on the baseline machine ([self-hosted runners](../ci/self-hosted-runners.md): i7-980, Westmere, SSE4.2, no AVX). `USE_SSE4_1`/`USE_SSE4_2` stay on. |
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

## Soft bodies, and how they map onto ADR-0026

`SoftBodyDesc` is the *backend surface* that ADR-0026's `DeformableVolume` is expected to sit on,
not the abstraction itself. A cage arrives as particles plus constraints, with no statement about
where it came from, how it is bound to a render mesh, or which LOD tier it is running at:

| ADR-0026 concept | Here |
|---|---|
| cage topology | `vertices`, `edges`, `volumes`, `faces` |
| material parameters (stiffness, damping) | per-constraint `compliance`, per-body `linear_damping`, `friction`, `restitution` |
| attachment: free, bone, rigid body, static collider | `SoftAttachment{vertex, body, local_point}`; a null `body` is a fixed world point |
| closed volume with pressure | `faces` plus `pressure` (refused without a surface to measure over) |
| per-tick solve against a contact set | `World::step`, with `iterations` as the cost knob the tier table turns |
| particle-and-shell cage (v1 kind) | `build_cloth_sheet` |
| adaptive lattice (v1 default kind) | `build_lattice_volume` — the generated version differs only in element size |

The word "compliance" is the only XPBD-shaped thing in the interface, and every position-based or
finite-element solver can honour it (0 is a hard constraint; larger is softer, in metres per
newton). Attachment is implemented without a Jolt constraint kind at all: an attached particle
gets zero inverse mass, and each step the world sets its velocity to cover the gap to its anchor
in one step — the particle form of `MoveKinematic`. A springy attachment is a later addition to
that struct, not a different mechanism.

**Deliberately not here, because ADR-0026 puts it a level up:** cage generation from a signed
distance field, layers and their boundary behaviour, adhesion, damage as constraint edits,
simulation LOD, strain output, and the binding to render vertices. **Not here because the backend
does not do it yet:** soft-against-soft contact (deferred by ADR-0026 — the pair count is
quadratic in the interacting set), skinning a cage to a skeleton (`SoftBodySharedSettings`'s
skinned constraints), per-region materials, and soft-body contact events (the rigid contact
listener does not see them).

`build_lattice_volume` uses the six-tetrahedron Kuhn decomposition — the one Jolt's own cube
fixture uses — rather than the five-tetrahedron alternative. The five-tetrahedron split has to be
mirrored in every other cell to stay conformal, and a cell whose orientation is wrong inverts
under compression and turns the cage inside out instead of pushing it back; that failure took a
while to recognise because it looks like a solver explosion rather than a topology bug.

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
assignment is the deformation system's, above this layer.

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

**Testing.** `tools/dev.ps1 test -Preset msvc-debug -Filter physics`. Twenty-one cases: a sphere
dropped on a static box comes to rest within the penetration slop and falls asleep; a hundred
boxes in ten towers of ten are still standing after 600 steps, with bounds on sideways drift and
on how far anything sank; a kinematic box pushes a dynamic one and stays behind it; contact
`Begin` is reported exactly once per pair with the normal pointing the documented way and the
user data carried along; the debris pool recycles its oldest piece at the cap; shapes are shared
and refuse to be destroyed underneath a body; a mesh shape refuses to be dynamic; the layer table
is symmetric and `Query` simulates against nothing; ray and shape casts report fraction, body,
position, and normal, and respect a `LayerMask`; a sphere rests at exactly one radius above a
flat heightfield and stays one radius off the surface while it rolls down a sloped one;
heightfields and hulls refuse grids and point sets the backend cannot build; the cloth and
lattice builders produce exactly the constraint counts they promise; a cloth pinned at two
corners sags, settles, and stays finite; a lattice cube pressed to 70% of its height by a
kinematic plate recovers more than 90% of it; soft-body descriptions are validated before the
backend sees them; 600 steps with 1 and with 8 workers are bit-identical; the job adapter runs
backend jobs on `jobs::JobSystem` workers; and a world with no job system produces the same
answer as one with four.

A single 100-high tower is *not* in the suite, and not because it was awkward to write: it falls
over inside two seconds. That is a property of sequential-impulse solvers rather than of this
wrapper — the bottom box carries a hundred times its own weight and the residual error at the
base is amplified all the way up — and a test that asserted otherwise would be asserting a bug.
Ten towers of ten is the shape a game builds anyway, and it puts ten islands in front of the
solver instead of one.

The size table pins `BodyId`/`ShapeId`/`SoftBodyId` at 8 bytes, `ContactEvent` at 64 (one cache
line, which is why the user data rides inline instead of being looked up per event), `RayHit`,
`ShapeHit`, and the three cage-element structs.

**Performance notes.** `tools/dev.ps1 bench -Preset msvc-release -Filter "physics.*"`, measured on
an i9-10980XE (18 cores), `RelWithDebInfo`, cross-platform determinism on, SSE4.2 baseline. One
machine settles layout and traversal decisions, not cross-machine defaults
([11 §11.8](../plan/11-performance-principles.md)):

| benchmark | µs per step |
|---|---|
| 1,000 boxes, 1 worker | 2,420 |
| 1,000 boxes, 4 workers | 1,090 |
| 1,000 boxes, 8 workers | 752 |
| 1,000 boxes, no job system | 2,430 |
| 512-particle soft cube, 4 iterations | 481 |
| 512-particle soft cube, 8 iterations | 942 |

The box benchmark measures a *settled* pile with sleeping switched off: a pile that is allowed to
sleep costs nothing after a second and would make the number a measure of the sleep heuristic.
Scaling from one to eight workers is 3.2x, which is what a contact solver that has to sort its
islands for determinism looks like — the 1-worker and no-job-system numbers being equal says the
adapter itself costs nothing measurable.

The soft cube is linear in iterations, as XPBD should be, and it is the number to watch: ADR-0026
budgets **1.5 ms per 60 Hz tick for all deformable volumes together**, and one 512-particle cage
at eight iterations already spends 0.94 ms of it. Either the T0 cages of that budget are much
smaller than 512 elements, or the budget needs the parallel constraint groups that
`SoftBodySharedSettings::Optimize` sets up and this module does not yet hand to the job system.
That is E19's question, and this is the first measurement it has.

Other hot-path decisions: contact recording is lock-free (a per-worker bucket, merged and sorted
once per step); `read_transforms` takes one pass over the caller's id array against the no-lock
body interface, so the renderer does not pay a lock per body; shapes are shared by handle with a
reference count, so a thousand instances of a mesh hold one copy of its triangles; and the debris
pool is a ring of handles, so recycling is one destroy at the head rather than a scan.
