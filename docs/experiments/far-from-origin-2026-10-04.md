# Far from the origin (2026-10-04)

The owner set the fly speed to 10,000 m/s, flew the endless desert out to about 460 km and dropped into walk mode at (−419070, 80.24, −66781.11) (`D:\workspace\game_engine_local\flythrough\endless-2026-10-04T1710-frames.jsonl`, `8be7e369`). The ground's geometry refined as the tiles arrived; **the ripples stayed "very low resolution"**. A game with fast travel or a teleport lands a player exactly there.

This page has two parts. **Part 1** is what the low resolution was and the fix ([gfx](../subsystems/gfx.md), "Far from the origin"; [renderer](../subsystems/renderer.md#the-sand-far-from-the-origin)). **Part 2** is a survey, not a fix: every other place an absolute world position is a float32 on the way to the picture or the game, with what it does at 420 km and at 10,000 km, as the facts for the large-world decision.

A float32's step between 2^18 and 2^19 m (262 to 524 km) is **3.125 cm**, and between 2^23 and 2^24 m (8,389 to 16,777 km) it is **1 m**. The owner's z, 66.8 km, is between 2^16 and 2^17, where the step is 7.8 mm.

## Part 1: the ripples at 420 km

### It was the coordinate, not the tiles

The record's last 400 frames, standing and walking at 420 km, all have `terrain.layout_lag_m` 0 and `chunks_built` 0, with `pending`, `requests` and `loads_in_flight` 0. The rings were laid where the camera was, nothing was waiting to be built, and the finest ring (50 cm a sample within 48 m) was what was drawn. The coarse-level reading is ruled out.

The offscreen reproduction (below) waits for every tile before it draws (`time_lapse.wait` true, `rings.max_lag_m` 0). It shows the same thing the owner saw.

### The instrument

`ground detail: 420 km and 10,000 km out the resolve draws the origin's function` (domain/gfx `ground_detail_tests.cpp`) draws sixty metres of the ergs' sand at the owner's spot. It holds every pixel to the CPU mirror in double at the point the pixel sees. Along each view's middle row it counts three things: how often the GPU's ripple height changes, how often the reference's changes, and how often the point's float32 world coordinate changes. It draws the same sand twice, once with the frame and once with the frame left at the world's origin, which is the old arithmetic.

At the owner's pixel density (the "millimetre" look, 1 mm a pixel), along 17.5 cm of sand:

| | Ripple height changes | Worst ripple height, of 255 | Worst grain, of 255 | Worst shaded, of 255 |
|---|---|---|---|---|
| Reference (double) | 149 | — | — | — |
| GPU, pattern at the float32 world coordinate (before) | **10** | 212 | 177 | 24 |
| GPU, with the frame (after) | 148 | 1 | 1 | 1 |

The float32 coordinate changes **10 times** along that row: x every 3.1 cm and z every 7.8 mm. **The GPU's pattern changed exactly as often as the coordinate did.** That is 57 distinct pattern coordinates a metre, where the pixels ask for 850. Over the 1.9 m "at the feet" row it is 92 coordinate changes and 87 GPU changes against the reference's 156. Over all three looks the old arithmetic was 212 to 251 of 255 off on the ripple height, 177 to 210 off on the grain, and 21 to 28 off on the shaded picture (12,622 of 25,600 pixels past the tolerance at the feet).

The grain was not drawn at all. The precision fade counts four float steps (12.5 cm) as footprint, and that is larger than every grain octave.

### The captures

Each capture is 3840×2160 at the owner's pose: 15° down, 55° field of view, eye 1.65 m over the sand, `--time-of-day 10`, traced shadows. Release build, offscreen. The files are in the author's scratch directory (paths in the change's report). Crops are 800×450 at full resolution, from the lower right.

- **Before**: staircased crests in 3.1 cm squares, a regular lattice of light and dark blocks, and no grain.
- **After**: the erg's ripples with their defects, the grain between them, and the same sand the walk's start near the origin draws.

What the "after" crop still shows: one line across the crests where they kink. It is in the "before" capture at the same place. Part 2 ("The rasterizer") is its likely cause.

### The fix and what it guarantees

The fix is described in [gfx](../subsystems/gfx.md) under "Far from the origin". The detail is evaluated in a frame whose origin is the corner of a 1,024 m grid nearest the eye. Where each lattice and lane direction stands at that origin is worked out in double on the CPU. The shader sees only small local coordinates plus exact integers.

What holds, and how it was checked:

- **The origin's tolerances far out.** The far case holds 1 of 255 on the shaded picture and at most 2 on the detail view at 420 km and at 10,000 km, on level sand and on a 32° slip face, at three looks.
- **No seam where the frame's cell changes.** The 420 km sand runs across a boundary of the frame's grid, with the walker's two eyes in the cells either side. Every view of every case is also drawn with the neighbouring cell's frame, and differs from its own by at most 5 of 255 on the raw detail view (4 before the ripples' travel became one phase at every scale; the test holds 6, which is the eye's 2 against the mirror plus the neighbour's 4).
- **Old sites improved.** 3.7 km out, the old sites now measure what the origin does.
- **Near the origin.** A frame at the origin is the old arithmetic, and every eye within 512 m of the origin gets it. Main's shader and this one were captured in the same tree, both at 3840×2160 with traced shadows:
  - At (−300, 1.65 m over the ground, 200), a walker's view, **no pixel differs**. No slip face is in that view, so the lanes' new stagger is not exercised there; it moves a tongue by under a micrometre.
  - At the erg walk's start, 1.4 km out, 320,703 of 8,294,400 pixels (3.9%) are one code off and 12 are two off. The causes are float rounding of the new local coordinate and the finest grain octave, which the old precision fade had removed at that distance.
  - On the owner's slip-face marker, 1.8 km out, 23,517 pixels are one code off and none more.
- **The mirror moved with the shader.** It takes every lattice from the world position in double, with indices wrapped to 32 bits as the GPU's are. The lanes' stagger is in 32-bit fixed point on both sides.

**Not checked:** the baseline tier. The shader uses `int2` arithmetic and 64-bit addresses, which the resolve already uses, and no `float64`. It was run on the RTX 5090 only.

### What it costs

Measured the way the fifth pass was ([sand-fifth-pass](sand-fifth-pass-2026-10-04.md#the-cost)):

- RTX 5090, `msvc-release`.
- `engine-view --benchmark` offscreen over the erg's `walk-path.json`: 1,201 frames, `--repeat 2`.
- 11520×2160 `surround3`, `--shadows csm`, `--wait-quiet 60`.
- Each batch under `tools/gpu-lock.ps1 run`.
- One session. "Main's shader" is main's three `.slang` files restored into this tree and rebuilt. The C++ side writes the 560-byte block, of which main's shader reads the first 224, which are the same.

The walk is 1.4 km out, so the frame's origin is −1,024 m and the local coordinate is within about 400 m.

| Detail | Resolve, median (two runs) | p95 | p99 | Frame, median | The detail costs |
|---|---|---|---|---|---|
| off | 1.215, 1.223 | 1.253, 1.259 | 1.309, 1.315 | 2.26 | — |
| main's shader | 2.044, 2.044 | 2.073, 2.072 | 2.371, 2.370 | 3.08 | **0.825** |
| with the frame | 2.098, 2.098 | 2.126, 2.126 | 2.449, 2.444 | 3.13 | **0.879** |

**The frame costs 0.054 ms at the owner's resolution, 6.5% of the detail.** The fifth pass measured the detail at 0.838 against an "off" of 1.154. The "off" here is 1.219, and main's shader 0.825 against it.

The cost was not taken apart. The candidates:

- a `GroundDetail` 80 bytes longer, read by value;
- the lane table's load on slip faces;
- an integer add per lattice point;
- the local point's arithmetic.

**The machine.** The GPU was 6 to 11% busy at the start of every run, with 7.7 to 8.2 GB of others' memory, and other processes took 8 to 41% of the CPU. The harness found no quiet minute (`quiet` false). Three runs (both of main's shader, the second with the frame) ended with another process at 99% of the GPU, so their last frames are upper bounds. Each pair's medians agree to the microsecond.

## Part 2: what else is a float32 absolute

### What the plan says

[02 §2.7](../plan/02-architecture.md) sets the hook for worlds beyond about 10 km: "positions are (tile index, float local offset); rendering uses camera-relative transforms; no absolute float32 world coordinate exists anywhere". The same rule appears in:

- [03 §3.7](../plan/03-data-model.md), the spatial partition;
- [13](../plan/13-reference-consumer-games.md), a requirement row;
- [ADR-0017](../adr/0017-no-hidden-limits.md), in its decision text.

[ADR-0050](../adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md) decision 3 makes the ground's lattice exact in float32 to 4,000 km at 25 cm or coarser. No ADR records a departure from the (tile, local offset) rule. Nothing names a floating origin, origin rebasing or f64 positions.

**The code does not follow the rule.** Almost every position below is an absolute float32. These places protect precision:

- the resolve's eye-relative reconstruction ([gfx](../subsystems/gfx.md), "Measured from the eye");
- ray and sky directions from `clip_to_ray`;
- the sky's split radius;
- the f64 inside the cascade fit;
- integer-millimetre tile coordinates and lattices;
- since today, the ground's detail.

### The two a player meets first, measured

**The eye moves in float steps, and a walker loses motion.** The owner's record holds 3,589 frames at 420 km below 100 m, walking and flying low (frames 2067 to 5655):

- **Every x is a multiple of 3.125 cm, and every z a multiple of 7.8 mm.**
- Of the frame-to-frame steps, 59% move x by nothing and 51% move z by nothing. The rest are whole steps: x by ±3.1, 6.3, 9.4 or 12.5 cm.

The walk integrates each 240 Hz tick as `feet += direction × speed × dt` in float32. This happens in the ground-follow path (`walk.cpp:518`) and in the physics path, where Jolt is built single-precision (`EnginePhysics.cmake:108`). So a tick's step smaller than half a float step is lost, and a larger one is rounded to whole steps.

Frames 5256 to 5266 show it. The walker sprints at yaw −2.748 (forward (0.384, …, 0.923)):

- z advances 0.46 m, two 7.8 mm steps a tick, which is 3.75 m/s;
- x does not move at all, where 0.19 m was asked for.

The sprint's 20.8 mm a tick has 8.0 mm along x, under half of x's 3.1 cm step. **The walker went straight along +z at 3.75 m/s instead of at 22° to it at 5 m/s.** At the walk's 1.5 m/s, a tick is 6.25 mm, so walking along x at 420 km does not move at all. At 10,000 km a tick needs 0.5 m to move (120 m/s at 240 Hz), so nobody walks.

The record's `walk.max_ground_error_m` is **0.0306 m**, one float step, between the walker's held height and the drawn surface at his float32 feet.

**The rasterizer decides coverage through the whole matrix.** The vertex transform is `mul(view_proj, world)` on absolute float32 positions, in all four rasterizers. The resolve reconstructs the point eye-relative on the triangle the visibility buffer names, and moves it onto that triangle's edge when the pixel's ray misses the triangle. The count of such clamped points is how far the rasterizer drew edges from where they are.

Clamped points per 160×160 view in the far case:

| Place | Clamped points per view |
|---|---|
| By the origin | 0 to 1 |
| 3.7 km out | 0 to 6 |
| 420 km out | 44 to 577 |
| 10,000 km out | 18 to 523 |

The mirror clamps the same way, so the detail still holds its tolerance. But a pixel clamped onto an edge shows the edge's point, not its own: a band along triangle edges whose width is the rasterizer's error. [gfx](../subsystems/gfx.md) measured that error at 1.5 cm 50 km out. It grows with the distance from the origin.

**Traced shadows and tile seams at 420 km.** These are captures at 3840×2160, release.

- **Traced shadows.** In the shadow view (`--view shadow`, a 17:00 sun, the owner's pose), the ground near the eye is lit everywhere and shows no acne. Far ridges show the dotted dark specks the endless desert's README describes by the origin, about as many as the same view at the erg walk's start.
- **Tile seams.** No crack or line where tiles meet was seen in the owner's view, or from 72 m over the same spot looking to the horizon.

Neither says how close they come. Two things were not swept:

- the shadow ray's offset (`shadow_bias`, from the scene's bounds, plus grid steps) against the origin's 1.6 cm horizontal rounding on a slope;
- the renderer's seam tests, which run by the origin and not 420 km out.

The one line the "after" capture shows lies on a triangle edge of the 50 cm ring (`--view tri`). It is where the rasterizer's band would be.

### The survey

Steps: 420 km, 3.125 cm (worst rounding 1.6 cm); 10,000 km, 1 m (0.5 m). "Abs f32" means an absolute world coordinate in float32.

| What | Type | At 420 km | At 10,000 km | What a player sees, or a test catches |
|---|---|---|---|---|
| Camera: `renderer::Camera::position`, `target` | `Vec3` abs f32 | eye on a 3.1 cm grid; target 100 m ahead, so 3e-4 rad of direction | 1 m grid; 0.6° of direction | the eye's steps and lost motion (above); the view's direction rounding |
| Fly controller `FlyState::position` (`fly_tick`, `fly_interpolate`) | `Vec3` abs f32, f32 add per 240 Hz tick | a tick under 1.6 cm is lost: below 3.75 m/s along x nothing moves | below 120 m/s along an axis nothing moves | flying slowly sideways stalls; the owner's 10,000 m/s hid it |
| Walk controller `Walker::feet`, Jolt character (`RVec3` = float) | `Vec3` abs f32, f32 add per tick | measured above: x frozen, z at whole steps | no walking | a walker goes the wrong way and the wrong speed; `walk_view_tests` would catch it only at those distances |
| `ResolveParams::camera`, `CullParams::camera`, `RayVisibilityParams::camera`, `PathTraceParams::camera` | `Vec4` abs f32 | the eye's 3.1 cm | 1 m | carried into each pass below |
| Resolve's world position `eye + from_eye` (shadows, lights, sky's light) | float3, one rounding | 1.6 cm | 0.5 m | shadow ray origins (below); the detail no longer reads it |
| `gfx::InstanceDesc::world` | `Mat4` f32, absolute translation (identity for terrain) | a placed mesh's vertices on a 3.1 cm grid | 1 m | props deform by up to half a step; ADR-0050 keeps terrain exact |
| Terrain tile vertices (world-space, identity instance) | f32 from integer mm (`terrain_tiles.cpp:357`) | exact (dyadic lattice, to 4,000 km) | not exact past 4,000 km at 25 cm | none at 420 km; tile seams' tests run near the origin |
| 16-bit position grid `MeshDesc::quant` origin; deform pool's terrain stage | abs f32 origin; pool f32[3] abs | lattice points exact; anything off-lattice on 3.1 cm | 1 m | deformed props |
| Tile source sampling (dunes provider) | lattice index i64 mm, pushed through f32 metres (`registry.cpp:15`, `dunes.cpp:190`) | exact for dyadic spacings; the walker's 1 mm lattice is not exact past 16 km; the walk's i32 mm index overflows past 2,147 km | as at 420 km, plus overflow | the walker's ground probes |
| Cull (`cluster_cull.slang`): frustum planes, cone apex, LOD spheres, occlusion corners | abs f32; plane w ≈ \|eye\| | planes off by centimetres against cluster radii of metres | 1 m off: clusters at the frustum's edge culled or kept wrongly | a cluster missing at a screen edge at 10,000 km |
| Rasterizers' `mul(view_proj, world)` | abs f32 | measured above: 44 to 577 clamped pixels a view | 18 to 523 | bands along triangle edges near the eye; the far case's clamp count |
| TLAS instance transforms (`TlasInstance`, written once at upload) | f32 3×4 abs (identity for terrain; BLAS vertices abs f32) | as the instances | as the instances | traced shadows of props |
| Shadow ray origin, `s.position + n × ray_offset` | abs f32; `shadow_bias` sized by the scene's bounds, not the eye's distance | origin off by 1.6 cm horizontally, so on a slope s by 1.6 s cm vertically | 0.5 m | acne or leaks (measured above) |
| Cascades (`fit_shadow_cascades`, `shadow_snap`, `make_shadow_cascade`) | f64 fit, then f32 matrices with absolute translations and texel snapping in absolute f32 | snapping in 3.1 cm steps against texels of centimetres: shimmer | the maps misregister by a metre | `--shadows csm` at 420 km |
| Sky (`sky.cpp:273`): altitude from `camera.position.y` | f32, y only | none (y ≈ 80 m) | none; the world is flat | nothing until the world curves |
| Lights `ResolveLight::position_radius`, `SceneData::center` | abs f32 | 3.1 cm | 1 m | stand-in lights sit at the scene's centre, not the eye |
| Collision: Jolt single precision, heightfield `offset`, `ground_height(f32 x, f32 z)` | abs f32 | the ground query is at 3.1 cm feet | 1 m | `max_ground_error_m` 0.031 (above) |
| World ring observers, terrain layout inputs (`camera_x`, `camera_z`) | abs f32 in, i64 mm / i32 tile out | tile choice from 3.1 cm feet: harmless | harmless (i32 tiles of 32 m overflow at 68.7 million km) | none |
| Document and game data: `engine.world.Transform.position`, scene `Instance.translation`, `CameraKey.position`, sim entity, NPC | `vec3` abs f32 | a placed object snaps to 3.1 cm | 1 m | authored content far out moves when saved |
| Session, trajectory, `FrameRecord::pose_position` | f32 written as exact JSON doubles | replay is bit-exact; the poses carry the steps | 1 m | the record shows the steps (above) |
| Ground detail | frame origin plus exact integers (today) | exact | exact | the far case |

### Proposal (not a decision)

**The cheapest design that meets 02 §2.7 is a render origin plus doubles on the CPU.** It does not need tiles everywhere.

1. **Hold the camera, the controllers and placed objects in f64 on the CPU.** This covers `FlyState`, `Walker::feet`, `renderer::Camera`, `engine.world.Transform` and the session's poses. It is the change a player feels first. The 240 Hz integration then keeps every tick to 10,000 km and beyond.
2. **A per-frame render origin O.** O is the eye snapped to a coarse grid, as the detail's frame is. Upload everything the GPU sees relative to O, in f32:
   - instance translations as `world − O`;
   - the view matrix from `eye − O`;
   - terrain tiles as integer-mm vertices relative to their tile's corner, with an instance translation `corner − O`;
   - TLAS instance records rewritten only when O moves, every kilometre or so.

   The cull, the rasterizers, the shadows, the cascades and the lights then work on numbers the size of the view. They need no shader change beyond what they read.
3. **Physics.** Either build Jolt with `JPH_DOUBLE_PRECISION`, or shift its world with O. The first costs Jolt's own double arithmetic and is one switch. The second needs every body moved on a shift.
4. **The document model** stores (tile, local) or f64. A schema version and a migration.

Order by what a player meets first: (1) and (3) for the walk, then (2) for the picture. The ground's detail needs nothing more: its frame can become the render origin's.

What it does not need: f64 on the GPU. The baseline tier is a Maxwell and a Pascal, where it is slow, and nothing above wants it once positions are relative.

## Part 3: placements

*Added 2026-10-06 by the placements change ([scene_gen](../subsystems/scene_gen.md#placements-far-from-the-origin)).* The survey's row for the scene generators: a placement was a float32 `Transform3`, an absolute world position, and the ground under it was asked at float32 metres. What it cost, and what it costs now, measured by the tests that hold the change, with the old arithmetic put back in the tests' generators for the "before" column (the generator computing its place in float32 world coordinates and asking the ground at the float32 metre; not committed). MSVC `msvc-debug`, 2026-10-06; these are positions, not timings, so the machine's load does not enter them.

**Where a placement lands** (`renderer: a placement lands within 2 um of where its generator put it, far out too`): a box 16.123456789 m by -7.654321098 m from the site, turned 30°, lifted 1.25 m off a sawtooth ground of millimetres, under a fit like a content mesh's; the distance from where the generator put the mesh's origin (its `WorldPos` composed with the fit in f64) to where the instance's cell and local put it.

| Site | Before | After |
|---|---|---|
| origin | 0.91 µm | 0.83 µm |
| 419,072 m | 3.18 mm | 0.83 µm |
| 10,000,000 m | 0.387 m | 0.83 µm |
| 100,000,000 m | 0.387 m | 0.83 µm |
| 0.4 µm either side of a cell's edge, at each site | 1.19 µm | 1.12 µm |

Before, the error is the float32's step at the site on x and z (0.123 m and 0.346 m of the box's offsets at 10,000 km, where a float steps by a metre, and at 1e8 m, where it steps by 8 m and the offsets happen to round the same way) and the sawtooth's height at the rounded millimetre on y (0.123 m). After, it is the fit's offset turned in float32 and the local's rounding at the box's place in its cell, the same at every site.

**Two pieces laid to touch** (`renderer: two pieces laid to touch far out leave no gap`): a box 0.6 m wide and one 0.8 m wide whose faces meet 0.3 m short of a cell's edge, the boxes on either side of it; the faces measured in the frame of an eye at the seam, as every pass measures a vertex. Positive is a gap, negative an overlap.

| Site | Before | After |
|---|---|---|
| 419,072 m | -12.5 mm | -1.55 µm |
| 10,000,000 m | +0.300 m | -1.55 µm |
| 100,000,000 m | -0.700 m | -1.58 µm |

**In collision** (`scene_collision: a streamed placement collides where its generator put it, far out`): a streamed box 16.123456789 m into its site's tile, its west face met by a ray along +x; the hit's distance from where it is by the origin.

| Site | Before | After |
|---|---|---|
| 419 km | 1.54 mm | 6e-12 m |
| 10,000 km | 0.123 m | 7e-10 m |
| 1e8 m | 0.123 m | 7e-9 m |

What the after column rests on: a placement's position is a `WorldPos`; a reader adds the mesh's fit to it in f64 and stores a cell and a local (`renderer::instance_translation`); the collision adds the same in f64 and subtracts the tile's corner; and the ground is asked at whole millimetres, which reach the field without a float. The ruins still hand over float32 world metres at one marked helper until they place in f64 (the next stage); the city places from its integer centimetres in f64.
