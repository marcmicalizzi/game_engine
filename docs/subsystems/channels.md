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
2. **The fill.** Priority flood from the region's edge **and from every sink** with a
   one-micrometre epsilon (Barnes et al. 2014), heap ordered by (height, index): every other cell
   gets a strictly falling path to one of them, and the order is total.
3. **The routing.** D8 over the filled heights, ties to the first direction; the region's edge and
   the sinks are the outlets. Contributing area summed after sorting by filled height.
   **Then the hollows** ([Hollows](#hollows), W122): every pit the fill raised is labelled and
   becomes a breach, a standing body of the plan, or a sink; a pit made a sink changes the fill, so
   steps 2 and 3 are taken again until nothing changes.
4. **Heads.** A cell is a channel where `A ≥ min_head_area_m2` and `A · S^α ≥ head_threshold_m2`,
   `S` the ground's (unfilled) drop to its receiver over the run; every cell below a channel is one.
   A head is a channel cell with no channel donor. **No channel begins under a body of the plan**:
   the slope the rule reads there is the lake's floor's.
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
  with every per-point array, the heads, the counts and the hollows (W122: every hollow that is
  not simply breached, with its kind, its level and its balance), as integers and IEEE bits,
  little-endian, closed by a hash of the whole (`plan_bytes`, `read_plan`); the lattice's own arrays (heights,
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
subcommand", is not built: the terrain's `cuts`, the scene's `water_channels`, the entry's `plan`,
where the layer names its bed's material the masks' `mask_regions`, and since W122 the plan's
standing bodies for the scene's `water` ([Hollows](#hollows)) are five things the author pastes
from one output. Closing it means the ground is made in
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

(2026-11-13, roadmap W120; the owner's proposal after walking batch 63; the grounding is
[stream-profile](../experiments/stream-profile.md).) Until W120 a channel's water was a description
laid on a bed that was cut separately: a level at a share of the cut's depth, clamped never to rise
downstream; a described width; a described parabola for its depth; wet where a catchment passed an
area. The owner walked it as steps, overhangs, water going uphill and beds with no water, and five
rows mended one symptom each of the description not being the ground. **The description is now
solved**: `engine.scene.WaterChannel` (version 6; 7 adds `derived_from`, "The cook") carries, at every station, the steady
one-dimensional flow of the station's discharge through the ground's own section there.

| Field, one entry a station | What it is |
|---|---|
| `points[i]` | the line, x and z to the millimetre; **y is the solved level**, and there is no second level |
| `width_m` | the cut's bankfull width (the exclusion's bank reach reads it; the draw does not) |
| `depth_m` | the solved greatest depth: the level less the lowest ground connected to the line under it. No parabola: the bed is the ground |
| `slope` | the friction slope. No reader derives a speed from it |
| `discharge_m3s` | what the sources upstream send through the station |
| `speed_mps` | the section's mean speed, Q / A |
| `froude` | the speed over sqrt(g A / T), T the surface's width, g the falls' 9.81 |
| `wet_left_m`, `wet_right_m` | metres from the line to the wet edge, left the side of (-dz, dx) (W93's). Only ground connected to the line under the level counts |
| `flags` | what the station is, below |

**The flags** (`domain/scene_gen/water_channel.h` holds the constants, so the writer, the draw and
the scene's reader share one copy):

| Bit | Value | Meaning |
|---|---|---|
| 0 | 1 | a **control**: a station the flow is solved from |
| 1, 2 | 0, 2, 4, 6 | the control's kind: 0 a **crest** (critical depth over a step's or a fall's lip), 1 a **mouth** (the body's level), 2 a **confluence** (the main stem's level), 3 a **spill** (a pool's outlet) |
| 3 | 8 | a hydraulic **jump's head**: the supercritical level |
| 4 | 16 | the **jump's foot**: the sequent level, a jump's length downstream |
| 5 | 32 | a **fall's lip** |
| 6 | 64 | the **fall's foot**: where the channel's surface resumes |
| 7 | 128 | **over bank**: the solved level stands over a bank's top here |

**A discontinuity is a pair of neighbouring stations**: a head is always followed by its foot and a
lip by its foot. Between a fall's lip and its foot there is no channel surface (the falls' cook,
S104, owns the water in the air). A tributary's last station carries the main stem's level there
to the millimetre, and a mouth's the body's.

### Stations

The profile is solved on nodes one ground lattice apart, and the description keeps the fewest of
them that hold three **pins** (`domain/channels/stations.h`, `select_stations`), so a reader joins
neighbours by straight lines and needs no curve:

- between two neighbours that are not a jump's or a fall's pair, the straight line stands within
  **1 cm** of the profile's level (`k_station_level_m`) and within **5 cm** of each wet reach
  (`k_station_wet_m`) at every node between them;
- two neighbours are never farther apart than **8 m** along the line (`k_station_max_m`);
- and never closer than the ground's lattice, since a station is a node and nodes are a lattice
  apart (a jump's two stations stand at the jump's own places, which the nodes about them keep a
  lattice clear of).

The selection is greedy from upstream and holds each chord to the pins less a millimetre, which is
what rounding a level and a reach to the description's millimetre may move. On the fixture below
the three channels keep 6, 12 and 11 stations of 161, 182 and 321 nodes (150, 240 and 138 a
kilometre): a uniform reach and a backwater are a station every 8 m, and the drawdown to a crest
takes them as close as its curve needs (three in the last metre).

### The fixture

`engine-content flow fixture <dir>` writes a scene — `ground.json` with its grid, and `scene.json`
whose terrain is that heightfield through the `tiles` provider — holding three solved channels
and two bodies. **Every number in it is from the closed forms of steady flow in a trapezoid**,
evaluated directly and integrated by the direct step method (`domain/channels/flow_fixture.h`), and
none is from the solver: it is what the solver's cases are held to, and what a draw can be built
against before the solver is whole. It is written by a command and not committed because its
ground is a binary grid.

**The ground** is 96 m by 60 m at a quarter of a metre: three trenches along +x at z = -20, 0 and
20 under a flat cap at y = 13, each a trapezoid with a flat bed b = 2 m and banks rising a metre a
metre (z = 1). The ground is `min(13, bed(x) + max(0, |z - z_line| - half(x)))`, a sum of a
function of x and a function of z that are each linear between lattice points, so the lattice's
bilinear surface **is** the trapezoid and a section sampled from the ground is exact.

**The flow** is one discharge in all three, with Manning's n = 0.04 held everywhere (the fixture
tests the profile, not a resistance law). With A = (b + z y) y, P = b + 2 y sqrt(1 + z^2),
T = b + 2 z y:

- **Normal depth is chosen**, y_n = 0.5 m on a slope of 0.01, and the discharge follows from
  Manning's equation: A = 1.25 m2, P = 3.4142 m, R = 0.36612 m, Q = (1 / 0.04) A R^(2/3) 0.01^(1/2)
  = **1.59930 m3/s**. The speed is 1.279 m/s and the Froude number 0.633: a mild reach.
- **Critical depth** is the root of Q^2 T = g A^3: **y_c = 0.37674 m** (A = 0.89541 m2, T = 2.7535 m;
  both sides 7.0427).

**"uniform"** (z = -20, x 8 to 48, bed 10 - 0.01 (x - 8)): normal depth at every station, the level
10.5 falling to 10.1, six stations 8 m apart, no flags. The friction slope equals the bed's.

**"step"** (z = 0, bed 12 - 0.01 (x - 8) to a crest at x = 38, a face dropping 2 m over one lattice
cell, a level apron at 9.7 to a mouth at x = 58 in a pool):

- *The approach* is an M2 curve: critical depth at the crest (station 6: level 12.077, Froude 1.000,
  flags 33 = control, crest, fall's lip), and upstream of it the direct step
  dx = dE / (S_0 - S_f) summed from y_c toward y_n in 40,000 equal steps of depth. At the head,
  30 m upstream, the depth is 0.49860 m: 1.4 mm under normal.
- *The fall's foot* is Rand's (1955) drop, in the reading [stream-profile](../experiments/stream-profile.md)
  gives it for a section that is not a rectangle. The hydraulic depth at critical is
  D_c = A_c / T_c = 0.32519 m and the drop h = 2 m, so the area at the foot is
  0.54 (D_c / h)^0.275 A_c = 0.29341 m2, a depth of **0.13728 m** at 5.451 m/s, Froude 4.85; the
  foot stands 4.30 h (D_c / h)^0.81 = 1.975 m from the lip, taken up to the lattice: x = 40
  (station 7, flags 64).
- *Below the foot* the water shoots along a level apron and thickens (an H3 curve by the same
  direct step with S_0 = 0): its level **rises** downstream, 9.837 to 9.880 over 1.25 m, which is
  what shooting water does when friction slows it.
- *The jump* stands where the shooting depth's sequent depth — the depth on the tranquil side with
  the same specific force M = Q^2 / (g A) + b y^2 / 2 + z y^3 / 3 — is the depth the pool's
  backwater holds a jump's length downstream, the length 6 (y_2 - y_1). The head is at x = 41.246
  with 0.18004 m (Froude 3.19; station 8, flags 8); its sequent depth is **0.67247 m**; the jump is
  2.955 m long and its foot is at x = 44.201 (station 9, flags 16), level 10.372.
- *The pool's backwater* is an H2 curve from the mouth back: 0.6 m deep at the mouth (station 11,
  level 10.300, the pool's level to the millimetre; flags 3 = control, mouth).

**"mouth"** (z = 20, x 8 to 88, bed 10 - 0.01 (x - 8), into a lake at 10.4): an M1 curve from 1.2 m
at the mouth (station 10, flags 3) back toward normal depth, 0.54378 m at the head 80 m upstream.
The level falls 14.4 cm in 80 m where the bed falls 80 cm.

The fixture's own case (`flow_fixture_tests.cpp`) checks each closed form against a second
evaluation — Manning's equation at the normal depth, Q^2 T = g A^3 at the crest, the specific
force either side of the jump, the lattice against the ground's function — and the solver's cases
solve the fixture's ground and are held to these stations.

### What refuses a description

`scene_gen::water_channel_solved` is the one check, made by `water::read_channel` before a channel
is drawn: fewer than two points; a per-station array that is not the points' long; a
width, depth or discharge that is not positive; a negative slope, speed, Froude number or wet
reach; a flag bit the build does not know; a head or a lip without its foot. **A block with none
of the solved arrays is refused by name, "cooked before W120: cook again"**: nothing is read in the
old meaning (`fit`, version 3's record of a ribbon narrowed onto the ground, is gone with the
ribbon). A block that names only a discharge is an author's line waiting for its solve, and says
so.

### Who writes it, and what a scene without one costs

**The solver takes a line, a discharge and a ground.** The channels planner is one writer of lines;
a `WaterChannel` an author writes by hand on an imported terrain — its `points` and a
`discharge_m3s` of one entry or one a point — is solved by the same command, and gets everything a
planned link gets. A scene with no `water_channels` reads none, solves none and pays nothing; the
check is a few comparisons a station at load. `engine-content flow solve` is the writer
("The cook", below).

**A recipe whose banks are not the defaults** writes `bank_width_widths` and `bank_width_m` on every
block (`WaterChannel` version 4; T78, 2026-11-09): they are the reach the placements leave bare beyond
the cut's edge ([scene_gen](scene_gen.md#exclusion)), which `scene_gen` reads from the description
because it may not name this capability.

### Discharge

(W120 stage A; `discharge.h`, `network_flow`.) **The water comes from somewhere.** The discharge at
a point of a link is

    Q = stage x (runoff x the point's contributing area + the springs and outlets upstream)
        + the melts upstream x each one's season

and it is conserved at every confluence by construction, since the contributing areas are. The
numbers are the channels entry's `flow` block (`engine.channels.ChannelFlow`):

- **`runoff_l_s_km2`**, the specific runoff, 30 by default: a humid temperate mountain's (the Swiss
  atlas's national mean is 32 and its basins run 8 to 73; a semi-arid basin is under 3).
- **`stage`**, the stage of the year: today's flow over the year's mean, 1 by default. No ratio of
  the mean annual to the bankfull discharge was found in a source, so none is assumed: the cook
  solves today's water through the bed as it is cut and reports every station whose level stands
  over a bank, which is the bed's bankfull flow measured rather than supposed.
- **`sources`**, the point sources a scene names (`ChannelSource`): a `Spring` and an `Outlet` (a
  lake's spill) with a flow that scales with the stage, a `Melt` with a flow at its peak and its
  own `season`, since melt is highest when rain-fed flow is lowest. A source joins the channel
  point nearest it (within `snap_m`, 25 m) and everything downstream, through every confluence,
  to a mouth; one that stands on no channel is refused by name with the distance. **A lake of
  the plan passes it on** (W122): a source's flow that reaches a mouth into one of the plan's
  own lakes at its spill goes on into that lake's outlet (of the lines that leave the lake, the
  one that begins nearest its spill) and everything below, through the next lake too, and the
  lake's loss is taken off below it as it is from the runoff; so does a source that joins an
  outlet, whose line holds no cells, into the link below it. Until then a lake fed by springs
  stood at its spill (by its balance, which counts them wherever a loss is named, and with no
  loss named as every lake does) over an outlet that ran with its catchment's runoff alone (the streams' judging scene: 0.66 m3/s in, 14 l/s out). **A scene's
  own body passes nothing on**, and neither does a body under its spill, which has no outlet:
  what leaves an authored lake is the author's to name as an `Outlet` at its lip.

**A link is wet from where its water is a sheet.** The threshold is the falls' own
(`k_fall_min_critical_m`): a discharge whose critical depth over a metre of the bed's width is a
centimetre, sqrt(g d^3) = 3.13 l/s a metre (`wet_depth_m`). Under it the water runs between the
bed's stones and not over them. At the default runoff a bed a metre wide is wet below about
0.10 km2 of catchment, the low end of where a survey of Colorado mountain streams found flow to
begin (0.1 to 0.5 km2, read as a snippet). `--min-area-m2`, an area with no bed in it, is gone.
A link that never reaches a sheet is **dry and says so**: the cook lists it with its discharge and
the film's depth, as an ephemeral gully. Its cut stays as the plan made it; dressing it as a gully
— no fresh gravel, no clasts, the sward down its banks — changes the bed's mask regions and the
tile pieces and is not done in this row.

`flow` is **not part of what a plan is keyed by** (`plan_layer_hash` leaves the field out of the
hashed text), so naming a spring or changing the stage re-plans and re-cuts nothing — **unless
the entry names a loss** (`basin_evaporation_mm_y`, `basin_seepage_mm_y` or a basin's `leak_m3s`;
W122, "Hollows"): then whether a hollow fills, and where a lake with no outlet stands, is the
runoff and the sources against the losses, a lake's shoreline is where its lines end, and
`runoff_l_s_km2` and `sources` are part of the key. A named plan is then refused under another
runoff or another source, and the refusal says so and what to run (W129, 2026-11-15: the forest
names a seepage, and its session read the old sentence above as a promise). **`stage` is never
in the key**: a lake stands at the year's mean balance, and today's water is today's, so more
water over a plan, on any entry, is a higher `stage`. What a finer key would need: the plan
holding the balance it was made from and each hollow's share of every source, so that a spring
that reaches no losing hollow passes; it is not built.

### The profile

(W120 stage B; `profile.h`, `solve_profile`.) Steady, gradually varied flow in one dimension
through **the sections the ground has**, by the textbook's method
([stream-profile](../experiments/stream-profile.md)):

- **Nodes** a ground lattice apart down the line. At each, the ground is sampled across the line a
  lattice apart. The **thalweg** is the lowest sample within the bed's half width of the line (the
  nearest hollow to the line where the line names no width); each **bank's crest** is the highest
  ground reached walking out from it before the ground falls away by 2 cm and a quarter of what
  it has climbed (so a ledge on a high bank's face is not its top). A level wets only ground
  connected to the thalweg under it, and no further than the crests: a hollow beyond a bank is
  not wet, and water is never written outside its bed. Area, wetted perimeter, top width and first
  moment are sums over the wet samples' trapezoids; there is no described shape.
- **Critical depth** where Q^2 T = g A^3. **Resistance** is Jarrett's (1984) n = 0.32 S^0.38
  R^-0.16 by the bed's own slope over 10 m either way and the section's hydraulic radius, each
  held to his data's range; one Manning's n instead with `--resistance manning`.
- **The standard step method both ways**: tranquil water upstream from every control, shooting
  water downstream from every node the tranquil profile passes critical depth at and from every
  fall's foot.
- **The regime by specific force.** Shooting water holds while its sequent level — the tranquil
  level with the same specific force — stands over what the tranquil profile holds a jump's
  length downstream; where it no longer does, a **jump**: its head between the two nodes the test
  changed at, its foot six rises on at the sequent level (HEC-RAS's comparison of specific force
  at one section is this rule with a jump of no length). Where shooting water meets a node it has
  not the head to pass even at critical depth (a **choke**), the jump stands at the last shooting
  node that still has the head the tranquil profile holds below it, so the total head never
  rises; a fall's foot with less head than that is drowned.
- **A fall** where the bed drops at a metre a metre or more by at least three critical depths, the
  critical depth at least a centimetre (S104's test, at the solved critical depth and not a share
  of a cut): critical at the lip, Rand's (1955) drop at the foot, no surface between. A foot the
  tailwater's sequent test drowns stands in tranquil water: a plunge pool.
- **Controls**: a crest (critical), a mouth (the body's level), a confluence (the main stem's
  level; links are solved downstream first), a spill (a link that begins at a body's outlet).
  What a line ends in is what its last station carries, to the millimetre, even where the water
  arrives over that level; only a tributary hanging over its main stem — the stem's level under
  the tributary's bed — cannot carry it, and is counted.
- **Tranquil water never rises downstream.** Where the energy equation alone would have the level
  climb as fast water slows into a pool, the velocity head it would recover is taken as lost in
  the expansion and the level is held. This is an expansion coefficient of at most 1 where
  HEC-RAS suggests 0.3 to 0.8; it is a choice for the picture, and in a channel that does not
  change section it changes nothing (the fixture's backwater is the integral to 0.4 mm).
- **Over a bank**: the level is solved with a wall at each crest, and where it stands over the
  lower crest the station is flagged, counted and reported, and written **at the crest** with
  the solved flow's speed. Once tranquil water has spilled it is written no higher downstream
  than where it spilled, until its solved level comes down under that (those stations are flagged
  too), so the written level does not climb from one crest to the next. **A pit on a line is a
  pool** (W130.2, below): where a line runs down into a hollow of the ground and up to its sill,
  the water stands at the sill and is written as the pool it is. **The cut does not grow**: the solve runs on the composed ground after
  the cut is in it, and the bankfull section is the plan's contract with the exclusion, the
  sediment and the flood bed. The count is the author's lever: a lower `stage`, or a wider law.

**A pit on a line is a pool** (W130.2, 2026-11-16; the owner's walk of 2026-11-15, "dry patches and
wet patches"; [steep-streams](../experiments/steep-streams.md)). The planner routes over its
lattice with the pits filled or breached, and the ground the solve reads is finer than that
lattice: a scour pit at a step's foot, a breached hollow under a metre deep, a dip between the
planner's points. A line runs down into such a pit and up to its sill, and the tranquil profile
taken up over the sill stands level through the pit at the sill's water — which, where the pit is
wider than the section is read (one and a half widths and four lattices each side), stands over the
section's crests. Until W130.2 those stations were written at the crest and then, as the bed climbed
to the sill, as a 5 mm film going uphill that the draw leaves out: a gap in the water at every pit.
Now **the sill of a node is the highest bed at or below it** (a running maximum from the line's
end), and a tranquil station whose bed stands more than `k_pool_pit_m` (5 mm) under its sill and
whose solved level stands at or over it is **written at its solved level, wet to where that level
meets the ground**, walked out across the section past its crests and its sampled row, no further
than `k_pool_reach_m` (25 m) each side. It carries the solved flow (the discharge through the
section with a wall at each crest, as a station over its bank does), is not flagged, and is
counted (`pools.stations` on the cook's line). Below the sill the profile is what it was: the water
passes the sill as the profile solved it and falls away. **Where the line is between a pool and a
lake**: a pit deeper than the plan's `breach_max_m` (1 m) and of `basin_min_area_m2` (200 m2) or more
is the plan's — a body, a body below its spill, or dry ([hollows](#hollows)) — and no line
crosses it; what the solve finds is shallower or smaller than that, or finer than the plan's
lattice. A pit whose level meets no ground within 25 m either side (`pools.open`) is left over
its bank as before: water that would stand wider than that is a lake the plan did not make, or
leaves the line sideways, and neither is a pool of this line's. Nothing keys on a generator: any
ground with a pit on a solved line gets its pool, and a line with no pit pays one running maximum.

**A pool stands no higher than the water above it spilled** (W138, 2026-11-16; the forest's need
126, "water walls", and the owner's raised slab at marks 12 and 13, need 138). Tranquil water that
has spilled over a bank is written no higher downstream than where it spilled, until its solved
level comes down under that (the cap, above); W130.2's pool overrode the cap, and where a pit's
upstream stations were over their banks — an open pit (`pools.open`), or a section read on the
hillside beside the line (row W131's) — the pool stood at the sill's level straight after them:
42 drawn sheets on 14 lines of v47 climbed 0.5 to 3.1 m over a quarter metre, and on the flat by
`region basin 15` `region link 696` held a pool at 62.43 m, 28 m wide, after films at 62.00. Now a
pool whose level stands over the cap **reaches back** over the stations above it written lower
than it, where the ground holds its level at each of them too (`pool_at`, past their crests): they
are the pool's (`pools.reached_back`). Where any of them cannot hold it, the water left the bed
there at the lower level and cannot stand higher here: **the pool is not written**
(`pools.spilled`) and the cap carries on, as before W130.2. Water is never written standing over
ground beside it that holds nothing, and the draw refuses a sheet climbing from a station over its
bank whatever a description says ([water](water.md#the-surface-from-the-solve)). The case: `profile: a pool stands
no higher than the water above it spilled` (a bowl open on one side over its upstream half: before
W138 its closed half was a pool 0.39 to 0.51 m deep over the spilled film; now no station there is
deeper than 0.2 m).

**No section is without a bed** (W129.1, 2026-11-16). A bank's crest is walked out from the
thalweg; on ground exactly as level as the thalweg (a floor the plan filled flat, or ground no
cut crosses that a tile set holds at one height) the walk never rose, the crest stood at the
thalweg on both sides, and the section wet nothing at any level, so the line was refused with "a
station has no water in its section" — the sentence that stopped the forest's whole solve on
`region link 294` (dry at the year's mean by a tenth of a millimetre of sheet) at every stage over
1, and the only way a section the profile reads can hold no water at a level over its thalweg
(the case: `profile: water on ground with no bank across it is a sheet, never refused`). On the
forest's v47 at stage 1.5 the first leg's build left 294 dry with that sentence and this one
solves it whole (598 stations, 4.5 to 6.4 l/s, 12 of them a film over a bank). The walk now goes on
over ground level with the thalweg, so such a section is the flat, and its water a sheet over it
(written over its bank where the level passes the flat's edge, as any). A line whose profile is
still refused is left dry and named on the cook's stderr and in `dry` ("unsolved: ..."), and every
other line is solved.

**Against the closed forms** (`profile_tests.cpp`, the fixture's ground through
`flow_fixture_ground`, n = 0.04): the uniform reach stands at normal depth to the last bit; the
M1 backwater is within 0.43 mm of the direct step integral at every station, which is the
millimetre the level is written to; the crest's depth is 0.377 m against 0.37674 m; the M2
drawdown is within 2 mm of the fixture's stations (the standard step's quarter-metre steps
against a curve that is vertical at the crest); the fall's foot is Rand's to the millimetre; the
jump's head stands at x = 41.241 m against 41.246 m and its foot's depth is the head's sequent
depth by the closed form. The step's description is pinned by hash, the same bytes from MSVC,
Clang and GCC.

**What rises.** The total head never rises downstream. The level may, and each rise is counted by
kind: across a flagged jump; along a shooting run (water that shoots and is slowed thickens, so
on a flat bed its level climbs: the fixture's 4.3 cm below its fall); beside a station written at
its bank; and any other, of which there are none in any case here.

**The head never rises, and what holds it** (the second pass of W120, after the forest's first
solve counted 27 rises of the head, the worst 12.7 cm). The count (`head_rises`) is taken from the
description's own numbers — a station's level plus its speed's head — between every station and
the next, **a fall's lip and its foot and a line's last station included**; only a pair with a
station over its bank is left out, since the level written there is the bank's. Five things each
made the head rise, and each has a case that fails without its mend (`profile_tests.cpp`):

- **A jump's head stands between two nodes**, and where the bed falls from one to the other the
  level between them is no level of either node's section: read in the lower node's it is deep,
  slow water with less head than the jump's foot. The head's station carries the two nodes'
  flows blended (`Out::from`): its area, surface width, speed and Froude number are between the
  two shooting flows', so a head is shooting water by construction.
- **A jump loses head, or it stands elsewhere.** A jump whose foot as it is written would carry
  more head than its head has — the sections change between the two — stands instead at the last
  shooting node that still has the head the tranquil profile holds below it, as at a choke.
  And **a jump's foot is tranquil water**: a stretch that ends at critical depth (a fall's lip, a
  hanging end) has no tranquil water at that node, so the tranquil level a jump is tested against
  is read no further than the node before it, the foot stands no further, and shooting water that
  reaches such an end shoots over it. (A jump was being written with a critical lip as its foot.)
- **A fall whose tailwater stands over its lip's critical level is no fall.** The stretches
  between falls are taken from the line's end upstream; where the tranquil level at a fall's foot
  is the lip's critical level or more, the pool below reaches up over the step, the two
  stretches are one, and the profile is carried up through the step as through any other node.
  (On the forest a tributary's last fall stood 1.8 m under the stem it joined, and its foot was
  written 1.8 m over its lip.)
- **A fall's foot has no more head than its lip.** Rand's area is for the lip's discharge; where
  the discharge grows across the fall the area grows with it, so the ratio of the two speeds is
  his; and the foot's level is never under the shooting level whose head is the lip's.
- **A section's area is not continuous in the level**: a level that passes a hump wets the
  hollow behind it at once. A root looked for by bisection (the level of an area, of a head, of a
  specific force) may then be a step and not a root, and the answer taken is the step's **upper
  side**, where the area is the larger and the speed the lower: the side that has lost head. The
  lower side gave a fall's foot on the forest 1.04 m more head than its lip.

**What a station's speed is.** `speed_mps`, `froude` and `slope` are the flow's at the solved
level, and the level and the depth are that level written to the millimetre: the discharge
through the written depth is Q to half a millimetre of the surface's width (a tenth of a thin
sheet's area, which is why the speed is not read back from the rounded level). Three stations
carry a flow that is not the written level's at all, and say so by what they are: one over its
bank (the solved flow with a wall at each crest); a jump's head (the blend above); and **a last
station brought down to the level its line meets** — the water arrives critical or shooting over
the stem's or the body's level and drops to it, and the station carries the arriving flow, not
the discharge through the film the met level leaves in the line's own section (which was 27 m/s
and a Froude number of 109 on the forest, and 40 m/s and 90 in the case that now holds it).

**How large a Froude number is.** A fall's foot is Rand's sheet, thin and fast, and that is
where every large number stands: on the forest all 290 stations at 5 or more are a fall's foot,
the largest 14.5 below the 9.5 m fall, and away from a foot the largest is 4.3 and the fastest
water 2.4 m/s (4.7 m/s at a foot). The friction slope at a foot is that sheet's under a law fit
to streams 15 cm deep and more — tens to hundreds — and says only that the sheet is stopped
within a lattice, which is what the solve then does (a jump at the foot); away from a foot it is
under 2.

**What it is not.** One dimension: a braided bed is one thread, a confluence is a level and not a
mixing, a bend has no superelevation, and the flow round a boulder is S103's. A solve on a band
of the bed's own lattice is the forest's needs' third size and a later row.

**Lines that climb, sized on the forest** (2026-11-13; the ground sampled at the planner's own
4 m lattice and filled by its rule, each wet line's bed laid against it). The planner
(`detail::plan_from_lattice`) fills every pit by priority flood into `filled_um`, routes D8 over
the filled heights, and nothing after the routing reads the fill: `build_line` gives a point the
unfilled ground, and `cut_bed` holds the bed at its running minimum only while the ground stands
within two thalwegs of it. So a line crosses a basin on its floor and climbs its far wall. The
forest's ground holds **eight closed basins 28 to 134 m deep** (2.9 to 43 ha, 47 million m3) with
no water body in any, and a lowland one whose lake stands 5.5 m under its spill; 110 wet lines
cross the eight (46 over one), 16.1 km of the 36.9 km of wet line lies in a hollow of its own
line, and 91 % of the over-bank stations are in one. By the hollow's depth: under 0.25 m, 778
hollows and 3.07 km; to 1 m, 278 and 4.59 km; to 5 m, 225 and 3.89 km; to 20 m, 22 and 1.77 km;
over, 13 and 2.77 km. A breach of the lip mends the 1,030 hollows (6.74 km) with no basin under
them or one under a metre deep; only a body filled to its spill mends the 207 (6.71 km, 57 % of
the over-bank stations) over a basin 5 m deep or more. The row that mends it — a pit past a
breach limit becomes a standing body of the plan at its spill, links split at its shore as at a
scene's body, a shallower pit breached by the cut — is not built.

### The cook

`engine-content flow solve <scene.json> <out.json> [--ddc <dir>] [--entry <i>] [--lattice <m>]
[--resistance jarrett|manning] [--manning-n <n>] [--reach <m>] [--jobs <n>] [--scene-out <scene>]`
opens **the composed ground the scene draws** (`renderer::SceneGround`, handed to this module as a
`scene_gen::Ground`: a function pointer and a context) and solves on it every wet link of every
`channels` entry, and every `water_channels` block that is not one of those links' — an author's
line. So the order of a cook is: `engine-content channels --cut` for the plan and the cuts; the
cuts pasted into the scene's `tiles.edits` and the plan's key into the entry; then `flow solve`,
whose `water_channels` are pasted (or written whole with `--scene-out`). The scene's own
`water_channels` are not read for the ground (`SceneFileOptions::without_water_channels`).

**What a block declares and holds says what the cook does with it, never what it is called**
(`WaterChannel.derived_from`, version 7; the first cut told a planner's block by its name's
prefix, which is a convention of one generator and no property of a block):

| The block | The cook |
|---|---|
| `derived_from` names an entry this cook solved (`channels <layer name>`) | that entry's last cook: replaced by the links solved now |
| `derived_from` names anything else | another writer's: kept as it is |
| no `derived_from`, a `discharge_m3s` | an author's line: solved (again, from its stations, if it was solved before) |
| no `derived_from`, no discharge, a depth and a slope a point | laid before W120, and nothing to solve it from: dropped and counted (`blocks_laid_dropped`) |
| no `derived_from` and none of those | an author's line that names no discharge: refused |

Any writer of channels may declare itself in `derived_from`; no reader draws by it. So a scene
cooked before W120 is cooked again by the same command, and a scene cooked twice holds each
link once. **A block of versions 3 to 5 reads** — in the cook and in every host — through
`WaterChannel`'s registered migration (`scene_gen::migrate_water_channel_5`, which drops `fit`,
as `schemas/README.md` asks of a removed field): a scene file stores no version, so the stored
version is told from the block (one that names `fit`), and the block is then refused by the
check for what it is, "cooked before W120: cook again", and not for a field's name. The lattice is the tile
set's heightfield's spacing unless `--lattice` names one. It writes the blocks and a `dry` list
(each dry link, its reason, its discharge and the film's depth) and prints one line: links wet and
dry, the scene's own blocks replaced, kept and dropped as laid, kilometres, stations a
kilometre, crests, falls, jumps, drowned feet, hanging tails, the largest Froude number, the
stations over a bank (count, worst, where), the rises by kind with the worst of each and where,
and the solve's seconds a kilometre. `--jobs` is 1 by default: until the tile set's point read
gives up its lock (T128), readers on a pool are slower than one thread.

**An author's line** is a `water_channels` block whose name is not a planner's link: its `points`
and a `discharge_m3s`. It is solved with Jarrett's law unless `--resistance manning` holds its own
`manning_n`; a line that runs into a body — a lattice past its last point stands inside a body's
circle or polygon — ends at the body's level as a mouth, and any other ends at normal depth. The
block keeps its optics, its seed and its banks, and takes the solve's stations. The end-to-end
case (`flow_tests.cpp`) writes the fixture, solves its three channels again this way through the
tiles provider, and holds them to the fixture's stations: the same flags in the same order, the
levels within 2 mm.

**Measured** (2026-11-13, `msvc-release`, one thread, beside other agents' builds, so the seconds
are upper bounds): `terrain-synth` heightfields at 0.5 m with a `channels` entry, cut, then solved.

| Scene | Stage | Links wet of all | Solved km | Stations a km | Crests | Falls (highest) | Jumps | Feet drowned | Over bank: stations, lines, worst | Rises: jump, shooting, at a bank, other | Head rises | s a km |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mountain, 2 km square | 1 | 13 of 16 | 9.44 | 1,491 | 34 | 0 | 0 | 0 | 18, 4, 0.21 m | 0, 0, 0, 0 | 0 | 0.15 |
| the same | 20 | 16 of 16 | 12.06 | 1,818 | 0 | 0 | 0 | 0 | 5,088, 11, 2.22 m | 0, 0, 677, 0 | 0 | 0.16 |
| steps, 1 km square | 1 | 6 of 23 | 1.60 | 1,455 | 6 | 6 (1.73 m) | 0 | 4 | 464, 1, 0.08 m | 0, 0, 0, 0 | 0 | 0.14 |
| the same | 20 | 23 of 23 | 7.74 | 1,909 | 30 | 36 (1.96 m) | 4 | 28 | 13,723, 21, 0.49 m | 4 (0.12 m), 0, 256, 0 | 0 | 0.13 |

(Taken again at the row's last solver commit, 2026-11-14 00:35, the same way; the first table was
from before the bank's crest was read past a ledge, which is what took the mountain's over-bank
stations in flood from 12,183 to 5,088. The largest Froude numbers are 1.0, 3.1, 8.7 and 8.6,
the last two at a fall's foot.)

At the year's mean the three dry links of the mountain end with 2.8, 3.5 and 4.0 l/s over beds
wider than a metre, films of 8 to 10 mm; the stepped scene's sixteen are smaller still. A stage
of 20 is a flood put through beds cut for far less, to see what the cook says: more than half the
stations stand over a bank on the stepped scene, and the level written there steps up where the
bed itself stands over the last spill (the 677 and 256 rises "at a bank"). **No tranquil rise
that is not at a bank remains in any scene, none at all at the mean, and the head rises
nowhere** (the stepped scene in flood had one rise of 3.7 cm before "The head never rises, and
what holds it"). The four rises across a jump there, to 12 cm, are steps a walker sees, drawn as
pairs.

**Stations a kilometre are about 1,500 on these grounds**, against 150 to 240 on the fixture and
against the 1,780 points a kilometre the laid description wrote here. The level's pin is not
what holds them: the wet reach's is. A bed two or three lattice cells wide is jagged at the
lattice, so the water's edge moves by more than 5 cm from one node to the next and almost every
node is a station. The lever, if the draw wants fewer, is `k_station_wet_m`.

### How whole a stream is drawn

(W130, 2026-11-15; the owner's walk: streams "incredibly disjointed on any slopes, with dry
patches and wet patches"; the grounding is [steep-streams](../experiments/steep-streams.md).) The
cook's line carries `runs_by_slope` (`runs.h`, `measure_runs`): every block it writes, classed by
its mean slope (its level's fall over its length: under 2 %, 2 to 5, 5 to 15, 15 to 30 and over
30 %, the forest session's own classes), and in each class the lines, the kilometres, and:

| Field | What it counts |
|---|---|
| `film_share` | the share of the length whose two stations are both 5 mm deep or less: the cook's least written depth |
| `undrawn_share` | the share the draw leaves out by its own rule (`scene_gen::channel_stretch_drawn`, the one copy the draw reads too): both stations over a bank and the water a film under 2 cm or climbing |
| `pieces_per_100m` | maximal runs of drawn surface, cut at every lip-to-foot pair: the forest's "runs of water" |
| `runs_per_100m` | the same joined across every fall the draw lays a nappe on (the foot lower and the stretch drawn): the water a walker sees as one |
| `falls`, `falls_drawn` | lip-to-foot pairs, and those with a nappe |
| `gaps` | maximal runs of undrawn stretches |
| `thin_share`, `alternations_per_100m` | the forest's own count: stretches leaving a station 1.1 cm deep or less, and the runs of one kind (thin or not), both kinds counted |

**How it differs from the forest's count.** Its session's script (`streams47b.py`, read on
2026-11-16 once the proof of concept was opened) classes every station by its depth alone: 1.1 cm
or less is "film", and its pieces are the runs of film and of water along the line, both counted.
That takes in more than the 5 mm film: **a fall's foot is Rand's sheet**, under a centimetre at
20 l/s below a metre's drop, and the shooting water below it stays that thin until its jump, so
every free fall on a steep line adds two pieces to theirs whether or not anything is missing from
the picture. The engine prints it beside its own (`thin_share`, `alternations_per_100m`, by
horizontal length where theirs is by the points' 3D distance, 7 % longer on a 37 % line); `runs`,
the water a walker sees as one, is the count a picture should be held to.

**Measured on `content/test-scenes/torrent`** (2026-11-16, `msvc-debug` `engine-content flow
solve`, one thread, the plan of 2026-11-15, CPU only, beside other agents' builds), the lines
steeper than 30 % (7 of 8 wet lines, 0.23 km; 8 lines at stage 3):

| Stage | Build | Film | Undrawn | Pieces / 100 m | Runs / 100 m | Falls (drawn) | Gaps | Over a bank | Pools |
|---|---|---|---|---|---|---|---|---|---|
| 1 | before W130.2 | 1.93 % | 2.36 % | 4.77 | 3.47 | 4 (4) | 1 | 15 | — |
| 1 | W130.2 | 0 | 0 | 4.77 | 3.04 | 4 (4) | 0 | 5 | 3 |
| 2 | before | 1.93 % | 2.36 % | 4.77 | 3.47 | 4 (4) | 1 | 20 | — |
| 2 | W130.2 | 0 | 0 | 4.77 | 3.04 | 4 (4) | 0 | 10 | 3 |
| 3 | before | 1.92 % | 2.35 % | 5.20 | 3.90 | 4 (4) | 1 | 22 | — |
| 3 | W130.2 | 0 | 0 | 5.20 | 3.47 | 4 (4) | 0 | 12 | 3 |

No station of the torrent is 1.1 cm deep or less at any stage (its feet land in plunge pools), so
the forest's count there is the runs.

**Measured on the world the owner walked** (the forest's `world\v47\base.json`, plan
780a7a78621fb16f, its tile sets and route plan copied to a root of the row's own; the same build
and machine as above, 131 to 154 s a solve, the largest 2.44 GB of commit). Over 30 %, by this
count and by the forest's own script run on the same output (its `streams47b.py`, whose numbers on
the stage-1 solve before W130.2 are the session's own, 7.4 % and 6.04, to the digit):

| Stage | Build | Lines, km | Film | Undrawn | Runs / 100 m | Falls (drawn) | Gaps | Theirs: film, pieces / 100 m |
|---|---|---|---|---|---|---|---|---|
| 1 | before W130.2 | 93, 6.24 | 4.66 % | 5.77 % | 3.77 | 544 (522) | 173 | 7.4 %, 6.04 |
| 1 | W130.2 | 93, 6.24 | 2.19 % | 2.81 % | 3.32 | 542 (527) | 136 | 4.9 %, 5.16 |
| 1.5 | W130.2 | 206, 14.30 | 1.27 % | 1.54 % | 2.51 | 1,509 (1,477) | 208 | 3.3 %, 3.97 |
| 2 | W130.2 | 294, 26.38 | 0.70 % | 0.89 % | 1.77 | 2,839 (2,799) | 242 | 1.9 %, 2.59 |
| 3 | W130.2 | 394, 63.39 | 0.40 % | 0.43 % | 0.96 | 11,242 (11,150) | 343 | 1.2 %, 1.34 |

Their pieces are this count's `alternations_per_100m` over the points' 3D length, which on these
lines is 18 % over the horizontal (6.11 and 5.16 at stage 1). The whole world at stage 1: stations
over a bank 3,174 before and 1,623 after, 407 stations written as pools and 695 in pits whose level
meets no ground within 25 m; no head rises at any stage. **More water joins the lines, but mostly by
adding lines**: the wet network grows from 26.6 km to 40.2, 55.9 and 98.6 km, and the new lines are
steep and short. **The owner's line, `region link 515`** (417 m, 153.6 m of fall, 37 %), by the
forest's `line47.py`: before W130.2, 34 runs of water over 299 m and 38.7 m of film over a bank in
11 runs; after, 37 runs over 321 m and 18.4 m of film in 9; at stages 2 and 3 (49 to 51 and 77 to
81 l/s) 40 and 45 runs and the same 18 m of film. Its gaps are its 30 lip-to-foot pairs (each drawn
as a nappe) and nine short stretches of film over a bank that the pool did not take (a pit whose
level meets no ground within 25 m, or a level over a bank where no pit is) — no stage mends those,
and the forest should not expect one to: `flow.stage` 2 doubles 515's depth
(6.5 to 9.4 cm) and leaves its pieces as they are. The stage changes little: the main stem is its spring's
21 l/s, and the runoff the stage scales is a few litres a second on catchments of a few hectares.

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
  area), **the bed held at its running minimum down the line**: a bed never rises downstream, in a
  link or across a junction, so where the ground at the line rises the cut runs through the rise
  that much deeper. (Until W122 the hold reached two thalwegs under the ground and no further,
  because on v35 the running minimum alone would have cut 103 m through a filled depression: where
  a line climbed out of one the bed climbed with it, which is the water going uphill of the
  owner's walk. A depression that deep is no longer crossed by a line at all: [Hollows](#hollows).)
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
  the range the section's swell can give it (the swell is planned after the bed), across the
  line's heading over a width and its heading between its neighbours, the lowest kept,
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

**Seen** (2026-11-12, `msvc-debug`, 1920×1080, `--raster hw --shadows rt`, world tiles, the
generated material sets, the base and this row built in one checkout against one derived-data
root each): from six metres back and two up, at 10 degrees the base's water ran out past the
downhill bank as a level sheet ending in a straight edge over the slope, and at 30 degrees the
bed and the water stood as a shelf along the hillside with the cut only on the uphill side; now
the water lies in a trench between two banks at every cross-slope, the downhill bank's lip
standing over it. What a walker would still object to is not this rule's: the waterline along
the downhill bank is the lattice's sawtooth ([The flood bed](#the-flood-bed)), the uphill cut's
lip reads as a row of teeth from below where a coarser ring draws it, and the scene's ground drew
in its default sand.

**What is left.** The meander still swings the contour stream a little down the slope at 20 and
30 degrees, so its profile reads step-pool for 31 to 85 of 433 points (printed by the case, not
held); a meander on a hillside wants the trough test applied across the whole width, not the
centre.

**W93's count, restated** (2026-11-12): the case "a line across a slope is moved between its
banks" held the points whose bank stands half a depth under the *line's* ground under 2 %, a
level reached by the untested width's move that kinked the line on a hillside; it now holds, at
every such point (58 of 1,969 on the valley, none excepted), that the section's top stands over
that bank by no more than the bound, and that the move leaves fewer such points than no move
(58 against 370 with `ChannelLayer::bank_move` off, `ChannelLayer` version 11).

**The seat, reshaped** (2026-11-14, roadmap W122; S104's finding that the seat eats a step, and
the stream solver's that 147 hollows a metre to 7 m deep and under 2 m long stood at the feet of
steps). The seat is kept: a section whose downhill bank stands under its bed holds no water, and
the solver, which finds the level in the real section, can only flag such a station as over its
bank. Three things about it changed, and one about the bed it lowers:

- **No bank is read across a step.** At a knickpoint the ground falls along the line, and a line
  that meets the step at an angle has one bank on the step's top and the other at its foot; the
  seat read there was the step's height. A knickpoint's points, and those within the read's own
  reach and the dilation's two points of one, take no seat: the water falls there, and what bounds
  it is the fall, not a bank.
- **A bank is read only where it is one**: ground that stands further under the line than it
  lies from it across (past a 45-degree fall) is a face's foot, or the ground below a cliff's
  edge, and is left out of the read. A stream along a cliff's edge is seated by the bench it
  runs on, not by the cliff (the forest's link 613 was seated 126 m).
- **A seat never reaches under the lip below it.** The bed does not rise, so what a seat lowers
  it by is carried down the line until the ground has fallen that far. Above each knickpoint's
  first point a seat is at most the ground's own fall from its point to that one, so a seated
  stretch a few metres above a cliff does not notch the cliff's lip.
- **The bed does not climb back.** Where a seat ended, the bed used to return to a thalweg under
  the ground within a point or two: a pit at the seat's foot. It now holds its level until the
  ground comes down to it ([The cut](#the-cut)).

On the cliffs' scene of S104's count (a valley at 3 % with cliffs of 3, 7, 10 and 14 m, eight
plans): **the seat within three metres of every cliff is 0** (it was 12.9 m at the 14 m cliff of
seed 2), **every cliff is one run, a plunge, with no pit at its foot and nothing tiered** (they
were two runs with a pit 12.7 m deep), held by `falls: no seat is read across a cliff, so each is
one fall with no pit at its foot`. The older case, which asks each drop within 0.25 m of its
cliff, keeps its `may_fail` for one run of the eight, and that is the count's: a point of the
line stands half-way down the cliff's 25 cm ramp, the stretch into it falls at 44.5 degrees, and
the fall is begun below it, half a metre short.

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
raising the one below and never more than half `fall_min_m` under the bed's own line, and the
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
the body's level at the shore (W121; until then to half a thalweg under it), and its width grows by
`fan_spread` (1, so to twice) at the shore. `mouth_level` is **the body's own level**
(`Exclusion::water_level_mm` at the line's first point under water; until W121 the ground at the
shoreline, which is the level only where the body's outline is the level's contour, and under it
where an author's outline stands in water). **Nothing of the cut stands under that level outside
the body** ([the base level](#the-base-level-and-a-fan-below-a-cliff-w121)), and **a fan is what
lies below a cliff**: where the ground under the line falls at 45 degrees or more between two of
the fan's points, the fan begins below it. The water's surface grades over the fan to the body's level at the shore, and on a link that
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

### The base level, and a fan below a cliff (W121)

(2026-11-13, roadmap W121; the owner's walk of that day, mark 8: where the forest's link 778 meets
the polygon pond `freshwater-swamp`, "a black triangular hole in the ground and a teal triangle";
and W119, the fan over a sea cliff that S104's cook found.) **A stream cuts no deeper than the
body it ends in.** The level of the sea, a lake or a pond a stream flows into is its *base level*,
the lowest level to which it can erode its bed (Powell 1875 named it; Mackin 1948 made it the
control the graded profile adjusts to); a channel's bed under that level near its mouth is under
the body's water, a backwater. The engine draws a body's water over its outline and nowhere else
(water.md, "The mesh": a ring of the outline, or a fill's flood), so W90's fan, which graded the
shore to half a thalweg under the ground at the shoreline and spread to twice the width there,
left whatever of its cut lay outside the outline under the waterline **and dry**: on an oblique
shore the fan's spread and its round cap past the last point dig the bank beside the shoreline,
and a dry link (one under the description's area, as link 778 is) has no ribbon over its bed
either. So on a link that ends in a body:

- `mouth_level` is the body's level (above), not the ground at the shoreline;
- the fan grades the bed to that level at the shore, and **every point's bed is held at the level
  wherever the cut's top stands over it**; where the ground at the line itself stands under it (an
  outline that falls short of the contour leaves such ground dry), the section's top is the level,
  so the cut lowers only ground over it and does not deepen what the outline left dry (that is the
  outline's to answer);
- the section's depth (`terrain_cuts_json`: a bend's pool, a riffle's swell) and the flood bed's
  (`flood_depth_m`) take no point under the level either.

**An outlet's start the same** (`ChannelLink::outlet_level`, the level of the body it leaves,
plan format 4): wherever the ground at the line stands at or over that level its bed is held at
it, and its section's depth and flood bed take nothing under it; where the ground falls under the
level, as it does below a sill, the cut is the outlet's own. A lake spills over its outlet's sill
at its level, so a bed under the level where the ground stands over it was a dry hollow under the
waterline beside the body. What is left under the level outside the body is the round top's crease, `bank_round` times the
depth over four (a few centimetres), where the cut's surface meets ground standing at the level.
**A fan is what lies below a cliff** (W119): the fan's span is the last `fan_widths` widths less
everything above the last stretch in them whose ground falls at 45 degrees or more
(`k_fall_gradient`, where the water leaves its bed), so over a sea cliff the bed follows the cut's
own line to the lip and the water falls, where W90 graded the cliff's top into a ramp. The fan's
spread and the seat's exemption still read the fan's whole length (`fan_share`).

**What an author's channel gets.** The rule is the planner's, applied where it writes a mouth: a
`TerrainEdit` written by hand is the author's section, and the stamp (tileset.md, "Channels cut
into the ground") knows nothing of bodies. A hand-made channel into a hand-made body meets the
same hole if its bed runs under the body's level outside the outline. A scene with no body costs
nothing of this: a link that ends at the region's edge or a junction is cut as it was.

### Where a cut meets a bank: the owner's marks 6 and 7

(2026-11-13, W121.) **What made the holes, by stage.** Each mark was reproduced on an engine scene
of its shape (`scratch\b64\W121\scenes\`: S104's V valley with cliffs of 3 to 14 m; the notch into
a pond turned 30 degrees whose outline falls 4 m short of the contour; the notch over a 20 m sea
cliff) and read two ways: on the tile set's edited ground at 25 cm (`cut_meets_tests.cpp`, the cuts
compiled by `tileset::TerrainEdits` and applied as the set's build applies them) and in a capture's
ids and depth (`render.capture`'s channels). **No capture had a pixel with nothing drawn under
drawn ground**: no stage leaves triangles out, every level and tile seam is whole. Each "hole" is
the ground the cut left:

- **Marks 6 and 7, a pocket in a hillside and a dark hole in a bank: the seat** (W116,
  `seat_section`). Where a point's lower-bank reading lands past a drop — a cliff the line meets at
  an angle, a cliff edge beside a contour stream, a trough across a bend — the top is lowered by the
  whole drop and the lowering dilated over two points either way; the cut's face then rises a
  thalweg at its slope and runs out to at most 20 rises over that top (tileset.md, "A face that runs
  out"), so where the hillside stands higher the cut ends in a wall: a closed pit metres deep with
  walls, which traced light draws black. Measured as the deepest closed hollow the cut made that
  the ground did not have (a priority flood over the window, Barnes, Lehman & Mulla 2014): **12.7 m
  at S104's 14 m cliff (seed 2), 0.85 m (seed 1), 7.7 and 7.8 m along a cliff's edge** (the seat
  8.2 m), on 61ccce67 and with W121. The stamp is not the fault: with a seat no deeper than the
  bank slope over the half width, the run-out meets any hillside under about 70 degrees. The seat
  is W120's row (it absorbs W117's cap, `65eed9cd`); the case is marked `may_fail` until it lands.
- **Mark 8 on the forest is an outlet, and its line climbs.** W120's solve of the forest (its
  `flow.json`) has link 778 *leaving* the polygon pond `freshwater-swamp`: its first station is the
  pond's spill, 2 m inside the outline, and its level climbs from 1.12 m to 2.09 m down the line,
  422 of its 435 stations over a bank: a line the planner routed out of a hollow over the filled
  lattice. The climb is the plan's to mend (a row of its own). What the cut did where it met the
  pond is this row's: the outlet's bed, held down the line from the shore, ran under the pond's
  level up the climb and out past the outline (an engine scene of the shape, a pond 0.5 m under its
  rim: 2.25 m² of ground that stood over the level cut under it, the worst 0.22 m; none with W121).
  Link 360 (mark 6) climbs too: 18 stations, a fall into a 0.41 m pool and the level rising 1.5 m
  to its end, 16 of them over a bank. Link 482 (mark 7) falls 75 m and has 89 stations of 550 over
  a bank; at the mark a pool 0.47 m deep behind a lip. **All three marks are lines in hollows**
  (W120's `hollows.txt`: the planner routes over the pit-filled lattice, which the cut, only
  lowering, does not reproduce; link 360 lies wholly in a hollow 1.89 m deep, link 482 in one
  0.42 m deep at the mark, link 778 in one 1.77 m deep at its pond), so the pockets are first the
  plan's (W122), and the seat's where a bank is read past a drop. "a line through a hollow leaves
  no pocket on its own line" (`may_fail`; the notch's 3 m hollow with no body: 79 of 184 points of
  the line in a pocket deeper than 0.3 m, the deepest 1.55 m) is the case that says when the plan
  is mended.
- **The black triangle beside a teal one at a mouth: the fan under the body's level** (above). The
  teal triangle is the body's own surface (`systems/water`'s ring of the outline, at the level
  over the whole outline), seen where the fan's cut took the ground under the level inside the
  outline: water rightly drawn. The black one beside it is the same cut outside the outline, dry
  under the waterline: 11.25 m² under the level, the worst 0.31 m, on the scene whose outline
  falls short (none with W121); 0.56 m², the worst 7 cm, where the outline reaches past the contour.
- **On the forest the black triangle stays** (2026-11-14, W121's binary, the staging re-cut with
  W121's plan into a derived-data root of its own, the owner's mark 8 camera, `--raster hw
  --shadows rt`, GI sky): the outlet's bed is held at the pond's 1.179 m outside the outline (the
  ground sampled there is 1.179 m), yet a band about 4 m across below the outline's straight edge
  draws near black (rgb 9 to 17), and the ids say it is the ground's own instance, not an empty
  pixel and not the water. So what darkens it is not the cut: ground at the body's level (or under
  it, where the author's outline falls short) beside a body that draws no surface over it. It is
  handed to the water's row with the capture (W121's hand-back); the engine scenes above show the
  cut's part of it and not this shading.
- **W119**: over the sea cliff, 6 of 24 points in the last 12 m above it had their bed graded up to
  16.9 m under the cut's own floor; none with W121.

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

## Hollows

(2026-11-14, roadmap W122; the owner's walk of the forest on 2026-11-13: "we have water going up
hill, we have stream beds with no water"; the grounding is
[the hollows page](../experiments/hollows.md).) **A line never leaves a hollow uphill.**

**The fault.** The planner filled every pit flat, routed and gathered area across the filled
flat, and then nothing after the routing knew the fill: a line took the unfilled ground at each
point, the cut's bed held its running minimum only within two thalwegs of that ground, and so a
line ran down into a hollow and climbed out of it. The head rule read the unfilled slope, so a
lake's sloping floor began channels on the lake's surface (46 wet links over one basin of the
forest). On the forest the stream solver sized it: eight closed mountain basins 28 to 134 m deep
with no water body in any, 110 wet lines climbing out of them, 44 % of the wet line in a hollow of
its own, and 91 % of the 28,999 stations it flags as over a bank.

**What a hollow is.** After the fill, the cells it raised, connected through their eight
neighbours, are one pit. The flood reached it first at one cell, from a neighbour that was not
raised: that neighbour is the **spill**, the lowest point of the rim on the lattice, and its
ground is the pit's level. The floor is the lowest cell; the area and the volume are what lies
under the level; the catchment is what drains through the spill. All of it is exact on the
lattice and integer, so it is the same on every toolchain.

**What the plan makes of one** (`HollowKind`; `route_hollows`, network.cpp):

| A pit that is | becomes | and its lines |
|---|---|---|
| no deeper than `breach_max_m` (1 m) | **breached** | cross it; their bed is carried through its lip at the level it came in at |
| deeper, at least `basin_min_area_m2` (200 m2), and fed more than it loses at its spill | **a lake of the plan**, at the spill less `basin_notch_m` (0.25 m) | end at its shore (a mouth at its level); one outlet link leaves, from the shore through the lip |
| deeper, fed less than it loses at its spill | **a body of the plan below its spill**, at the level where its losses meet its inflow | end at its shore; nothing leaves |
| deeper, and its losses take all it is fed, or its author says `dry` | **dry** | end at its floor (a swallow hole, a playa, a basin that drains underground) |
| named `infilled` by its author | **ground**: a lake that silted up, filled flat to its spill ([a dried lake's floor](#a-dried-lakes-floor)) | cross its floor and leave by the old outlet |
| deeper, under the least area | a **deep breach**, counted | cross it |
| holding a scene's own body within `breach_max_m` of the spill | the **author's lake** | end at its shore; its outlet runs through the lip, a breach inside the limit |
| holding a scene's own body further under the spill | **where the basin's water ends** | end at its shore; nothing begins there |

A body, a dry floor's lowest cell, a scene's body under its spill and every cell of an **open**
body's sea are **sinks**: the fill is seeded from them as it is from the region's edge (Barnes et
al. 2020 seed their ocean the same way), so everything in the basin drains to them and no route
crosses the rim. A basin made a sink no longer drowns the hollows inside it, so the fill, the
routing and the labelling are taken again until no pit changes (twice on a ground with one
terminal basin; refused past sixteen).

**The balance** is the year's mean, in the scene's own units:

    inflow  = flow.runoff_l_s_km2 x (catchment - the water's area) + flow.sources that drain in
    losses  = (basin_evaporation_mm_y + basin_seepage_mm_y) x the water's area + the basin's leak

`basin_evaporation_mm_y` is **net of the rain on the water**, 0 by default: the default runoff
(946 mm a year) is a climate whose rain passes a lake's evaporation (500 to 900 mm on cold
temperate lakes), so every hollow fills unless its bed seeps or its floor leaks. A hollow whose
inflow does not cover its losses at the spill takes cells from its floor up until they do (Barnes
et al. 2021 grow the same queue from the pit cell until it holds the water), which is coarser than
their Fill-Spill-Merge: one level a hollow, the inflow not divided between its sub-basins. A source
feeds every hollow its water passes on the way down. **A lake's outlet carries what its lake
does not lose** (`network_flow`, discharge.cpp): the lake's loss is taken off every point of its
outlet and of each link that outlet runs on into, never more than reaches the outlet's first
point, and not scaled by `flow.stage` (a bed seeps in a dry month too); the command's line counts
them (`lake_outlets_losing`, `lake_loss_l_s`). A plan read from its file holds no lattice of
bodies, so an outlet's lake is the one whose sill it holds, the nearest spill between two at one
level. **Not in the balance**: a lake's loss is not taken off the inflow of the lake below it
(the planner gives each hollow its whole catchment's runoff), nor carried through that lower
lake to its outlet; the lake's own area still sends its runoff down the outlet as land would
(the rain on it less its evaporation is about half that in the climate the default describes);
the stage of the year is not read by the planner.

**What the author gives** (`ChannelLayer.basins`, each `{name, at: [x, z], dry, leak_m3s,
infilled}`): what the ground cannot say about the pit that holds `at`. `dry`: no body, whatever
its depth and inflow. `infilled`: the hollow is a lake that silted up, and is ground (below);
it is not `dry` and names no leak. `leak_m3s`: what its floor passes underground (talus, moraine, karst); a spring where
that water returns is a `flow.sources` entry of the author's, since the plan draws no pipe
between the two. What feeds a hollow beyond its catchment is a `flow.sources` entry standing in
its catchment. An entry that names neither, a point outside the region and a point in no pit are
each refused by the entry's name. To refuse a lake an author names its basin dry, or raises
`breach_max_m` over its depth (a canyon through the lip), or authors a body of their own there.

**The plan's key.** `flow` is not what a plan is made from (W120), so naming a spring re-plans
nothing. That stays so wherever no loss is named. With a loss or a leak a hollow's kind is the
runoff and the sources against it, and `plan_layer_hash` takes those two in.

**The body the plan writes** (`plan_bodies_json`; `engine-content channels` puts them under
`water` in its output, for the scene's `water`): `{name: "<layer> basin <n>", level_m, shape:
{fill: {seed, max_extent_m}}}`, seeded at the hollow's lowest cell, the extent the hollow's own
reach from it and a margin. It is an ordinary `engine.scene.WaterBody` that fills, so the water
capability floods it over the drawn ground and draws it as any body, with
[the look its size gives it](#a-lakes-look); every other field is the author's to set on the
pasted block. A second run of the command leaves out the bodies an
earlier run wrote (by that name), so it plans over the same bodies and writes the same ones.
**Not built**: an entry that names no plan (whose cuts reach the ground through the hook) has
its lines split at the plan's bodies and nothing writes those bodies into its scene; the host's
reader would have to take them from the plan, as it would the water's description.

**A body of the plan is a body like any other where a link meets it.** Its mouths and its
outlet take W121's rules as a scene's body's do
([the base level](#the-base-level-and-a-fan-below-a-cliff-w121)): a mouth's `mouth_level` is the
plan's level exactly, its fan grades to it and its bed stands nowhere under it; an outlet's
`outlet_level` is the body's sill, **two centimetres over its level** (so no rounding of the
cut's stamp leaves a thread of the lip under the body's level for its flood to follow down the
channel), and the outlet's bed is held there wherever the ground at the line stands over it: a
lake spills over its sill, and the cut is the sill's notch.

**A lip is held by every link that begins on it** (`lip_sill`, network.cpp; found on the forest
on 2026-11-14 by sampling the drawn ground round each spill). A lake stands a notch under its
spill, so the cells between its shore and its lip are dry ground inside the hollow, and the lines
that cross the water (they follow the fill's own routing over the flat) merge on that shelf as
readily as under it. A link that begins at such a junction, or at the spill itself, begins at no
shore and was no outlet: its cut dug the lip to its own bed. At the spills of the forest's three
largest lakes that bed stood 0.12, 0.17 and 0.29 m **under the lake's level**, the lake's own
outlets arriving beside it held at the sill, and only the natural rim between that pit and the
water: the lakes held by 1 to 9 cm. So a link that begins within a cell and a half of a lake's spill holds
that lake's sill from its first point for as long as its points stay that near and its top
stands over the sill, in the plan's bed and in the cut's depth (no pool or swell under it); the
bed's line then steps down past the lip and rises nowhere. The case is two lakes that share one
spill (`hollow_tests.cpp`).

**The level against the ground between the lattice's points** (measured, not yet a rule of the
cook). The plan finds a spill on its 4 m lattice and stands the body a notch (0.25 m) under it;
the water's flood (`water::fill_body`) runs on a lattice of two thirds of a metre and refuses a
body whose level finds a way out. On the forest's mix the drawn ground was sampled on the
water's own lattice in a 96 m window round each of the nineteen bodies' spills (402,816 points)
and the lowest pass from the lake's side to its outlet's channel below the lip was found. Of the
thirteen with an outlet's cut to follow, in twelve the pass stands **2 to 26 cm over the level
(the sill's 2 cm in five of them; 1 cm in one before the lip was held)**; the thirteenth, a pond
at -0.71 m beside the authored lake at -2 m, could not be told from that lake's own low ground
in the window. Of the six without (one leaves by the region's edge, one stands 34 m under its
spill, four are small ponds whose outlet the search did not find), the drawn ground within 2 m
of the spill stands 2 to 25 cm over the level in five and **7 cm under it at one** (0.5 ha at
(132, -28)), not decided. So the notch covers what the lattice steps over wherever it was
looked for, by centimetres. It was looked for round the spill only: a saddle elsewhere on a rim
that the lattice missed is not excluded by it.

**The cook says which body a load would refuse** (`check_fill`, flow_solve.cpp). The whole check
is the water's own flood at the level, so `engine-content flow solve`, which has the drawn
ground with the cuts in it and the scene's bodies, runs it for every body that fills: the
water's rule mirrored (a lattice of `shore_m / 3` held between 0.25 and 1 m about the seed, four
neighbours a node, wet where the ground stands under the level, grown by a quarter until the
region's nodes and edge crossings fit 65,536; water.md, "A body that fills"), not called, since
the command does not link the capability: the two move together. Its line gains `fills`:
`bodies`, `spilling`, `seed_dry`, `seconds` and a `list` of each body's `level_m`, `spacing_m`,
`nodes` and `holds`; a body whose level reaches its `max_extent_m`, or whose seed stands dry, is
named on stderr with the water's own reason. The scene is not changed and the exit code is the
solve's: lowering a level that escapes is the author's, or the plan's when its notch is sized
from this (not built). An authored body that fills on an imported ground is checked the same; a
scene with none pays nothing. The case is three bodies in the fixture's pool
(`flow_tests.cpp`). **On the forest's mix all nineteen bodies hold** (4.3 s for the nineteen
floods, beside a solve of 7.7 s): the pond beside the authored lake and the pond with ground
under its level by its spill among them, at lattices from 0.67 m (eleven ponds) to 3.18 m (the
43 ha lake, 42,959 nodes), so no load refuses the scene for a body of the plan.

**A breach's top.** Where the bed is held more than a thalweg under its section's own depth the
banks keep their slopes by reaching the top further out, so the cut widens with its depth
(`ChannelLink::widen`, `plan_widening`): at one width the stamp steepens both banks to a V, and a
breach two metres deep in a stream a metre wide was a slot. The top is carried out only as far as
the ground beside the line stands at or over it (the largest of the whole, three quarters, a half
and a quarter of what the slopes ask, the narrowest reading over two points either way): over
ground that falls away it would stand in the air, since a cut only lowers, so on a hillside or a
bench the width stays and the stamp steepens the banks as it did.

**The rule, measured** (`measure_hollows`; the command's `hollows` object): the links whose
bed's line rises anywhere (`climbing`, 0), the largest rise, the links that end over where they
began (`ends_higher`, 0), and what the breaches dig (`trench_max_m`, `trench_line_m`). The bed's
line is the cut's bed with a step-pool's own pool taken back out, since a pool below a step is a
hollow the reach is made of, and an outlet's lip is its body's sill, held and not measured.

**On the forest** (2026-11-14, `msvc-release`, the staging scene of the proof of concept
re-planned, re-cut and solved with this row's `engine-content` into a derived-data root of its
own; other agents building, 44 to 53 GB of commit free; against the stream solver's figures for
the plan of 2026-11-12 at the same solver, in brackets). The plan: 724 links (784), 121.4 km of
line, made in 17 s with two fills. **91 pits breached** (the deepest 0.99 m, 19.5 ha between
them). **Twenty bodies of the plan**: the eight mountain basins, 28 to 135 m deep and 2.9 to
43.6 ha, each at its spill less the notch with one outlet; and twelve lowland ponds 1.0 to 3.3 m
deep, 0.07 to 14.8 ha, which a scene that does not want them breaches by raising `breach_max_m`
to 3.5. **One authored body under its spill**: the pond at the owner's mark 8, 1.18 m in a
hollow that spills at 4.05 m, where the outlet that climbed 1.77 m out of it no longer begins.
The lowland lake authored at -2 m is no longer in a hollow of the routing at all: its basin's
floor stands under the open sea's level, which is a sink. 179 mouths, 32 outlets.

| On the solved lines | before | after |
|---|---|---|
| stations over a bank | 31,964 on 255 lines, the worst 74.7 m | 6,447 on 161 lines, the worst 7.8 m |
| of them in a hollow of their own line | 26,395 | 3,567 |
| hollows over 20 m / 5 to 20 m / 1 to 5 m | 13 / 22 / 225 | 0 / 6 / 157 |
| lines that end higher than they begin | 88 | 27 |
| rises beside an over-bank station | 14,240 | 1,171 |
| wet links, wet line | 393, 37.0 km | 277, 25.2 km |
| falls | 872 | 752 |
| the seat: the most, and the line seated | 126 m, 134 km | 1.83 m, 35.5 km |

**What is not zero, traced** (2026-11-14, from the run's own files). The rule is held on the
plan's own bed by the engine's cases, and on the forest's plan no bed's line rises (`climbing`
0). On the solved lines 27 still end higher than they begin and 163 hollows over a metre remain,
0.26 km of line between them, a metre and a half long on average. **They stand where the plan's
line turns sharply in one step**: 130 of the 163 (16 of the 17 over 4 m) have their lowest
station within 3 m of a point where the line turns 45 degrees or more between one metre-long
step and the next, and the plan has only 470 such points in 237,049 (27 of them over 90
degrees, the sharpest 178). The deepest, 7.7 m in 0.24 m on link 644, is a hairpin of a metre's
radius in a cut 3.7 m wide at the top of an 8 m step: the plan's bed there is level to a
centimetre (214.97 to 214.87 m over ten points), and the solver's sections, which stand across
the line, swing through 120 degrees in under a metre, reach their 7 m limit over the step's edge
and read its foot (207.3 m) as the station's bed. So what is left is the line's shape, not its
level: a turn tighter than the cut is wide, at a lip. The case that bounds a line's turn
(`the line's turn between consecutive points is bounded on every reach`) holds on the engine's
scenes and not on these points, which were moved after it (the seat's and the bank's moves are
the suspects; not traced further). The mend is the planner's (a turn no tighter than half the
cut's width, held after every move of a point) or the solver's (a section's direction taken
over the cut's width, not one step); neither is built. The wet line is a third shorter because
the lines that crossed the basins' filled flats are gone, not because streams dried.

**The forest's mix** (2026-11-14, the owner's "probably a mix ... maybe one dried out over time
with no glacier or well feeding it"; the same staging, with the losses grounded on
[the hollows page](../experiments/hollows.md): `basin_evaporation_mm_y` 0, since the rain on a
lake passes its evaporation in this climate, `basin_seepage_mm_y` 2,190, the published median,
and two entries under `basins`). The plan: 726 links, 122.1 km, 91 pits breached, **eighteen
lakes with outlets, one body below its spill, one basin named dry**, the authored pond at mark
8 where its basin's water ends; no bed's line rises. Of the 726 links 280 are wet (24.9 km).
With the bed's seepage alone every hollow still fills: the balance leaves the eight mountain
basins 61 to 90 % of what runs in, so a lake below its spill needs a floor that leaks (the
smallest catchment for its area, 90 ha feeding 15, is short by 20 l/s), and a dry one a name.

| Basin (floor x, z) | Level m | Water ha | Deepest m | Catchment ha | In l/s | Lost l/s | Became |
|---|---|---|---|---|---|---|---|
| (332, 4908) | 157.31 | 43.6 | 88 | 494 | 135.0 | 30.1 | a lake at its spill; 105 l/s leaves |
| (144, 8080) | 322.46 | 18.9 | 135 | 134 | 34.5 | 13.1 | a lake at its spill; 21 l/s leaves |
| (-1412, 4824) | 263.24 | 10.8 | 91 | 153 | 42.6 | 7.5 | a lake at its spill, which is the region's west edge |
| (-488, 6932) | 592.57 | 10.6 | 124 | 94 | 25.0 | 7.3 | a lake at its spill; 18 l/s leaves |
| (-404, 8848) | 714.57 | 6.8 | 76 | 81 | 22.2 | 4.7 | a lake at its spill; 17 l/s leaves |
| (-1088, 8856) | 804.01 | 2.9 | 28 | 39 | 10.9 | 2.0 | a lake at its spill; 9 l/s leaves |
| (-1416, 7172), `leak_m3s` 0.02 | 660.07 | 7.1 | 57 | 90 | 24.9 | 24.9 | **a body 33.7 m below its spill** (693.81), no outlet: its floor passes 20 l/s and its bed 4.9 |
| (-1420, 7968), `dry` | none | none | bowl 58 | 23 | 6.8 | all | **the dried lake**: the highest and the least fed; its lines end at its floor |

The areas are under the spill; a deep bowl's water a notch lower covers the same to a tenth of
a hectare (43.4 and 18.8 ha flooded on a 4 m grid), a shallow pond's much less.

The twelve lowland ponds, 1.0 to 3.3 m deep, all at their spills with outlets (areas under
their spills): the two large ones are **14.8 ha at (576, -768)**, level 3.59 m, 1.9 m deep, fed
18.8 l/s by 74 ha, of which the water at its level floods 4.5 ha about its floor on a 4 m grid,
and **3.9 ha at (-1016, 256)**, level 3.88 m, 1.1 m deep, on a stream of 133 l/s; then 2.8 ha at
(-640, -768), 2.5 ha at (-44, 8), 1.3 ha at (-896, -704), 1.2 ha at (-976, 444), 1.0 ha at
(-144, -136), 0.6 ha at (-448, -768), 0.5 ha at (132, -28), 0.3 ha at (-640, 1316), 0.2 ha at
(68, 128) and 0.07 ha at (4, 1312). A scene that wants none of them sets `breach_max_m` 3.5.

**What stands where the owner's marks and the old climbing links were** (by position; a link's
number moves with the plan). Old links 37 and 779, which left the lowland lake at -2 m and
climbed to 2.6 and 0.2 m: gone. Nothing is cut within 25 m of link 37's line; along link 779's,
two short lines now run the other way, down into the lake (-0.90 to -2.02 m), beside a pond of
the plan at -0.71 m. Old link 360 (the owner's mark 6, the pocket in the hillside): its place
is **under the largest lake**, 3.7 to 5.2 m down; the nearest cut is 121 m away. Old link 482
(mark 7): the same line, still wet, and its station-to-station rises are still there (the
largest 2.0 m): it stands on a steep reach with the turns described above, not in a hollow of
the plan. Old link 778 (mark 8), the outlet that climbed out of the pond: gone; two lines now
end at the pond, which is where its basin's water ends. Their solved water stands 0.45 and 0.75
m under the pond's level at their last stations, flagged over the bank: the solver gives a line
that ends at an authored body under its spill no mouth to meet (not traced; the solver's).

**Seen on the forest** (2026-11-14, the mix loaded in `engine-view` at the owner's settings,
1920 by 1080: 19 minutes cold, 49 GB of commit at its peak, no body refused; twenty-one views
with ids and depth, kept outside the repository). The bodies are lakes: each follows its
valley's contour, with a turquoise fringe of shallows and the sky in it. What a person objects
to, most damaging first. **The forest stands in them**: the proof of concept's trees, logs and
rocks are authored instances placed before the lakes existed, and nothing removes an author's
instance under a body of the plan, so whole stands rise out of the water (about 17,000 of the
scene's 470,641 instances stand under the seven mountain bodies, 12,349 of them under the
largest); the bodies are in the scene's `water` for the author's own placement to read, and a
generator's placements read them through the exclusion as they read any body. **A lake has no
shore**: a bare slope enters the water as a ruled line with one thin white stroke along it, no
beach, no wet band, no rock, sedge or drift, where the reels' lakes have a strip of stones and
grass and trees to the waterline. **The body below its spill and the dried basin say nothing of
what they are**: the first is a lake in a bare bowl with no mark of the higher water, the second
a wooded hollow with no flat floor (that night; [a dried lake's floor](#a-dried-lakes-floor) came the next). **The
streams' own draw is not this row's and is wrong on this description** (flat cyan polygons and
slabs at the old link 482's place and at a lake's lip). The pond at the owner's mark 8 has a
whole shore: no hole and no triangle.

**To the gate** (the night of 2026-11-14, on the streams' branch and batch 64's main; what the
paragraphs above call not built, where it now is).

*The section's direction.* The solver's sections stand across the chord between the nodes half
the cut's width up and down the line, never less than one node each way (`solve_profile`,
profile.cpp), where they stood across one step. That is the solver's half of the mend named
above; the planner's (a turn no tighter than half the cut's width, held after every move of a
point) is still not built, and the engine's case that bounds a line's turn is unchanged.

*A line that ends on a scene's body under its spill ends in it.* The routing ends such a line
at the body's cells, which are a sink, but the line's own last point can stand a step short of
the outline, where the exclusion finds no water and the shorelines give no mouth: its bed was
then cut under the body's level and the solver ended it over a bank (the owner's mark 8: two
lines 0.45 and 0.75 m under the pond's 1.18 m). The link is now given the body's level as the
base level a mouth is held to (`plan_from_lattice`, after the shorelines), asked of the lattice
cell under the line's last point, since a piece the shorelines split off on its way holds no
cells. **That was not the forest's case**, which two re-plans found (`k_channels_version` 13
and 14, each cooked on the forest and each leaving the two lines as they were): the two lines
end at a confluence 27 m from the pond's floor, and the trunk that ran on from it into the
pond is dropped whole by the shorelines, being under the water or too short to keep, so they
ended in the air with their beds graded to a trunk that is not cut. **The links that end
where a dropped trunk began end in the body that took it** (`apply_water`: the body's level
kept by the trunk's first cell, and given as a mouth's level to every line whose last point
stands on that cell), at `k_channels_version` 15. The engine's case (a pond three metres
under its bowl's spill) holds the mouth and its level on every line that ends on the body's
cells; no engine case has a confluence beside a body, so none fails on the base: the forest's
two lines are what decide it. **Their re-run (run `n`, version 15): both now end in a mouth,
and neither at the pond's level.** `region link 25` and `region link 28` end at (-600, -276)
with the mouth's flags, their last stations written at 1.102 and 1.111 m where the pond stands
at 1.179: 7.7 and 6.8 cm under it, where they were 45 and 75. What is left is not the plan's
to mend. The pond is the consumer's authored polygon, whose edge is 5 m past the confluence,
and its level stands over the ground the lines cross to reach it. Held to the pond's level as
their base, the beds are no longer cut under it (link 25's stood 1.2 m under the pond three
quarters of the way down, lower than the pond's own floor; it is now at 1.17 m a quarter of
the way down, 0.97 at three quarters and 1.10 at the confluence), and on that flat a bed at
the pond's level has no bank to speak of: the pond's level, which the solver now carries up
both lines as a mouth's backwater, stands over the lower bank at 280 of link 25's 339
stations (from x -773 on; 92 of 190 before) and both of link 28's, and the solver writes a
bank's crest there and flags the station, as it does wherever water would leave its bed. The
depth written is 26 cm at the most and a film of 5 mm over the last 20 m, which the streams'
draw leaves out, and link 28's two stations rise 6 cm (the 23rd line that ends higher than it
begins): **a person sees a shallow bed whose water thins out and stops 5 m short of the
pond**, where before they saw a wet trench ending in the air half a metre under it. A pond at
that level would cover the flat the lines cross; its outline is the author's, and widening it
to its level's contour (or naming it at the level its shore stands at) is theirs. The plan
could carry a line across the short dry stretch a dropped trunk leaves (here 5 m, under the
12 m a run is kept at) so that it ends on the outline itself: not built. On the whole forest
the mend costs 185 stations over a bank (4,870 on 152 lines, 2,688 of them in a hollow of
their own line) and leaves every other figure of the table below as run `k` had it.

*A lake passes its springs on.* [The discharge](#discharge) carried a named source down to
the first mouth and no further, so the outlet of a lake that springs had filled ran with its
catchment's runoff less the lake's loss: nothing, in the engine's case (a spring of 1 m3/s
into a bowl whose bed seeps three times what its catchment sends), and 14 l/s below 0.66 m3/s
on the streams' judging scene, whose bed below the lake was drawn dry. A source now goes on
through a lake of the plan into its outlet. This is `network_flow`'s, which the cook's solve
reads and the plan does not: no plan changes and `k_channels_version` stays 15. The same walk
carries a lake's loss below the next lake down, which it did not, and that is all the forest
sees of it, having no source: solved again on run `n`'s plan (run `p`), 14 of its 278 lines
carry less, by 8 l/s at the most (0.128 to 0.121 m3/s on `region link 314`), and the solved
lines come to 47,949 stations, 4,868 of them over a bank on 152 lines and 2,689 of those in a
hollow of their own line; the hollows, the 23 lines, the nineteen bodies that hold, the 13,884
instances under water and the two lines at mark 8 are as run `n` left them. **What is left at
a lake's lip** (the judging scene cooked again with the mend, read as text): the outlet
carries 0.663 m3/s from its first station and runs 0.29 to 0.58 m deep from 10.6 m below the
shore on, but over those first 10.6 m, the sill the cut holds 2 cm over the lake, the bed has
no bank over it, so the solver writes a film of 5 mm at the sill's own height and flags the
stations (18 of the outlet's 44), and the draw leaves a film dry: a bar of gravel between the
lake and its stream, where two thirds of a cubic metre a second would go over the lip as a
sheet some 15 cm deep. The sill wants a section of its own (a notch the width of the bed with
banks, or the sheet written at its critical depth over the lip): not built.

*The forest again* (run `k`: the owner's mix with the dried basin named `infilled`; this row's
`engine-content` at `k_channels_version` 13, one job, into last night's derived-data root; the
machine shared with seven other rows building and loading, 18 to 41 GB of commit free). The
plan: 721 links, 122.5 km, 657 cuts, made in 48 s with two fills; 91 pits breached, **eighteen
lakes with outlets, one body below its spill, one infilled floor** (0.98 million m3 of fill
under 3.7 ha at 855.83 m), the authored pond where its basin's water ends; every level of the
table above to the centimetre; no bed's line rises and none ends higher. All nineteen bodies
hold by the cook's flood (2.7 s).

| On the solved lines | last night (run `i`) | tonight (run `k`) |
|---|---|---|
| stations over a bank | 6,283 on 160 lines, the worst 7.8 m | 4,685 on 152 lines, the worst 6.5 m |
| of them in a hollow of their own line | 3,567 (run `d`) | 2,609 |
| hollows over 1 m / over 5 m, the deepest | 163 / 6, 7.7 m (run `d`) | 57 / 3, 6.4 m |
| lines that end higher than they begin | 27 (run `d`) | 22 of 278 |
| wet links, wet line | 280 of 726, 24.9 km | 278 of 721, 25.3 km |

So the direction over the cut's width takes two thirds of the hollows over a metre and leaves
57, three of them over 5 m; those and the 22 lines were not traced further. **13,884 of the
scene's 470,641 authored instances stand in a body's water** (4.4 s to count and list them):
5,922 in the largest lake, 1,699 and 990 in the two largest ponds, 21 in the sea and 6 in the
authored pond. Last night's figure (about 17,000, 12,349 in the largest lake) was a count by
box and level; this is by each body's own flood.

**General purpose.** The fill, the breach and the bodies come from the heights of whatever
ground the scene names, an imported heightfield as readily as a generated one: nothing reads
which generator made it, and every limit is a field of the layer. **A scene with no `channels`
entry pays nothing**: none of this runs. **A hand-drawn `WaterChannel` through a hollow** gets
what it got: the solver's over-bank flag on the stations in the hollow; what this adds for its
author is the plan's list of hollows (`engine-content channels`, `hollows.list`: each one's
floor, spill, depth, area and what became of it), where one stands on the same ground.

**What it costs.** The fill, the routing and the labelling are one more sweep of the lattice a
pass and one more pass a ground with a terminal or an infilled basin; the plan's file grows by
153 bytes a hollow that is not simply breached, four bytes a point of line (`widen`), and four
bytes a lattice point of an infilled hollow's box.

### A dried lake's floor

(2026-11-14, the owner on the forest's dried basin: "maybe one dried out over time". The
grounding is [the hollows page](../experiments/hollows.md#a-dried-lakes-floor).) Two things a
dry hollow can be, and the author says which. **One that drains underground** (`dry`) keeps its
bowl, and its lines end at its floor; nothing was read about what such a floor looks like, and
nothing is built for it. **One that silted up** (`infilled`) is a lake that filled with its own
sediment: the surveyed floors of relict lakes stand at 0 to 3 % with 0 % the representative
slope, at about the level of the old outlet, with a low-gradient stream crossing them.

**An infilled hollow is ground.** In `route_hollows` the hollow's cells are raised to its spill
on the plan's own lattice and the fill and the routing are taken again over what is now a flat,
so the lines that reach it cross it along the fill's own routing and leave through the old lip
as over any ground: no body, no sink, no mouth, and the bed's line falls or holds all the way
(the flat's bed is a thalweg under the floor, which is what passes the lip). The hollow's record
(`HollowKind::Infilled`, `level_m` the floor) carries **the floor as a grid** on that lattice:
0 on the hollow's cells and on the ring of cells round them, so the floor runs flat to its
walls with no slot between a deep cell and the wall beside it and nothing is raised over the
level; far under any ground everywhere else in its box, so nothing else is raised.

**The edit** (`terrain_fills_json`; `engine-content channels --cut` writes it among `edits`,
and counts `hollows.floors_written`): one `engine.scene.TerrainEdit` of mode `Fill` a floor, a
patch whose outline is the grid's box and whose grid is the floor. The tile set composes every
fill before every cut, so the cuts of the lines that cross the floor are made in it. A hollow
whose box passes a ground edit's 1,024 points a side is refused by its entry's name.

**The ground the plan's lines lie on** (`floored_ground`). The planner lays its lines, seats,
sections and flood beds on the ground with its floors in it, by the edit's own rule (the grid's
bilinear surface where it stands over the ground), since the bowl under the floor is not where
the line is. A host's ground has the edit in its tile set. `engine-content channels`, which
holds the ground before its edits, asks the same wrapped ground for everything it sets a line
against (the water's ribbon, the seat's measure, the clasts).

**Its cover is the ground's own.** The masks are built from the tile set's heights with the
edit in them, so a level floor takes what the priors give level ground (their soil, no scree,
no bedrock), and the placements that read the ground stand on it as on any flat. **Not built**:
a fen's or a wet meadow's own material and sedges (nothing names one), the floor's own
wetness, the stream's sinuosity beyond the planner's meander rule, a fen's pond.

**What it costs.** One more pass of the fill and the routing; the floor's grid in the plan
(four bytes a lattice point of the box); one patch in the tile set's edits, read only by the
tiles its box touches. **A scene with no `infilled` basin pays nothing**: no pass, no edit, and
`floored_ground` hands back the ground it was given. The case
(`hollows: a lake that silted up is a floor its stream crosses`) is the bowl on a slope named
infilled: the floor flat at the old spill, the ground itself everywhere else and nowhere
raised over the level, the stream on the floor with its bed under it, the edit, the same
through the plan's file, and the plan's and the edit's bytes pinned for every toolchain.

### A lake's look

(2026-11-14; the grounding is [the hollows page](../experiments/hollows.md#a-lakes-look).) A
body the plan wrote was a `WaterBody` at its defaults: a pool. `plan_bodies_json` now gives
each what its own size says, from the body's own water (`Hollow::water_m2`, `water_m3`: the
area and volume under its level, which for a shallow pond a notch under its spill is far less
than its hollow holds):

| Field | What it is given | From |
|---|---|---|
| `wind_m_s` | the layer's `basin_wind_m_s` (4 m/s; 0 writes none) times `(1 - exp(-0.3 A / km2))^(1/3)` | Hondzo & Stefan's sheltering by area, which scales the wind's energy; the cube root is this page's step |
| `wet_m` | 0.15 m, that wind's wave over the body's reach, and half the yearly range of a lake's level (0.12 m with an outlet, 0.17 m without) | a sand's capillary rise; the growth law the water draws by; House 1985 |
| `depth_fade_m` | the mean depth, between 0.5 and 8 m | Carlson's range of Secchi depths; the selector is this page's |
| `shore_m` | two hundredths of the body's side, between 0.75 and 2 m | not from a source: what a pond can spare |
| `seed` | its place among the plan's bodies | so two lakes' waves are not one |

With a wind the water capability draws a closed body's waves from the spectrum over its own
reach, with a lee under the shore the wind comes from, in place of four regular sines; a pond
of a fifth of a hectare gets a twelfth of the region's wind and is nearly a mirror. The wind
is **not part of what a plan is keyed by** (`plan_layer_hash` leaves it out, as it leaves the
flow): it is read when the bodies are written. The exponential and the cube root are the
tree's own (`det::exp`, `det::log`), so the block is the same text from every toolchain, and
the case pins it. **What it costs**: nothing at a load beyond what a body with a wind costs
the water (its rings past the outline for the swash); a scene whose author wants the old
blocks sets `basin_wind_m_s` 0. **For the water's owner**: a body with a wind grows its mesh
over the bank by twice its wave and a quarter metre, and the cook's flood check
(`check_fill`) mirrors the flood at the level only.

### What stands in the water

(2026-11-14.) A body the plan writes stands where a scene's author placed things before it
existed, and nothing removes an author's instance. **The cook counts and lists them**:
`engine-content flow solve`, which already floods every body that fills over the drawn ground,
asks of each of the scene's own `instances` whether the ground at its x and z stands under a
body's level in that body's water (a filling body's own flood; a circle's or a polygon's
inside; an open body's anywhere), counts it under the first body of the scene's list that
covers it, prints `under_water` on its line (`instances`, `under`, `by_body`) and a sentence
on stderr, and with `--under-water <list.json>` writes every one: its place in the scene's
`instances`, its mesh, where it stands and how deep the ground under it is, a body at a time.
That file is what a consumer's own placement reads to move or drop them. An authored body is
counted as the plan's are; a scene with no body or no instance pays one look.

**And in a line's water** (W138, 2026-11-16; the forest's need 130: after W130.2's pools 26 of its
instances newly stood in channel water, 22 on one line whose pools reach 49 m wide, and the list
did not name one). An instance no body covers is asked of the solved lines too
(`channels::ChannelWater`, `in_water.h`): it stands in a line's water where it lies within the wet
reach of a stretch the draw lays a sheet on — each side by its own `wet_left_m` or `wet_right_m`,
the level and the reach interpolated along the stretch as the draw does, a fall's stretch left out
(a nappe is in the air) — and the ground there stands under that level. The line prints
`in_lines` and `by_line` beside `under` and `by_body`, a sentence on stderr, and the list file a
`lines` array as its `bodies` (each line's name, count, deepest and instances). The lines are found
through a grid of 16 m squares holding the stretches whose reach touches each, so an instance costs
the few stretches near it; a scene with no wet line pays nothing.

**A generator's placements keep off a body of the plan as off any body**: the plan's block is
an ordinary `WaterBody` in the scene's `water`, which is what the exclusion every placement
generator asks is built from. The case
(`hollows: a body of the plan carries its size's look, and placements keep off it`) reads the
block through the scene's schema, builds the exclusion from it and holds every lattice point
of the plan's own water excluded, the valley above and beside it not. **A finding, not this
row's to mend**: the exclusion knows a filling body as its box and the ground under its level
in it (the flood runs later), so on a mountainside it also keeps placements off dry ground
that lies under the lake's level inside the box, down the outlet's valley.

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

## Where the water leaves its bed (S104)

(2026-11-12, roadmap S104, the build's stages 0 and 1; the design, its grounding and its sources
are [the waterfalls page](../experiments/waterfalls.md), whose "Built: stages 0 and 1" says what
was read again tonight.) Water is a projectile its bed may hold up and cannot pull down. `falls.h`
holds two things, both new files beside the plan and neither touching it: `k_channels_version`,
the plan's format and `ChannelLayer` are as they were, and the falls carry `k_falls_version` (1)
of their own.

**The test, on the solver's own steps** (stage 0, `cook_falls`). A run is a maximal run of a
link's stretches whose cut bed (`ChannelLink::bed`, after the seat, the chute's limit and the fan)
falls at least its run, 45 degrees: the bed the water would draw as falling today. **Every link is
read**, whether or not the water's description draws it: the island's streams over their sea
cliff drain under `--min-area-m2` and carry no water block, so a count over the description never
sees them; `drawn` and `drawn_falls` say which falls the description draws. Each run is marched
(below) over a window from two of the lip's widths above it to two below its last point, nine
columns across the water's width at the lip, each with its head over the lip's section and a share
of the discharge as that head to the 3/2 power. A column's flight is a **fall** when it drops at
least **three critical depths** (Rand's water behind the nappe is under half the drop there) and
its critical depth is at least **a centimetre**; a run's kind is its highest falling column's —
a plunge (it comes down in water), a horsetail (on a face, and slides), tiered (two falls in one
column) — else what its columns came to: held (no column left the bed), drowned (it left, short of
three critical depths: a riser and S103's hole) or a trickle (under a sheet's discharge: wet rock).
**At a mouth** the landing past the line's last point is the body's level, and a run that reaches a
mouth link's last point is counted under `mouth_falls` whatever it lands on.

**The march** (stage 1, `march_column`). The water's depth at a lip is `stage` of the thalweg as
the description lays it in the cut, its speed Manning's at the description's n (0.04) over the bed's
fall across the knickpoint's window above the lip. On the bed the speed follows the energy equation
against Manning's friction slope, stepped every 5 cm in a half-implicit form that tends to the
normal speed and never overshoots it. At each point the column **leaves** where the parabola launched
along the stretch above at its speed (the brink's, q over 0.715 d_c, where it arrives subcritical)
stands more than a millimetre over the bed at the next point; aloft it is the free jet in closed
form, and it **lands** at the first root of the parabola against each stretch's surface in turn —
the rock of a face, the water standing over any other stretch, a pool's level backed up against a
face's foot, a body's level past a mouth — keeping the part of its velocity along what it struck,
and **slides** on a face. The path is sampled by the bed's points and, aloft, by a step from a
quarter of the sheet's thickness at the lip growing to a metre. Every decision is + - x / and a
square root, the cube root a fixed Newton iteration, and the links are cooked a job each and merged
by link index: the same bytes from every toolchain and at any thread count.

**The data the water reads** (`falls_bytes`, `read_falls`, the reader; `ensure_falls` keeps it at
`<derived-data root>/channels/falls-<plan key>-<options hash>.channel-falls`, beside the plan, and
`engine-content falls` cooks it and prints the count). The water capability may not link this
module (ADR-0027), so the layout is the contract, little-endian, each record its bytes with no
padding (pinned in the size table):

| Part | Bytes | Fields |
|---|---|---|
| header | 24 | `u32` magic `CHFL`, `u32` `k_falls_version`, `u64` the plan's key, `u64` the options' hash |
| stats | 120 | 14 `u64` (links, runs, held, drowned, trickles, falls, plunges, horsetails, tiered, drawn, mouth, knick, columns, vertices), `f64` the highest drop |
| `Fall` | a `u32` count, then 88 a fall | `i64` x, y, z of the lip (world mm; y its bed); `u32` link, lip point, run's last point; `i32` heading x, z (1e-4); `u32` discharge (ml/s), critical depth (um), launch speed (mm/s); `i32` drop (mm); `u32` break-up length (mm, K = 0.77: the design page's own fit); `i32` landing height from the lip (mm); `u32` water depth there (mm); `u32` first column, columns; `u8` kind, landing, reach above, turbulence class, knick, drawn, two pad |
| `FallColumn` | a `u32` count, then 28 a column | `i32` offset across, left positive (mm); `u32` q (mm²/s), critical depth (um); `i32` drop (mm); `u32` first vertex, vertices; `u8` kind, landing, flights, pad |
| `FallVertex` | a `u32` count, then 28 a vertex | `i32` x, y, z from the lip (mm; y the lower face of the water); `u32` time from the column's first vertex (us), speed (mm/s), thickness q over the speed (um); `u8` phase (0 bed, 1 air, 2 slide), three pad |
| closing | 8 | `u64` the hash of everything before it |

A reader refuses a file of another magic or version, one that does not hash to its closing, and one
whose indices run past their arrays. The sheet's mesh is then the columns' paths side by side: a
row a vertex, UV.x the travel time, UV.y the thickness, as the design asks (waterfalls.md, "What is
drawn").

**Held** (`tests/falls_tests.cpp`): the march against the free jet's closed forms for the design's
scenes A, B and C to rounding (the landing v_b sqrt(2H/g) out, after sqrt(2H/g), at
sqrt(v_b² + 2gH); the strike 2 v_b² tan² θ / g down a 60-degree face and the film at Manning's
normal speed on it); the count on cliffs of known heights down a fall line and across a slope; a
sea cliff at a mouth; and the bytes at one job and six, read back and pinned (the pin over a
network made by hand, so it holds the cook and not the planner).

**Measured** (2026-11-13, `msvc-release`, `channels.falls_cook` with `--wait-quiet=120`, which
proceeded on a busy machine: other processes at 28 % of the CPU and the GPU lock held by another
agent's frame budget, so an upper bound): 640 runs on 64 links made by hand in 529 ms on one
thread, **0.83 ms a run** (nine columns marched over the window of four widths about the run, the flights in closed
form); the file holds **about 11.7 KB a run** (nine columns of about 45 vertices at 28 bytes, the
column and fall records 340 bytes). On W93's 4.1 km ridged heightfield (305 links, 16.8 km²,
no steps in its beds) the cook found no run and took 5 ms; `engine-content falls` there was 6.1 s,
all of it the plan (3.6 s) and the uncut ground's set.

**What the count found on b7681ac9** (the cliffs' scene: a valley at 3 % with cliffs of 3, 7, 10
and 14 m, a channel about 1.2 m wide, eight plans): every cliff is found, at a marked knickpoint,
a plunge; but **the seat (W116) eats steps where a line meets a cliff at an angle**, down a fall
line as much as across a slope. A point's lower bank is read across the line's heading; on or just
above a face that the line crosses at 20 to 30 degrees the reading lands on the ground below it, so
the seat is the step, and its dilation over two points lowers the lip a step deep a metre upstream
and leaves a pit a step deep at the foot, until the floor of two thalwegs under the ground brings
the bed back up. Seed 2 down the fall line: 12.9 m of seat at the 14 m cliff, two runs, drops of
21.9 and 13.9 m; with the move off, 0.64 m. A case marked `may_fail` holds what the rule should
give (each cliff one run of its own height, no pit at its foot), for the row that fixes the seat.
**W122 fixed the seat** ([The section seated under its lower bank](#the-section-seated-under-its-lower-bank-w116),
"The seat, reshaped"): every cliff is one run with no pit on all eight plans, and what the marked
case still fails on is one run's drop, short by the count's own 45-degree test at a lip.

**A sea cliff at a mouth** (a valley at 3 % over a 20 m cliff whose foot stands in the sea): the
link ends at the shoreline on the cliff's face and its fan grades the last three widths of bed to
the sea, so the cliff in the bed is a ramp of about 80 degrees; the lip's water strikes it 8.4 m
down and slides into the sea, a horsetail, where the cliff itself would be a plunge. The fan is the
rule for a stream meeting a body over low ground, not over a cliff.

## Invariants

- The network is a function of the layer, the seed and the ground's answers at the lattice's
  millimetres: the same at one job and six (test: "the same network at one job and six").
- **No link's bed rises downstream** (W122): the bed's line (the cut's bed less a step-pool's own
  pools) never stands over itself a point upstream, in a link or across a junction, and no link
  ends over where it began; an outlet's lip, held at its body's sill, is the one stretch not
  measured (tests: `hollows: a bowl on a slope stands at its spill, and no line climbs out of
  it`, and each of the hollows' cases after it).
- **No line lies under a body of the plan and no channel begins on one**; a mouth into one
  carries its level exactly (the same test).
- **The plan is the same bytes at one job and six and through its file**, hollows and bodies
  included (`hollows: the same plan at one job and six, and through its file`).
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
