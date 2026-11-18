# HDR10's codes, and what the overlay's 3 % is

**Roadmap R187, 2026-11-18 (batch 69). An analysis by computation on the CPU: no engine binary ran
for it.** The SDR encode rounds its own codes since R183 (`display_stored`; renderer.md, "The code
is the engine's, not the store's"). Rounding HDR10's PQ codes the same way was built that morning
and taken out again, because one case moved: `overlay_tests.cpp`'s "overlay: on HDR10 its white is
paper white and its blend is in linear light" measured its half-alpha blue's blend at **3.39 %**
against its **3 %**, where the store's rounding had given **2.14 %**. This page says what that 3 %
is, what the case's true invariant is, and the check that states it. **The decision to round HDR10
in the encode is the owner's**; nothing here changes a test or a shader.

The scripts are in the row's scratch (`<scratch>/b69/R187/`: `overlay_hdr10.py`, `search.py`,
`sweep.py`, `variants.py`, `check.py`, `check2.py`) and mirror, in float32 where the device computes
in float32: `display.h`'s `pq_encode`, `pq_decode`, `k_bt709_to_bt2020` and its inverse in double,
the noise (`display_ign`, `display_dither`, `display_dithered`), `capture.cpp`'s `capture_light`,
`frame_encode.slang`'s `encode_under_ui`, the overlay's layer (`overlay.slang`, the pipeline's
"over" blend in `device.cpp`), and the case's arithmetic line for line.

## What the case looks at

- The scene is the 20 m hills at 320 × 180, camera 3 m up, pitched 6.5° down, 60° vertical field.
  The blue quad covers x 100–179, y 20–59: rows that look **4° to 17° above the horizon**, and the
  hills stand at most 1.37 m, so **every pixel under the blue is the stand-in sky's flat clear**.
  The case's "under" is one colour, and only the dither varies across the quad's 3,200 pixels.
- **Before** (no overlay): the resolve discards an empty pixel (`sky_is_clear`), so the target keeps
  its clear, `stand_in_sky(k_sky)` computed on the CPU — PQ values 484.458, 508.328, 562.618 codes,
  stored as **484, 508, 563** by nearest and by the store's rule alike.
- **With** the overlay: the frame takes the linear path; the linear image is cleared to
  `sdr_decode(k_sky)` as half floats, the overlay's blue (64, 128, 255) at alpha 128 is blended
  "over" into the 8-bit sRGB UI layer (premultiplied: sRGB codes 42.83, 93.30, 187.84, stored 43,
  93, 188), and `encode_under_ui` composites them in linear light at paper white (200 nits), then
  BT.2020, PQ and the dither: undithered PQ values **449.925, 477.951, 573.437** codes.
- **The figure**: each Rec. 709 channel decoded from the three stored BT.2020 codes
  (`bt2020_to_bt709`), against `under × (1 − α) + blue × α` with `under` the *before* frame's own
  decoded light, as `|seen − expect| / max(expect, 0.05)`, the worst over the quad.

## The two figures, reproduced

- **2.13541 %** — reproduced to the digit. It is one code triple, **(451, 479, 574)**, at the quad's
  pixel (102, 20) in the mirror, on its red channel (seen 0.16000 of paper white against 0.15665).
  The mirror gives exactly this worst under round-to-nearest and under the store's measured rule
  (the value cut to 14 fraction bits, then nearest: batch 68's banding log), for the *with* frame and
  the *before* frame in every combination, and with the layer's 8-bit quantization or without it.
- **3.38659 %** — reproduced as a figure, not as a pixel. It is exactly the triple **(451, 478,
  574)**: red one code up and green one code down from the 2.14 % pixel's (a search of every triple
  within three codes; no other gives either figure). **The mirror's floats cannot produce that
  triple under either rule**: the dither is one number for all three channels, so red rounding up
  while green rounds down needs green − red under 28 codes, and the mirror puts it at **28.026**
  (28.015 with the linear image in float32, 28.19 without the layer's quantization; a native
  `exp2(log2)` pow moves it by under 0.005). The device's floats, then, sat at least 0.015 of a code
  on the other side of that line, and its nearest rounding dealt a pixel into the split; the store's
  rule, which moves each channel's edge by its own 0 to 1/16 of a code, happened to close that window
  in the morning's run. **Worst achievable by construction for these colours, over every dither
  value, is 2.140 % in the mirror** under all four rule pairs — the 1.25 points between the two
  figures are two hundredths of a code of float difference, not a property of either rule.

## The case's colour space at large

Swept over 4,000 random cases — the under colour any light below paper white, the UI colour any
8-bit colour, alpha any 8-bit value — and every value of the dither (2,048 a case), the same figure
by construction (`sweep.py`):

| Rules (under, with) | p50 | p90 | p99 | p99.9 | max | cases over 3 % |
|---|---|---|---|---|---|---|
| nearest, nearest | 1.93 % | 4.00 % | 8.21 % | 12.0 % | 13.0 % | 17.2 % |
| cut, cut (the store) | 1.93 % | 4.02 % | 8.20 % | 12.0 % | 13.0 % | 17.2 % |
| cut, nearest (R183's form) | 1.93 % | 4.03 % | 8.21 % | 12.0 % | 13.0 % | 17.3 % |

**The 3 % is a bound neither rule holds by construction.** One case in six of the colour space can
exceed it under either rounding, and the two rules' distributions are the same to the second digit.
The figure is relative in Rec. 709 light with a floor of 0.05 of paper white, and a Rec. 709 channel
is three BT.2020 channels through a matrix whose red row is about 1.66, −0.59, −0.07: a code of error
on a bright green channel lands on a dark red channel at the green's size. The worst sample was a
dark-red, bright-green under at alpha 6/255. The case's own colours sit at 2.14 % by construction,
inside the 3 % by 0.86 of a point; the device's floats took it to 3.39 % with one pixel.

## The true invariant, and the check that states it

**What the encode promises** for a pixel of the quad, in each BT.2020 channel *i* separately (no
matrix between the promise and the check):

1. The device's undithered value `v_i = PQ(n_i) × 1023`, where `n_i` is the composite in nits,
   `(1 − α) × under_i + (M · L)_i × W` (`M` the Rec. 709 to BT.2020 matrix, `L` the layer's light,
   `W` paper white).
2. The stored code `c_i` = the rounding of `v_i + d × room`, with the dither `|d| ≤ 1` and `room ≤ 1`.
   Round to nearest leaves `c_i − (v_i + d)` in [−1/2, 1/2]; the store's measured rule in
   [−1/2 − 1/16, 1/2). So **`v_i − 25/16 ≤ c_i ≤ v_i + 3/2`** under either rule.
3. The case does not know `v_i`: it knows the before frame's stored code `u_i` of the under (no
   dither on the clear), whose true value lies in [`u_i − 1/2`, `u_i + 9/16`] by the same two rules;
   the linear image holds the under as a half float (relative error ≤ 2^−11, and `M`'s entries are
   all positive, so the BT.2020 nits keep that bound); and the 8-bit sRGB layer holds `blue × α` at
   one of the two codes round it (`L_lo`, `L_hi`, the floor and the ceiling).
4. PQ and its inverse are monotone, so `v_i` lies in [`lo_i`, `hi_i`] with
   `lo_i = PQ((1 − α) · PQ⁻¹((u_i − 1/2)/1023) · (1 − 2^−11) + (M · L_lo)_i · W) × 1023` and
   `hi_i` the same with `u_i + 9/16`, `(1 + 2^−11)` and `L_hi`. No slope argument is needed (an
   earlier draft bounded the propagated term by PQ's slope ratio; `x · PQ′(x)` is not monotone over
   the range, so that bound does not hold everywhere, and the interval form replaces it).

**The check** (proposed for `hold_hdr`'s HDR10 branch; the scRGB branch keeps its relative figure,
which is half floats with no dither and holds 0.32 %):

```cpp
// Every BT.2020 channel of the half-alpha quad within the codes a linear-light composite of the
// before frame's own codes and the layer's two possible codes may store: dither one code, rounding
// half a code and the store's sixteenth, the under's own rounding, a half's rounding, and 1/32 of a
// code for the device's float32 against this CPU's.
const f64 lo = pq_code((1 - a) * pq_nits(u - 0.5) * (1 - 0x1p-11) + ui_nits_lo) - 25.0 / 16 - 1.0 / 32;
const f64 hi = pq_code((1 - a) * pq_nits(u + 9.0 / 16) * (1 + 0x1p-11) + ui_nits_hi) + 3.0 / 2 + 1.0 / 32;
CHECK(c >= lo); CHECK(c <= hi);   // c, u: the with and before frames' stored codes of channel i
```

`pq_code` (nits to PQ code, `pq_encode × 1023` in double) and `pq_nits` (a code to nits) are the
case's helpers; `ui_nits_lo`/`_hi` are `(M · L_lo)_i × W` and `(M · L_hi)_i × W`. The check needs
the stored BT.2020 codes of both frames, which `CapturedFrame::light` has already decoded and its
`CodeHistogram` holds only in aggregate: the restated case reads them from the raw 10-bit capture
of both frames.

**Verified on the CPU** (`check2.py`): over 20,000 random cases × 257 dither values × 3 channels,
simulated with the half-float under, the layer's nearest code and both rounding rules, **no correct
composite falls outside the window** (0 under nearest, 0 under the store's rule). The window is 4.6
codes wide at the median (5.7 at p99, 13.6 at its widest, where a dark layer's 8-bit sRGB step is
several PQ codes). For the case it is red 447.30–451.89, green 475.74–480.35, blue 571.10–575.46:
both device triples, (451, 479, 574) and (451, 478, 574), are inside; one code more on every channel,
(452, 480, 575), is outside. **Its teeth**: the same quad composited in PQ codes instead of light
stores (442, 472, 573), outside by five codes on red, and over the colour space a PQ-space composite
falls outside the window in 92 % of cases. An alpha wrong by 8/255 moves the case's codes by 2.3,
2.0 and 0.5, which the window sees only near its edge; the check is about where the composite
happens, not a measurement of alpha (that stays the white's job and the layer's own test).

**The white's check already is a bound**: a PQ code at 200 nits is 0.94 % of the light, so
`25/16` codes either way is 0.9855–1.0147 of paper white, inside its 2 %; measured 0.9865–1.0051.
**The outside figure (1.93 % against 3 %) is the first kind**: the before and with frames share the
noise and the rounding rule, so their codes differ only where a half's rounding crosses an edge; the
same code-space form, `|c_with − c_before| ≤ 1` a channel, is its invariant (reasoned here, not
swept).

## What rounding HDR10 in the encode changes, and what it costs

- **On a display**: about 3 % of the PQ channels of an HDR10 picture move one code up, on this card,
  to the code the CPU's rounding gives (the SDR measure, which batch 68 found the same at 8 and 10
  bits; not re-measured on PQ). One 10-bit PQ code is 0.98 % of the light at 100 nits, 0.94 % at
  200, 0.90 % at 1,000 and 1.9 % at 1 nit (computed from ST 2084's constants in `display.h`); the
  dither is a code of noise on every pixel already, so a picture rounded either way is the same
  grain with a different 3 % of its pixels a code apart.
- **What it buys**: the same as SDR's — the picture's bytes are the engine's on every driver, so a
  10-bit HDR10 picture can be held to a band invariant (no step of two codes where the float moved
  under one) the way `banding_tests.cpp` holds SDR, and a driver's last bit stops moving a case.
- **What it costs**: one multiply, a floor and a divide per channel in the resolve's and the
  encode pass's PQ branch — nothing to measure at a frame's scale (to confirm with the frame budgets
  at the merge: `pwsh tools/frame-budget.ps1`).
- **What has to move with it**: the overlay case's HDR10 blend check, to the form above, in the same
  commit, and the outside check to its code-space form; renderer.md's "What is still the store's"
  loses HDR10. The stand-in sky's clear and the data views stay the store's (one value, or not a
  picture).

## The owner's decision

Rounding HDR10 in the encode makes its codes the engine's on every driver, as SDR's are since R183,
at no measurable cost; on a display it moves about 3 % of PQ channels by one code, under a percent
of their light, inside the dither's own grain. The overlay case did not find a fault in it: its 3 %
is a figure one colour in six exceeds under either rounding, and the store's rule passed it that
morning because of where two hundredths of a code of float difference happened to fall. **My
recommendation: round HDR10 in the encode, and restate the case's blend check in BT.2020 codes in
the same commit** — the check above passes every correct composite by construction and still
catches a composite done in the PQ curve. It is independent of which HDR route R79's measurement
keeps: both HDR10 and the UI's seam are built, and whichever stays is better with its bytes fixed.
