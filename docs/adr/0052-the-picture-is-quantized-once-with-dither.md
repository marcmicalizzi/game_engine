# ADR-0052: The picture is quantized once, with dither, at the depth the display takes

- **Status:** Proposed
- **Date:** 2026-10-04
- **Plan references:** docs/plan/04-renderer.md §4.1 (step 8, "Post and upscale") and §4.6 ("Presentation")
- **Docs touched:** `docs/subsystems/renderer.md` ("The output encode"), `docs/subsystems/gfx.md` ("The output encode"), `docs/subsystems/apps.md` (`--dither`, `--present-bits`, the summary's `output` block), `docs/subsystems/protocol.md`, `docs/experiments/sky-banding-2026-10-04.md`

## Context

With the physical sky ([ADR-0048](0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md)) the owner saw colour bands across the slow gradients — a dusk, a dawn, the night under the moon, the glow round the sun — on three 4K panels driven at 10 bits a channel, chosen for exactly that. The picture was quantized to 8 bits with no dither anywhere: the renderer's colour target is `R8G8B8A8Unorm` offscreen and the swapchain's `B8G8R8A8Unorm` in a window, and the resolve wrote `pow(radiance, 1/2.2)` straight into it.

Before fixing anything the bands were measured ([the experiment](../experiments/sky-banding-2026-10-04.md)): the erg's sky drawn into a float target, which holds the encoded value before any quantizer, and into 8- and 10-bit targets. The float under the bands is smooth — nearly every pixel along a line holds a value of its own, and no step is coarser than a code where the float moved less than one — and the 8- and 10-bit pictures are that float rounded (to within the store's own rounding, about a sixteenth of a code at the edge between two codes). So every band is a step of one code at the final quantizer: nothing upstream (the sky's tables, which are float32 buffers read with a software bilinear; the aerial table; the exposure) is quantized early. At 8 bits the widest band on a 640 × 360 view was 146 pixels at dusk, 128 under the moon; at 10 bits still 82 and 91, because the night sky moves less than a 10-bit code across a hundred pixels. A deeper target alone does not remove them.

The alternatives were: a deeper target alone (10 bits removes three bands in four but leaves the slowest gradients banded); dither alone at 8 bits (no bands, but a grain of half an 8-bit code where the display could take a quarter of it); a dither in each writer (the resolve, its sky, the reference's tonemap), which drifts; a noise with a frame index in it (temporal dither), which breaks every byte-for-byte equality the engine holds; a blue-noise texture, which needs a binding the encode does not otherwise have; and white noise from an integer hash against interleaved gradient noise.

## Decision

1. **One output encode.** Linear radiance — exposed and through the sky's shoulder — becomes the target's code values in one function, `display_output` in `domain/gfx/shaders/display.slang` (no bindings, no entry points), which every writer of the picture includes: the resolve's shaded pixels and its sky, and the reference path tracer's tonemap. The ray path writes visibility and the resolve shades it, so it has no encode of its own. `domain/gfx/display.h` is its CPU side.
2. **The encode dithers**: triangular noise of one code step of the target — 1/255 at 8 bits, 1/1023 at 10 — added in the encoded domain before the store, narrowed within one step of either end so nothing clips (black stays black, white white, every mean its value). The noise is **interleaved gradient noise**, computed in 32-bit fixed point so the CPU and every GPU agree to the bit, through the inverse of the triangular distribution's cumulative function. It was chosen over white triangular noise by measurement: the same grain per pixel (0.50 of a code RMS) and half the error an 8 × 8 average leaves (0.027–0.037 of a code against 0.062).
3. **The noise is a function of the pixel's place in its view and of nothing else** — no frame index, no binding, not the view's offset in the target, so a surround's centre view draws the noise a single view of that monitor draws, and the reference tracer's one-view image the noise the resolve put there. The same frame twice is the same bytes; two pictures that agree before the encode agree after it, so the equalities between rasterizers, hosts, layouts, occlusion on and off and the ray path hold unchanged with the dither on.
4. **Quantized once, at the depth the display takes.** The steps are the target's own (`gfx::display_steps`): the renderer's colour target is the swapchain's format in a window, and a window takes `A2B10G10R10Unorm` (then `A2R10G10B10Unorm`) in the sRGB non-linear colour space where the surface offers one (`engine-view --present-bits auto|8|10`, `auto` the default). Nothing between the resolve and the present requantizes. **Never an `_SRGB` format** for the picture: the encode is the shader's, and the swapchain chooser takes a UNORM format wherever the surface offers one; an `_SRGB` or float target gets no noise rather than a wrongly scaled one.
5. **Offscreen is 8 bits**, because its capture is an 8-bit PNG and a picture quantized once at 8 bits with the dither is a better 8-bit picture than a 10-bit one rounded again. A windowed `--capture` of a 10-bit window reduces it to the PNG through the same noise at 8 bits.
6. **`RenderSettings::dither`, on by default in both hosts** (`engine-view --dither on|off`, the protocol's `settings.dither`). Tests that hold the lighting model to a CPU reference within a code or two turn it off: they measure the model, and the quantizer is measured once, by the banding test, where it is the subject. `gfx::ResolveParams::dither_steps` zero — `domain/gfx`'s own tests, a data view — is the encode exactly as it was.

## Consequences

- Every shaded picture of every scene moves by up to one code where the dither is on; with it off, and for every data view (ids, depth, normals, the shadow and detail views, the albedo view), nothing moved. The stand-in sky's flat clear is not dithered (it has no gradient, and it is the clear the pass already wrote). `ResolveParams` grew from 336 to 352 bytes.
- A capture is now a picture with grain in it; a test that compares a picture to a number within a code turns the dither off or allows the code.
- The owner's display gets the picture at 10 bits with a quarter-code grain; an 8-bit display or PNG gets it at 8 bits with a half-code grain. Neither bands.
- HDR output is not decided by this: it needs a tone curve parameterized by the display and a target past [0, 1]. When it is built, its encode is a second branch of `display_output`, not a second function.

## Revisit when

- A temporal dither is wanted (it would cost every byte-for-byte equality, so it would be an option, not the default).
- HDR output is built: the encode's curve and its steps change, and the 10-bit PQ target's noise is one code of PQ, not of the 1/2.2 curve.
- A display or capture path needs an `_SRGB` target, which would mean scaling the noise through that curve's slope.
