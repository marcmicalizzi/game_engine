# The sky's colour bands: where they come from, and what a dither leaves of them

- **Question:** the owner saw colour bands across the sky's slow gradients — a dusk, a dawn, the night under the moon, the glow round the sun — on three 4K panels driven at 10 bits a channel. Are they the final 8-bit quantizer's steps, which a dither removes, or an intermediate quantized too early (a sky table, the aerial table, the exposure after an 8-bit store), which is a different defect? And which noise, and at what depth?
- **Date:** 2026-10-04. **Machine:** the development box (i9-10980XE, 18 cores, Windows 11 Pro 26200); **GPU:** NVIDIA GeForce RTX 5090, driver 610.88, Vulkan 1.4.341. **Build:** `msvc-debug`.
- **Machine state:** shared — other agents' builds on the CPU and their flights holding the GPU lock between this run's frames. Nothing here is a timing: every number is a property of a picture, which the load does not change.
- **Decision:** [ADR-0052](../adr/0052-the-picture-is-quantized-once-with-dither.md) (Proposed): one output encode, dithered with triangular interleaved gradient noise of one code step of the target, at 10 bits in a window that offers them.

## Setup

The instrument is a test, `systems/renderer/tests/banding_tests.cpp`. The erg's sky (its calendar: latitude 31.1°, day 100, the moon 12.39 days old, turbidity 1.6, ground albedo 0.38) over the built-in hills, at 640 × 360 and a 60° vertical field of view, looking towards a light's azimuth or away from it, 24° up so the horizon is low in the frame. Each frame is drawn into:

- an `R32G32B32A32Sfloat` target, which holds the value the resolve hands its store — the encoded picture before any quantizer;
- `R8G8B8A8Unorm` and `A2B10G10R10Unorm` targets, with the dither off and on.

Along every column of sky that is **smooth** in the float picture (nothing covers the pixel or its four neighbours; in every channel a slope under one 8-bit code a pixel and a curvature under a quarter of one, which leaves out stars and discs' edges) it measures:

- **the widest band**: the widest run of one code with a one-code step at each end (a run at 0 or at the top code is the clip, not a band), and how far the float under it moves;
- **one-code steps** between neighbours along the lines, and **coarser** steps (two codes or more where the float moved less than one);
- **floats distinct**: the share of neighbours along the lines whose floats differ — a staircase upstream would make it small;
- **grain**: the RMS of each pixel's code less its float, in codes;
- **the 8 × 8 residual**: the RMS over 8 × 8 blocks of the mean of code less float — what is left once an eye has averaged the grain away, which is what a band is.

The hours are found from the provider: **dusk** the first quarter hour after noon with the sun 3° under the horizon (18:45, the sun at −5.4°), **dawn** the first after midnight with it there again (05:30, −2.4°), **the moonlit night** the first with the sun well down and the moon well up (20:00, the sun at −20.7°).

```powershell
# the kept test: where the bands come from, before and after, at 8 and 10 bits
build/msvc-debug/systems/renderer/engine_renderer_tests.exe -tc="banding:*"
# the measurement behind the choices: the whole day, and the candidate noises (skipped by default)
build/msvc-debug/systems/renderer/engine_renderer_tests.exe -tc="banding: the sweep*" --no-skip
```

## Results

### Where the bands come from: the final quantizer, and nothing upstream

| view (undithered) | widest band, 8-bit | its float spans | widest, 10-bit | floats distinct | coarser steps | captured vs the float rounded |
|---|---|---|---|---|---|---|
| dusk, towards the sun | 17 px | 0.95 codes | 6 px | 1.0000 | 0 | 3.0% differ, all by one, all lower, within 0.062 of the edge |
| dusk, away | **146 px** | 0.99 | 82 | 0.9999 | 0 | 3.2% |
| dawn, towards | 30 px | 0.92 | 15 | 1.0000 | 0 | 3.2% |
| dawn, away | 53 px | 0.95 | 25 | 1.0000 | 0 | 2.9% |
| moonlit night, towards the moon | **128 px** | 0.96 | 91 | 1.0000 | 0 | 2.7% |
| moonlit night, away | 73 px | 1.05 | 27 | 1.0000 | 0 | 3.0% |

**The float under every band is smooth**: nearly every neighbour along a line holds a float of its own (0.9998–1.0000), no step is coarser than one code where the float moved less than one, and the float under the widest band moves about one code — the code the band is. A table or an exposure quantized upstream would show as a staircase in the float picture, with runs of one float value and steps between them; there is none. That matches the code: the sky's tables are float32 buffers read with a software bilinear (`sky_bilinear`), the aerial table the same, the exposure a float the sky pass computes, and the resolve writes `display_encode(...)` of all of it into the target in one pass.

**And the 8- and 10-bit pictures are that float rounded** — to within the store's own rounding, which surprised (below): about 3% of pixel-channels land on the code under the one round-to-nearest gives, every one of them within 0.062 of a code of the half-code edge, the same at both depths. So every band is a step of one code at the final quantizer, which is what a dither fixes.

### The whole day

The widest band at 8 and at 10 bits, every quarter hour, towards the sun and towards the moon (undithered, from the float picture rounded on the CPU; selected rows of the sweep):

| hour | sun | towards the sun: 8-bit / 10-bit | towards the moon: 8-bit / 10-bit |
|---|---|---|---|
| 00:00 | −51.3° | 103 / 50 px | 184 / 136 px |
| 04:30 | −14.8° | 85 / 31 | 147 / 148 |
| 05:15 | −5.5° | 16 / 7 | 118 / 76 |
| 06:00 | +4.0° | 38 / 21 | 32 / 16 |
| 09:00 | +42.0° | 101 / 62 | 33 / 16 |
| 12:00 | +66.7° | 97 / 54 | 57 / 46 |
| 16:30 | +23.3° | 123 / 84 (code 254 / 1017) | 31 / 13 |
| 18:45 | −5.4° | 17 / 6 | 150 / 84 |
| 19:30 | −14.7° | 63 / 25 | 178 / 88 |
| 20:45 | −29.2° | 71 / 32 | **190 / 131** |

There is no hour without bands at 8 bits: the narrowest widest band of the day is 16 px (the sun's glow at dusk and dawn, where the gradient is steepest). The slowest gradients are the night's towards the moon (128–190 px at 8 bits from 20:00 to 01:30) and the sky away from a low sun (146 px straight away from it at 18:45, 150–178 towards the moon at 18:45–19:30). By day the widest bands are just under white round the sun (code 254 of 255, 1017–1022 of 1023), where the display's shoulder compresses the glow — the "gradient round the sun".

### The noise: interleaved gradient noise against white noise

Each candidate applied on the CPU to the float picture, at one code step of amplitude, then rounded (grain and residual in codes):

| view | depth | none: widest / grain / 8×8 | IGN triangular | white triangular | white rectangular |
|---|---|---|---|---|---|
| dusk, towards | 8 | 17 / 0.29 / 0.055 | **8** / 0.50 / **0.030** | 13 / 0.50 / 0.062 | 13 / 0.41 / 0.051 |
| dusk, away | 8 | 146 / 0.29 / 0.180 | **15** / 0.50 / **0.027** | 31 / 0.50 / 0.063 | 66 / 0.41 / 0.051 |
| dawn, away | 8 | 47 / 0.29 / 0.077 | **15** / 0.50 / **0.028** | 22 / 0.50 / 0.063 | 35 / 0.41 / 0.051 |
| night, towards | 8 | 128 / 0.28 / 0.135 | **15** / 0.50 / **0.029** | 28 / 0.50 / 0.062 | 80 / 0.40 / 0.050 |
| night, away | 8 | 70 / 0.28 / 0.169 | **15** / 0.50 / **0.027** | 24 / 0.50 / 0.063 | 40 / 0.40 / 0.050 |
| dusk, away | 10 | 82 / 0.29 / 0.085 | **15** / 0.50 / **0.032** | 27 / 0.50 / 0.063 | 45 / 0.41 / 0.050 |
| night, towards | 10 | 91 / 0.29 / 0.075 | **15** / 0.50 / **0.037** | 24 / 0.50 / 0.063 | 48 / 0.41 / 0.052 |

**Interleaved gradient noise (IGN) through the triangular distribution's inverse wins on both counts**: the same grain per pixel as white triangular noise (0.50 of a code RMS, the triangular dither's √(1/12 + 1/6)), half the error an 8 × 8 average leaves (0.027–0.037 against 0.062), and runs of one code half as long. On its own, with no picture, its 8 × 8 average is 0.018 against white noise's 0.051 (`display_tests.cpp`). Rectangular noise leaves less grain (0.41) and keeps the long runs, because its error's strength follows the signal. The engine's dither is IGN, triangular, the same value for the three channels (a grain of brightness, not of colour).

### Before and after, on the GPU

The kept test's frames (widest band in px, 8 × 8 residual in codes; grain 0.28–0.29 codes before and 0.50 after at either depth):

| view | 8-bit before → after | 10-bit before → after |
|---|---|---|
| dusk, towards the sun | 17 px, 0.064 → 7 px, 0.046 | 6 px, 0.075 → 6 px, 0.048 |
| dusk, away | **146 px, 0.185 → 15 px, 0.047** | 82 px, 0.094 → 15 px, 0.052 |
| dawn, towards | 30 px, 0.063 → 11 px, 0.048 | 15 px, 0.072 → 8 px, 0.049 |
| dawn, away | 53 px, 0.085 → 15 px, 0.044 | 25 px, 0.068 → 11 px, 0.047 |
| moonlit night, towards | **128 px, 0.139 → 15 px, 0.045** | 91 px, 0.083 → 14 px, 0.051 |
| moonlit night, away | 73 px, 0.174 → 15 px, 0.046 | 27 px, 0.095 → 15 px, 0.049 |

The dithered pictures are **the CPU mirror's noise rounded** — the same 3% of pixel-channels a code lower within 0.062 of the edge as undithered, and none further — so the shader's noise is `display.h`'s to the bit. The same frame twice is the same bytes at either depth. **Along one column of the dusk sky a 10-bit target holds 310 distinct codes of one channel**, an 8-bit one 163: the 10-bit target is written at 10 bits, not at 8 and widened.

### The 10-bit window

A 10-bit window cannot be checked on a capture, so it was checked by what can be read: engine-view in a 1280 × 720 window with vsync, 180 and 120 frames (2.8 s and 2.0 s), over a small terrain under the erg's sky at 18:45, looking west 30° up (`--present-bits auto`, then `8`, both with `--capture`):

| `--present-bits` | swapchain format, colour space | the surface offers 10 bits | distinct codes of one channel down one column, read back from the presented image |
|---|---|---|---|
| auto | `A2B10G10R10Unorm`, sRGB non-linear | yes | **472** |
| 8 | `B8G8R8A8Unorm`, sRGB non-linear | yes | 182 |

```powershell
# sky-check.json: {"format":"engine.scene.v1","name":"sky-check","sky":{"latitude_deg":31.1,"day_of_year":100,
#   "moon_age_days":12.39,"turbidity":1.6,"ground_albedo":0.38},"terrain":{"size":65,"extent":64,"seed":5}}
# west-sky.json: one still key at [0, 1.65, 0] (ground) looking at [-40, 24.7, -6], 60 degrees
build/msvc-debug/bin/engine-view --scene sky-check.json --camera-path west-sky.json --time-of-day 18.75 `
  --width 1280 --height 720 --frames 180 --capture window.png --validation --log info
```

The renderer drew into the swapchain's own format (the summary's `output.format`), so the 472 levels are the picture as presented, not a widening of an 8-bit one. `swapchain_tests.cpp` creates, presents and reads back a 10-bit chain on the same surface. **No validation layer was run**: `VK_LAYER_KHRONOS_validation` is not installed on this machine (engine-view's `--validation` says so and carries on), so "no validation message" is not shown here; the engine's own log had no warning or error beyond that one. The owner judges the result by eye.

## What surprised me

- **10 bits alone do not remove the bands.** Under the moon and away from a low sun the sky moves less than a 10-bit code across 80–140 pixels at 640 × 360 — and the owner's panels show the same sky over six times as many pixels. A deeper target makes the steps a quarter as tall; only the dither makes them go.
- **The store does not round to nearest.** On the RTX 5090 (driver 610.88), a float a little above the half-code edge — up to 1/16 of a code above it — is stored as the lower code: about 3% of pixel-channels at 8 and at 10 bits, every one lower, a mean of −0.03 of a code. Vulkan allows it (the conversion's rounding is recommended, not required). It is a bias the dither does not average away, and it is most of what the dithered 8 × 8 residual is: 0.044–0.052 on the GPU against 0.027–0.037 for the same noise rounded exactly. Not compensated: a shader offset of +1/32 of a code would cancel it on this driver and add a bias on any device that rounds to nearest.
- **White noise at a steep gradient adds more low-frequency error than the bands it removes** (0.062 against the undithered 0.055 looking at a dusk sun): where bands are a dozen pixels wide, white noise's own clumps are as large as they are. IGN's spread is what makes the dither a win at every hour.
- **IGN in float disagreed with itself.** The first version computed IGN's two `frac`s in float with `precise`; six pixels of 230,400 came out at the other end of the noise's range on the GPU, because a `frac` is a cliff and a last bit either side of an integer falls off it. The same function in 32-bit fixed point (`display.h`, `display.slang`) is integer arithmetic on both sides and agrees everywhere.

## What it decides

That the bands are the final quantizer's, and that the encode dithers (IGN, triangular, one code step, a function of the pixel alone) at the depth of the target, which is 10 bits in a window that offers them ([ADR-0052](../adr/0052-the-picture-is-quantized-once-with-dither.md)). It does not decide HDR output ([the proposal](hdr-output-proposal.md), E39), and it does not judge the result by eye: that is the owner's, on his panels.

## Caveats

One GPU and one driver; the store's rounding is the driver's and may differ elsewhere (the test allows any rounding within 0.08 of a code of the edge). 640 × 360 at 60°: a band's width in pixels scales with the pixels a degree, so the owner's surround sees each band several times as wide, and the dither's grain the same half code. The metrics are along columns; a band round the sun is circular and a column crosses it at an angle, which makes it look narrower here than across its own width. The 8 × 8 residual is a stand-in for an eye's averaging, not a model of contrast sensitivity.
