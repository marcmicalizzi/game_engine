# The conifers' far form over the forest proof of concept's v25 world (2026-11-01)

Roadmap G120; [flora](../subsystems/flora.md#the-conifers-chain-g120) has the chain and
[the coverage under the mask](../subsystems/flora.md#the-coverage-under-the-mask) the bake;
[renderer](../subsystems/renderer.md#the-far-forest) and [R111's
measurement](far-forest-2026-10-31.md) the far forest that draws it. What the conifers' far form
was before their impostor chain and what it is after, at two cameras, and what the change costs
the content build.

## The defect

The owner, flying over v25: the far crowns look sparse, and the conifers' far forms visibly load in
while flying. The far forest draws each far tree's **coarsest form** — the star where the container
has one, the top of its DAG otherwise (R111) — and no conifer container had a star: the conifer
solver took no impostor chain at all (flora.md has the cause, with the proof of concept's `pine-0`
entry, which is today's exact key with no section 38). So every far pine, spruce, juniper and cedar
was the simplifier's top of a needle-card DAG: a trunk and a handful of cards.

## How it was taken

`msvc-release` engine-view, offscreen at 1920 × 1080, the run's own `scene.json`
(`reports/lab/20261101-115716-655`, read as data), its v22 material pack, and the derived-data cache
copied to scratch for this batch; the owner's flags less his game module and window: `--raster rt
--shadows rt --no-lights --time-of-day 15 --world-rate 1 --time-rate 0 --lod 1 --deform-pool-mib 64
--rt-budget-mib 0 --tunable renderer.flora.motion=true --tunable renderer.flora.wind_strength=0.7
--terrain-tiles --terrain-far 6 --far-forest on --frames 8 --capture`, each camera a path of one key
held. *Before* is the base, 83af2e3c (its binaries kept aside); *after* is G120's first commit,
c0c5e7b9 (the second moves only which hash the chain's version is in: the conifers' containers are
the same bytes under the same keys, and the deadwood's keys are the base's again). All four runs went
under one GPU lock, after 15 minutes in its queue behind four other agents' measurements; the
machine was shared, and **no timing is quoted** — the numbers below are counts, which the machine
does not move.

Cameras: R111's **overview** — 450 m up at (−300, −300) looking at (900, 60, 900) — and the owner's
**frame 4320**: 568 m up at (182.5, 1927.7) on the mountain's top, yaw −0.2°, pitch −10.4°, looking
back over the whole region (the climb of frames 3760–3960 ends there).

## What the far forest builds

| | before | after |
|---|---:|---:|
| far tiles, trees (unchanged: the same 4 m floor and the same instances) | 1,812, 59,178 | 1,812, 59,178 |
| triangles, finest level | 2,761,477 | 1,917,979 |
| triangles a tile, a tree | 1,524, 46.7 | 1,058, 32.4 |
| clusters = prefix pairs | 93,454 | 95,159 |
| left out as heavier than 512 triangles | 37 | 37 |
| far tiles drawn, their triangles: overview | 1,184, 1,630,456 | 1,091, 958,584 |
| far tiles drawn, their triangles: frame 4320 | 867, 1,473,286 | 867, 1,087,835 |

A conifer's far form is now its star — 16 planes, 32 triangles, the broadleaves' — where it was up
to 512 triangles of decimated needle cards, so the far forest is 31% lighter in triangles, for 1.8% more pairs.

**The ring.** The conifers' near forms changed too: inside the ring a conifer's DAG now coarsens into
branch cards and the star instead of decimated needles, so the same instances take fewer pairs. At
the overview the tail held 15,184,961 pairs with 94 outer-ring tiles withheld by the pair cap, and
holds 13,389,343 with none withheld (outer-ring tiles 353 → 447); at frame 4320, 4,699,471 →
3,676,208. The authored forms' clusters went from 198,672 to 190,387.

## What the pictures show

- **Overview, before**: the far slope and the mountain's flank past the ring are thousands of thin
  grey-brown vertical strokes on bare ground — the conifers' trunks with a few cards — and the same
  strokes stand on the slope inside the ring to the lake's right; only the broadleaves at the left
  read as crowns.
- **Overview, after**: the slope and the flank up to the snow line are a dark green conifer forest,
  each tree a dense crown with a ragged edge, as dark as the broadleaves' crowns and greener than the
  ground between them; the strokes are gone, near and far, and no boundary between the ring and the band past it can be picked out at
  this size.
- **Frame 4320, before**: the broadleaf forest round the lake reads as canopy; the conifer belts — the
  strip between the forest and the coast, the band below the lake and the region's left edge — are grey
  stubble.
- **Frame 4320, after**: those belts are green crowns; the strip along the coast reads as forest to
  the beach, and the left edge has a tree line where it had a fringe of sticks.

**The hand-over's pop** — a tile's switch between the ring's tail and the far forest, counted in
changed pixels across a flight — was not measured: it needs a flight crossing a ring boundary with
frame pairs captured across the switch, and the batch's GPU queue left no room for it. Both sides of
that switch are now the same star where the ring draws a conifer at its coarsest, which is the case
the switch distance gives at the ring's outer radius.

## What it cost the content build

**The conifers' containers.** The world's recipes name 20 conifer forms (cedar 3, juniper 4, pine 4,
spruce 6, `conifer` 3); each is one new entry, keyed by `k_conifer_chain_version`, its two levels'
atlases three images each (120 textures). Cooked one at a time by `engine-content flora` in
`msvc-release` with the pack, on a machine whose CPU other agents held at 100% (so these are upper
bounds): 2.4 s (`pine-sapling`) to 36.6 s (`spruce-old`), 271 s in all, `pine-0` 12.6 s; for scale,
the same run's `beech-0` took 4.9 s and `inherited-birch-0` 17.6 s, whose stars are the same bake.
In `msvc-debug`, `pine-0`'s container went from 0.97 s to 42 s. Each is one bake and up to four
measures of its two levels. The far forest then rebuilds every tile holding a conifer: 1,711 of
1,812 (16.2 s at load, 182 MB); the other 101 kept their keys.

**What the after run cooked, and why it was more than that.** The after run above was taken with the
chain's version first mixed into `flora_source_hash` itself, which the deadwood's key also takes
(a dead tree is the flora solve of its species, without the chain): its load cooked 589 containers
— the 20 conifer forms and **569 deadwood entries** (415 spruce, 152 pine, 2 juniper; 2.2 GB in all)
whose bytes the chain does not touch — in 111 s more of mesh loading than the before run (123.4 s
against 12.5 s, 37 threads). The second commit moves the version to the container's hash
(`flora_source_hash_with` and the generator's plain hash) and leaves the tree's alone, so the
deadwood keys are the base's again and the conifers' are the same numbers the after run cooked
(`spruce-0`'s is `2c1e28016d51bae4` either way). The 569 deadwood entries are orphans in the batch's
scratch copy of the cache. The proof of concept's own cache (`world/v22/ddc`) has none of the new
entries: its author's next run cooks the 20 conifer forms and the far tiles, once.
## The coverage the levels draw

`engine-content flora` over `pine-0` and `spruce-0` with the pack, the coverage bake's own
comparison (`LevelMeasure::kept`: the cards' coverage over the finest geometry's over the ring of
sixteen views), plain mask against coverage kept and fitted:

| level | `pine-0`: plain → fitted (scale) | `spruce-0`: plain → fitted (scale) |
|---|---|---|
| branch cards | 1.150 → 0.999 (215/256) | 1.025 → 0.944 (256/256) |
| the star | 1.621 → 1.024 (110/256) | 1.341 → 1.079 (139/256) |

A plain star draws a pine's needles 1.6 times over: each of its four directions carries every
needle, and a sparse crown is seen through several planes at once. Fitted, the far pine draws the
needle mass its near form does, so the switch to the star neither thins nor thickens the crown.
