# What feeds a shore's wave train: fetch, wind and the onset (W153)

Roadmap W153 (2026-11-20). The owner, after walking the forest: small ponds a few metres across, under trees, draw a shore train that oscillates about every half second **even with the wind at zero** — on one a white foam ring jumps in and out, on another the whole pale margin swells and shrinks. A pool with no fetch has millimetre ripples and no swash. This page holds the audit of how a body got its waves, the published relations a body's train is taken from with the range they were measured over, and the numbers chosen; [water](../subsystems/water.md#what-feeds-the-train) says what was built.

## How a body got its waves (main at 4a51ab021)

Read before anything was changed:

- **The open sea** (`open: true`): sixteen JONSWAP bands from its wind (its own `wind_m_s`, the ground's where it follows it, or the scene's) over its `fetch_km`; its shore train the wind sea's peak and a second component, or its swell; a tide. All of it from the wind, the fetch, the swell and the tide the scene names ([water](../subsystems/water.md#the-spectrum)).
- **A lake** (closed, with a `wind_m_s` or under a scene wind): the same bands over **its own reach along the wind** (`gfx::water_fetch_field` on its depth map, no longer than `fetch_km`), its train the wave the fetch laws give there (`gfx::swash_wave`), lifted and run up its shore.
- **A pond or a pool with no wind**: no table; **the material's four sines** at the block's `wave_amplitude_m` and `wave_length_m` (defaults 2 cm and 2.5 m), travelling at the deep-water speed — and S97's **foam line** (`water.slang`, `water_foam`), drawn on every body with a depth map that has no wind and no tide: a line a tenth of `foam_m` wide running up and back between 0.3 and 1 of `foam_m` from the waterline at the main sine's phase, a pale trail behind it, its strength the sines' amplitude over 2 cm. **It read no wind**: not the block's, not the scene's, not `renderer.wind.speed_m_s` (which reaches only a body with a table, `scene_wind.cpp`'s `water_wind_frame`).
- **A stream's pool**: a channel's surface is its solve's ([water](../subsystems/water.md#channels)); it draws no shore train.

**Where a still pool was given a train**: the staging's three pools by the clearing (`shaded-pool`, `opening-pool`, `wet-margin`, each about 7.5 m across) name no wind, 5 mm sines 1.8 m long and the default `foam_m` of 0.6 m. Their main sine's period is √(2π · 1.8 / 9.81) = 1.07 s: the line between 0.18 and 0.6 m from the waterline went in and out about every half second, a quarter as strong as at 2 cm, with its trail widening and narrowing behind it — the ring and the pale margin. **Why zero wind did not silence it**: nothing in it reads a wind.

**A second way a zero wind did not silence a train**: the GPU scene's `water_swash_numbers` took the block's own `wind_m_s` whenever the wind the reader resolved was 0, so a body following a calm scene wind (`sky.wind.speed_m_s` 0) ran up its shore at the wind its block named.

**And the lightest winds**: seventeen of the staging's region basins and the inlet's backwater name winds of 0.21 to 1.29 m/s; the fetch laws give them sheets of millimetres. No picture showed those, but nothing in the tree said a wind that light raises no wave at all.

## The published relations, and how far they reach

**Fetch-limited growth** (the relations the tree already takes; their reading is [the wind page's](wind.md#how-the-sea-answers) and [the shore page's](shore.md#the-spectrum-for-every-body)): JONSWAP's peak law ω_p = 22 (g² / U₁₀ F)^(1/3) and the energy law g H / U² = 1.6 × 10⁻³ (g F / U²)^(1/2) (Hasselmann et al. 1973, as the Coastal Engineering Manual II-2 carries them), with the spectrum's shape by its development (Lewis and Allos 1990) and full development at g F / U² = 19,539. **Their range**: the simple power laws describe fetch-limited growth reasonably over a dimensionless fetch of about 3 × 10² to 10⁴ (Hwang and Wang 2004, read through a search result's quotation; the paper's PDF could not be read here). A pool's fetch under a wind is far below it: 4 m under 20 m/s is g F / U² = 0.098, under 5 m/s 1.6. **In wind-wave tanks**, whose fetches are metres to tens of metres, the growth plotted against the dimensionless fetch scatters widely and goes over to JONSWAP's only at longer fetches, while **Toba's 3/2 law between height and period holds in the tanks and the field alike** (Lamont-Smith and Waseda 2008, *J. Phys. Oceanogr.*, read in its abstract through search; the full text was not read). So at a pond's fetch the laws give the right order — centimetres at a fraction of a second — and not a tank's measured number; the engine keeps them there, since a second law for short fetches would put a seam in every body's height at its crossover, and the train it feeds runs up millimetres either way.

**The onset of waves**: Kahma and Donelan (1988, *J. Fluid Mech.* 192, 339–364; the abstract read on cambridge.org) measured the first wavelets, about 10 µm high, in a flume with a slope gauge: growth begins at a friction velocity near **2 cm/s**, below the shear-flow theory's 4 to 5 cm/s (which the paper's introduction puts at about 1.3 m/s over aerodynamically smooth water, as quoted by a search result); the wind at which waves become **readily visible** is appreciably higher and depends on the water's temperature through its viscous damping; above a millimetre of height growth follows the theory. **The Beaufort scale's sea criteria** (the Hong Kong Observatory's table, read): force 0, under 1 kn, the sea like a mirror; **force 1, 1 to 3 kn, scale-like ripples with no foam crests**; **force 2, 4 to 6 kn, small wavelets whose glassy crests do not break**; force 3, 7 to 10 kn, larger wavelets whose crests begin to break, perhaps scattered white horses. Force 1 is 0.3 to 1.5 m/s at 10 m (the scale's 1946 relation v = 0.836 B^(3/2), quoted from Wikipedia through search).

**A swell entering from a connected sea**: a swell that reaches a basin through a gap is diffracted, Sommerfeld's solution applied to water waves by Penney and Price (1952, the gap as two mirrored semi-infinite breakwaters), with diffraction diagrams for gaps of half to five wavelengths (Johnson 1952), carried by the Coastal Engineering Manual II-7 (read through search results; the manual's figures were not read). Behind a gap narrower than a few wavelengths the swell spreads and falls, so a lagoon's shore takes a fraction of the sea's swell, more of it the wider the gap and the closer the shore to it.

## What a pond of a few metres shows

From the relations above, on a foreshore of 1:10 (the engine's own numbers on a steeper bowl, printed by `water_scene_tests.cpp`'s case, are two sections on):

| Wind at 10 m | What the scale says | A 4 m pool by the fetch laws | Its train |
|---|---|---|---|
| 0 m/s | a mirror (force 0) | nothing | none |
| 1.2 m/s | scale-like ripples, no foam (force 1) | under the onset | none; any table's ripples drawn |
| 5 m/s | wavelets begin to break at sea (force 3) | about 5 mm at 0.17 s, 4.5 cm long: capillary-gravity ripples | runs up about a millimetre; no foam |
| 20 m/s | a gale at sea (force 8) | about 2 cm at 0.27 s, 11 cm long | runs up a few millimetres; no foam (the breaking foam's threshold is a 2 cm wave, `water.slang`) |

A pond under trees takes the wind its block or the scene names at 10 m; a canopy's shelter is the author's to say.

## The numbers chosen

- **No foam line on a still shore.** A body with no train breaks nothing: the shore's entry carries a reach of 0, and S97's line is drawn on no body. `foam_m` is read by nothing; it stays in the schema so older scenes read.
- **The train takes the wind the reader resolved, 0 included.**
- **`gfx::k_swash_wind_onset_m_s` = 1.5 m/s**, the top of force 1: at or under it `gfx::swash_wave` gives no wave, so a body has no swash, no lift and no foam of breaking. It sits above the smooth-flow theory's 1.3 m/s and Kahma and Donelan's earlier inception, at the wind where the scale's ripples become wavelets — the first gravity waves that could run up a shore. The surface's table is not cut: a body's ripples under light air are drawn as before.

## What the engine draws now, by body

`water_scene_tests.cpp`, "a body's shore train is its own fetch's under its wind, and none without one" (on the CPU, through the renderer's scene reader): a pool filling a 3 m bowl with no wind, under light air (1.2 m/s) and under a calm scene wind with a block wind of 6 m/s: no wave and no run-up; under 20 m/s its own fetch and the laws' height and period there — 2026-11-21, msvc-debug: **3.6 m, 1.7 cm at 0.26 s, a run-up of 1.3 cm** on the bowl's steep bank (Stockdon's grows with the slope, under the steep shore's ceiling of 1.97 H); a lake filling a 350 m bowl under 7 m/s: **520 m, 8.4 cm at 0.96 s, a run-up of 1.7 cm**; the open sea over the same bowl: the laws at its 100 km, as before. The breaking foam's threshold is a 2 cm wave (`water.slang`), so the gale's pool makes no foam and the lake's waves make flecks where they break.

On the forest's staging: the three pools lose their foam line and keep their 5 mm sines; the eighteen bodies under the onset lose a swash of millimetres; the two region basins at 1.52 and 1.98 m/s, the mountain lake and the eastern sea at 7 m/s are as they were.

## Not done

- A lagoon's swell by diffraction through its gap: no closed body knows it is joined to the sea.
- A short-fetch law of the tanks' own: the laws are taken below their field range.
- The canopy's shelter of a pool's wind; a pool's capillary bands are drawn with gravity's dispersion.
- A body loaded with no table does not follow a wind raised during the session, and no shore's train turns with a wind that veers (the arrival and flux fields are built at load); W151's live wind reaches the trains' height and run-up through `water_wind_frame` alone.
