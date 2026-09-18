# animation (systems)

**Purpose.** The capability that plays clips. [anim](anim.md) is the data half — skeletons, poses, clips, the blends over a pose, 3×4 skinning matrices — and nothing ticks it; this module is the tick. It owns a playhead per entity, an SoA **pose pool**, and the bone-matrix buffer a skinned instance uploads, and it is the first capability in this engine built end to end through [ADR-0027](../adr/0027-additive-capabilities.md)'s contract: a schema file, three `sim::SystemDesc`s registered with `ecs::register_system`, an LOD policy over `sim`'s tiers, a docs page, tests, a size table and a bench, in one directory, behind one switch.

What it deliberately does not do: **draw**. The renderer contract is a span and two offsets ("The renderer contract" below), and filling `gfx::DeformDesc` from it is the renderer's change — which has now been made, so a character imported through [assets](assets.md), ticked here, and skinned in `deform.slang` is one command line ([apps](apps.md), `engine-view --animate`). Also absent and named rather than implied: blend trees and state machines as data, IK, motion matching ([05 §5.11](../plan/05-simulation.md#511-integration-notes)).

**Why this shape.**

*The pose is not a component, and that is the whole design.* [03 §3.4](../plan/03-data-model.md#34-the-runtime-world)'s hedge — "hot systems own their own data; the ECS holds identity, relationships and gameplay components" — is usually quoted about physics. Animation is the cleaner case, because a pose fails every test for being a component at once. It is *variable length*, so as a component it is either a fixed-width worst case (a 23-joint character paying for a 100-joint one) or three `Vector`s per entity — three heap allocations and three pointer chases per instance per tick, which is the layout [anim](anim.md) spends its first page arguing against. It is *private*: nothing outside this capability reads a pose, and putting it in the world would show it to the protocol, to a persistence walk and to every query that touches the archetype for nobody's benefit. And it is *recomputable* from `(skeleton, clip, time)`, so putting it in the world would make the world carry a cache. So the ECS holds a `u32` slot and the capability holds the arena.

*The arena is one set of channel arrays with every instance's joints laid end to end*, plus one array of `anim::JointMatrix` with the same slot geometry. Sampling an instance walks four contiguous runs; the matrices of *every* instance are one span the renderer can upload whole. There is no per-slot stride, so there is no waste: the arena is exactly the sum of the live skeletons' joint counts. Free lists are per skeleton, so a released slot is reused by an instance of the same skeleton and its run stays the right length, and reuse is LIFO — which is what makes slot assignment a deterministic function of the acquire/release sequence, and what the bit-identical tests rest on.

*The playhead is `Id128`-addressed but index-resolved.* `AnimationPlayer::clip` is an `Id128` because that is what a save holds ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 3), and a hash lookup is the wrong thing to do per instance per tick. `advance_players` resolves the handle into `SkeletonInstance::clip_index` and checks it with a 16-byte compare against the library's own record, so only a clip that actually changed pays for a lookup — including one set from JSON through `WorldCommands`, which arrives with no cached index and is resolved on the tick it changed.

**Owned data.** The pose pool (`PosePool`: three channel arrays, the matrix array, the slot table and the per-skeleton free lists), the clip library (`Library`: `anim::Skeleton`s and `anim::Clip`s loaded from glTF, their `Id128`s and their standard-skeleton mappings), and the three components' *meaning*. Nothing else may hold a pose; nothing else may hand out a pool slot.

## The components, and what a save contains

Declared once, in `systems/animation/schemas/animation.schema`, as `@kind(component)` structs ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 1). The split between them *is* this capability's answer to "what does a save need to resume the same pose".

| Component | Persisted? | Holds |
|---|---|---|
| `AnimationPlayer` | **yes** | `clip`, `time`, `speed`, `weight`, `looping`, and the cross-fade: `fade_clip`, `fade_time`, `fade_elapsed`, `fade_seconds`, `fade_looping` |
| `SkeletonInstance` | no (`@transient`) | `skeleton`, `pose_slot`, `joints`, and the resolved `skeleton_index`/`clip_index`/`fade_index` |
| `AnimationLod` | no (`@transient`) | `tier`, `divisor`, `phase`, `interpolate`, `skin`, `frozen_at_us` |

**Why exactly those fields are the persistent ones.** A pose is a pure function of `(skeleton, clip, time)`, and of `(fade_clip, fade_time, fade_elapsed/fade_seconds)` as well while a cross-fade is in flight. Save those and the pose comes back; save the pose instead and you have saved a cache that the clip library can regenerate and that a re-authored clip would make wrong. `speed`, `looping` and `weight` are there because they are inputs to the *next* tick's time, not because they are pose state.

**Why the skeleton reference is not persistent** even though it is an asset id. Which skeleton an entity rides is a property of the model it materializes from, exactly like its mesh: the entity's asset brings it back. Saving it here would give one fact two owners, and the two would disagree the first time a character's model changed. The pool slot is a run-time allocation that means nothing in the next process, and `AnimationLod` is derived from the tier, which is derived from where the cameras are this session.

**What this does to the archetype set** ([ADR-0028](../adr/0028-ecs-and-persistent-store.md)'s guard rail): three components and **no relationship**, so an animated entity adds at most one archetype per distinct combination it appears in, and nothing here multiplies tables as a cross product. The tier is a *field*, not a relationship — see "What the contract could not express" for what that costs and when it should change.

## The pose pool

`PosePool::acquire(skeleton, joints)` hands out a slot; `release` gives it back. `pose(slot)` is an `anim::PoseView` over the slot's run and `matrices(slot)` its bone matrices; `upload_buffer()` is all of them.

**When the arrays may move.** `acquire` may reallocate, so no view or span from the pool survives one. The capability acquires and releases only from tier transitions, which happen outside the parallel systems phase, so within a tick the arena is stable and every instance writes only its own run — which is also what makes the sampling systems' writes safe at any worker count.

**Growth is geometric, and it is the container's job now.** A pool that appends one skeleton's worth of joints and resizes to the new total used to reallocate and copy on every acquire, because `Vector::resize` took the size exactly: O(n²) in the population, and not "slow" so much as "never finishes" — filling 65,536 slots that way spun for ten CPU-minutes in a debug build. `PosePool::grow` worked around it by reserving a doubled capacity by hand; [containers](containers.md) now grows geometrically on `resize`, so the workaround is gone and this is four `resize` calls. `AnimationConfig::reserve_joints` is still how a world that knows its population never reallocates at all.

**Cost.** 40 bytes of pose (`Vec3` + `Quat` + `Vec3`) and 48 bytes of matrix per joint of every slot ever used. A 23-joint character is 2,024 bytes; 10,000 of them are 20 MB, of which 11 MB is the upload buffer. `PosePool::bytes()` reports the arena **in use**; while the population is still growing the allocator holds up to half again as much, which is what geometric growth costs and what `reserve_joints` avoids.

## The systems

Three, registered through `ecs::register_system` with a `sim::SystemDesc` each ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 2). `AnimationSystem::install(sim)` is the one call that does it, plus the components.

| System | Phase | Reads | Writes | Determinism |
|---|---|---|---|---|
| `animation.advance_players` | `Systems` | `AnimationLod` | `AnimationPlayer`, `SkeletonInstance` | `Hashed` |
| `animation.sample_poses` | `Systems` | all three | resource `animation.pose_pool` | `Hashed` |
| `animation.build_skinning_matrices` | `Systems` | `SkeletonInstance`, `AnimationLod`, resource `animation.pose_pool` | resource `animation.joint_matrices` | **`Derived`** |

`advance_players` moves the playhead, resolves the clip handles, and collapses a finished cross-fade so the sampler never changes state. `sample_poses` fills the slot with the rest pose, samples the clip into it, blends the incoming clip when a fade is in flight, and blends toward the bind pose when `weight < 1`. `build_skinning_matrices` composes the pose into model space and writes `model * inverse_bind` as 3×4.

All three match on all of `AnimationPlayer`, `SkeletonInstance` and `AnimationLod`, so **an entity that has a player but no instance is not ticked**: a playhead with no skeleton animates nothing, and `AnimationSystem::attach` is what gives it one. That is the shape an external edit arrives in — `WorldCommands` can set an `AnimationPlayer` from JSON before anything has attached a skeleton — and it costs a tick of nothing rather than a branch per instance forever. `AnimationStats` is written once per system per tick by stage 0 only, which makes it free and makes it stage 0's share rather than the world's total when there is more than one worker; `promotions` and `demotions` are exact, because a tier transition never happens on a worker.

**Every ordering the three need is declared, and one of the two declarations did not exist when this capability was built.** `advance_players` before `sample_poses` is a component conflict and always was: one writes `AnimationPlayer`, the other reads it. `sample_poses` before `build_skinning_matrices` is a conflict on **the pose pool**, which is not a component and must not be (see "The pose pool" above). That used to be inexpressible, so the third system sat in `PostPhysics` and borrowed its ordering from the phase — honest, and arguable on its own merits, but it moved a system for a reason that had nothing to do with which phase it belongs in. `sim::SystemDesc::reads_resources`/`writes_resources` ([sim](sim.md), "Named resources") say it directly: `sample_poses` writes `animation.pose_pool`, `build_skinning_matrices` reads it and writes `animation.joint_matrices`, and all three run in `Systems`, one wave apart, for exactly the reason the code requires. Their determinism stances are unchanged and are now visibly the *system's* rather than the phase's: the matrices are `Derived` because nothing reads them back into gameplay and a load rebuilds them from the playhead.

**What actually orders them today, which is not the declaration.** flecs' pipeline inserts a sync point for a staged write, not for an in-place write to a `$this` term (`flecs_pipeline_check_term` returns false for owned terms), so there is no barrier between these three systems at any worker count — there was none across the `Systems`/`PostPhysics` boundary either, because a phase boundary is not a sync point in flecs. What makes the tick correct at eight workers is that flecs hands each worker the *same* slice of the same table in consecutive systems, so one entity's three systems run in order on one thread; the bit-identical worker test is what holds that down. The declaration is what [ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7's scheduler will read, and writing it is what turns "correct because of how flecs partitions" into a stated property that survives the executor changing.

**Determinism ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)): `hashed`** for the capability as a whole. Everything inside a tick is arithmetic over per-entity state with no wall-clock read, no shared accumulator and no worker-count-dependent ordering; slots come off a LIFO free list outside the parallel phase. One worker and eight produce identical bytes, and so do two runs — asserted, not assumed.

## The LOD policy

The tier itself is `sim::TierAssignment`'s — the minimum over observers of f(distance, importance, weight), with the hysteresis band and the rate limits the engine's reference implementation already gets right. What belongs to this capability is the *bands* and what each tier *costs*:

| Tier | Divisor | Cross-fade | Skinning matrices | Pool slot |
|---|---|---|---|---|
| LOD0 | every tick | blended | yes | held |
| LOD1 | every 2nd tick | **off** (a cut at the fade's midpoint) | yes | held |
| LOD2 | every 4th tick | off | **no** | held |
| LOD3 | never | — | no | **released** |

The bands are tunables (`animation.lod.near/mid/far/hysteresis`, 12 / 40 / 120 m of observer score), much tighter than `sim::TierParams`' 32 / 128 / 1024 defaults, because the question is different: a settlement's economy is worth simulating a kilometre away and a character's fingers are not worth sampling at forty metres. `animation::lod_tier()` is the single-entity form of the same rule, and the test pins it against `sim::TierAssignment::tier_of` band by band so the two cannot drift.

**Interpolation is the first thing to go, not the sampling rate.** At LOD1 a character is a few pixels across; the cross-fade is invisible long before the halved update rate is, and sampling a second clip is the larger of the two costs. At LOD2 the pose still exists — gameplay reads it for a foot plant, a grab point, a footstep query — and nothing on screen deforms from it, so the matrices are what stop.

**The divisor is staggered by the slot** (`(tick + phase) % divisor == 0`, with `phase` derived from the pool slot), so a crowd at LOD2 spreads over four ticks instead of costing four ticks' work on one of them. Deriving the stagger from the slot rather than from the iteration order is what keeps it identical at one worker and at eight.

**LOD3 freezes the playhead and gives the slot back, and the promotion advances the playhead analytically.** `AnimationLod::frozen_at_us` records the game time at the demotion; the promotion computes where the clip would have got to over the gap in closed form — the same `advance_time` the tick uses, with the whole gap as its delta — and then re-acquires a slot and fills it with the rest pose. A clip's playhead is affine in elapsed time and its wrap is a modulo, so this meets [sim](sim.md)'s summarizer contract *exactly* rather than approximately: equivalence, determinism from stated inputs, idempotence over a partition. That is why LOD3 can be "not animated" rather than "animated cheaply" — nothing pops in time on the way back.

**How tiers reach the capability.** `AnimationSystem::hooks()` is a `sim::MaterializationHooks` row for `sim::SimScheduler::add_hooks`: `materialize`, `promote`, `demote` and `dematerialize` all go to one function, so the scheduler-driven path and a game calling `set_tier` directly cannot behave differently. Promote/demote are idempotent and the tests drive both paths.

**What a coarsened instance draws is the *renderer's* question, and the answer is the rest pose at LOD2 as well as at LOD3.** At LOD3 there is no choice: the pool slot is released, `joint_run` answers false, and the renderer reads that absence as "this instance draws its rest pose" ([renderer](renderer.md#skinned-instances)). At LOD2 the slot is still held and its matrices simply stop being rebuilt, so a consumer that handed them over would draw the character frozen at whatever pose the last LOD1 tick left. `engine-view` hands a zero-length run instead, and a game should. Three reasons, in the order that decided it: it is what LOD2 already *means* here — the pose exists for gameplay and nothing on screen deforms from it, and a stale pose is deforming from it; keeping the last pose makes what is on screen depend on *when* an instance was demoted, so the same character at the same distance differs between two runs and between two cameras, and this project compares captures byte for byte; and at LOD3 keeping one would mean the renderer holding a private copy of every frozen instance's matrices, which is the pose pool rebuilt on the far side of the boundary that exists to keep poses off it. What it costs is measured — see the boundary table below, where the LOD2/LOD3 switch costs **nothing at all** precisely because both sides draw the same pose.

**And which name each hook carries is the contract's, not this capability's choice.** `materialize` is handed the record's persistent `Id128`, resolves it through `ecs::entity_for`, and answers with the `sim::EntityHandle` for the entity it found — this capability attaches a pose to an entity that already exists rather than bringing one into being, so a record naming nothing this world holds gets a null handle and `reconcile_tile` skips it rather than promoting into nothing. `promote`, `demote` and `dematerialize` read their handle with `ecs::entity_of` and keep nothing ([sim](sim.md), "Which name each hook carries"; [ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 3).

## The renderer contract

`AnimationSystem::joint_matrices()` is every animated instance's bone matrices as **one contiguous span**, in slot order, of `anim::JointMatrix` — three `Vec4` rows, 48 bytes, the record `deform.slang` mirrors. An instance's own run is `[first_joint(slot), first_joint(slot) + joint_count(slot))`.

What a renderer does with it:

```cpp
gfx::DeformDesc desc;
desc.flags       = gfx::k_deform_skin;
desc.joint_count = animation.joint_count(instance.pose_slot);
desc.joints      = upload_address + animation.first_joint(instance.pose_slot) * sizeof(anim::JointMatrix);
```

One upload per frame for the whole population rather than one per instance — which is why the pool's slots are a shared arena — while `DeformDesc::joints` is per instance, because two characters share one mesh, one `geometry::SkinBinding` stream and one skeleton and have entirely different poses ([gfx](gfx.md) says the same thing from the other side).

**It is wired up now.** `systems/renderer` takes the span and a `renderer::InstanceJoints` — a `{first, count}` per scene instance — and does the rest: one memcpy into the frame slot's region of a joint buffer, and one pass over its own copy of the deform table pointing each skinned instance's `joints` at its run ([renderer](renderer.md#skinned-instances)). An instance the array does not reach, or whose `count` is zero, **draws its rest pose**, which is what an instance at LOD3 or LOD2 looks like from the other side: the renderer read the absence rather than this module reaching across the boundary to say it.

## The app-side glue

The tick is not the renderer's. A game that animates characters writes the code below, and `engine-view --animate` ([apps](apps.md)) is it, so this is what to copy:

```cpp
ecs::SimWorld sim;                          // the fixed step; 60 Hz by default
animation::Library library;                 // the skins and clips of the model files
animation::AnimationSystem animation(library);
library.load_gltf(mesh_path);               // skins become skeletons, curves become clips
animation.install(sim);                     // three systems, three components, once

ecs::WorldCommands commands(sim.world());
for (u32 i = 0; i < instances; ++i) commands.create(entity_id(i));
commands.apply();
for (u32 i = 0; i < instances; ++i) {
  animation.attach(entity_id(i), skeleton_id, clip_id);
  animation.set_playhead(entity_id(i), phase_of(i), speed);   // a crowd, out of lockstep
}

// per frame
sim.step();
for (u32 i = 0; i < instances; ++i) {
  runs[i] = {};
  if (lod_plan(tier[i]).skin) animation.joint_run(entity_id(i), runs[i].first, runs[i].count);
}
frame.joints = animation.joint_matrices();
frame.instance_joints = {runs.data(), runs.size()};
```

## The app-side LOD glue: what the camera decides

The tier policy above sat unused until 2026-09-18, because nothing told it where the cameras were: `engine-view --animate` attached every instance at LOD0 and left it there, so 1,024 foxes cost **3.2 ms** of CPU a frame for a crowd of which most is a smudge. `engine-view --anim-lod` (on by default) closes that, and the code is `apps/engine_view/anim_lod.h` plus forty lines of `main.cpp` — **this is the pattern a game copies**, so it is spelled out rather than folded in.

**Why it is in the app and not in either module.** The tier is `sim::TierAssignment`'s, the pose is this capability's, and where the cameras are is `systems/renderer`'s — and the renderer must not depend on this capability, on `domain/ecs` or on flecs, while `domain/sim` must not depend on the renderer. The one place the three meet is the host that owns all three. What crosses each boundary is data, and nothing else.

```cpp
// once, when the population is attached: the rows the tier code scores
for (u32 i = 0; i < instances; ++i) {
  row_instance.push_back(i);
  positions.push_back(world_centre_of(i));                 // the instance's bounding sphere
  radii.push_back((mesh_radius(i) + bounds_padding(i)) * scale_max(i));   // padded, as the cull is
}
tier_state.resize(rows, 0);                    // attach() put every one of them at LOD0
params = view::scaled_tier_params(animation::tier_params(), lod_scale);

// per frame, before the tick
view::build_observers(renderer.update_views(camera), camera, observers);  // one per view
view::view_importance(views, positions, radii, importance);               // what the camera says
changes.clear();
tiers.assign_tiers({positions, importance, tier_state}, observers, params, changes);
for (const sim::TierChange& c : changes) animation.set_tier(entity_id(row_instance[c.index]), c.to);
sim.step();
```

**One observer per view, and the frustum gate is importance.** `sim::ObserverSet` gets one observer per view of the `ViewSet`, at that view's eye and with that view's weight — the centre monitor 1, the side monitors `1 / ViewQuality::lod_scale`, which is the same knob `--peripheral-lod` already uses for geometry, because a player who has said the sides are worth a quarter of the detail has said it about the animation too. Today every view of a `ViewSet` shares one eye, so that set is degenerate and the minimum over it is the distance over the largest weight; that is not worked around, because it is the shape a split-screen or co-op layout fills with two genuinely different eyes and everything above keeps working. What a *view* is worth therefore reaches the entity through its **importance**: the best weight among the views whose frustum contains its padded bounds, over the best weight in the set. Importance divides the distance, so a character only a side monitor can see scores four times as far away and coarsens sooner, which is exactly the owner's "the side monitors are peripheral vision".

**Off screen is `k_offscreen_importance` = 1/16, not infinity.** The cull pass already knows exactly what was drawn — one frame late and on the device ([renderer](renderer.md), "The renderer never reads a buffer back inside a frame") — and a tier that lags the camera by a frame pops on every cut. So the glue runs its own conservative frustum test on the CPU, six planes against one sphere per instance, against `SceneRenderer::update_views(camera)` so the frusta are **this** frame's. The sphere carries the instance's `bounds_padding`, so a limb that swings out of the rest-pose sphere still counts as on screen, which is the same direction the cull pass errs in. A factor rather than infinity because infinity would pin a character standing two metres behind the camera — about to be turned back towards — at the coarsest tier, where 1/16 lets it sit at LOD2 and keep the pose a foot-plant query may ask for.

**The changes go through `set_tier(Id128, u8)` and not through the hooks table.** `sim::MaterializationHooks` speaks `sim::EntityHandle`, only `domain/ecs` may make one, and `<flecs.h>` does not belong in `apps/` ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 5). Both paths go to one function here — see "How tiers reach the capability" — so the demotion still releases the pool slot and the promotion still advances the playhead analytically; the `Id128` overload exists for exactly this caller.

**What it is worth, measured.** 1,024 Khronos Foxes, 3840×2160, `--orbit 22 --no-vsync`, 300 frames, `msvc-release` on the RTX 5090, every row taken twice. `tick` is the world's fixed step — the sampler, the skinning matrices and the per-instance `joint_run` — and `lod` is the tier assignment in front of it.

| Tier histogram | `--anim-lod-scale` | tick, run 1 / run 2 | lod | against LOD0 |
|---|---|---|---|---|
| 1,024 / 0 / 0 / 0 (`--anim-lod off`) | — | 3.263 / 3.218 ms | 0.001 ms | 1.0× |
| 1,024 / 0 / 0 / 0 | 1600 | 3.118 / 3.254 ms | 0.031 ms | 1.0× |
| 0 / 1,023 / 1 / 0 | 400 | 1.931 / 1.790 ms | 0.036 ms | **1.7×** |
| 0 / 0 / 1,024 / 0 | 200 | 0.798 / 0.844 ms | 0.038 ms | **3.9×** |
| 0 / 0 / 867 / 157 | 120 | 0.754 / 0.727 ms | 0.044 ms | **4.4×** |
| 0 / 0 / 0 / 1,024 | 1 (default) | 0.284 / 0.312 ms | 0.036 ms | **10.8×** |

**Machine state:** the box was shared throughout — other processes at 6–27% of the CPU and the GPU 1–97% busy at the ends of the runs, 783 MiB of the card held — so every figure is an **upper bound** ([bench](bench.md#measuring-on-a-shared-machine)). The two runs of each row agree to within 8%, and the *ratios* are what the table is for.

Three things it says. **The policy's 5.6× is real and the mix decides which multiple you get**: 1.7× at LOD1, 3.9× at LOD2, 10.8× frozen, against the bench's 5.6× for its own 5/15/30/50 mix. **The assignment costs 0.03–0.04 ms for 1,024 instances**, about a hundredth of what it saves, and it is flat across the mix because it scores every instance every tick whatever tier it is at (`sim`'s own note that the rate limits exist partly so LOD assignment does not have to act on everything applies to the *materialization*, not to the scoring). And **the default bands are metres and a sample asset's units may not be**: the Khronos Fox has a bounding radius of 82 in its own units, so a 32×32 grid of them seen from `--orbit 22` is thousands of units away and lands entirely at LOD3 with `--anim-lod-scale 1`. That is the policy working on the numbers it was given, and `--anim-lod-scale` is the knob that says so; a game with metres in its assets would not need it.

**What a tier change costs the picture, which is what makes the thresholds defensible.** One fox, 1280×720, `--orbit 22`, frame 61, rendered on both sides of each band boundary at the distance where the switch happens (found by bisecting `--anim-lod-scale`, so the two pictures differ in nothing but the tier):

| Boundary | What changes | FLIP mean | FLIP max | PSNR | SSIM |
|---|---|---|---|---|---|
| LOD0 → LOD1 | cross-fade interpolation off, every second tick | **0.0011** | 0.565 | 47.6 dB | 0.9984 |
| LOD1 → LOD2 | no skinning matrices: the character draws its rest pose | **0.0097** | 0.895 | 34.4 dB | 0.9876 |
| LOD2 → LOD3 | the pool slot is released; the pose is the same one | **0.0000** | 0.000 | ∞ | 1.0000 |

FLIP calls about **0.1** the threshold at which a person starts to notice a difference, so the worst of the three is a tenth of that and the first is a hundredth. The LOD1/LOD2 step is the whole of the visible cost, which is the honest reading: that boundary is where a character stops moving, and `animation.lod.mid` (40 m of observer score) is what decides where it happens. The LOD2/LOD3 step is free *by construction* — both draw the rest pose, see "What a coarsened instance draws" — and that is the point of the decision rather than a coincidence.

**Why there are `Id128` overloads of `attach`, `play`, `set_playhead` and `joint_run` at all.** `<flecs.h>` belongs to `domain/ecs`, `systems/` and `game/` (AGENTS.md, [ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 5) and `apps/` is deliberately not on that list — so a host outside `systems/` would have had to break the seam to attach a single character. The four overloads resolve the id through `ecs::IdentityMap` exactly as `set_tier(const Id128&, u8)` already did, and between them they are the whole of what such a host needs. They are also the vocabulary seam 3 asks for: an id that survives a tick, a save, and the wire.

**The bound a renderer needs, and why this module computes it.** A skinned instance is culled by its mesh's **rest-pose** cluster spheres, and a skinned vertex is not in them, so the renderer wants one number per instance saying how far skinning can move a vertex ([renderer](renderer.md#bounds-the-cluster-spheres-are-the-rest-poses-and-a-skinned-vertex-is-not-in-them)). That number is a property of a clip and a mesh, and the renderer has neither a clip nor a skeleton, so it lives here: `joint_influence_bounds` takes a mesh's positions and its binding stream and gives each palette slot the bounding sphere of the vertices it actually influences, and `clip_displacement_bound` maximizes `|(A - I) c + t| + ||A - I|| r` over every joint and every sampled phase of the clip. It is conservative for every vertex and any weights.

Both halves of that shape were measured rather than reasoned about. Bounding each joint by the *mesh's* own sphere instead — the obvious simplification — asks how far the tail joint would move a vertex at the nose, and gives the Khronos Fox a padding of 249 units on a mesh of radius 82. Splitting the inequality into `||A - I|| |p| + |t|`, which has the appeal of not needing the mesh at all, is looser again: a rotation about a joint a hundred units up the rig carries a hundred-unit translation that the centre term exists to cancel. The per-joint form gives the Fox 49.3 and RiggedFigure 0.48 on 0.83.

The sampling is every keyframe time of every track plus a uniform grid. That is exact for STEP and LINEAR tracks — slerp runs along the geodesic between two keys, so the displacement a joint induces is largest at an end of the interval — and dense-but-not-proven for CUBICSPLINE, the one glTF interpolation that can overshoot its keys. The test measures the truth by brute force at 997 phases and checks the bound against it: conservative, and 1.38× the measured worst on the fixture rig.

## The clip library

`Library::load_gltf(path, prefix)` reads every skin and animation of a glTF or GLB file through [assets](assets.md) and turns them into `anim::Skeleton`s and `anim::Clip`s. Skins become skeletons **in palette order**, because a vertex's `geometry::SkinBinding` names a palette slot and those bindings live in mesh streams this capability does not own; a palette that is not parent-before-child is therefore refused with a sentence rather than sorted ([ADR-0017](../adr/0017-no-hidden-limits.md): stated, not hidden). `assets::load_gltf` sorts the node array that way and every exporter writes the palette in node order, so this has not been seen in practice. Authored `inverseBindMatrices` win over recomputed ones, as [anim](anim.md) says they should. An animation is matched to the skin most of its channels land in; a channel that lands in none is counted in `LoadStats::skipped_channels` rather than dropped silently.

**Ids are a pure function of the asset's name**, `"<prefix>/<name>"` hashed, so the same content gives the same ids in every process and on every machine and a save written by one build resolves in the next. **The prefix defaults to the file's *stem*, never its path** — a path would put "D:/work/tree/rig.glb" in the id and make every save unloadable on another machine. Two files with the same stem and a skin of the same name collide, and the answer is to pass a prefix, which is why the parameter exists.

**The standard-skeleton mapping is computed once per skeleton, at load.** `anim::map_joints` is three passes of normalized string matching over every joint; per instance or per retarget it would be a load cost paid every tick. `Library::build_retarget(from, to)` pairs two of the library's skeletons through those mappings.

## What the contract could not express

This capability was built to find out what ADR-0027's contract is like from the inside. Three things it could not say, recorded here because the next capability will meet them:

1. **A system could not declare that it writes data outside the ECS.** ~~`sim::ComponentMask` addresses components, and "the pose pool" is not one.~~ **Closed, 2026-09-18**: `sim::SystemDesc` carries named resources, the scheduler's wave layering treats a resource conflict exactly like a component conflict, and this capability's third system moved back into `Systems` where it belongs, ordered after `sample_poses` by `animation.pose_pool`. See [sim](sim.md), "Named resources", and "The systems" above.
2. **The materialization hooks and the ECS did not share an identity vocabulary.** ~~`sim::MaterializationHooks` speaks `u64`~~ **Closed, 2026-09-18**: `EntityRecord::entity` is the `Id128` it came off the store with, and `promote`/`demote`/`dematerialize` take a `sim::EntityHandle` — a distinct type, valid only inside the call, which only `domain/ecs` may look inside. This capability's `materialize` hook resolves the record through the identity map and answers with the handle, which is what `reconcile_tile`'s promotions then act on; the other three go through `ecs::entity_of`. See [sim](sim.md), "Which name each hook carries".
3. **A tier is a field, not an index.** [ADR-0028](../adr/0028-ecs-and-persistent-store.md)'s own guard rail says a component the query reads and rejects on is not an index, and `AnimationLod::divisor` is exactly that: every tick, `sample_poses` visits every animated instance and skips three quarters of the LOD2 ones. A tier *relationship* would make each tier its own archetype and the query would iterate only what it should — at the cost of an archetype cross product, and of an archetype move per tier change. That trade is worth measuring once there is a scene with a real tier distribution; it is named here rather than discovered later.

## Invariants (each one tested)

- A glTF skin becomes a skeleton in palette order with the parent before the child, the file's own inverse binds, and a standard-skeleton mapping; its animations become clips on that skeleton.
- Asset ids are a function of the name alone: a second library over the same content resolves the first one's ids.
- A player advances by exactly the step times the speed times the divisor, and looping wraps exactly.
- The pose in a slot equals `anim::rest_pose` plus `anim::Clip::sample` done directly, and the matrices equal `anim::skinning_matrices` of that pose.
- A cross-fade equals `anim::blend` of the two clips at the fade's fraction, and ends by making the incoming clip the clip.
- A tier change acquires or releases exactly one slot, LIFO, and preserves the playhead — the promotion lands where ticking through the gap would have.
- The divisor samples each instance once per cycle, spreads the cycle across ticks, and keeps the playhead's average rate.
- Two runs, and one worker against eight, produce bit-identical bone matrices.
- `WorldCommands` can set `engine.animation.AnimationPlayer` from JSON by schema type name, and a type the schema does not declare cannot be named.
- Every system's query writes only components its `SystemDesc` declares.

**Public API.** `include/systems/animation/animation.h` (`AnimationSystem`, `LodPlan`, `lod_plan`, `tier_params`, `lod_tier`, `AnimationStats`, `AnimationConfig`, `k_determinism`, `k_phase_*`, `k_resource_pose_pool`, `k_resource_joint_matrices`, `k_tier_count`), `library.h` (`Library`, `SkeletonAsset`, `ClipAsset`, `LoadStats`, `JointBounds`, `joint_influence_bounds`, `clip_displacement_bound`, `skeleton_displacement_bound`), `pose_pool.h` (`PosePool`, `k_no_slot`). The components come from `schemas/animation.schema` and arrive as `<schemas/animation.h>`.

**Depends on.** `base`, `containers`, `math`, `time`, `log`, `jobs`, `ids`, `json`, `schema`, `tunables`, `geometry`, `anim`, `assets`, `sim`, `ecs`, `animation_schemas`.

**Testing.** `tools/dev.ps1 test -Preset msvc-debug -Filter animation` — 19 cases over `tests/animation_tests.cpp` (the library, the playhead, the pose against `anim::`, the cross-fade, the weight, `WorldCommands`, the empty world, the declarations — including the three waves the pose-pool resource puts them in — and the bit-identical worker case, **the `Id128` surface a host with no flecs drives — attach, phase-shift two instances, read the runs back, and find them at the offsets the pool reports, with an entity at LOD3 answering false rather than a run of zeros — and the displacement bound, checked against the truth measured by brute force at 997 phases**) and `tests/lod_tests.cpp` (the plans, the agreement with `sim::TierAssignment`, the LOD3 round trip, **a promoted instance's bone matrices bit-identical to a continuously ticked instance's after 97 frozen steps**, the divisor, the materialization hooks driven by `EntityHandle`, `materialize` named by `Id128` and answering with a handle, and slot determinism). The app-side glue has its own cases beside it in `apps/engine_view/tests/anim_lod_tests.cpp`: the per-view weights, the observer set, the frustum gate and its off-screen factor, the band scale, and a crowd of 512 walked across every boundary and back whose worst instance changes tier **6 times in 400 ticks** — the six crossings it actually made, which is hysteresis doing its job. The fixture is a two-bone skinned GLB with two clips, **written at test time** (`tests/animation_glb.h`) into a `TempDir`, for the reason [assets](assets.md)' fixture is: a binary in the tree is something nobody can review. Benchmarks: `tools/dev.ps1 bench -Preset msvc-release -Filter 'animation.*'`.

## Performance notes

`build/msvc-release/systems/animation/engine_animation_bench.exe --filter=animation.* --wait-quiet=1800`. **10,000 instances of a 23-joint skeleton** playing a one-second clip with a rotation track on every joint — the worst case for the sampler and the honest one for a character — through the real `SimWorld` and the real three systems, **single-threaded**.

**Machine state.** i9-10980XE, 36 logical CPUs, release build, 2026-09-18. The harness waited for a quiet machine and the run was quiet at both ends: other processes at 8.3% of the CPU at the start and 8.7% at the end, GPU 1% and 6% ([bench](bench.md#measuring-on-a-shared-machine)). A second run of the tick rows agreed within 1.4% on every row, and is not quoted because it *ended* under load (others at 17.7%) — which is exactly the case that makes a number an upper bound rather than a cost.

| Benchmark | Per tick | Per instance | What it covers |
|---|---|---|---|
| `animation.tick.lod0` | **25.28 ms** | 2.53 µs | 10^4 instances, every one every tick, sampled and skinned |
| `animation.tick.lod1` | **13.08 ms** | 1.31 µs | every second tick, no cross-fade interpolation |
| `animation.tick.lod2` | **4.08 ms** | 0.41 µs | every fourth tick, sampled but not skinned |
| `animation.tick.lod3` | **87.6 µs** | 8.8 ns | nothing animated: the query match and the divisor test |
| `animation.tick.mixed` | **4.48 ms** | 0.45 µs | 5% / 15% / 30% / 50% across the four tiers |
| `animation.pool.churn` | 7.3–7.5 ns | per slot | acquire + release, at 256, 4,096 and 65,536 slots |

Five things these say:

1. **The tier mix is the cost, which is the LOD policy's whole claim.** The same population and the same clip cost **25.28 ms** with nothing coarsened and **4.48 ms** at a plausible distribution: **5.6×**, and it is the only reason a crowd of this size is affordable at all. A build where those two rows are close is a policy that is not working.
2. **10,000 characters all at LOD0 does not fit a frame, and was never going to.** 25 ms against a 16.6 ms budget is the honest number to put beside [ADR-0028](../adr/0028-ecs-and-persistent-store.md)'s "simulation LOD stops being an optimization and becomes load-bearing". The mixed row, at 4.5 ms, is 27% of a 60 Hz frame single-threaded — a budget item ([ADR-0018](../adr/0018-frame-budgets-as-merge-gates.md)), not a rounding error, and the thing the rate limits in `sim::TierParams` exist to keep from arriving all at once.
3. **Where the time goes, read off the rows themselves.** LOD2 samples a quarter of the population per tick for 4.08 ms, so a sampled instance is ~1.6 µs; LOD1 samples *and skins* half of it for 13.08 ms, so an instance is ~2.6 µs. The difference — about **1.0 µs** — is `local_to_model` plus `skinning_matrices`, 46 `Mat4` products for 23 joints, which is close to what that arithmetic costs and leaves little to win. The other **1.6 µs** is the sampler, and it is dominated by 23 `slerp`s with their `acos` and two `sin`s: that is `domain/anim`'s code and the first place to look, along with the per-track cursor this module still owes (see "Not yet").
4. **LOD3 is the floor, and it is what "absent capabilities are free" means per tick** ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)): the instances exist, the components exist, all three systems are registered and running, and 10^4 of them cost **8.8 ns each** — a query match and a divisor test.
5. **The pool's free list is O(1) and stays that way.** 7.3 ns per acquire-and-release at 256 slots and 7.5 ns at 65,536: flat over 256× the population, which is the property a per-skeleton LIFO list is there for. It is also the number a camera cut across a tier boundary pays per instance, and it is small enough that the rate limits are about the *materialization*, not about this.

**The number to plan against is the single-threaded one.** [ADR-0028](../adr/0028-ecs-and-persistent-store.md) records that hosting flecs' workers on `core/jobs` currently costs a flat 1.6–2.2 ms a tick, so every worker count is slower than one worker; until that is fixed, a world does not get parallelism from `ecs::set_workers` and this capability's tick is measured without it. Nothing in these systems would stand in the way of it: they are per-instance work over disjoint writes, which is the shape that scales, and the bit-identical worker test says the answer does not change when it does.

**Zero cost when unused ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)): no linked code, and no instances.** `ENGINE_WITH_ANIMATION=OFF` (and the minimal presets) drop the module, its schema library, its tests and its bench; the module disappears from `modules.json` and is listed under `disabled_capabilities`. With the capability *in* the build, a world with no `AnimationPlayer` matches no query, allocates no pool and costs three empty query matches a tick.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Two things outside its directory did change, both deliberately and both with their own commit: `cmake/EngineModule.cmake` gained the capability graph (`engine_capability_requires`), because anything that ticks depends on `domain/ecs` and that combination did not configure; and `domain/anim` gained non-owning pose views, because a pool could not be the storage its sampler writes into. Both are argued in [plan 02 §2.8.1](../plan/02-architecture.md#281-the-first-capability-built-through-the-contract).

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(animation ecs)` | declared; off when `ecs` is off |
| Component and event types | `systems/animation/schemas/animation.schema` | three components, no events yet |
| Tick scheduler entry | three `sim::SystemDesc` through `ecs::register_system` | done |
| Render-graph passes | none | this capability produces a buffer; it does not draw |
| Content-build derived step | none | clips are imported at load, not precomputed |
| Protocol methods | none | the components are already reachable through `world.apply`; a `animation.play` method is a follow-up |
| Tunables | `animation.lod.near/mid/far/hysteresis/max_promotions/max_demotions` | read once per call, never per entity |
| LOD policy | `lod_plan()`, `lod_tier()`, `AnimationSystem::hooks()` | done, over `sim::TierParams` |
| Determinism | `hashed` (the playhead and the pose); the matrices are `derived` | done |
| Zero cost when unused | no linked code, and no instances | done |
| Tests and size table | `tests/animation_tests.cpp`, `tests/lod_tests.cpp`, `tests/size_table.cpp` | 18 cases |
| Bench | `bench/animation_bench.cpp` | 10^4 instances by tier mix |
| Removal proof | `ENGINE_WITH_ANIMATION`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_ANIMATION=OFF`, or `-DENGINE_WITH_ECS=OFF`, which takes this capability with it) drops the module, its schema library, its tests and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else still builds and passes.

## Not yet

- **Blend trees and state machines as data** ([05 §5.11](../plan/05-simulation.md#511-integration-notes)). One player per entity is what this samples; `AnimationPlayer::weight` is already the number a blend-tree node would carry, which is why it is persisted now rather than added later with a migration.
- **A track cursor.** `anim::Clip::sample` walks each track's keys from the front, which [anim](anim.md) says is right for a clip and notes belongs with the playback state a system layer owns — that is this module, and it does not do it yet. It is the first thing to try if the sampler shows up in a profile.
- **Additive layers.** `anim::blend_additive` and `make_additive` exist and nothing here calls them.
- **IK, foot locking, a contact channel, retarget-driven playback.** `Library::build_retarget` exposes the retarget; no system applies one.
- **Protocol methods.** `animation.play`, `animation.attach` and a query over the library would be a small `register_methods()` — and would now have the `Id128` overloads to call, which is what the host-facing surface above was shaped for.
- ~~**Tiers driven by the camera.**~~ **Closed, 2026-09-18**: `engine-view --anim-lod` (on by default) builds a `sim::ObserverSet` from the renderer's views and runs `sim::TierAssignment` over the animated instances every frame. See "The app-side LOD glue" above for the pattern and the numbers. What is still open there: the crowd's positions are rebuilt once because engine-view's instances do not move, so nothing yet measures the cost of refilling them per tick; the scoring visits every instance every tick, which is 0.04 ms at 1,024 and would want a spatial structure at 10^5; and the tier is still a field rather than a relationship, so `sample_poses` visits the frozen instances too (see "What the contract could not express").
- **A tier relationship instead of a tier field**, once there is a scene whose tier distribution is worth measuring. See "What the contract could not express".
