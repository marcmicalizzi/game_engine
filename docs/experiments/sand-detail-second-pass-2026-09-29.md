# The sand close up, second pass (2026-09-29)

**The question.** The owner walked the first pass of the sand's detail ([sand-detail-2026-09-29](sand-detail-2026-09-29.md)) on the RTX 5090 at 11520×2160 from 1.65 m, beside a photograph of a real dune, and found it "mostly good" with three faults:

- ripples over a crest on the side the wind blows toward;
- ripples uniformly tight everywhere;
- sand that looks pixelated at his feet.

This page holds what the second pass measured and what it could not ([renderer](../subsystems/renderer.md#the-sand-close-up), [gfx](../subsystems/gfx.md)).

**Where.** Everything was measured on a 4-vCPU Linux container with no GPU. The shader compiles there and does not run, so every number below comes from the CPU mirror, `domain/gfx/tests/ground_detail_reference.h`, in double precision. The mirror is the function the GPU test holds the shader to on the owner's machine. The tests are in `domain/gfx/tests/ground_detail_tests.cpp`. The pictures come from `ground_detail_preview_tests.cpp`, which is skipped unless run with `--no-skip`.

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

- **Anything on a GPU.**
- **The tolerance.** `domain/gfx/tests/ground_detail_tests.cpp`'s GPU case and the renderer's two detail tests build their blocks from the defaults, which draw exactly the first pass. Their tolerances should therefore not move: 2 of 255 by the origin, and 4 shaded and 16 in the detail view at 3.7 km. A case with every v2 term on is worth adding at the merge. The author expects:
  - within 3 of 255 near the origin;
  - about 6 at 3.7 km, shaded, where the 2.5 mm octave's normal is the term most exposed to the reconstruction's millimetre.
- **How the streaks and the grain's normal look at 11520×2160 in motion.** The CPU filter tests hold the mean and the variation; crawl under a moving camera is the owner's eye's.
- **The shelter the normal cannot see** (a brink's separation bubble, a ruin's lee). It is a design note in [renderer](../subsystems/renderer.md#the-sand-close-up), [terrain](../subsystems/terrain.md#what-the-effect-needs) and [scene_gen](../subsystems/scene_gen.md#the-two-kinds), not built.
