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

## On the GPU, measured at the merge

The pass was written where no GPU ran. At the merge, on the RTX 5090 through the mesh path (`msvc-debug`, under the machine-wide GPU lock), and on the Titan Xp through the vertex path (`linux-server`). These are differences between two computations of one picture, not timings.

**What the first run found.** Two GPU cases failed, both for what they assumed and neither for what the shader draws:

- gfx's second-pass case drew `ground_ref::erg_numbers()` and required the second pass's flags of it. It now draws the ergs' numbers as the second pass had them, and the third pass has a case of its own.
- The rings' seam instrument reads one number back from a plain sinusoid, the spacing's scale. A patch scales the wavelength by position and the steering turns the ripples with the normal, so it read 26 pixels at the ring's border for 125 and called their doing a step of 0.053. It now runs without patches, steering and travel, and reads what it read before (125 pixels, 0.009 at most). What the steering adds at a border is bounded by the same step of the normal: 0.004 radians there, which by the mirror's measurement above moves a crest by 0.02 of a wavelength, half what the spacing does.

**The resolve against the mirror** (`domain/gfx/tests/ground_detail_tests.cpp`, "the resolve draws the third pass's function on sloped sand"): the ergs' numbers with the test's wind and a frame's motion (3.7 cm of travel, 2 mm a frame, the wind at its mean, the avalanche clock a third of the way round a cycle), on five planes, each by the origin and 3.7 km out at three looks. Worst difference of 255 over the three looks:

| Plane | Terms | Shaded, origin | Shaded, 3.7 km | Ripple height, 3.7 km | Grain, 3.7 km |
|---|---|---|---|---|---|
| Level | patches, travel | 1 | 4 | 16 | 16 |
| 12°, its line 50° off the wind | steering, patches, travel | 1 | 4 | 7 | 19 |
| The same, storm half way to flat | flattening | 1 | 3 | 7 | 19 |
| Falling with the wind at 27° | lanes (partial weight), episodes | 1 | 2 | 24 | 15 |
| Falling with the wind at 32° | lanes, episodes | 1 | 2 | 35 | 25 |

By the origin every channel of the detail view is within 2. The tolerances are the second pass's (2 by the origin; 5, 28, 2 and 28 at 3.7 km) except the raw ripple height 3.7 km out, which is 40. The reason was isolated before it was widened: on the 32° lee at the feet, where the view's top row is grazing pixels, the channel is 35 with the patches and 22 with the patches alone switched off, everything else the same; the patch's scale at the worst pixel is 0.76, so the wavelength there is three quarters of the second pass's and the reconstruction's millimetre is that much more phase, and the patches also spread the kernels further. No ripple is drawn on that slope (the exposure takes them; the shaded picture is within 1): the channel shows the height the kernels would have. The author's expectation of 1 and 4 on the shaded picture held.

**The reference path tracer** (`systems/renderer/tests/ground_detail_tests.cpp`, "the reference path tracer shades the ergs' sand on a slope", which was "the second pass's sand" and copied only the second pass's fields): FLIP against the path tracer at the feet, 64 samples, one bounce.

| Ground at the feet | Without the detail | With it |
|---|---|---|
| Climbing into the wind at 11.9° | 0.0479 | 0.0495 |
| Falling away from it at 26.0° (the lanes' band) | 0.0476 | 0.0491 |

The detail adds 0.002 and 0.001; the second pass's streaks added 0.003 on the lee.

## The cost on the GPU, measured after the merge

RTX 5090, `msvc-release` at `dffad32`, `engine-view --benchmark` offscreen over the erg's `walk-path.json` (1,201 frames, twice) at 11520×2160 `surround3`, under the machine-wide GPU lock with the GPU at 2 to 8% before each run and other agents' builds on the CPU (8 to 27% at the start of a run, 90 to 100% at the end of two of the nine). Three scenes: the erg with the detail off, with the second pass's numbers (streaks on, nothing of the third pass) and with the third pass's, each under cascaded maps, no shadows and traced shadows. The resolve pass's GPU milliseconds at the median; the frame's total beside it.

| Shadows | Detail off | Second pass | Third pass | Second pass costs | Third pass costs | Frame, third pass |
|---|---|---|---|---|---|---|
| Cascaded maps | 0.897 | 1.625 | 1.760 | 0.728 | **0.862** | 2.71 |
| None | 0.767 | 1.487 | 1.627 | 0.720 | 0.860 | 2.41 |
| Traced | 2.313 | 2.495 | 2.556 | 0.182 | 0.243 | 3.22 |

Two things moved. **The third pass's numbers cost 0.13 to 0.14 ms more than the second pass's** under the same shader (0.06 with traced shadows, where the resolve's cost is elsewhere): the patches' eight hashes a rippled pixel, the steering, the travel. And **the second pass's own numbers cost 0.10 ms more than they did before the third pass was compiled in** — 0.728 against the 0.63 measured at its merge with the same scene, the same path and the same shadows ([second pass](sand-detail-second-pass-2026-09-29.md#the-cost-on-the-gpu-measured-at-the-merge-2026-09-30)); the baseline without the detail is unchanged (0.897 against 0.922). That is the cost of the grainflow, patch, steering and motion code being in the shader at all, whichever flags are set, as the sky's stars cost their share whether or not they are drawn. Against the 0.33 ms the plan gave the sand at the owner's resolution, the third pass under cascaded maps is 0.86 ms: 2.6 times the budget, where the second pass was 1.9 times it.

What to cut first, if the owner wants the budget back: the ripples' kernel sum is still eighteen kernels a pixel and is the largest term; the patches could take their two octaves from one hash instead of two (four hashes a pixel for eight); and the third pass's terms could be compiled as a variant of the resolve so a scene that does not set them pays nothing for their code.

**The owner's decision, 2026-10-03: the cost stands.** The third pass's 0.86 ms under cascaded maps at 11520×2160 is what the sand close up costs at this quality, and nothing is cut to get back to 0.33 ms. The budget in the plan was an estimate made before the terms existed; the terms are wanted. What is kept from the list above is its shape: when the renderer grows a detail setting, the sand's terms are its first rungs (the patches and steering, then the grain's fine octaves, then the ripples' kernel count), each a compile-time variant of the resolve so a lower rung does not pay for a higher one's code.
