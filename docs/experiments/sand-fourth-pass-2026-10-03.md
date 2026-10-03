# The sand close up, fourth pass: grainflow lanes (2026-10-03)

**The question.** The owner, on the third pass ([sand-third-pass-2026-09-30](sand-third-pass-2026-09-30.md)) at 11520×2160 with a low sun, from the brink of a slip face looking along and down it: "Close up (and especially looking down) the lee side streaking still looks very elliptical dottish." On the face he saw:

- round to oval blobs a few decimetres across;
- thin dashes a few metres long at slightly different angles, some crossing;
- nothing that read as a tongue of sand.

The fix is in [renderer](../subsystems/renderer.md#the-sand-close-up), "The slip face, fourth pass".

**Where.** A 4-vCPU Linux container with no GPU. The pictures are the CPU mirror (`domain/gfx/tests/ground_detail_reference.h`) shaded by `brdf_reference.h`, from the preview case "the slip face's lanes, in plan and on the face" (`ground_detail_preview_tests.cpp`, `--no-skip`). The numbers are from `ground_detail_tests.cpp`.

## The instrument

The preview draws `<variant>_face_plan`, `_face_brink` and `_face_feet`, with suns raking across the lanes and in front:

- `all_*`: every lane present (`flow_share` 1), to see the function itself;
- `ergs_*`: the ergs' numbers, with the episodes.

The views are:

- **Plan:** 30 m × 30 m of a 32° face from straight above, at 2 cm a pixel.
- **Brink:** from 1.7 m over the brink of a 20 m face, looking down it 30° off the fall line.
- **Feet:** from 1.7 m over the face's middle, looking down at 60°.

**Before** (the third pass's cells), the defect is there:

- `all_face_plan` is a field of dashes a few metres long, each at a slightly different angle, crossing their neighbours and ending within a cell.
- `ergs_face_brink` shows smudges and short dashes on the upper face.

The instrument sees what the owner saw. The ovals at his feet are the same beats seen closer.

**After** (sixteen fixed directions):

- `all_face_plan` is straight, unbroken lanes across the whole 30 m. With every lane present it reads as corduroy.
- `ergs_face_plan` has a quarter of the lanes: a few long chutes with levees over smooth sand.
- `ergs_face_brink` and `ergs_face_feet` show long shallow lanes running down the face from the camera, continuous as far as the face goes.
- **Not yet right:** every lane is a perfectly straight, uniform line for its whole length. A real grainflow wavers, narrows and stops part-way. The brief's optional window, tens of metres long and soft over metres, would answer that; it is not built.

## Numbers

| Measure | Before (cells) | After (directions) |
|---|---|---|
| Value rms, slope rms per lane width | 0.119, 0.429 | 0.142, 0.515 (the block's constants) |
| Autocorrelation along a lane at 5, 20 and 40 m | falls within a cell | 1.00, 1.00, 1.00 |
| Autocorrelation along the true fall line, 7° off the lane's direction | — | −0.37, 0.04, 0.00 at 5, 20 and 40 m (the lane runs straight; the fall line leaves it in a few metres) |
| Gradient's share along the fall line, 99th percentile, on a direction / 8° off it / at the blend's middle | — | 0 / 0.139 / 0.97 (two families crossing) |
| A milliradian of normal 3 km out, worst point | 5.9% of rms | 11.6% at the blend's steepest; 0 outside a blend |
| Jump across the blend's edges (9°, 13.5°) and a direction's own angle | — | none (under 1e-4) |
| Filter's mean against 8×8 supersampling at 5, 15 and 40 cm pixels | 0.01%, 0.002%, 0.13% | 0.004%, 0.01%, 0.46% |
| Lattice hashes on a slip-face pixel | 8 | 4 (plus one per lattice point for the episodes, as before) |

**Why the blend is narrow.** A full-sector blend (the brief's first form) crosses two families 22.5° apart over every fall line except the sixteen exact directions. On the mirror that scored 0.97 at the 99th percentile everywhere off a direction, and the along-fall-line correlation was zero at 5 m. Over the middle fifth only, most fall lines see one family. The price is that the weight turns five times as fast with the normal, hence 11.6% where it was 5.9%. In shading that is a change in slope of under 0.006, under a level of 255. Sixteen directions were enough. Thirty-two would bring the lanes within 4.5° of the fall line but not change the crossing in the blend.

## At the merge

The GPU case "the resolve draws the third pass's function on sloped sand" draws `ground_ref::erg_numbers()` falling with the wind at 27° and 32°, and its mirror is the new function, so it needs no code change. The author expects the same tolerance, 1 of 255 by the origin and 4 at 3.7 km:

- the lane's coordinate is `dot(p, across) / width`, about 6,000 lanes 3.7 km out, where a float keeps 5e-4 of a lane;
- the 12° slope 50° off the wind is never a slip face.

## Not measured

- Anything on a GPU, and the cost in milliseconds.
- Whether 22.5° crossings in the blend zone read as crossings at a walker's feet. The owner's eye on a face whose fall line sits within 2.25° of a sector's middle (11.25°, 33.75°, …) decides.
