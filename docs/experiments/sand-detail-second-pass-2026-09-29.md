# The sand close up, second pass (2026-09-29)

**The question.** The owner walked the first pass of the sand's detail ([sand-detail-2026-09-29](sand-detail-2026-09-29.md)) on the RTX 5090 at 11520×2160 from 1.65 m, beside a photograph of a real dune, and found it "mostly good" with three faults:

- ripples over a crest on the side the wind blows toward;
- ripples uniformly tight everywhere;
- sand that looks pixelated at his feet.

This page holds what the second pass measured and what it could not ([renderer](../subsystems/renderer.md#the-sand-close-up), [gfx](../subsystems/gfx.md)).

**Where.** Everything was measured on a 4-vCPU Linux container with no GPU. The shader compiles there and does not run, so every number below comes from the CPU mirror, `domain/gfx/tests/ground_detail_reference.h`, in double precision. The mirror is the function the GPU test holds the shader to on the owner's machine. The tests are in `domain/gfx/tests/ground_detail_tests.cpp`. The pictures come from `ground_detail_preview_tests.cpp`, which is skipped unless run with `--no-skip`. What the GPU measured afterwards, on the owner's machine, is [the last section](#on-the-gpu).

## The instrument

The preview shades the mirror with `brdf_reference.h` under a sun 20° up, from upwind (`_along`) and from the side (`_across`), with the wind blowing +x. It draws:

- 2 m patches from above at 2 mm a pixel: flat, a 15° windward slope, a 15° lee slope, and a 32° slip face;
- a 25 cm patch at 0.25 mm a pixel;
- a walker's view over the flat;
- a walker's view of a dune from its downwind floor. The dune is 24 m wide, with a windward slope easing up to 12°, a rounded crest and a 32° slip face.

Each picture is drawn twice: `before_*` by the first pass's mirror, frozen as `ground_detail_reference_v1.h`, and `after_*` by the live mirror with the ergs' numbers. The masks sit beside them: the ripples' weight in red, the exposure in green, the streaks' weight in blue.

The whole set takes about 40 s in a debug build.

## What the pictures show (read by the author; the owner's walk decides)

- **`eye_dune`.** Before, the dune's lee flank, gentler than 22°, is rippled right down to the floor; after, it is smooth. The windward side and the floor are unchanged, and no line marks where one becomes the other. The mask shows the exposure falling across the brink and the flank as a soft band. The slip face carries faint streaks down its fall line, a few percent of albedo. They are clearest under the across sun, where the face is lit at a glancing angle.
- **`grain`.** Before, it was soft blotches a centimetre or two across with visible cell structure, the "upscaled image". After, it reads as a rough granular surface at every scale down to a millimetre, with no lattice. Under the grazing sun the grain's normal gives it relief. At 0.06 it reads a little like fine stucco at 0.25 mm a pixel, a scale no walker sees; at a walker's pixel it is texture.
- **`top_windward15`.** The ripples are visibly wider apart than on the flat (about 18 cm against 12), and the grain lies between them.
- **What does not read as sand yet.**
  - The ripples still have no self-shadowing at a low sun.
  - Nothing glints.
  - The streaks are ellipses summed, not the branching, narrowing tongues real grainflows are. From a few metres they read as streaks; close up they read as smooth smears. This is the term the owner's eye should judge first.

## Measurements

| What | Number | Test |
|---|---|---|
| Defaults (no v2 field) against the frozen first pass | identical at 4,000 probes: 0 differences in normal, albedo, roughness, weight, fade, ripple and grain | "the defaults are the first pass, to the bit" |
| Exposure's largest step over a sweep of 0.01° of lee slope | < 0.004 | "the ripples go by which way the ground faces the wind" |
| Gradient noise: value rms, slope rms per cell | 0.181, 0.746 (the constants are 0.182 and 0.743, held to 3%) | "the grain reads as sand" |
| Grain filter against 8×8 supersampling, from above, pixels of 2, 5, 10 and 30 mm | mean within 0.05%; variation never above the supersampled picture's | "the grain's filter keeps the mean and adds no sparkle" |
| Streaks: the block's closed-form scales against the measured sum | rms 0.98 of the target, slope rms 0.95 | "streaks run down a slip face and nowhere else" |
| Streak and ripple weights and albedo over a sweep from flat to 40° of lee | no step above 0.005 per 0.01° | same |
| A thousandth of a radian of normal, 3 km out | moves the tongues' sum by 1.2% of its rms | same |
| Streak filter on a 32° face, pixels of 5 to 40 cm | mean within 0.4%; variation at most 5% above the supersampled picture's | "the streaks' filter …" |
| Spacing: zero crossings over 40 m | 666 at scale 1, 424 at scale 1.53 (ratio 1.57) | "the ripples' spacing follows the wind …" |
| Slide per 0.1° of normal on a 12° windward slope, gain 2.5 | one kernel's edge: 0.0086 wavelengths; the crests, measured: 0.0127 (limit 0.05) | same |

### Precision far from the origin

The resolve's position is a float. At 3.7 km its step is 0.244 mm, and the reconstruction is off by a few steps. Each grain octave was evaluated at positions rounded to float and compared with exact positions. The error is given as a share of the octave's own rms:

| Octave | 900 m | 3.7 km |
|---|---|---|
| 20 mm | 0.3% | 1.1% |
| 10 mm | 0.6% | 2.4% |
| 5 mm | 1.2% | 4.6% |
| 2.5 mm | 2.3% | 9.1% (kept at 0.41) |
| 1.25 mm | 4.5% | 18% (gone) |

So each octave also fades by four float steps as footprint. The step is read exactly from the position's exponent bits, on both sides. At 3.7 km the finest octave that still holds whole is 5 mm; within 900 m every octave holds to 5%.

### Cost

These are counts, since no GPU ran here.

- **Grain.** An octave of the gradient grain is four lattice hashes of three PCG steps each. At a walker's feet (a pixel of about 0.8 mm) four octaves draw, because the 1.25 mm one fades under the pixel: sixteen hashes. The first pass's grain was eight.
- **Ripples, for comparison.** Eighteen kernels, each a hash, three more draws and a sine and cosine, with the spacing adding one divide.
- **Exposure.** A dot product, a divide and a smoothstep.
- **Streaks.** Twenty-seven kernel hashes, but only where the streaks' weight is above 0, on a slip face, and never on the same pixel as ripples, since the ripples are gone there.

So a rippled pixel pays about half again what it paid, and a slip-face pixel pays about what a rippled one did. The first pass cost 0.09 ms at 11520×2160. The budget for all of it is a third of a millisecond; the GPU's milliseconds are the owner's to measure at the merge.

## What could not be measured here

- **Anything on a GPU**, and so **the tolerance**. The author expected within 3 of 255 near the origin and about 6 at 3.7 km, shaded, the 2.5 mm octave's normal being the term most exposed to the reconstruction's millimetre. [On the GPU](#on-the-gpu), below, has what was measured instead: 1 and 4.
- **How the streaks and the grain's normal look at 11520×2160 in motion.** The CPU filter tests hold the mean and the variation; crawl under a moving camera is the owner's eye's.
- **The shelter the normal cannot see** (a brink's separation bubble, a ruin's lee). It is a design note in [renderer](../subsystems/renderer.md#the-sand-close-up), [terrain](../subsystems/terrain.md#what-the-effect-needs) and [scene_gen](../subsystems/scene_gen.md#the-two-kinds), not built.

## On the GPU

Measured on 2026-09-30 on the RTX 5090, through the mesh path, `msvc-debug`, under the machine-wide GPU lock with the GPU idle before each run (0–1% busy, 7.1–7.4 GB held by other processes). These are differences between two computations of one picture, not timings, so the machine's load cannot move them; the runs were repeated and gave the same numbers.

### The resolve against the mirror

`domain/gfx/tests/ground_detail_tests.cpp`, "the resolve draws the second pass's function on sloped sand" ([gfx](../subsystems/gfx.md), "The second pass on the GPU"): the ergs' numbers, every term on, with the test's wind, on five planes tilted along it, each by the origin and 3.7 km out, from a walker's eyes looking along the sand (a pixel up to a metre long), down at the feet (a centimetre) and down at the feet at a millimetre (6° over 160 pixels). The worst difference of any covered pixel from the CPU, of 255: shaded (and how many pixels are over 1), then the detail view's ripple height, drawn share and grain.

| Plane | Look | By the origin | 3.7 km out |
|---|---|---|---|
| level | along | 1; 1, 1, 1 | 1; 12, 1, 11 |
| | feet | 1; 1, 0, 1 | 3 (467 over 1); 14, 0, 16 |
| | millimetre | 1; 1, 0, 1 | 4 (701); 13, 0, 12 |
| climbing 12° | along | 1; 1, 1, 1 | 1; 8, 1, 10 |
| | feet | 1; 1, 0, 1 | 3 (189); 10, 0, 20 |
| | millimetre | 1; 1, 0, 1 | 4 (898); 6, 0, 15 |
| falling 14° | along | 1; 1, 1, 1 | 1; 17, 1, 13 |
| | feet | 1; 1, 1, 1 | 1; 11, 1, 13 |
| | millimetre | 1; 1, 0, 1 | 4 (70); 13, 0, 12 |
| falling 26° | along | 1; 1, 0, 1 | 1; 16, 0, 14 |
| | feet | 1; 1, 0, 1 | 1; 11, 0, 17 |
| | millimetre | 1; 1, 0, 1 | 2 (4); 18, 0, 11 |
| falling 32° | along | 1; 1, 0, 2 | 1; 13, 0, 13 |
| | feet | 1; 1, 0, 1 | 1; 24, 0, 25 |
| | millimetre | 1; 1, 0, 1 | 2 (5); 11, 0, 11 |

The first pass on the same helper (the defaults, level sand) measures what it always did: 1 by the origin on everything; 3.7 km out 1 and 3 shaded (469 over 1) and 12 and 14 on the ripple height. The ripples are drawn on the level plane and the two gentler slopes and nowhere on the 26° and 32° lees, where the exposure has taken them; the streaks are weighted in on every pixel of those two and none of the other three.

**No term of the shader differs from the mirror**, and the author's expectation was pessimistic: **1 of 255 by the origin, 4 at 3.7 km**, where the millimetre look resolves the reconstruction's millimetre in the grain's 5 and 2.5 mm octaves and its normal. The tolerances set from this: 2 by the origin everywhere; 5 shaded at 3.7 km, and 28 on the detail view's ripple and grain there, whose 24 and 25 are the look down the 32° lee, which runs down the slope to grazing pixels 20 cm long (the first pass's 16 stays for the first pass).

**What the first run found was the reference, not the shader.** On the sloped planes the first comparison put the grain's channel 10 off by the origin (the windward slope's long look), 3 on the 14° lee and 2 on the 32° one, every one of them at a pixel whose centre the reference found just outside the triangle the visibility buffer named. The rasterizer snaps corners to a fraction of a pixel, so a centre that close to an edge can be drawn by the triangle across it, and the resolve's `reconstruct_screen` then clamps the barycentrics into that triangle and shades the point on the edge — under a grazing footprint a metre long, up to a millimetre from where the ray meets the surface. With the reference clamping the same way, every one of them is within 1. (Level sand has such pixels too — four in the long look by the origin — and they happened to be within 1 already.) The reference also shades the pixel's own triangle rather than the plane the mesh was cut from, since the 16-bit grid turns a triangle of a sloped plane by up to half a milliradian, which the spacing reads.

### The reference path tracer

`systems/renderer/tests/ground_detail_tests.cpp`, "the reference path tracer shades the second pass's sand on a slope": the resolve against the path tracer, which calls the same function unfiltered, at 64 samples a pixel at the pixel's centre and one bounce, traced shadows, 160×120, on the renderer's waves made steep enough to have slip faces (16 m over 60 m) and laid out on a grid of a metre, with the ergs' numbers, at a walker's feet looking down the wind. FLIP's pooled mean, and its mean over the picture's lower half (the ground at the feet) and upper half:

| Ground at the feet | Without the detail | With it | Lower half, without / with | Upper half, without / with |
|---|---|---|---|---|
| climbing into the wind at 11.9° | 0.0479 | 0.0489 | 0.0483 / 0.0493 | 0.0475 / 0.0485 |
| falling away at 26.0° | 0.0476 | 0.0508 | 0.0514 / 0.0520 | 0.0437 / 0.0496 |

The detail adds 0.001 and 0.003, held to 0.01. What it adds on the lee is the upper half's: ten metres down the slope the resolve fades a 40 cm tongue by its footprint and a reference at the pixel's centre point-samples it.

**The first run was on the flat case's 4 m grid, and it found a defect of the resolve's that is not the detail's.** There the lee read 0.0691 against 0.0510, most of it in the lower half (0.0745 against 0.0470 there), and 0.015 of it the streaks'. With the streaks alone at full contrast the two pictures showed why: the path tracer drew each tongue as the ellipse it is, and the resolve drew the same tongues at the top of the picture and then ran them on as straight stripes to its bottom edge, at the feet. Looking some 60° down from 1.65 m, the camera's plane meets the ground about 3 m behind the walker, and the 4 m triangles under the feet reach past it; `reconstruct_screen` divides the corners by w before it takes the pixel's barycentrics, which is meaningless for a corner behind the camera, so the resolve shades a point the pixel does not see. The ripples and the grain hide it — drawn at the wrong point they still look like ripples and grain — and 2 m tongues do not. On a metre grid the two pictures draw the same tongues. The fix is the resolve's, not this pass's, and is not made here ([gfx](../subsystems/gfx.md), "What `reconstruct_screen` cannot rebuild").

### Seams across the rings

`systems/renderer/tests/ground_detail_rings_tests.cpp`, "the second pass on both sides of every ring and chunk border": the rings' own test view (the terrain shrunk to 128 m, an inner ring at 25 cm and a middle one at 50 cm, five metres over the dunes looking down across the inner ring's border and a chunk border inside both rings, 256×192) drawn in the detail view with the ergs' block read from their scene file (which the test also holds `ground_ref::erg_numbers` to). Each covered pixel's world point comes back through its depth.

| | Inside a chunk | At the ring's border | At a chunk's border |
|---|---|---|---|
| pixels | 46,867 | 512 | 881 |
| grain against the CPU, worst (over 3) | 23 (52) | 9 (2) | 1 (0) |
| the share of the ripples drawn, largest step between neighbours | 19 | 19 | 7 |
| ripple height against the CPU at the height function's normal, worst | 236 | 24 | 96 |

The first pass on the same view measures what it did (the grain 11, 4 and 1; the ripple 13, 1 and 1). The grain holds as a function of position: 23 inside a chunk against the first pass's 11, since the gradient grain's coarse octave changes faster with position than the value noise did and a float depth is a few millimetres along the ground at a grazing angle; held to 32, and each border to no worse than the inside. The ripple height is not a function of position any more: the spacing scales its wavelength by the normal the level interpolated there, and against the height function's normal the two are up to 0.05 of scale apart inside a chunk, near a brink — a fifth of a wavelength, any height at all.

**The step at a border, read back.** The instrument is the same view with the ergs' numbers but a metre-long plain sinusoid and no defects: a pixel's ripple height is then a smooth function of the scale the GPU drew it at, and solving the CPU's ripple at the pixel's world point for it (Newton's method, from the height function's scale, trusted where a level of the channel is 0.002 of scale or less and the solution is within 0.75 of a level) reads back the GPU's scale less the height function's at 25,764 pixels. The height function's scale is continuous, so that difference's step between neighbours is the step the level's normal takes, as the spacing reads it:

| | Pixels with a step read | Largest step | 99.9th percentile | 99th percentile |
|---|---|---|---|---|
| inside a chunk | 25,743 | 0.081 | 0.050 | 0.020 |
| at the ring's border | 125 | 0.009 | 0.006 | 0.005 |
| at a chunk's border | 549 | 0.028 | 0.028 | 0.017 |

A step of Δs in scale slides a crest by `(6 - 4.5 defects) Δs / s` wavelengths — the kernels' reach in wavelengths, the same at a metre as at 12 cm — so at the ergs' own scale of 1 the ring's border moves their crests by at most 0.04 of a wavelength, **5 mm**, and a chunk's border by at most an eighth (15 mm), where a chunk's own simplification meets a brink; the normal steps by at most 0.004 and 0.011 radians. Inside a chunk the normal moves by more than either between one pixel and the next, one pixel in a hundred, round the brinks the levels' triangles cannot follow. Held: the ring's border to 0.015, and both borders' 99th percentile to the inside's.

## The cost on the GPU, measured at the merge (2026-09-30)

**What was run.** `engine-view` from `msvc-release` at `a4b3550`, offscreen, the erg flown along **`walk-path.json`**: a walker's eyes 1.65 m over the western floor, so the lower half of every frame is sand close enough to draw ripples. Three scenes that differ only in the `detail` block: none, the first pass's nine numbers, the ergs' second pass. The resolve pass's GPU milliseconds a frame, from `gfx::GpuTimer`, the median over 1,201 frames and three repeats, with cascaded shadows:

| Detail | 1920×1080 median | p95 | p99 | 11520×2160 median | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| none | 0.091 | 0.101 | 0.104 | 0.917 | 0.956 | 1.056 |
| first pass | 0.133 | 0.141 | 0.166 | 1.452 | 1.478 | 1.748 |
| second pass | 0.138 | 0.147 | 0.176 | 1.550 | 1.578 | 1.892 |

**What the detail costs depends on the shadows**, which the first pass's page did not know. The same walk at 11520×2160 under each mode, two repeats:

| Shadows | Resolve without the detail | The first pass adds | The second pass adds |
|---|---:|---:|---:|
| off | 0.788 | 0.531 | 0.632 |
| cascaded maps (`csm`) | 0.922 | 0.530 | 0.630 |
| traced (`rt`) | 2.327 | 0.113 | 0.119 |

- **With cascaded maps or no shadows, the detail costs 0.53 ms a frame as the first pass drew it and 0.63 ms as the second draws it.** The second pass adds 0.10 ms, a fifth. The owner's walk script runs `--shadows csm`, so these are his numbers.
- **With traced shadows it costs 0.11 and 0.12 ms**, which is the 0.09 ms the first pass's page measured, again. The likely reason, not established: a resolve that traces rays is waiting on traversal and memory for most of its 2.3 ms, and the detail's arithmetic runs in time the pass was already spending. A pass's GPU time is not the sum of its parts.
- At 1920×1080 with cascaded maps the detail costs 0.043 and 0.048 ms, a twelfth of the surround's, as the pixel count says.

**What this corrects.** The first pass's page gave 0.09 ms at the surround size and called it under the budget of a third of a millisecond. That holds under traced shadows and nowhere else. **Under the shadows the owner walks with, the first pass was already 0.2 ms over that budget, and the second pass is 0.3 ms over.** The whole frame is 2.5 ms of GPU time at 11520×2160, so the budget is a guard against creep and not a frame-rate limit; whether the detail has to come down is the owner's.

**The machine's state.** RTX 5090, under the GPU lock (ours). Every pass the detail cannot touch — the cull, the Hi-Z, the rasterizer, the shadow maps — has the same median to four digits in all three scenes of a mode, so nothing else on the machine moved these numbers between runs. The harness still marked every run an upper bound: other processes used 1 to 20% of the CPU, and the GPU's busy counter read 52 to 99% at a run's end, which for an unthrottled offscreen run is mostly the run itself. Another session was working a diffusion model on and off that evening and held about 8 GB of the card's memory throughout.

**What to cut first, if it has to come down.** The ripples, not the second pass's terms: eighteen kernels a pixel, each a hash chain and a sine and cosine, against the grain's sixteen hashes. Two candidates, neither built: skipping a lattice cell whose nearest point is further than a kernel's radius before hashing its kernels, which a point in a cell's middle can do for most of the eight neighbours; and drawing the ripples from one kernel a cell past the distance where the profile has already eased to a sinusoid.

**A look.** Captures every sixtieth frame of the walk, first and second pass, and the second pass's detail view, read by the engine side. On the floor the two passes are close to the same picture at 1920×1080: the second's ripples stand a little further apart on the rising ground, and its grain is finer than a pixel shows from standing height at that size. The walk path stays on the floor and crosses no slip face, so the streaks and the smooth lee are not in these pictures; the owner's walk is where they are judged. In the detail view the near face of a low rise draws its ripples further out than the floor beside it, with a straight edge where the rise begins. A face turned toward the eye has a smaller footprint, so that is what the filter should do; an edge that straight is still worth the seams test's attention with the second pass on.
