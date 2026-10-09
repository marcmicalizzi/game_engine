# Traced-shadow shards on the far ground (2026-10-09)

- **Question.** On the erg's western floor, with `--terrain-rings` and `--shadows rt`, the owner's surround drew dark grey shards on the sand far from the camera ([the sand-detail captures](sand-detail-2026-09-29.md#captures); [far ground](far-ground-2026-10-03.md#what-is-open) saw specks too). The suspect written down was shadow rays stopped by the rings' own geometry: the scene grid's sunken hole under the middle ring, or a skirt. What stops those rays, and what is the fix?
- **Date:** 2026-10-09. **Machine:** the owner's desktop (RTX 5090), `msvc-release`, offscreen, every capture under the GPU lock. **Machine state:** other agents' builds held 20–31% of the CPU through the measurement, and 46% at the start of one of the timed runs, beside this session's own debug build (`--wait-quiet 300` ran out every time, `quiet: false`); the GPU was ours. The timings below are an upper bound on the cost, and the counts no load changes.
- **Status:** diagnosed, fixed ([renderer](../subsystems/renderer.md#one-cut-for-every-view), "One cut for every view"), and held by a test. What is left is a handful of single-view specks (F17) and a surround defect found on the way (F18).

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

- **Single-view specks (F17).** After the fix, 4 of the 5 blocked rays at 5760×1080 hit their own cluster from behind and 1 hit a neighbour this view drew: a ray from a point the resolve put on its triangle's edge (where a pixel's centre lies within the rasterizer's snap of it), under a neighbour across a concave crease. The far-ground page's specks along far tiles' borders are this kind; not measured again here.
- **The surround's side monitors and the camera's field of view (F18).** Found on the way, not fixed: see [renderer](../subsystems/renderer.md#multi-view-a-viewset-over-one-scene).
- **The maps in a surround.** The cascades already took the finest view's LOD, so a coarser side view's receiver is compared against a map drawn from a finer cut. The maps' receiver-plane bias and normal offset hid it here (0 pixels the maps darkened that the rays lit), and it is not changed.
- Brown dashes a few pixels long on the far floor in both the before and after captures are not shadows (they are lighter than the sand around them); not looked into.
