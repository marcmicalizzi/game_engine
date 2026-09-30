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
