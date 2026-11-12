# channels (domain, capability)

**Purpose.** Channels from the ground's drainage (roadmap W86; the owner, 2026-11-06: "running water —
rivers, downhill rivers, waterfalls"). The placement generator `channels` routes the flow over a
lattice of the ground it is handed, begins channels where contributing area and slope say a channel
begins, follows them downstream, and gives each point a bankfull width and depth. Since W89 the
channel is **cut into the ground** below its floodplain: `engine-content channels --cut` writes the
terrain's `cuts` beside the water's description and the tile set is built with them
([tileset](tileset.md#channels-cut-into-the-ground); [The cut](#the-cut)). Since W93 the bed, the
bars and the flood bed are that cut's own form, and since W97 what the generator places is the
bed's sediment: the clasts above the drawn size on the bed, the bars, the banks' feet and the flood
bed, the steps' boulders among them, one merged mesh a tile ([Bed sediment](#bed-sediment),
[What it places](#what-it-places)); W86 dressed the trough with a gravel slab,
bar lenses and bank berms, none of which is laid any more. The same network is the description the flowing water
draws: `engine-content channels` writes it as `engine.scene.WaterChannel` blocks. The published
models every number comes from, and each constant's range, are on the grounding page,
[experiments/channels.md](../experiments/channels.md).

**Why it is shaped this way.** The proof of concept's erosion model already cut real drainage into
its ground (v31: 688 heads, 5–7 km of channel per km²), and its v34 drew dry channels as paint, which
cannot give a bed relief, bars or banks (the POC's engine-needs page, "After the owner's v32 walk" §3).
Cutting the ground again would fight the erosion model and every consumer of its heights, so the
generator never edits the ground: it finds the drainage the ground has and lays meshes on it. It is a
domain-layer capability beside the deposits generator it is modelled on (`domain/rocks`, G128),
because it needs nothing of the renderer: the ground is the `scene_gen::Ground` it is handed, the
meshes are recipes the mesh-generator registry builds through the derived-data cache, and the scene
readers place them as they place any generator's instances.

**Owned data.** The network (`ChannelNetwork`: the lattice's heights, the filled heights, D8
receivers, contributing area, channel cells, heads and links with their lines) is planned once,
kept as a derived file (`<ddc>/channels/plan-<key>.channel-plan`; [One plan](#one-plan)) and is the
source of truth for every cut, every piece and every `WaterChannel` block made from it. The
pieces' recipes are kept in memory by the opened entry and handed with their tiles (W97b; [A
tile's bed, laid when it activates](#a-tiles-bed-laid-when-it-activates)); no recipe file is
written, and a built piece lives in the derived-data cache under its recipe's key.

## The network

`plan_channels(layer, ground, seed, jobs, out, error)` is **the one function** both the generator and
the subcommand call:

1. **The lattice.** The region (`region_min/max`, whole spacings from the world's origin) at
   `spacing_m` (4 m), at most `max_samples` points. Heights are asked at whole millimetres and kept as
   integer micrometres; a job a row of rows when a job system is offered.
2. **The fill.** Priority flood from the region's edge with a one-micrometre epsilon (Barnes et al.
   2014), heap ordered by (height, index): every interior cell gets a strictly falling path to the
   edge, and the order is total.
3. **The routing.** D8 over the filled heights, ties to the first direction; the region's edge is the
   outlet. Contributing area summed after sorting by filled height.
4. **Heads.** A cell is a channel where `A ≥ min_head_area_m2` and `A · S^α ≥ head_threshold_m2`,
   `S` the ground's (unfilled) drop to its receiver over the run; every cell below a channel is one.
   A head is a channel cell with no channel donor.
5. **Links.** From every head and every junction (two or more channel donors) down to the next
   junction or the region's edge, in order of their first cell.
6. **Lines.** A link's cell centres low-passed (σ = max(6 m, 2 w)), resampled every `step_m`, bent by
   the meander rule where the link's slope allows and held inside the trough, and given per point the
   bankfull width and depth from its area, its ground height and the bed's fall per metre over a
   pool-to-pool length.

**Determinism.** The heights are integers, the fill and the area sort are total orders over
(height, index), the lattice rows are the only work handed to jobs and each writes only its own
heights, and every transcendental is `core/math/deterministic.h`'s, so the network is the same bytes at
any thread count, any tile order and on every toolchain. `channels: the same network at one job and
six` holds the two thread counts to the byte.

## One plan

(2026-11-10, roadmap W93, continued; the POC's solver needs, 2.1 item 9: "the network planned
once, not every load".) **The network is one object, made once from the ground before its cuts
and kept as a derived file**, and everything that needs a channel's line reads it: the cut
(`terrain_cuts_json`), the water's description (`water_channels_json`), through that description
the placements' corridor ([scene_gen](scene_gen.md#exclusion)), and what the generator still
places (`place_pieces`). Until this the subcommand planned the network over the uncut ground and
wrote the cut and the water from it, the tile set was built with the cut, and then the placement
generator **planned again at every load over the cut ground** and got another line: near the
cut's, not on it, since the fill, the low-pass, the meander rule and the trough test all ran again
over ground the first plan had lowered. Everything laid from the second line lay beside the
first's: the bed slabs up the banks and across the flood bed as plates (item 8), and it cost the
load the whole plan (24 to 32 s on v35).

- **The file.** `<derived-data root>/channels/plan-<key>.channel-plan`, beside the recipes the
  generator already writes there (`derived_dir`: the host's `--ddc`, else the repository's
  `ddc/` found from the scene's directory, else `ddc/` beside the scene). It holds the links
  with every per-point array, the heads and the counts, as integers and IEEE bits, little-endian,
  closed by a hash of the whole (`plan_bytes`, `read_plan`); the lattice's own arrays (heights,
  fill, receivers, areas) are not in it, since no reader of a plan asks them (the one that did,
  the confluences' order in the water's description, now reads each link's `area_end`). A file
  that is short, of another `k_channels_version`, changed, or whose arrays disagree is refused
  and never read as a plan.
- **The key is what the plan was made from**: the layer's canonical JSON with `plan` cleared,
  `k_channels_version`, the world's seed, the ground's height at every point of the layer's
  lattice to the micrometre, and whether a water body covers each (`ensure_plan`). The lattice is
  what the routing reads; the line's own tests (the trough, the banks, the flood bed's walk) ask
  the ground between its points, so two grounds that agree at every lattice point and differ
  between them would share a key, which for a ground sampled every 4 m or finer is a ground that
  is the same one.
- **An entry on uncut ground names no plan.** `ensure_plan` samples the lattice, keys it, reads
  the file on a hit and plans and writes it on a miss: the first load of a world plans, every
  load after it reads.
- **An entry on cut ground names its plan** (`ChannelLayer.plan`, the 16 hexadecimal digits
  `engine-content channels` prints and writes under `plans`, pasted into the entry's `params`
  beside the `cuts` and the `water_channels`). The cut ground's heights are not what the plan
  was made from, so the key cannot be made again from them, and **the generator never plans over
  a ground its own cuts lowered**: it reads the named file, whatever the ground, and checks that
  the plan records the entry's own layer and the world's seed. A named plan that is not in the
  directory, or that was made from another layer or seed, is refused with the command that makes
  it. That is a load that stops on a machine whose derived data was thrown away, where the tile
  set and the masks would be built again: they are rebuilt from the scene's heightfield, and the
  plan needs the ground before the cuts, which a placement generator is not handed
  (`scene_gen::Context` carries the ground it stands on and nothing of the terrain entry). An
  entry that carries pasted cuts and names no plan plans over the cut ground as every entry did
  before, since the generator cannot tell a cut ground from another; the subcommand's output is
  what says to paste the key.
- **The subcommand** clears the entry's `plan` and the terrain's `cuts`, opens the ground before
  its cuts and asks `ensure_plan`, so a second run over the same ground and layer plans nothing;
  its summary carries `plans` (the keys), `planned` (how many this run made) and `plan_s`.
- **`plans_made()`** counts the planner's runs in a process; the tests hold a second ask, a load
  that names its plan and the generator's `open` on cut ground to none.

**Measured** (2026-11-10, `msvc-release`, the machine at 24 to 25 % CPU with other agents
building, so upper bounds; `plan_s` is the subcommand's own clock round `ensure_plan`, one thread):

| Ground | Lattice | Line | Plan made | Plan read, keyed from the ground | The file |
|---|---|---|---|---|---|
| W93's scratch valley (0.098 km², 7 links) | 6,305 points | 482 m | 0.023 s | 0.013 s | 68 KB |
| a 4.1 km ridged heightfield at 2 m (16.8 km², 305 links, 167 heads) | 1,050,625 points | 63.6 km | 1.64 s | 0.49 to 0.54 s | 9.2 MB |

A load before this made the plan every time (the first column of times, and on v35 24 to 32 s,
where every ground query goes through a 2.6 GB tile set). A load now reads the file: keyed from
the ground it costs the lattice's heights and a hash (the second column, most of it the 1.05
million heights), and **an entry that names its plan asks the ground nothing** and reads the
file's bytes, about 145 bytes a metre of line at the default 0.5 m step. By the plan's own
scaling on the second ground (18 ms a kilometre of line beyond the lattice) a world of 10 km of
stream plans in about 0.3 s and its plan is a 1.5 MB file; v35's figure is 20 to 100 times that
a kilometre, which is the tile set's cost a query and not the planner's, and it is the cost a
named plan no longer pays at all. Whole-process, the subcommand on the scratch valley is 0.78 s
before and after with the plan cached (the tile set's open, the pieces and the summary's
measurements, not the plan), and on the larger ground 6.2 s before, 5.9 s making the plan and
4.7 s reading it.

**Still pasted.** The second half of item 9, "the cut applied by the generator, not pasted from a
subcommand", is not built: the terrain's `cuts`, the scene's `water_channels`, the entry's `plan`
and, where the layer names its bed's material, the masks' `mask_regions` are four things the
author pastes from one output. Closing it means the ground is made in
two steps (the ground before the cuts, the plan, the cut ground) and the scene reader takes the
water from the plan, which are the tile provider's and the scene reader's to change, with a
registration point a placement generator contributes a ground edit through.

## What it places

**What is an object of its own, and nothing else** (W93, continued, 2026-11-10): the boulders of a
step-pool reach's steps and of a cascade's clusters. **The bed and the bars are the ground's.**
W86 laid a gravel slab on the bed and a lens of finer sediment on it as bars; W89 cut the channel
into the ground and narrowed the slab to the cut's flat bed; W93 gave the cut a point bar and a
flood bed of its own and laid skins on them. Each of those was a mesh standing on a ground that
already had the shape, and each showed as what it was: plates with walls wherever its line parted
from the cut's (the POC's item 8, and the pale plates on the flood bed and the sliver in a bank on
W93's scratch scene), a smooth slab where a bar should be sediment (item 6). With
[one plan](#one-plan) there is no second line to lay them from, and with the cut's own section
there is no form left for them to add, so they are gone: the generator places no bed and no bar on
any reach, and the test "nothing is laid on the bed" holds a pool-riffle and a plane-bed valley to
no piece at all. What a slab carried that the ground does not yet is the bed's **material** (a
gravel's texture and its centimetre relief), which is the terrain's to draw through its masks
([The flood bed](#the-flood-bed)), and the riffle's rise, which is the cut's section now.

**Since W97 the bed carries its sediment** ([Bed sediment](#bed-sediment)): every clast from
`clast_min_mm` (64 mm) up that the plan's sediment puts on the bed, the bars, the banks' feet and
the flood bed, and the steps' and cascades' boulders among them, **one merged piece a world tile**
([The clasts](#the-clasts)). What is finer than the drawn size is the bed's material, painted
through the terrain's masks. A tributary's clasts stop at its trunk's banks (`keep`), and a reach
decides its boulders ([Reaches](#reaches)): a row across the bed at each step-pool crest and one
each side half in the bank; one to three at each of a cascade's clusters and, at about half of
them, one against a bank. The bed they stand across is the cut's flat bed between its banks' toes,
a quarter of the width where the section is a V.

- **No banks** since W89. W86 laid a berm each side that rose *above* the ground, so every stream
  read as a canal between levees. The banks are the cut's own faces ([The cut](#the-cut)).
- **No bed slab and no bar skin** since W93's continuation, as above. `ChannelLayer` version 7 has
  none of the fields that shaped them (`riffle_rise`, `shields_bankfull`, `bar_rise`,
  `bar_width_widths`, `bar_length_widths`, `bank_min_width_m`, `bank_height`, `bed`, `bars`,
  `banks`), and a scene that names one is refused by the reader. Version 10 drops `piece_m` (a
  piece is a tile's clasts), and W93's 7 by 7 domes are gone with `ChannelSlab`.

### Materials

**Since W97c each rock samples its own set** through the tile contract's textures (R147: a tile's
mesh arrives with its textures, [scene_gen](scene_gen.md#a-tiles-meshes)): the piece is handed with
its tile, its container names the sets' images and records them
(`texture::fill_cluster_texture_records`), and the host builds and admits them before the mesh.
Each stone is one of four lithologies in two tones ([What a stone is made of](#what-a-stone-is-made-of));
each rock takes its own role, the tone a factor on the set. `boulders.role`, named, is the one set
every stone samples; `boulders.tint` multiplies the set beside the tone; `tile_m` (1.2 m by
default) is the repeat over a stone's surface, whose UVs are its vertices' x and z plus half their
height, offset by its seed; `sampling` is the set's placement. `boulders.color` and `roughness`
are what a stone whose role the pack lacks draws (absent, its rock's dry albedo). W97b drew every
stone as those constants, because a tile's mesh had no textures yet; before W97b a pack's
`rock/boulder`, else `rock/granite`, else `rock/sandstone` set was applied to all of them.
**The bed's and the bars' materials are the terrain's**: `bed_material` paints the cut's bed and,
with `bar_material` (W97), the flood bed each side is a region of its own in the bars' material, so
a bar reads apart from the bed. **A palette layer names its images outright**
(`TerrainMaterialLayer.base_color` and the rest): the generated sets `gravel/bed` and `sand/bar`
(G148, `engine-content matgen`) reach the bed only when the scene's `gravel` and `sand` layers name
their files. W97's first pictures drew flat paint between the stones for that reason: their scene's
layers named tints and no images, so nothing of the generated pack reached the ground; the same
scene with the layers naming the sets draws the bed as a cobble texture between the drawn clasts
(W97b's pictures). The same scene drew its whole valley, meadow and all, in the `sand` layer
until its `masks.priors` named palette layers (`soil` `meadow`, `scree` `gravel`, `bedrock` and
`snow` `soil`): with the priors left at their defaults, three of which the palette does not have,
nothing refused the scene and the ground beyond the bed took the bars' sand. That is the terrain
masks' to look at (a row this leaves). Both are the scene's to say, not the generator's; a palette layer that names a
role and finds it in the pack is the terrain's to add (a row this leaves).

## Bed sediment

W97 (2026-11-10; the proof of concept's solver needs 2.1, items 3, 6 and 8): the photographs are
cobbles and boulders from scarp to thalweg, hundreds in a frame; W93 drew bare ground with a few
domes. **Each point of each link carries its bed's surface sediment in the plan**
(`ChannelLink::sediment`, a `ChannelSediment` a point; the plan file's format 2), so the clasts,
the water (row S101 reads `d50_m` and `d84_m` as the bed's roughness) and any other reader get the
same answer, and the plan's key moves with it. Every number's source and how it was read is on the
grounding page ([Bed sediment](../experiments/channels.md#bed-sediment)).

- **The median** is what the bankfull flow just moves: `D50 = h S / (1.65 tau*)` with
  `tau* = 0.15 S^0.25` (Lamb, Dietrich & Venditti 2008), h the point's bankfull mean depth and S the
  bed's fall low-passed over five widths (at least 10 m), so a lattice step makes no grain-size step.
  It is capped so that d90 stays under the reach's relative roughness (Montgomery & Buffington
  1997: 0.3 of the depth on pool-riffle, 0.8 on plane-bed and step-pool, 1 steeper), and a
  dune-ripple bed is sand (2 mm at most).
- **The distribution** is lognormal in size (Bunte & Abt 2001) with sigma in phi units by reach:
  0.6 dune-ripple, 1.2 pool-riffle, 1.5 plane-bed, 1.75 steeper; `d84 = d50 2^sigma`,
  `d16 = d50 / 2^sigma`.
- **Roundness** is `1 - exp(-x / rounding_m)` (1 km), x the channel length upstream of the point
  along the network (the longest path from a head), clamped to 0.05–0.95 (Domokos et al. 2014: a
  pebble's edges round within about a kilometre of transport). A colluvial gully's clasts are
  angular; a lowland river's rounded.
- **Cover**: the share of the bed's surface under clasts of `clast_min_mm` or larger, the
  lognormal's tail above it (a pebble count samples by area: Wolman 1954); a bedrock reach keeps
  0.3 of it.
- **A bar's run**: where the section is a bend's to a quarter of its asymmetry or more, all on one
  side, `bar` runs from 0 at the run's head to 1 at its tail; elsewhere -1.

The normal's tail is a logistic approximation through the engine's exponential, so the plan is
the same bits on every toolchain; the size test holds what it draws against the true normal.

### The clasts

`place_pieces` lays every clast from the plan alone, **station by station** (each point of a
link's line, a strip half its neighbours' spacing either way), each strip **in cells** about 0.5 m
across the section from the right edge to the left (the flood bed's reach, or the bank tops), and
in each cell **by size bins** a quarter of a psi unit wide from the drawn size to 2.5 sigma above
the median: a bin's expected count is its share of the cell's area divided by the plan area a
clast of its size covers (pi/4 a b, a = b / 0.7), its count that rounded by a draw, every draw
seeded by the link, the station and the cell. So a clast is the same whichever tile came first,
whatever the tile size and from whatever start, and the counts follow the distribution
(`channels: the clasts' sizes follow the sediment's distribution over several seeds`: above-D84
share of the drawn cover within 0.01 of the lognormal's on four seeds). Where on the section:

| Zone | Where | Its grain sizes |
|---|---|---|
| 0 the bed | the cut's flat bed between its toes | the point's |
| 1 a bar | a bend's inner bank, to 0.9 of its height, and its flat bed's inner side | 1.2 of the median at the bar's head to 0.8 at its tail (Rice & Church 2010) |
| 2 a bank's foot | the lower half of an outer bank, or of a riffle's banks | the point's |
| 3 the flood bed | from 0.25 m past the channel's lip to 0.5 m short of the scarp | 0.8 of the median |
| 4 a step's keystone | a row across each step-pool crest, one each side half in the bank | D84 to D97.7 of the reach, 0.2 to 2 m |
| 5 a cascade's cluster | one to three across the bed, about half with one against a bank | the same |

None on a bank's lip, none on a scarp, none where the bed lies more than 1.6 mean depths under the
bank's top (the deepest of a bend's pool, where the water is twice the mean deep: the pool's finer
sediment and its depth hide the bed there, a design choice), none at a knickpoint, none under a
water body (a link ends at its shore).

**A clast** (`Clast`, `k_clast_ints` integers in a recipe): its b axis drawn in its bin, a = b /
(0.7 ± 0.15), c = b (0.6 ± 0.2); its a axis across the flow within about 20 degrees, its a-b plane
dipping upstream 10 to 30 degrees (imbrication); **protruding** over the ground at its centre by
0.36 of b on average (0.15 to 0.57), never more than 0.9 of its own height, and its lowest point at
least 10 mm under the lowest ground beneath its footprint, **so none floats and every one is partly
buried** (`channels: a clast is partly buried and none floats`). Every number is rounded to the
recipe's millimetres and ten-thousandths before the burial is decided, so the test holds the
recipe, not a float behind it.

**One merged piece a tile, or several**: the clasts are bucketed by the world tile their centre
stands in, the tiles in z then x order (their ids 1, 2, …), each tile's clasts in the order laid,
and **a tile's run is cut into pieces of at most 1,800 clasts and 120,000 drawn triangles before
burial** (W97c), so no piece outgrows the 4,096 clusters a mesh that arrives with a tile may have
(`renderer.meshes.max_clusters`): a stone is a cluster or more of its own at the finest level
(2,290 clasts measured 3,859 clusters, the DAG included), and the scratch valley's busiest 32 m
tile, one piece of 5,852 clusters, was refused by every streamed host and drew no stones at all
(W97c's world run, 14 of its pieces). The cut is in the order laid, so a tile's pieces are the same
whichever tile came first. A clast whose footprint crosses a tile's border is in one tile's
piece, so the bed is continuous. The
recipe (`engine.channels.ChannelPiece` version 2, `part` `Clasts`) is decided at `open` and handed
with its tile ([A tile's bed, laid when it activates](#a-tiles-bed-laid-when-it-activates)); its
mesh is built only when the host builds what the tile hands, through the mesh generator `channel`
and the derived-data cache (`pieces_solved()` holds `open` to none). The mesh: each clast an
ellipsoid on a cube-sphere grid by its b axis (2 quads a face edge
under 128 mm, 48 triangles; 3 under 0.5 m, 108; 4 under 1 m, 192; else 6, 432), cut by ten facets
at 0.78 to 0.92 of its reach and blended back toward the ellipsoid by its roundness, so an angular
clast has faces and a rounded one none; **the triangles wholly under the lowest ground beneath it
are left out**, so each clast is open only underground (`channels: the placed pieces ...`). Since
W97b the triangles are grouped by the clast's material, one cluster LOD DAG a material, merged, so
a cluster is one rock at every level; the merged DAG is the tile's far form.

### What a stone is made of

(W97b with G165, 2026-11-11.) A gravel bed carries a handful of lithologies from its catchment,
the resistant ones enriched downstream by abrasion (the grounding page, [The stones'
rocks](../experiments/channels.md#the-stones-rocks)). A clast is one of four, drawn from its own
seed by share, so a recipe needs no new field and the same recipe is the same stones on every
host; and one of two tones, 0.85 and 1.15. Each of the eight is a material of the piece's
container. **Each rock samples its own set of the pack** (W97c), and **the tone is a factor on the
set**, not a second set, so the eight materials share four sets' images and a set is uploaded once
whatever the tiles. Where the pack has no set for a rock's role, that rock's two materials draw
its dry albedo and roughness, untextured, and `resolve_set` says once which role was missing:

| Rock | Its role | Dry albedo, linear (no set) | Roughness (no set) | Share |
|---|---|---|---|---:|
| granite (grey granite, gneiss) | `rock/granite` | 0.19, 0.18, 0.17 | 0.8 | 0.45 |
| pale (quartzite, limestone, chalk) | `rock/limestone` | 0.27, 0.26, 0.24 | 0.75 | 0.25 |
| basalt (and greywacke) | `rock/basalt` | 0.12, 0.12, 0.125 | 0.75 | 0.15 |
| sandstone | `rock/sandstone` | 0.16, 0.12, 0.085 | 0.85 | 0.15 |

All four roles are among the generated sets (`engine-content matgen`, [matgen](matgen.md)), so a
checkout that has run it draws every stone textured; the engine's fetched pack holds
`rock/sandstone` alone and the generated sets stand behind it. The pale rock takes the limestone set
because it is the only pale rock with one (a choice; a quartzite set is a row this leaves). The albedos are a measured table's (grey granite 0.18, basalt 0.12,
sandstone 0.12, chalk 0.25); the hues, the tones and the shares are design choices, the shares
those of `gravel/bed`'s generated set (G148) so the stones and the bed's texture between them agree.
**Not built**: the mix from the scene's formation (`Context::formation`) or from the catchment's
rocks upstream, the abrasion that enriches the hard rocks downstream, and wet stones at and under
the waterline (`rock/granite_wet` exists; which stones are wet is the water's surface, closed to
this row). `channels: the stones are of four rocks in their shares, each its own set` holds the
container with no pack (no image, eight materials, every cluster one of them, each mean albedo in
0.08 to 0.32), with a pack lacking `rock/basalt` (each rock's two materials name its own set's
images, the tone the factor, basalt's two on their constants, a texture record an image) and with
`boulders.role` named (every material that set), and the shares of the triangles over two valleys
within 0.05 of the table's (measured 0.456, 0.250, 0.147, 0.147).

### The stray triangle on the riffle (T98)

W97's first pictures showed a flat triangle about 13 cm across in the open water of the scratch
valley's riffle (`riffle-up`, about pixel 680, 823 of 1920 by 1080): dark while the stones were
constant colours, pale cream once they wore their rocks' sets. W97b read the id buffer there (the
trunk's ribbon) as "no bed piece above the water"; W97c read the colour as "a bed piece's
triangle, a coarse level suspected". **Both were half right, and the cause is the water's**
(T98, 2026-11-12, on 5b044794, one camera, `--raster hw --shadows rt`):

- **What the picture holds there is the water.** Every pixel within 4 of the marker carries one
  ribbon triangle (`water/valley link 4`, its cluster 93, triangle 19). The scene less its
  `water_channels` draws no pale shape there at all: the straight view meets the cut ground and,
  a few pixels off, a stone of a bed piece at level 1. And with water and `--shadows csm`, which
  traces no ray, the shape is gone while the ids stay the same.
- **So it is what the water's refraction ray meets** (`water_refract.slang`; water.md, "What is
  under the water"): a bed stone, standing in about 0.30 m of water (the ribbon's depth there; its
  surface 9.50 m) about 0.22 m over its ground. The refraction counts a hit as the ground when it
  lies within 8 cm plus 0.5 % of the eye's distance of the depth map's ground, about 10 cm here, so
  only the stone's top stands "in the water": a cap with a hard edge where the stone crosses that
  margin, shaded almost untinted at a few centimetres' depth, round which every other stone of the
  bed is the terrain material's pebbles. That is the pale triangle.
- **No level of a piece bridges stones.** The suspicion (meshoptimizer's sloppy fallback welds on a
  grid, and could join two stones) is held to "no" by a case: over three seeds, the busiest tile's
  piece has no coarse triangle with corners on two stones (test: "no coarse level of a piece joins
  two stones, over seeds", 8.5 s in `msvc-debug`). Beside it, printed: about 5 % of a sample of
  coarse triangles stand farther than their cluster's error from the finest form, the worst 1.4
  times it (60 to 97 mm at levels 7 to 9): the simplifier's error is a quadric estimate, not a
  bound, and nothing a viewer resolves at those levels' distances.

Nothing here changes what this module builds (`k_channels_version` stays 9). What the water's
refraction should do with a stone that crosses its margin, and with the stones under it, is the
water's: a request, with the numbers above.

### A tile's bed, laid when it activates

(W97b, 2026-11-11; R142's contract, [scene_gen](scene_gen.md#a-tiles-meshes), ADR-0061.) Until
W97b `open` wrote every tile's recipe into the derived-data root and listed every piece in its
`meshes`, so a host read each one at scene read and built it there: 2.9 s for the busiest tile of
the test valley, at load, before the first frame. Now:

- **`open`** reads the plan, lays every clast (the plan, the sediment, the draws: what `place_pieces`
  does) and keeps, a piece, its recipe's JSON, its placement and its tile. It builds nothing and
  writes nothing but the plan file.
- **No resident meshes**: the entry has no `meshes` entry point, and its descriptor says
  `tile_meshes`, so a host makes room for meshes that arrive with tiles.
- **`tile`** hands the tile's piece as `PlacementMesh{path, name, 0, recipe, reach}` with its
  placement indexing it: `path` is where the file would be (`<ddc>/channels/channel-<hash>.channel.json`,
  which names it), `recipe` its bytes, `reach` the farthest clast centre from the tile's corner plus
  its a axis (`piece_reach_m`). The host builds it through the mesh generator `channel` and the
  derived-data cache, keyed by the recipe, off the frame's thread ([world](world.md#a-tiles-meshes-residency-by-reference)).
- **`expand`** lists every piece with its recipe, so a scene read whole is the same scene.

**What a walker sees while a tile's bed builds** (W97c, read from the world's placements consumer,
[world](world.md#a-tiles-meshes-residency-by-reference)): the tile goes on drawing what it drew
until a commit has its piece and the piece's textures in, so **a tile new to the ring draws no
stones** — the ground's bed and bar materials alone — for the build's seconds and then the stones
in one commit, and a tile already held draws its stones throughout, because the entry's
`representation` is one in every ring and a ring change builds nothing again. A tile enters the
ring at the outermost ring's radius, hundreds of metres out, so a walker (1.4 m/s: about 4 m in
the busiest tile's 2.7 s) never sees a near tile without its stones; a camera that jumps or
starts somewhere does not either, because a whole commit (a restart, an offscreen capture's
fill) waits for every build. A fast flight over a stream is where the gap can show: at 100 m/s a
tile could be about 270 m nearer before its stones arrive. No far form stands in for the stones
meanwhile (the bed's material is all there is), which is the far form this page leaves.
**Measured by a run** (W97c, `engine-view --world` over W93's scratch valley, its nine views,
`msvc-release` at d653871b, the machine busy): the first fill's whole commit spent 15.5 s on the
placements before the first frame, and no later update built a piece, since the default rings
(1.5, 8 and 24 tiles of 32 m) held every tile of the 384 m valley from the start. **And 14 pieces
were refused** — one of 5,852 clusters against the 4,096 a tile's mesh may have — so those tiles
drew no stones at all from any view, where the scene read whole drew them: the defect the cut
into pieces of 1,800 clasts (above) closes. The same run at e6947eee refused none: 25 pieces
built and admitted, textured, in a first fill of 10.1 s, and the riffle drew its bars' stones
streamed as it does read whole.

What the contract carries today: the mesh, its materials and their textures (since R147 and W97c:
the four rocks' sets, built and admitted before the mesh, a set shared by every tile that names
it), its collision proxy (a mesh a tile hands is walked on), and its cluster DAG as the far form.
What it does not, and the bed needs: a footprint other than a disc
of `reach` about the tile's corner (a later entry is handed a disc as wide as the tile for a piece
that covers a thin strip of it); and a far form past the DAG (the bed's material darkened and
roughened by cover). `channels: whole expansion equals any tile order, with stable ids` holds a
tile's hand-over to the whole read's, recipe for recipe, and that no file is written;
`channels: a tile's bed is the same bytes whichever tile activates first, any jobs` holds two
entries, one planned with six workers and one with none, to the same recipe at every tile in
opposite orders.

### What a tile costs

`channels: what a tile of bed costs (measured, printed)`, msvc-release, 2026-11-10 22:45, the
machine at 100 % CPU (`Win32_Processor.LoadPercentage` before and after: other agents' builds), so
every time is an upper bound, one run. The gentle test valley drained at W93's scratch scene's
hydraulic geometry (a trunk 3 to 5 m wide, its D50 49 mm, D84 112 mm, 36 % of its bed under clasts
of 64 mm or more):

| Measured | |
|---|---:|
| Clasts, the region (14 tiles of 64 m) | 66,253 over 2,443 m² of bed: 27 a m² |
| Laying them all (what `open` adds to the plan) | 98 ms |
| The busiest tile's clasts | 12,251 over about 452 m² (its share of the clasts times the bed's area: an estimate) |
| Its finest form | 610,781 triangles: 1,352 a m² |
| Its coarsest (the DAG's roots) | 17 triangles: the far form is the bed's material, the clasts gone |
| Its activation (solve, cluster DAG) | 2.9 s (W97, at scene read); 4.1 s (W97b, `msvc-release`, 2026-11-11 13:40, the machine at 100 % CPU: eight DAGs, one a rock, at the tile's activation off the frame's thread) |
| Its coarsest form since W97b | 275 triangles: the eight DAGs' roots |
| Its activation, the bench (W97c) | `channels.tile_activation`, `msvc-release`, 2026-11-11 20:49, the tile still one piece: median 2.74 s, min 2.55 s, max 2.88 s over three runs after a warm-up, 12,251 clasts. **Not quiet**: `--wait-quiet=1800` waited the half hour out, the machine at 62 % CPU from others at the start and 88 % at the end, so an upper bound; the textures are the host's and not in it |
| A piece's build since pieces are cut (W97c) | the same bench at 22:00, no quiet wait (the machine as busy): the busiest piece, 1,800 clasts and 3,069 clusters, median 384 ms (min 377 ms); the busiest tile is seven such pieces, which a streamed host builds on its pool of two |
| Laying the region, the bench | `channels.lay_region`: median 56 ms at 20:49, 63 ms at 22:00 |
| W93's scratch scene (`engine-content channels`) | 65,611 clasts over 474 m of channel, 14 tiles |

**Where the triangles go**: a clast under 128 mm is 48 triangles before those under the ground
are left out, and on a lognormal bed most clasts are the smallest drawn. 1,352 a square metre is nearly five
times the deposits' finest (rocks.md, "Where the triangles go"); the cluster DAG draws a tile's
far clasts coarse, and `clast_min_mm` is the knob (128 mm takes the count to about a quarter on
such a bed). **Resident**: the plan's `ChannelSediment`, 32 bytes a point of line (64 bytes a
metre at the 0.5 m step); since W97b `open` keeps each tile's recipe text in memory with its
placement (12 integers a clast, about 60 bytes of JSON: about 0.7 MB for the busiest tile, 4 MB
for the test valley's 66,253 clasts) instead of writing it, which is what scales with the bed's
area before any tile is near. **Who pays the build**: a host that streams (the world's placements
consumer) builds a tile's piece when the tile activates, off the frame's thread, and holds it while
the tile is held; a host that reads a scene whole (`engine-view --scene` without streaming) still
lists and builds every piece at load through `expand`, as the contract requires, so it measured
5.5 s of meshes at load on W97's scratch valley where the base took 15.4 s, both on a cold cache
and the machine loaded (an upper bound each). What scales with the world: the plan, by
its length of channel; the recipes and meshes, by its area of bed.

## In a scene

```json
{"generator": "channels", "stream": false,
 "params": {"name": "fells", "seed": 1, "region_min": [-512, 1024], "region_max": [512, 2048],
            "head_threshold_m2": 1500}}
```

The entry is opened once: its plan is read (or, for an entry on uncut ground whose plan is not
cached yet, made; [One plan](#one-plan)) from the host's derived-data root's `channels/` (else the
repository's `ddc/`, else `ddc/` beside the scene), every piece's recipe decided and kept, and
`expand`/`tile`/`occupies` hand out what was decided, a tile's piece with the tile. An entry of a scene whose ground carries the
plan's cuts names the plan: `"plan": "<the key engine-content channels printed>"`. A moving ground is refused (the dunes have no
drainage to dress).

**An entry that names no plan plans its own cuts** (W97c, 2026-11-11, through R144's hook,
[scene_gen](scene_gen.md#what-an-entry-plans-for-the-ground)): `plan_ground` hands the network's
cuts, the very blocks `engine-content channels --cut` writes (`terrain_cuts_json`), as the
ground's edits, so such a scene pastes no cut and its ground is the pasted scene's: one list of
edits under one key (`channels: an entry that names no plan plans its cuts through the ground
hook` holds the planned list to the pasted one byte for byte, and R144's own case holds a list to
the ground it builds). Its `open` stands the plan on `Context::uncut()`, so the planning pass and
the second open on the cut ground make one plan and plan once. An entry that **names** its plan
stands on cuts the scene carries and plans none: the two are one entry's two spellings, never
both. What is still pasted: the water's ribbons (`water_channels`) and the beds' mask regions,
which the hook has no list for (`GroundPlan` holds edits and openings); `engine-content channels`
stays the way to inspect a plan and to write those. **Since this, an entry that names no plan on a
terrain with no `tiles` block is refused** by the scene reader (an edit is written into a tile
set's heights), where it used to lay its stones on the uncut ground: a channel is cut into the
ground (W89), and a ground that cannot be cut cannot carry one.

**The host's root wins, the plan's too** (T112, 2026-11-11). A host given `--ddc` reads a named
plan under that root and nowhere else, as it builds the ground's set there over the scene's own
`tiles.ddc` (T90); without one, under the scene's own (the repository's `ddc/` above the scene, else
`ddc/` beside it). So a plan `engine-content channels` wrote without `--ddc` is not found by a host
given one: the entry is refused, and when the plan is under the scene's own root the refusal says
so and where (`channels: the host's root wins over the scene's own`). Reading it from either root
would make which plan a scene draws depend on which roots happen to hold one; refusing loudly
keeps one rule and one answer. `engine-content channels --ddc` takes the same root for the plan
and for the uncut ground's set, so the subcommand and the host are given one `--ddc`, or neither. Every field, its default and range is in `schemas/channels.schema`.

## The water's description

`engine-content channels <scene.json> <out.json> [--min-area-m2 <m2>] [--stage <f>] [--entry <i>] [--cut]`
reads the scene's terrain through the ground provider it names (the registry the renderer uses) and
runs every `channels` placement (or the one at `--entry`) through `plan_channels`, and writes
`{"water_channels": [...]}`: one block per link whose last point drains at least `--min-area-m2`
(50,000 m² by default), each

```json
{"name": "<layer> link <k>", "points": [[x, y, z], ...], "width_m": [...], "depth_m": [...],
 "slope": [...], "seed": <u64>}
```

— `engine.scene.WaterChannel` version 1, the shape agreed with the flowing-water row (S96), which
declares the type in `scene.schema`: `points` the line downstream, every resampled point (`step_m`),
x and z to the millimetre and **y the water's surface**, which is the bed (the ground's running minimum
down the line) plus `--stage` (0.5) of the bankfull depth, never rising downstream; `depth_m` the bed
below that surface; `width_m` the bankfull width; `slope` the bed's fall per metre there. It prints
one JSON line: entries, heads, links, metres of channel, the region's km² and km per km², blocks
written. `water_channels_json` is the function, and the subcommand's end-to-end test holds its output
to the network planned in-process over the same provider.

**A recipe whose banks are not the defaults** writes `bank_width_widths` and `bank_width_m` on every
block (`WaterChannel` version 4; T78, 2026-11-09): they are the reach the placements leave bare beyond
the water's edge ([scene_gen](scene_gen.md#exclusion)), which `scene_gen` reads from the description
because it may not name this capability. At the defaults neither is written, so every description
written before is still the bytes it was, and a scene that holds one reads at the defaults.

## The cut

(2026-11-07, roadmap W89; the POC's v35 engine-needs, item 1, and the owner's walk, item 11.) A real
channel lies below its floodplain: the bed at bankfull depth under the banks' tops, cut banks falling
to it. W86 laid its bed 2 cm over the trough the erosion model left and its banks above that, which
is why it read as a raised canal; the ground was never lower than the floodplain where the water
ran. **`engine-content channels --cut`** now writes, beside `water_channels`, the terrain's
**`cuts`** (`engine.scene.TerrainCut`; `terrain_cuts_json`): one per link of the network, every link,
the rills' too, each point with

- **the top of the bank** (its y): the ground under the line, the height the floodplain stands at;
- **the bankfull width** of the hydraulic geometry;
- **the bed's depth below the top**: the section's thalweg, a trapezoid with banks at the layer's
  `bank_slope` (rise over run, 1 by default) whose area is the width times the hydraulic geometry's
  mean depth (`D (W − D / m) = W d̄`; a V of twice the mean where the banks are too gentle for that
  area), the bed held down the line where the ground at it rises a little (the cut runs through a
  small rise deeper than a thalweg) but **never more than two thalwegs under the ground at the line**:
  where the line climbs out of a filled depression or over a lake's outlet the bed climbs with it (on
  v35 the running minimum alone would have cut 103 m through one, and 8.6 % of the network's points
  more than a metre);
- and the bank slope.

The author pastes them into the `tiles` block (`terrain.tiles.cuts`), and **the tile set is built
with them**: every level finer than a channel's width holds its section, so every ring and tile the
renderer draws, the ray path, the cascades, the collision and every placement stand on the cut
ground ([tileset](tileset.md#channels-cut-into-the-ground) has the rule, the cell rule and the keys).
The source of the section, the bank slope and the stage is
[the grounding page](../experiments/channels.md#the-cut).

**The water within.** With `--cut` each `WaterChannel` point's y is the cut's bed plus `--stage` of
its thalweg depth, and `depth_m` that water's depth, so the surface lies inside the banks: at the
default 0.5, half the thalweg depth below the floodplain. It never rises downstream except where the
cut's bed climbs over a sill, where the water keeps half its depth over the bed rather than run
under the ground. S96's ribbon is wider than the water it
carries (the bankfull width and a quarter past each edge); at a stage below bankfull those edges are
under the cut's faces and the depth test hides them. Without `--cut` the subcommand writes what it
wrote before: no cuts and the surface on the ground as W86 laid it, for a scene whose ground is not
cut.

**The network is planned over the ground before its cuts.** The subcommand clears the scene's
`tiles.cuts` before it opens the ground, so writing the cuts of a scene that already carries them
gives the same file again (the end-to-end test asserts it). The placement generator, inside a scene
whose set is cut, **reads that same plan and never makes another** ([One plan](#one-plan)): until
W93's continuation it planned again over the cut ground, and its lines followed the troughs the
cuts deepened without being the cuts' lines.

**Bed and bars: the ground, not pieces** (W93, continued; W89's brief chose the other way and this
reverses it). W89 kept the bed and the bars as recipes laid on the cut ground, because a piece
carried the gravel's relief and the riffle's rise and cost none of the masks' four layers a tile.
What it cost was seen on every scene since: a slab is laid from a line and stands on the ground at
its vertices, so it showed as plates wherever its line was not the cut's or the ground between two
of its vertices was not a plane. The riffle's rise is the cut's section now, and the bed's material
belongs with the ground it is ([What it places](#what-it-places)).

**Seen** (2026-11-08, `msvc-debug`, 1920×1080, `--raster hw --shadows rt`, world tiles with v35's
rings): a synthetic flat ground at 25 cm with a 200 m link bending 25 m, 3 m wide and 0.6 m to its
bed, and a 1.5 m tributary joining it, authored by hand. The channel lies in the ground, its banks
falling to the water at their slope and shadowed on the sun's far side, continuous round the bend
with no gap and no levee, the water inside them; beside it the same water laid on the uncut ground
stands on it as a raised strip. Past the 1 m ring the channel narrows to the 2 m and 4 m rings'
point samples and reads jagged, and the 1.5 m tributary fades to a line, as the cell rule and the
rings' sampling say it should. v35 itself was not re-cooked tonight (its set is 2.6 GB at 25 cm; see
the roadmap row).

**The cook of a cut set, measured** (W91 continuation, 2026-11-09; W89 and W90 recorded no cook of
v35's or v36's cut set): v36 with W91's 739 cuts, cooked by `engine-view`'s first load (`msvc-release`,
`ENGINE_PIPELINE_CACHE=0`, under the GPU lock, other agents building beside it) into an empty
directory. Twice, the same key and the same bytes: the tile set's manifest appeared 44 s and 36 s
after the process started, the masks' 242 s and 233 s, so the cook is about **four minutes** of
the load, and the set is **3.14 GB** (2.73 GB of tiles, 396 MB of masks; v35's uncut set was 2.6 GB).
The process's peak working set over the whole run — load, cook and two frames, so not the cook alone
— was **18.7 GB** (17.4 GiB), read from the second run's last sample. The first run, which also drew
the 16 poses, took 719 s; the second, two frames, 500 s.

**Steep reaches** (the owner's item 10: 30–40° faces) get the same cut — a gully is still cut into
its slope — and since W90 their pieces follow their reach ([Reaches](#reaches)): nothing laid on a
colluvial or bedrock reach, boulder steps on a step-pool reach and boulder clusters on a cascade
(W91).

## The section

(2026-11-10, roadmap W93; the POC's solver needs, 2.1 items 1, 6 and 8: "a lowland stream reads as
a canal" through three updates, because W89's cut was one trapezoid along a reach.) **The cut's
section follows the line.** `plan_channels` gives every point of a link an **asymmetry**, -1 to 1
(`ChannelLink::asym`; positive where the line bends left, toward (-dz, dx)), and `terrain_cuts_json`
writes the section it stands for (`engine.scene.TerrainCut` version 2: `bank_slope_left`,
`bank_slope_right`, `bank_round`):

- **At a riffle** (asymmetry 0, the crossing between two bends) the section is W89's trapezoid, half
  `riffle_widening` (0.12) wider than the hydraulic geometry's width and that much shallower, so its
  area is the hydraulic geometry's.
- **At a bend's apex** (asymmetry 1 or -1) it is a **triangle of the same area**: the outer bank a
  cut bank at `cut_bank_slope` (3), the thalweg at its toe twice the mean depth under the top, and
  the **point bar** rising from there at a few degrees to the inner bank's top; half
  `riffle_widening` narrower. Between the two the thalweg and the outer bank go linearly with the
  asymmetry and the inner bank's run is what keeps the area, so the section's area is the hydraulic
  geometry's at every point ([the grounding](../experiments/channels.md#the-section) has the
  formula and where each number is from).
- **The pool is under the bed's line.** `ChannelLink::bed` (the cut's bed W89 to W91 planned, which
  the water stands on) is the riffles' bed; a pool lowers the thalweg below it and a riffle's
  shallower section stands a few centimetres over it. The water's surface is where it was, so a
  pool is deep water against the outer bank, the bar's upper part stands dry above it, and a riffle
  is shallow from bank to bank. The water's description is unchanged: its `depth_m` is the depth
  over the bed's line, which is what Manning's speed is read from (a pool's greater depth would
  speed its water up, and a pool is slow).
- **The banks' tops are rounded** over `bank_round` (0.75) of the depth: the cut's face and the
  ground meet by a smooth minimum, not a crease ([tileset](tileset.md#channels-cut-into-the-ground)).
- **The faces run out to the ground.** W89's face ended a depth over its top; where the ground
  beside the channel stood higher (a channel in a steep-sided valley, a line off its trough's
  floor) the cut ended in a wall, and the lattice drew the wall as **a row of teeth along the
  lip**: the POC's item 7, reproduced on this row's scratch scene in a valley with 17- to
  19-degree sides. A rounded cut's faces go on past that end over one more run of their own,
  steepening, and meet the ground wherever it is ([tileset](tileset.md#channels-cut-into-the-ground),
  "A face that runs out").

**Where the asymmetry comes from.** A bend's share is the line's mean curvature over **four
widths** about the point — the turn between its heading two widths above and two below, over that
length — times the width times `bend_radius_widths` (3: Leopold & Wolman's radius of 2 to 3
widths is the fully asymmetric bend). Four widths, not the line's own 0.5 m step, because a bar
and its pool are built by the flow along a bend and not by a kink: read point by point, the kinks
the trough test left put a pool every 2.7 widths, alternating sides (they are low-passed out of
the line now, [The planform](#the-planform), and the window stays). Where a stretch
does not bend (the mean bend over a pool-to-pool length either way under 0.2 to 0.5) the asymmetry
**alternates sides** every `pool_riffle_widths` (6), a sine of the link's own phase, to
`alternate_asymmetry` (0.6): a straight channel's thalweg swings from bank to bank and has its
pools too. By reach: pool-riffle and dune-ripple reaches take both; a **plane-bed** reach half a
bend's and no alternation (Montgomery & Buffington's plane bed has no free bars; a bend still
forces a pool); a knickpoint, a fan, a tributary's last points inside its trunk's banks, a bed
narrower than `bar_min_width_m` and the first and last two widths of a link take none; and
step-pool, cascade, bedrock and colluvial reaches have a section of their own
([The steep reaches](#the-steep-reaches)). The share is eased by two binomial passes, so a
reach's edge is not a step in the section.

**The pool lies downstream of its bend's apex**: the section at a point is the bend's one width
upstream of it — 0.36 of the channel distance from an apex to the next inflection, the mean over
a surveyed river's bends, which for a meander 10.9 widths long is a width
([the grounding](../experiments/channels.md#the-steep-reaches-the-pools-place-and-three-numbers-read-again)).
W93's first cut put the pool at the apex.

**Nothing is laid on the section.** W93's first cut of this kept a bed slab, each row finding its
bed with nine ground probes, and a bar skin below full asymmetry, because the generator planned
its own line over the cut ground and the slab had to be fitted to a cut it did not know. With
[one plan](#one-plan) the line is the cut's, and the slab and the skin are gone with the reason
for them ([What it places](#what-it-places)). The switch back to W89's trapezoid
(`section_asymmetry` 0), which kept W86's lens code alive, is gone too: every cut the generator
writes is shaped, with a slope a bank a point, a rounded top (`bank_round`, 0.1 to 1) and faces
that run out; and `TerrainCut` itself (version 3) refuses a rounding of 0, so a cut written by
hand cannot ask for the crease and the wall either
([tileset](tileset.md#channels-cut-into-the-ground)).

**Measured** (12 seeds on the test valley, `domain/channels/tests/channels_tests.cpp`): at 327 of
341 bends the outer bank is the deeper side (551 of 553 before the kinks were taken out of the
line and the pool moved a width downstream of the apex, where the test's bend is still defined);
pools stand **6.6 widths** apart on average (149 spacings, 2.7 to 24; Keller & Melhorn's 5 to 7); the section's area is the hydraulic geometry's within 4 % at
every point whose bed runs through no rise; a riffle at its widest is 1.128 of a pool at its
narrowest.

**What a second client of the cut would add** is on the cut's own header
(`domain/tileset/include/domain/tileset/cuts.h`): the profile across a line, its left and right
and its change along the line are a trail's tread or a terrace as they stand; a fill rule (the
edit only lowers), a top per side and a scene block of its own are what they lack.

## The flood bed

(2026-11-10, roadmap W93; the POC's solver needs, 2.1 item 3: "a river's bed is much wider than its
water", and the page's own "the valley's confinement is not read for the reach".) **The reach reads
the valley's confinement from the ground, and the cut becomes a compound section**: the channel
inside a flood bed, bounded by a scarp each side where the valley leaves room for one
(`plan_flood`, `ChannelLink::flood_left`, `flood_right`, `entrenchment`).

- **The confinement.** At a station every bankfull width along the line, the ground is walked
  across the line each way, from the line out in steps of a sixth of the width (0.5 m at least), to
  where it first stands `flood_height_depths - 1` thalwegs over the ground at the line: Rosgen's
  **flood-prone elevation**, twice the greatest bankfull depth over the thalweg (the stated height
  above the bed: two thalwegs, so one over the top of the bank). The crossing is found between the
  two steps that bracket it. The two distances over the bankfull width are the **entrenchment
  ratio** (`entrenchment`; Rosgen 1994). A side whose ground falls a thalweg *under* the line's
  first is not a valley's floor — the line is perched on a slope — and its walk ends there. The
  walk stops at the furthest a bed could reach, so a ratio past that is recorded as that (5 at the
  defaults). **A bed is as narrow as the narrowest reading near it**: each station's reach is the
  least over itself and its two neighbours either way, because the line kinks and a walk across a
  kinked line can run along the valley and read a floor several times as wide as it is (on this
  row's scratch scene that put a scarp metres up a valley's side); the walk's direction is across
  the line's heading over four widths for the same reason.
- **The bed.** Where the ratio is at least `flood_entrenchment_min` (1.4: below it Rosgen calls a
  channel entrenched, and it fills its valley's floor) the bed reaches, each side, the flood-prone
  distance less the scarp's run, and half `flood_bed_widths` (3) widths at the most; 0.6 of that
  more on the inside of a bend and less on its outside, since the bar builds on the inside and the
  outer bank is cut into the terrace; never less than the channel's own bank; and a bed under 1.1
  widths is none. So **its width follows the confinement**: on a floor wider than three widths it
  is three widths; on a narrower one it is the floor less the scarps; in a V it is nothing.
- **Its level and its scarps.** The bed's surface stands `flood_bed_depth` (0.25) of the thalweg
  under the top of the bank, which is over the water's default stage (half the thalweg), so the
  water is one thread in it; its scarps stand at `scarp_slope` (1.5) and top out at the flood-prone
  elevation, a thalweg over the top: where the valley's floor is wider than the bed that is a turf
  edge a quarter of a thalweg high, and where the bed runs to the valley's side it is a raw scarp
  up to a thalweg and a quarter high.
- **Where.** Pool-riffle, dune-ripple and plane-bed reaches at least `flood_min_width_m` (2 m)
  wide, away from a knickpoint, a fan and a tributary's last points; the reaches close to the
  channel's own width over two widths at a stretch's ends. `flood_bed_widths` is 1.25 to 12:
  no value switches the bed off, since a channel on a floor wide enough for one has one.
- **Written** into the cut (`flood_left_m`, `flood_right_m`, `flood_depth_m`, `scarp_m`,
  `scarp_slope`; [tileset](tileset.md#channels-cut-into-the-ground) has the section and the cell
  rule, which reads the bed by its own width, so a level too coarse for a stream may still hold
  its bed) and onto the water's description (`WaterChannel` version 5: `flood_left_m`,
  `flood_right_m`), which is how **the placements leave the bed bare**
  ([scene_gen](scene_gen.md#exclusion)): `scene_gen` may not name this capability, so the reach
  travels as data on the description the two share, as the bank's does (T78).

**The bed is ground, and its material is the ground's.** The cut gives the bed its form, and since
W93's continuation nothing is laid on it or on the channel's own bed
([What it places](#what-it-places)). Its material goes through the terrain's masks: a layer that
names `bed_material` — a layer of the terrain's own palette (`terrain.materials.layers[].name`),
standing for the role `gravel/bed`, which no pack here has a set for — has every stretch of its
beds written by `engine-content channels --cut` as an `engine.scene.TerrainMaskRegion` under
`mask_regions` (`bed_regions_json`), for the terrain's `masks.regions`. A stretch is a run of a
link's points a metre wide or more, off a knickpoint; its outline reaches, each side, the further
of the cut's own bank top and the flood bed's edge, a corner about every width, its edge
feathered over `bed_feather_m` (0.4 m). So the bed's gravel is the plan's too: the outline is the
cut's, with no polygon drawn by hand over it (W93's scratch scene built one in its script). It
is one more thing pasted ([One plan](#one-plan), "Still pasted"); the masks themselves were not
changed, and a region costs the mask level one of its four layers a tile where it lies, which
W89 counted against paints and which the plates cost more than. Held by "the beds' outlines are
mask regions that hold the cut and its flood bed". The bars' finer sediment (`sand/bar`) is not
separated from the bed's: one material for the whole bed.

**The waterline on a bar is still a sawtooth, and what is left of it is the water's.** The
water's ribbon is a strip at the water's level, three quarters of the hydraulic geometry's width
either side of the line, and what a picture shows as the waterline is where the depth test lets
that strip through the ground as it is drawn. A point bar rises at a few degrees, so a 5 cm step
between two points of the ring a view draws the ground from (0.5 m past the nearest, 1 m and 2 m
beyond) moves the waterline by a metre: the teeth are the ground's lattice, not the ribbon's
edge. The channel's mesher could end the strip at the waterline the cut's profile gives exactly
(the section's two slopes and the stage say how far from the line the water reaches on each
side; it would need those two distances a point on `WaterChannel`), and **that is not built**.
It would smooth the edge only where the drawn ground stands under the strip's edge; where the
drawn ground stands over it the ground still cuts the strip in teeth. Closing it for good is the
water's shading — the surface fading out over its last centimetres of depth, read from the
depth behind it, so that the edge is where the water is thin and not where two meshes cross —
or a ground drawn finer along a channel, and neither is this capability's.

**Measured** (`channels_tests.cpp`, "the flood bed's width follows the valley's confinement"):
on three valleys whose floors are 32, 9 and 2 m wide between 45-degree walls, with a 4.6 to 5.3 m
stream down the middle, the measured ratio is the floor's and the walls' to 3 % (5 where the walk
stops, about 2, and 0.6), and the bed is three widths, the floor less its scarps, and none.
The subcommand's summary line carries `flood_bed_m` and `flood_bed_m2` (metres of line with a bed
and the bed's area) and `pools`.

**Not built**: bars as relief on the bed beyond the channel's own point bar (a flood bed's bars
are the low-flow channel's; the bed between them is flat); braided threads; a bed whose level
steps down terrace by terrace; a scarp a walker reads on a wide floor (the bed stands a quarter
of a thalweg under the bank's top, 15 cm on a 0.6 m channel: a change of colour, not a step);
the bars' own material apart from the bed's. W93's continuation reached the bed's material and
stopped there.

## The steep reaches

(2026-11-10, roadmap W93, continued; the owner's pictures: the steep `stream` view "still reads
as a canal: one width, one depth, a ribbon in a V, and tributaries down the hillside as straight
chutes". Grounded first:
[the grounding page](../experiments/channels.md#the-steep-reaches-the-pools-place-and-three-numbers-read-again)
says what was read in the papers, what at second hand and what is a design choice.) On step-pool,
cascade, bedrock and colluvial reaches W93's first cut kept W89's section, one trapezoid of one
width. Montgomery & Buffington's steep reaches are confined by their valley's walls, their bed is
boulders in channel-spanning steps (step-pool, a pool every one to four widths) or disorganised
(cascade, pools under a width apart), and steps form where a channel narrows. So:

- **The width follows the steps** (`ChannelLink::swell`, `step_widening` 0.3). On a step-pool
  reach the cut is 15 % narrower than the hydraulic geometry's width at each step's crest and 15 %
  wider over the pool below it, a cosine between crests; on a cascade, a bedrock or a colluvial
  reach, which have no steps, it varies as much irregularly (a value noise of the link's own, a
  knot every width and a half). The section's area is kept, so the narrow places are the deep
  ones.
- **The thalweg wanders between the banks** (`steep_asymmetry` 0.35): an irregular asymmetry, a
  knot every two widths, read by the same section as a bend's ([The section](#the-section)): the
  bank the thalweg lies against is the steeper and the other the gentler, so the two bank lines
  are not parallel offsets of the centre line and neither is one slope along a reach.
- **Rocks at the banks.** A step is an accumulation jammed between the banks: beside the row of
  clasts across the bed, each step has a clast at each end, 1 to 1.5 of the step's clast across,
  its centre a third of itself past the bed's edge, half in the bank; about half of a cascade's
  clusters have one against a bank too ([What it places](#what-it-places)). They are what breaks
  the bank's line where the cut's own form is too smooth to.
- **A steep line wanders** (`steep_wander_widths` 0.75; `build_line`). Above the meanders' slope
  the routing's line down a planar slope is a ruled line. Where the meander fades the line takes
  an irregular offset instead: a value noise with a knot every five widths (5 m at least), up to
  three quarters of a width (of a metre at least) either way, tested against the trough exactly
  as a meander's offset is, so in a gully with sides it is drawn back and on a plane it stays.
- **Where.** Reaches at least `bar_min_width_m` (1 m) wide for the section (a lattice of 25 cm
  holds no asymmetry in a narrower one); every steep link for the wander; not at a knickpoint, in
  a fan, over a tributary's last points or within two widths of a link's ends.

**Held** (`channels_tests.cpp`, "a steep reach's width follows its steps and its banks are no
ruled line"): on a 4.5 % valley with a 3 m channel the cut is 0.857 to 1.143 of the hydraulic
geometry's width (a ratio of 1.33 between its widest and narrowest; the test asks 1.27), narrower
at every crest and wider between; the asymmetry runs -0.31 to 0.33. On a planar hillside at 35 %,
whose gully was a ruled line for all its 440 m, neither bank's top holds a stretch longer than
17 m within 5 cm of a chord (the test holds 20 m), and the gully's sinuosity is 1.006.

**What it is not.** The banks are still earth: a heightfield's face with clasts standing in it,
not bedrock ledges or a boulder revetment, and the cut's bed between the steps is smooth ground
with no cobbles. The width's variation is a design choice in amount. A wander of three quarters
of a width over five widths is visible from above and slight from the bank; a stronger one (1.5
widths) was measured to cost the downhill bank at 2.7 % of the test valley's points and was not
kept.

## A line across a slope

(2026-11-10, roadmap W93; the POC's solver needs, 2.1 item 2: where a line runs across a slope the
cut leaves a bench with no downhill bank.) **Measured before choosing.** `engine-content channels`
now reports how the line stands on its ground: for every stretch at least a metre wide, the ground
at the two bank tops (half the width across the line each way) against the cut's bed, and the
line's heading against the ground's fall line over a width — `line_measured_m`,
`line_by_fall_angle_m` (0-15, 15-30, 30-45, 45-60 and 60-90 degrees), `half_bank_m` and
`no_bank_m` (a bank's ground more than half the cut's depth under the line's, and under the bed)
with the same bins, and `no_bank_mean_slope`.

On this row's scratch scene (a V with sides at 0.35 whose floor swings 18 m either way every 80 m,
a plane-bed reach and a wide floor; 470 m of line a metre wide or more), before the move below:
**16 m with less than half a bank, 12 of them at 60 to 90 degrees to the fall line, and none with
no bank**. On the test valley of the same shape (`channels_tests.cpp`, "a line across a slope is
moved between its banks"; 1,969 points) after one round of the move 100 points had less than half
a bank and 12 none. A caution on the angle: on a valley's floor the fall line over a width is the
valley side's wherever the line is off the floor's middle, so half of any line reads 60 to 90
degrees (240 of the 470 m) without being wrong; the bank is the test, and the angle says which way
the line runs where the bank is missing.

**The line is what is wrong, not the edit.** A stream does not traverse a slope: D8 sends every
cell to its steepest neighbour, so a stretch that runs across the fall with one bank missing was
put there by what comes after the routing — the low-pass that takes the line off a 4 m lattice
(sigma 6 m or two widths) cuts a bend's corner across the valley's side, and the meander rule
offsets it again. W91's trough test holds the line's *centre* within half a depth of the trough's
floor; a channel is a width wide, and what was left is where the centre passes and a bank does
not. An edit that could raise would build a levee on a hillside, which is what W86's berms were
and what read as "a raised canal between levees"; it was not built.

**The line is moved between its banks** (`build_line`, after the trough test). Each point at
least a metre wide is tested across its own heading: where a bank's ground stands more than half
a depth under the ground at the point, the point moves toward that bank a quarter of the width at
a time, a width at the most, until neither does. The moves are eased by three binomial passes,
which takes some of each back, so the whole is done twice (a third round was measured to undo part
of the second: 32 points with less than half a bank after two, 48 after three). The two ends stay
where the junctions are; rills under a metre are left. `ChannelLink::bank_moved` and the summary's
`bank_moved_points` count the points moved. **What is left** on the test valley: 32 of 1,969
points (1.6 %) with less than half a bank and 12 (0.6 %) with none, the worst 1.23 depths under;
the test holds 2 % and 1 %. They are where a width's move does not reach the floor, and a ribbon
there is still narrowed or lowered by W91's fit. Since the kinks were low-passed out of the line
and the steep line wanders ([The planform](#the-planform)): 36 points (1.8 %) with less than half
a bank and **none** with no bank, the worst 1.08 depths under.

### The section seated under its lower bank (W116)

(2026-11-12, roadmap W116; the owner's walk of the forest that morning, marks 3 to 5, and the
forest's engine needs 53 and 21: "a level bar or flood-bed surface on a cross-slope stands out of
the hillside on its downhill side".) Moving the line answers a line on a valley's side above its
floor; it cannot answer a stream that runs along a hillside's contour, where every place a width
across falls the same. There the cut's **top of bank was the ground at the line**, so the downhill
bank's ground stood under the top by half the width times the cross-slope — under the bed and the
water past a little cross-slope — and the cut, which only lowers, left the section's downhill half
standing over the hillside: the water carried level over the drop, and a flood bed whose level
reached out over ground that falls away (the tan plate of mark 4, the bed's mask region painted on
it).

**The rule.** At every point of every kept stretch at least `bar_min_width_m` wide, nothing of the
section stands over the ground beside it by more than `k_seat_bound_m` (**2 cm**, under the 25 cm
lattice's height step on a slope of one in ten): the top of the bank (and with it the bed, the bar
and the water under it) over the ground at the **lower** bank top, read across the line's heading
over a width at the section's half width; and each side's flood-bed level over the ground at that
side's edge. The section's faces rise from its bed to its top at the bank tops, so on ground that
is a plane across a width a top that holds holds every place between. Fans (a body's level stands
over their banks), knickpoints and a tributary's confluence grade (it opens into its trunk) are not
read. `measure_seat` measures it and `engine-content channels` prints it as `seat` in its summary
line: the points read, those over the bound (`bank_over`, `flood_over`) and the most, how far and
how much line the plan seated, and the water's fit codes.

**How it is met**, in the order the brief asked:

- **The section narrows on the downhill side where nothing needs its width: the flood bed.** Each
  side's walk ([The flood bed](#the-flood-bed)) also finds where the ground first stands under the
  bed's level (`flood_bed_depth` of a thalweg under the top), and the bed reaches no further on
  that side. A side whose ground falls away from the line has no bed; on a hillside the uphill
  side's walk meets the flood-prone height within a width or two, so a seated reach is entrenched
  and has none either — which is what confined reaches are (Montgomery & Buffington 1997: "little
  to no" flood plain; Fryirs, Wheaton & Brierley 2016: floodplain pockets under a tenth of a
  confined reach).
- **The channel is cut deeper** (`seat_section`, `ChannelLink::seat`): its width is the hydraulic
  geometry's and carries its area, so it is not narrowed; its top is the ground at the lower of its
  two bank tops, its bed, its flood bed and its water that much lower. Bankfull is the stage at
  which the water first spills (Leopold, Wolman & Miller 1964, as Buffington's field guide states
  it), so on unequal banks it is the lower bank's top that bounds it, and water standing over its
  downhill bank would leave down the fall line. The bank's ground is read at nine half widths over
  the range the section's swell can give it (the swell is planned after the bed), the lowest kept,
  rounded up to the millimetre and taken as the most over two points either way. The uphill bank
  is then the higher, its face running out to the hillside as every cut's does
  ([tileset](tileset.md#channels-cut-into-the-ground), "A face that runs out").
- **No fill.** A `Fill` would build the downhill bank as a levee, and a natural levee is the
  overbank deposit of a channel on its floodplain, a ridge between the two (Brierley, Ferguson &
  Woolfe 1997); a channel cut into a hillside has neither, so a bank built there reads as an
  earthwork, which is what W86's berms read as. Cutting deeper always holds the rule, so no reach
  is refused for it.
- **The line is moved toward a bank only where the move finds one** (`build_line`): the place that
  holds both banks, or the tested place (a width at the most) whose bank falls a tenth of a depth
  less than at the line. W93's move took a width untested whenever no place held, which on a
  hillside carried the line off the trough the routing found and down the slope: on W116's scratch
  scene it moved 493 of 707 points, kinked the line where each move began and ended, and turned a
  1.5 % contour stream into 169 points of step-pool and 30 of cascade.

**What a cross-sloping reach looks like in section now**: a channel incised into the hillside, its
downhill bank top at the ground and its uphill bank higher by the width times the cross-slope plus
the seat (2.3 m on a 3.3 m stream across 30 degrees), the water at half the thalweg under the low
bank, no flood bed on either side, and the bed's mask region no wider than the cut's own bank tops.

**Measured.** W116's scratch scene (a 384 m stream along a hillside's contour at the
toe of a metre-wide bench, falling 1.5 %, the hillside falling across it at 10, 20 and 30 degrees,
`--cut`, 707 points): on the base, **648 points' tops stood over their lower bank's ground by more
than 2 cm, 399 m of the 438 m, the worst 1.08 m** (at 30 degrees); 39 of 101 flood-bed sides over
their edge, the worst 0.20 m; the ribbon's fit narrowed 384 points; `no_bank_m` 133. Now: **none**
(the worst 13 mm), no flood-bed side over (43 read), the seat up to 1.09 m over 370 m of line, the
fit narrows 12 points and lowers none, `no_bank_m` 0, and the reaches 95 points of step-pool and
none of cascade. Held by "a stream across a hillside is seated under its downhill bank" (three
cross-slopes, three seeds, `measure_seat` and the water's fit) and, on the tile set's own edited
ground (`seat_ground_tests.cpp`, built where the tileset capability is: the cuts compiled by
`tileset::TerrainEdits`), "on the edited ground nothing of a cross-slope's section stands in the
air": the section read at nine places a side out to its bank top or flood edge, 7,758 places a
case, none over the ground under it by more than 2 cm, and the ribbon's edges and the handed
width's within the fit's tolerance of the edited ground. The W93 valley above, read by the same
measure: no point over (the worst 4 mm), seated up to 0.40 m.

**What is left.** The meander still swings the contour stream a little down the slope at 20 and
30 degrees, so its profile reads step-pool for 31 to 85 of 433 points (printed by the case, not
held); a meander on a hillside wants the trough test applied across the whole width, not the
centre. The W93 valley's count above (points whose bank stands half a depth under the *line's*
ground) is 58 of 1,969 with the move as it is now, where W93 held 2 % (36): those points are
seated, so their cut has both banks, and the count no longer reads a missing bank.

## Seen: the section, the flood bed and the proof of concept's items 6, 7 and 8

(2026-11-10, W93; `msvc-release`, 1920×1080, `--raster hw --shadows rt --terrain-tiles
--terrain-far 2`, offscreen, one process a run; a scratch scene outside the repository: a 384 by
256 m heightfield at 25 cm, a V with sides at 0.35 whose floor swings 18 m every 80 m, then a
plane-bed reach on a 6 m floor, then a meander reach on a 44 m floor, one channels layer at 14 m
and 1.2 m a square kilometre; "before" is 76d96ced built in the same worktree, both over one
derived-data cache; the ground has no texture set and no plants, so what is judged is form. The
references looked at: the reels r03 and r49 and the motion sheet `clear-riffle-cobbles`.)

- **`pool-riffle`** (the meander reach, eye height). Before: a ribbon of one width in a trench of
  one section, two grey bar lenses standing on the bed as mounds, a pale strip of bed slab along
  the left lip. After: the water is a thread of changing width inside a grey bed three times as
  wide (the scene paints the cut's reach with a mask region), shallow and pale over the bar's
  side and deeper against the other, the banks low with rounded shoulders, no lens. **Still wrong**:
  the waterline on the bar is a sawtooth of the lattice (the rings past the nearest sample the cut
  at 0.5 m and coarser); the bed's edge on a wide floor is a change of colour and not a scarp (a
  quarter of a thalweg, 15 cm); a bed slab lies on the flood bed as a pale plate where the
  generator's own line is more than three quarters of a width off the cut's.
- **`plane-bed`** (the 6 m floor between 17-degree sides). Before: the trench's right bank is a
  wall of grey slabs — the bed pieces laid about the generator's line, riding up the bank (**item
  8**, and what made item 6's slabs) — over straight faces. After: the slabs are gone but one
  sliver, and the bank is the cut's own face run out into the hillside in rounded scallops. With
  the first cut of this row the same view had **item 7**, a row of teeth along the lip and a
  gouge metres up the valley's side: the face's end as a wall, and a flood bed measured across a
  kinked line. Both are closed (the run-out, the narrowest reading). **Still wrong**: the line
  kinks, so the banks zigzag.
- **`stream`** (the V, a 2 m channel). Before: a trench with a notch and teeth where its face
  ended against the valley's side. After: a soft fold, no wall. **Still a canal**: on a reach
  steeper than plane-bed the section is W89's, one width and one depth, and two tributaries come
  straight down a planar side as chutes.

**The same eight views after W93's continuation** (2026-11-10, the same scene, camera and flags,
its entry naming its plan; `msvc-release`; references looked at again: r03 and the motion sheet
`riffles-and-pools`).

- **`pool-riffle`**: the pale plates on the flood bed are gone; the bed is one clean floor. The
  waterline on the bar is still a sawtooth, and the bed is still flat: no bar on it, no scarp to
  speak of.
- **`plane-bed`** and **`plane-bed-high`**: the sliver of slab is gone, and the banks run in
  smooth curves with no zigzag; from above the line reads as a stream's. The shaded bank's face
  still shows the lattice as faint scallops.
- **`stream`**: the left tributary now bends on its way down and two pale boulders stand at a
  step where it meets the trunk; the trunk's banks no longer kink. **It still reads as a ribbon
  in a smooth fold.** The trunk here is a pool-riffle reach by its 1.2 % slope and 2 m wide, so
  its section's swing is centimetres on a 25 cm lattice; the water's ribbon is drawn at the
  hydraulic geometry's width whatever the cut's is; the right tributary, in a fold with sides, is
  still nearly straight; and the boulders, with no rock set in the pack for them, draw as smooth
  pale eggs.

**Against the reels** (r03's cobble river between turf banks; `riffles-and-pools`' bank of
boulders and cobbles under a raw scarp, boulders standing in the water, a white riffle) what a
person would object to first is not the form: the ground beside and under the water is bare and
smooth where the photographs are cobbles and boulders from the scarp to the thalweg, hundreds in
a frame, and the engine places a few dozen domes on a whole reach. After that: every view lacks
what the cut cannot give: a cobble bed seen through the
water, turf to the edge and an undercut, a riffle's broken water. Those are the masks', the
sward's and the water's.

## Reaches

(2026-11-08, roadmap W90; the POC's v35 engine-needs, item 10, and the owner's walk, marks 2, 3 and
8: on 30–40° faces the pieces stood as stepped slabs, with sand bars on mountainside reaches.) A
mountain channel's form follows its slope (Montgomery & Buffington 1997): **dune-ripple, pool-riffle
and plane-bed** reaches are alluvial and low-gradient, **step-pool and cascade** reaches are boulder
beds of steep streams, and above those a channel is **bedrock** (the flow carries more than it is
given, nothing stays) or, at the network's tips, **colluvial** (a hollow on a hillslope filled with
what fell in). Each point of a link takes its reach from the ground's fall per metre along the line
over a window centred on it, `reach_window_widths` (20) widths long and at least `reach_window_min_m`
(20 m) — Montgomery & Buffington classify a reach over many channel widths, so a lattice bump or a
single step does not reclass a point — by the layer's bounds:

| reach | fall per metre | what is laid |
|---|---|---|
| dune-ripple | below `slope_pool_riffle` 0.001 | bed, bars |
| pool-riffle | to `slope_plane_bed` 0.015 | bed, bars |
| plane-bed | to `slope_step_pool` 0.03 | bed (Montgomery & Buffington's plane bed has no bars) |
| step-pool | to `slope_cascade` 0.065 | bed, steps with boulders |
| cascade | to `slope_steep` 0.2 | bed, boulder clusters at irregular places (W91) |
| bedrock | above, on a link below a junction | nothing: the cut's faces, a chute; falls at knickpoints alone (W91) |
| colluvial | above, on a link that begins at a head | nothing: the cut's faces, a chute; falls at knickpoints alone (W91) |

The bounds and their sources are on [the grounding page](../experiments/channels.md#reaches); the
confinement of the valley (Montgomery & Buffington's second axis) is not read. Each `WaterChannel`
point carries its reach's code in `reach` (`engine.channels.ChannelReach`: 0 dune-ripple to 6
colluvial), for the flowing water to read; S98's regimes do not read it yet.

**Steps, on step-pool reaches alone** (W91; the owner's v36 walk, item 20: W90 laid them on cascade
and bedrock reaches too, and on v36's mountainside, bedrock at a median slope of 0.40, they made a
flight of square pools and 4 m block risers a width apart, 1,874 stretches the water drew as falls
where v35 drew 81 points). Montgomery & Buffington's cascade is disorganised and their bedrock and
colluvial channels have no alluvial steps, and the step relations below were measured at slopes of
about 0.03 to 0.2 ([the grounding](../experiments/channels.md#w91-re-grounded)). On a step-pool reach
a step every `L = w clamp(step_pool_widths sqrt(slope_step_pool / S), 1, 4)` metres — the 1 to 4
widths the step-pool literature reports, 2.5 at the step-pool's gentlest, 1.7 at its steepest — of
height `H = step_height_ratio S L` (1.5; Abrahams, Li & Atkinson 1995: steps stand between 1 and 2
times the slope over their spacing), so at most 0.1 L on the reach's 0.065. Below a crest the pool
lowers the cut's bed by H and the bed comes back up to the cut's line at the next crest, so the bed
is never raised and its mean fall is the ground's. **The riser is a slope, not a wall**: H drops over
`H / tan(riser_angle_deg)` (38°, a boulder step's face at its clasts' angle of repose), at least a
stretch of the line and at most half the run, so no riser is steep enough for the water to leave its
bed (45°): a step-pool's step is drawn as a steep rapid. The cut's section follows the bed point by
point and the tile set's 0.25 m lattice samples it, so a 1 m drop over its 1.28 m riser is five
lattice cells falling about 0.2 m each, a ramp of 38° in steps of the lattice, not a vertical face (a
step-pool's H rarely reaches 1 m: it needs a channel about 6 m wide). **Boulders**
(`ChannelPart::Boulder`, a dome slab 7 by 7 points): at each step-pool crest a row across the bed, as
many as the bed's width holds at the step's height (0.25–1.5 m, ±20 %), each jittered a fifth of its
size along and across, standing 0.6 of its width over the ground at the lip and buried a third; they
sample `rock/boulder` or `rock/granite`. They are the channel's own domes, not the scatter's rock
kinds, which a capability may not reach (ADR-0046).

**Cascades** (W91): no steps and no pools. Boulder clusters (`ChannelLink::cluster_at`) stand at
irregular places down the reach, their spacing drawn from an exponential of mean
`cascade_boulder_widths` (1.5) widths, at least half a width — a memoryless spacing has no rhythm —
each cluster one to three domes of one to two bankfull thalweg depths (0.25–1.5 m, ±30 %) anywhere
across the bed and a size along it: clasts that protrude through the water at most stages. The water
runs on over the cut's continuous bed; at a cascade's slope Manning's speed puts it past a Froude
number of one, which S98's surface draws as rough, aerated water among the domes, so the flowing water
needs nothing more than the reach code it already carries.

**Bedrock and colluvial reaches, a chute and its knickpoints** (W91). Nothing is laid and no step is
cut: the cut's bed follows the ground under the line (W89's rule), a chute. A fall is drawn only where
the ground itself has a **knickpoint**: over a window of `knick_window_widths` (3) widths, at least
`knick_window_min_m` (2 m), the ground's fall per metre below each point, Sd, and above it, Su; a
knickpoint is a run of points whose Sd is at least 1 (45° over the window) and that drops at least
`fall_min_m` (1 m) from its first point to a window past its last. Its first point is its lip, where
the profile turns convex (there Su < 1 ≤ Sd: the window's second difference is positive, and the
summary reports its mean as `lip_convexity_mean`); its points and the window below are marked
(`ChannelLink::knick`) and the bed follows the ground through them, so the water falls there. A gate on
the second difference itself (a lip only where Sd − Su exceeds 0.3) was tried and dropped: a face
that steepens by degrees then had no lip, and the chute's limit cut the whole face deeper. Knickpoints
are read on every reach, since a scarp inside a cascade is a fall too. **The chute's limit**: away
from a knickpoint, a stretch of the bed that would fall at 45° or more (a bump of the ground, the
lattice's 0.5 m noise) lowers the bed above it to `chute_gradient_max` (0.9, 42°) per metre, never
raising the one below and never more than half `fall_min_m` past the cut's two thalwegs, and the
lowering runs on upstream while it leaves the next stretch that steep; a stretch still that steep is
counted (`steep_left`).

## Mouths

A link that reaches a water body (the scene's `water`, through the placements' exclusion: a point
inside a body's outline whose ground is below its level, or anywhere in an open sea) **ends at its
shoreline**: the line is cut at the first point under water, and its last point is put on the
shoreline itself by bisecting the stretch that crosses it, the ground there being the body's level.
A link begun under a body (a lake- or sea-floor head) is **dropped**; a stretch that leaves a body
again (a lake's outlet) is a link of its own, appended after the network's links (`outlet`); a dry run
shorter than `max(4 m, 2 w)` is dropped. Nothing of a link — cut, pieces, water — continues under
the body.

The cut ends in a **fan**: over the last `fan_widths` (3) widths its bed grades from where it was to
half a thalweg under the body's level, and its width grows by `fan_spread` (1, so to twice) at the
shore. The water's surface grades over the fan to the body's level at the shore, and on a link that
ends in a body never stands below that level anywhere (the body backs it up). The grade starts at
the first fan point itself and never runs under the bed: at least a centimetre of water stands over
it (W91 continuation). W90 anchored the grade at the first fan point but already applied that
point's own share of the fan, so the surface dropped by the share at once; on a chute that follows a
steep face that was more than the water's depth — v36's link 690, falling at 0.72 m/m into the
eastern sea with 0.19 m of water, dropped 0.43 m and ran under the cut's bed for five points, which
wrote a depth of 0 that the water capability refuses (`systems/water/src/channel.cpp`, "depth_m …
must be positive"). W90's stepped beds never showed it; W91's chutes, which follow the ground, did.
The subcommand counts any depth that is not positive (`depths_not_positive`; 0 on v36 since). The
source of the fan's length is on [the grounding page](../experiments/channels.md#mouths-and-confluences).

**A ribbon's end a body covers is trimmed** (W91 continuation; the POC's item 25, flat mint shapes
at mark 7). A point inside a body's outline whose surface lies under the body's level is under the
body's own water — a link that begins on a beach inside a lake's outline, its ground above the level
so the mouth rule keeps it, whose cut then digs its water under the level. Drawn, its ribbon is a
second water surface a few centimetres under the body's, and with ray tracing on, the body's
refraction trace meets it as a solid floor and shows its albedo with no water shading (see item 25
under [Against the proof of concept](#against-the-proof-of-concept)). `water_channels_json`, given
the bodies, trims such points off both ends of each ribbon (the exclusion's water kind asked against a
ground standing at the point's surface, so an open sea covers whatever lies under its level), writes
no ribbon left with fewer than two points, and counts what it trimmed (`RibbonTrim`; the
subcommand's `covered_trimmed`, by body). The cut is not trimmed: the body's surface shows in the
trench, which is right. Only the ends are trimmed, since a run covered in the middle of a line would
be a body the line crosses, which the mouth rule has already ended it at.

**The bodies' outline is the exclusion's** (scene_gen.md, "Exclusion"): a fill body is its seed's
box where the ground is below the level, not the flood `water::depth_at` traces, so a hollow below a
lake's level within its box ends a link as the lake would. The placement generator reads them from
`Context::exclusion`, which the host builds only with placement exclusion on: without it the pieces
run on (the subcommand always reads the scene's bodies).

## Confluences

At each junction the incoming link of the largest contributing area is the **trunk**, which runs on
as the link that begins there; every other incoming link is a **tributary** of that link
(`ChannelLink::trunk`). Junctions are taken by the downstream link's area, largest first, ties by
index. A tributary's water and pieces stop at its first point within half the trunk's width of the
junction (`keep`), its bed is lowered — never raised — over its last `confluence_widths` (3) of its
own widths to the trunk's bed, reaching it at that point and holding it to the junction, and its
surface is lowered the same way to the trunk's surface there (keeping a centimetre of water), so the
two ribbons meet at the trunk's bank line without overlapping. The tributary's cut runs on to the
junction, where the tile set's union of sections opens it into the trunk's.

## The planform

The meander's wavelength, 10.9 widths (Leopold, Wolman & Miller 1964; 10–14 in Leopold & Wolman
1960), and its amplitude vary along the line by `meander_wavelength_variation` (±25 %) and
`meander_amplitude_variation` (±50 %) of themselves, a value noise of the link's seed with a knot each
bend, so no two bends are alike; the owner's walk saw W86's single wavelength per link as a regular
sawtooth from the air.

**The line held in its trough** (W91; the owner's v36 walk, item 21: lines across a mountainside stood
out of it). The meander's trough test (an offset that would climb more than half the depth out of the
valley is halved) saw the meander's offset alone, and the low-pass carries a line off the floor by
itself where it cuts a bend's corner across the valley's side. Every point of the final line, after
the low-pass and the meander, is now tested the same way against the trough the lattice's cells run
down: the ground at the point against the ground at the nearest point of the cells' own polyline
(within the low-pass's reach of its segment); a point standing more than half the depth over it is
drawn back toward it, halving its offset up to three times and then onto it. The shares are eroded over
three points each way and smoothed by three binomial passes, which leaves every point drawn back at
least as far as its own test asked. On v36 it drew back 78,693 of the network's 287,000 points.

**The kinks taken out** (W93, continued; the owner's pictures: "the banks zigzag"). What the trough
test leaves turns left and right again inside two or three widths, and the cut's banks follow the
line. After the test the line is low-passed once more at the channel's own scale: a Gaussian
along its length of sigma half a width (two steps at least), eased to nothing at the two ends,
which stay where the junctions are. Half a width is far under a meander's wavelength (a bend 10.9
widths long keeps 96 % of its amplitude) and moves a point by a fraction of the width, before
the banks' test ([A line across a slope](#a-line-across-a-slope)) which then holds it. **Held**
("the line's turn between consecutive points is bounded on every reach", seven valleys, four
seeds each): no radius of curvature between consecutive points under half a width (of a metre at
least); the tightest measured is 1.02 widths. On the test valley the points with less than half a
downhill bank are 36 of 1,969 (32 before) and those with none 0 (12 before).

**A steep line wanders** ([The steep reaches](#the-steep-reaches)): where the slope allows no
meander the line takes an irregular offset instead, held in its trough by the same test.

## Against the proof of concept

`engine-content channels` over a copy of v31's terrain-only scene (`repro-terrain-hex/scene-norot.json`)
with two layers at the defaults — the lowland forest, x −1,536 to 512 m and z −1,792 to 1,024 m, and
the fells above it, z 1,024 to 3,072 m — 9.96 km² in all (2026-11-07, `msvc-debug`):

| | Channels (W86) | The POC's erosion model (v31) |
|---|---|---|
| heads | 198 (19.9 per km²) | 688 over 38.5 km² (17.9 per km²) |
| channel | 48.7 km, **4.9 km per km²** | 5–7 km per km² |
| links | 360 | — |
| blocks ≥ 50,000 m² | 215 | — |

**v36, W90 against W91** (2026-11-08, `msvc-debug`, `engine-content channels --min-area-m2 1000000
--cut` over a copy of the POC's v36 scene, one layer of 38.5 km², 784 links, 126 water blocks of
50,054 points; "W90" is the v36 water as shipped, measured with `--measure`, and the W90 code's own
run for the steps and boulders; edges are measured against the ground before the cuts, the region's
border, two lattice spacings, apart):

| | W90 (v36 as shipped) | W91 |
|---|---|---|
| stretches the water draws as falls (≥ 45°) | 1,874 (bedrock 1,654 of 8,981, cascade 204 of 7,381, step-pool 6, dune-ripple 10) | 25 (bedrock 16, dune-ripple 7, cascade 1, pool-riffle 1) |
| falls (runs of them) | 1,865 | 15 |
| highest single drop | 3.91 m | 1.25 m |
| steps laid | 23,395 (bedrock 13,790, cascade 8,334, step-pool 1,271) | 1,287, step-pool alone |
| boulders | 23,220 (rows at crests) | 17,822 (step-pool rows, 6,649 cascade clusters) |
| knickpoints (every link) | — | 172 |
| ribbon edges over the ground by > 0.3 m | 2,979 of 100,022 (3.0 %) | 0 of 100,020 |
| by > 0.05 m | 7,138 (7.1 %) | 0 (none over 0.03 m) |
| the most | 2.63 m, link 377 at (195.6, 5,040.4) | 0.03 m |
| at the region's border (links 346 and 355) | 40 edges over 0.05 m, 5.65 m at x −1,532 | 0 |
| points fitted | — | 14,005 narrowed or moved (median to 0.72 of the width), 47 lowered under the downhill ground |
| line points drawn back into the trough | — | 78,693 of 286,904 |

The falls left are the knickpoints' (bedrock), the mouths' fans grading to a lake (dune-ripple) and a
few stretches the chute's limit could not spread (`steep_left`: 20 over every link's bed).

**The continuation's re-cut** (2026-11-09, the same command after the fan's grade and the covered
trim, [Mouths](#mouths)): the cuts are byte for byte the ones above (so the tile set's key and its
cook are unchanged); `depths_not_positive` 0 where the first W91 run wrote 5 (link 690's fan, points
273–277, which the water capability refused at load); 86 ribbon points trimmed off 6 ribbons'
ends, 83 under `mountain-lake-east-extension` and 3 under `freshwater-swamp`, none under the open sea,
no ribbon dropped and **no mouth lost any of its fan** (a fan grades to the level and never stands
under it); link 779's ribbon now begins about 6 m down its line, at the lake's outline. Every other count
in the table stands (ribbon edges measured 99,848, the trimmed points' 172 fewer).

**Item 25, the flat mint shapes at mark 7** (the POC's list; `before\captures\00008-m7-eye.png` of
the W91 scratch). The ids at five of the shapes' pixels name instance 5, cluster 1252970 — the
`water/mountain-lake-east-extension` body, not a channel piece and not a ribbon — and the shallow
water around them is the same instance, cluster and even triangle, so it is not a material row or a
normal. Their colour, about rgb(150, 199, 184), is the body's default shallow albedo (0.30, 0.55,
0.50) in sRGB, unlit. Met with the lake's plane, every such pixel lies 0.15–0.32 m from link 779's
centreline, where the shipped ribbon stood 0.149 m under the lake's level inside its outline (the
others 0.78–1.46 m off it): with ray tracing on, the body's refraction trace (S86 part 2, water.md
"visibility through the water") hits that ribbon as a solid surface and shows its albedo with no
water shading. The channels' half is fixed here (the covered ends trimmed). **The water stage's half
is a request to S100**: a refraction ray that hits another water surface — a ribbon or a body — under
the water should pass through it or shade it as water, never as an opaque albedo
(`domain/gfx/shaders/water.slang` and the water stage of `visibility_resolve.slang`, the
refract-volume hit's shading); a ribbon under a body anywhere else (a confluence's, an author's) would
show the same.

**Seen on v36** (2026-11-09, `msvc-release` at the continuation's commit, 1920×1080, `--raster hw
--shadows rt`, the common engine flags, `--offscreen`; "before" is eeb8077c over v36 as shipped,
"after" W91 with the re-cut, the same 16 poses; the machine shared with other agents' GPU work, so
no timing is read from these runs). **Bedrock** (`00010`): before, a wide flat ribbon plate over the
beach ending in a square edge with a drop toward the sea; after, the water lies narrowed inside its
trench as a continuous chute to the beach, no plate and no fall — the trench still ends in a square
cut at the shore, as before. **Cascade** (`00011`): before, white fall slabs down a staircase of
blocks beside a tree; after, one continuous sheet of rough white water down the slope, no step, no
riser. **Step-pool** (`00012`): a straight cut with boulder rows across it and pools between, the
water continuous — the reach W91 kept its steps on, unchanged to the eye. **Colluvial** (`00013`): a
shallow cut under the trees with a thin rough ribbon in it, no step. **Mark 4** (`00002`, item 21):
before, the ribbon stood as a bent plate over the ground in front of the cut's wall; after, the water
lies inside the banks, lower and narrower along the channel, the pool on the left inside its rim —
the plate is gone; the cut's tall bank wall behind it is the same in both (W91 did not move the
cut's banks: "Not yet", below). **Mark 7** (`00008`/`00009`, item 25): the mint shapes, 34,405
pixels of rgb(150 ± 14, 199 ± 14, 184 ± 14) in `00008` and 15 in `00009` before, are 0 and 0 after:
the lake's water shows its own bed in the trench. **The mouth** (`00014`, after only): the fan a shallow sheet
over the sand into the sea, no mint before or after. The gentle **stream** (`00015`) looks as before
(the pale bed-piece edge along its left bank is in both). Nothing
was seen that is worse. Captures: the W91 scratch's `before\captures` and `c2\after\captures`.

The lines lie in the model's troughs: the nearest channel point is 1.1 m from the v34 hand-off's
fell-rill camera target (a 1.3 m rill, below the banks' threshold) and 6.5 m from its channel-bars
target (a 3.0 m bed, where the paint drew 5.2 m: the forest layer's region cuts off part of that
channel's catchment). The density is a little under the model's because a region's edge cuts the
catchments that flow in across it. The captures and their verdict are in the W86 row's report.

## Invariants

- The network is a function of the layer, the seed and the ground's answers at the lattice's
  millimetres: the same at one job and six (test: "the same network at one job and six").
- Every channel cell drains to the edge through channel cells; a head has no channel donor; a link
  ends at a junction or the edge (test: "one channel down a V-notch").
- Width and depth follow the hydraulic-geometry power law exactly at every point (same test).
- Every placed mesh is closed, two-manifold, outward and has one component per slab (test: "the
  placed pieces are closed, welded and outward").
- `expand` equals the union of `tile` over any order, with ids 1, 2, … (test: "whole expansion
  equals any tile order").
- The `WaterChannel` blocks have seven fields (W86's six and W90's `reach`; an eighth, W91's `fit`,
  when the ground is handed in), one value per point, and a surface that never rises downstream
  (test: "the water's description has the WaterChannel shape"; the subcommand's test).
- Reaches by the pinned bounds; steps on step-pool reaches alone, 1–4 widths apart with H / L / S
  between 1 and 2 and no riser steep enough to fall; a cascade with irregular boulder clusters, no
  steps and no bars; a 35° face colluvial with nothing laid (tests: "reaches by the pinned bounds", "a
  step-pool's steps …", "a cascade gets boulder clusters …", "a 35-degree face …").
- **No staircase on a bedrock or colluvial reach** (W91): no step; the bed falls at 45° or more only at
  a knickpoint of the ground, and the water draws one fall there (tests: "a bedrock chute falls only at
  its knickpoint"; `apps/engine_content/tests/channel_reach_tests.cpp`).
- **Every point of the line stands within half the depth of the trough its cells run down** (W91;
  [The planform](#the-planform)), and **no ribbon edge stands more than 3 cm over the ground beside
  it** where `water_channels_json` is handed the ground, as the subcommand always does: an edge over
  lower ground is drawn in to where the ground meets the surface, the point moved and its width
  narrowed to the two edges (`fit` 1); where they leave less than a quarter of the width (at least
  0.2 m) the bed stands over the downhill ground, and the surface is lowered to the lower edge's
  ground less 2 cm (`fit` 2; the description's depth kept at a centimetre, the water under the ground
  there). A point under a water body is left as it is. The surface never rises downstream except
  over a sill and at a lowered point (test: "a line across a tilted plane stays in its trough, the
  ribbon on the ground").
- A link ends at a lake's shoreline with nothing under its level, its fan grading the bed and the
  water to the level; a drowned link is dropped; no bodies, the same network to the byte (test: "a
  link ends at a lake's shoreline …"; `apps/engine_content/tests/channel_mouth_tests.cpp`).
- **Every depth the description writes is positive**, a fan down a 35° face included: the fan's
  grade starts at its first point and keeps a centimetre over the bed (test: "a fan down a 35-degree
  face keeps water over its bed to the shore"; the subcommand's `depths_not_positive`).
- **No ribbon point lies under a body inside its outline** at either end of a ribbon: such a run is
  trimmed, exactly that run, and everything upstream is written as it was (test: "a ribbon's end
  under a lake inside its outline is trimmed, and only that").
- A tributary ends at the trunk's banks, its bed lowered to the trunk's and its surface lowered,
  never raised, to the trunk's (test: "a tributary meets its trunk …").
- The cuts have the `TerrainCut` shape, one per link, the top at the ground under the line, a bed that
  never rises downstream and a thalweg of the hydraulic geometry's area; the water in the cut lies
  between that bed and the top (test: "the cuts have the TerrainCut shape and the water lies inside
  them"); no bank piece, and the bed narrower than the channel (tests: "the placed pieces …", "the bed
  lies inside the cut's banks"); `engine-content channels --cut` is the in-process network's text, the
  same twice and the same from a scene that carries the cuts (`apps/engine_content/tests/
  channel_cut_tests.cpp`).

- **The section follows the line** (W93): at a bend (a turn of 30 degrees between the line's
  heading two widths above a point and two below) the outer bank is the deeper side, on 95 % of
  bends at least over 12 seeds; pools stand 5 to 7 widths apart on average; the section's area is
  the hydraulic geometry's within 4 % from riffle to pool, a bend's apex twice the mean depth deep;
  the gentler bank is the inside one; a riffle is at most `riffle_widening` wider than a pool; the
  water stands under the top and over the cut's deepest point (tests: "at a bend the outer bank is
  the deeper side, and pools stand 5 to 7 widths apart", "the section keeps the hydraulic
  geometry's area from riffle to pool").

- **The flood bed follows the confinement** (W93): on valleys with floors of 32, 9 and 2 m the
  entrenchment ratio read from the ground is the geometry's to 3 %, and the bed is three widths,
  the floor less its scarps, and none; every point 0.9 of the bed's reach from the ribbon's line
  is excluded by the exclusion built from the written description; **and the cuts' text on each
  of the three hashes to a pinned value**, taken on MSVC and reproduced by Clang and GCC in the
  containers (test: "the flood bed's width follows the valley's confinement").
- **Neither bank stands more than half a depth under the line** but at 2 % of points at the most,
  and under the bed at 1 % (W93; test: "a line across a slope is moved between its banks").

**Public API.** `include/domain/channels/channels.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `jobs`, `assets`,
`geometry`, `materials`, `texture`, `scene_gen`, `channels_schemas`.

**Testing.** `tools/dev.ps1 test -Filter channels` (the module's tests and engine-content's
`channels` end to end).

**Performance notes.** Nothing runs per frame. Opening an entry costs the lattice's height queries
(one per point, rows on the job pool), a heap fill and a sort over the points, and a few height
queries per placed vertex; the region's `max_samples` bounds it. The pieces are ordinary cluster-LOD
scene instances. `ChannelPoint` (40 bytes) is pinned in the size table. **W93's share of the
plan** (2026-11-10, `msvc-release`, `engine-content channels --cut`, the process whole, best of
three, the machine at 34 % CPU beside other agents' builds): on the row's scratch scene (0.098
km², 475 m of channel, 7 links) 0.73 s on the base and 0.78 s with the section, the flood bed's
walks (a station a width, a sixth of a width a step) and the banks' test (three ground queries a
point and try, four tries, twice). It was **not measured on the proof of concept's world**, where
the plan is 24 to 32 s: the row may not run that world's files. What scales with the world: the
banks' test with the line's points a metre wide or more, the flood bed's walks with the metres of
alluvial line 2 m wide or more.

**Not yet.** The cut is written by the subcommand and pasted by the author, not applied by the generator itself (the ground is built before the placements are); a procedural ground (the dunes) takes no cut; D8 only (no D∞ spreading); heads by a local four-metre slope, not a slope
averaged over the source area; no braided reaches; the valley's confinement is read for the flood
bed ("The flood bed") but not for the reach's class, which is the slope's alone; a bend with a point bar is cut 6 % narrower than its crossings where a surveyed river's
is wider ([the grounding](../experiments/channels.md#the-steep-reaches-the-pools-place-and-three-numbers-read-again)); undercut and
turf banks are not the heightfield's to draw; the line is moved between its banks by a width at the most, so where the trough's floor is
further off than that a bench with half a bank is left (1.8 % of the test valley's points, and
none with no bank; the ribbon's fit still narrows the water there); knickpoints are read from the ground's profile along the line, not
from rock strength; a plan named by an entry on cut ground cannot be made again at a load
([One plan](#one-plan): `scene_gen::Context` hands a generator the ground it stands on and not
the ground before its cuts); the mouths read the exclusion's box for a fill body, not its traced flood.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Component and event types | `domain/channels/schemas/channels.schema`: the layer and the recipe, no components | done |
| Tick scheduler entry | none | not needed: nothing ticks |
| Render-graph passes | none | not needed: its meshes are scene instances |
| Content-build derived step | the mesh generator `channel` (`.channel.json`) | done |
| Protocol methods | none | not needed |
| Tunables | none | every number is the layer's |
| LOD policy | each piece's cluster LOD DAG | done |
| Determinism | integer lattice and total orders; same bytes at any thread count and tile order | done |
| Zero cost when unused | a scene with no `channels` entry never opens it (the registry) | done |
| Tests and size table | `tests/channels_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | none | nothing per frame |
| Removal proof | `ENGINE_WITH_CHANNELS`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_CHANNELS=OFF`) drops the
module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and
is listed there under `disabled_capabilities`. A scene naming `channels` is then refused with the
registry's sentence and `engine-content channels` with one naming the capability.
