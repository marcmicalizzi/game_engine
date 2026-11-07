# The shore: where a body's level meets the ground (S97)

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

**Where the research and the budget were traded.** The band is applied **after the lighting**, as a multiplier on the shaded colour, in the resolve's water modules: the terrain's material shader belongs to another row tonight (R136), and a pre-lighting change would have needed its sampling. The limit is plain: a wet surface is smoother and shines (Lekner and Dorf's film is specular too), and a post-multiplier darkens the sun's highlight on the band instead of sharpening it. The roughness is not changed.

## The foam line: what it is drawn from

**Reflectance.** Fresh, dense foam reflects about 55% in the visible (Koepke 1984, *Applied Optics* 23, 1816–1824: a fresh whitecap's reflectance, whose effective 22% is that times the share of a whitecap's life it is fresh); whole whitecaps measured since average about 40% across the visible (Dierssen 2019's review in *Frontiers in Earth Science*, of the measurements since Whitlock, Bartlett and Gurganus 1982, *Geophysical Research Letters* 9, 719–722, whose visible numbers it flags as uncertain). A swash's foam line is fresh foam. Pinned: **`k_water_foam_albedo` = 0.55** (0.4 drew the line darker than the wet sand under it on the basin, a grey-blue thread, 2026-11-07), lit as a level Lambertian surface by the sun as it reaches the water (its shadow included) and the sky's light.

**Where.** The foam at a lake's edge lives in the swash zone, whose width along the slope is the run-up over the foreshore slope: Stockdon's 5.5 cm on 1:10 is 0.55 m. Pinned: **`foam_m` = 0.6 m** (default). A sea's (S93's open bodies) is larger — a 1 m wave of 8 s on 1:20 runs up about 0.55 m vertical, 11 m along the slope — and the field is the body's.

**How.** A procedural mask, not a simulation: a line a tenth of `foam_m` wide stands between 0.3 and 1 of `foam_m` from the waterline at the largest wave's phase at the pixel (so it runs up and back once a wave period), a thinner trail of what the backwash leaves lies between it and the waterline, and value noise at a quarter of `foam_m` breaks both. Its strength is the waves' amplitude over 2 cm (a mirror has none) and it fades to nothing at `foam_m`. A line narrower than the pixel's footprint is widened to it and dimmed by the same share, so a far shore keeps its mean and does not alias.

## The waterline per pixel

The distance from the waterline that the shore fade and the foam are drawn by is **the depth over the slope of the depth map**: d / |∇d|, with the slope the larger of the one-sided differences either way, so at the waterline, where the map is clamped at zero on the bank, the water's side gives it. On a bank of constant slope it is exact; on a bowl it overstates the distance by the curvature over the fade's few metres (on the S97 basin, 1:3.5 at the waterline, a few centimetres). Four more reads of the map a water pixel.

## What the fill costs at load

Measured on MSVC debug (2026-11-07, `fill_tests.cpp`'s messages): the paraboloid of 10 m radius at 0.667 m — 698 nodes inside, 118 edge points, 816 vertices, 1,512 triangles — flooded, traced and meshed in **1.8 ms** (its traced outline within 6 mm of the circle). The v31 lake's numbers are in [water](../subsystems/water.md#a-body-that-fills).

## Sources

- Ångström, A. (1925). The albedo of various surfaces of ground. *Geografiska Annaler* 7, 323–342.
- Koepke, P. (1984). Effective reflectance of oceanic whitecaps. *Applied Optics* 23(11), 1816–1824.
- Lekner, J. and Dorf, M. C. (1988). Why some things are darker when wet. *Applied Optics* 27(7), 1278–1280.
- Stockdon, H. F., Holman, R. A., Howd, P. A. and Sallenger, A. H. (2006). Empirical parameterization of setup, swash, and runup. *Coastal Engineering* 53, 573–588.
- Whitlock, C. H., Bartlett, D. S. and Gurganus, E. A. (1982). Sea foam reflectance and influence on optimum wavelength for remote sensing of ocean aerosols. *Geophysical Research Letters* 9(6), 719–722.
- Dierssen, H. M. (2019). Hyperspectral measurements, parameterizations, and atmospheric correction of whitecaps and foam from visible to shortwave infrared for ocean color remote sensing. *Frontiers in Earth Science* 7:14.
- Capillary rise by grain size (Silin-Bekchurin 1958, tabulated): NPTEL, *Groundwater hydrology*, Module 1 lecture 1; laboratory columns: GSA North-Central Section abstracts 2012 (paper 202803) and 2013 (paper 218691).
