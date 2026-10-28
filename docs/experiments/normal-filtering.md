# Normal filtering across scale (R97)

2026-10-26 to 2026-10-27. Research and the first implementation by a Codex night session on
`5552a2b3` (0e8cce17, 11bb36e4); reviewed, corrected, held to a path-traced fixture, measured on the
forest and turned on by default on `dad55f90` (roadmap R97). **Verdict: on by default** — the
fixture's error against the path tracer falls by half where the bumps are unresolved, the forest's
ground loses 8–15% of its sun-specular energy at the owner's marked views, and the cost is
0.1–0.3 ms of resolve at 2560×1440 and 83 MB of textures on the forest; what it does not fix on the
mountain is [below](#what-r97-does-not-fix).

## Question and baseline

The forest's native terrain fades normal slopes to zero between 32 and 128 m while
roughness approaches a layer mean. The normal builder also renormalizes each mip,
and BC5 reconstructs unit length, discarding evidence of unresolved variation.
The experiment must preserve that variation in the GGX response and compare it
with supersampled shading, rather than increase distance settings or roughness
globally. The owner's tags 1, 5 and 8 are the beach, mountain and backshore cameras
in the read-only POC's `reports/quality-v15/surface-audit/markers.json`.

**Machine state:** every measurement below names the harness's `machine_state`; numbers taken beside
other load are upper bounds.

## Sources and what they establish

- [Toksvig, Mipmapping Normal Maps (2005; NVIDIA technical brief dated 2004)](https://developer.download.nvidia.com/whitepapers/2006/Mipmapping_Normal_Maps.pdf):
  the length L of an averaged unit normal estimates variation; equation (1) uses
  `(1-L)/L`. Its original highlight adjustment is Blinn-Phong. Reusing that
  exponent adjustment for GGX would be an unsupported model change.
- [Olano and Baker, LEAN Mapping (2010)](https://userpages.cs.umbc.edu/olano/papers/lean/):
  filter first and second slope moments before deriving covariance, preserving
  anisotropy and allowing layers to share a tangent frame. The Ward shading model
  is not this engine's GGX model. The author's page includes normalization errata.
- [Dupuy et al., LEADR Mapping (2013)](https://perso.liris.cnrs.fr/victor.ostromoukhov/publications/pdf/SAsia2013-LEADR.pdf):
  displacement gradients and their moments reconstruct a noncentred anisotropic
  Beckmann distribution. Masking, shadowing and projected-area weighting matter:
  filtering normal-map shading is not equivalent to filtering displaced geometry.
- [Kaplanyan, Hill, Patney and Lefohn (2016)](https://research.nvidia.com/sites/default/files/pubs/2016-06_Filtering-Distributions-of/NDFFiltering.pdf):
  filter the NDF over the footprint in slope space. Section 5.4 distinguishes
  exact Beckmann convolution from approximate fitting of a filtered GGX lobe;
  section 5.5 combines normal-map and geometric filtering. Large grazing-angle
  derivatives require care, not an unbounded roughness increment.
- [Tokuyoshi and Kaplanyan (2019)](https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf)
  and [their 2021 projected-space formulation](https://www.jcgt.org/published/0010/02/02/paper.pdf):
  projected normal derivatives avoid the unstable division by a grazing half
  vector. The 2021 equation (13), listing 5 gives
  `alpha_filtered^2 = alpha^2 + min(2 sigma^2 (|dn_dx|^2 + |dn_dy|^2), kappa)`,
  with `sigma^2 = 1/(2 pi)` and `kappa = 0.18`. Those are a filter-kernel variance
  and an overfiltering cap, not measured constants of a ground material.
- [Hill, Specular Showdown in the Wild West](https://blog.selfshadow.com/2011/07/22/specular-showdown/):
  the article is dated **2011**, not 2012, and discusses Blinn-Phong rather than
  providing a GGX formula. Its useful observation here is that two-component
  normal encodings lose length, so a precomputed auxiliary map avoids that loss.

## Implementation boundary and candidate

The statistic is optional texture section **5** (`normal_variance`; texture.md, "Normal
variance"). The Codex tree took 6 and left 5 for an R96 histogram section; R96 landed needing no
section, so 5 is the next kind of an append-only list and nothing was ever written as 6. Existing
sections and layouts stay unchanged. Store the statistic independently of roughness so the same
normal image can be shared by materials with different roughness and normal strength. Build it
from unrenormalized averages of source unit normals, never from reconstructed BC5. Positive filter
weights are needed for a meaningful distribution; Mitchell's negative lobes cannot be interpreted
as probabilities. An unmipped map writes no chain: its one level would be zeros.

The compact candidate is isotropic Toksvig shortening with a GGX width fit:
`v = (1-L)/max(L, epsilon)`, `alpha_filtered^2 = min(1, alpha^2 + 2 v)`.
Here perceptual roughness r means `alpha = r*r`; the final perceptual roughness
is the fourth root, not the square root, of the adjusted squared alpha. This is
an approximation, not exact GGX moment convolution (GGX slope variance diverges).
The supersampling fixture below bounds this fit on nine sinusoidal fixtures; it does not establish an error bound for arbitrary normal maps.
The same final alpha must feed both the GGX distribution and Smith visibility.

R97 owns post-fetch normal/roughness combination and fades; R96 owns the terrain
sampling helpers and colour policy. Absent statistics must keep legacy bytes.
Geometry derivatives must come from the hit triangle and `screen_barycentrics`,
not fullscreen quad differences across unrelated surfaces. The reference path
tracer's zero-footprint reads must not be widened a second time merely because
the supersampling reference has many rays.

Two corrections from the review (2026-10-27, d579bed1). **A stochastic layer** (R96) had its colour
and height faded by the variance path although R96 never fades it, and its widening was applied to
each of its three tiles before the variance-preserving blend, which scales a tile's departure from
the layer's (unwidened) mean roughness by up to √3 at a hex's centre: the widening itself would have
varied with the lattice. The tiles are now read unwidened, their shortening averaged by the hex
weights as a plain mixture, and the widening applied once per projection. **The switch**:
`renderer.material.normal_filter`, read at a scene's material upload; off uploads no chain and sets
no flag, so a capture is main's to the byte.

## Reference fixture, 2026-10-26

The double reference shades a black dielectric plane (F0 0.04) under a unit sun
15 degrees above the horizon. A normal-map period has slopes
`(a sin(2 pi u), a sin(2 pi v))`, normalized with the up component 1.
The pixel centre sees a flat normal; 8x8 midpoint samples integrate the period.
For each row, 1,845 views span 45–89 degrees from the normal and azimuth -20–20
degrees about the specular direction. RMSE is linear outgoing sun-specular radiance,
not a display-space score. `terrain_material_tests.cpp` runs the engine's double
BRDF for every sample; all nine assertions passed on MSVC.

| roughness | slope amplitude | unfiltered RMSE | filtered RMSE | reduction |
|---:|---:|---:|---:|---:|
| 0.2 | 0.1 | 2.72279 | 0.674553 | 75.2% |
| 0.2 | 0.3 | 3.02920 | 0.556570 | 81.6% |
| 0.2 | 0.6 | 3.00828 | 0.0888406 | 97.0% |
| 0.5 | 0.1 | 0.0729033 | 0.0241265 | 66.9% |
| 0.5 | 0.3 | 0.289647 | 0.0512663 | 82.3% |
| 0.5 | 0.6 | 0.388387 | 0.0330602 | 91.5% |
| 0.8 | 0.1 | 0.00347532 | 0.00244025 | 29.8% |
| 0.8 | 0.3 | 0.0190291 | 0.00904675 | 52.5% |
| 0.8 | 0.6 | 0.0321309 | 0.00879838 | 72.6% |

This is an analytic normal-map fixture, not a converged path-traced forest image.
The low-roughness error remains large at some grazing views. The approximation
loses anisotropy, and it does not track covariance between triplanar projections.
Layer combination uses `sum(w r^4) + sum(w |slope|^2) - |sum(w slope)|^2`, clamped
to [0,1], then a fourth root. Wetness remains the existing subsequent operation.
The projected geometry kernel uses the published 1/pi coefficient and 0.18 cap;
only variance-bearing surfaces enable it, preserving legacy material bytes.

The GPU tests require absolute agreement within 2e-6 for the formula and layer
combination. A synthetic BC4 chain sampled at 2,048 increasing footprints spans
eight mips, requires monotonic roughness and a maximum adjacent step below 0.008.
That is a stated fixture bound, not a claim that arbitrary source textures must
have locally monotone variance. Flat inputs and serial/parallel builder identity
are separately checked by texture tests. Clang's double-promotion warning requires
explicit casts in GPU-to-double comparisons; MSVC does not diagnose those.

## The plane: the filter against the path tracer (2026-10-27)

`renderer: normal filtering holds a grazing normal-mapped plane to its converged picture`
(`systems/renderer/tests/terrain_materials_tests.cpp`) is the picture-level check the analytic
fixture above could not be: flat terrain whose one tiled layer carries a 64-texel map of eight
periods of `sin x sin z` (slopes to 0.35) every 0.5 m — a bump is 6.25 cm — at a flat roughness of
0.7 (no glint clips), dark, under a sun 12° up straight ahead, seen at the sun's own elevation with a
20° view from 1, 4 and 16 m, so the pixel's footprint grows from a quarter of a bump to about four.
The truth is the reference path tracer at 256 jittered samples a pixel: it reads level zero, which
has no variance, and integrates the footprint with its rays. The filtered and the unfiltered scene
are the same load uploaded twice, once with `renderer.material.normal_filter` off. RMSE is over the
picture's linear RGB (the display encode undone), RTX 5090, msvc-debug:

| distance | RMSE, filtered 1x | unfiltered 1x | lower by | the picture's light, filtered / unfiltered vs traced | FLIP vs traced, filtered / unfiltered |
|---:|---:|---:|---:|---|---|
| 1 m | 0.0477 | 0.0544 | 12% | +7.2% / +9.6% | 0.147 / 0.156 |
| 4 m | 0.0214 | 0.0462 | 54% | +0.5% / +14.3% | 0.068 / 0.114 |
| 16 m | 0.0180 | 0.0431 | 58% | −6.6% / +14.9% | 0.112 / 0.115 |

The test asserts the filtered picture nearer the truth at every distance, by at least a third at
4 and 16 m, nearer in its light, and no worse by FLIP. It does not assert the reference tests'
FLIP bound of 0.05: that is a diffuse plane's, and here the tracer's box-integrated pixel and the
resolve's point sample differ wherever the bumps are resolved, filtered or not (at 1 m both are
near 0.15).

**The supersampled resolve is no truth for the unfiltered path.** The brief's 8x render, box-averaged
in light, was rendered beside it: against the path tracer its error is 0.039, 0.036 and 0.021
filtered and 0.039, 0.039 and 0.043 unfiltered — at 16 m the 8x unfiltered picture is still 12%
too bright, because each of its sub-pixels' mips flattens bumps that sub-pixel still spans. That is
the defect itself, one octave down, and why the tracer is the reference here.

**Where the fit is wrong.** At 16 m the filtered picture is 6.6% dark where the unfiltered one is
15% bright. The widened lobe's Smith masking and shadowing grows with α′ at grazing, while a normal
map's bumps occlude nothing (LEADR's point): the fit trades a little energy at grazing for the
lobe's width. Feeding D the widened roughness and Smith the material's own would be the next
experiment; it is the BSDF's model, not the roughness term, and was not changed here.

## The forest (v18), 2026-10-27

Own offscreen msvc-release engine-view on `dad55f90` plus this work, the POC's v18 maps scene
(`expanded-forest-csm-wide-leaf-wide-20261027-201557/scene.json`) on a scratch copy of
`world/v18/ddc`, the run's own arguments (`--shadows csm`, `--lod 1`, sway on), each camera held
still for 60 warm-up and 240 measured frames under the GPU lock with `--wait-quiet 120`; the energy
from separate 8-frame `--view sun-specular --present-format linear` EXR captures, R averaged where G
marks ground. "Off" is the same binary with `renderer.material.normal_filter=false`. Tag 5's marked
camera (262.3 m) stands under the ground v17's authored relief raised there — its probe reads
1.8 × 10⁻⁸ over 2.9 million ground pixels, the slope's underside — so it is measured 3 m higher, the
same direction.

| view, 2560×1440 | ground pixels | sun-specular, off → on | roughness, off → on | resolve ms, off → on | machine state (others' CPU % start/end, GPU % start/end), off; on |
|---|---:|---|---|---|---|
| tag 1, beach | 1,396,718 | 0.018365 → 0.015582 (−15.2%) | 0.735 → 0.781 | 0.928 → 1.026 | 8.3/16.5, 2/1; 9.4/13.2, 4/2 |
| tag 5, mountain (+3 m) | 2,121,327 | 0.029495 → 0.028839 (−2.2%) | 0.667 → 0.672 | 1.501 → 1.756 | 24.2/53.0, 2/8 (busy); 9.8/11.6, 2/2 |
| tag 8, backshore | 2,155,802 | 0.010272 → 0.009435 (−8.1%) | 0.800 → 0.822 | 1.110 → 1.249 | 8.2/7.3, 2/2; 6.6/8.9, 1/1 |

At 11520×2160 on tag 1 the resolve goes 7.328 → 8.138 ms (total 28.90 → 29.67 ms; others' CPU
8.5/6.9% and GPU 1/1% off, 9.2/11.1% and 3/100% on — the GPU figure at the end is the run's own).
The cost is the chain's read beside every normal read and, on a tiled layer past `detail_far_m`,
the reads the fade used to skip: +0.10 to +0.26 ms at 2560×1440 (10–18% of the resolve), +0.81 ms
(11%) at the surround width. The scene's textures go from 402,573,312 to 485,723,648 bytes: 167
chains, 83,150,336 bytes, 20.7%.

Codex's v14 measurement the night before (same cameras, tag 5 unlifted on the v14 ground, a GPU at
100%) found −17.0%, −2.5% and −16.3%: the same direction and the same mountain.

**The first POC probes measured nothing** — a lesson worth keeping. They returned identical ground
energy with filtering on and off, because the frozen terrain containers held version-1 embedded
texture keys, and scene preparation trusted an embedded image's recorded key where it recomputed a
path image's: the new section was never built. `prepare_scene_textures` now derives every key from
the source identity and the current options and version (texture.md, end). Check that a cache holds
the section before reading an unchanged picture as a shading result.

## What R97 does not fix

**The mountain's sheen.** Tag 5 loses 2.2% of its sun-specular energy where the beach loses 15%: the
broad sheen on the rock is not normal-map variation the footprint lost. The rock layer's own
roughness (0.67 on average over the view) on a surface smooth between the heightfield's vertices is
what shines; what would break it up is relief at the decimetre-to-metre scale the heightfield does
not carry (R98) and substrate detail inside the layered path (R99), and the masks' choice of layer
and roughness is content. R97 makes the far ground keep the roughness its near ground has; it does
not make the near ground rougher.

**Not measured.** Temporal shimmer along a moving camera (the fixture is still; the chain is a mip
like any other and moves nothing between frames, but no flythrough was compared). The rays' path
(`--raster rt`): R97 changes shading and not visibility, and the fixture draws `hw`. A byte-identity
capture of the Khronos samples and the desert overlook: neither is in this worktree
(`tools/fetch-samples.ps1` downloads them), so the identity was taken on desert-erg, desert-endless,
rock-field and oasis-palms instead (main's engine-view, this one off, this one on: the same bytes
each; none of them has a chain) and on the forest, which has 167.

## The default

**On** (`renderer.material.normal_filter` true, 2026-10-27): the fixture holds the filtered picture
nearer the path tracer at every distance and by more than half where the bumps are unresolved; the
forest's marked views lose the sheen the filter is for and keep the mountain's, which it is not; the
cost is the table's, 0.1–0.3 ms at 2560×1440 and 0.8 ms at the surround width, and a fifth more
texture memory on the forest; and with it off, or on a material without a chain, every capture is
main's to the byte.
