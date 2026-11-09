# The shore: where a body's level meets the ground (S97, S94)

Roadmap S97 (the owner, 2026-11-06: "the better water boundary for bodies of water"). The design and the code are [water](../subsystems/water.md), "A body that fills" and "The shore"; this page is the grounding for the numbers the schema pins and the rules the resolve follows, and where the research and the budgets were traded.

## What a lake's edge is

A still body's edge is a **contour**: the line where the ground's height equals the water's level. Nothing about it is a polygon an author draws; a hollow filled to a level has exactly the outline its contour gives, which is why S97 floods the ground from a seed rather than asking for a shape. Two things lie at that contour on a real shore and are what the eye reads as "water meets ground":

1. **A band of wet ground above it.** Sand and gravel at a water body's edge are wet above the still level for two reasons: the **capillary fringe** — water drawn up the pores above the free surface — and the **swash**, where each wave runs up the beach and back.
2. **A line of foam on the water near it**, where small waves break and run up, moving up and back with the waves.

## The wet band's height: the capillary fringe and a lake's swash

**Capillary rise** above a free water surface depends on grain size. The textbook ranges (Silin-Bekchurin 1958, as tabulated in groundwater hydraulics courses, e.g. NPTEL's *Module 1: Introduction*): coarse sand 2–5 cm, sand 12–35 cm, fine sand 35–70 cm, silt 70–150 cm; laboratory columns of sand measured about 13.5 cm over five tests (GSA North-Central abstracts, 2012 and 2013, which question the larger textbook values for fine material). The fringe's top is not a sharp line: saturation falls from near full at the water table to the residual over the fringe, so the ground dries upwards.

**A lake's swash** is small: a lake's waves are a few centimetres to a decimetre high with periods of a second or two. Stockdon et al. (2006) give the 2% exceedance run-up on a natural beach as R₂ = 1.1 (0.35 β_f (H₀L₀)^½ + ½ [H₀L₀ (0.563 β_f² + 0.004)]^½), with H₀ the deep-water wave height, L₀ = g T² / 2π its wavelength and β_f the foreshore slope. It was fit to ocean beaches; for a lake's H₀ = 0.1 m, T = 1.5 s (L₀ = 3.5 m) on a foreshore of 1:10 it gives **R₂ ≈ 5.5 cm vertical**, about 0.55 m along the slope.

**Pinned: `wet_m` = 0.3 m** (default) — the top of sand's capillary range, with the swash's few centimetres inside it. A gravel shore wants a few centimetres; fine sand or silt up to 0.7 m; the field is the body's.

## How dark wet ground is: Lekner and Dorf

Lekner and Dorf (1988, after Ångström 1925): a water film over a dry albedo *a* returns (1 − r_e)(1 − r_i) a / (1 − r_i a) of the light, with water's diffuse external reflectance r_e = 0.066 and internal r_i = 1 − (1 − r_e)/n² = 0.4744 at n = 1.333; the factor is 0.4909 / (1 − 0.4744 a). It is the same relation the terrain's beach layer already uses (`terrain_material.slang`, `terrain_wet_darkening`), and the wet band takes the same two constants (`water.slang`, `k_water_wet_through`, `k_water_wet_internal`) on the pixel's own albedo: sand of albedo 0.4 is darkened to 0.61 of itself, a dark soil of 0.15 to 0.53. Saturation eases from full at the waterline to none at `wet_m` by a smoothstep, the fringe drying upwards.

**Where the research and the budget were traded.** The band is applied **after the lighting**, as a multiplier on the shaded colour, in the resolve's water modules: the terrain's material shader belongs to another row tonight (R136), and a pre-lighting change would have needed its sampling. The limit is plain: a wet surface is smoother and shines (Lekner and Dorf's film is specular too), and a post-multiplier darkens the sun's highlight on the band instead of sharpening it. The roughness is not changed. (Superseded 2026-11-08 by S99: the band is a saturation the ground's material takes before the lighting; ["Wet ground"](#wet-ground), below.)

## The foam line: what it is drawn from

**Reflectance.** Fresh, dense foam reflects about 55% in the visible (Koepke 1984, *Applied Optics* 23, 1816–1824: a fresh whitecap's reflectance, whose effective 22% is that times the share of a whitecap's life it is fresh); whole whitecaps measured since average about 40% across the visible (Dierssen 2019's review in *Frontiers in Earth Science*, of the measurements since Whitlock, Bartlett and Gurganus 1982, *Geophysical Research Letters* 9, 719–722, whose visible numbers it flags as uncertain). A swash's foam line is fresh foam. Pinned: **`k_water_foam_albedo` = 0.55** (0.4 drew the line darker than the wet sand under it on the basin, a grey-blue thread, 2026-11-07), lit as a level Lambertian surface by the sun as it reaches the water (its shadow included) and the sky's light.

**Where.** The foam at a lake's edge lives in the swash zone, whose width along the slope is the run-up over the foreshore slope: Stockdon's 5.5 cm on 1:10 is 0.55 m. Pinned: **`foam_m` = 0.6 m** (default). A sea's (S93's open bodies) is larger — a 1 m wave of 8 s on 1:20 runs up about 0.55 m vertical, 11 m along the slope — and the field is the body's.

**How.** A procedural mask, not a simulation: a line a tenth of `foam_m` wide stands between 0.3 and 1 of `foam_m` from the waterline at the largest wave's phase at the pixel (so it runs up and back once a wave period), a thinner trail of what the backwash leaves lies between it and the waterline, and value noise at a quarter of `foam_m` breaks both. Its strength is the waves' amplitude over 2 cm (a mirror has none) and it fades to nothing at `foam_m`. A line narrower than the pixel's footprint is widened to it and dimmed by the same share, so a far shore keeps its mean and does not alias.

## The waterline per pixel

The distance from the waterline that the shore fade and the foam are drawn by is **the depth over the slope of the depth map**: d / |∇d|, with the slope the larger of the one-sided differences either way, so at the waterline, where the map is clamped at zero on the bank, the water's side gives it. On a bank of constant slope it is exact; on a bowl it overstates the distance by the curvature over the fade's few metres (on the S97 basin, 1:3.5 at the waterline, a few centimetres). Four more reads of the map a water pixel.

## The swash

Roadmap S94 (the owner, 2026-11-07, with eight frames of a real beach: a sheet of foamy water runs up the sand to a ragged front and slides back, leaving the sand dark and glossy up to the highest line it reached, and the dark band dries from the top down until the next sheet, not always as far). The code is [water](../subsystems/water.md#the-swash); this section is the models, their constants and the trades.

### The wave a wind raises

A closed body has no swell: its waves are the wind's, fetch-limited. JONSWAP (Hasselmann et al. 1973) gives the peak frequency ω_p = 22 (g² / U₁₀F)^(1/3) and the Phillips constant α = 0.076 (U₁₀² / Fg)^0.22, the laws [the ocean page](ocean-surface.md#the-published-models) already cites. The swash takes **the peak period T = 2π/ω_p** and **the spectral significant height H_m0 = 4√m₀**, m₀ the spectrum integrated over [0.3, 6] ω_p (`gfx::swash_wave`); the test holds H_m0 within a fifth of the fetch-limited growth law of the Coastal Engineering Manual (II-2), g H_m0 / U² = 1.6 × 10⁻³ (gF/U²)^½, and T within 1% of the law's own. **5 m/s over 2 km: H = 0.131 m, T = 1.34 s** (the growth law 0.114 m); **10 m/s over 20 km: H = 0.87 m, T = 3.64 s** (the law 0.72 m): the spectrum's variance runs about a seventh to a fifth above the law's, inside the scatter of the fetch-limited data. A closed body's fetch is the smaller of `fetch_km` and its own longest reach (pinned: the diagonal of its depth map's square, which bounds every straight line over its water); an open body's is `fetch_km`.

### The run-up

Stockdon et al. (2006, eq. 19), already used above for the foam's reach: R₂ = 1.1 (0.35 β (H₀L₀)^½ + ½ [H₀L₀ (0.563 β² + 0.004)]^½), L₀ = gT²/2π. Worked by hand and pinned in `swash_tests.cpp`: a lake's 0.1 m, 1.5 s wave on 1:10 runs up **5.48 cm**; an ocean's 1 m, 10 s wave on a 0.08 foreshore **0.984 m**. β is the foreshore slope at the waterline, from the signed depth map (the mean slope across every texel pair the waterline crosses, kept to [0.01, 0.5]). R₂ is the 2% exceedance, so it is taken as the **largest** wave of a group's reach, not the mean's.

### The front's motion

**Ballistic swash.** After a bore collapses at the shoreline the front runs up the beach face as a projectile decelerated by gravity along the slope and returns accelerating: the shoreline's position is x_s(t) = u₀t − ½ g sin β t² (Shen and Meyer 1963; Hughes 1992 on a sandy beach; Baldock and Holmes 1999), a parabola in time. Friction and infiltration make the backwash longer than the uprush on real beaches. Pinned: **the uprush is the first 0.4 of the period** (`k_swash_uprush`; a choice inside the asymmetry the field measurements show), both halves parabolic, so in the wave's own phase u the profile is 4s(1 − s) with s = u/2q on the uprush and ½ + (u − q)/2(1 − q) after; the front runs from **0.2 R below the still level** (`k_swash_rundown`, the backwash's rundown) up to the wave's reach and back. Every wave starts and ends at the rundown, so the profile is continuous whatever two neighbouring waves reach. The swash period is the wave's: on a reflective lake beach the swash is saturated by the incident wave, one sheet a wave.

**Groups.** Waves come in groups (every few waves larger): pinned **seven waves a group** (`k_swash_group`), the reach of wave j of the group R · (0.55 + 0.45 (½ − ½ cos 2π(j + ½)/7)), 0.57 R at the group's ends and R in its middle. Periodic in the wave's index, which is what makes the last crossing a closed form (below). **Along the shore** the reach is jittered by value noise of the body-local point into [0.7, 1.3] and the phase drifts by up to 1.5 waves over the larger of 4 m and three swash widths, so the front is a ragged curve and never a straight line.

**On the water's clock.** The resolve's water clock wraps every 1,024 s, so T is rounded to make a whole number of groups fit (2.004 s for a 2 s wave: 73 groups), and the profile has no jump at the wrap (`swash_tests.cpp` holds it periodic to 10⁻⁹).

### When the front last passed (the derivation)

For ground at height h above the still level and the place's phase c = t/T + φ, wave k = ⌊c⌋ and u = c − k: wave k's front is at or above h where z_lo + (A_k − z_lo) · 4s(1 − s) ≥ h, i.e. 4s(1 − s) ≥ c_k = (h − z_lo)/(A_k − z_lo). If c_k > 1 the wave never reaches h. Otherwise with r = √(1 − c_k), s ∈ [(1 − r)/2, (1 + r)/2], and mapped back through the two halves, **u ∈ [u₁, u₂] = [(1 − r) q, q + r (1 − q)]**. The front last stood at h at the end of the most recent such interval:

- if wave k has reached h (u ≥ u₁): under the sheet while u ≤ u₂ (age 0), else **age = (u − u₂) T**;
- else the most recent earlier wave k − j (j ≥ 1) with c_{k−j} ≤ 1: **age = (u + j − u₂(k − j)) T**.

The reach repeats every seven waves, so waves k − 1 … k − 7 hold every reach there is: the search is at most seven, and a height none of them reaches was never reached (age ∞). The wetness above the fringe is then exp(−age / τ). Nothing is stored and every frame is a function of the clock. **Checked** (`swash_tests.cpp`): at a thousand random (t, h) the closed form against a brute-force scan of the profile backwards in steps of a 4,000th of a period over two groups, every one within a step, with some under the sheet, most drying and some never reached.

### How long the sand stays wet

The film a sheet leaves on sand drains into it fast: Turner and Masselink (1998) measured vertical flows of the order of 10⁻³ m/s through a beach face under individual swashes, so a few millimetres of film go in **seconds** — the gloss (pinned `k_swash_film_s` = 3 s). The sand under it stays near saturation much longer, draining to the damp of the capillary fringe over **tens of seconds** on sand: pinned **`drain_s` = 20 s** (a choice within the brief's "tens of seconds"; gravel drains in a few, silt and clay hold water for minutes, and the field is the body's). The darkening is Lekner and Dorf's as above, at the wetness the clock gives.

**The fringe under a swash is damp, not filmed.** Version 4's band darkened by the full factor at the waterline because it stood for the swash's few centimetres too. With the swash drawn, the band above the highest sheet is the capillary fringe alone, whose surface is tension-saturated grains with no free film: pinned **half the saturated factor** (`k_swash_fringe`, a choice: Lekner and Dorf's relation is for a film), smoothstepping out at `wet_m`. Without that, a 0.3 m fringe would cover a lake's few-centimetre swash at full darkness and the drying the owner described would not show. A body with no wind and no tide keeps the full band.

### Tides

**M2**, the principal lunar semidiurnal constituent, the largest on most coasts: period 12.4206012 h (its speed 28.984° an hour; Doodson 1921). An open body's level is level + tide_m sin(2π t / T_M2) on the world clock (the phase computed in double on the CPU). One constituent, so no spring–neap cycle (S2 would add it) and no diurnal inequality; a scene that wants a particular stage of the tide at its start sets its world time.

### Where the research and the budget were traded

The mesh is not extended above the still level (a flat surface there is behind the ground and never drawn); the sheet over the bank is drawn on the ground's pixels as saturated sand with the water's Fresnel reflection of the sky and the foam, not as a volume: a few centimetres of sheet are what the eye sees as a dark glossy film, and a tide of metres over the bank is the same with no depth colour — the limit is on [water](../subsystems/water.md#the-swash). The film's gloss is a reflection added after the lighting, not a lowered roughness of the terrain material (S99). The per-pixel cost is a search of up to eight waves (a square root each) on the pixels of ground in the swash band and on the water within its band of the shore.
## Wet ground

Roadmap S99 (2026-11-08): the darkening, the lowered roughness and the water film, from the saturation, before the lighting ([terrain](../subsystems/terrain.md#wet-ground), [water](../subsystems/water.md#the-swash)). What the owner's eight frames of a real beach show, and what each piece of the model answers: nearest the water a saturated sand that reflects the sky and the light like a thin film (the film); above it a band darker than dry sand but matte, its upper edge ragged and receding (the darkening, with the roughness barely moved, and S94's clock); the dry sand pale and rough; the film-to-damp boundary sharp, the damp-to-dry one diffuse.

**The darkening: Lekner and Dorf (1988).** A rough surface wetted keeps a water layer over its grains; light that leaves a grain is partly reflected back into the ground at the water's upper surface, totally beyond the critical angle, and the grain absorbs some of it again. Averaged over a diffuse field the wet albedo is a′ = (1 − r_e)(1 − r_i) a / (1 − r_i a), r_e = 0.066 water's external diffuse reflectance and r_i = 1 − (1 − r_e)/n² = 0.4744 at n = 1.333 (the section above has the factor). Pinned as `wet_ground.slang`'s two constants, the same as R99's beach and S97's band; the double reference computes them from r_e and n.

**Its weight: porosity.** Lekner and Dorf's picture needs pores to hold the water; Jensen, Legakis and Dorsey (1999, "Rendering wet materials", EGWR) separate water *in* a material (darker by the same internal reflection, which they simulate) from water *on* it (a dielectric layer, glossier, little darker). Pinned: the darkening's weight is *s* × porosity / 0.3, at most 1 — 0.3 the low end of a sand's porosity (0.3–0.5), so any sand or soil darkens fully at saturation and a rock of porosity 0.05 a sixth as much (it mostly films). `TerrainLayer.porosity` (default 0.5; the v35 pack's meadow 0.55) is the input; it was already read by Lagarde's rain rule.

**The roughness: measured and chosen.** Zhang and Voss (2006, *Applied Optics* 45(34), bidirectional reflectance of dry, wet and submerged particulate layers) measured a strong specular peak on wet layers at large illumination zenith angles that the dry layers lack, and darkening consistent with Lekner and Dorf; Zhang, Voss and Lewis (2002) found the peak "not obvious" on a wetted powder whose morphology stayed the dry one's — the peak is the film's, not the wet grains'. Nolet et al. (2014) fit beach sand's spectral reflectance against surface moisture up to about 24% volumetric with a physical optical model. **No measured roughness of damp sand as a function of saturation was found**, so the curve is stated: the damp substrate's roughness falls to 0.7 of its own at *s* = 1 (gentler than Lagarde 2013's porous rule, which takes up to 0.4 off, because the owner's frames show the damp band matte), and the film takes it to 0.05 by the film's weight. The film's weight is the larger of S94's film term (exp(−age/3 s), Turner and Masselink 1998's infiltration through a millimetre film) and the free water standing once the pores are past 85% full, ((s − 0.85)/0.15)², so the film-to-damp boundary is as sharp as the saturation and the damp band never shines. The film's Fresnel is water's, F0 0.0201 (n = 1.333), for the sky reflected along the mirrored normal; the BSDF's direct lobe stays at its single dielectric F0 0.04 (Lagarde, who lerps F0 towards water's 0.02 for a flooded surface, had a second F0 to lerp; `brdf.slang` does not, and the path tracer shares it).

**The capillary fringe above a still waterline, by grain** (the channels' banks). The ranges above (Silin-Bekchurin) and the laboratory columns of Lane and Washburn (1946) as Lohman (1972, USGS Professional Paper 708) and Fetter tabulate them, at a porosity near 41% after 72 days: fine gravel 2.5 cm, coarse sand 13.5, medium sand 24.6, fine sand 42.8, silt 105.5 (`gfx::k_capillary_fringe_m`). Each lies inside the Silin-Bekchurin range for its class and the coarse sand's matches the GSA columns' 13.5 cm; **the USGS paper itself refused the fetch (HTTP 403), so the five values were checked against the ranges and the one column, not against Lohman's page**, said here so the next reader checks them there. A channel's description names no bank material, so its banks take `k_bank_default`, fine sand's 0.428 m — a loam's rise lies between fine sand's and silt's, and the lower bound keeps a forest stream's damp strip to tens of centimetres of height. The saturation falls over the fringe by a smoothstep (1 at the water, 0 at the top, steepest midway): the shape of van Genuchten's retention curve, not its parameters, which would need each soil's (Wösten et al. 2001 tabulate them; FAO's Irrigation and Drainage Paper 56 Annex 5 shows one silt's profile over a metre).

**The spray band** over a rapid (0.25 m) and a fall (half the drop, at most 3 m) is a stated choice: no measured reach of a small fall's spray onto its banks was found in the time spent.

**Bared ground** (the swash's backwash, a falling tide) is now the ground's own material at the clock's saturation (its terrain material, its plain albedo over porosity 0.5, or the low tier's shallow tint), not a fixed sand of albedo 0.35.
**A bank is damp, never flooded** (2026-11-08, after the first v35 captures). The fringe gives saturation 1 at and below the stage, and the film's rule then laid a standing film wherever a bank's ground lay at or below the channel's surface: on v35's stream that was the whole floodplain beside it out to the reach's 26 m, every pebble of the gravel normal map glinting in the sun. A capillary fringe holds water in the pores, not on them, so a bank's saturation is now held at 0.85, the film's threshold (the swash keeps its film, and the channel's surface is water), and a point's height is taken as at least its run from the wetted edge over 25, the reach's slope, so the damp falls to nothing at the reach's edge on flat ground instead of stopping there.

### What it costs, and what it looks like

**Captures** (msvc-release, `engine-view --offscreen`, 1920 × 1080, the v35 copy with the engine-only flags of the owner's run, `--raster hw --shadows rt`, 15:00, the path `s99-wet` holding shore-open, east-shore, stream and lake-bank 2 s each, a capture at the end of each; before is main 86b543cb's installed build, after is S99's; the basin is S94's sequence, eight frames a second apart, msvc-debug after and S94's own frames and 86b543cb's before; under the GPU lock). **The basin**: the band above the waterline is darker than the dry sand and matte, the sheet's film mirror-dark, and where the bank faces the camera with the sun behind it the film carries the sun's glint, sharp-edged at the fringe's top (the film's 0.05 roughness), with no halo past its edge; nothing under the sheet is dry. **Shore-open, east-shore, lake-bank**: unchanged where the ground is dry; the waterline's damp strip as dark as S94's or a little lighter (the darkening now weighted by porosity), the film a thin brighter line at the water's edge. **The stream**, first after: the whole floodplain beside the channel glittering with a standing film out to the reach (the defect above); the fix is a shader change with no capture after it (the GPU lock's queue that night), so its picture is the next run's to confirm.

**Cost**: the resolve's median `gpu_ms.resolve` per held view (frames 30–119 of each 2 s hold, three repeats, n = 270), RTX 5090, 2026-11-08, `--wait-quiet 120`, others' CPU 12–22% and the GPU otherwise idle at each run's start and end (`machine_state`), so close to quiet but not certified so; measured before the bank fix, which only removes work (the walk now ends at 0.85).

| View | 1920 × 1080 before → after | 3840 × 2160 before → after |
|---|---|---|
| shore-open | 1.793 → 2.145 ms (+0.352) | 6.565 → 7.454 ms (+0.889) |
| east-shore | 1.905 → 2.252 ms (+0.347) | 5.494 → 6.134 ms (+0.640) |
| stream | 2.765 → 4.066 ms (+1.301) | 8.638 → 12.400 ms (+3.762) |
| lake-bank | 2.374 → 2.896 ms (+0.522) | 8.221 → 9.782 ms (+1.561) |

**The banks after the cut** (2026-11-09, 904a6d48: d90f9780's damp-not-flooded banks, the reach ending at 25 `wet_m`, the walk reading a record's square first), **paired in one binary**: the worktree's msvc-release over the same path under `renderer.water.banks=cycle`, the banks walked on frames whose (f / 8) is even and skipped on the others, medians of each set (n = 126–144), RTX 5090, `--wait-quiet 120`, others' CPU 12–15 %, the GPU otherwise idle (the harness's `machine_state`). What the banks add to the resolve:

| View | 1920 × 1080 banks / none (added) | 3840 × 2160 banks / none (added) |
|---|---|---|
| shore-open | 2.128 / 2.022 ms (+0.105) | 7.218 / 6.890 ms (+0.328) |
| east-shore | 2.191 / 2.134 ms (+0.057) | 6.302 / 6.108 ms (+0.194) |
| stream | 3.291 / 2.855 ms (+0.436) | 10.256 / 8.640 ms (+1.616) |
| lake-bank | 2.823 / 2.641 ms (+0.182) | 9.829 / 9.183 ms (+0.646) |

The stream's 1080p is now of the order of the open shores' whole S99 cost (+0.35 ms); its 4K, +1.6 ms, is still over the +1 ms bar, because at 4K every ground pixel of the frame's lower half lies inside several overlapping reaches and still walks to their leaves: the per-tile bake (one texel a pixel) is what removes that, and is not built yet (roadmap S99). The table above it was paired against the installed binary, which the bench page now says not to do; this one is not.

**The stream after the fix** (2026-11-09, `c2\` in S99's scratch: the installed main eeb8077c and the worktree's msvc-release, `--raster hw --shadows rt`, 1920 × 1080, the v35 copy): the floodplain beside the channel is darker than main's and matte out over the reach, no standing film and no glitter on the plain; the bed and the water are as they were; the far plain beyond the reach is main's.

**How the band is bounded**: a pixel that is not ground pays nothing; a ground pixel tests each body's record's square (and the swash's heights) and walks the banks' tree, whose nodes skip a subtree whose square or heights miss it. Where the ground is far from every channel that is the bodies' tests and the tree's top; at the stream every ground pixel is inside some stretch's reach (25 × 0.428 + 1 = 11.7 m either side), the reaches overlap, and the walk reaches many leaves: 1.3 ms at 1080p and 3.8 ms at 4K is the largest cost S99 adds and the first to cut (a tighter reach, or the banks' saturation baked into the ground's tiles at load, since a channel's level is static).

## What the fill costs at load

Measured on MSVC debug (2026-11-07, `fill_tests.cpp`'s messages): the paraboloid of 10 m radius at 0.667 m — 698 nodes inside, 118 edge points, 816 vertices, 1,512 triangles — flooded, traced and meshed in **1.8 ms** (its traced outline within 6 mm of the circle). The v31 lake's numbers are in [water](../subsystems/water.md#a-body-that-fills).

## Sources

- Jensen, H. W., Legakis, J. and Dorsey, J. (1999). Rendering of wet materials. *Rendering Techniques '99* (EGWR), 273–282.
- Lagarde, S. (2013). Water drop 3b: physically based wet surfaces. Blog post, seblagarde.wordpress.com.
- Lane, K. S. and Washburn, D. E. (1946). Capillarity tests by capillarimeter and by soil filled tubes. *Proceedings of the Highway Research Board* 26, 460–473 (as tabulated by Lohman 1972).
- Lohman, S. W. (1972). Ground-water hydraulics. USGS Professional Paper 708.
- Nolet, C., Poortinga, A., Roosjen, P., Bartholomeus, H. and Ruessink, G. (2014). Measuring and modeling the effect of surface moisture on the spectral reflectance of coastal beach sand. *PLoS ONE* 9(11), e112151.
- Zhang, H. and Voss, K. J. (2006). Bidirectional reflectance study on dry, wet, and submerged particulate layers: effects of pore liquid refractive index and translucent particle concentrations. *Applied Optics* 45(34), 8753–8763.
- Zhang, H., Voss, K. J. and Lewis, G. (2002). Bidirectional reflectance of dry and wet samples (Ocean Optics XVI).
- Wösten, J. H. M. et al. (2001). Water retention and conductivity in the soils of Europe (as used by FAO Irrigation and Drainage Paper 56, Annex 5).

- Ångström, A. (1925). The albedo of various surfaces of ground. *Geografiska Annaler* 7, 323–342.
- Koepke, P. (1984). Effective reflectance of oceanic whitecaps. *Applied Optics* 23(11), 1816–1824.
- Lekner, J. and Dorf, M. C. (1988). Why some things are darker when wet. *Applied Optics* 27(7), 1278–1280.
- Stockdon, H. F., Holman, R. A., Howd, P. A. and Sallenger, A. H. (2006). Empirical parameterization of setup, swash, and runup. *Coastal Engineering* 53, 573–588.
- Baldock, T. E. and Holmes, P. (1999). Simulation and prediction of swash oscillations on a steep beach. *Coastal Engineering* 36, 219–242.
- Doodson, A. T. (1921). The harmonic development of the tide-generating potential. *Proceedings of the Royal Society A* 100, 305–329.
- Hasselmann, K. et al. (1973). Measurements of wind-wave growth and swell decay during the Joint North Sea Wave Project (JONSWAP). *Deutsche Hydrographische Zeitschrift* A8(12).
- Hughes, M. G. (1992). Application of a non-linear shallow water theory to swash following bore collapse on a sandy beach. *Journal of Coastal Research* 8(3), 562–578.
- Shen, M. C. and Meyer, R. E. (1963). Climb of a bore on a beach. Part 3: Run-up. *Journal of Fluid Mechanics* 16, 113–125.
- Turner, I. L. and Masselink, G. (1998). Swash infiltration-exfiltration and sediment transport. *Journal of Geophysical Research* 103(C13), 30813–30824.
- US Army Corps of Engineers (2002). *Coastal Engineering Manual*, EM 1110-2-1100, Part II, chapter 2 (fetch-limited wave growth).
- Whitlock, C. H., Bartlett, D. S. and Gurganus, E. A. (1982). Sea foam reflectance and influence on optimum wavelength for remote sensing of ocean aerosols. *Geophysical Research Letters* 9(6), 719–722.
- Dierssen, H. M. (2019). Hyperspectral measurements, parameterizations, and atmospheric correction of whitecaps and foam from visible to shortwave infrared for ocean color remote sensing. *Frontiers in Earth Science* 7:14.
- Capillary rise by grain size (Silin-Bekchurin 1958, tabulated): NPTEL, *Groundwater hydrology*, Module 1 lecture 1; laboratory columns: GSA North-Central Section abstracts 2012 (paper 202803) and 2013 (paper 218691).
