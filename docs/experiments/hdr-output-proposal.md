# HDR output: a proposal, not a measurement

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), E39; [04 §4.6](../plan/04-renderer.md#46-extreme-displays), "HDR metadata per output"):** should the engine present an HDR signal on the owner's panels, and by which route — HDR10 (PQ on a 10-bit target, BT.2020 primaries) or scRGB (linear on a 16-bit float target)?
- **Date:** proposed 2026-10-04. **Machine:** the owner's: an RTX 5090 driving three 4K panels as one 11520 × 2160 NVIDIA Surround display at 10 bits a channel.
- **Machine state:** nothing measured yet.
- **Decision:** none. This page proposes the experiment that would produce one; there is no ADR for HDR until it has run. What is decided is the SDR output it would extend ([ADR-0052](../adr/0052-the-picture-is-quantized-once-with-dither.md)).

## What the engine has

- **Scene-referred radiance.** With a sky ([ADR-0048](../adr/0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md)) the resolve and the reference compute radiance in the sky's physical scale, up to the sun's disc and down to a moonless night, before anything is done to show it.
- **An exposure rule** (incident-light metering at ISO 100 with a knee below which darkness is only partly compensated; [renderer](../subsystems/renderer.md#exposure)), the frame's own light with no history.
- **A display-referred SDR tail**: the sky's per-channel shoulder (`sky_tone`, linear to 0.6 then rolling off towards 1), the transfer curve (a 1/2.2 power) and the dither, in one output encode (`display_output`, `display.slang`) that every writer of the picture calls ([renderer](../subsystems/renderer.md#the-output-encode)).
- **A 10-bit swapchain** in the sRGB non-linear colour space where the surface offers one (`--present-bits`), so the SDR picture already reaches the panels at 10 bits.

## What it lacks

- **A tone curve parameterized by the display.** The shoulder maps exposed radiance onto [0, 1] of an SDR signal whose white is wherever the desktop puts it. HDR needs the display's **peak luminance** (and its full-frame peak) and a **paper-white level** — the luminance a diffuse white is shown at, where UI and SDR content sit — and a curve that holds everything below paper white as the SDR picture shows it and spends the range above it on highlights (the sun's aureole, a sunset's horizon, the moon's disc), rolling off at the peak.
- **A place where UI composites.** There is no UI yet ([04 §4.7](../plan/04-renderer.md#47-ui-coordinate-architecture)). In HDR it has to be composited at paper white, in the output's own space, after the tone curve and before the encode — so the output encode becomes the frame's last pass rather than the end of the resolve.
- **The extensions.** `VK_EXT_swapchain_colorspace` on the instance, which the colour spaces past sRGB non-linear need even to be listed, and `VK_EXT_hdr_metadata` on the device for the mastering display's primaries and luminance and the content's MaxCLL and MaxFALL. Neither is enabled.
- **16-bit or float captures.** Every capture is an 8-bit PNG. An HDR picture cannot be inspected through one; it needs a half-float EXR (or a 16-bit PNG with a `cICP` chunk naming PQ and BT.2020) and a viewer that tone-maps it, and the tests' references would compare in the scene's radiance or in PQ codes rather than in 8-bit bytes.

## The two routes

**HDR10.** `A2B10G10R10Unorm` (or `A2R10G10B10Unorm`) in `VK_COLOR_SPACE_HDR10_ST2084_EXT`. The engine converts its linear Rec. 709 radiance to BT.2020 primaries (one 3 × 3 matrix), scales it to nits by the exposure and paper white, applies the display's tone curve, encodes with SMPTE ST 2084 (PQ), and dithers by one 10-bit PQ code — the existing encode with a different curve and the same noise. The swapchain is the size of today's 10-bit one, and the signal is what the panel takes, so the desktop compositor has nothing to convert. PQ's 10-bit steps are close to the threshold of visibility across its range, so the dither matters as much here as in SDR; out-of-gamut colours and the mastering metadata are the engine's to get right.

**scRGB.** `R16G16B16A16Sfloat` in `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`: linear, Rec. 709 primaries, 1.0 at 80 nits, values above 1 for highlights and below 0 for colours outside Rec. 709. The engine writes linear radiance scaled to that convention after its tone curve; the compositor converts to the signal. No PQ in the engine and no quantization worth dithering, and UI composites linearly. It costs twice the bytes a pixel: 199 MB a swapchain image at 11520 × 2160 against 100 MB, and the resolve's colour write is the bandwidth-bound part of the frame there ([renderer](../subsystems/renderer.md#the-sky), "What it costs"). The compositor does not fit the picture to the display's peak; the engine still needs the peak to tone-map.

## What Windows and NVIDIA Surround do — to be checked, not assumed

- With Windows' "Use HDR" on for a display, the desktop compositor works in scRGB and sends the panel an HDR10 signal; an SDR swapchain is shown at the "SDR content brightness" level as paper white. What a **10-bit SDR** swapchain's precision survives through that composition, and with HDR off whether the 10 bits reach the panels (the NVIDIA control panel's output colour depth at 10 bpc), is the first thing to measure — it is also what today's `--present-bits 10` relies on.
- Whether the HDR toggle is available for an **NVIDIA Surround** display group, whether all three panels then receive HDR10, and what peak luminance the system reports for the group (DXGI's `MaxLuminance`, `MaxFullFrameLuminance` and `MinLuminance` per output) are unknown here. Vulkan reports none of those numbers; the engine would read them from DXGI on Windows or take them from the player.
- Which surface formats and colour spaces NVIDIA's driver offers with HDR off and on once `VK_EXT_swapchain_colorspace` is enabled, and whether presenting an HDR chain changes the FIFO pacing the display pacer was tuned to ([apps](../subsystems/apps.md#pacing)).

## What to measure on the owner's hardware

1. **The offers.** With the extension enabled, every surface format and colour space the Surround display offers with HDR off and with it on (a probe beside `engine-cli gpu.adapters`), and the luminances DXGI reports for it.
2. **The SDR 10-bit path, by eye and by instrument.** A 10-bit ramp and the erg's sky at the banding test's hours ([sky-banding](sky-banding-2026-10-04.md)) through `--present-bits 10` with the dither on and off, HDR off and on: whether the panels show what the 10-bit picture holds.
3. **HDR10.** The same scenes through a PQ encode at a few paper whites (100, 200, 300 nits) and the reported peak: banding by the instrument in PQ codes and by eye, highlight detail round the sun and the moon, and the encode's cost at 11520 × 2160 (expected small beside the resolve; to be measured with `--wait-quiet`).
4. **scRGB.** The same scenes through a half-float chain: the resolve's and the present's cost at 11520 × 2160 against the 10-bit chain, the compositor's added latency (present timing), and whether its conversion matches the HDR10 picture.
5. **Captures.** A half-float EXR capture of either route, and what a test would compare in.

The ADR it would produce names the route (or both, chosen by what the surface offers), the tone curve's parameters and where they come from, where UI composites, and what a capture is.

## What is built

**2026-10-09** (roadmap R79; the measurement is still R60's, and the owner's). Both routes, so the measurement can choose between them; **no ADR**, because the route is the measurement's to decide and it has not run.

- **The extensions.** A presenting device enables `VK_EXT_swapchain_colorspace` on the instance and `VK_EXT_hdr_metadata` on the device where they are offered, and only then ([gfx](../subsystems/gfx.md#the-hdr-branches-and-the-surfaces-offers)). The RHI says it in its own words: `gfx::ColorSpace`, `SurfaceFormat`, `HdrMetadata`, `PresentFormat`.
- **The probe** (measurement 1): `engine-cli gpu.displays` ([protocol](../subsystems/protocol.md)) lists every output Windows reports — bits per colour, DXGI colour space (which says whether "Use HDR" is on), primaries, minimum, peak and full-frame luminance, Windows' SDR white level — and per Vulkan device what a hidden window on it is offered. No window is shown, no device opened, no setting changed.
- **The chains**: engine-view's `--present-format hdr10` (`A2B10G10R10Unorm` in `HDR10_ST2084`) and `scrgb` (`R16G16B16A16Sfloat` in `EXTENDED_SRGB_LINEAR`), refused with a sentence naming the surface's offers where it has neither; `auto` and `sdr10` are today's `--present-bits auto` and `10` exactly ([apps](../subsystems/apps.md)).
- **The encode** ([renderer](../subsystems/renderer.md#hdr-output)): the SDR shoulder with its ceiling moved to the display's peak over **paper white**, so everything the SDR picture shows below its knee is shown at the same luminance at paper white; then BT.2020 and PQ with the dither at one 10-bit code, or scRGB's linear light. Peak and paper white are the flags', the tunables', what Windows reports for the window's output (DXGI's `MaxLuminance`, the SDR white level), or 1000 and 200 nits, and the summary says which. An HDR10 chain carries matching static metadata. **UI** has a named seam (between the curve and the encode, in `display_output`) and nothing built.
- **The capture** (measurement 5; 2026-10-09, R79's second half): `--capture <file>.exr` writes a picture's **linear light** as a half-float OpenEXR — linear Rec. 709 with 1.0 at the picture's white (paper white for HDR, whose nits the file's `whiteLuminance` carries), so an SDR, an HDR10, a scRGB and a linear capture of one frame compare directly — with `<stem>.codes.json`, the histogram of the codes it was stored in (PQ codes for HDR10, and for scRGB the PQ codes an HDR10 signal of its light would carry). **The resolve's linear radiance** is `--present-format linear`: the exposed radiance before any curve, offscreen into a half-float target, so the tone curve can be read off a pair of files. And **the 10-bit ramp** is `--view ramp`, a grey test pattern through the frame's own encode and dither in place of the scene ([renderer](../subsystems/renderer.md#hdr-output), [apps](../subsystems/apps.md)).
- **The script** (2026-10-09, R80): `tools/hdr-measure.ps1` runs measurements 1 to 5 in one go under the GPU lock and writes one folder with everything below and a README to read at the panels.

**What it costs** (measured 2026-10-09 at the batch 12 merge gate; `msvc-release`, the erg walk at 1920×1080 offscreen with traced shadows and rings, 30 warm-up frames, GPU lock held, `--wait-quiet 300`, 8 to 17% of other CPU load during the runs, so upper bounds): the resolve pass is 0.200 ms at the median through SDR (the frame budget's quiet run of the same path), 0.203 ms through HDR10, 0.200 ms through scRGB and 0.199 ms through the linear capture encoding; the frame is 0.725 to 0.728 ms throughout. The encode itself is about three microseconds a frame at 1080p, within noise. What remains to measure on the owner's surround is the present side: the 16-bit scRGB swapchain's bytes at 11520×2160 and the compositor's latency against the 10-bit HDR10 chain, which is measurement 4 of this page.

**What this machine reported** (2026-10-09, `engine-cli gpu.displays`, the desktop as the owner left it): `DISPLAY1`, 7680 × 1440, driven at 10 bits, Windows' HDR **on** (`rgb_full_g2084_none_p2020`), DXGI's luminances 0.0001 / 1015 / 658 nits (minimum, peak, full frame), SDR white level 280 nits; the RTX 5090 offers a window on it `A2B10G10R10Unorm` in `hdr10_st2084` and `R16G16B16A16Sfloat` in `extended_srgb_linear` beside the SDR formats, and `VK_EXT_hdr_metadata`. A second, 8-bit SDR output (1280 × 720, 270 nits) offers the SDR formats only. So both routes can be presented here; the Surround group was not the desktop's configuration at the time.

## What the owner measures

Two runs of one script (R80), with a release build: the engine changes no display setting and reads only what Windows reports, so **Windows' "Use HDR" and the NVIDIA control panel's output colour depth (10 bpc) are set by hand before each run**.

```powershell
tools/dev.ps1 build -Preset msvc-release
# Run 1: "Use HDR" off for the Surround display, the NVIDIA control panel at 10 bpc.
tools/hdr-measure.ps1 -Out D:\workspace\game_engine_local\e39\hdr-off
# Run 2: "Use HDR" on (the SDR content brightness where you keep it: it is one of the paper whites).
tools/hdr-measure.ps1 -Out D:\workspace\game_engine_local\e39\hdr-on
```

Each run takes the GPU lock once, probes the displays (measurement 1: `displays.json`), then shows, borderless over the whole display for four seconds each, the ramp and the erg's sky at dusk, dawn and the moonlit night looking west and east (the banding test's hours) through `sdr10` with the dither on and off (measurement 2), `hdr10` at paper whites of 100, 200 and 300 nits and at Windows' SDR white level, the reported peak in all of them (measurement 3), and `scrgb` (measurement 4), each captured on its last frame as an EXR with its code histogram (and the SDR ones as a PNG too), with its present timings; and the offscreen linear radiance under each view (measurement 5). Formats the display does not offer with HDR off are recorded as not offered. **Read `README.md` in the folder while you look at the panels**: it says what each picture should and should not show, and holds the table of every capture's distinct codes and present timings. `-DryRun` shows what would run; `-Views`, `-Formats` and `-PaperWhites` run a part of it again. What it cannot measure is the eye: whether a band is visible, whether the highlights look right, whether HDR on the Surround group is worth having — that verdict, and R60, are the owner's.
