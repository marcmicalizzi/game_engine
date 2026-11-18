# The sun against the sky on level ground (R182, 2026-11-18)

**What prompted it.** The owner finds outdoor scenes flat: conifer crowns one even grey-green,
dark rooms near black beside a bright outside. Two rows measured the night before.
G218 (its page is `docs/experiments/conifer-crown-light.md`, on the branch `b68-G218` as this is
written) wrote that on level sand in the open a sunlit point returns only about 2.1 times what a
shadowed one does with the bounce off, and that a shaded needle returns as much light as an albedo
of one under the whole exposure. R168 read the sky's own meter at noon at
110.0 klux of which the sky is 21.6. A clear sky at a high sun gives a level surface several times
more direct than diffuse light, so either the 2.1 is not a ratio of light, or something beside the
sky lights a shadow, or the sky's diffuse light is too strong against its sun. This page is which,
with the sky's model held to published clear-sky models on the CPU
(`domain/gfx/tests/sky_balance_tests.cpp`), and the mend designed for a second leg. No GPU was used:
nothing here was drawn, and every number is either read out of G218's kept captures or computed by
the sky's double-precision mirror (`domain/gfx/tests/sky_reference.h`).

**In one paragraph.** The 2.1 is sunlit **sand** over shaded **bark**, two surfaces pooled in one
class; on the sand alone G218's own captures give **4.4 to 4.5** with the bounce off, which is what
the mirror computes for that sky (4.6) and within 3 to 13 % of the published models at the same
aerosol at a high sun. The sky's sun and its diffuse light are in balance. Two things are not.
**The air a turbidity draws is about one Linke unit hazier than its name** (a scene's 2.5 draws the
air of a Linke turbidity of 3.6: sunlit ground 4.2 to 4.8 times its shadow under a high sun, where a
Linke turbidity of 2.5 gives 8 to 9). And **what lights a shadow on anything that is not level is
the ambient's lower half**: the
scene's ground taken as the desert's sand (albedo 0.84, 0.69, 0.47, whatever the terrain is made
of), under the full sun everywhere and hidden by nothing, which is 77 to 81 % of the light on a
shaded wall, trunk or needle with the bounce off. A dark room is neither: its exposure is metered
under the open sky wherever the eye stands.

## What the 2.1 was

G218's measure (`Crown.cs` in its scratch) decodes a capture's code by the 2.2 power, takes the
picture's luminance over the albedo view's, and compares pixels the shadow view calls lit with
those it calls blocked. A capture is exposed radiance through the display's shoulder
(`k_sky_shoulder`, 0.6) and the 1/2.2 power, so the quotient is linear in light below the shoulder
and compressed above it. The quotient was honest; **the class was not**. "The ground beside it" was
every pixel of another instance that is not green, which pools the sand with the trunks and bare
branches of the nine other trees. Its lit pixels are nearly all sand and its blocked pixels mostly
bark, so the 2.1 (its 300 m views: 2.07 and 2.00) is sunlit sand over shaded bark.

Read again from the same captures (`<scratch>\b69\R182\Ground.cs`: pixels whose albedo is
sand-coloured, binned by the albedo's luminance, the shoulder inverted; sun 63°, turbidity 2.5,
`--raster rt --shadows rt`, main's build a1eae86f, G218's runs of 2026-11-17 21:05, the session
locked):

| surface (albedo, linear) | bounce | lit | blocked | lit over blocked | by the 2.2 power alone |
|---|---|---|---|---|---|
| sand (0.843, 0.687, 0.470) at 30, 100, 300 m | off | 0.99 to 1.01 | 0.222 to 0.226 | **4.36 to 4.53** | 4.24 to 4.39 |
| | on | 0.98 to 1.00 | 0.167 to 0.178 | 5.56 to 5.99 | 5.40 to 5.81 |
| bark (0.26, 0.18, 0.12 and 0.30, 0.22, 0.15) | off | 0.89 to 0.96 | 0.48 to 0.53 | 1.77 to 1.91 | the same |
| | on | 0.77 to 0.89 | 0.27 to 0.41 | 2.19 to 3.17 | the same |

*Lit* and *blocked* are exposed light per unit albedo: 1 is a white level surface under the
metered light. The display transform is 3 % of it: every lit sand pixel has a channel above the
shoulder (red is exposed to 0.88 and shown at 0.80) and none is clipped. The mirror's prediction for
this sky at 63° is a lit sand of 1.01, a blocked one of 0.222 and a ratio of 4.55 (the table below,
between its 50° and 70° rows): **the captures are the model, to within 4 %**.

So a shadow on open sand is 22 % of the sunlit sand beside it, not 48 %; and a trunk in shade is
half as bright as a white card in the sun, which is the finding the rest of this page follows.

## Every term that lights a point on level ground

Read in `sky.slang`, `sky_luts.slang`, `visibility_resolve.slang` (`fs_resolve`), `material.slang`
(`scene_ambient`), `brdf.slang`, `gi_update.slang` and `gi_cache.slang` on main 0073605c. Units are
sun units (`gfx::k_sky_sun_lux`: 1 is 128 klux); magnitudes are the mirror's, luminance-weighted,
for a noon at 40° N (the sun 50° up at an equinox, 73° at midsummer: the table's 50° and 70° rows)
at the schema's default turbidity of 2 and at the test scenes' 2.5.

| term | where | what it is | klux at 50° and 70°, T 2 | T 2.5 |
|---|---|---|---|---|
| the sun, direct | `fs_resolve`: `sky_light_at` times `shadow_visibility` times `brdf_direct` | the sun's illuminance outside the air (1 at 1 AU) times the transmittance table towards it from the point's height; nothing below the planet's horizon; the shadow query's 0 or 1 (a filtered share under the maps) | 94.7, 100.1 normal; 72.5, 94.0 on the level | 88.5, 94.7; 67.8, 89.0 |
| the sky, diffuse | `fs_resolve`: `sky_ambient(frame, normal)` | the frame's nine harmonics of the sky-view table over the upper hemisphere (2,048 directions, `sky_frame_main`), convolved with the cosine lobe and over π; at a level normal it is the hemisphere's integral to 3 % from 30° up (5 to 7 % over with the sun at 10°). **Unoccluded**: no term of the resolve hides any of it with the bounce off, and a material's occlusion map is the only thing that scales it | 17.4, 19.5 | 21.0, 23.9 |
| the ground, in the same harmonics | `sky_frame_main`: `local_albedo * (lit + level) / π` as a uniform lower hemisphere | `SceneData::ground_albedo` under this frame's full sun and sky. **For any scene with a terrain it is `terrain_sand_albedo()`**, 0.84, 0.69, 0.47 (luminance 0.71), whatever the terrain's materials are (`scene.cpp`, `load_scene`); 0.2 grey without a terrain. A level normal takes none of it; a wall half; a face turned down all | 0 on the level; 63.7, 80.5 facing down | 0; 63.0, 80.0 |
| a floor, a stand-in ambient | none | with a sky the stand-in's two halves are not read; `sky_ambient` clamps at zero and adds nothing | 0 | 0 |
| foliage's sphere of normals (batch 67) | `fs_resolve`: `brdf_foliage_ambient`, `foliage_direct` | no new light: the same harmonics read at four directions (the sheet's front and back, along and against the view) and weighed by `transmit` and `spread`; its mean over every normal is the first harmonic, half the sky's level light plus half the ground's | 41.2, 49.7 (the sphere's mean) | 42.3, 50.9 |
| the bounce, when on | `scene_ambient` reading the world-space cache | the cells' ratio to the harmonics times the harmonics' luminance at the normal: what the frame's structures let through and what they return (`gi_bounce`: a hit's albedo under the sun through one shadow ray, plus the cache at the hit; a miss is `sky_environment`, the sky-view table above the horizon and the uniform ground below). Foliage reads its crown's prior times coarser cells. Where no level has a sample it is the harmonics again | on G218's sand: the shadow goes from 0.222 to 0.17 of white | |
| the air to the eye | `sky_aerial` | the surface's light times the aerial table's transmittance plus its in-scatter | 1 to 3 % of a sunlit sand's light at 300 m (from the captures) | |
| exposure | `sky_frame_main`, `sky_exposure_ev100` | incident metering: 128,000 π over the metered lux, the sun on the level plus **π times the sky's mean radiance** (the dome: 18.8, 19.4 klux at T 2), so a white level surface in the sun is 0.98 to 1.02. Metered at the eye's height under the open sky, with no walls and no history | 91.3, 113.4 metered | 89.4, 111.2 |
| the display | `display_output` | below 0.6 a channel is itself, above it `0.6 + 0.4 (1 - exp(-(x - 0.6) / 0.4))`, then the 1/2.2 power | sunlit sand's red 0.88 shown as 0.80 | |

The sky's share of the meter (R168's 21.6 of 110.0 klux) is the dome, not the level light; the two
are within 8 % of each other from 50° up, and at 30° the dome is a quarter to a third more (the
test's `frame` and `dome` columns).

## The published models

Read on 2026-11-18. **The formulas were read in two open implementations, not in the papers**:
pvlib's `clearsky.py` and `atmosphere.py` (main branch) and Radiance's `gendaylit.c` (NREL's
mirror), through a fetch that quotes the source lines; each implementation cites its paper, named
here. No table of any paper was read; every published number on this page is those formulas
evaluated by the test.

| what | source | how far read | used for |
|---|---|---|---|
| Direct normal and diffuse horizontal irradiance from the aerosol's depth at 380 and 500 nm, water, ozone and the ground's albedo | Bird and Hulstrom 1981, *A Simplified Clear Sky Model for Direct and Diffuse Insolation on Horizontal Surfaces*, SERI/TR-642-761 | the formulas, in pvlib `clearsky.bird` (every constant quoted); its defaults: ozone 0.3 cm, a forward-scattered share of 0.85, 1,364 W/m² outside the air | the comparison at the engine's own aerosol |
| Bird and Hulstrom's broadband aerosol depth, `0.27583 aod380 + 0.35 aod500` | the same report | pvlib `bird_hulstrom80_aod_bb` | the above, and the Linke turbidity of an aerosol |
| Global and direct irradiance from a Linke turbidity | Ineichen and Perez 2002, *Solar Energy* 73:151-157 | the formulas, in pvlib `clearsky.ineichen` (sea level, no enhancement) | what a turbidity's **name** asks |
| The Linke turbidity of an air with a broadband aerosol depth and a water column: `(9.4 + 0.9 m)(δcda + δw + δa)`, `δcda = -0.101 + 0.235 m^-0.16`, `δw = 0.112 m^-0.55 w^0.34` | Kasten 1980 and 1996; Molineaux et al. 1998; Ineichen 2008, *Solar Energy* 82:1095 | pvlib `kasten96_lt`, with the comments quoting Ineichen 2008 | the Linke turbidity of what the engine draws |
| The air mass, `1 / (cos z + 0.50572 (96.07995 - z)^-1.6364)` | Kasten and Young 1989 | pvlib `get_relative_airmass` | every model above |
| The luminous efficacy of the diffuse light and of the beam, by the sky's clearness bin | Perez, Ineichen, Seals, Michalsky and Stewart 1990, *Solar Energy* 44:271-289 | the eight bins' coefficients and both formulas, in Radiance `gendaylit.c` (`glob_h_diffuse_effi_PEREZ`, `direct_n_effi_PEREZ`) | watts into lux: a clear sky's diffuse light is 125 to 150 lm/W where its beam is about 100, so the diffuse share of illuminance is larger than that of irradiance |
| What a Linke turbidity is, and its usual values | the SoDa service's page, *Linke turbidity factor* (cited by pvlib) | the page: it "typically ranges between 3 (clear skies) to 7 (heavily polluted skies)"; a deep blue sky is "just above 1"; in a European summer it "is larger than 3"; in polluted cities "close to 6 - 7" | which turbidity a clear day is |
| Daylight's round numbers | Wikipedia, *Daylight* (its table carries no citation for these rows) | 111,000 lux "bright sunlight"; 20,000 lux "shade illuminated by entire clear blue sky, midday" | a check of the order only: 5.5 |
| AM 1.5 | Wikipedia, *Air mass (solar energy)* | the sun 48.2° from the zenith; 109,870 lux for the reference spectrum's 1,000.4 W/m²; its rule of thumb takes the diffuse light as 10 % of the direct | a check of the order only |
| The engine's own model | Hillaire 2020, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique* (EGSR), and Bruneton and Neyret 2008 | not read for this row; neither gives irradiance on the ground in a table this page could use (as recalled) | |

**Not read**: ASTM G173's own page (the host did not resolve) and its aerosol depth; the CIE
standard clear sky; the IES and CIE daylight illuminance tables; Perez's all-weather sky. **Chosen,
not read**: 1.5 cm of precipitable water (a temperate summer's; a Linke turbidity moves 0.3 for a
centimetre either way round it).

**What they say a shadow on open level ground is.** Global over diffuse light on a level surface,
in lux (Ineichen and Perez at the Linke turbidity through Perez's efficacies; in brackets Bird and
Hulstrom at that turbidity's aerosol, the same way):

| sun | Linke 2.5 | Linke 3 (the source's "clear skies") | Linke 4 | Linke 5 |
|---|---|---|---|---|
| 10° | 2.6 (2.4) | 1.9 (1.7) | 1.4 (1.35) | 1.3 (1.2) |
| 30° | 6.0 (5.35) | 4.5 (3.8) | 3.05 (2.6) | 2.5 (2.2) |
| 50° | 8.1 (7.65) | 6.3 (5.5) | 4.5 (3.8) | 3.4 (3.0) |
| 70° | 9.4 (9.2) | 7.4 (6.6) | 5.3 (4.7) | 4.3 (3.5) |

A shadow on open ground under a high sun is a ninth of the sunlit ground beside it in very clear
air, a sixth to a seventh on an ordinary clear day, a fifth at a Linke turbidity of 4 and a quarter
to a third in haze; with the sun at 30° a sixth to nearly a half; at 10° most of the light is the
sky's. The two models differ by 2 to 21 %, which is the width a bound on the engine can have.

## The engine's sky against them

`tools/dev.ps1 test -Filter gfx` runs it; alone, with the table printed,
`engine_gfx_tests "-tc=sky: the sun against the sky*"` (no device; 50 s in `msvc-debug` beside
other builds, 2.5 s a row, most of it the mirror's own sky-view table, 663,000 marched steps a sun:
that is what the case adds to a debug `gfx.1`, and a release build runs it in a few seconds). The
case builds the mirror's transmittance and multiple-scattering tables for an aerosol, the sky-view
table for each sun, and integrates the sky's radiance times the cosine over the hemisphere twice:
through the table (96 by 384 directions) and by marching every direction with no table (24 by 96,
48 steps).
The beam is the sun's illuminance through the transmittance table times the elevation's sine. klux,
luminance-weighted (Rec. 709 weights over the model's 680, 550 and 440 nm), 128 klux a sun unit.

| T | aerosol at 550 nm | sun | beam, normal | beam, level | sky, level | as the resolve adds it | **G/D** | Bird + Perez, same aerosol: beam, sky, **G/D** | the air's Linke turbidity | Ineichen + Perez at that: G/D | **at the named T: G/D** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 0.108 | 10° | 36.5 | 6.3 | 6.6 | 6.9 | 1.96 | 31.0, 8.2, 1.65 | 3.0 | 1.9 | 4.8 |
| | | 30° | 80.9 | 40.5 | 13.7 | 13.8 | 3.95 | 75.9, 15.4, 3.47 | 3.1 | 4.2 | 9.1 |
| | | 50° | 94.7 | 72.5 | 17.3 | 17.4 | **5.20** | 90.8, 16.6, 5.19 | 3.35 | 5.5 | 10.1 |
| | | 70° | 100.1 | 94.0 | 19.1 | 19.5 | **5.92** | 95.8, 17.0, 6.28 | 3.5 | 6.2 | 10.7 |
| 2.5 | 0.163 | 10° | 27.3 | 4.7 | 7.0 | 7.3 | 1.67 | 23.5, 8.9, 1.46 | 3.6 | 1.6 | 2.6 |
| | | 30° | 73.0 | 36.5 | 16.2 | 16.1 | 3.25 | 69.7, 17.9, 2.95 | 3.6 | 3.4 | 6.0 |
| | | 50° | 88.5 | 67.8 | 21.0 | 21.0 | **4.23** | 85.7, 19.4, 4.38 | 3.8 | 4.8 | 8.1 |
| | | 70° | 94.7 | 89.0 | 23.5 | 23.9 | **4.79** | 91.6, 20.2, 5.25 | 3.9 | 5.45 | 9.4 |
| 3 | 0.217 | 10° | 20.4 | 3.5 | 7.2 | 7.4 | 1.49 | 19.5, 9.8, 1.35 | 4.2 | 1.4 | 1.9 |
| | | 30° | 65.8 | 32.9 | 18.2 | 18.0 | 2.81 | 64.3, 20.0, 2.61 | 4.0 | 3.0 | 4.5 |
| | | 50° | 82.7 | 63.4 | 24.2 | 24.2 | **3.62** | 80.7, 23.3, 3.65 | 4.2 | 4.3 | 6.3 |
| | | 70° | 89.6 | 84.2 | 27.3 | 27.8 | **4.08** | 87.4, 22.4, 4.67 | 4.3 | 5.0 | 7.4 |
| 5 | 0.434 | 10° | 6.4 | 1.1 | 6.9 | 6.9 | 1.16 | 9.9, 10.7, 1.16 | 6.5 | 1.2 | 1.3 |
| | | 30° | 43.6 | 21.8 | 22.4 | 21.9 | 1.97 | 48.0, 25.7, 1.93 | 5.8 | 2.2 | 2.5 |
| | | 50° | 63.1 | 48.3 | 32.8 | 32.5 | **2.47** | 65.7, 31.0, 2.62 | 5.9 | 2.9 | 3.4 |
| | | 70° | 71.8 | 67.5 | 38.4 | 38.9 | **2.76** | 73.3, 32.9, 3.09 | 6.0 | 3.4 | 4.3 |

- **At its own aerosol the mirror agrees with the published models.** Its beam is Bird's through
  Perez's efficacy to 2 to 7 % from 30° up at turbidities to 3 (18 % over at 10°, and 0.65 of it in
  the thickest haze at 10°: three wavelengths are not a spectrum). Its global over diffuse is 0.87
  to 1.19 of Bird and Perez's, a little less sky than theirs at a low sun and a little more at a
  high one, and 4 to 19 % under Ineichen and Perez's at the air's own Linke turbidity from 50° up.
  The march and the table agree to under 1 %, so the sky-view table loses nothing of the hemisphere.
  **The sky's balance of sun and sky is not what makes a shadow bright.**
- **The name is what is off.** `earth_atmosphere` gives a turbidity T an aerosol depth of
  (T − 1) × 0.108 at 550 nm, reasoning that a Linke factor counts clean atmospheres of extinction.
  A Linke factor counts them **over the whole solar spectrum, water vapour's absorption included**,
  and with 1.5 cm of water the clean air and the vapour alone are a Linke turbidity of 2.2. By
  Kasten's formula the air the engine draws for T has the Linke turbidity **1.30 + 0.91 T** at an
  air mass of 2: 3.1 for the schema's default of 2, 3.6 for the 2.5 most test scenes write, 4.0 for
  3, 5.8 for 5. So the default sky is the source's "clear skies", and a scene that writes 2.5 or 3
  for a clear day gets shadows 1.7 to 2 times as bright against the sunlit ground as that
  turbidity's (the last column over the eighth: 8.1 to 9.4 against 4.2 to 4.8 at 2.5).
- **The same mirror with the aerosol a Linke turbidity names** (an aerosol of
  (T / 11.2 − 0.197) / 0.749 at 550 nm: 0.035 for 2.5, 0.094 for 3, 0.214 for 4, 0.333 for 5) gives
  a global over diffuse of **8.1 and 9.2 at 50° and 70° for 2.5, 5.5 and 6.3 for 3, 3.6 and 4.1 for
  4, 2.85 and 3.2 for 5**, within the same bounds of the published models; so the mend is the
  mapping and not the scattering. The committed case builds 2.5, the air clearer than anything the
  scale draws today; 3, 4 and 5 were built once with the same code (2026-11-18, the first form of
  the case, its table kept in the row's scratch) and left out for the case's time, 3 and 4 naming
  aerosols near today's 2 and 3 (0.094 against 0.108, 0.214 against 0.217).

What the test holds, for both blocks: the table's integral within 3 % of the march's (measured:
under 1 %); the harmonics' level value within 0.96 to 1.08 of the table's (0.98 to 1.03 from 30° up,
5 to 7 % over at 10°, where the sky is a bright ring at the horizon); the beam within 0.6 to 1.25 of
Bird and Perez's everywhere and within a tenth from 30° up at an aerosol to 0.25; the global over
diffuse within 0.85 to 1.22 of Bird and Perez's (0.87 to 1.20); and, of the published models
alone, Ineichen and Perez's global over diffuse within 0.95 to 1.25 of Bird and Hulstrom's at each
Linke turbidity's aerosol, which guards the transcription of both. What it does not hold, because the engine does not meet it, is the
turbidity's name: that is printed (`Linke`) and is this page's finding.

**What is not level** (the same run; π times the ambient at a normal, klux, over the default
terrain's sand):

| T | sun | sky on the level | a wall's normal | facing down | the mean over every normal | the ground's share of the wall's, of the mean |
|---|---|---|---|---|---|---|
| 2 | 50° | 17.4 | 40.5 | 63.7 | 41.2 | 0.79, 0.77 |
| 2 | 70° | 19.5 | 49.3 | 80.5 | 49.7 | 0.81, 0.81 |
| 2.5 | 50° | 21.0 | 41.0 | 63.0 | 42.3 | 0.77, 0.74 |
| 2.5 | 70° | 23.9 | 50.1 | 80.0 | 50.9 | 0.80, 0.78 |

A wall in shade takes twice the light of the level ground's shadow beside it, and four fifths of it
is the sand's. That is G218's bark (0.48 to 0.53 of white measured; the mirror's wall over the
metered light is 0.44 to 0.46) and its needles (a sphere of normals lit from both sides: 1.0 to
1.07).

## Where the gap is

**1. Open level ground.** At a turbidity of 2.5 and a high sun the shadow is 21 to 24 % of the
sunlit ground (bounce off; 17 to 18 % with it on beside G218's trees).
1. *The turbidity's scale*: all of the distance from what the scene's number names (11 to 12 %) to
   what is drawn. At the schema's default of 2 nothing is wrong: 17 to 19 %, a clear day's.
2. *The sky model's own balance*: a global over diffuse 0 to 13 % under Bird and Perez's at a high
   sun at the same aerosol, inside the two published models' own disagreement.
3. *How the resolve applies the sky*: the harmonics give a level normal the integral to 3 %. There
   is no floor. Nothing hides the sky with the bounce off, which on open ground is right.
4. *The display*: 3 % on this sand (the shoulder takes the sunlit side's red and green down), none
   on a ground under 0.6 of white.
5. *The bounce*: darkens the shadow, by what the trees hide.

**2. A needle inside a conifer crown.** Bounce off, a shaded needle returns 1.0 to 1.07 per unit
albedo (G218), the sun adding a third.
1. *The ambient's lower half*: three quarters to four fifths of a shaded needle's light is the
   ground, taken as sand of luminance 0.71 under the full sun with nothing between: not the scene's
   ground (a forest floor in the same scene file would still be this sand), not the ground in the
   tree's own shadow, and not hidden by the boughs below. With the bounce on the cells carry what
   rays found instead, and the harmonics return wherever a read has no samples.
2. *No occlusion of either half with the bounce off*: `scene_ambient` returns the harmonics before
   it applies the crown's prior, so a tier without the cache has no crown shade at all; the
   literature's openness inside a spruce crown is 0.2 to 0.9 (G218's page). With the bounce on the
   prior takes 10 to 30 %.
3. *The scene it was measured in*: `tree-species` stands its trees on that sand, so even the traced
   bounce is lit from below by a ground three to five times a forest floor's albedo (recalled: litter
   and soil 0.1 to 0.2; not read). G218's evenness is partly this scene's and was not measured on a
   forest's ground.
4. *The sky's balance and the turbidity*: the sky is a fifth of a shaded needle's light, so a
   clearer air moves it 6 %.

**3. A wall inside a room with one door.** Not measured here; by the code.
1. *The exposure*: the meter is the sun on the level and the sky's dome at the eye's height, with
   no walls and no history ("No eye adaptation", renderer.md), so a room is exposed for the
   outdoors. A room lit through a door is two to three orders under the open ground, and a
   photograph exposed for the street shows it black; an eye indoors does not. This is the whole of
   "near black beside a bright outside" as far as the sky is concerned.
2. *With the bounce off* the wall takes the open sky's harmonics as if there were no room (41 to
   50 klux): too bright, and the opposite complaint.
3. *With the bounce on* the cache errs dark by its own rule (a hit whose cells are empty returns its
   own light and nothing of the bounces after it; `gi_update.slang`), which is R171's ground.
4. *The sky's balance*: nothing.

## The mend, designed

Nothing below was built: each changes pictures, the sky is one file with three readers, and this
leg had no GPU. In the order it would be built:

1. **The ambient's lower half is the scene's own ground** (`load_scene` in
   `systems/renderer/src/scene.cpp`; `SceneData::ground_albedo`). Today any terrain is the desert's
   first surface. Take instead the mean base colour of what the terrain is made of: the layered
   ground's layers weighted by their masks' coverage (the means `terrain_hit_material` already
   blends for the bounce), the desert's four surfaces by their share for a dune field, the neutral
   grey without a terrain. One vector at load; the sky's `local_albedo`, the stand-in's lower half
   and the volumetrics' ambient follow with no shader change. *Gains*: on a forest floor of a fifth
   the sand's albedo a shaded trunk or needle with the bounce off loses about three fifths of its
   light, and the cache's fallbacks stop leaking sand. *Pays*: nothing a frame. *Cases*: the
   lighting test that pins the sand (`flythrough_tests.cpp`), a scene with a layered ground, every
   golden over a terrain that is not sand. **Built by R185 (2026-11-18)** as designed, with the
   masked palette's shares read off the mask level rather than blended per hit
   ([renderer](../subsystems/renderer.md), "The ground's albedo is a scene value"); what it moved
   is in [the section below](#r185-the-lower-half-from-the-scenes-own-ground).
2. **The turbidity is the Linke turbidity it says** (`earth_atmosphere`, `domain/sky/src/earth.cpp`;
   `schemas/scene.schema`'s `Sky`). The aerosol at 550 nm becomes
   `max((T / 11.2 − δcda(2) − δw(2, water)) / 0.749, least)` over a stated water column (a new
   `Sky.water_cm`, 1.5), by Kasten's formula as above. So that no picture moves by itself, the
   schema's default goes from 2 to 3 and a scene written before the version keeps its air by
   `T' = 1.30 + 0.91 T`; then a scene that wants a clear day writes 2.5 to 3 and gets shadows a
   sixth to a ninth of the ground. The sky capability's test of the air, the `earth()` fixture of
   `sky_tests.cpp` and this case's `engine_aerosol_550` change in the same commit (the case's second
   block is already the new mapping and passes). *Gains*: the knob means what the literature's
   tables mean, and the clear end of it is reachable (today 1.0 is as clear as it goes and still
   draws a Linke 2.2). *Pays*: nothing a frame; the two air tables are rebuilt as today. *Until
   then*: a scene that wants a clear day writes 1.5 to 2, and [sky](../subsystems/sky.md#the-air)
   says what a number draws. **The cheaper half, which moves no picture**: keep the mapping and
   change the word. What the engine computes, the whole optical depth over the molecules' at one
   visible wavelength, is as recalled (not read for this row) the turbidity of Preetham, Shirley and
   Smits 1999, the scale graphics uses, and is self-consistent; only "Linke" in the schema's
   comment, `earth.h` and the sky page is wrong, and the labels beside it (2 very clear, 3 clear)
   are a Linke scale's. Saying "a visible-band turbidity; its Linke turbidity is 1.30 + 0.91 T" in
   those three places, and writing 2 where a test scene means a clear day, is the whole of it.
   Which half is taken is the owner's: the first makes the number mean what solar tables mean, the
   second leaves every scene as it draws.
3. **A crown's own shade without the cache** (`scene_ambient`, `material.slang`): apply the crown's
   prior to the harmonics before the early return, so the vertex tier and `renderer.gi.mode` off
   shade a crown's inside; and give the prior's lower mean the ground under the crown, which is in
   the crown's shadow at a high sun (the prior's own grid can hold the share of the sun that
   reaches the ground under each column). *Gains*: the tiers with no rays get the light and dark
   between boughs the literature asks for. *Pays*: the prior's lookup on a crown's pixels with the
   bounce off (it is paid already with it on; stage 8 measured it dearer than nothing on every
   masked mesh, so crowns only). *Cases*: the resolve's foliage cases, a pair on `tree-species`.
4. **The exposure where the eye is** (`sky_frame_main`; renderer.md, "Exposure"): meter what
   reaches the eye's surroundings rather than the open sky, by the cache's cells round the eye
   where the frame reads them, held by the rule's knee and slope and by a time constant, so a room
   is exposed as an eye sees it and the door is bright. A design row with its own measurement: what
   it does to the meter's determinism (today a function of the frame, to the bit) is the question.
5. **A forest floor under `tree-species`**, or a second scene: a crown is not judged over white
   sand again. Content, no engine change.

Not proposed: any change to `sky.slang` or its reference. Nothing in the scattering contradicts
its sources at the precision this page measured.

## R185: the lower half from the scene's own ground

Built 2026-11-18 as step 1 above (`load_scene`, `terrain_ground_albedo`; the rule is on the
[renderer page](../subsystems/renderer.md), "The ground's albedo is a scene value"). Two engine-view
binaries built in `msvc-release` from the base (the R182 merge, whose code is main 5d0634bb's) and
from the mend, the same hour, at fixed poses (`--raster rt --shadows rt`, 1920 × 1080, 12 frames, the
renderer's stand-in sun on the two tree scenes and the village's own sky at 15:00 on day 170 at 52° N).
Luminance is the capture's sRGB codes decoded to linear and weighted by Rec. 709, the display's
shoulder not inverted, the mean of a box (`<scratch>\b69\R185\lum.py`, `boxes.json`):

| scene, bounce | surface | before | after | after / before |
|---|---|---|---|---|
| `tree-species`, off | a shaded spruce trunk; its needle mass; the sunlit sand | 0.069; 0.128; 0.400 | the same bytes | 1 |
| `tree-species`, on | the same | 0.058; 0.102; 0.387 | the same bytes | 1 |
| `grove`, off | a shaded palm trunk; a young spruce's needles; the sand under the crowns | 0.081; 0.150; 0.226 | the same bytes | 1 |
| `grove`, on | the same | 0.061; 0.124; 0.189 | the same bytes | 1 |
| village, off | a gable in shade (two houses); the near left cottage's front, lit at a slant; the sunlit turf | 0.300, 0.252; 0.651; 0.127 | 0.105, 0.090; 0.511; 0.127 | 0.35, 0.36; 0.79; 1.00 |
| village, on | the same | 0.099, 0.067; 0.510; 0.127 | 0.093, 0.063; 0.507; 0.127 | 0.95, 0.95; 0.99; 1.00 |

- **Both tree scenes stand on the desert's sand**, a ground with no ridge or basin, so their lower
  half is the sand to the bit and their pictures did not move: every capture is the same file
  before and after. Their crowns still read over white sand, which is step 5's (content), not this
  row's.
- **The village stands on turf, soil, a verge, wear and a gravel lane** by its masks, and a wall in
  shade with the bounce off loses about two thirds of its light (0.35 of it): its ambient's lower
  half was sand of luminance 0.71 and is now the village's ground. That is the R182 page's
  prediction (four fifths of a shaded wall's light was the lower half; a ground a fifth as bright
  leaves 0.2 + 0.8 / 5 ≈ 0.36). The shade also takes the ground's green where it took the sand's
  cream. With the bounce on, the cache's rays already found the turf, and the walls move 5 %, which
  is what its fallbacks (a read with no samples, a miss below the horizon) took from the sand.
- **On the tier with no bounce at all** (the Titan Xp's: no ray queries, so no cache), the
  harmonics' lower half is all the indirect light a shaded surface gets, and the bounce-off rows are
  that tier's picture: the mend is the whole of the change there.
- The village's ground came to (0.122, 0.141, 0.066) linear, luminance 0.13, from the sand's
  (0.84, 0.69, 0.47), luminance 0.71 (the load's `scene loaded` line logs it at `--log info`).
- **The cost is nothing a frame**: the value is one vector the frame already carried. The grove path
  at 3840 × 2160, `--raster rt --shadows rt`, bounce on, 3,001 frames, flown before, after, before,
  after under one lease (session unlocked, the machine's other processes at 70 to 100 % of the CPU):
  GPU frame median 5.33, 5.48, 5.69, 5.52 ms and p95 5.99, 6.20, 6.44, 6.25 ms — the two sides
  interleave, and the grove's pictures are the same bytes.
- What is resident: nothing new beyond a float per palette layer (`TerrainDesc::mask_cover`); a mask
  tile's words and a decoded coverage map are held only while summed at load.

## What an imported asset gets, and a scene with none

Nothing changed in what is drawn: the row adds a CPU test and pages. Of the design, steps 1 and 3
key on what any scene states (its ground's materials, a mesh's crown record) and step 2 on the
scene's sky entry; a scene with no sky and no terrain draws what it drew through all of them.

## Not done

- No GPU case and no picture: the captures read are G218's, of main a1eae86f.
- The forest was not measured; what its floor's albedo is and what its scenes write for a
  turbidity were not read.
- Rooms were reasoned from the code, not measured.
- The sand's own albedo (0.84, 0.69, 0.47, luminance 0.71) was not held to a measured sand; as
  recalled a quartz dune is nearer 0.35 to 0.45, and at 0.71 the sunlit desert sits on the display's
  shoulder.
- The published numbers are two implementations' formulas evaluated here; no paper's table was
  read, and the Perez bins make the Bird column step (its sky at 70° under its sky at 50° at a
  turbidity of 3).
