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
