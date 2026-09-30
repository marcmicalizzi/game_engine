# The sand close up, third pass (2026-09-30)

**The question.** The owner flew and walked the second pass ([sand-detail-second-pass-2026-09-29](sand-detail-second-pass-2026-09-29.md)) on the RTX 5090 at 11520×2160. His verdicts:

- "The sand texture in general looks much better."
- The lee side's streaks look "very blotchy and short streaky ... doesn't look like sand streaking down the lee side at all."
- "Ripples are still incredibly uniform as well throughout the sand."
- Under a fast time-lapse, the rising sand keeps "the ripples staying on top."

This page holds what the third pass measured ([renderer](../subsystems/renderer.md#the-sand-close-up), [gfx](../subsystems/gfx.md)).

**Where.** A 4-vCPU Linux container with no GPU. Every number comes from the CPU mirror (`domain/gfx/tests/ground_detail_reference.h`, in double), or from the preview (`ground_detail_preview_tests.cpp`, `--no-skip`). The preview draws "before" with the second pass frozen (`ground_detail_reference_v2.h`) and the ergs' second-pass numbers, and "after" with the live mirror and `ground_ref::erg_numbers()`.

## The slip face

The preview adds a slip face 20 m tall, seen from the floor 5, 20 and 60 m downwind of its toe (`slip_<d>m`). Each view is drawn under two suns 20° up: one raking across the face from the side (`_raking`), and one in front of it (`_front`).

- **`before_slip_20m_raking` reproduces the owner's screenshot.** A band of short light and dark dashes across the face, "rain on a window". The instrument sees what he saw.
- **`after_slip_20m_raking`: long shallow lanes.** They run from near the brink down most of the face, closely spaced, some ending partway down, lit on one side and shaded on the other by the raking sun. The author reads them as grainflow. They are more regular than a real face, a combed look where real lanes are more irregular in spacing and in length. From 60 m they are a fine vertical texture on the face. From 5 m under the front sun they almost vanish, as relief should.

Measured on the mirror (`ground_detail_tests.cpp`, "grainflow lanes …"):

| Measure | Result |
|---|---|
| The lanes' own rms, value and derivative per lane width | 0.119 and 0.429 (the block's constants) |
| Mean gradient along the fall line against across it | 0.051 against 0.527 |
| Seam: millimetre steps over 240 m of lines crossing cells, lanes and windows | no jump |
| A thousandth of a radian of normal, 3 km out | moves the lanes by 5.9% of their rms |
| 32° face against 8×8 supersampling, pixels of 5, 15 and 40 cm | mean within 0.01%, 0.002% and 0.13% |
| Cost | eight lattice hashes a pixel, only where the lanes' weight is above 0, never where ripples are drawn; the streaks were twenty-seven kernels |

### The lanes as episodes

The owner on `after_slip_20m_raking`, as first drawn: "I don't think the entire lee needs to be covered in streaks, just there should be some that appear and disappear over time depending on wind activity."

Each lane now comes and goes on a clock driven by the sand the wind moves (`flow_share` 0.25, `flow_turnover` 800 on the ergs).

| Measure | Result |
|---|---|
| Share of the face in a lane at a time | 18.6% |
| Share of the face that changes half a cycle on | 28.3% |
| A lane's presence over a cycle | 25%, and no step at the cycle's wrap |
| Clock advancing half the share a frame (a storm at game rate, a time-lapse) | no lane drawn |

`after_slip_20m_raking`, `after_slip_20m_clock33_raking` and `after_slip_20m_clock67_raking` show one face at three readings of the clock. Each is mostly smooth sand with a scattering of lanes, and the set differs from reading to reading.

## Ripples that are not all alike

Measured on the mirror (`ground_detail_tests.cpp`, "ripples in patches, steered by the ground …"):

| Measure | Result |
|---|---|
| Patches' wavelength scale over a kilometre of samples | 0.70 to 1.40, mean 1.00 |
| Steering on a 25° slope, round the compass | at most 19.5°, at most 0.1° per 0.1° of azimuth |
| A tenth of a degree of steering | moves the crests at most 0.009 wavelengths |
| Half-millimetre steps along lines crossing patches, slope swinging across the wind | no jump |
| Cost | patches: eight lattice hashes a rippled pixel; steering: arithmetic |

The first steering rule took `steer_gain` of the component along `g = n.xz / n.y`, which grows with the slope's square: 7° at most on a 25° slope. Taking `min(gain tan θ, 1)` of the component along the unit fall line reached the clamp. But where the wind runs nearly along the fall line, it swung 1.4° per 0.1° of azimuth, which would slide the crests by an eighth of a wavelength. Capping the share at a half bounds the swing at a degree a degree.

In `after_eye_dune_*`, the floor's ripples bend along the dune's flank and their spacing visibly changes across the view; the author reads that as less uniform, and the owner's eye decides whether it is enough.

## Ripples that move

| Measure | Result |
|---|---|
| A travel of 3.7 cm on the mirror | moves the phase by 3.7 cm to within 3e-16 wavelengths |
| The erg's record: a calm day's transport | 0.0036 m² (36 cm²) across a metre; a year's mean day on the storm erg, with gain 1,000, 24.9 m² |
| Storm 42's peak wind over the record's mean | 2.31 |
| The day after storm 42, celerity 2,000 | 7.2 m, 0.5 cm a minute on average |
| At a day a second (60 frames a second) | 12 cm a frame, a wavelength: the ripples are gone, and return below about 4,300× (a quarter of a wavelength a frame) |

The first celerity tried, 10, was chosen from a guess at the record's flux: it gave 3.6 cm a day. The record's calm day is far below a real saltation flux (of order a square metre a day in an 8 m/s wind), because it is tuned to the dunes' migration and not to grain physics. So the ripples' celerity is a stylization, like the storm gain.

## What could not be measured here

- **Anything on a GPU.** The GPU cases at the merge run on tilted planes. The author expects each new term to hold the existing tolerance (1 of 255 by the origin, 4 at 3.7 km) with these looks:
  - grainflow, on a 32° plane falling with the wind (the lanes' normal is smooth and eight hashes a pixel, all integer);
  - patches and steering, on a 12° plane across the wind (they change only the wavenumber and the direction the kernels already take);
  - motion, on the flat with a travel of a few centimetres (one multiply a kernel).

  At 3.7 km the lanes' `s = offset / width` is taken from an offset under a cell, so it keeps its precision.
- **The cost.** It is counted, not timed:
  - grainflow: eight hashes, only on a slip face;
  - patches: eight hashes a rippled pixel;
  - steering: arithmetic;
  - motion: one multiply a kernel.

  The ripples' eighteen kernels are unchanged. The owner measured that the detail costs 0.63 ms with cascaded maps; the patches will add to that, and their share needs the owner's GPU.
