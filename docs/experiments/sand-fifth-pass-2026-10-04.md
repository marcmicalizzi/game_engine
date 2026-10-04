# The sand close up, fifth pass: what the dashes were (2026-10-04)

**The question.** The owner flew the endless desert (`content/test-scenes/desert-endless`, 11520×2160 `surround3`, `--time-of-day 10`, `--shadows rt`) and dropped markers at a dune's slip face. Close up, especially looking down or along it, the face was covered edge to edge in a dense, even field of short light and dark dashes, all pointing the same way, like rain on a window: "very elliptical dottish", "a lot of speckling". Nothing read as a tongue of sand. It was the complaint before the fourth pass ([sand-fourth-pass-2026-10-03](sand-fourth-pass-2026-10-03.md)) and still the complaint after it. The fourth pass had been written with no GPU and judged by the mirror's numbers alone. This pass looked first.

The fix is in [renderer](../subsystems/renderer.md#the-sand-close-up), "The slip face, fifth pass".

**Where.** RTX 5090, `msvc-release`, `engine-view` offscreen. The owner's two flights were recorded at `70f88d15` (`endless-2026-10-04T0912-input.jsonl`, markers at ticks 2296 and 2342; `endless-2026-10-04T0910-input.jsonl`, tick 1573) and replayed with `--replay-input <log> --marker-captures <dir> --width 3840 --height 2160 --views single --time-of-day 10 --shadows rt`. That is a single view at the owner's pixel density: 2,160 rows over the session's `fov_y` of 0.96 rad. A camera path of seven markers round the same face (`--camera-path`, the same flags) gave the other views. The captures are outside the repository, in the session's scratch directory `sand-5/`.

## What the owner saw

The marker at tick 2296 (2342 is the same camera):

- **The camera.** At (−1783.2, 81.6, 418.9), looking 33° down.
- **The face.** 33.4° steep, its fall line 204° round from +x (from the normals capture). The camera sees it 24° off its normal from about 43 m. A ruler scene (the ripples turned into a 1 m plain sinusoid on every slope) put the pixel at 3.3 cm along the fall line at the top of the frame, 2.1 cm in the middle and 1.3 cm at the bottom.
- **The light.** The face is in its own shade: the sun is 53° up behind the dune. Only the sky lights it.
- **The picture.** Dashes edge to edge. Light and dark ones in equal number, each 9 to 14 pixels wide and 28 to 72 long, all down the fall line. No grain shows (its 2 cm octave has faded at these pixels) and no ripples (the face is past their 30° fade). The luminance varies by 4.2 levels root-mean-square over the frame; with no detail term it varies by 0.4.

## Which term draws them

The same marker, on scratch copies of the scene with one term switched off at a time:

| Off | What is left |
|---|---|
| nothing | the dashes |
| the streaks (`streak_start_deg`, `streak_full_deg` 0) | a smooth face |
| the streaks' albedo | a smooth face, a trace of shape at 8× contrast |
| the streaks' roughness | the dashes, unchanged |
| the streaks' normal | the dashes, unchanged |
| the ripples (`ripple_height` 0) | the dashes, unchanged |
| the grain (`grain_albedo`, `grain_roughness`, `grain_normal` 0) | the dashes, unchanged |

**The dashes are the second pass's streaks, drawn by their albedo.** Each is one kernel: an ellipse 2 m down the fall line and 0.4 m across, under `(1 − r²)³`. At half its height that is 0.91 m by 0.18 m on the surface, which is the 9 to 14 by 28 to 72 pixels in the picture. Three kernels share each 2 m cell, so there are 0.75 a square metre. Each moves the albedo by its draw times 0.40 at its centre (the block scales the sum to the scene's 6% root-mean-square), so a single dash stands up to 40% off the sand.

**Why the endless desert had them.** Its detail block was copied from the erg's when the scene was made (`14bfeb76`, 2026-09-30), before the third pass changed the erg's (`d86ae0df`, the same day). It kept the streaks, 22° to 30°, and had no `flow_*` at all. Every pass since changed `desert-erg` alone. The tests held `ground_ref::erg_numbers()` to `desert-erg` alone. Every preview and measurement judged the erg's tongues. So for four days and two passes the owner flew the second pass's streaks: the "rain on a window" the third pass's page already describes.

## The fourth pass on the GPU

The erg's block on the endless scene, under the shader as it stood, at the same marker:

- **Close up.** Long, faint tongues running down the face, a few dozen across the frame, replacing the dashes. Each chute under the brink drew as a light centre between two dark hairlines a few pixels wide: the levees.
- **From 350 m.** The face, seen with pixels of about 15 to 35 cm, was covered in faint one-pixel vertical lines. At 5× contrast they are rain on a window again, finer.
- **The 100 m pair, a metre apart.** The same features, moved. No shimmer.
- **Walking down the face.** The chute reads as a double-ridged groove.

Two things were wrong with it:

- **The levees.** Their steep sides, a few centimetres across, came and went along the tongue (the chute fades out down it, the width pinches, the meander swings). Under a low sun the face read as broken light and dark dashes a metre or two long. The instrument below counts them: 19 features per 100 m², a median 1.8 m long.
- **The filter.** The fourth pass faded the tongues by a whole lane width (0.6 m), so they were whole up to 15 cm pixels and gone at 30 cm. A tongue's narrowest relief is far finer than that. Between 10 and 20 cm the pixels drew up to 36% more variation than 8 × 8 samples of each pixel hold. That excess is the one-pixel lines.

## What changed

- **The endless desert carries the erg's block**, field for field, and a test holds the two files together (`ground_detail_rings_tests.cpp`, "the endless desert draws the erg's sand").
- **The chute is a shallow scour with no levees**: `−0.3 (1 − r²)²` across the tongue's centreline, easing into the lobe as before. Everything else about a tongue is the fourth pass's: the lanes in sixteen directions, the segments, the episodes, the meander, the pinch, the split lobes, the soft head and toe.
- **The tongues fade by the footprint against 0.75 lane widths a period** (`k_ground_flow_body`), whole at four pixels a period and gone at two, the rule every term of the detail follows. On the ergs that is whole to 11 cm pixels and gone at 22.5 cm, where the fourth pass's was 15 and 30. A tongue's relief is its lobe's two flanks, 0.4 of a lane width across for a split lobe and up to 1.3 at a wide toe. The number was chosen by the speckle reading below, with the scour in place:
  - at a whole lane width the pixels drew 1.19 times the variation they hold, at 15 cm;
  - at half a lane width the tongues were gone at 15 cm, where 8 × 8 samples still held them at 3% of contrast;
  - at 0.75 the pixels stay within 1.09 at every size.
- **The scales.** The lanes' own root-mean-square value and slope, which the block scales `flow_albedo` and `flow_normal` by, are measured again over the mirror: 0.213 and 0.723 per lane width (0.222 and 1.008 with the levees). So `flow_normal` 0.05 still means a slope of 0.05 root-mean-square.

A first attempt kept the levees and eased them away in a first fade stage, as the ripples ease their profile. It removed the excess at a distance, but under a raking sun at 5 cm pixels the face still read 19 to 21 features per 100 m² a median 1.6 to 1.9 m long, because the fragmentation is the levees' shape, not their sampling. A softer levee (`1.2 r² − 0.45`) read worse: 26 features, 0.7 m. The plain scour reads 7 features, 8 m.

## The instrument

`ground_detail_tests.cpp`, "a slip face draws a few long faint tongues, not speckle", on the mirror. Two readings:

- **Features.** A 32° face 40 m down its fall line and 16 m across, in plan at 5 cm a sample (the owner's pixels were 1.3 to 3.3 cm), each sample shaded with that footprint and every term of the block. Two lights: the sky alone (the face in its own shade, as the owner saw it) and a sun raking across the fall line 20° up. A feature is a region where the luminance stands more than 2% off the face's mean, light and dark apart, eight-connected. Each feature's length and aspect come from its second moments along the surface.
- **Speckle at a distance.** 1,500 pixels scattered over the same face at 3 to 40 cm, under the raking sun. For each, the variation the pixels draw against the variation 8 × 8 samples of each pixel hold.

| Reading | Second pass's streaks (what the owner flew) | Fourth pass's tongues | Fifth pass |
|---|---|---|---|
| In shade: features per 100 m² | 49 | 2.7 | **2.0** |
| In shade: median length, aspect | 1.7 m, 5.8 | 3.8 m, 15.5 | **3.9 m, 14.6** |
| In shade: contrast at the 99th percentile | 26% | 3.7% | **3.9%** |
| Raking sun: features per 100 m² | 52 | 19.2 | **7.2** |
| Raking sun: median length, aspect | 1.7 m, 6.2 | 1.8 m, 18.6 | **8.1 m, 24.3** |
| Raking sun: contrast at the 99th percentile | 28% | 24% | **21%** |
| Drawn over held variation at 5 / 7 / 10 / 15 / 20 cm pixels | — | 1.05 / 1.08 / 1.17 / 1.36 / 1.19 | **1.02 / 1.04 / 1.09 / 0.89 / 0.17** |

**What the test asserts.** In shade: under 5 features per 100 m², longer than 3 m at the median, an aspect over 8, and under 5% contrast at the 99th percentile. Under the raking sun: under 12 features, longer than 5 m, an aspect over 12. At every pixel size, never more variation drawn than 1.15 times what the pixels hold. Without this change the fourth pass fails the raking sun's count and length and the speckle ratio at 10 to 20 cm. As a control, the second pass's streaks are asserted to read as speckle: over 25 features per 100 m², under 2.5 m, over 15% contrast.

The fourth pass's column was measured by the same readings with the fourth pass's chute, levees and fade (a whole lane width) put back into the mirror for the run.

**The raking sun's contrast is high by design.** The relief is the scene's `flow_normal`, a slope of 0.05, and a 20° sun across it shows a few millimetres of relief strongly, as it does on a real face. What changed is that it shows as long features rather than short ones.

## The captures

Before (the streaks), the fourth pass on the GPU (`iso/erg-block`, `views-erg`), and after (`after-*`, `views-after`), all 3840×2160:

- **The owner's markers** (`before-0912/tick-0002296.png`, `after-0912/tick-0002296.png`, and the same for 2342 and for `-0910/tick-0001573.png`).
  - After: a smooth face with long, faint tongues running down it, light lobes and soft darker scours. No dashes.
  - Over the whole frame the luminance varies by 0.82 levels root-mean-square, against 4.17 with the streaks and 0.39 with no detail term.
  - The 0910 marker is another face in shade: the same dashes before (4.20 levels root-mean-square over the frame) and the same faint tongues after (0.80).
- **From about 100 m** (`00060-above100.png`) and **350 m** (`00180-above350.png`).
  - At 100 m: the tongues as long faint lines.
  - At 350 m: the face as a smooth shaded band under its lit brink. At 5× contrast faint one-pixel lines remain where the fade has the tongues at about a tenth of their strength. The pixel-to-pixel luminance difference over the face halves against the fourth pass (0.39 and 0.12 levels across and down, from 0.81 and 0.18).
- **Grazing along the face** (`00240-grazing-along.png`) and **walking down it** (`00300-down-face.png`).
  - Along it: the grain at the feet as fine sand, and the face smooth beyond.
  - Down it: one tongue running away down the face, a soft darker scour under the brink opening into a lighter lobe. The fourth pass drew a double-ridged groove there.
- **A metre apart** (`00060`/`00120`, `00300`/`00360`): the same features, moved by the parallax and nothing else. The luminance statistics over the same window agree to 0.01 of a level.

**The 22.5° crossing in the blend zone.** The fourth pass left this one for the owner's eye. The 100 m view's upper left is a band of face whose fall line lies within 2.25° of a sector's middle (found from the normals capture). At 1× the tongues there lean about 22° off their neighbours'. At 6× contrast the two families can be seen crossing. **It is visible, faintly**, as a patch of tongues at another angle rather than as a lattice of X's. It is not changed here.

## The cost

Measured the way the third pass's was ([sand-third-pass-2026-09-30](sand-third-pass-2026-09-30.md#the-cost-on-the-gpu-measured-after-the-merge)):

- **The run.** RTX 5090, `msvc-release`, `engine-view --benchmark` offscreen over the erg's `walk-path.json` (1,201 frames, `--repeat 2`) at 11520×2160 `surround3`, `--shadows csm`, `--wait-quiet 60`, each run under `tools/gpu-lock.ps1 run`.
- **The fourth pass's column.** Built from `main`'s `ground_detail.slang` in the same tree, then the fifth pass's restored and rebuilt.
- **The figures.** The resolve pass's GPU milliseconds at the median, the frame's beside it.

| Detail | Resolve, median | p95 | p99 | Frame, median | The detail costs |
|---|---|---|---|---|---|
| off (two runs) | 1.156, 1.152 | 1.201, 1.199 | 1.266 | 2.20 | — |
| fourth pass | 1.990 | 2.020 | 2.330 | 3.02 | 0.836 |
| **fifth pass** (two runs) | **1.992, 1.992** | 2.027, 2.021 | 2.337, 2.333 | 3.03 | **0.838** |

**The fifth pass costs what the fourth did**, within 0.002 ms, and under the third pass's 0.862, which the owner accepted on 2026-10-03.

**The resolve without the detail has grown** from 0.897 to 1.15 ms since the third pass's measurement, from other work in the resolve. So the detail's cost is the difference of each pair, not the totals.

The walk is mostly level floor, where the tongues are not drawn. What the change saves is on a slip face seen from more than about 200 m, where the tongues are now skipped and were drawn before.

**The machine.** The GPU was 4 to 7% busy at the start of every run, holding 7.1 to 7.9 GB of others' memory. Other agents' builds took 9 to 54% of the CPU. Three runs (one "off", the second "off", the fourth pass) ended with another process at 50 to 99% of the GPU, so their last frames are upper bounds. The two fifth-pass runs, one quiet at both ends and one at a third of the CPU, agree to the microsecond at the median.

## Seen and not fixed

- **The terrain's facets.** At 8× contrast both the fourth and fifth passes' captures, and the one with no detail term at all, show the terrain's triangles as polygons of slightly different shade, about a level apart at 1×. That is the ground's shading normal, not the detail.
- **Roughness at a tongue's edges and toe.** Real grainflow has coarser grains there. The detail moves the albedo and the normal of a tongue and not its roughness, because a new number would need a new field in the scene's `TerrainDetail` and its reader, outside this change.
- **The blend's crossing**, above.
- **The lobes' short meander.** At 100 m a narrow lobe's centreline wiggle (the 2 to 4 m harmonic) reads at 6× contrast as a twisted rope. It does not shimmer between frames a metre apart.
