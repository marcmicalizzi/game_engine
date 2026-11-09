# channels (domain, capability)

**Purpose.** Channels from the ground's drainage (roadmap W86; the owner, 2026-11-06: "running water —
rivers, downhill rivers, waterfalls"). The placement generator `channels` routes the flow over a
lattice of the ground it is handed, begins channels where contributing area and slope say a channel
begins, follows them downstream, gives each point a bankfull width and depth, and **dresses** the
channel with a gravel bed skin and bars of finer sediment, so a riverbed without water reads as
eroded. Since W89 the channel is **cut into the ground** below its floodplain: `engine-content
channels --cut` writes the terrain's `cuts` beside the water's description, the tile set is built with
them ([tileset](tileset.md#channels-cut-into-the-ground)), and the bed and bars lie in the cut
([The cut](#the-cut)). The same network is the description the flowing water
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
receivers, contributing area, channel cells, heads and links with their lines) is computed when an
entry is opened and is the source of truth for every piece and every `WaterChannel` block made from
it; the recipes (`<ddc>/channels/channel-<hash>.channel.json`) are derived data.

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

## What it places

`place_pieces` cuts each link's line into pieces of at most `piece_m` (32 m), each standing at its
first point's whole millimetre with y the ground there, filed in the world tile that point is in, and
gives each piece up to three meshes, **in link order, piece order, then bed, bars, boulders** (W90;
W86's banks are placed no more) — the order the ids (1, 2, …) are given in, so a tile's placements
carry the same ids whichever tile came first. A tributary's pieces stop at its trunk's banks, and a
reach decides which parts it has ([Reaches](#reaches)): the bed is a slab a run of rows whose reach
has a bed.

- **The bed.** A slab the width's wide, `columns` points across (one per 0.4 m, 3 to 17, constant
  along a link so the pieces' joins agree), its top the ground plus 2 cm plus the gravel relief (up to one
  median grain, `D50 = h S / (1.65 τ*bf)`, clamped to 0.5–3 cm, a hash of the vertex's world place)
  plus the riffle's rise (`riffle_rise` of the depth at the riffles of the pool-to-pool rhythm,
  highest mid-bed); its two edge columns down at the ground plus 2 cm, where the banks begin; its
  bottom 25 cm under the ground.
- **The bars.** One every half a pool-to-pool length, alternating sides along a straight reach and
  on the inside of a bend; a lens `bar_width_widths` wide and `bar_length_widths` long with its outer
  edge at the bed's, its crown `bar_rise` of the depth above the bed's relief. None on a bed narrower
  than `bar_min_width_m`.
- **No banks** since W89. W86 laid a berm each side that rose *above* the ground, so every stream
  read as a canal between levees, its 32 m pieces showed as blocks with gaps on a bend, and its faces'
  UVs smeared. The banks are now the cut's own faces ([The cut](#the-cut)): ground, continuous
  round a bend, textured as the ground is. The layer's `bank_*` fields still read and are checked, and
  place nothing.

The bed's width, and the bars' outer edge, are the cut's **flat bed** (`bank_slope`'s trapezoid
below), a quarter of the width at least where the section is a V, so the gravel lies on the bed and
not up the banks.

Each part is a **recipe** (`engine.channels.ChannelPiece`): its slabs with every height decided here,
in the piece's frame, and its look. The mesh generator `channel` builds a recipe into one closed mesh
per piece — each slab its top, its bottom and four walls, welded by position, outward — and a
container through the derived-data cache, exactly as the deposits' patches are built. A recipe's file
is named by its canonical JSON's hash, so two pieces that say the same thing share one entry.

### Materials

Each part names a material-pack role (`surface.role`) or, empty, takes the first its pack has of a
list: the bed `gravel/bed`, `rock/gravel`, `rock/granite`; the bars `sand/bar`, `soil/parent`,
`soil/subsoil`; the banks `soil/subsoil`, `soil/topsoil`. The set is sampled at `surface.tile_m` in
metres of world x and z (each piece's UVs carry its origin modulo 1,024 m, so the texels run on across
the joins) and `Stochastic` by default (R131). With a pack, `surface.tint` multiplies the set (absent: the bed 0.62, 0.60, 0.56, the banks 0.8, 0.75, 0.7, the bars none). With no pack, the part's flat colour (`surface.color`,
or the part's own: grey gravel, pale bar sand, dark soil). **The POC's v30 pack has no gravel role**:
a bed there samples `rock/granite`; a gravel set in the pack is a request on its owner.

## In a scene

```json
{"generator": "channels", "stream": false,
 "params": {"name": "fells", "seed": 1, "region_min": [-512, 1024], "region_max": [512, 2048],
            "head_threshold_m2": 1500}}
```

The entry is opened once: the network is planned, every piece's recipe written under the host's
derived-data root's `channels/` (else the repository's `ddc/`, else `ddc/` beside the scene), and
`expand`/`tile`/`occupies` hand out what was decided. A moving ground is refused (the dunes have no
drainage to dress). Every field, its default and range is in `schemas/channels.schema`.

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
whose set is cut, plans over the cut ground: its lines follow the troughs the cuts deepened, and its
bed and bars, which stand on the ground at every vertex, lie in the cut; it does not know the cuts and
writes nothing of them.

**Bed and bars: pieces, not mask paints** (the brief's step 2). The bed and the bars stay recipes
laid on the cut ground — the bed narrowed to the cut's flat bed, no bank piece — rather than terrain
mask paints (`gravel/bed` and `sand/bar` laid along the cut by the masks): a piece carries the
gravel's relief and the riffles' rise, which a paint cannot; it costs none of the masks' four layers a
tile, two of which a gravel bed and a sand bar would take wherever a channel crosses a meadow, a trail
or a shore (the budget T72's check holds); and it needs no mask cook when a channel moves. The cost is the slab's edges, which on
the cut lie at the banks' toe, a few centimetres over the ground.

**Seen** (2026-11-08, `msvc-debug`, 1920×1080, `--raster hw --shadows rt`, world tiles with v35's
rings): a synthetic flat ground at 25 cm with a 200 m link bending 25 m, 3 m wide and 0.6 m to its
bed, and a 1.5 m tributary joining it, authored by hand. The channel lies in the ground, its banks
falling to the water at their slope and shadowed on the sun's far side, continuous round the bend
with no gap and no levee, the water inside them; beside it the same water laid on the uncut ground
stands on it as a raised strip. Past the 1 m ring the channel narrows to the 2 m and 4 m rings'
point samples and reads jagged, and the 1.5 m tributary fades to a line, as the cell rule and the
rings' sampling say it should. v35 itself was not re-cooked tonight (its set is 2.6 GB at 25 cm; see
the roadmap row).

**Steep reaches** (the owner's item 10: 30–40° faces) get the same cut — a gully is still cut into
its slope — and since W90 their pieces follow their reach ([Reaches](#reaches)): nothing laid on a
colluvial or bedrock reach, boulder steps on a step-pool reach and boulder clusters on a cascade
(W91).

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

**Public API.** `include/domain/channels/channels.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `jobs`, `assets`,
`geometry`, `materials`, `texture`, `scene_gen`, `channels_schemas`.

**Testing.** `tools/dev.ps1 test -Filter channels` (the module's tests and engine-content's
`channels` end to end).

**Performance notes.** Nothing runs per frame. Opening an entry costs the lattice's height queries
(one per point, rows on the job pool), a heap fill and a sort over the points, and a few height
queries per placed vertex; the region's `max_samples` bounds it. The pieces are ordinary cluster-LOD
scene instances. `ChannelPoint` (40 bytes) is pinned in the size table.

**Not yet.** The cut is written by the subcommand and pasted by the author, not applied by the generator itself (the ground is built before the placements are); a procedural ground (the dunes) takes no cut; D8 only (no D∞ spreading); heads by a local four-metre slope, not a slope
averaged over the source area; no braided reaches; the valley's confinement is not read for the
reach; the ribbon's fit narrows the water where the ground across the line is lower than its surface
but does not move the cut, so on a cross-slope the cut's bench still has no downhill bank (the water
narrows to what the bench holds); knickpoints are read from the ground's profile along the line, not
from rock strength; **the network is planned again on every load** (24–32 s on v35, T73's finding): caching it as
a derived node needs the ground's identity, which `scene_gen::Context` does not expose (W90's report
asks for it); the mouths read the exclusion's box for a fill body, not its traced flood.

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
