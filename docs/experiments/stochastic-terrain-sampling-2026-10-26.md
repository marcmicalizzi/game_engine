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
optional bounded per-tile turn with Mikkelsen's normal reorientation, and no detail fade. The
measurements are below.

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

*(filled below once measured)*
