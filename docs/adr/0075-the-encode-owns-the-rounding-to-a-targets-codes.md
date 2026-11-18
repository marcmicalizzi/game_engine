# ADR-0075: The encode owns the rounding to an integer target's codes

- **Status:** Accepted
- **Date:** 2026-11-18
- **Plan references:** docs/plan/04-renderer.md §4.7 (the output and the UI's seam), [ADR-0052](0052-the-picture-is-quantized-once-with-dither.md) (the picture is quantized once, with dither), [ADR-0054](0054-the-tools-ui-is-dear-imgui-drawn-by-the-renderer.md) decision 3 and its status note of 2026-10-19 (the overlay on HDR in linear light)
- **Docs touched:** [renderer](../subsystems/renderer.md) ("The code is the engine's, not the store's", "The overlay"), [gfx](../subsystems/gfx.md) ("The encode rounds, not the store"), [the analysis](../experiments/hdr10-code-rounding.md), `docs/roadmap.md` (R187, R183)

## Context

The picture's output encode (`display_output` in `display.slang`, mirrored by `display.h`) turns
linear radiance into a colour target's values, and until 2026-11-18 it handed an integer target a
continuous value and left the last step — which of the two codes round it — to the store. Vulkan
pins only whole numbers there: rounding a float to a normalized fixed-point code to nearest is a
"should", and either neighbouring code is allowed. The RTX 5090 cuts the value to four bits under a
code before it rounds, so a value up to a sixteenth of a code above an edge stores as the lower
code — about 3 % of pixel-channels — and which pixels those are turns on the last bit of the
driver's own `pow`.

On 2026-11-17 the display driver went from 617.42 to 621.24. One pixel of the erg's dawn moved
across that cut, and `banding_tests.cpp` found a step of two codes where the float moved 0.999999
of one: the picture's banding was the driver's to decide. R183 made the SDR encode round to its own
codes the next morning (`display_stored`: the code's centre, half a code from either edge, so any
store that errs by less than 7/16 of a code returns it). It built the same for HDR10's PQ branch and
took it out again, because `overlay_tests.cpp`'s HDR10 case measured its half-alpha blend at 3.39 %
of the light against a 3 % figure, where the store's rounding had given 2.14 %.

The analysis of that figure (R187, [the page](../experiments/hdr10-code-rounding.md), computed on
the CPU) found it was a bound neither rounding holds by construction: one colour in six of the
case's space exceeds 3 % under either rule, the two rules' distributions agree to the second digit,
and the 1.25 points between the morning's two figures were two hundredths of a code of float
difference between the device and a mirror, at one pixel. The case's true invariant is per BT.2020
channel in codes, and a check of that form holds every correct composite and still refuses one done
in PQ's curve.

**The owner's decision, 2026-11-18**: "let's go for your recommendation on HDR10" — round HDR10 in
the encode, and restate the case's blend check in BT.2020 codes in the same commit.

## Decision

1. **Every picture the encode writes into an integer target is rounded by the encode**, to the
   centre of the target's code it rounds to, nearest with a half up (`display_stored`, with the
   target's codes in `DisplayTarget::codes`): the SDR picture at 8 and 10 bits (since R183) and
   **HDR10's PQ codes** (since R187), in the resolve's `display_output` and in the frame passes'
   `encode_under_ui` alike. `display.h` mirrors each branch on the CPU (`display_code`,
   `display_stored`, and `display_pq_codes` for HDR10), and a case holds the device's codes to the
   mirror's (`hdr_output_tests.cpp`, `banding_tests.cpp`).
2. **No picture relies on how a driver's store rounds.** A check that reads a stored picture holds
   it to the codes the encode chose — to the mirror's code, or to a window of codes derived from
   what the case knows — never to a figure that a store's choice between two codes happens to pass.
3. What stays the store's is what the encode does not write as a picture's code: the stand-in
   sky's flat clear (one value, computed on the CPU), the debug and data views, a blend the
   hardware does after the shader (the overlay on an SDR target, an overlay drawn straight onto an
   HDR target by a frame with no linear path), and a float target, which has no codes.

## Consequences

- A picture's bytes are the engine's on every driver: a driver update moves a case only where the
  device's own arithmetic moved by more than a last bit near an edge, and an HDR10 picture can be
  held to a band invariant the way `banding_tests.cpp` holds SDR.
- About 3 % of an HDR10 picture's PQ channels move one code up on this card, to the code the CPU's
  rounding gives: under a percent of their light, inside the dither's own grain.
- The cost is one multiply, a floor and a divide per channel in the PQ branch; the frame budgets at
  the merge say whether it shows.
- `overlay_tests.cpp`'s HDR10 blend is held to `hdr10_blend_window` per BT.2020 channel, and its
  CPU case simulates the window against both rounding rules; the 3 % figure is gone, not widened.
  scRGB keeps its relative figure (half floats, no dither, no codes).
- A new writer of a picture into an integer target ends on `display_stored`, or says why the store
  rounds instead.

## Revisit when

- The engine draws into an integer target whose codes are not uniform steps of the encoded value
  (an `_Srgb` store, a target with a hardware transfer of its own).
- HDR10 is dropped as a route (R79), which leaves only the SDR half of decision 1.
- A device is found whose store does not return a code's centre as that code; the centre is half a
  code from either edge, so that would be a broken store and a driver bug to report.
