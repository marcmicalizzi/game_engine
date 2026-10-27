# Normal filtering across scale (R97)

2026-10-26. Work in progress. The starting tree is `5552a2b3`. No appearance or
performance acceptance is claimed by this research note.

## Question and baseline

The forest's native terrain fades normal slopes to zero between 32 and 128 m while
roughness approaches a layer mean. The normal builder also renormalizes each mip,
and BC5 reconstructs unit length, discarding evidence of unresolved variation.
The experiment must preserve that variation in the GGX response and compare it
with supersampled shading, rather than increase distance settings or roughness
globally. The owner's tags 1, 5 and 8 are the beach, mountain and backshore cameras
in the read-only POC's `reports/quality-v15/surface-audit/markers.json`.

**Machine state:** no measurements yet. Future measurements must record the
harness's `machine_state`; shared-machine times are upper bounds. Captures and
logs belong in `D:/workspace/ge-codex-scratch/astra-filter`, using this worktree's
offscreen binary and a scratch copy of the frozen v14 DDC.

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

Reserve optional texture section **6** for normal statistics; section 5 belongs
to concurrent R96 histogram work. Existing sections and layouts stay unchanged.
Store the statistic independently of roughness so the same normal image can be
shared by materials with different roughness and normal strength. Build it from
unrenormalized averages of source unit normals, never from reconstructed BC5.
Positive filter weights are needed for a meaningful distribution; Mitchell's
negative lobes cannot be interpreted as probabilities.

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

## Acceptance still required

Deterministic texture bytes across MSVC and Clang; flat-map identity; monotone
footprint fixture and a stated neighbouring-pixel continuity bound; GPU/double
agreement; 1 versus 64 samples per pixel error; raster/ray terrain byte agreement;
zero frame-loop allocations; three 2560x1440 camera captures and ground sun-specular
means before/after; 11520x2160 resolve timings with machine state. The texture and double-reference results above are established; GPU and corrected forest acceptance remain pending.

The texture cache-version bump also changes the embedded impostor texture keys in
cluster section 33. The broadleaf/ancient full-container golden folds therefore move
from `50be5dcf910f9bf5` / `6d4e6b50d6180080` to
`060c68283b28f955` / `daf6c70391dc9df3` (Clang); no geometry or bake algorithm changed.
The Windows cross-check passed (one case, 318 assertions), matching Clang on both folds.

The first POC probes returned identical ground energy with filtering on and off:
the frozen terrain containers held version-1 embedded texture keys. External image
keys were recomputed, but embedded keys were trusted unchanged, preventing the
new derived section from being built. Scene preparation now derives current keys
for embedded sources as well. The first `after-*` captures and probes therefore
measure the legacy ground path and are **not evidence of R97's appearance effect**.
They are retained in scratch as the diagnostic trail; corrected captures must use
the refreshed cache path. This is why section presence must be checked in cached
content before interpreting an unchanged image as a shading result.

## Forest captures, 2026-10-26

Own offscreen MSVC-debug engine-view, RTX 5090, frozen v14 scene, CSM shadows,
120 measured frames after 30 warm-up frames, copied scratch DDC, tags 1/5/8.
Every timing run used `--wait-quiet 60` under the GPU lock. Other CPU activity
persisted: these are shared-machine upper bounds, not a clean pass-cost delta.
Full machine states and inputs are in each scratch `report.json` and `stdout.log`.

| view | resolve before ms | resolve after ms | other CPU % before start/end | after start/end | GPU % before start/end | after start/end |
|---|---:|---:|---|---|---|---|
| tag1-11520 | 5.116016 | 6.835728 | 9.1/12.3 | 9.9/14.0 | 1/1 | 1/100 |
| tag1-2560 | 0.699184 | 0.906240 | 9.9/12.8 | 41.5/34.9 | 1/1 | 100/100 |
| tag5-2560 | 0.704080 | 0.897808 | 9.7/14.8 | 93.9/78.1 | 2/100 | 6/100 |
| tag8-2560 | 0.909344 | 1.163200 | 9.9/9.5 | 41.4/30.3 | 1/1 | 100/100 |

Captures are under `D:/workspace/ge-codex-scratch/astra-filter/`:
`before-tag{1,5,8}-2560/view.png`, `after-refresh-tag{1,5,8}-2560/view.png`,
and `before-tag1-11520/view.png` / `after-refresh-tag1-11520/view.png`.
The earlier `after-*` directories without `refresh` predate the cache fix and are not acceptance results.

| camera | ground pixels | mean sun-specular before | after | reduction | mean roughness before/after |
|---|---:|---:|---:|---:|---|
| tag 1 | 1401717 | 0.0198943250 | 0.0165057694 | 17.03% | 0.727135/0.774451 |
| tag 5 | 2142811 | 0.0312707988 | 0.0305017770 | 2.46% | 0.668045/0.673129 |
| tag 8 | 2170521 | 0.0299807760 | 0.0251023760 | 16.27% | 0.534245/0.596096 |

These are raw linear EXR probes of the shadowed sun term before aerial scattering,
not brightness measured from PNGs. The legacy probe disables R97; old and refreshed
runs have identical ground-mask counts and zero nonfinite ground values. Files:
`sun-specular-legacy-tag{1,5,8}-2560/probe.exr` and
`refreshed-sun-specular-tag{1,5,8}-2560/probe.exr`. Probe reports retain their
machine states; those runs measure energy, not pass cost.

The scene texture allocation rose from 429,208,064 to 485,723,648 bytes.
A broad sheen remains visible on the beach and especially the mountain. This
is a measured reduction, not closure of the surface audit. The triplanar-axis
covariance, storage-quantization error in the supersampling fixture, requested
raster/ray fixture byte parity, temporal shimmer and quiet cost acceptance remain
unproven. R97 stays in progress. Colour-fade policy, relief, shadows and the ray
acceleration chain were not redesigned.

The first focused GPU run measured a maximum adjacent roughness step of 0.00097394
and passed the 2e-6 formula comparisons. Its identity assertion used CPU and GPU
`i/7` as if they were bit-identical: 3/7 and 6/7 differed by division rounding.
The test now uses exactly representable `i/8` inputs, retaining an exact comparison
for zero widening. This changes the fixture inputs, not the shading formula or its
tolerance. The corrected run remains to be recorded.
