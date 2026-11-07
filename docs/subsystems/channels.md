# channels (domain, capability)

**Purpose.** Channels from the ground's drainage (roadmap W86; the owner, 2026-11-06: "running water —
rivers, downhill rivers, waterfalls"). The placement generator `channels` routes the flow over a
lattice of the ground it is handed, begins channels where contributing area and slope say a channel
begins, follows them downstream, gives each point a bankfull width and depth, and **dresses** the
trough the ground already holds with a gravel bed skin, bars of finer sediment and cut-soil banks, so
a riverbed without water reads as eroded. The same network is the description the flowing water
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
gives each piece up to three meshes, **in link order, piece order, then bed, bars, banks** — the
order the ids (1, 2, …) are given in, so a tile's placements carry the same ids whichever tile came
first.

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
- **The banks.** Where the link is a stream's (`bank_min_width_m`, 1.5 m) and not a rill's: a berm
  each side from 5 cm inside the bed's edge out `bank_width_widths w + bank_width_m`, its face rising
  over the first fifth to `bank_height` of the depth (half again on the outside of a bend, half less
  inside) and its back falling to the ground. The banks' outer edge is the **riparian edge** a later
  consumer can read: half the width plus the bank's reach from the line.

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

`engine-content channels <scene.json> <out.json> [--min-area-m2 <m2>] [--stage <f>] [--entry <i>]`
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
- The `WaterChannel` blocks have six fields, one value per point, and a surface that never rises
  downstream (test: "the water's description has the WaterChannel shape"; the subcommand's test).

**Public API.** `include/domain/channels/channels.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `io`, `jobs`, `assets`,
`geometry`, `materials`, `texture`, `scene_gen`, `channels_schemas`.

**Testing.** `tools/dev.ps1 test -Filter channels` (the module's tests and engine-content's
`channels` end to end).

**Performance notes.** Nothing runs per frame. Opening an entry costs the lattice's height queries
(one per point, rows on the job pool), a heap fill and a sort over the points, and a few height
queries per placed vertex; the region's `max_samples` bounds it. The pieces are ordinary cluster-LOD
scene instances. `ChannelPoint` (40 bytes) is pinned in the size table.

**Not yet.** The ground is not cut (the erosion model's job); no waterfalls (a link's steep reach is
step-pool rhythm only); D8 only (no D∞ spreading); heads by a local four-metre slope, not a slope
averaged over the source area; no braided reaches.

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
