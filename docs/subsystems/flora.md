# flora (domain, capability)

**Purpose.** The engine's tree solver: a species' parameters and a seed in, a mesh out, the same
bytes every time and on every toolchain. Vegetation in this engine comes from a solver and not from
modelled assets or image-to-3D, which is the owner's direction for it (plan 05 §5.15, "trees under
wind") and what E10 measured: a generated mesh loses its detail past a person's size
([e10](../experiments/e10-generated-props.md)), and a tree is several times that. The first species
is the **date palm** (`flora::solve_palm`, `engine.flora.Palm`), because Desert Survival's oases
need palms before anything else (plan 13 §13.1, "Oases"); after it come a **broadleaf** and a
**conifer** (`flora::solve_broadleaf`, `solve_conifer`; [the branching species](#the-branching-species)),
because the next game is a fantasy RPG that needs forests. Every species emits, beside its mesh,
the **motion contract** a sway stage will read ([motion attributes](#motion-attributes)). It does
not place what it makes (a scene's instances do, and a scatter layer will — roadmap G55), move it
in the wind (S66), or texture it yet (G60).

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
| `trunk_top_radius_m` | 0.03 m – base | 0.18 | the radius where the leaf sheaths begin; they swell it 40% by the apex |
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
   leaflets of one segment (a single triangle each), the collar's boots as wedges of three sides;
2. then the trunk's and the rachis's cross-sections — 12 segments round the trunk, a ribbon
   rachis instead of a keeled trough, two petiole rows, 8 segments, one row a ring;
3. then fewer, wider leaflets, halved repeatedly with their width doubled so a frond keeps its
   leaf area (a frond of half the leaflets twice as wide reads as the same frond from a distance);
4. then every other persistent base, down to eight; rings merged two by two; six segments round
   the trunk; and last the collar itself — the fronds' sheaths, then the persistent bases.

The fronds themselves are never dropped: the crown's outline is what a palm is recognized by, and
the motion contract's parts are the same on every rung. A budget that cannot hold even the bottom
rung is refused with the count it would need; inside the documented ranges that does not happen
(the test builds everything at its most expensive on 2,000: 1,370 triangles after 32 rungs). The
default palm is 55,312 triangles at full detail — trunk 11,920 (93 rings), collar 1,200 (36 sheaths
and 64 persistent bases), crown 30,472, skirt 11,720 — under its 60,000; on 40,000 it is 30,928
(one-segment leaflets), on 20,000 19,560 (and 12 segments, two rows a ring), and on 2,000, 1,516
(one leaflet a side, eight persistent bases, the rings merged in pairs).

### The trunk

A tapered generalized cylinder swept along a spine. The spine leans towards an azimuth drawn from
the seed, and its angle from vertical falls linearly with arc length, from `lean_deg` at the ground
to `(1 - curve)` of it at the apex: that has a closed form (the height after an arc *s* is
(sin *a* − sin(*a* − *bs*)) / *b*), so the arc length that puts the apex at exactly `height_m` is
solved for, not searched. The radius tapers linearly from `trunk_base_radius_m` at the ground to
`trunk_top_radius_m` where the leaf sheaths begin ([the crown](#the-crown-one-spiral-by-age)), and
above that the sheaths swell it smoothly to 1.4 times the top radius at the apex. The **leaf-scar
rings** are a radial profile along the spine: each ring is a leaf base's boot, the wall stepping in
sharply at the scar and swelling back out to the boot's lip, drawn with four rows a ring (two or one
when the budget asks). The lip is where the radius is the one asked for. The rings are
`ring_spacing_m` apart up to the collar and then **tighten** towards the apex, to 0.4 of the spacing
at the top, as a stem's internodes shorten where it is still growing — the scars run up into the
collar instead of stopping under it. Each ring turns a few degrees against the one below it, from
the seed, interpolated through the ring so the wall never shears — what makes the scars read as
grown rather than lathed. The apex is closed by a low dome, hidden inside the youngest fronds'
bases. One UV island: *u* once round with one seam column, *v* up the trunk at the same scale so a
bark texture is not stretched; the dome is a small disc of its own beside it.

### The crown: one spiral by age

**What a date palm's top is.** The owner's note on the first palms was that the tops of the trunks
looked wrong: they ended in a bare cone with the fronds stuck on it. A real *Phoenix dactylifera*
has no bare top. Its leaves come from the apex one at a time — a new one every few weeks — on a
spiral, each turned close to the golden angle (137.5°) from the last and a short internode below it,
so a leaf's place on the stem *is* its age; the leaf bases clasp the stem in fibrous sheaths, which
is why the crown's head is wider than the trunk under it; the youngest leaf (the spear) stands
nearly upright at the centre, still folded, and each older leaf leaves the stem further from
vertical and arches further, until the oldest green ones stand out flat and droop; the dead leaves
hang below them from the same spiral; and below those, the bases of the leaves that were cut or
fell persist for years as a dense collar of stubs before they weather off into the scar rings of the
bare trunk. (Written references on the species' morphology; no imagery was copied.)

**The solver's model**, every number chosen for that picture, the budget and the harness:

| Quantity | Value | Why |
|---|---|---|
| divergence between successive leaves | the golden angle, 137.5° | the palm's phyllotaxis; it fills the circle evenly at any count |
| internode (stem between two green leaves) | 2 top radii ÷ `frond_count`, at least 1.5 cm | the green fronds' bases span about two trunk radii (0.39 m on the default, at the 1.5 cm floor), the depth of a crown of sheaths; a model frond stands for several real leaves, so the spacing is a modelling unit |
| dead fronds' spacing | a quarter of a frond's length over the dead fronds, never less than the internode | the skirt hangs from a band below the green crown, as the first palms' did |
| a short palm | the whole spiral scaled into the top half of the stem | a 1 m palm keeps a trunk |
| leaf *k*'s base | its place on the spiral, at 0.55 of the stem's radius there (0.8 for a dead frond) | one spiral for green fronds (*k* < `frond_count`), dead fronds next, persistent bases after them |
| a green frond's angle from the axis | `frond_spread_deg` × √age, ±6% from the seed | leaf inclination rises fastest just after a leaf opens and then levels off; √ also fills the crown's cone evenly by area |
| its droop and length | `frond_droop` × 100° × (0.5 + 0.5 × age); `frond_length_m` × (0.8 + 0.2 × age), ±5% | the young are shorter and barely curve, the old arch and droop |
| the spear (the youngest 8%) | leaflets closed by up to a quarter of their angle and up to 12° more upright out of the rachis's plane | an unfolding leaf is a closing fan; kept slight because a closed leaflet is thin geometry the coarse cut loses |
| the sheaths' swelling | the stem's radius × (1 + 0.4 smoothstep) from the lowest dead frond's base to the apex | the head of clasping sheaths, wider than the trunk |
| a boot at every frond's base | the sheath the petiole leaves from, snug to the stem at the apex (20° from the axis) and opening with age to the persistent bases' 50° | the crown's head reads as leaf bases, not as a bare column |
| persistent bases | six top radii of the spiral below the dead fronds, at the internode, at most 64 (64 on the default) | the collar that turns into the bare trunk's scars |
| a base's shape | a tapered box 0.9 × 0.3 top radii across, 0.8 top radii out of the wall (±15%), 50° from the axis (±15%), cut square | a petiole's boot cut a hand's width from the stem, pointing up and out |

The collar is its own primitive in the trunk's material, built with cones (one-sided like the
wall), so the wall stays one closed column; each base is a tube whose open end is inside the trunk,
one UV chart every base shares beside the trunk's, and a small island for its cut face. Its vertices
belong to the trunk's motion part.

**The fronds.** A frond's **rachis** is a curve whose angle from the axis grows with the square of
its arc length — by its droop at the tip — so a frond rises, arches and droops; its cross-section is
a trough (two edges and a keel below, a ribbon when the budget asks), a petiole's width at the base
and millimetres at the tip. Below the first leaflet is a bare petiole (22% of the rachis). On each
side, `leaflet_count` **leaflets** stand on the rachis's own edge vertices — each a strip from two
base corners, out at `leaflet_angle_deg` towards the tip and 28° up out of the rachis's plane (a
date palm's fronds are V-shaped in section), tapering to a point and sagging a little; longest a
little past the middle of the frond and at most a ninth of its length.
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

`dead_skirt_fronds` **dead fronds** hang below the crown in tan, from their places on the spiral —
a band a quarter of a frond deep below the green fronds, each from its sheath — pointing down at
120°–145° from the axis and bending 40° further, their leaflets folded in (75% of the angle, 5°
down out of the plane) and sagging more.

**The UV layout.** One island a frond: *u* along the rachis and *v* across it, both in the frond's
own length, so the chart is the frond laid flat — rachis down the middle and the leaflets at their
angle either side, never overlapping each other — and every frond shares the same chart, so one
frond texture serves them all. The trunk is one strip and its cap a disc beside it.

## Motion attributes

Every species' solver emits, beside its mesh, what a deform stage needs to sway the tree through
the deformed-vertex pool — wind in the crown, a slight bend of the trunk, fronds pushed aside by a
walker — without knowing which species it is (`include/domain/flora/motion.h`, `flora::Motion`).
Nothing moves yet: the stage that reads these is the next piece of work (roadmap S66), and a tree
whose weights are all zero is exactly the rest pose the renderer draws today.

**Per part**, one `geometry::ClusterFileMotionPart` (32 bytes, mesh space). A **part** is what
bends as one: the trunk, each green frond, each dead frond, and in a branching species each branch.

| Field | What it is |
|---|---|
| `pivot` | where the part meets its parent; the trunk's is its foot |
| `direction` | the unit tangent the part leaves its pivot along, at rest |
| `length` | the part's arc length, root to tip, in metres |
| `parent` | the part it hangs from; `k_motion_no_parent` for the trunk |

**Per vertex**, one `geometry::ClusterFileMotionVertex` (4 bytes) per cluster vertex:

| Field | What it is |
|---|---|
| `part` | the vertex's part, `u16` |
| `weight` | its **bend weight**, a 16-bit unorm: 0 at the part's root rising to 1 at its tip, by arc length along the part's own axis |

**The rules a deform stage can rely on.** Ids are **dense**, the trunk is 0, and a parent's id is
below its children's, so one pass over the table in order meets every parent first. The ids are
**stable across the budget ladder**: no rung drops or merges a part (the palm keeps every frond on
every rung), so one parameter set names the same parts at every budget. A vertex off the part's
axis — a leaflet, a leaf card, the far side of the trunk — carries the weight of its **station**:
the point of the axis it was built from, so a leaflet moves rigidly with the piece of rachis it
stands on and a ring of the trunk with its spine point. The pivot and direction are the part's,
reached through the id rather than repeated per vertex — 32 bytes a part instead of 24 more a
vertex. The palm's parts are the trunk (0), the green fronds 1 to `frond_count` youngest first,
then the dead fronds; every frond's parent is the trunk.

**In the container** the table is **section 35** (`motion_parts`, resident) and the per-vertex
records **section 36** (`motion_vertices`, parallel to `Vertices` and paged with it, as `Skin` is)
— two kinds because the format's rule is one fixed-size element per section ([geometry](geometry.md)).
`build_flora_container` maps each cluster vertex to the solver's vertex it was built from through
`vertex_source` (`motion_records`). Both are written only by a generator that emits them, so every
imported mesh's container is the bytes it was; the default palm's container with the two cleared
hashes to exactly what it did before they existed, and its other sections are byte for byte the
same (the test compares them section by section).

## The branching species

After the palm, two species a forest is made of (roadmap G61): `engine.flora.Broadleaf`, a
deciduous tree, and `engine.flora.Conifer`, a pine or spruce, each a struct of the flora schema
with its ranges (`broadleaf_in_range`, `conifer_in_range` refuse a field out of range by name), a
solver file of its own (`src/broadleaf.cpp`, `src/conifer.cpp`, over the shared shapes in
`src/kit.h`) and the same `.flora.json` dispatch: the file's `species` — `"broadleaf"` or
`"conifer"` — picks the member the solver reads. Both make two primitives, **bark** (closed tubes,
one-sided, cones on) and **leaves** (two-sided card sprays, cones off), and the motion contract.

**Why self-similar branching and not space colonisation.** Space colonisation grows a tree towards
attraction points, which makes beautiful crowns, but its branch count is whatever the simulation
ends with: the triangle count is known only after growing, a budget can only be met by growing again
or pruning afterwards, and a pruned branch moves every motion id after it. Seeded recursive
branching knows its whole structure from the parameters — `limbs`, `branches`, `twigs` — so the
solver counts the triangles of a plan *before* it builds, exactly as the palm does, walks the same
kind of ladder, and never drops a part. It is also the cheaper thing to keep deterministic: a fixed
number of draws per shoot from a stream per order, where colonisation's nearest-point searches
order their work by floating-point distances.

**The structure is grown once, at a fixed resolution, whatever the budget.** Every shoot's axis is
grown at 16 rows (the trunk at 32), its children placed along that axis, and a mesh is the axis
resampled at the plan's rows: so a lower budget changes how finely a shoot is drawn, never where it
is, and the motion parts' pivots, directions and lengths are the same bits on every rung (the test
holds them).

**The shapes** (`kit.h`). A shoot's axis grows from its bud along a direction that **turns towards
the light** (`upturn`, a share of 40° over its length, negative for a weeping habit) — a rotation
about the axis the two directions span, slowing as it aligns, so it never overshoots — and
**wanders** by up to `wander_deg` a step; its frame is parallel-transported, so a tube never
twists. A **tube** is a tapered closed cylinder along it, capped by a cone, open at the base inside
its parent, its UVs in metres (round the circumference and along the axis) so a tiled bark set lies
on it unstretched. A **spray** is leaves or needle bundles on a twig: the twig's centre line split
into equal steps, two cards on each step (one either side, `leaf_angle_deg` towards the tip, lifted
15° out of the spray's level plane), consecutive cards sharing their centre-line vertices — so a
spray is **one connected surface** the LOD builder collapses along the twig (the palm's lesson) —
and every card's UV the **full card** (card *i* spans *u* in [*i*, *i* + 1], one side *v* in [0, 1]
and the other [0, −1]), so a one-leaf image sampled with repeat lies on every card whole.

### The broadleaf

An oak by default (12 m, `content/flora/oak.flora.json`). The **trunk** leans by `lean_deg` towards
a seeded azimuth and straightens towards the vertical; its radius is `trunk_radius_m` and every
other radius follows by the **pipe model** — a shoot's base radius is the trunk's times (its
terminal twigs ÷ all of them)^(1/2.5), da Vinci's rule that a parent's cross-section is about the
sum of its children's — tapering along each shoot. `limbs` **scaffold limbs** leave the trunk
between `crown_base` and nine tenths of it on the golden angle, at `limb_angle_deg` (the upper ones
a third more upright), `limb_length` of the height long (the upper ones down to half). Each limb
carries `branches` **branches** from a quarter of the way out, alternating round it on the golden
angle at `branch_angle_deg`, `branch_length` of their limb long (shorter towards its tip); each
branch carries `twigs` **twigs** from a fifth of the way out, and the leader's top carries `twigs`
more. A twig is a spray of `leaves` cards a side, `leaf_length_m` long, its plane laid level so the
cards face the sky. Parts: the trunk, every limb, every branch, every twig (its parent its branch,
or the trunk for the leader's tuft) — 1 + 8 + 48 + 441 = 498 on the default.

**The budget** (default 60,000; 8,000 to 2,000,000). The default is 27,356 triangles at full detail
— bark 6,188 (trunk 12 sides × 30 rows, limbs 8 × 14, branches 5 × 7), leaves 21,168 (441 twigs of
12 cards a side) — and the ladder, rung by rung: branch rows 4, limb rows 8; sides 4/6/10, then
3/5/8; the trunk's rows halved; then the cards halved repeatedly (a twig of half the cards keeps
its leaf area, each card twice as wide); then rows 2/4/3, sides 4/6, and limb rows 2 and branch rows
1 last. On 12,000 the default is 7,516 (3 cards a side); everything at its most expensive (12 limbs,
8 branches, 10 twigs, 40 leaves) holds 8,000 with 7,740.

### The conifer

A spruce by default (16 m; `spruce-tall.flora.json` is 22 m, `spruce-young.flora.json` 4 m). The
**trunk** is straight (no wander, a lean taken back towards the vertical), tapering from
`trunk_radius_m` to a centimetre at the leader's tip. `whorls` **whorls** stand from `crown_base` to
the top, each of `branches_per_whorl` branches spaced evenly round the trunk from a seeded turn —
fewer up the crown by `thinning`, so the top tiers are sparse — at an angle from vertical running
from `bottom_angle_deg` at the lowest (past 90°: hanging below level) to `top_angle_deg` at the
top, `crown_radius_m` long at the bottom shrinking to a tenth at the top (as (1 − *t*)^0.9 of the
crown's height, a cone with full shoulders), each tip turning back up towards the light by
`upturn`. A branch is a woody tube and a **spray of needle-bundle cards** from a sixth of the way
out to its tip, `needle_cards` a side on the longest branch and fewer in proportion to length,
`needle_length_m` long, splayed 60° forward and 15° up; the leader carries a tuft. Parts: the trunk
(which owns the tuft) and every branch, the branch's tube and spray one part — 136 on the default.

**The budget** (default 40,000; 8,000 to 2,000,000). The default is 12,882 triangles at full detail
— bark 5,510, needles 7,372 — and the ladder: branch rows 3, then 2; trunk sides 8; branch sides 3;
the trunk's rows halved; the cards halved repeatedly; branch rows 1; trunk sides 6; and last the
branches' woody tubes, their sprays then reaching to the trunk. On 8,000 it is 6,109 (12 cards a
side); everything at its most expensive (60 whorls of 10, 60 cards, 60 m) holds 8,000 with 2,654.

### What the harness said

The same measurements as the palms' (below), over the three recipes, 2026-10-18:

| Tree | Triangles | Clusters / levels | Islands | Seam vertices | Smallest island (texels) | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|---|---|
| `oak` | 27,356 | 848 / 11 | 498 | 5.1% | 5,475,665 | 0.567 (204 of 360) | **0.0065** | 36.6 / 0.993 |
| `spruce-tall` | 13,058 | 410 / 9 | 272 | 14.5% | 295,087 | 0.465 (79 of 170) | **0.0039** | 38.2 / 0.994 |
| `spruce-young` | 3,442 | 112 / 8 | 72 | 14.0% | 69,530 | 0.739 (34 of 46) | **0.0009** | 46.0 / 0.999 |

All three pass every threshold, the young spruce's collapse narrowly: at the harness's framing a
4 m tree of 3,442 triangles is a few dozen clusters, and a card spray's coarsest level is a strip
of a few triangles it keeps. The picture error is a third of the palms' and less, because a card is
a surface with area to lose gradually where a leaflet is a sliver a pixel wide. Islands: one a
spray and one a tube, which is the layout rule again.

**What they look like** (`content/test-scenes/grove/`, offscreen captures, not committed): trees,
at once — limbs and twigs with leaves on them, whorled spruces with drooping lower tiers — and
stylized, because a card is an opaque parallelogram until alpha-tested cards (R65) give it a leaf's
outline: the spruce's sprays read as flat green strips close up, the oak's leaves as tiles. The
shapes are right for the cards an image will be cut into.

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
and walks in under the crowns: a scene to fly that needs no sample asset. Beside them,
`oak.flora.json` (a broadleaf), `spruce-tall.flora.json` and `spruce-young.flora.json` (conifers);
[`content/test-scenes/grove/`](../../content/test-scenes/grove/README.md) stands four oaks, seven
spruces and a palm on a nearly flat ground with a camera path round and into them.

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

**The crown on its spiral (`k_flora_version` 2, 2026-10-18)**, the same measurements by the same
procedure (a scratch script outside the repository; msvc-debug, RTX 5090, captures under the GPU
lock, none of it a timing):

| Palm | Triangles | Clusters / levels | Islands | Seam vertices | Smallest island (texels) | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|---|---|
| `date-palm` (default) | 55,312 | 1,748 / 11 | 238 | 4.1% | 34,200 | 0.240 (192 of 799) | 0.0213 | 27.2 / 0.960 |
| `-tall` | 51,984 | 1,545 / 11 | 238 | 5.7% | 35,195 | 0.143 | **0.0176** | 27.0 / 0.964 |
| `-old` | 28,624 | 822 / 10 | 146 | 7.6% | 33,338 | 0.255 | **0.0106** | 29.4 / 0.978 |
| `-leaning` | 52,048 | 1,637 / 11 | 232 | 4.2% | 34,240 | 0.235 | 0.0283 | 26.0 / 0.948 |
| `-young` | 42,700 | 1,378 / 11 | 198 | 3.8% | 39,797 | 0.375 | 0.0385 | 24.9 / 0.924 |

The atlas and the collapse still pass everywhere: the collar's boots are two islands each (a tube
and its cut face), which is where the islands and the seam share went, and its smallest island is
still tens of thousands of texels. **The picture moved the wrong way by about a tenth**: the
default palm from 0.0194 to 0.0213, now just over the threshold with the leaning and the young palm
that were over it already; the tall and the old palm pass. The heat map puts the error where it
was — leaflets and dead fronds — and none on the collar (its boots are irreducible, so the coarse
cut draws them exactly). Bisected with a switch per change over the default palm: the boots, the
sheaths' swelling and the tightening rings each moved FLIP by less than the run-to-run spread
(±0.002 from the fit orbit following the bounding sphere); the fronds' bases on the spiral instead
of at the apex cost about 0.002, because fronds springing from a 0.4 m band overlap their
neighbours' leaflets more than fronds from one point; a deeper crown (3 top radii) and a stronger
spear (leaflets closed by half, 40° up) cost another 0.002 each, which is why the crown is two radii
deep and the spear slight. What the spiral costs is thin geometry overlapping, which is R65's
problem and not the solver's: the conclusion above stands.

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

## Not yet

- **Textures** (G60): the materials are flat PBR constants, and the recipe's `materials` block
  (`engine.flora.FloraMaterials`: a tiled set per part — bark, leaf, skirt, wood — with `tile_m`)
  is in the schema and the key but not yet read by the solvers. The branching species' UVs are
  already what a set needs (tubes in metres, cards whole); the palm's trunk is still one
  circumference-scaled strip. The engine half that is left: lay each set's images into the
  container's materials (section 32 through the derived-data cache), the palm's trunk UVs in
  metres, and a stand-in set a test writes; the sets themselves are the material lab's.
- **Wind and sway** (S66): the deform stage that reads the motion contract — plan 05 §5.15's rods,
  the wind field and the sway tiers, a slight trunk bend, fronds and branches pushed aside by a
  walker. The contract is emitted and stored; nothing reads it.
- **Alpha-tested cards** (R65): a card is an opaque parallelogram until a leaf image's alpha cuts
  it, which is also the coverage-preserving foliage LOD the palms' thin leaflets want
  ([above](#what-the-harness-taught)). The cards' UVs already give each card the whole image.
- **The interior for environment destruction**: a trunk cut by destruction needs a cross-section
  material and a cap — the `wood` slot of the materials block is recorded for it and drawn by
  nothing — and the cut itself needs the tube's rings, which the solver has and does not keep.
- **Placement** (G55): a scatter layer naming flora files, so an oasis or a forest is a rule and
  not a list.
- **More species**: the kit's axis, tube and spray make a new branching species a structure
  function and a ladder (a birch's weeping twigs, a pine's tufts at the branch tips, a fan palm);
  bark that is not rings or tubes (plates, fissures) wants displacement or the height map of a set.
- **The palm's solver on the kit**: `palm.cpp` keeps its own copies of the f64 vector, the stream
  and the emitter that `kit.h` now holds for the branching species; moving it over is mechanical
  and must leave its pinned bytes alone.

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
- the trunk's apex stands at `height_m`, its radii at the base and where the sheaths begin are
  the ones asked to 1%, and the apex is 1.4 times the top radius; every trunk face looks outwards
  (the column's signed volume is the taper's);
- the crown is one spiral by age: the green fronds' bases one internode apart down the stem, the
  dead fronds' below them, the youngest frond within a quarter of the spread of vertical and the
  oldest past 85% of it, younger steeper; the collar a boot per frond plus the persistent bases,
  below the sheaths, out of the stem;
- each green and each dead frond is one connected surface (the components of the crown are its
  fronds), wound consistently with its normals; the crown stands above the apex and the skirt
  below it; no skirt is asked, no skirt primitive;
- a smaller budget walks down the ladder and holds, and keeps every frond;
- one seed is one palm, and another seed another;
- the broadleaf and the conifer: valid, in budget at their defaults with no rung walked and on
  every smaller budget down to 8,000 (everything at its most expensive included), two primitives
  (bark one-sided, leaves two-sided), each spray one connected surface, the motion parts the same
  bits on every rung, one seed one tree and another seed another, both pinned by hash; a file's
  species picks the solver and its canonical form carries that species alone;
- the motion contract: a record per vertex, part ids dense with the trunk 0 and every parent
  before its children, every part reaching weight 0 at its root and 1 at its tip, and its weights
  an arc-length parameter (the centroids of its vertices binned by weight lie along it at the
  share their weight says, to 0.12); the same parts on every rung of the ladder; the mesh the same
  bytes whether the contract is asked for or not;
- the contract in the container: every cluster vertex's record is its source vertex's, the table
  and the stream round-trip through a file, the two sections are appended after the others, and
  with them cleared the container is byte for byte the one built before they existed;
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
`PalmReport`), `include/domain/flora/trees.h` (the branching species: `broadleaf_in_range`, `conifer_in_range`, `solve_broadleaf`, `solve_conifer`, `TreeReport`), `include/domain/flora/motion.h` (the motion contract: `Motion`, `MotionPart`,
`k_trunk_part`, `encode_bend_weight`, `decode_bend_weight`, `motion_records`), `include/domain/flora/flora.h` (the capability: `k_flora_version`,
`k_species_date_palm`, `k_species_broadleaf`, `k_species_conifer`, `solve_flora`, `read_flora_file`, `canonical_flora_json`, `flora_source_hash`,
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
| Component and event types | `domain/flora/schemas/flora.schema`: `Palm`, `Broadleaf`, `Conifer`, `MaterialSet`, `FloraMaterials`, `FloraFile` | parameters, not components: a palm has no runtime state |
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
