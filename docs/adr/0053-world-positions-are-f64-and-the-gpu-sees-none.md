# ADR-0053: World positions are f64 on the CPU, and the GPU never sees an absolute one

- **Status:** Accepted
- **Date:** 2026-10-05 (accepted 2026-10-06, after the three stages that built it were merged and gated)
- **Plan references:** docs/plan/02-architecture.md §2.7 (the row for very large worlds), docs/plan/03-data-model.md §3.7, [ADR-0017](0017-no-hidden-limits.md)
- **Docs touched:** `docs/subsystems/math.md` ("World positions"), `docs/plan/02-architecture.md` and `03-data-model.md` (status notes), `docs/experiments/far-from-origin-2026-10-04.md` (the measurements this rests on)

## Context

The plan has always said that no absolute float32 world coordinate exists anywhere: "positions are (tile index, float local offset); rendering uses camera-relative transforms" (02 §2.7, 03 §3.7, ADR-0017). The code never did it. On 2026-10-04 the owner flew the endless desert 420 km out and walked: the sand's ripples drew as a staircase, and the survey that followed ([far from the origin](../experiments/far-from-origin-2026-10-04.md)) found an absolute float32 almost everywhere a position travels. There a float steps by 3.1 cm: his eye sat on a 3.1 cm grid, a sprint 22° off an axis went straight down the axis, a walk along x at 1.5 m/s does not move at all, and the rasterizers drew triangle edges centimetres from where they are. A game with fast travel or a teleport lands a player exactly there.

The owner's instruction for the fix: whichever way is most correct and best for the people who build and run games on the engine; performance first, correctness in general second.

Three representations were weighed for a position on the CPU:

- **(cell, float32 local) everywhere**, the plan's wording. Uniform precision and float32 hot data, but every subtraction of two positions is a function call, and the mistake of subtracting two locals compiles, passes every test near the origin, and fails only when two things straddle a cell boundary. Unreal's world-origin shifting, which is this idea, was a standing source of bugs for the teams that used it and was replaced by doubles in UE5.
- **64-bit fixed point.** Uniform and exact, but with a smallest step that slow motion falls under unless every integrator carries a remainder, and foreign to every library the engine talks to.
- **f64.** Ordinary arithmetic that cannot be got wrong, what the physics library offers natively, what a JSON number already is, and the same 24 bytes as three (i32, f32) pairs. Its step is 2 nm at 10,000 km and 30 µm at the distance of the sun.

And the hot loops do not run on world positions: bones are model-local, particles emitter-local, contacts body-local, vertices mesh-local, the picture eye-local. A world position is one field of an entity's transform.

## Decision

1. **A world position is three f64, and it is a type.** `engine::WorldPos` (`core/math/world.h`) is a point; `engine::DVec3` is a displacement. A point minus a point is a displacement; a point plus a displacement is a point; two points do not add; and nothing narrows to float32 except `narrow(DVec3)` and `relative(point, origin)`, which are spelled out at the call. The compiler, not a review, keeps an absolute position out of a float.
2. **Float32 is for local frames, and stays there.** Mesh, bone, emitter, body and eye space are `Vec3` as before. A function that takes a `Vec3` position says in its name or its parameter which frame it is in.
3. **The GPU never holds or computes an absolute position.**
   - **What is stored across frames holds a cell and a local**: `engine::WorldCell`, an i32 cell index per axis on a 64 m grid and a float32 offset in [0, 64). `gfx::InstanceDesc` carries one in place of its matrix's translation. Sixty-four metres puts a placement within 2 µm of where it was authored, anywhere; i32 cells reach 1.37e11 m, which is where f64 itself has coarsened to 30 µm.
   - **What is computed in a frame is relative to the frame's origin, which is the eye.** A frame carries one `engine::WorldEye`: the eye's cell, its local, and the residual the local's rounding left, so the eye is exact to nanometres. A shader forms an instance's translation from the cells' difference times 64, which is exact, the two locals and the residual, summed in the order that keeps every intermediate the size of the result (`relative(WorldCell, WorldEye)` is that arithmetic on the CPU): the result is within three float steps at its own size, so precision is finest at the eye and falls off with distance exactly as a perspective picture needs. View matrices carry rotation and at most the small offset of a view's own eye from the frame's.
   - **Nothing is rewritten when the eye moves.** The picture stays a function of the scene, the camera and the clock, with no state that depends on where the camera has been. A rewrite of every instance each time an origin snapped would be a frame-thread spike a kilometre, and would make the picture depend on the snap.
4. **Physics runs in the library's double-precision mode** (Jolt's `JPH_DOUBLE_PRECISION`): body positions and queries in f64, collision arithmetic float32 relative to a base, which is the same rule as 2. Its cost is measured with the physics bench and written beside this decision; shifting a single-precision world under the player was rejected for the reasons in 3.
5. **Documents and files store positions as f64.** The schema language gains a double vector type; a JSON number already is one. A record's tile stays a function of the record alone.
6. **The test of all of it is translation**: a scene and its camera moved by a whole number of cells draw the same bytes, at 420 km, at 10,000 km and at 1e8 m, on every rasterizer and the ray path; a controller walks the same steps there as by the origin. An absolute float32 on any path fails it by centimetres, so the rule is held by a test and not by reading the code.

This supersedes the plan's "(tile index, float local offset)" as the form a position takes on the CPU. The rule it was written to guarantee, that no absolute float32 world coordinate exists anywhere, stands and becomes enforced.

## Consequences

- An entity's transform grows by 12 bytes. Hot loops that touch world positions in bulk (a broad phase, a cull on the CPU) work on float32 relative to a local origin and convert at their edge.
- Every GPU-mirrored struct that held a world matrix or a world point changes, with its size table and its shader mirror, and every reader of it changes in the same commit.
- Terrain tiles stop being world-space vertices under an identity instance: a tile's vertices are relative to its corner and its instance carries the corner.
- Session and trajectory files written before this hold float32 poses and replay under a new format version with different rounding; their hashes are not comparable across the change.
- Jolt's double mode costs what its bench says; the build has one precision, not a switch.
- Past 1.37e11 m a cell index does not exist and the conversion refuses. A game on that scale needs a hierarchy of frames, which this does not provide.

## Revisit when

- the physics bench shows double precision costing more than a tenth of a step on the reference scenes: then islands with local origins, under the same types;
- a game needs more than one star system: a frame hierarchy above `WorldPos`;
- a measured hot loop is found doing f64 arithmetic per element where a local frame would do.
