# Sand and snow as substrates of the layered ground (R99, 2026-11-02)

**Question.** The layered ground ([renderer](../subsystems/renderer.md#ground-materials-by-place),
W72) draws every substrate as a colour, a roughness and its maps. Sand from eye height and snow on
a mountain need more than a colour: what does each substrate do to light that its maps do not, what
does the literature give for it, and how much of it can the material stage carry inside the
budgets? This page states the facts the design started from, the research and the numbers pinned
from it, then what was built and what it measured. The rules that follow are in
[renderer](../subsystems/renderer.md#ground-materials-by-place) ("Sand and snow").

## The facts before the change

**W42's sand detail never reached a layered pixel.** It was exclusive twice over, so R99's row
text ("mutually exclusive") was right:

- the scene reader refused a terrain naming both `materials` and `detail`
  (`systems/renderer/src/scene.cpp`, `load_terrain` and the file reader: "terrain.materials and
  terrain.detail are alternative surface-detail paths; name one");
- and had both been allowed, `upload_materials` cleared `gfx::k_material_ground_detail` from the
  terrain root (`gpu_scene.cpp`, the root branch of the native terrain's entries), and
  `sample_terrain_material` returned `detail_mask = 0` (`terrain_material.slang`, its last lines),
  where `visibility_resolve.h` (`k_material_ground_detail`, around line 270) says the detail is
  weighted by "its sand share, which is the metallic-roughness map's alpha". The resolve's call
  (`ground_detail_shade(..., sampled.detail_mask, ...)` in `visibility_resolve.slang`, and the same
  in `path_trace.slang`) multiplies everything it does by that share, so a zero share is no detail.

The resolve is unchanged by R99 (it is R115's tonight): the share is the one input the detail
already takes, so a wind-sand layer composes W42 by setting it — the layers' blended share of wind
sand — and nothing in the resolve moves.

**What a layer record can carry.** A palette layer is a `gfx::ResolveMaterial` (112 bytes) whose
terrain fields `gfx::set_terrain_layer` packs into words no other reader uses for a layer:
`uv_scale` (the pitch), `uv_transform` (the means), `uv_offset` (weight, porosity, turn, height
mean). Two more are free on a layer and are what R99 uses, so **no record grows and no size table
changes**:

| Word | What a layer used it for before | R99 |
|---|---|---|
| `emissive.rgb` | nothing: upload zeroed it, and `sample_terrain_material` discards a layer's emission | the response's numbers: glints; the snow's grain and lobe, or the beach's wet darkening |
| `emissive.w` | the height map's factor (B through `metallic`) | unchanged |
| `occlusion_strength` | nothing: a layer's occlusion slot is W68's coverage, read without it | the snow's thickness |
| `flags` | `k_material_terrain_*` | three bits: `k_material_terrain_snow` (16384), `_sand` (32768), `_beach` (65536) |

The branch on the response is taken **once per layer**, on the record's `flags`, where the layer is
sampled; nothing branches per texel inside a loop.

**What the material stage cannot carry.** `sample_terrain_material` answers a point and its
footprint, never a light: the light's direction and the BSDF are the resolve's
(`brdf_direct(surface, light_dir, radiance)`), and `MaterialSample` holds albedo, roughness,
metallic, emission, the detail share and a filter flag. So a snow model's terms that depend on the
light — a wrap or translucency term, a forward lobe shaped otherwise than GGX's — are out of the
material's reach without a change to the resolve and `material.slang`. What the material can do,
and R99 does: the albedo, the specular lobe's weight through the metallic word (exactly, below),
the height blend, and glints as **facets in the shading normal and roughness**, which the resolve's
own GGX then lights for every light, shadowed as the pixel is.

## The research

### Snow's albedo is its grain's

Snow is white because ice barely absorbs in the visible and the grains scatter light many times
before it leaves: Warren (1982, *Optical properties of snow*, Rev. Geophys. 20(1)) and Wiscombe
and Warren (1980, *A model for the spectral albedo of snow. I: Pure snow*, J. Atmos. Sci. 37) find
the visible albedo of pure snow near 0.96–0.99 and nearly flat across the band, and the
near-infrared's falling steeply with grain size — which is why broadband albedo falls from about
0.9 for fresh fine snow to 0.6–0.7 for old coarse snow while snow stays white to the eye, and why
its shadows take the sky's colour (the albedo does not tint them). What lowers the visible albedo
of real old snow is mostly impurities (soot, dust), which an author gives a layer as its `tint`.

The asymptotic radiative transfer theory gives the albedo as a closed form of the grain (Kokhanovsky
and Zege 2004, *Scattering optics of snow*, Appl. Opt. 43(7); the form used here as written in
Kokhanovsky et al. 2018, *On the reflectance spectroscopy of snow*, The Cryosphere 12, 2371–2382):
the spherical (white-sky) albedo

    r_s(λ) = exp(−sqrt(α(λ) l)),   α = 4π χ(λ) / λ,   l = ξ d

with χ the imaginary part of ice's refractive index, d the optical grain diameter and l the
effective absorption length. ξ depends on the grains' shape, not their size: 13–20 (Kokhanovsky
2006, as cited in Kokhanovsky 2021, *The broadband albedo of snow*, Front. Environ. Sci. 9:757575,
which "assume[s] ξ = 16"); 2018's paper derives 11.38 from g = 0.75 and B = 1.6. **Pinned: ξ = 16.**
The spherical albedo is the one for diffuse light, which is what a Lambertian albedo stands for;
the plane albedo under a direct sun is r_s raised to 3(1 + 2μ₀)/7, a light-dependent term the
material cannot carry (above).

χ is Warren and Brandt (2008, *Optical constants of ice from the ultraviolet to the microwave: A
revised compilation*, J. Geophys. Res. 113, D14220), read from the compilation's own table: 6.890e-9
at 610 nm, 2.289e-9 at 550 nm, and 1.640e-10 at 465 nm (linear between its 460 nm 1.325e-10 and
470 nm 1.956e-10 rows). **A design choice: the channels are represented by 610, 550 and 465 nm**,
near the sRGB primaries' dominant wavelengths, rather than integrated over their spectra. The
absorption coefficients are then 0.14194, 0.05230 and 0.004433 per metre.

| Optical grain d | r_s red (610) | green (550) | blue (465) | |
|---|---|---|---|---|
| 0.05 mm | 0.9894 | 0.9936 | 0.9981 | new snow |
| 0.1 mm | 0.9850 | 0.9909 | 0.9973 | |
| **0.25 mm** | **0.9765** | **0.9856** | **0.9958** | **the default** |
| 0.5 mm | 0.9669 | 0.9798 | 0.9941 | |
| 1 mm | 0.9535 | 0.9715 | 0.9916 | settled |
| 3 mm | 0.9208 | 0.9511 | 0.9855 | old, coarse |

(ξ = 11.38 raises each by under 0.01.) **Pinned: `grain_mm` 0.25 by default**, the mean optical
diameter Kokhanovsky (2021) gives for two years of Greenland's EGP site ("0.1–0.4 mm for most of
cases", means 0.25–0.28), refused outside 0.03–5 mm. The coarser the grain the bluer the white:
red absorbed more, as the theory says and as deep snow's blue shows.

### The lobe: snow is near-Lambertian in the visible

Dumont et al. (2010, *High-accuracy measurements of snow Bidirectional Reflectance Distribution
Function at visible and NIR wavelengths – comparison with modelling results*, Atmos. Chem. Phys.
10, 2507–2520) find that where light penetrates deeply — the visible — the anisotropy factor is
nearly constant and decreases at grazing observation angles, while the strong forward peak belongs
to the wavelengths where ice absorbs (the near-infrared); Hudson et al. (2006, *Spectral
bidirectional reflectance of Antarctic snow: Measurements and parameterization*, J. Geophys. Res.
111, D18106) say the same of Dome C's snow — more anisotropic where ice is more absorptive — and
parameterize it by albedo for that reason. Their tables' numbers were not read here (the PDFs did
not open), so **no forward-peak value from them is pinned**.

What a game keeps is a diffuse term plus a forward lobe of stated weight. The engine's BSDF already
has one: GGX with Schlick's Fresnel at F0 = 0.04. Its reflectance factor at the specular peak
(the BSDF over the Lambertian of the same albedo, 0.98), computed from `brdf.slang`'s formulas:

| Sun's zenith angle | 30° | 52° | 65° | 75° |
|---|---|---|---|---|
| roughness 0.6 | 1.06 | 1.18 | 1.78 | 5.21 |
| roughness 0.8 | 0.99 | 1.01 | 1.11 | 1.66 |

At roughness 0.8 the dielectric lobe is the measured visible behaviour — near-Lambertian, a mild
forward rise at grazing sun — and **the scene's snow is given roughness 0.8**. Ice's own Fresnel
is lower (F0 = 0.018 at n = 1.311, Warren and Brandt's 550 nm row) but the forward peak of snow is
volume scattering, not the grains' surface; **pinned: `lobe` 1 by default (F0 0.04), a design
choice**, with 1–4 accepted for an author who wants more sheen (wind-glazed or melt-refrozen
crust). The weight is carried exactly through the metallic word with the diffuse kept: F0 =
0.04 (1 − m) + a′ m = 0.04 w and a′ (1 − m) = a solve to

    m = 2c / (b + sqrt(b² + 4kc)),   k = 0.04, b = a + k (w − 2), c = k (w − 1)

(the root of k m² + b m − c = 0 in its cancellation-free form; a′ = a / (1 − m)), so w = 1 is
m = 0 exactly and the picture is the dielectric's.

**Not built: the wrap.** Snow is a dense scattering medium: light enters, travels millimetres and
leaves, so a thin edge glows and a terminator softens. A wrap or translucency term needs the
light's direction, which only the resolve has; it is R99's remainder, asked of the resolve's owner
(below). At the terrain's scale the edge it would light is geometry R98's relief has not built yet.

### Glints

What makes snow read as snow at eye height is its sparkle: single facets of ice — and grains of
quartz and mica in sand — that mirror the sun into the eye. Jakob et al. (2014, *Discrete
stochastic microfacet models*, ACM TOG 33(4)) model a surface's microfacets as a finite random
population, so a pixel's footprint holds a countable few and the specular lobe is their mean only
when the footprint is large; Zirr and Kaplanyan (2016, *Real-time rendering of procedural
multiscale materials*, I3D) count the facets in the footprint on multilevel object-space grids,
stable and anti-aliased, and Deliot and Belcour (2023, *Real-time rendering of glinty appearances
using distributed binomial laws on anisotropic grids*, CGF 42(8)) give the counting the right
statistics for any footprint, 1.5–5× faster. All three key the facets to **the surface, at the
footprint's scale**, never to a texture of sparkles, and all three let the glints fade into the
smooth lobe as the footprint grows.

R99's version is the simplest member of that family that the material stage can express: the
ground is cut into world-anchored square cells whose side is the smallest power-of-two millimetres
at least the pixel's footprint (from the frame's integer cell and its local millimetres, never a
world float, as the mask and the lattice are), each cell's integer hash decides whether it holds a
glinting facet, and a facet that is there replaces the layer's slope with its own normal — drawn
from GGX of alpha 0.15 about the macro normal (Walter et al. 2007's sampling), turned at most 25° —
and its roughness with a facet's, 0.1. The resolve then
lights it with its own GGX for every light, shadowed like the pixel, so a facet flashes only where
the sun, the facet and the eye line up and twinkles as the eye moves, as real snow does. The chance
a cell holds one is

    p = density × min(1, 4 mm / footprint) × (1 − smoothstep(128 mm, 256 mm, footprint))

full up to a 4 mm footprint, falling as one over the footprint past it (the screen's glints thin
with distance, each one footprint in size), and none past 256 mm. **Every number of it is a design
choice, stated as one**: snow's density 0.5, sand's 0.15, the facets' tilt (GGX alpha 0.15, at most
25°), their roughness 0.1, the 4 mm and 256 mm. **Why the facets lean so little**: a glint replaces
the pixel's one normal, so its diffuse term turns with the facet too. The first captures drew the
facets from the layer's own lobe (alpha 0.64 at roughness 0.8, a third of them past 30°) at density
0.1, and the snow read as peppered: most facets miss the sun, and a pixel turned 30° from the sun
or towards the browner ground of the sky's ambient is a dark speck. Facets within 25° of the normal
move a pixel's diffuse by at most a tenth and glint where the half vector is near the normal — towards
the sun, where snow sparkles most; a backlit glint, which needs a facet turned past 40°, is the
resolve's to draw (it needs the light). The path tracer has no footprint and draws none (its many samples a pixel converge to
the lobe the glints are drawn from).

### Sand: the desert's and the beach's

W42's detail (renderer.md, "The sand close up"; [the fifth pass](sand-fifth-pass-2026-10-04.md))
is desert sand under wind: ripples across the wind, grain, grainflow. A beach is another surface:
wave-washed, finer and better sorted, with no wind ripples below the swash but swash marks and
rills, and **darker where it is wet**. Two papers explain the darkening.
Ångström (1925) proposed that a water film over a rough surface traps the light the surface
scatters back by total internal reflection at the film's top, so it meets the absorbing surface
again; Lekner and Dorf (1988, *Why some things are darker when wet*, Appl. Opt. 27(7)) carried the
model to an interpolation that agrees with measurement, the film's internal reflectance for
diffuse light being 0.475 for water rather than Ångström's 1 − 1/n² = 0.437. Twomey, Bohren and
Mergenthaler (1986, *Reflectance and albedo differences between wet and dry surfaces*, Appl. Opt.
25(3)) add that liquid in the pores lowers the refractive-index contrast, so the grains scatter more
forward and the light goes deeper before it returns: more absorption, and more saturated colour.

Summing Ångström's bounces with Lekner and Dorf's reflectances gives the wet diffuse albedo of a
dry albedo a:

    A = (1 − r_e)(1 − r_i) a / (1 − r_i a),   r_i = 1 − (1 − r_e) / n²

with water's diffuse external reflectance r_e = 0.066 and n = 1.333, so r_i = 0.4744 and (1 − r_e)
(1 − r_i) = 0.4909. For Ångström's dry sand (0.182, as Lekner and Dorf quote it) it gives 0.098
against his measured 0.091, a factor of 0.537:

| Dry albedo | 0 | 0.182 | 0.30 | 0.42 | 0.55 | 0.62 |
|---|---|---|---|---|---|---|
| Wet / dry | 0.491 | 0.537 | 0.572 | 0.613 | 0.664 | 0.696 |

Brighter sand darkens less, so applied per channel it **saturates** the colour as Twomey et al.
say. W72's porosity rule (Lagarde 2013) darkens a porosity-0.9 sand to 0.28 of its dry albedo,
twice the measured darkening; the beach response uses Lekner and Dorf instead. **Pinned:
`wet_darkening` 1, the model's own darkening at saturation**, scaled by the place's wetness (the
mask's `wet_m`, the scene's `wetness`), and the beach's roughness loses half that factor as W72's
wet ground does. Zhang and Voss (2006, Appl. Opt.) found neither model sufficient for submerged
layers; the bed under water is the water's optics, not this rule.

Grain sparkle — quartz (F0 0.045 at n = 1.54) and the mirror flakes of mica — uses the glints above
at sand's density. **Not built** (stated, a remainder): the beach's swash marks, rills and the
heavy-mineral laminae; and the wind sand's own glints and response were kept to W42's detail plus
the glints, since W42 has its own pages.

## What was built

A palette layer names a `response` (`TerrainMaterialLayer` version 4, `Terrain` version 10) with a
`snow` or `sand` block whose fields default to the numbers pinned above; the renderer's page has the
rules ([renderer](../subsystems/renderer.md#ground-materials-by-place), "Sand and snow"). In
`terrain_material.slang`, `terrain_respond` applies a layer's response where the layer is sampled,
once per layer on its record's flags; the blend then weighs it by its coverage and the height blend
as before. The snow's lobe weight and the wind sand's share are the two blend-level outputs
(`MaterialSample::metallic` and `detail_mask`), zero unless a layer responds. The test scene
(`content/test-scenes/ground-by-place`) gives its prior-placed `snow` the response and its sandy
pond a `beach_sand` margin in place of gravel.

**The tests**, each failing without the change: `gfx: sand and snow responses agree with their
double references` (the probe's `response_main` against the formulas above computed from the
papers' own numbers — χ, λ, ξ, n, r_e — the glint cell's integer hash exactly, its chance and facet
to float precision); `renderer: a snow layer whitens by its grain, a beach darkens wet, a default is
its bytes` (the mask probe's two layers: the east, default, layer byte-identical beside a snow
layer; the snow's albedo the table's 0.9765, 0.9856, 0.9958 within 0.006; a saturated beach's blue
at 0.733 of its dry, Lekner and Dorf's 0.735, where porosity 1 left 0.2); `renderer: a layer's sand
or snow response parses, packs and keeps every key` (the schema's new fields, the record's words, a
`Default` layer packing no word, the container key unmoved, the refusals).

## What it measured

Captures under the GPU lock, `msvc-release`, RTX 5090, 1920 × 1080, `--offscreen --terrain-tiles
--terrain-far 2`, `--wait-quiet 120` (the machine was never quiet: other agents' CPU at 65–100 %),
four held poses of one camera path (`r99-poses`, under the session's scratch): the north face's
snow from eye height looking uphill, the same place looking west, the snow cap from 50 m over the
slope, and the sandy pond's margin from eye height; at noon (sun 63.7° up) and at 19:00 (low sun).
"Before" is `main` at 83af2e3c drawing the scene as it was (snow a 0.86/0.88/0.92 tint at
roughness 0.6, a gravel margin); "after" the change drawing the scene with its responses.

| Pose | Noon: pixels changed (over 8 codes) | Low sun: changed (over 8) |
|---|---|---|
| Snow, eye height, uphill | 62.3 % (40.7 %) | 62.3 % (28.3 %) |
| Snow, eye height, west | 35.2 % (32.9 %) | 35.0 % (13.5 %) |
| Snow cap from 50 m | 44.1 % (30.6 %) | 44.2 % (27.8 %) |
| Pond margin, eye height | 56.6 % (47.2 %) | 55.8 % (44.5 %) |
| **The scene's old palette, all `Default`, after the change** | **0 of 2,073,600 at every pose** | |

What they read as. **The snow** is white rather than the old tint's grey, and from eye height uphill at noon it is strewn with single-pixel glints — mostly bright,
a few grey where a facet turns from the sun; looking west into the low sun its lit face is warm
white with a dense sparkle, and some sparkles are blue, facets turned to the sky. **The first
version peppered it** (the facets drawn from the layer's own lobe: most specks dark and brownish,
facets turned from the sun and towards the sky's ground term), which is why the facets lean at most
25° (above). From 50 m the glints thin with the footprint as designed and the cap reads as smooth
snow. **The beach** reads as tan sand where the gravel was grey, wet and so darker towards the
water by Lekner and Dorf's factor, with sparse sand glints; it has no swash marks yet.

**The material stage** (`--tunable renderer.resolve.stop_after=material`, the scene's own 20 s
path flown three times, `gpu_ms.resolve` per frame):

| Build, palette | Median ms | p95 ms | Machine (CPU others, start → end) |
|---|---|---|---|
| before, all `Default` | 0.443 | 0.575 | 80 % → 100 % |
| after, all `Default` | 0.408 | 0.845 | 100 % → 70 % |
| after, with the responses | 0.293 | 0.371 | 66 % → 82 % |

The three runs differ by more than any difference the change could make (the responses' run was the
fastest), so on this shared machine the stage's cost is **held to the noise**, with the `Default`
palette drawing the same bytes. The responses' run was taken with the first glint constants; the
change to them moves no operation in or out of the path.