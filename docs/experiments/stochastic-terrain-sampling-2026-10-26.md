# Stochastic terrain sampling (R96), 2026-10-26

**Question.** The forest's far ground is blotchy: past `detail_near_m`..`detail_far_m` (32 to
128 m in the scene) every triplanar layer fades to its mean colour, so beyond that the ground is
flat patches of four means whose edges are the 1 m masks' bilinear blends
([forest surface quality](forest-surface-quality-2026-10-26.md), the v9 report). The fade exists
because a triplanar layer repeats every `tile_m`, and the repeat shows at distance. Remove the
repeat and the fade can go: the texture's own mips then carry its variation to the horizon. Which
nonperiodic sampler, at what cost, and does the far ground regain variance at the texture's scale?

**Answer.** An opt-in `"sampling": "Stochastic"` per triplanar layer: a triangle lattice of
random tile offsets (three reads per map per projection), Burley's exponentiated weights at
power 7, Heitz and Neyret's variance-preserving operator applied to the texels directly (no
Gaussian transform, so **no lookup table, no second texture and no container section**), an
optional bounded per-tile turn with Mikkelsen's normal reorientation, and no detail fade. On the
forest it costs 0.2–0.7 ms of resolve at 2560×1440 and 1.7–2.3 ms at 11520×2160, no residency,
and gives the 150–500 m ground its texture back (it was flat means); past 500 m it adds little,
because the mips there hold little — the far blotches are mostly the masks' patches.

## Sources

- **Heitz and Neyret 2018**, "High-Performance By-Example Noise using a Histogram-Preserving
  Blending Operator", *Proc. ACM Comput. Graph. Interact. Tech.* 1(2), HPG 2018
  ([project page](https://eheitzresearch.wordpress.com/722-2/)). The output plane is a triangle
  grid; each vertex takes a random patch of the input and a point blends the three patches of its
  triangle by barycentric weights. Linear blending of independent samples loses variance (and
  contrast, and makes ghosts); for Gaussian inputs the fix is analytic — scale the blend's
  deviation from the mean by 1/√(Σwᵢ²), which restores the variance exactly — and non-Gaussian
  inputs are first sent through a histogram transform *T* to a Gaussian and back through *T⁻¹*
  after. The method is for stochastic, non-periodic textures (moss, granite, sand, bark).
- **Deliot and Heitz 2019**, "Procedural Stochastic Textures by Tiling and Blending", *GPU Zen 2*
  ([project page](https://eheitzresearch.wordpress.com/738-2/)): the real-time recipe — three 1D
  transforms in the colour histogram's eigen-space instead of the 3D transform, the triangle grid's
  skew (`uv * 2√3` then `[[1, -1/√3], [0, 2/√3]]`), sampling with the pixel's own UV derivatives
  (`SampleGrad`) since the jittered UV's are discontinuous, a lookup-table prefiltering that fixes
  the colour drift *T⁻¹* causes at coarse mips, and block-compressed transformed textures.
- **Burley 2019**, "On Histogram-preserving Blending for Randomized Texture Tiling", *JCGT* 8(4)
  ([paper](https://www.jcgt.org/published/0008/04/02/)): per-channel 1D look-up tables built
  trivially from the input histogram on load; a truncated Gaussian and a soft-clipping contrast
  operator against the three sources of clipping (as many as 8.2% of blended pixels clip for three
  equal weights under the linear operator); YCbCr with luminance-only preservation where per-channel
  preservation colours a texture; and **exponentiated weights** wᵢ^γ / Σ w^γ (Eq. 5) against
  ghosting — ghosts at γ ≤ 2, tile structure visible at γ ≥ 8, γ = 4 good for his example.
- **Mikkelsen 2022**, "Practical Real-Time Hex-Tiling", *JCGT* 11(2), 77–94
  ([paper](https://jcgt.org/published/0011/03/05/), [code, MIT](https://github.com/mmikk/hextile-demo)):
  drops histogram preservation to sample the original texture with no precomputation; argues the
  omission is most sensible for normal maps, which are derivatives of a height field and so blend
  linearly; γ = 7 and a luminance-driven weight metric; **per-tile random rotation**, which is
  possible for normal maps only without histogram preservation, because each sample's derivative
  must be turned with its tile before blending (`d = mul(rot, d)`, gradients turned for
  `SampleGrad`); a rotation-strength parameter scales the angle.

## The choice, by the numbers

Per layer, per projection, per map (colour, roughness+height, normal):

| method | reads | extra data per image | texture residency | precompute | histogram error (W1/σ) | variance kept |
|---|---:|---|---:|---|---:|---:|
| tiled (today) | 1 | none | 1× | none | 0 (it is the image) | 1 (but repeats; faded past 32–128 m) |
| Heitz–Neyret / Deliot–Heitz | 3 + 1 LUT | Gaussianized copy + eigen-basis + per-mip inverse LUT | 2× | transform build per image | ≈ 0 by construction | 1 |
| Burley 1D | 3 + 1 LUT | Gaussianized copy + 1D LUTs | 2× | trivial | ≈ 0 by construction | 1 |
| Mikkelsen, linear, γ 7 | 3 | none | 1× | none | 0.074 | 0.883 |
| linear blend, γ 1 | 3 | none | 1× | none | 0.343 | 0.501 |
| **variance-preserving on texels, γ 7 (chosen)** | 3 | none | 1× | none | **0.071** | **1.0008** |
| variance-preserving on texels, γ 4 | 3 | none | 1× | none | 0.118 | 1.001 |

The histogram error and variance columns are `gfx: the stochastic terrain sampler keeps the
texture's mean, variance and histogram` (`domain/gfx/tests/terrain_material_tests.cpp`): a 128²
pebble image with a skewed two-humped histogram, a million points over 260 × 260 repeats, each
method against the tiled read of the same points; W1 is the 1-Wasserstein distance between the
two value distributions over the image's standard deviation. The means agree to 0.0003 in every
row.

Why this one:

- **The transform buys little here and costs a texture.** Its purpose is to keep a non-Gaussian
  histogram's *shape* where weights mix; with γ = 7 the three-way mixes are a narrow band at each
  tile's rim, so the variance-preserving operator on the raw texels already lands closer to the
  image's histogram (W1 0.071 σ) than Mikkelsen's linear γ 7 (0.074 σ) while keeping the variance
  exactly. The transform would double every stochastic image's residency (a Gaussianized copy per
  map, because the originals stay for tiled users of the same bytes), add a dependent LUT read per
  map, need a per-mip LUT to undo the drift Deliot and Heitz describe — and could not coexist with
  rotated normals (Mikkelsen §3).
- **The operator is affine per channel with one set of weights**, so colour, roughness, height and
  the normal's slopes all take the same three places and the same weights: the maps stay
  coordinated, and no hue outside the convex range of the three samples' deviations appears (the
  coloration Burley traces to the nonlinear transform cannot happen). There is no mip drift to
  fix either: the operator is linear, so a coarse mip's blend has the coarse mip's mean.
- **The cost is three reads where tiling makes one**, but the stochastic branch also skips a
  projection whose share is under 1/128 (and renormalizes the rest), so on flat or gently sloped
  ground it reads one projection's three tiles: **9 texture reads per layer, the same 9 a tiled
  layer makes reading all three projections**. A 45° slope reads two projections, 18; a corner
  where all three projections weigh in reads 27.
- **γ = 7** is Mikkelsen's default and under Burley's γ ≥ 8, where his Figure 10 shows the tile
  structure; of the blends measured it is the closest to the image's histogram. Clipping: the
  output is saturated to [0, 1]; the means and variances above include it.

What it cannot do: preserve the histogram's *shape* exactly where three tiles mix (the 0.071 σ),
and invent variation finer than the texture's mips hold. Past the distance where a pixel covers a
whole repeat, the mip is the image's average and the ground is its mean again — at the forest's
1–3 m repeats that is several kilometres at 2560 wide; until then the far ground keeps whatever
variation its mips hold at the pixel's footprint.

## The lattice in integers

The lattice is Deliot and Heitz's skew, `s = [[2√3, −2], [0, 4]] uv`, with 2√3 replaced by 97/28
(5 × 10⁻⁵ long: the triangles stay equilateral to that). With a rational skew the lattice point's
integer part is exact from the frame's integer cell in 64-bit integers — `97·i_u − 56·i_v` and
`112·i_v` millimetres over `28·pitch` — and only the frame-local rest is a float, so no absolute
world position is formed ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)),
a tile, ring or far level that draws a point reads the same three tiles, and the hash of a lattice
point (Wellons' `lowbias32`, integers only) is the same on the CPU and every GPU. The probe holds
the GPU's lattice to the double reference at an eye 100,000 km out.

The turn: a tile reads its content at `centre + R(point − centre) + offset`, its UV derivatives
turned by R; the normal map's y is the image's up, opposite the UV's v, so the stored slope pair
turns by R too (in a true gradient frame it is Rᵀ, Mikkelsen's `mul(rot, d)` with his opposite
sampling convention). `gfx: a turned stochastic tile turns its normal map's slopes with its
colour` checks it against finite differences of the blended height at turns of 0, 0.9 and 2.5 rad.
With no turn (the default) a directed texture keeps its direction: the gradient energy along
stripes is 0.24 of that across them with offsets alone (all of it in the tiles' narrow blend rims) and 1.0 with turns to 180°
(`gfx: a stochastic layer without a turn keeps a directed texture's direction`).

## Measurements on the forest

**Setup.** This worktree's `engine-view` in `msvc-release` (RTX 5090), offscreen, the POC's frozen
v14 scene (`reports/quality-v15/engine-c1ee971d/runs/expanded-forest-rt-wide-…/scene.json`) copied
to scratch with its tile set's `ddc` pointed at a scratch copy of `world/v14/ddc`; the arguments of
the POC's run (`--terrain-tiles --terrain-far 6 --lod 1 --time-of-day 15 --no-lights --raster hw`,
`--rt-budget-mib 1024 --deform-pool-mib 64`, flora motion on) with `--shadows csm`, so that the
resolve's time is its material work and not its shadow rays. *Before* is the base (5552a2b3) on the
scene as it is; *after* is this change on the same scene with all nine triplanar layers
`"sampling": "Stochastic"` (no turn). Views: the owner's tag 5 (the mountain) and tag 1 (the beach)
from `surface-audit/tag-0N-path.json`, and **across the lake** from tag 1's place 30 m up, looking
at (−755, 15, 711), the far shore 300 m to 2 km out. 241 frames of a still camera, `--wait-quiet 120`.

**Machine state**: never quiet tonight. Every run waited its 120 s and measured anyway, with other
agents' GPU jobs holding the GPU lock (`claude-engine`, `codex-night-astra-relief`,
`codex-night-sol-alloc`), other processes at 9–100% of the CPU and the GPU 1–100% busy at start.
The totals swing by a factor of three between neighbouring runs and are not comparable; the
resolve's own GPU time was stable within each run (p95 within 2% of the median) and is the number
quoted, as an upper bound.

| view | size | resolve before (ms) | resolve after (ms) | Δ | texture residency before / after |
|---|---|---:|---:|---:|---|
| tag 5, mountain | 2560×1440 | 0.71 | 1.38 | +0.67 | 383.9 / 383.9 MiB |
| tag 1, beach | 2560×1440 | 0.70 | 0.86 | +0.16 | 383.9 / 383.9 MiB |
| across the lake | 2560×1440 | 0.63 | 0.80 | +0.17 | 383.9 / 383.9 MiB |
| tag 5, mountain | 11520×2160 | 3.52 | 5.84 | +2.32 | 383.9 / 383.9 MiB |
| tag 1, beach | 11520×2160 | 5.12 | 6.78 | +1.66 | 383.9 / 383.9 MiB |
| across the lake | 11520×2160 | 3.63 | 5.75 | +2.12 | 383.9 / 383.9 MiB |

**Samples per ground pixel.** A tiled layer reads 9 texels inside `detail_far_m` (128 m here) and
**none** past it — it is its mean there — so most of the forest's far ground cost no texture reads
at all before. A stochastic layer reads 9 on ground under 16.6° (where the side projections' share,
sin⁴/(sin⁴ + cos⁴), is under 1/128), 18 on steeper ground, 27 at a three-way corner, at every
distance, times the one to four layers a mask tile weighs there. That is the whole of the cost: the
mountain (tag 5) is steep, so it reads two projections nearly everywhere, and the far ground in
every view went from 0 reads to 9–18 per layer. Texture residency does not move: no texture,
table or section is added.

**The same bytes.** The base and this change draw the tiled scene to identical PNGs, shaded and
albedo, at all three views at 2560×1440 (file hashes equal).

**The blotch measure.** Albedo (`--view albedo`) of the same scene with its instances, placements
and fires removed, so every brown pixel is ground; each ground pixel's world point found by marching
the source heightfield (`region.f32`, 0.5 m) along the pixel's ray; the points binned into
50 m × 50 m windows; in each window of 30 points or more, the standard deviation of linear
luminance, and a fine-scale deviation (each pixel less the mean of its 5×5 ground neighbours, the
part of the variation that is not the masks' patches). Means and medians over the windows:

| view, band | size | window σ, median: before → after | fine σ, mean: before → after |
|---|---|---|---|
| across the lake, 150–500 m | 2560×1440 | 0.0000 → 0.0028 | 0.0035 → 0.0061 |
| across the lake, 150–500 m | 11520×2160 | 0.0000 → 0.0035 | 0.0018 → 0.0061 |
| across the lake, 500 m–2 km | 2560×1440 | 0.0040 → 0.0061 | 0.0251 → 0.0259 |
| across the lake, 500 m–2 km | 11520×2160 | 0.0174 → 0.0214 | 0.0154 → 0.0181 |
| tag 1, 150–500 m | 11520×2160 | 0.0110 → 0.0095 | 0.0129 → 0.0153 |
| tag 1, 500 m–2 km | 11520×2160 | 0.0646 → 0.0666 | 0.0232 → 0.0263 |
| tag 5, 500 m–2 km | 11520×2160 | 0.0861 → 0.0862 | 0.0356 → 0.0376 |

Before, the median window across the lake from 150 to 500 m has **no** variation at all (σ 0): it is
one layer's mean, and the only change across the ground is at the masks' patch edges — the blotch,
measured. After, every window carries the texture's own variation at its mip, and the fine scale
rises 1.7× at 2560 and 3.4× at 11520 there; the far shore's gravel band shows its grain
(`captures/before-lake-2560-farshore-x2.png` against `after-…`). **Past 500 m the gain is small**
(window σ +0.002 at 2560, +0.004 at 11520): at 1–3 m repeats a pixel there covers 0.4–4 m of
ground along the view, so the mip it selects is within one to three levels of the image's average
and holds little of the texture's variance. The far ground's blotchiness past 500 m is mostly the
masks' patches and the layers' mean colours themselves (the window σ is dominated by windows
holding a patch edge), which a sampler cannot change: that is content (patch scale and contrast) and
R92/R99 work, not R96's.

Captures (scratch, not committed): the six albedo and six shaded runs per state under the agent's
scratch `stochastic/runs/` (`albedo-before-lake/view.png`, `albedo-after-lake/view.png`,
`ground-{before,after}-{tag05,tag01,lake}[-11520]/view.png`, `{before,after}-{view}-{size}/view.png`
with their `report.json`), and the far-shore crops in `stochastic/captures/`.
