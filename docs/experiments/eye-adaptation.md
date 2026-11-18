# The eye's adaptation: what the frame is exposed for (R168, R57's first leg)

**Question.** The sky's rule ([renderer](../subsystems/renderer.md#exposure)) meters the light that
falls at the eye from the sky, so a room at noon is exposed for noon: the village's rooms measured 20
to 38 of 255 with the bounce on, at `ev100` 14.97 indoors and out ([village](../subsystems/village.md#what-was-measured)),
and the cave's passage is darker still. What should the frame be exposed for, how fast should the
exposure move, and what does it cost? Roadmap R168 (exposure indoors) and R57 (eye adaptation; the
post chain's first part). The design is on the renderer's page, "[Exposure: the eye's
adaptation](../subsystems/renderer.md#exposure-the-eyes-adaptation)"; this page is the sources, the
numbers chosen from them, and the measurements.

## The sources, and what was read in each

Every number below was read in the source's own text on 2026-11-17 (the PDFs' text extracted in a
scratch script; nothing recalled). Where a number is a choice rather than a source's, it says so.

| Source | What was read | Used for |
|---|---|---|
| Lagarde and de Rousiers 2014, *Moving Frostbite to PBR*, as Filament's documentation restates it (§8.1, "Physically based camera") | `EV = log2(L S / K)` with K = 12.5 (Canon, Nikon, Sekonic) and S = 100; incident `EV = log2(E S / C)`, C = 250 for a flat sensor; `L_max = 1.2 * 2^EV100` from q = 0.65 and the saturation-based 78 / S. §8.1.4: metering by a downsampled log average "can be unstable and its output should be smoothed over time"; a histogram "can ignore extreme values and offers more stable results"; spot (1 to 5% of the image), centre-weighted and matrix modes; adaptation as `L_avg += (L - L_avg)(1 - e^(-Δt τ))` after Pattanaik et al. 2000 | the units (the engine's rule is the incident form at C = 250, sky.h); the histogram over the log average; the exponential step |
| Epic Games, *Auto Exposure in Unreal Engine* (documentation) | histogram metering between a low and a high percent, "a good starting range is 70 to 80" for the low and 80 to 95 for the high; speed up and speed down in f-stops a second, linear far from the target and exponential within 1.5 stops; an exposure compensation curve over the average scene EV100; a metering mask, "giving importance to pixels toward the center of the screen"; min and max brightness clamps | the shape (a window of the histogram, two speeds, a floor and a ceiling, centre weight); its window was **not** taken, below |
| Durand and Dorsey 2000, *Interactive Tone Mapping* (EGWR) | §3.3: light adaptation simulated with exponential filters, "τR = 0.4 sec for rods and τC = 0.1 sec for cones" from Adelson 1982 and Hood et al. 1987; dark adaptation "can take up to tens of minutes" and is not modelled; the matinee: entering a theatre on a sunny day, "initially, everything inside appears too dark". §4.2: "Nikon's weighted centered metering uses 75% of the weight in the 12mm center of a 24x36 film"; spot metering with Gaussian weights | the light-adaptation constants; the centre weight |
| Krawczyk 2007 (PhD thesis, MPI Informatik; the method of Krawczyk, Myszkowski and Seidel 2005, *Perceptual effects in real-time tone mapping*) | eq. 4.5, the exponential decay after Durand and Dorsey; eq. 4.6, `t_rod = 0.4 s`, `t_cone = 0.1 s`, which "describe the adaptation to light", dark adaptation "not simulated because the full process takes up to tens of minutes"; eq. 4.7, the rods' sensitivity `s(Y) = 0.04 / (0.04 + Y)` after Hunt 1995; eq. 4.11, the key `a(Y) = 1.03 - 2 / (2 + log10(Y + 1))` from the absolute adapting luminance, specified empirically ("a matter of experience") because a log-average-relative key makes "dark night scenes appearing too bright and very light too dark"; eq. 4.12, `t(Y) = s(Y) t_rod + (1 - s(Y)) t_cone`; ch. 3: scotopic light adaptation is "extremely rapid", 75% within the first 400 ms, the cones need about 3 minutes to reach full sensitivity | the key (the eye's partial compensation); the blend of the two constants |
| Kalloniatis and Luu, *Light and Dark Adaptation* (Webvision) | the rod–cone break of dark adaptation "after 5 to 8 minutes"; the absolute threshold "about 10^-5 cd/m2 after about forty minutes in the dark"; cones lose sensitivity below about 0.03 cd/m2 | why dark adaptation's real constant is not used (below) |

Not read, and not used: Pattanaik et al. 2000's own text (its copies refused the fetch; Filament and
Krawczyk restate its filter, which is all that was taken), ISO 2720 itself (the constants are as
Filament quotes them).

## What was chosen, and why

- **Metered**: the frame's own luminance, as the resolve exposed it and back through that exposure
  to cd/m² — a histogram of 128 quarter-stop bins from 2^-10 to 2^22 cd/m², one pixel in each 2 × 2
  tile. A histogram rather than a log-average chain, for the reason Lagarde gives: the extremes are
  left out, and the frame is not dominated by the sun's disc or a black doorway jamb.
- **The window**: the log mean of the weight between the **10th and the 90th** percentile. Unreal's
  starting range (70–80 to 80–95) meters the brightest quarter of the frame, which is a camera's
  aim — keep the highlights — and in a room with a door would meter the door. The eye adapts to the
  field's level (Durand and Dorsey: "a weighted average"; Krawczyk: the logarithmic average), so the
  window is the log average with the tails trimmed: the brightest tenth (the doorway, the sky
  through a window, the sun) and the darkest tenth (a jamb's shadow) do not pull it. A choice, on
  the sources' log average; tunable (`renderer.exposure.low`, `high`).
- **Centre-weighted**: a disc a third of the view's width across weighs 16 against 1 outside it,
  which puts 74.6% of the weight in it on a 16:9 view — the centre-weighted meter Durand and Dorsey
  cite (75% in the 12 mm circle of a 36 mm frame: the circle is a third of the frame's width).
- **The target**: the exposure that shows the metered luminance at **Krawczyk's key** — `EV100 =
  log2(π L / (2.5 a(L)))` in the engine's units (sky.h shows L at `L π / (2.5 · 2^EV100)`). The key
  is the eye's partial compensation in one curve: 0.03 at a moonless ground, 0.42 in a room lit to
  100 lux, 0.72 on sunlit sand. On the erg's sand at noon (0.7 × 110 klux / π) it lands within 0.05
  of a stop of the sky's own rule (15.39 against 15.42), so adapting changes nothing where the rule
  was right; in a room at 19 cd/m² it is 5.8, nine stops up from noon's 15; under a full moon (a
  ground at 0.02 cd/m²) −0.45, against the rule's 0.9 — the same partial compensation the rule's
  knee was fitted to by eye (sky-2026-09-30), from a published curve instead.
- **Light adaptation** (the picture darkening): Durand and Dorsey's constants, 0.1 s for the cones
  and 0.4 s for the rods, blended by the rods' share at the metered luminance (Krawczyk eq. 4.12).
- **Dark adaptation** (the picture brightening): **1.5 s, a choice, not the eye's.** The eye's cone
  plateau takes minutes and the rods' forty; Krawczyk does not simulate it ("tens of minutes") and
  runs the light constants both ways. A game cannot keep a room black for two minutes, and both ways
  at 0.1 s would make a doorway a flash. 1.5 s keeps the order the eye has — the room reads dark
  first, as in Durand and Dorsey's matinee, and opens up — and settles a nine-stop step to within a
  quarter stop in 5.4 s. Tunable (`renderer.exposure.dark_tau_s`).
- **The floor and the ceiling**: EV100 −6 to 18. −6 is under the rule's moonless night (−4.5), so
  the darkest scene the sky draws is not lifted past what the rule already gave it; 18 is above
  sunlit snow's target (15.7). Tunable.
- **One exposure for a surround**: the metered view is the one holding the target's centre — the
  centre monitor — and the side monitors are not metered at all. The owner plays on three screens and
  reads the side ones as the periphery (plan 04 §4.6, the attention region); a bright window at the
  edge of a side monitor would otherwise pump the centre's picture, and an eye does not adapt to what
  it sees only at 60° off its axis.

## Determinism

The bins are integer counts added by atomics, so their totals do not depend on the threads' order;
the window and the step are one thread's loop in bin order; the step's time is the host's
`FrameDesc::dt_s` or, where it gives none (engine-view, the protocol), the frame index's at 60 Hz —
the clock every path and capture already advance. So frame *n* of a path is the same exposure in
every run on one device (`exposure_tests.cpp`, "the same walk is the same exposure", to the bit and
to the byte); across devices the GPU's `exp2` and `log2` may differ in the last place, as every
shaded pixel may.

## Measurements

(Filled by the row's runs below.)
