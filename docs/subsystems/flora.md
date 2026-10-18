# flora (domain, capability)

**Purpose.** The engine's tree solver: a species' parameters and a seed in, a mesh out, the same
bytes every time and on every toolchain. Vegetation in this engine comes from a solver and not from
modelled assets or image-to-3D, which is the owner's direction for it (plan 05 §5.15, "trees under
wind") and what E10 measured: a generated mesh loses its detail past a person's size
([e10](../experiments/e10-generated-props.md)), and a tree is several times that. The first species
is the **date palm** (`flora::solve_palm`, `engine.flora.Palm`), because Desert Survival's oases
need palms before anything else (plan 13 §13.1, "Oases"). It does not place what it makes (a scene's
instances do, and a scatter layer will — roadmap G55), move it in the wind (S66), or texture it yet
(G60).

**Why this shape.** A solver's output is a mesh like any other, so it reuses everything a glTF goes
through rather than growing a path of its own: an `assets::MeshData` into `geometry::build_cluster_lod`
for the LOD DAG, the `.clusters` container, and the derived-data cache keyed by content. The solver
is a pure function in the domain layer with no io — its callers read the file and hand over the
text — so the content build, the renderer's scene reader and a test all call the same code.

## The date palm

`engine.flora.Palm` (`schemas/flora.schema`) is every parameter the solver reads; the defaults make
a 9 m palm. A parameter out of its range is refused with its name (`palm_in_range`).

| Field | Range | Default | What it is |
|---|---|---|---|
| `seed` | any u64 | 1 | everything left to chance: the lean's azimuth, each ring's twist, each frond's turn and spread |
| `height_m` | 1–30 m | 9 | the trunk's apex above its base, where the fronds spring |
| `trunk_base_radius_m` | 0.05–1 m | 0.24 | the radius at the ground, at a scar ring's lip |
| `trunk_top_radius_m` | 0.03 m – base | 0.18 | the radius at the apex |
| `lean_deg` | 0–40° | 7 | how far from vertical the trunk leaves the ground |
| `curve` | 0–1 | 0.5 | how much of the lean the spine takes back by the apex (1: vertical again) |
| `ring_spacing_m` | 0.04–1 m | 0.11 | one leaf-scar ring every this many metres |
| `ring_depth_m` | 0 – min(0.1 m, top radius / 4) | 0.014 | how deep a scar cuts below the radius |
| `frond_count` | 1–64 | 26 | green fronds in the crown |
| `frond_length_m` | 0.5–8 m | 3.8 | a green frond's rachis |
| `frond_droop` | 0–1 | 0.45 | how far a rachis curves down (1: its tip 100° further from vertical than its base) |
| `frond_spread_deg` | 5–120° | 75 | the crown's cone: the outermost frond's angle from vertical at the apex |
| `leaflet_count` | 2–160 | 64 | leaflets on each side of a frond |
| `leaflet_width_m` | 0.005–0.12 m | 0.04 | a leaflet's base, never wider than the gap between two |
| `leaflet_angle_deg` | 10–85° | 38 | a leaflet's angle to the rachis, towards the tip |
| `dead_skirt_fronds` | 0–64 | 10 | dead brown fronds hanging below the crown |
| `triangle_budget` | 2,000–2,000,000 | 60,000 | the most triangles the mesh may have ([the budget](#the-budget)) |

### The budget

The solver counts the triangles a plan makes before it builds anything (the count is exact: the
test holds `PalmReport::triangles` to the index buffer), builds the full detail when it fits
`triangle_budget`, and otherwise walks down a fixed ladder one rung at a time until it does:

1. detail that does not change the outline — leaflets of two segments, scar rings of two rows,
   leaflets of one segment (a single triangle each);
2. then the trunk's and the rachis's cross-sections — 12 segments round the trunk, a ribbon
   rachis instead of a keeled trough, two petiole rows, 8 segments, one row a ring;
3. then fewer, wider leaflets, halved repeatedly with their width doubled so a frond keeps its
   leaf area (a frond of half the leaflets twice as wide reads as the same frond from a distance);
4. then rings merged two by two, and six segments round the trunk last.

The fronds themselves are never dropped: the crown's outline is what a palm is recognized by. A
budget that cannot hold even the bottom rung is refused with the count it would need; inside the
documented ranges that does not happen (the test builds everything at its most expensive on
2,000). The default palm is 52,704 triangles at full detail — trunk 10,512, crown 30,472, skirt
11,720 — under its 60,000; on 20,000 it is 18,132 (one-segment leaflets, 12 segments, two rows a
ring), and on 2,000, 1,896 (two leaflets a side).

### The trunk

A tapered generalized cylinder swept along a spine. The spine leans towards an azimuth drawn from
the seed, and its angle from vertical falls linearly with arc length, from `lean_deg` at the ground
to `(1 - curve)` of it at the apex: that has a closed form (the height after an arc *s* is
(sin *a* − sin(*a* − *bs*)) / *b*), so the arc length that puts the apex at exactly `height_m` is
solved for, not searched. The radius tapers linearly from base to apex, and the **leaf-scar rings**
are a radial profile along the spine: each ring is a leaf base's boot, the wall stepping in sharply
at the scar and swelling back out to the boot's lip, drawn with four rows a ring (two or one when
the budget asks). The lip is where the radius is the one asked for. Each ring turns a few degrees
against the one below it, from the seed, interpolated through the ring so the wall never shears —
what makes the scars read as grown rather than lathed. The apex is closed by a low cone. One UV
island: *u* once round with one seam column, *v* up the trunk at the same scale so a bark texture is
not stretched; the cap is a small disc of its own beside it.

### The crown and the skirt

`frond_count` green fronds spring from the apex, round the trunk's own axis there, on the golden
angle as a palm's leaves come; frond *k* of *n* leaves at `frond_spread_deg` × √((*k* + ½)/*n*) from
the axis (so the youngest stands up the middle and the oldest at the cone's edge, the cone filled
evenly by area), the older ones springing a little lower and outward, and every frond is turned and
tilted a little from the seed. A frond's **rachis** is a curve whose angle from the axis grows with
the square of its arc length — by `frond_droop` × 100° at the tip, more for the outer fronds — so a
frond rises, arches and droops; its cross-section is a trough (two edges and a keel below, a ribbon
when the budget asks), a petiole's width at the base and millimetres at the tip. Below the first
leaflet is a bare petiole (22% of the rachis). On each side, `leaflet_count` **leaflets** stand on
the rachis's own edge vertices — each a strip from two base corners, out at `leaflet_angle_deg`
towards the tip and 28° up out of the rachis's plane (a date palm's fronds are V-shaped in
section), tapering to a point and sagging a little; longest a little past the middle of the frond
and at most a ninth of its length.

**Why a frond is one connected surface.** The LOD builder can only collapse what is connected: a
leaflet made as a loose strip keeps its last triangle at every level, and a crown of a few thousand
of them does not coarsen at all. Standing every leaflet on the rachis's edge makes a frond one
surface whose leaflets collapse into the rachis and whose rachis then collapses along itself.
Every triangle is wound to face the frond's upper side, so the area-weighted normals the crown gets
are a surface's and not a cancellation.

**Why leaflets are geometry and the crown is two-sided.** Alpha-tested cards are roadmap R65 and
open, so a leaflet is triangles, seen from both faces: the crown's material is `double_sided`, and
`build_flora_container` builds that primitive's clusters with `normal_cones = false` and the
trunk's with cones on — two primitives, two cone settings, one mesh (geometry.md, "Normal cones").
The resolve turns a shading normal that ended up behind the surface to the camera's side, which is
what makes one sheet of triangles read from below as well as above.

`dead_skirt_fronds` **dead fronds** hang below the crown in tan, from the trunk's surface between
one and two of its radii below the apex down to a quarter of a frond's length, pointing down at
120°–145° from the axis and bending 40° further, their leaflets folded in (60% of the angle, 15°
down out of the plane) and sagging more.

**The UV layout.** One island a frond: *u* along the rachis and *v* across it, both in the frond's
own length, so the chart is the frond laid flat — rachis down the middle and the leaflets at their
angle either side, never overlapping each other — and every frond shares the same chart, so one
frond texture serves them all. The trunk is one strip and its cap a disc beside it.

## The build

A palm is derived data, built once per parameter set. `flora::build_flora_container` solves the
file's palm and builds its container: one cluster LOD DAG per primitive, **each with its material's
cone setting** — the trunk's cones on, a two-sided frond's off (`build_cluster_lod` takes the option
per call, so one mesh carries both and the trunk keeps its backface culling) — merged in primitive
order, laid out in streaming pages, with the three materials, the identity and **section 34**, the
generator section: `flora`, the species and the canonical parameters, which `engine-content info`
prints as `generator` ([geometry](geometry.md)). The key is `flora_build_key`:
`geometry::cluster_cache_key` over `flora_source_hash` — `hash_bytes` of the **canonical
parameters** (the schema's JSON of the file, defaults filled in, compact and in field order, so two
files that say the same thing are one key whatever their spacing, order or spelled-out defaults),
mixed with the species and `k_flora_version` — under the LOD options, no weld, and the page target.

`engine-content flora <params.flora.json> <out.clusters> | --cache [--ddc <dir>] [--force]` is the
command line over it ([apps](apps.md)): a named output, or the derived-data cache entry the key
addresses, skipped when it already records that source hash and key. It is the same function the
renderer reaches through the mesh-generator registry ([assets](assets.md#generated-meshes)), under
the same default LOD options and page target, so either finds what the other built.

## In a scene

A scene's mesh table names a flora file as it names a glTF — `{"name": "palm", "path":
"../../flora/date-palm.flora.json"}` — and its instances place it. The renderer's scene reader
cannot link this capability (ADR-0027), so it finds the solver through the **mesh-generator
registry** (`domain/assets/mesh_generator.h`), which `src/flora.cpp` registers "flora" into: a path
ending in `.flora.json` names it, and the reader builds the container through the derived-data cache
under the key `engine-content flora` uses, or reads the entry either of them wrote
([renderer](renderer.md), "A generated mesh in the mesh table"). A host carries the generator by
linking this capability. A build without it refuses the path with the registry's sentence, which
names `ENGINE_WITH_FLORA`.

`content/flora/` holds the default palm (`date-palm.flora.json`, every parameter spelled out) and
four variants — `-tall` (14 m), `-young` (3.5 m, a heavy skirt), `-leaning` (22°), `-old` (sparse,
drooping, no skirt). [`content/test-scenes/oasis-palms/`](../../content/test-scenes/oasis-palms/README.md)
stands twelve of them round a basin on a small dune field with a camera path that circles the grove
and walks in under the crowns: a scene to fly that needs no sample asset.

**What the first pictures taught.** The first oasis capture read as sticks: a leaflet 3 cm wide
tapering linearly to its tip is under a pixel from 25 m at 640 x 400, and a crown of them vanished
into its rachises. A date palm's pinna is linear — nearly its full width most of its length — so a
leaflet now keeps nearly its width to its last row (1 − s³ at first, straight edges since the
harness, below), and the default width went from 3.2 to 4 cm (never more than 0.9 of the gap
between two); the same frame went from 1,101 frond pixels to 3,812, and to 6,205 with the straight edges. Thin geometry still aliases at
a distance where a leaflet is a fraction of a pixel; alpha-tested cards (R65) are what fixes that
properly.

## What the harness taught

E10's judge (`tools/e10-harness.ps1`, [e10](../experiments/e10-generated-props.md)) takes GLBs, so
the palms were judged by the same measurements made by hand over the five files in
`content/flora/`: `engine-content flora` into a container, `engine-content stats` on it for the
atlas, and the container's coarse (1 px) and finest (0.05 px) cuts captured offscreen through
engine-host from one orbit camera at the harness's `-Offscreen` framing (640 x 640, the fit orbit,
shadows off, hardware raster), the visible pairs from `render.capture` and FLIP from `engine-image
compare`. msvc-debug, RTX 5090, 2026-10-17; none of it is a timing, so the machine's load does not
enter. The thresholds: 0 warnings, smallest island ≥ 1 texel of a 4096 atlas, coarse-against-finest
FLIP ≤ 0.02, coarse cut ≤ 0.75 of the finest's pairs.

| Palm | Triangles | Clusters / levels | Islands | Seam vertices | Smallest island (texels) | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|---|---|
| `date-palm` (default) | 52,704 | 1,656 / 12 | 38 | 1.6% | 913,029 | 0.196 (148 of 756) | **0.0194** | 27.4 / 0.964 |
| `-tall` | 58,464 | 1,780 / 12 | 38 | 2.3% | 899,972 | 0.126 | 0.0176 | 27.2 / 0.965 |
| `-old` | 27,088 | 769 / 10 | 18 | 4.3% | 883,021 | 0.208 | 0.0098 | 29.7 / 0.980 |
| `-leaning` | 49,464 | 1,548 / 12 | 36 | 1.6% | 913,029 | 0.227 | 0.0250 | 26.5 / 0.953 |
| `-young` | 40,168 | 1,283 / 11 | 32 | 1.0% | 913,029 | 0.231 | 0.0322 | 25.7 / 0.942 |

**The atlas and the collapse pass everywhere, because of the layout rule.** One island a frond and
one strip for the trunk is 38 islands for the default palm (26 fronds, 10 dead, the wall, the cap),
1.6% of its vertices on a seam (the trunk's seam column and the cap's ring), and the coarse cut
draws a fifth of the finest cut's pairs: the crown collapses. That rule was in the solver from the
first frond, because the alternative — one island per leaflet strip — puts every leaflet's border on
a seam the simplifier may not cross (geometry.md), which is the triangle remesh's failure E10's
second pass measured (coarse cuts 86–100% of the finest).

**The picture passes for the default palm and not for every palm, and the reason is thin
geometry.** The default, the tall and the old palm are under 0.02; the leaning and the young one are
not. The heat maps put the error on the leaflets and the dead fronds, not on a seam: a leaflet is
one to three pixels wide at this framing, the cluster LOD's 1 px threshold lets a coarse level move
an edge by a pixel, and a pixel is the whole of a leaflet — so a cut that moved no surface by more
than the threshold still changes how much of a crown is leaf. The whole-frame mean follows the
crown's share of the frame (the young palm is mostly crown). Two solver changes the measurements
chose, each worth a little on every palm: **straight-edged leaflets** — a long, slightly tapering
quad to the last row, then a point, so the rows between are collinear and the simplifier removes
them first without losing area (a curved 1 − s³ taper lost a quarter of every leaflet at the first
level; the default's coarse cut went from 180 pairs to 148 and its PSNR from 27.1 to 27.4 dB) — and
**trunk normals at half the scars' slope**, which took the ring bands off the coarse levels' trunks;
and the dead fronds open wider and hang less folded, which gave them area to lose gradually. What
would pass every palm is a coverage-preserving LOD for foliage — alpha-tested cards (R65), or a
threshold that knows a strip's width — not more tuning of the solver.

## Determinism

The same parameters give the same bytes on every toolchain and at either CPU baseline: the solver
computes in f64 with IEEE `+ - * /` and `sqrt` only, contraction is off tree-wide (ADR-0035), every
sine and cosine is `det::sin`/`det::cos` (`core/math/deterministic.h`), its randomness is a
splitmix64 over the seed with one stream per part (so a draw added to the crown never moves the
trunk), and it runs on one thread. The default palm's mesh is pinned by hash in
`tests/flora_tests.cpp` ("the committed bytes on every toolchain"); a change that moves it bumps
`flora::k_flora_version` and the number in the same commit.

**Owned data.** The species' parameter types and the solver. Nothing persistent: a palm is derived
data, rebuilt from its parameters.

**Invariants.** Each checked by `tests/flora_tests.cpp`:

- every parameter at either end of its range builds a valid mesh: finite positions, indices in
  range, no degenerate triangle, primitives covering the index buffer in order;
- the mesh holds its `triangle_budget`, down to everything at its most expensive on 2,000;
- the trunk's apex stands at `height_m` and its radii at the base and the apex are the ones asked,
  to 1%; every trunk face looks outwards (the column's signed volume is the taper's);
- each green and each dead frond is one connected surface (the components of the crown are its
  fronds), wound consistently with its normals; the crown stands above the apex and the skirt
  below it; no skirt is asked, no skirt primitive;
- a smaller budget walks down the ladder and holds, and keeps every frond;
- one seed is one palm, and another seed another;
- a parameter out of range is refused with its name;
- a `.flora.json` in a scene's mesh table becomes a mesh with the three materials through the
  cache — a hit the second time, the same clusters built in memory — and the oasis scene's first
  frame, captured offscreen, has its crowns' pixels green and its trunks' brown, the crowns above
  (`systems/renderer/tests/flora_scene_tests.cpp`);
- the default palm hashes to the committed number, and so does its container
  (`geometry::cluster_file_hash`, whose clusters are the other half of a palm's bytes); the
  trunk's clusters carry cones and no frond's does; the container records its identity and the
  generator section; the canonical parameters are the same for every spelling of one file.

**Public API.** `include/domain/flora/palm.h` (the solver: `palm_in_range`, `solve_palm`,
`PalmReport`), `include/domain/flora/flora.h` (the capability: `k_flora_version`,
`k_species_date_palm`, `read_flora_file`, `canonical_flora_json`, `flora_source_hash`,
`flora_build_key`, `build_flora_container`). The mesh generator "flora" registers from
`src/flora.cpp`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `assets`, `geometry`,
`flora_schemas`.

**Testing.** `tools/dev.ps1 test -Filter flora`.

**Performance notes.** No hot path: the solver runs once per parameter set at content-build time,
in milliseconds for the default palm. What a palm costs to draw is its cluster DAG's.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Component and event types | `domain/flora/schemas/flora.schema`: `Palm`, `FloraFile` | parameters, not components: a palm has no runtime state |
| Tick scheduler entry | none | the solver runs at content-build time and has no tick |
| Render-graph passes | none | a palm is a cluster mesh like any other |
| Content-build derived step | `engine-content flora`, the mesh generator "flora" | [the build](#the-build): keyed by the canonical parameters, the species and `k_flora_version` |
| Protocol methods | none | not needed |
| Tunables | none | everything the solver reads is a parameter of the file |
| LOD policy | the cluster DAG's | a palm is one mesh; distance is the DAG's |
| Determinism | the same bytes on every toolchain | [above](#determinism) |
| Zero cost when unused | nothing runs unless something asks for a palm | plan 11 §11.10: absent work |
| Tests and size table | `tests/flora_tests.cpp`, `tests/size_table.cpp` | written |
| Bench | none | no hot path |
| Removal proof | `ENGINE_WITH_FLORA`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_FLORA=OFF`) drops the
module, its schema types and its tests; the module disappears from `build/<preset>/modules.json`
and is listed there under `disabled_capabilities`. Everything else must still build and pass.
