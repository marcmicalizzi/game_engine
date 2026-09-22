# anim (domain)

**Purpose.** The data half of animation ([05 §5.11](../plan/05-simulation.md#511-integration-notes)): skeletons, poses, clips, the blends over a pose, the matrices a skinned mesh is deformed by, and the first cut of the **standard skeleton** that makes generated characters tractable. It is the CPU side of the skinning mode of the deformed-vertex pool ([04 §4.3](../plan/04-renderer.md#43-geometry), [gfx](gfx.md)) and the reference the GPU pass is checked against. There is no system, no scheduler, and no GPU here: blend trees and state machines *as data* belong to a `systems/animation` capability that will build on this, and the plan's IK and motion matching are further out still.

**Owned data.** `Skeleton` (the joint hierarchy, its bind pose, and the inverse bind matrices), `Pose` (one local transform per joint), `Clip` (keyframe tracks), `JointMatrix` (the 3×4 skinning matrix the shader reads), and `Retarget` (the per-joint offsets between two skeletons). Nothing else owns a pose; the vertex streams a pose deforms belong to [geometry](geometry.md) and the per-frame pool to [gfx](gfx.md).

## Why SoA poses

A `Pose` is three parallel arrays — `translation`, `rotation`, `scale` — and not an array of `Transform3`. That is the plan's wording ("own system with SoA poses"), and the reason is what the work over a pose actually looks like:

- A blend is a weighted average **per channel**. `blend` is three streaming loops, each reading two contiguous arrays and writing a third; in the array-of-structs form every one of those reads 40 bytes of a joint to use 12.
- Most tracks in most clips are **rotations**. A clip whose tracks are all rotations writes one contiguous array and never touches the other two; interleaved, it would dirty every cache line of the pose.
- Additive layers are routinely rotation-only (an aim, a lean, a recoil). Applying one should not load a translation.

The same reasoning goes one level deeper in `Clip`, which holds *every* track's keyframe times in one array and every track's values in another, with a 20-byte `Track` record naming each one's run. A clip is read-only data sampled thousands of times, its tracks are sampled in order, and the alternative — a `Vector` per track — is one allocation and one pointer chase per joint per channel. **Nothing here allocates per joint**: a `Pose` is three `Vector`s sized once, `sample` and `blend` write into storage the caller owns, and `local_to_model` and `skinning_matrices` take spans.

### A pose that is not its own storage

`PoseView` and `ConstPoseView` are three spans over the same three channels, and they are what `sample`, `blend`, `blend_additive`, `make_additive`, `rest_pose` and `local_to_model` actually take; `Pose` converts to either implicitly, so every call site reads as it did.

They exist because "writes into storage the caller owns" was only true while the caller owned *one* pose. A system layer owns thousands: [animation](animation.md)'s pose pool is one arena with every instance's joints laid end to end — [03 §3.4](../plan/03-data-model.md#34-the-runtime-world)'s "hot systems own their own data" applied to poses — and a `Pose` per instance would be three heap allocations and three pointer chases per instance, which is the layout this page spends its first section arguing against. Before the views, the only way to sample into a pool was a scratch `Pose` and a copy of every sampled pose in and out of it, per instance per tick. That is the whole of the reason, and it was found by building the first capability on top of this module rather than by reasoning about it.

The `Pose&` overloads stay, because a view cannot be resized and the resizing is what a one-off caller wants; each one sizes its output and then calls the view form, so there is exactly one copy of every loop. A view whose three channels are different lengths writes nothing, the same answer mismatched `Pose`s already got.

A skeleton is SoA for a narrower reason: the only whole-skeleton operation, composing local transforms into model ones, walks `parents` and the pose together and touches nothing else. **A joint's parent always comes before it**, which is what makes `local_to_model` one forward pass with no stack and no recursion; `assets::load_gltf` already sorts its node array that way, and `Skeleton::validate` refuses a skeleton that is not sorted rather than letting the pass compose against a stale matrix.

## Why the skinning matrix is 3×4

`JointMatrix` is three `float4` **rows**, 48 bytes, pinned by the size table and mirrored in `deform.slang`. A skinning matrix is `model * inverse_bind`, a product of rigid transforms and scales, so it is affine for every joint of every pose and its fourth row is `(0, 0, 0, 1)` always. Storing that row costs a quarter of the array — 16 bytes a joint, 1.6 KB for a hundred-joint character — and **the array is uploaded per instance per frame**, which is exactly the kind of bandwidth [11 §11.2](../plan/11-performance-principles.md) says to count rather than assume away. A crowd of two hundred characters is 320 KB a frame saved for nothing given up.

The shader pays nothing for the choice: skinning a position is three dot products with `float4(p, 1)` either way, and row-major is what makes those three dots three contiguous 16-byte loads. What the form costs is that a `JointMatrix` cannot be multiplied by another one without rebuilding the fourth row — and nothing downstream of the skinning does, because the composition happens in `local_to_model`, in `Mat4`, before the conversion. `joint_matrix`, `mat4_from_joint`, and `transform_point` convert and apply; the last of the three is the CPU's copy of the shader's arithmetic, which is why the GPU test can compare against it.

## Sampling

`Clip::sample(time, pose)` writes **only the joints its tracks name**, so the caller fills the pose with `rest_pose(skeleton, pose)` first (or with the output of another clip, for a layered blend); a joint nothing animates then holds the bind value rather than whatever the buffer contained. Time is seconds, with an overload on `GameTime` so a scheduler-driven layer never has to invent a float. With `loop`, time wraps into `[0, duration)`; without it, it is clamped.

All three glTF sampler modes are here, because a clip is imported and not authored ([assets](assets.md)): `k_interp_linear` (lerp, and **slerp on the short arc** for a rotation — the two keys of a quarter turn are 45° apart in the middle either way, but a component lerp gets there at the wrong speed), `k_interp_step`, and `k_interp_cubic`, which is glTF's CUBICSPLINE with the in- and out-tangents the exporter wrote, evaluated on the Hermite basis with the tangents scaled by the key interval exactly as the specification's pseudo-code does. A time before the first key or after the last **takes that key's value rather than extrapolating**: every DCC tool does the same, and extrapolating a quaternion past its last key is how a blend flies apart at a clip boundary.

Key lookup is a linear walk from the front, not a binary search. A track has a handful of keys, they are walked in memory order, and the predictable compare wins until key counts nothing here will see. A cursor per track, which is what a playing clip really wants, belongs with the playback state the system layer will own, not with the clip.

### Morph weight tracks

A clip also carries `WeightTrack`s, and `Clip::sample(time, pose, weights)` returns the weights **alongside** the pose in one walk of the clip. `weights` is a plain `std::span<f32>` the caller owns — a slot of an array a whole crowd shares, the same contract the joint matrices travel on — and, like the pose, it is *not* cleared: a clip writes only the channels its tracks drive, so a caller fills it with the mesh's default weights first. A span shorter than a track's range takes the part that fits, so a caller may size it by the mesh rather than by the clip. `sample_weights` is the weights alone, for a facial performance played over a body pose another clip owns.

**A weight track is its own record rather than a fourth `k_channel_*`, and the reason is the shape of the data.** glTF writes one SCALAR sampler that drives *all* of a mesh's targets at once, so one key is as wide as the rig — a `Track`'s one-byte `components` and its per-joint `joint` field could carry neither. `WeightTrack` therefore names a run of morph channels (`first_channel`, `channel_count`) and shares `Clip::times` and `Clip::values` with the joint tracks, so a clip is still two arrays and sampling still allocates nothing. All three interpolation modes work the same way they do for a joint track; there is no slerp case, because a weight is a scalar. `morph_count` is the rig width the clip was authored against, and `validate` checks every track's range against it.

## Blending

`blend(a, b, t, out)` is the interpolation, clamped to 0..1, one channel at a time. `make_additive(pose, reference, out)` builds a **difference** and `blend_additive(base, additive, weight, out)` applies one: translation and scale add, and the rotation composes **on the right** — `base.rotation * slerp(identity, additive.rotation, weight)` — which is the order that makes an additive aim or lean turn the joint in its own frame rather than in its parent's, so the layer looks the same whatever the base pose did above it. Building a difference and applying it at full weight gives the pose back, which the test pins.

Two poses of different lengths leave the output **untouched** instead of writing a partial pose. That is the case a blend graph hits by wiring two rigs together, and half a pose is harder to diagnose than none.

## The standard skeleton, first cut

The plan's argument ([05 §5.11](../plan/05-simulation.md#511-integration-notes)) is that a game whose characters are generated cannot have one rig per character and one clip per rig: the clip library is authored once against a skeleton that exists only as a naming convention, and every character declares which of *its* joints plays each canonical role.

What is built: `StandardJoint`, a 23-role humanoid core (the spine chain, the head, two arms, two legs); `map_joints(skeleton)`, which finds a skeleton's joint for each role **by name**, the only signal every exporter agrees on; and `build_retarget`/`retarget_pose`, which pair two skeletons through the roles and apply per-joint offsets.

Name matching normalizes (lower-case, everything but letters and digits dropped) so that `mixamorig:LeftForeArm`, `Left_Fore_Arm`, and `leftforearm` are one name, then runs **three passes — exact, then suffix, then substring** — with each skeleton joint claimed by at most one role. The passes are what stop `LeftHandIndex1` from taking `LeftHand`'s place when the real hand is in the skeleton, and the alias tables carry the spellings the rigs that actually arrive use (Mixamo, the Khronos samples, Rigify, the Bip01 lineage). It is a heuristic and it is meant to be replaceable: `JointMapping` is a plain array, so an authored mapping file can fill it without calling `map_joints` at all.

The retarget is the classic **bind-difference** form:

```
rotation    = target_bind.rotation * (inverse(source_bind.rotation) * source.rotation)
translation = target_bind.position + (source.translation - source_bind.position) * scale
scale       = the source's, unchanged
```

What transfers between two rigs is how far a joint has turned **from its own rest**, not where it points in its parent's frame, because the two rests differ by exactly the modelling decisions a retarget exists to absorb. It is exact when the two skeletons are the same one — the round-trip test pins that, and it is the property a wrong offset breaks first. `translation_scale` is the ratio of the two rigs' bind hip heights, so root motion authored for a 1.8 m character covers the same fraction of a stride on a 1.2 m one instead of making it skate. A joint no role maps keeps its bind transform, so the result is always a complete pose; two skeletons that share no role are a content error and `build_retarget` says so rather than silently producing the bind pose for every input.

**What the standard skeleton needs next**, listed in `standard_skeleton.h` beside the code so nobody has to find this page first: IK, and therefore foot and hand locking (a retargeted walk on different leg proportions slides, and the fix needs a contact channel nothing authors yet); bone-length compensation on the limbs, since only the root's translation deviation is scaled today; twist distribution, without which a forearm-twist joint the source lacks keeps its bind rotation and the wrist shears; joint limits and pose validation; an authored mapping file, fingers, toe joints past the base, and a face; and a per-joint basis correction for rigs whose bone-axis conventions differ by more than their bind poses account for (a T-pose against an A-pose is fine; a 90° axis convention is not).

## Skinning

`skin_positions(positions, bindings, matrices, out)` is the CPU reference for the skinning mode of `domain/gfx/shaders/deform.slang`: linear blend skinning, four influences a vertex, weights out of 255, an influence with weight 0 skipped outright. It lives here rather than beside the shader because it is the *definition* of what the shader has to reproduce, and because a CPU skinning path has callers that never build a device (a collision proxy, a footstep query, a headless server). A non-zero influence naming a joint past the end of `matrices` is skipped too, so bad data deforms a vertex short rather than reading out of bounds.

**This is where the crack rule lands for skinning.** A deformed position has to be a function of quantities the cluster build guarantees every copy of a surface point shares ([gfx](gfx.md), "Deformed clusters"). A skinned position is a function of the rest position and the binding, and *both* are per-vertex data inherited from the source vertex through `vertex_source`, on every cluster and every LOD level ([geometry](geometry.md), "Skinned meshes"). So skinning is position-only and cannot tear — unlike the `wave` deformer of [E25](../experiments/e25-deformed-clusters.md), which reads a normal and does.

## The matrix kernels, and the AVX2 regression they used to be

[ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) moved the tree to x86-64-v3 and recorded one
regression as the price: `animation.tick.lod0` 15% slower, isolated by subtracting two LOD tiers to
`local_to_model` plus `skinning_matrices` — **0.967 µs an instance at v2 against 1.372 µs at v3,
+42%**, for a 23-joint skeleton. Its instruction was to fix the kernel rather than exempt the
module. **That is done, and the v3 build is now faster than the v2 build ever was.**

### What it actually was, which was not what it looked like

It looked like "AVX2 made a 4×4 product slower". Reading MSVC's output for both baselines
(`/FA`, same source, `/O2`) says something else: **neither build vectorized anything.** Both were
scalar — 64 `mulss` and 48 `addss` a call at v2, exactly 64 `vmulss` and 48 `vaddss` at v3 — and
the v3 build was **25% *fewer* instructions**, 254 against 337. No FMA contraction (MSVC does not
contract at `/fp:precise`), no wider arithmetic, fewer instructions, and 22% slower.

The difference was three lines at the bottom of the loop. Both builds computed the sixteen
products scalar into a stack temporary; v2 then read it back with 16-byte `movups` and stored
`out[i]` as four of them, while **v3 read it back with two 32-byte `vmovups` and stored `out[i]`
as two more**. That is a store-forwarding stall twice a joint — a load wider than the stores
feeding it cannot forward and waits for them to reach L1 — and it lands on the loop's critical
path, because `local_to_model` is a forward pass in which every non-root joint reads the matrix
the previous iteration wrote. `/arch:AVX2` did not make the arithmetic wider; it made the *copy*
wider, in the one place where width is a liability.

### The fix

`affine_column` in `skeleton.cpp`: the product in 128-bit registers, four 16-byte column stores,
and for `skinning_matrices` the 3×4 built **directly** — the four product columns transposed in
registers with three rows stored, so the row `joint_matrix` used to drop is never written. SSE2 is
guaranteed by x86-64, so this is not dispatch and there is no run-time choice. The summation order
is `((c0·x + c1·y) + c2·z) + c3·w`, which is exactly `core/math`'s `operator*(const Mat4&, Vec4)`,
so the result is the same float and not an approximation of it; `anim_tests.cpp` compares both
kernels against `Mat4 operator*` with `==` rather than `Approx`.

### Numbers

`build/msvc-{release,release-v2}/domain/anim/engine_anim_bench.exe --filter=anim.skeleton.*`, one
23-joint chain per iteration. i9-10980XE, 36 threads, RelWithDebInfo. **Machine state: not quiet.**
The box was between 10% and 89% others' CPU with the GPU at 92–100% throughout
([bench](bench.md#measuring-on-a-shared-machine)), so no absolute figure here is a cost. The method
carries the comparison instead, as ADR-0031's did: the four binaries — {before, after} ×
{v2, v3} — were **alternated seconds apart**, six rounds, and the column below is the minimum over
those rounds of each binary's own `min/iter`. The quoted session ran at 10.7–18.9% others' CPU;
a second session at 23–63% reproduced every row to within **1.3%**.

Nanoseconds per call, lower is better:

| Benchmark | v2 before | v2 after | Δ | v3 before | v3 after | Δ |
|---|---|---|---|---|---|---|
| `anim.skeleton.local_to_model.one` | 641 | **444** | −31% | 787 | **571** | −27% |
| `anim.skeleton.skinning_matrices.one` | 347 | **145** | −58% | 417 | **144** | −65% |
| `anim.skeleton.skinning.one` (both, one instance) | 985 | **587** | −40% | 1,322 | **744** | −44% |
| `anim.skeleton.humanoid_tree.one` (branching rig) | 648 | **444** | −31% | 787 | **568** | −28% |
| `anim.skeleton.skinning.crowd` (256 instances, µs) | 264.6 | **166.4** | −37% | 348.1 | **202.2** | −42% |
| `anim.skeleton.local_matrices.one` (`core/math`, untouched) | 245 | 245 | 0% | 437 | 434 | −0.7% |

**The bench reproduces ADR-0031's inference.** Its 0.967 / 1.372 µs came from subtracting two tick
rows measured through the ECS, the pose pool and three systems; `skinning.one` measures the same
pair directly and says **0.985 / 1.322 µs** for the same code. Two methods, three sessions apart,
within 4%. After the fix the pair is **0.587 µs at v2 and 0.744 µs at v3** — so the v3 build now
costs **23% less than the v2 build did before ADR-0031 landed**, and the regression is not merely
paid off.

**`skinning_matrices` is where the diagnosis is confirmed.** It was 20% slower at v3; it is now
**1.3% faster** at v3 than at v2. Nothing about the arithmetic changed between those two states —
only the width of the stores — which is the evidence that the store width was the whole of it.

### What is left, and where it turned out to be

`local_to_model` was still **28% slower at v3 than at v2** after the fix, and the last row of the table pointed at `core/math::mat4_from_transform`: `anim.skeleton.local_matrices.one` is 23 calls to it and nothing else, it was **+77% at x86-64-v3**, and by subtraction it was 89% of `local_to_model`'s remaining v3 penalty.

**The function is fixed, in `core/math`, and the subtraction was half right** ([math](math.md#aggregates-returned-by-value-and-the-avx2-copy), 2026-09-22). The cause was the same 32-byte copy of a freshly built temporary, plus an out-of-line `mat3_from_quat`; the rotation is now expanded in place. Measured the same way as the table above — {before, after} × {v2, v3} alternated six rounds, two sessions — `local_matrices.one` went **from 245 / 435 ns (v2 / v3) to 162 / 140 ns**: the +77% is gone and v3 is now 14% *faster* than v2. But `local_to_model.one` moved only from 444 / 564 to 449 / 547 ns. The two rows are not the same call: the bench's loop inlines `mat4_from_transform` and was paying the copy the fix removed, while `local_to_model` calls it **out of line** (MSVC declines to inline it here under `/Ob1`), where its result is written with sixteen 4-byte stores at both baselines and then read by `affine_column`'s 16-byte loads — which cannot store-forward at either baseline, so the fix had nothing to remove there. The ~22% that `local_to_model` still costs at v3 is therefore in this module's loop and its call boundary, not in `core/math`, and nothing here says yet which instruction pays it. Two shapes would take the matrix out of memory entirely and are the follow-up: build `affine_column`'s broadcasts from `b[k]` scalars rather than shuffles of a 16-byte load (MSVC keeps an inlined `mat4_from_transform` wholly in registers in that shape), or inline `mat4_from_transform` into the loop. Either is measured against the rows above before it is kept.
One thing tried and rejected, so it is not retried: a root joint's `out[i] = matrix` compiles to a
256-bit `vmovups` pair at v3, which looks like the same disease. Written by hand as four 16-byte
stores it measured **453 ns against 444 ns** — inside the session spread — because one joint in
twenty-three is a root and a store nothing reads back in the same iteration does not stall.

### GCC and Clang, and the one place this fix loses

MSVC is not the only compiler that builds this, so the same before-and-after ran on the Linux GPU
server — real GCC 14.3.1 and clang 22.1.8, RelWithDebInfo, on a Sandy Bridge Xeon, at x86-64-v2
because that is the only baseline that machine can execute. Alternated the same way, six rounds,
and a second run reproduced every figure to **0.5%**. Nanoseconds per call:

| Benchmark | GCC before | GCC after | Δ | clang before | clang after | Δ |
|---|---|---|---|---|---|---|
| `local_to_model.one` | 1,616 | 1,889 | **+17%** | 1,577 | **973** | −38% |
| `skinning_matrices.one` | 1,238 | **765** | −38% | 1,086 | **761** | −30% |
| `skinning.one` (both) | 2,856 | **2,637** | −8% | 2,674 | **1,735** | −35% |
| `skinning.crowd` (µs) | 756.5 | **704.7** | −7% | 711.1 | **466.3** | −34% |
| `local_matrices.one` (untouched) | 525 | 527 | 0% | 647 | 646 | 0% |

**`skinning_matrices` improves on all three compilers** — 38% on GCC, 30% on clang, 58% on MSVC —
which is the kernel with no loop-carried dependency and the clearest case.

**`local_to_model` is 17% slower under GCC, and that is a real cost, not noise.** Subtracting the
untouched `mat4_from_transform` row isolates it to the composition: 1,091 → 1,363 ns on GCC
(**+25%**) against 930 → 327 ns on clang (**−65%**) for the same source. GCC was already
auto-vectorizing that loop well and the explicit intrinsics take the choice away from it. The
likely mechanism, not yet confirmed by reading GCC's output: `const f32* b = matrix.data()` takes
the address of a local that GCC otherwise keeps in registers — it inlines `mat4_from_transform`,
where MSVC calls it out of line — so the intrinsic loads force a round trip GCC did not have.
Confirming that, and finding a form that does not, is the open follow-up on this section.

**The module still comes out ahead on every compiler**, because `local_to_model` is the smaller
half of what a skinned instance pays: the combined kernel is **−8% on GCC**, −35% on clang and
−40% on MSVC, and the 256-instance streaming row moves the same way. The trade was taken with the
numbers above in hand rather than by accident, and if the GCC case is ever what matters most, this
table is where to start.

### Determinism, and what a v2 and a v3 build disagree about

Within a build the kernels are **bit-identical across runs and across worker counts**: they are
per-instance sequential loops with no reduction, no accumulation order to vary and no state, and
`systems/animation`'s bit-identical-worker test covers the path that reaches them.
`anim_tests.cpp` runs one twice into different storage and compares with `==`.

**Across baselines is a different question, and the answer is "no, and it already was no."** On
MSVC the two agree: `/fp:precise` does not contract, so v2 and v3 emit the same operations in the
same order. On GCC 14 and clang 22 they do not, because `-march=x86-64-v3` brings FMA and the
default contraction turns `a*b + c` into one rounding instead of two. Compiled on the Linux server
and counted:

| | GCC 14.3 | clang 22.1 |
|---|---|---|
| this translation unit, `-march=x86-64-v2` | 0 FMA | 0 FMA |
| **before** this change, `-march=x86-64-v3` | 102 FMA | 81 FMA |
| **after** this change, `-march=x86-64-v3` | 88 FMA | 81 FMA |

So a v3 build's skinning matrices have always differed from a v2 build's in the last bit on those
compilers, this change did not introduce it, and it slightly *reduces* it on GCC — explicit
intrinsics leave the optimizer less to contract than free-form scalar float does.

**Nothing depends on cross-baseline equality today**, and two things nearly do. The GPU skinning
test compares the shader against this CPU reference to a tolerance, which a last-bit difference
does not spend. `input::InputLog` replays bit for bit, but replays *input*, not poses. What will
depend on it is a recorded gameplay replay shared between machines — plan 05's determinism stance
— and the recommendation for whoever writes that is: **record the build's baseline in the replay
header and refuse a replay taken at a different one**, the way `ActionMap`'s hash already refuses a
binding set that does not match. That is cheaper and more honest than pinning `-ffp-contract=off`
tree-wide, which would cost every kernel that legitimately wants FMA in order to make one
comparison work.

**Public API.** `domain/anim/skeleton.h`: `k_no_joint`, `Skeleton`, `compute_inverse_bind`, `Pose`, `PoseView`, `ConstPoseView`, `rest_pose`, `blend`, `make_additive`, `blend_additive`, `local_to_model`, `JointMatrix`, `joint_matrix`, `mat4_from_joint`, `transform_point`, `skinning_matrices`, `skin_positions`. `domain/anim/clip.h`: `k_interp_linear`/`step`/`cubic`, `k_channel_translation`/`rotation`/`scale`, `Track`, `WeightTrack`, `Clip` (`add_track`, `add_weight_track`, `sample`, `sample_weights`, `clip_time`, `validate`). `domain/anim/standard_skeleton.h`: `StandardJoint`, `k_standard_joint_count`, `standard_joint_name`, `standard_joint_from_name`, `JointMapping`, `map_joints`, `RetargetJoint`, `Retarget`, `build_retarget`, `retarget_pose`.

**Depends on.** `base`, `containers`, `math`, `time` (`GameTime`, so a clip can be sampled on the engine's own clock), `log` (one record when a retarget is built, naming how many roles matched — a content-quality signal a loader should not have to invent an error for), and `geometry`. The last one deserves a sentence: `geometry::SkinBinding` is the per-vertex format, and `skin_positions` is the CPU reference for the shader that reads it, so anim takes a dependency on a module in its own layer rather than keeping a private copy of an eight-byte record that could drift from the one on disk.

**Invariants (tested).** A joint's parent comes before it and is a joint; the pose arrays are the same length; a clip's track runs stay inside its `times` and `values`, its keyframe times do not go backwards, a rotation track has four components, and no track names a joint outside the clip; at bind, every skinning matrix is the identity; retargeting a skeleton onto itself reproduces the pose exactly.

**Benchmarks.** `tools/dev.ps1 bench -Preset msvc-release -Filter 'anim.skeleton.*'` —
`bench/anim_bench.cpp`, the two matrix kernels with no ECS, no pose pool and no world above them,
on the same 23-joint chain `systems/animation`'s bench uses, plus a branching humanoid for
contrast and a 256-instance streaming row so a fix that wins in L1 by spending bandwidth cannot
hide. The numbers and the machine's state are above.

**Testing.** `tools/dev.ps1 test -Filter anim`. `anim_tests.cpp` builds the two-bone skeleton the assets fixture is skinned to and checks the inverse binds, the composed model transforms, and the identity skinning matrices at bind; then samples a clip with one track of each mode against values worked out by hand — STEP holding until 0.5, LINEAR rotation at half a quarter turn giving `w = cos(22.5°)`, CUBICSPLINE with zero tangents giving the smoothstep 1.5 at the middle and 1.15625 a quarter of the way — plus looping, clamping, the `GameTime` overload, and the fact that a clip leaves the joints it does not animate alone. The blending case pins both ends exactly, clamps a weight outside 0..1, round-trips an additive layer, and checks that mismatched lengths write nothing. The skinning case skins the bar at bind (the rest pose comes back exactly, whatever the weights) and with the tip turned a quarter turn, against hand-computed corners and against the weighted average of the two matrices for the half-and-half row. `retarget_tests.cpp` maps a Mixamo-shaped rig and a `thigh_l`-shaped one onto the same roles, keeps a finger from claiming the hand, retargets a rig onto itself and gets the pose back to 1e-5, and retargets onto a rig that is twice as tall and differently named: the hips' deviation scales by two, the spine takes the source's deviation composed onto the *target's* bind orientation rather than the source's absolute one, an unmapped joint keeps its bind transform, and two skeletons sharing no role are refused. The morph case adds a weight track over two channels starting at channel 1 beside a joint track, and checks that one `sample` call does both, that channel 0 — which nothing animates — keeps the caller's default, that a span narrower than the track takes the part that fits rather than writing past the end, and that a track naming channels outside `morph_count` fails `validate` with its own sentence. The size table pins `JointMatrix` (the GPU record), `Track`, `WeightTrack`, and `RetargetJoint`.

**Performance notes.** Sampling and blending are per-instance per-frame work, so the layout above is the whole design: three streaming passes per blend, one contiguous walk per clip, no allocation once the pose is sized. `skin_positions` is a reference, not a hot path — the GPU does the skinning of anything on screen, and a CPU caller skins a proxy of a few hundred vertices. The one number worth watching when a system layer arrives is the bone-matrix upload, which is 48 bytes a joint per instance per frame and is why the 3×4 form was chosen.

**Next.** A cursor per track, so sampling a clip forward does not rescan its keys — that belongs with the playback state, which now has an owner: [`systems/animation`](animation.md) is the capability that ticks this module, and it owns the pose pool, the clip library, the playhead and the LOD policy. Still open here: blend trees and state machines as data, IK for feet and hands, a contact channel on the clip, and motion matching. On the renderer side, the missing piece is the engine-view flag that drives a skinned instance's bone matrices from a clip — the deform pass and its `k_deform_skin` mode are built and tested, `systems/animation` now produces the matrices as one contiguous span, and nothing joins the two outside a test yet.
