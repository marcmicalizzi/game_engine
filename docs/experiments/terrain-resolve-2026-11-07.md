# The terrain resolve by what the pixel can see (R136, 2026-11-07)

Roadmap R136; the rules are on [renderer, "What a far pixel reads"](../subsystems/renderer.md#what-a-far-pixel-reads).
Batch 53's R134 measured the v31 fell's resolve at 1.92 ms at 1920×1080 and 17.07 ms at
11520×2160 on a terrain-only scene with every ground layer stochastic: about a hundred texture
reads a pixel (three tiles of colour, metallic-roughness with height, normal and its variance chain
for each kept projection, four layers), whatever the distance. This page measures five rules that
read less where the pixel cannot tell, alone and together, and says which one ships.

## The rules

| Bit | Rule | Where it acts | Picture |
|---|---|---|---|
| 1 | **Tiles by footprint**: one tile (the nearest lattice point) instead of three past a cell a pixel; a band from one cell (sqrt \|du × dv\| = 28/97 of a repeat) to two | far ground only | the same distribution: the variance-preserving blend of three independent tiles keeps the mip's mean and variance (Heitz and Neyret 2018, Eq. 9), and one tile is that mip |
| 2 | **Projections by what they contribute**: the 1/128 floor rises past a cell to a band of 1/32–1/16, renormalized | far slopes only | a dropped share s moves the colour by s × the gap between two projections' reads at a coarse mip |
| 4 | **Maps by footprint**: past a repeat's filter width (max(minor, major / 16), the sampler's anisotropy being 16) the hardware reads each map's last mip, one texel, which the layer's means stand for; a band from half a repeat to one; the variance chain still read once | the horizon only | the means against the 1×1 mip: the compression's error |
| 8 | **Layers by weight**: a mask layer under 1/128 of the pixel not read past a cell of it, a band to 1/64, its coverage still counted | far transitions only | the layer's share, renormalized by the blend |
| 16 | **Tiles by weight**: a tile whose Burley weight is under 1/256 not read, a band to 1/128, the kept weights renormalized and the gain from them | everywhere | the tile's share of its departure from the mean, under 1/128 of it |

Lever 4 of the brief asked first whether the mask sampler already drops small layers: it skips a
layer only where its filtered weight is exactly 0 (`sample_terrain_material`), and the bilinear
weights leave small shares across every transition.

Why tiles by weight reads fewer tiles: with the barycentrics' seventh powers normalized, the second tile's share is under
1/128 where its barycentric is under half the largest's (0.5⁷ = 1/128); the region of a lattice
triangle where the largest barycentric is at least twice each other one is a sixth of it per
vertex, half of the triangle in all, and the band at 1/256 drops a little less. The probe test
counts it over one cell's 4,096 points.

## The measurement

- **The scene**: the forest proof of concept's v31 terrain repro with every layer stochastic and
  unturned (`reports/quality-v31/repro-terrain-hex/scene-norot.json`, copied, its tile cache pointed
  at this agent's copy of R134's), the `v30` materials, the fell camera (`hex-fell-path.json`, one
  pose held), `--raster hw --shadows csm --terrain-tiles --terrain-far 6 --time-of-day 15
  --time-rate 0 --sun-rate 0 --no-lights`, offscreen, 240 frames with `--wait-quiet 120`, the
  capture after them.
- **The variants**: one `msvc-release` engine-view at this branch, the terrain resolve's modules
  replaced per run through `--shaders <manifest>` naming SPIR-V compiled by the build's slangc with
  the build's flags and `-DTERRAIN_FOOTPRINT_LEVERS=<bits>`; **base** is the same modules compiled
  from `8be9c6d4`'s `terrain_material.slang`. Bits 0 draw base's bytes (0 changed pixels).
- **The machine**: RTX 5090, each group under the GPU lock (`agent-R136`); the GPU idle at every
  start; other processes on 5–57% of the CPU (other agents' builds), so by the harness's rule each
  number is an upper bound — the resolve is a GPU timer and moved by under 0.03 ms between repeats
  of the same module (base 1.575, bits 0 1.556 and 1.595 across the three groups).

## 1920 × 1080

The resolve's GPU median of 240 frames; changed pixels against base (of 2,073,600), their split
over the frame's thirds top to bottom, the largest channel step, and `engine-image compare`'s FLIP
mean.

| Variant | Resolve ms | Changed | Thirds | > 2 codes | Worst | FLIP |
|---|---|---|---|---|---|---|
| base | 1.575 | — | | | | |
| 0 (the rules compiled out) | 1.556–1.595 | 0 | | | | 0 |
| 1 tiles by footprint (first form, its own path) | 1.943 | 7,436 | 2,585 / 4,851 / 0 | 4 | 4 | 0.00020 |
| 1 tiles by footprint (one loop) | 1.655 | 7,437 | 2,586 / 4,851 / 0 | 4 | 4 | 0.00020 |
| 2 projections | 1.569 | 3,044 | 1,082 / 1,962 / 0 | 0 | 1 | 0.00008 |
| 4 maps (one loop) | 1.599 | 167 | 167 / 0 / 0 | 3 | 7 | 0.000006 |
| 8 layers | 1.578 | 0 | | | | 0 |
| 15 the four footprint rules | 2.017 | 9,658 | 3,439 / 6,219 / 0 | 8 | 7 | 0.00023 |
| **16 tiles by weight (shipped)** | **1.276** | 38,246 | 6,752 / 12,865 / 18,629 | 70 | 29 | **0.0012** |
| 31 all five | 1.659 | | | | | |

**The four footprint rules do not pay here.** They act on half a percent of the frame — the far
mountain and the slope's upper edge, never the bottom third — and their branches and live values
cost every pixel: each alone adds 0.0–0.1 ms, and together 0.44 ms, more than their sum (the
kernel's registers cross an occupancy step). The first form of the one-tile rule, a second copy of
the reads behind an early branch, cost 0.37 ms by itself; folding it into the three-tile loop with
the skipped reads predicated off brought it to 0.10. At 11520×2160 the vertical field is the same
over twice the rows, so every footprint is half as large and the rules act on fewer pixels still.

**Tiles by weight does**: 0.32 ms of 1.60, a fifth of the resolve, for 1.8% of the pixels moved,
all but 70 by one or two codes (the 20 over eight are the grass's sun glints, where a slope moved
by a hundredth turns a specular highlight), FLIP 0.0012 against the bound of 0.005. Together with
the footprint rules it loses most of that again (1.659), which is why it ships alone.

## 11520 × 2160

To be measured: base, tiles by weight and all five on the same flight (see the hand-off).

## The v25 walk

To be measured: R115 stage 2's walk (`walk.json`, records 6693–6993) on the v25 replay, base
against the shipped rule, at 3840×2160 and 11520×2160.
