# Traced-shadow shards on the far ground (2026-10-09)

- **Question.** On the erg's western floor, with `--terrain-rings` and `--shadows rt`, the owner's surround drew dark grey shards on the sand far from the camera ([the sand-detail captures](sand-detail-2026-09-29.md#captures); [far ground](far-ground-2026-10-03.md#what-is-open) saw specks too). The suspect written down was shadow rays stopped by the rings' own geometry: the scene grid's sunken hole under the middle ring, or a skirt. What stops those rays, and what is the fix?
- **Date:** 2026-10-09. **Machine:** the owner's desktop (RTX 5090), `msvc-release`, offscreen, every capture under the GPU lock. **Machine state:** other agents' builds held 20–31% of the CPU through the measurement, and 46% at the start of one of the timed runs, beside this session's own debug build (`--wait-quiet 300` ran out every time, `quiet: false`); the GPU was ours. The timings below are an upper bound on the cost, and the counts no load changes.
- **Status:** diagnosed, fixed ([renderer](../subsystems/renderer.md#one-cut-for-every-view), "One cut for every view"), and held by a test. Of what was left, the surround defect found on the way (F18) is fixed and the single-view specks (F17) are re-diagnosed as fold-overs in the terrain's coarse clusters, which no ray offset can clear ([2026-10-10](#2026-10-10-the-census-again-f17-and-the-surround-at-the-cameras-field-of-view-f18)).

## Reproducing it

The camera the sand-detail page names: eye height on the western floor at (−1400, 0), looking west (a held two-key path, `fov_deg` 60 like the scene's own paths). The sighting predates the erg's sky, so its sun was the stand-in (azimuth 48.4°, **53.0° up**); a copy of `scene.json` without its `sky` block reproduces it. At that sun **no part of the erg can shadow itself**: the steepest sand is a slip face at about 34°, and a ray rising at 53° clears any slope under that. So in the shadow view (`--view shadow`) every sun-facing pixel should be white, and every black one is a shadow nothing in the scene casts.

| capture | sun-facing pixels black |
|---|---|
| 11520×2160 `surround3`, rings | **3,766**, all on the far ground, rows 877–949, in the right monitor and the centre's edge beside it |
| 5760×1080 `surround3` (1080p a monitor), rings | 3,190 |
| 5760×1080 `surround3`, **no rings** | 2,052 |
| 1920×1080 single view, rings | 1 to 3 |
| 11520×2160 `surround3`, the erg's own sky at 10:00 | 3,749 |

The maps (`--shadows csm`) lit every one of them. So the first two things the page said were not so: the shards draw without the rings too, and they are a multi-view effect — one view draws almost none. (The 1080p sighting was, by its size, a surround at 1080p a monitor or a crop; a single 1080p view draws one to three pixels, below.)

## What the rays hit

The resolve's shadow view was taught, for the measurement and not committed, to say what a blocked sun ray hit: its distance, whether the hit was the receiver's instance, its cluster, its visible entry, and whether that entry was this view's own (the view's pair-to-entry table names the entry it drew a pair as) — and whether the receiving triangle faced away from the camera. At 5760×1080 `surround3` with the rings:

| what the ray hit | rays |
|---|---|
| another instance (a skirt of another chunk, the grid under a ring) | **0** |
| the receiver's instance, a cluster **another view drew**, from behind | **3,185** |
| the receiver's instance, a cluster this view drew | 1 |
| the receiver's own cluster | 4 |

at 0.1 mm to 0.4 m from the ray's origin (a median near 3 cm), and **no** receiver faced away from the camera. Without the rings, 2,042 of 2,052 hit another view's cluster of the grid. The rings' geometry stops nothing: a ray leaves its surface and meets, a few centimetres on, **the same ground at another level of detail**, which another monitor's cut put into the same bottom-level structure.

Why the cuts differed. The structures are built from every view's visible list, one bottom-level structure per instance, and each view's cull took its own projection scale. A surround's views do not share one: a side monitor of a turned surround stands further from the eye along its own axis, and on a flat one — this sighting — the side monitors keep the field of view the layout was sized from (55°) while the centre takes the camera's (60°), so the side views' cut is 1.109 times as fine (F18). Two views' frusta both keep a far cluster whose sphere crosses the seam between them, and on the far ground the clusters are large, so a band tens of metres wide carries both levels. Where the coarser stands above the finer by more than a ray's offset, the finer's rays start under it.

## The suspects, in the brief's order

- **(a) A skirt or the grid's sunken hole as an occluder:** ruled out. Zero of 3,190 blocked rays hit another instance; the same shards draw without rings.
- **(b) The offset** (`ray_offset`: 2^-18 of the scene's reach plus one step of the receiver's own grid): ruled out as the cause. A ring chunk's positions come out of the deformed-vertex pool, which the structures are built from, so the receiver and its own structure agree exactly; the hits were on *another* level's triangles up to 40 cm away, which no offset sized by a grid should clear; and with the cut made one, the same offset leaves 6 black pixels in 24.9 million.
- **(c) Float32 precision two kilometres out:** ruled out. The resolve's point never leaves its triangle (its barycentrics are clamped onto it), float32 at 2 km is a fifth of a millimetre, and no receiver was back-facing.
- **(d) The far tiles' instance transforms in the top-level structure:** ruled out. Every hit was on the receiver's own instance, and the fix below changes no transform.

The reference path tracer could not triangulate this one: it draws one view, and one view's structures hold one cut.

## The fix

When a frame builds acceleration structures (`ResolvedSettings::rt_chain`: traced shadows, the ray path), every view's cull takes the finest view's LOD — its projection scale and threshold, the same view the cascades take theirs from — so the views' cuts are parts of one cut, bit for bit, and every receiver's surface is the surface its rays are traced against (`scene_renderer.cpp`, the cull pass's two blocks). Frames without the chain keep each view's own cut. The price: `--peripheral-lod` does not coarsen the side monitors under traced shadows, and a view whose projection is coarser than the finest draws finer than its pixels ask.

| 11520×2160 `surround3` | before | after |
|---|---|---|
| sun-facing pixels black, stand-in sun | 3,766 | **6** |
| the same, the erg's sky at 10:00 | 3,749 | **6** |
| 5760×1080 `surround3`, rings / no rings | 3,190 / 2,052 | 5 / 5 |
| a single 1920×1080 view | 1–3 | the same bytes |
| shaded picture: pixels darker than the other by more than 20 of 255 | 3,809 | 8 |

**Cost**, the erg's sky at 10:00, rings, traced shadows, 11520×2160 `surround3`, 300 frames of the held view, two rounds alternated: the frame's GPU median 3.533 and 3.527 ms before, 3.540 and 3.535 ms after (+0.007 ms, 0.2%); the pairs drawn 4,047 before and 4,103 after (+1.4%), the CLAS and BLAS builds +0.002 and +0.004 ms. Under the machine's noise.

## The test

`renderer: a turned surround traces its shadows against one cut and not three` (`systems/renderer/tests/renderer_tests.cpp`): the procedural heightfield (257 a side, 20 m across), which cannot shadow any part of itself under a sun straight overhead, in a 35° surround from 2.5 m over its edge, in the shadow view. Every ground pixel must be white. It requires the side views' projection scale to exceed the centre's by 5%, the precondition for two cuts. Without the fix (`msvc-debug`, RTX 5090): 386 of 81,843 ground pixels black, 181 in the centre monitor and 205 in the right, 391 pairs drawn. With it: 0, 404 pairs. Eleven seconds in `msvc-debug`.

## What is left

- **Single-view specks (F17).** After the fix, 4 of the 5 blocked rays at 5760×1080 hit their own cluster and 1 a neighbour this view drew. This page put them down to a ray from a point on its triangle's edge under a neighbour across a concave crease; the census of 2026-10-10 (below) found no such point, and the specks to be rays from under **fold-overs in the terrain's coarse clusters**, which no offset can clear.
- **The surround's side monitors and the camera's field of view (F18).** Fixed 2026-10-10 (below, and [renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene)): the surround is laid out from the camera's field of view, so a flat surround's three views share one projection scale.
- **The maps in a surround.** The cascades already took the finest view's LOD, so a coarser side view's receiver is compared against a map drawn from a finer cut. The maps' receiver-plane bias and normal offset hid it here (0 pixels the maps darkened that the rays lit), and it is not changed. Since F18 a flat surround's views are one scale and this case is a turned surround's alone; not looked at again on 2026-10-10.
- Brown dashes a few pixels long on the far floor in both the before and after captures are not shadows (they are lighter than the sand around them); not looked into, then or on 2026-10-10.

## 2026-10-10: the census again (F17), and the surround at the camera's field of view (F18)

**Machine:** the owner's desktop (RTX 5090), `msvc-release`, offscreen, every capture under the GPU lock. **Machine state:** other agents' builds and test runs on the CPU throughout (the budget runs below say how much); the GPU was ours for each capture. Counts are not timings, and no load changes them.

### The census, reproduced

The resolve's shadow view was taught again, for the measurement and not committed (the patch is in the session's scratch directory), to say what a blocked sun ray hit — the receiver's instance, cluster, entry and triangle against the hit's, whether the hit triangle was met from behind — and **where the ray left from**: on an edge (a barycentric the reconstruction clamped to zero, the rasterizer's snap), within one grid step of an edge, or inside. A second set of modes then read, per blocked pixel, the receiver's grid step, triangle size and distance; the hit triangle's corners' heights over the receiver's plane, its normal against the receiver's, the corners it shares with the receiver; and the same triangle at rest, off the mesh's grid, against the pool's. Same scene (the erg without its sky, the stand-in sun 53° up), same camera (`along.json`, 60°), rings, traced shadows, `surround3`, at `main` (`6f19f721`, F4's fix in):

| | 5760×1080 | 11520×2160 |
|---|---|---|
| sun-facing pixels blocked | **5** (the page's 5) | **7** (the page had 6) |
| ray left from an edge (the clamp) / within a grid step of one / inside | 0 / 1 / 4 | 0 / 2 / 5 |
| hit: a triangle of the receiver's own cluster / another cluster this view drew / anything else | 4 / 1 / 0 | 7 / 0 / 0 |
| the hit triangle upside down against the receiver (normal 105°–166° from it) / flat, above it | 4 / 1 | 4 / 3 |
| ...of those, overlapping already in the rest mesh / made by the pool | 5 / 0 | 6 / 1 |
| at | 0.55 to 1.4 km | 1.0 to 1.7 km |
| ray length to the hit | 2.6 to 10 cm | 0.3 to 20 cm |

Every receiver is a terrain triangle 1 to 26 m across (a deformed instance: the scene's grid or a ring), its grid step 2 mm or 9.4 cm. The hit triangles of its own cluster share an edge or a corner with it; the upside-down ones stand up to half a metre over its plane. The one the pool made had a corner pulled 3 m across onto a ring's hole edge and was upright at rest — the fold `deform.slang` documents and sinks. The rest overlap at rest: **the terrain's coarse clusters hold triangles folded over their neighbours**, and a ray leaving the lower one meets the fold, which the acceleration structure holds because the cluster does. The specks are shadows of real, wrong geometry. They are not a crease under a ray's offset: none left from an edge, and no offset sized by a grid step clears a fold half a metre over the receiver.

### The candidates

Each candidate was selected at run time for the measurement (an environment variable through `FrameLighting`, not committed), and each was captured on the census scene and on three scenes with real traced shadows, in the shadow view at 1920×1080: the erg under its own sky at 08:00 (204k pixels shadowed), `desert-dunes` and `desert-endless` on their own paths. Changed pixels are against the offset as it is.

| candidate | blocked at 5760×1080 | erg 08:00 | dunes | endless |
|---|---|---|---|---|
| the offset as it is | 5 | — | — | — |
| (a) move the origin toward the triangle's centroid until it is a grid step inside every edge, then the normal offset | 7: two of the five stay, three new on another instance, each from within a step of an edge | 0 | 0 | 0 |
| (a) with two steps | 9 | 1 | 2 | 3 |
| (b) one grid step more along the ray | 5 | 0 | 21 | 26 |
| (a) and (b) | 4 | 0 | 20 | 26 |
| one grid step more along the normal | 4 | 2 | 22 | 17 |
| (c) `RAY_FLAG_CULL_BACK_FACING_TRIANGLES`, or the front-facing flag | 5, the same five | 0 | 0 | 0 |

- **(a)** removes nothing, because nothing left from an edge, and where a triangle is a sliver it moves the origin a long way along it (the centroid of a thin triangle is metres from its edge), onto ground another instance covers.
- **(b)** and a second normal step leave four or five of five and move real dune shadows by 17 to 26 pixels: that is the price of a larger offset everywhere, for one speck.
- **(c)** changes nothing: every instance of the frame's top-level structure is built with `TriangleFacingCullDisable` (`gfx::TlasInstance`'s default), so a ray's facing flags are ignored. Taking it would be a change to the structures; it would then drop the shadow a one-sided thin caster throws from its back and part the rays from the cascades, which draw both faces.

None was kept, and `ray_offset` is unchanged. The reference path tracer offsets by the same function, so its agreement is unchanged too: on this tree, which changes no shader, the shading tests (`domain/gfx`, `msvc-debug`) hold the traced shadows to the CPU reference with the shadowed light removed at worst 1 of 255 (the sun's: 1,444 shadowed and 9,600 lit pixels; the point light's: 3,364 and 6,772; tolerance 2), and the attributes tests at worst 1 of 255. **What would remove the specks** is geometry that does not fold: the terrain's simplification refusing a collapse that turns a triangle over its neighbour (the LOD build, `domain/geometry`'s `cluster_lod.cpp` through meshoptimizer), and the ring's hole projection's fold, which is already sunk under the finer ring and was one ray of twelve here. A test that counts upside-down triangles in a heightfield's cut would hold it. That is a geometry change and was not made here.

### The surround at the camera's field of view (F18)

`ViewSet::update` now lays the surround out again from the camera's field of view whenever it is not the one the frusta were laid out from ([renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene)). On the census scene, whose path is at 60° beside the 55° layout, the side monitors now show the camera's field of view (2,792,060 of the 6,220,800 pixels of the 5760×1080 picture change, all in the side monitors) and every view takes the same cut, a coarser one than the side views forced on all three before; the census counts move with the cut (5 → 3 sun-facing pixels blocked at 5760×1080, 7 → 10 at 11520×2160), still all fold-overs.

The owner's two surround budgets (`tools/frame-budget.ps1 -Run endless-surround3-rt,erg-walk-surround3-rt`, under the script's own GPU lock and `--wait-quiet 600`), the same machine and build, `main` against `main` with the change:

| run | GPU frame, median / p99 | resolve | CLAS + BLAS | pairs drawn | machine |
|---|---|---|---|---|---|
| `erg-walk-surround3-rt`, before | 3.483 / 3.926 ms | 2.345 | 0.429 | 4,322 | busy: 6.5% others at the start, 34.2% at the end, GPU 0% |
| `erg-walk-surround3-rt`, after | 3.451 / 3.830 ms | 2.323 | 0.420 | 4,083 (−5.5%) | quiet: 9.5% and 7.1% others, GPU 0% |
| `endless-surround3-rt`, before | 4.378 / 5.355 ms | 1.872 | 0.571 | 9,395 | busy: 8.3% and 74.6% others, GPU 0% then 23% |
| `endless-surround3-rt`, after | 4.347 / 5.332 ms | 1.864 | 0.566 | 9,395 | busy: 9.5% and 7.1% others, GPU 0% then 27% |

The erg's walk is at 60°: its side views no longer force a 1.109-times finer cut on all three, so it draws 5.5% fewer pairs and its median is 0.03 ms lower, inside its noise but in the direction the pairs say. The endless desert's path names no field of view and takes the layout's 55°, so nothing changed: the same pairs, and the medians within noise of each other. Both stay within their budgets (3.482 / 4.010 and 4.454 / 5.571 ms); the budgets were not re-baselined.
