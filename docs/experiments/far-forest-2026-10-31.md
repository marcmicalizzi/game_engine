# The far forest over the forest proof of concept's v23 region (2026-10-31)

Roadmap R111; [renderer](../subsystems/renderer.md#the-far-forest),
[ADR-0059](../adr/0059-the-far-forest-is-the-renderers-per-world-tile.md). What the far forest
builds over the proof of concept's v23 scene, and what it costs a frame, before (`--far-forest off`)
and after (`--far-forest on`, the default).

## How it was taken

`msvc-release` engine-view at a870435f, offscreen at 1920 × 1080, the proof of concept's scene as
read-only data (`world/v23/base.json`, its v22 material pack, its v22 derived-data cache copied to
scratch), with the owner's flags less his game module: `--terrain-tiles --terrain-far 6
--time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --rt-budget-mib 0`, and
`--raster hw --shadows csm` or `--raster rt --shadows rt`; `--benchmark` over each camera path (121
frames, the median quoted), `--wait-quiet 120`, each run under the GPU lock. RTX 5090. **The machine
was shared throughout**: no run found it quiet in its two minutes; other processes held 30–100% of
the CPU at a run's start and end (each row's `machine_state` is in the run's summary), and other
agents' GPU jobs ran between the runs, so a difference under about 0.3 ms is within the runs' own
spread (south-lake's traced pair went the other way by as much).

Cameras: the proof of concept's `ridge` and `south-lake` paths (static holds), and an **overview**
of this measurement's own — 450 m up at (−300, −300) looking at (900, 60, 900), across the region
— because `ridge` looks up a slope inside the ring and sees no far tile (below).

## What the load builds

| | |
|---|---:|
| authored instances streamed | 243,808 |
| taken by the far forest (4 m and more, coarsest form ≤ 512 triangles) | 59,178 |
| left out as under 4 m (the understory, saplings, the grass, litter and small deadwood) | 184,593 |
| left out as heavier than 512 triangles | 37 |
| scene meshes contributing | 251 |
| far tiles (one per occupied 64 m world tile) | 1,812 |
| triangles, finest level | 2,761,477 (46.7 a tree) |
| clusters = prefix pairs | 93,454 (1.58 a tree; 0.56% of 2^24) |
| the far forest's load stage, every tile read back from the cache | 1.2–5.4 s on the shared machine |

The prefix grows from 64,011 to 157,465 pairs (the fixed reservation, with the terrain slots, from
444,707 to 538,161). The cold build's time was not captured: its run's log was overwritten by the
second pass. The load's other stages are unchanged.

## What a frame draws and costs

| camera | path | far tiles drawn | far pairs | far triangles | GPU ms off → on (median) | shadow ms | visible pairs off → on | pixels that differ (¼ res) |
|---|---|---:|---:|---:|---|---|---|---:|
| ridge | hw / csm | 926 | 56,193 | 1,568,861 | 5.535 → 5.740 | 4.229 → 4.425 | 15,848 → 15,848 | 36 |
| ridge | rt / rt | 926 | 56,193 | 1,568,861 | 6.360 → 6.736 | — (rt 2.177 → 2.480) | 47,404 → 55,464 | 49 |
| south-lake | hw / csm | 771 | 29,916 | 974,670 | 9.626 → 9.791 | 7.852 → 8.010 | 185,766 → 187,669 | 3,824 |
| south-lake | rt / rt | 771 | 29,916 | 974,670 | 7.577 → 7.261 | — (rt 3.942 → 3.663) | 834,445 → 861,793 | 824 |
| overview | hw / csm | 1,136 | 48,322 | 1,556,704 | 9.310 → 9.642 | 7.325 → 7.643 | 239,286 → 249,650 | 45,841 |
| overview | rt / rt | 1,136 | 48,322 | 1,556,704 | 6.441 → 6.772 | — (rt 2.736 → 2.685) | 572,101 → 580,650 | 24,117 |

"Far tiles drawn" is what the last hand-off drew (the rest are parked under tiles the tail holds);
"far pairs" and "far triangles" are those tiles' clusters and finest triangles, before the cull.
Device memory (`gpu_memory_used_mib`, the device's, so others' allocations are in it) rose by
109–127 MiB with the rasterized paths and 173–194 MiB with the traced ones.

**Where the cost goes.** Under the cascaded maps nearly all of it is the shadow pass: the far forest
is a rigid caster (ADR-0059 decision 4) and the farthest cascades reach it — at `south-lake` the
third cascade's pairs rose 1,100,814 → 1,116,534 and the fourth's 1,902,034 → 1,930,468, at the
overview the fourth's 1,460,928 → 1,476,989 — while the camera's own raster rose 0.02 ms. An
untraced far forest in no cascade, once `place_instances` keeps the caster policy, would take most of
the shadow line's 0.16–0.32 ms back.

**What it draws.** From `ridge` the far forest is behind the slope the camera looks up: the same
visible pairs, 36 sampled pixels (a far tree's shadow in the last cascade). From `south-lake` the
mountain's lower flanks past the ring carry trees where they carried none. From the overview the
band past the ring — the far slope and the mountain's flank — is drawn where it was bare ground; it is
almost all of the 45,841 differing samples. The scattered small patches nearer
in, in every rasterized pair, are **not far tiles**: no tile there is withheld at `south-lake`, and
they are the masked cross-fade's dither, which is seeded by the instance slot (R91) — 1,812 more
prefix instances move every tail instance's slot, so a fading transition keeps other pixels of the
same trees.

**The pair cap.** The prefix's 93,454 extra pairs come off what the world's consumer can admit: at
the overview the tail held 15,597,279 pairs with 33 outer-ring tiles withheld, and 15,512,646 with
46 withheld. Those 46 tiles are now drawn by the far forest, where the 33 were drawn by nothing.
