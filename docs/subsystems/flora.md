# flora (domain, capability)

## Shared growth rules and named species (G85)

The named profiles `maple`, `oak`, `elm`, `walnut`, `beech`, `ash`, `pine`, `spruce`, `cedar` and
`juniper` use a common `tree` parameter block. The older `date_palm`, `broadleaf`, `conifer`,
`birch`, `willow`, `orchard` and `ancient` names and parameter blocks remain valid. The first six
new profiles share the pipe-model broadleaf crown; pine and spruce share whorled shoots; cedar
and juniper use the same evergreen scaffold with staggered branch stations and scale sprays.
`src/species.cpp` owns the profile table, not ten independent mesh generators.

```json
{"species":"maple","tree":{"seed":17,"height_m":18}}
```

`Tree.height_m` and `trunk_radius_m` default to 0 (the profile's reference height and its
height/diameter allometry); `crown_base` defaults to -1 (the profile's), and `crown_scale` to 1.
Height is 3–40 m when explicit, radius 0.05–1.5 m, crown base 0.1–0.8 and crown scale 0.5–1.5.
The resolved family parameters must also pass their range checks: a combination whose derived
radius or crown exceeds that family's limits is refused with the field named, not silently
clamped. Explicit triangle budgets are 8,000–2,000,000. Omission or zero selects the species'
source budget from the shared geometry catalogue (G86, foliage ceilings revised in G87): maple
200,000; oak and ash 400,000; elm and walnut 1,000,000; beech 900,000; pine and spruce 60,000;
cedar 80,000; juniper 30,000.
These provisional, owner-tunable budgets retain individual leaves on the reference trees;
a million or more is an explicit allowance for a particularly large tree. The runtime cluster cut and baked chain
choose the rendered detail. An explicitly small source budget can still reach the existing coarse,
merged-card fallback; it is a quality trade, not a new botanical leaf size. `card_bake_px` and `impostor_px` have the broadleaf
fields' meanings; evergreen profiles still have no baked chain (G83). Canonical JSON and cache
identity include only the active `tree` block, species name and materials.

Maple and ash grow opposite, decussate lateral pairs; elm's ascending scaffolds carry spreading,
drooping fine growth; beech carries a low layered crown. Walnut's terminal sprays represent a
compound rachis with eight leaflet pairs, ash's three pairs and a terminal leaflet; their cards are leaflets, not enlarged
simple leaves. Pine concentrates foliage towards erect branch ends in a broader crown. Cedar
means western redcedar (*Thuja plicata*), with fluted bases and flat sprays; juniper means eastern
redcedar (*Juniperus virginiana*), with denser irregular scale foliage. These are representative
habits, not every species within each genus. Their source observations and the chosen, unmeasured
visual parameters are separated in [shared growth rules](../experiments/tree-growth-rules.md).

The [G86 profile audit](../experiments/tree-profile-audit.md) gives every profile's current values,
source comparisons and remaining unmeasured controls. Species names, default profile budgets and
motion compliance live in `geometry/flora_species.h`; `species.cpp` refers to IDs. The renderer
rejects unknown motion species, and compile-time checks reject missing profile or motion coverage.
Legacy families retain their schema budgets. The catalogue stays available with flora disabled.

Named profiles use their own green wood strength/density for a first-order scaffold self-weight
limit, independent of triangle budget. It caps broadleaf scaffold and evergreen primary branch
length before fine shoots grow; it does not replace the ancient's recursive load model or account
for snow, wind, dwelling loads or total recursive crown mass. LAI belongs to each broadleaf profile
(currently the retained 4.5 target); evergreen coverage uses cohorts and sprays. Pine and spruce
use 0.45 m whole-shoot cards; these are not individual needles.

G86 raises willow's default sprays from 210 to 784 (7 limbs, 7 branches, 16 curtains), with a
30-degree spray lift. The existing ladder keeps its 60,000 source budget; explicit branch and
curtain counts stay explicit. Leaf length is unchanged; a narrow willow atlas remains a material
refinement. Ancient named segment joins now start inside their parent's end and smoothly recover
their centre and radius. This removes exposed flared rims without changing structural axes,
platform anchors, triangle counts or the wide canopy. It is an overlapping join, not a watertight
union. Solver cache version 8 invalidates previous builds, including the following maple correction.

Maple selects the shared `opposite_simple` leaf placement rule. Unmerged crowns retain all woody
orders through the terminal twig, with individual rectangular leaf quads whose image carries the stalk.
Each pair shares a node; successive pairs turn around the twig, with seeded station, blade-size
and orientation variation. The complete image maps once to each rectangle, bottom-centre stalk
at the attachment, preserving the maple lobes instead of squeezing them along a connected strip.
The 0.10 m card and 0.08 m width are unchanged. Node spacing limits leaf pairs
to the available twig length, so the realised LAI can be below the 4.5 target. The report records
that realised area. All terminal wood is counted before choosing the source budget;
an insufficient budget places real-sized blades on retained higher-order shoots. The default
200,000 budget is unchanged. G87 extends independent leaves to oak, elm and beech with alternate
nodes, birch when its source budget holds the full foliage, willow's hanging shoots, and the
spreading/mixed ancient crowns. Ash and walnut carry separate green rachises with independent
leaflets; their opaque rachis primitive is material 2, between the bark and leaf primitives.
`leaf_triangles` includes rachises, while `leaf_cards` and leaf area count the blades alone.
Willow's curtain stems share the existing motion parts and remain non-colliding; their rows are
reduced before leaves and omitted only on the smallest budgets. Geometry and material expectations,
source ranges and authored variation are recorded in the [profile audit](../experiments/tree-profile-audit.md#maple-terminal-shoot-correction).

All branching tree families now use the kit's supported wood surface: an asymmetric branch
collar fading over the base, root flare, low-frequency fluting, gravity-oriented oval branch
sections and seeded local swellings. The ring topology and triangle count are unchanged; the
surface and its derivative normals are sampled at the selected detail. The core axis, part IDs,
weights and structural radii are unchanged by the relief. This is overlapping supported geometry,
not a watertight union or a mechanical failure solver. Fine fissures and scars still need materials.
Straight structural parts bypass the relief to preserve the exact platform-anchor envelope.

The natural ancient habit defaults retain their limb counts, bands and fork levels. Explicit limb
counts now accept 2–24 and the highest limb up to 0.85 of height, allowing a denser authored fantasy
crown without changing every natural tree's silhouette.
All habits reserve shorter straight platform runs by default, then fork and
curve; an explicitly requested `limb_run_m` remains a straight run. Renewed shoots occupy short
automatic runs and more of the inner limbs; on an explicitly reserved run the upright stays at
the trunk-side crotch, leaving the walking span clear. Their wood-equivalent load is included in the mechanical
cap. Leaves retain their size. A previously authored 20 m straight run remains that request;
use a shorter run in a revised dwelling recipe and recheck its platform positions. Solver version
6 invalidates old cached geometry. Revalidate ancient platform anchors even when retaining an
old recipe: the additional crown load can shorten an explicitly requested run, and the renewed
branches change the structural table. The renderer assigns new names their growth family's wind
stiffness, so they retain motion rather than falling through to an unknown species.

Tests in `growth_rules_tests.cpp` cover all ten profiles, repeatability, distinct geometry, valid
motion, budget-invariant skeletons, canonical round trips, invalid input, wood relief and exact
straight sections. Existing tests continue to cover the pipe rule, LAI, all three ancient habits,
village recipes, mechanical caps, hollows and structural anchors.

**Purpose.** The engine's tree solver: a species' parameters and a seed in, a mesh out, the same
bytes every time and on every toolchain. Vegetation in this engine comes from a solver and not from
modelled assets or image-to-3D, which is the owner's direction for it (plan 05 §5.15, "trees under
wind") and what E10 measured: a generated mesh loses its detail past a person's size
([e10](../experiments/e10-generated-props.md)), and a tree is several times that. The first species
is the **date palm** (`flora::solve_palm`, `engine.flora.Palm`), because Desert Survival's oases
need palms before anything else (plan 13 §13.1, "Oases"); after it come a **broadleaf** and a
**conifer** (`flora::solve_broadleaf`, `solve_conifer`; [the branching species](#the-branching-species)),
because the next game is a fantasy RPG that needs forests; then a **birch**, a weeping **willow**
and an **orchard tree** for the forest's variety, and the **megaflora** — an ancient tree 40 to
150 m tall whose trunk and major limbs are metres thick, for elven villages built in and between
trees ([megaflora](#megaflora)). Every species emits, beside its mesh, the **motion contract** a
sway stage will read ([motion attributes](#motion-attributes)), and the second batch the
**structural table** a structure solver anchors to ([the structural table](#the-structural-table)). It does
not place what it makes (a scene's instances do, and a scatter layer will — roadmap G55) or move it
in the wind (S66). Its parts sample tiled PBR sets — a recipe's own, or a material pack's default
for the part's role — through [materials](materials.md) ([below](#materials)).

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

**Every species' ladder changes a tree's detail and never its structure** (the owner, 2026-10-20):
graphics presets pick rungs and gameplay must not change with them, so on every rung the parts,
their pivots, directions, lengths and parents, the load-bearing flags and the structural table are
the same bytes, and a rung drops only rows, sides, cards and folded wood ([the budget of a
crown](#the-budget-of-a-crown)).

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
bases. One UV island **in metres**: *u* once round at the mean radius with one seam column, *v* the
arc up the trunk, so a tiled bark set repeats every `tile_m` both ways and is not stretched; the
dome is a disc of its own at its own size, a hand's width below the wall's island
([materials](#materials)).

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
The renderer's flora stage reads them since 2026-10-19 ([Motion](#motion), below); a tree whose
weights are all zero is exactly the rest pose.

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

## Motion

**Trees move** (2026-10-19, roadmap S66): wind sways the crowns with a slight bend of the trunk,
and a walker who walks into a palm's crown bends its fronds aside instead of meeting a wall. Nothing
in this module changed for it — the contract above is what the renderer reads — and nothing in the
renderer names a species beyond a table of numbers: the stage is the deformed-vertex pool's
([renderer](renderer.md#the-flora-stage)), the model is `systems/renderer/flora_motion.h`, and the
collision rule is [scene_collision](scene_collision.md)'s.

**What a frame does with the contract.** Each part bends about its `pivot` by one rotation, the sum
of the wind's and the walker's; a vertex takes its part's rotation scaled by its bend weight, so the
root stays where its parent put it and the tip travels; and a child rides its parent — its frame is
the parent's frame times the parent's own bend at the weight where the child leaves it, the child's
pivot projected on the parent's axis. The CPU composes the frames parent-first each frame (the ids'
parent-first order is what makes that one pass), so the shader reads one pose per vertex whatever the
depth. Because the part, the weight and the rest position are per-vertex data every copy of a
surface point shares on every rung and every LOD level, a bent tree cannot crack.

**The wind.** Where it blows to and its strength over its record's mean come from the scene's
ground's wind record at the **sky's hour** (the hour's direction and strength: the weather follows the day; [renderer](renderer.md#one-world-clock-and-the-days-length)), from
`renderer.flora.wind_deg` and `renderer.flora.wind_strength` (default 1, a mean day, towards +x)
where the ground says none, or from the frame (`FrameDesc::flora`). A part's steady lean is
`compliance × s × (0.6 + 0.8 g)` and its flutter `compliance × s × (0.25 + 0.35 g) × sin(2π f t +
φ)`: `s` the strength held to [0, 2], `g` the gust (two sines of 7.7 s and 3.2 s with phases from the
instance's seed, in [0, 1]), `f` 0.9 Hz over the square root of the part's length — a longer part
sways slower, and since its tip travels the angle times its length, farther — and `φ` the part's
own phase from its pivot in the mesh's frame and the seed, so no two fronds and no two trees swing in
step and no world position is involved. Its axis turns the part's rest direction towards where the
wind blows (a part lying along the wind flutters up and down instead). **`t` is the world clock**
(`FrameDesc::world_time_s`, since 2026-10-20, roadmap R88): the gust and the flutter run at real
speed whatever the day's length, faster or slower only with the world rate (engine-view's `,` and
`.`), and stand still at a world rate of 0. It is the number the frame index at 60 Hz gave before,
for a host at sixty frames at real time, so a replay sways as it did.

**Stiffness, per species** — radians of steady lean at a mean wind, by class of part: the trunk
(part 0), a limb (a child of the trunk; a palm's fronds), a twig (deeper), a leaf (a part the
structural table gives no wood: a spray of cards, a willow's curtain). A part the structural table
flags **load-bearing** takes a quarter of its class's (`k_flora_load_bearing`), which is what keeps
the ancient tree's limbs, which a village hangs from, nearly still.

| Species | Trunk | Limb | Twig | Leaf |
|---|---|---|---|---|
| `date_palm` | 0.012 | 0.10 | 0.12 | 0.14 |
| `broadleaf` | 0.006 | 0.03 | 0.07 | 0.12 |
| `conifer` | 0.008 | 0.035 | 0.06 | 0.08 |
| `birch` | 0.015 | 0.05 | 0.09 | 0.14 |
| `willow` | 0.006 | 0.03 | 0.08 | 0.16 |
| `orchard` | 0.006 | 0.025 | 0.06 | 0.10 |
| `ancient` | 0.0015 | 0.008 | 0.04 | 0.10 |
| `shrub` | 0.01 | 0.07 | 0.10 | 0.14 |
| `fern` | 0.01 | 0.18 | 0.20 | 0.22 |
| `tuft` | 0.01 | 0.20 | 0.22 | 0.26 |
| `herb` | 0.01 | 0.16 | 0.18 | 0.20 |

At a mean wind a 6 m palm trunk's top leans 6.6 cm and its 3 m fronds' tips move 0.24 m downwind on
average over ten seconds (the renderer's test), the flutter swinging them either side of that; at
the strength the sway saturates at (2), twice that. They are chosen by eye against how a palm, an
oak and a willow move in a breeze, not measured, and the table is the one place to change them. The
understory is the most compliant (G75): its part 0 is a root with no geometry, a shrub's stems are
its limbs and sway like a tree's twigs, and a fern's fronds, a tuft's blades and a herb's leaves and
flowers have no wood and take the leaf class — a tuft's blade leans 0.26 rad at a mean wind, the
most of any part; a herb's stem is its limb. A part flagged `k_structural_collider` (a shrub's
stout stem) is never pushed.

**The push.** A walker's capsule (its feet, 0.3 m radius, 1.8 m tall: [apps](apps.md#walking)) is
handed to the frame. A part that is neither the trunk nor load-bearing whose rest axis comes within
the capsule's radius plus the part's own reach — its wood's radius, or an eighth of its length for a
part with none (a frond's leaflets stand out from its rachis) — plus 5 cm of the capsule's axis is
turned about its pivot, away from the walker, by the angle that carries its nearest point clear, at
most 0.9 rad; the push eases in and out over 12 frame ticks (0.2 s), a smoothstep of a ramp held per
part between frames. A part's children go with it. The walker's own collision leaves those parts out
([scene_collision](scene_collision.md)), so it walks into the crown and the fronds bend away, and the
trunk still stops it.

**Determinism.** A pose is a function of the parts, the frame index (at 60 Hz), the instance's seed,
the wind and the walker's capsule, and of the push's state, which the frames before left as a
function of the same: a replay of a recorded walk draws every pose it drew (the renderer's test
replays a walk through a palm's crown to the same bytes).

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
branching knows its whole structure from the parameters — the named orders' counts and, for the
broadleaf and the ancient tree, the [allometric recursion](#allometry)'s sizes and ratios — so the
solver counts the triangles of a plan *before* it builds, exactly as the palm does, walks the same
kind of ladder, and never drops a part. (Self-organising growth is roadmap G74, proposed; what it
would add and cost is in [tree allometry](../experiments/tree-allometry.md#g74-what-self-organising-growth-would-add).) It is also the cheaper thing to keep deterministic: a fixed
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

### Allometry

**A bigger tree is more tree, not a scaled-up one** (roadmap G73, 2026-10-20; the sources, the
constants and the measurements are [tree allometry](../experiments/tree-allometry.md)). The
broadleaf's and the ancient tree's crowns (and the shrub's and, since G81, the [birch's](#the-birch)) are one recursion (`kit.h`, "the allometric crown") from
the shoots of their named orders — the broadleaf's limbs and its trunk's top, the ancient tree's
limbs past their runs and its trunk's top — down to a **terminal twig of the species' own size**,
and the leaves on it are **one leaf a card, at the species' size**, on a 3 m tree and a 150 m one.
Until G73 the ancient tree was the broadleaf scaled up, six fixed orders ending in 5 m twigs of
1.4 m cards, which the owner's first walk under it read as giant leaves.

| Rule | Species constant, or what scales | Where it comes from |
|---|---|---|
| A node's children share its cross-section: R^Δ = Σ r^Δ, the leader `leader_share` of it and the laterals the rest equally | `area_exponent` Δ (2; measured trees 1.8–2.3) and `leader_share`, the species'; the radii scale | the pipe model, da Vinci's rule (Shinozaki 1964, Eloy 2011) |
| A shoot of the recursion bears `branching_ratio` − 1 laterals along its distal part and a leader at its tip; the named roots bear `branches` laterals | `branching_ratio` (4) the species' | Horton–Strahler's bifurcation ratio, 3 to 5 |
| A child is `length_ratio` of its parent's length; a root's laterals `branch_length` of its length | `length_ratio` (0.63) the species' | WBE's n^(−1/3) |
| A shoot whose thinnest child would be thinner than `twig_radius_m` is a **terminal twig**, `twig_length_m` long | the twig's radius and length are the species', never the tree's | Corner's rules; WBE's invariant terminal unit |
| So the twigs number about (R / r_twig)^Δ and the orders about 1 + log_Rb of that | both scale with size: the oak's 3,384 twigs over 8 orders, the ancient tree's 214,834 over 12 (its wide crown before the habits, G79) | follows |
| The card count is `leaf_area_index` × the crown's footprint ÷ one card's area (`leaf_width` × `leaf_length_m`²), spread over the twigs as evenly as integers go, each at most `leaves` a side | LAI and the leaf the species'; the footprint (the convex hull of every shoot tip from above) scales | the leaf area index (Asner et al. 2003) |
| The broadleaf's `trunk_radius_m` of 0 is 0.35 m × (H / 12 m)^(1 / `allometry_exponent`) | the exponent the species' (2/3) | McMahon's elastic similarity, H ∝ D^(2/3) |
| The crown's radius is the limbs' reach, a share of the height for the broadleaf | scales as H ∝ D^(2/3) | crown ∝ D^0.6–0.8 |
| The lowest limbs shed, apical control weak in an old crown | `crown_base` / `limb_low_m` and `leader_share`, the recipe's | Hallé, Oldeman and Tomlinson 1978; deriving them from age is G74 |

**The structure never depends on the budget.** Every shoot is grown once, from a splitmix64 stream of
its own (its index in the depth-first order), so a draw added to one shoot moves no other. Shoots of
the **part class** and up (the finest radius class whose shoots, with every coarser one, number at
most 400 for the broadleaf and 2,000 for the ancient tree) are motion parts with grown axes, eight
rows at the structure's resolution; every finer shoot — the twigs among them — is **folded** into
the part it grows from: a straight segment that takes half its turn towards the light at once,
riding that part at the station where its branch left it (its vertices carry that part and that
one bend weight). A part is a tube on every rung; a folded shoot is a tube of one row on the rungs
whose `wood_class` reaches its class, and nothing on the rest; its leaves are always there. The old
fields still read: `twigs` (and the ancient tree's `sub_branches`) cap a shoot's laterals, `leaves`
caps a twig's cards a side, and `branches` is the limbs' own laterals.

#### The budget of a crown

The ladder may change a tree's detail but **never its structure**, because graphics presets pick
rungs and gameplay must not change with them (the owner, 2026-10-20): on every rung the same
shoots with the same axes, radii, parents and pivots, the same part ids, the same load-bearing
flags, the same structural table byte for byte, and the same motion parts; a rung drops tube rows
and sides, leaf cards, and the finer orders' wood folded into its part, nothing else
(`allometry_tests.cpp` holds the part list and the table identical on every rung of the default
broadleaf and ancient tree). The order, rung by rung: the finest wood first (a centimetre's shoot is
under a pixel past a few metres), then rows and cross-sections, then the finer orders' wood folded
one radius class at a time up to the part class, then the trunk's, and only where the budget
forces it **the cards merged up the hierarchy, finest first**: at merge level *m* a twig's cards
gather on its ancestor *m* orders up (never past a part) into one spray of about the twigs' cards a
side whose cards reach as far as that sub-tree does — a **branch-group card** — then those sprays'
cards halved, then the last rows. On the default ancient tree the merge starts at the ninth rung, on
any budget under about 1.15 million triangles, and a merge-1 card is 1.22 m; on the oak at the third,
under about 38,000, a 1.04 m card. A merged card drawn opaque is a parallelogram again; alpha-masked
cluster textures (R65) baked from the solver's own finest geometry (R89) are what make it read as
foliage ([tree allometry](../experiments/tree-allometry.md#what-the-aaa-pipelines-do-with-a-million-leaves)).

### The impostor chain

**G87 (2026-10-24):** branch-group representations now contain three orthogonal masked quads
in the fitted frame, sharing one subtree element and transition. Sixteen representative subtrees
need 48 tiles (a 7 × 7 atlas at the default); when every group has a representative, it samples its
own tiles. The far tree keeps four vertical viewing directions but partitions each into four
depth slices: at most 16 planes, each baking only its slice's whole triangles. The default far
atlas is 1024 × 1024. Coverage compares each entire three-plane group, and the whole source tree
against all far slices. These replace the single-plane groups and unsliced star described in the
historical R89/R91 measurements below. They preserve volume across viewing angles without a
runtime camera-facing billboard or a new shader; high overhead views remain an approximation.

The build host now supplies encoded images through `MeshGeneratorHosts::read_image`. The CPU
bake samples actual base-colour RGB and leaf alpha, with sRGB decoding, bilinear repeat sampling,
material factors, bark UV scale and the material's alpha cutoff. Its coverage measurement uses
the same sampler. A red maple therefore stays red through the baked levels. The canonical flora
source hash additionally covers the bytes of both consumed base-colour images; same-path edits
invalidate the tree. The renderer's generator host and `engine-content flora` share this path.
Direct pure calls without an image host retain solver colours and the ellipse fallback. Normal
and roughness textures are not baked; the existing geometric normals and leaf roughness remain
the approximation. Solver cache version **9** invalidates the previous geometry and atlases.

The geometric foliage changes and explicit source-budget compromises are recorded in
[G87's audit](../experiments/tree-profile-audit.md#terminal-foliage-and-distance-follow-up).

Roadmap R89 (2026-10-21; the grounding and the switch distances are in [tree
allometry](../experiments/tree-allometry.md#what-the-aaa-pipelines-do-with-a-million-leaves)). The
budget ladder above picks **one** rung for a whole tree; the chain gives the same tree's coarser
forms to the cluster DAG as **authored levels** above its finest leaves
([geometry](geometry.md#authored-levels)), so a far tree draws them and a near one its leaves, with no
preset chosen. The broadleaf, the ancient tree and (since G81) the birch build it (`card_bake_px`, default 64, and
`impostor_px`, default 256; 0 turns either off); every other species is the container it was.

| Level | What it is | Its error |
|---|---|---|
| finest | the plan's leaves, decimated as before, cut where the first card level's error starts | the simplifier's |
| branch-group *k* (*k* = 1, 2, …) | **one card a sub-tree**: every spray's leaves gather on their root *k* orders up (never past a part — the ladder's merge rule), and the sub-tree becomes one two-sided masked quad in the plane its leaves lie nearest (the least-variance plane of their vertices, a fixed-sweep Jacobi in double) the size of their extent in it, riding the root's part | the farthest any leaf vertex of any sub-tree lies from its card — the parallax a card cannot show — or half a baked texel on the largest card, whichever is larger |
| the tree | a **star of planes**: four vertical through the trunk's axis 45° apart, each two-sided, each the tree's extent in it (R91 dropped the level plane R89 had at the crown's mid-height: from near that height it was edge-on, a thin dark line across the tree, and hiding it by the view's angle would make the coverage a function of the view on every path; from high above the four read as a cross, which is R90's octahedral frames' case); the bark hands over to it (its DAG's last level is an empty one at the same error) | the farthest any vertex lies from any of the four planes — about the crown's radius |

The levels stop where a step merges nothing more (every root a part) or at six. **The error formula**:
a card shows its geometry exactly from its baked view; seen at an angle θ off it, a point *δ* from the
plane is drawn *δ* sin θ from where it is, so over every view the error is *δ*max — the level's
`parallax` — and a level is chosen at *d* = *e* · `proj_scale` / threshold + *r* from its sphere's
centre (*r* the sphere the chain's transitions share). That is honest and it is large: the 12 m oak's
first card level switches in at about 580 m at 1080p and its star at about 7.8 km; the elder grove's
in [tree allometry](../experiments/tree-allometry.md#what-the-aaa-pipelines-do-with-a-million-leaves).
R91 brought them in: each card group switches on its own sub-tree's sphere, and by its coverage error under its own threshold (below), so the oak's first cards come in about 28 m from a group and the star at about 420 m.

**The bake** (`bake_level`, `src/impostor.cpp`): an orthographic view per tile from the plane's front,
rasterized on the CPU — vertices snapped to 1/256 of a texel, edge functions in 64-bit integers with
the top-left rule, 4 × 4 samples a texel, nearest-first by depth along the view — then resolved to an
RGBA8 colour (sRGB through `texture::linear_to_srgb8`, alpha the samples covered) and an RGBA8
**tangent-space normal** (the faces' normals turned to the viewer, in the tile's u, v and normal
axes). It shades with the species' own colours, not a pack's, because the solver has no io to read
an image; where a pack cuts the near leaves to an outline (a masked card material) the bake cuts its
leaves to the ellipse the card holds. A branch-group level bakes **16 representative sub-trees**
(spread evenly over the groups' order) into a 4 × 4 atlas of 64-texel tiles, and every card samples
one of them by its root's hash — tens of thousands of sub-trees each baked would be hundreds of
megabytes, and a sub-tree's picture is statistically its neighbours'. The tree bakes its five planes
into a 2 × 2 atlas of 256-texel tiles (512 × 512) of the bark and the finest leaves. Each atlas pair
is embedded in the container as PNG (sections 21 and 22) and drawn by a material of its own after the
species' — white, alpha-tested at one half, two-sided, the colour as base colour and the normal as a
normal map — so the texture step builds them like any embedded image (BC7 and BC5 with mips), and
the renderer draws them with R65's masked variants and its ordinary tangent frames, with no new
shader. **Section 38** (`ClusterFileImpostor`, 48 bytes a level) records what each level is, its
material, atlas and error; `engine-content info` prints it as `impostors`.

**The plane fit is by area** (R91): each triangle counts by its area with its exact second moment, where R89 averaged the vertices — a spray's cards share their centre-line vertices, so the twig's line outweighed the leaves either side of it and a sub-tree of a few sprays could take the plane through its twigs edge-on to the leaves and bake as a sliver.

**Every card switches on its own sub-tree** (R91; [geometry](geometry.md#an-authored-hierarchy-one-sphere-a-sub-tree)). The solver records how the chain nests — every finest leaf triangle's first-level card (`TreeLevels::leaf_element`), every card's card one level up (`TreeLevel::parent_of_element`; a sub-tree's root k + 1 orders up is its k-th root's one order up, so all of a card's sprays agree), and the last level's cards in the star — and the leaves' part is built by `build_cluster_lod_hierarchy`: a group of up to a cluster's worth of cards (16 quads) switches on the sphere of exactly the sub-trees it stands for, so the far side of a crown is cards while the near side is leaves. The bark is decimated as before and hands over to the star, the one transition the parts share. With the chain, the finest leaves are alpha-tested even without a pack's image (a mask of the factor alone keeps every pixel), so the cross-fade into the first cards is a screen door on both sides.

**The coverage bake** (`measure_level`, R91). A card is judged by what it changes at the pixel, not by the parallax of a point off its plane, so each level also carries a **coverage error**: its `feature_m` — what it stands in for, the square root of an element's area: a leaf for the first branch-group level, a card of the level below for the next, the last level's card for the star — times its **difference** D, measured at build time. The bake's own fixed-point rasterizer draws each representative sub-tree's finest geometry (shaded flat, cut to the leaf where the bake cuts) and the card that stands for it (its tile of the level's atlas, masked at one half, nearest texel) from a ring of sixteen orthographic views — eight azimuths at the horizon, four 30° above and four below — at four texels to the feature (16 to 96 a side, 2 × 2 samples a texel), and pools, texel by texel, the coverage difference Σ|a − b| / Σ max(a, b) (a soft one minus the overlap) and the mean colour difference where both draw; D = 1 − (1 − coverage)(1 − colour). The star compares the whole tree with its four planes. The errors are kept rising level by level. Section 38's last two words carry the coverage error and D; section 39 carries each cluster's own and parent coverage (geometry.md, "Two errors"). Measured (`engine-content info`, 2026-10-21):

| | Oak (12 m): coverage error, D | Ancient tree (100 m): coverage error, D |
|---|---|---|
| first cards | 0.15 m, 0.90 | 0.20 m, 0.89 |
| second | 0.92 m, 0.86 | 1.01 m, 0.84 |
| third | 1.54 m, 0.83 | 1.63 m, 0.79 |
| fourth | 2.25 m, 0.77 | 2.32 m, 0.75 |
| fifth | — | 3.11 m, 0.69 |
| the star | 2.25 m (raised to the level below's), 0.28 | 3.12 m (raised), 0.45 |

A card differs from its leaves by eight or nine tenths at four texels a leaf — one plane for a three-dimensional spray, a tile shared with fifteen other sub-trees, edge-on from some of the ring — which is what the threshold is fitted against ([renderer](renderer.md#the-foliage-threshold)). The star differs least because at four texels to a top-level card its planes and the crown are both blobs.

**What it holds**: the solver's mesh is its bytes with the chain asked for (the cards are appended
after everything, their triangles kept apart); the atlases and the coverage bake are the same bytes every time and on
every toolchain (`impostor_tests.cpp` pins the 8 m oak's card and tree atlases); every card names a parent and every leaf a card; the star is chosen 1 % past its switch distance and not 1 % inside it, by the geometric error alone and by the coverage error under a foliage threshold, with nothing else drawn past it; some distance draws the crown's near leaves and far cards at once; sections 38 and 39 round-trip. Drawn (`systems/renderer/tests/impostor_view_tests.cpp`), the 8 m oak's star from its +z plane's baked view shares 63% of its silhouette with the finest leaves' and is within 12.5 of 255 in colour where both draw — bounds of one half and 50 held by the test ([tree allometry](../experiments/tree-allometry.md#what-r89-built-and-where-it-switches-2026-10-21) has the costs). **Not built**: a true octahedral
atlas (the brief's 8 × 8 views blended by direction) needs coverage that depends on the view, and
R65's mask is a function of the UV alone on every path — the rasterizers, the rays and the shadow
maps — which a static star of planes satisfies and a view-chosen octahedral frame does not; and the
dithered cross-fade at a switch, which needs both levels drawn across a band (roadmap rows in
[tree allometry](../experiments/tree-allometry.md#what-the-aaa-pipelines-do-with-a-million-leaves)).

### The broadleaf

An oak by default (12 m, `content/flora/oak.flora.json`). The **trunk** leans by `lean_deg` towards
a seeded azimuth and straightens towards the vertical; its radius is `trunk_radius_m`, or the
allometric radius of the height when that is 0 (0.35 m at 12 m, the default). `limbs` **scaffold
limbs** leave the trunk between `crown_base` and nine tenths of it on the golden angle, at
`limb_angle_deg` (the upper ones a third more upright), `limb_length` of the height long (the upper
ones down to half); by the pipe rule the trunk's top keeps `leader_share` of its cross-section and
the limbs share the rest. From every limb (`branches` laterals from a quarter of the way out, each
`branch_length` of the limb) and from the trunk's top the [allometric recursion](#allometry) grows to
the 4 mm, 0.7 m twig; a leafy twig is a spray of its cards a side, `leaf_length_m` long and
`leaf_width` of that wide, at the twig's tip, its plane laid level so the cards face the sky. On the
default: 3,384 twigs over 8 orders, 4,501 shoots, 16,638 cards over a 120 m² footprint — a realised
LAI of 3.9 (3.67 measured over the leaves' own hull) — and **230 parts**: the trunk, the 8 limbs
and the 221 recursion shoots of the part class (3) and up. The broadleaf emits no structural table.

**The budget** (default 60,000; 8,000 to 2,000,000). The default is 49,157 triangles at full detail
(bark 15,881: the trunk 12 sides × 30 rows, limbs 8 × 14, the recursion's parts by the plan's edge
and row steps and its shoots of class 1 and up a row each; leaves 33,276) with no rung walked. The
ladder ([above](#the-budget-of-a-crown)): class 2's wood folded; limb rows 8 and the parts' rows
halved; the cross-sections; the folded classes one by one to the part class; the trunk's rows
halved; then the merges and the halvings; then the last rows. On 40,000 it is 38,396 (merge 0); on
25,000, 13,576 (merge 1, cards to 1.04 m); on 8,000, 6,364 (merge 2, 2.2 m); everything at its most
expensive (12 limbs, 8 branches, 10 twigs, 40 leaves) holds 8,000.

### The conifer

A spruce by default (16 m; `spruce-tall.flora.json` is 22 m, `spruce-young.flora.json` 4 m). The
**trunk** is straight (no wander, a lean taken back towards the vertical), tapering from
`trunk_radius_m` to a centimetre at the leader's tip. `whorls` **whorls** stand from `crown_base` to
the top, each of `branches_per_whorl` branches spaced evenly round the trunk from a seeded turn —
fewer up the crown by `thinning`, so the top tiers are sparse — at an angle from vertical running
from `bottom_angle_deg` at the lowest (past 90°: hanging below level) to `top_angle_deg` at the
top, `crown_radius_m` long at the bottom shrinking to a tenth at the top (as (1 − *t*)^0.9 of the
crown's height, a cone with full shoulders), each tip turning back up towards the light by
`upturn`. Between two whorls stand `interwhorl` shorter branches (0.45 ± 0.15 of the whorl length
there, a seeded azimuth each). The model is a Norway spruce's shoots (G80; the sources and the
numbers are [conifer shoots](../experiments/conifer-shoots.md)):

- **A branch is thin and needled on its outer years.** Its base radius is Colin and Houllier's
  largest branch of a whorl (15.9 mm + 0.978 mm per cm of breast-height diameter, halved) times its
  length over the crown's reach to the 0.8 — 3.2 cm at the default's crown foot. Its needles are on
  its outer `needle_years` × `shoot_growth_m` (6 years of 0.2 m: 1.2 m), so a long branch's inner part
  is bare wood, as a real spruce's is.
- **Second-order laterals**, `laterals_per_m` a metre (10) alternating sides along the branch's outer
  two needled reaches, each as long as its age allows (`lateral_length_m`, 0.7 m, a needled reach in
  from the tip; 0.3 of it at the tip; never past 0.4 of the branch), leave at 55° in the branch's
  level plane and **hang by `pendulous`**: dipped by 0.35 × `pendulous` and turned towards the ground
  by 3 × `pendulous` radians over their length. Their spray's plane holds the branch's direction, so
  a hanging lateral is a curtain (a comb spruce, 1) and a level one a plate (a plate spruce, 0); the
  default 0.7 is between brush and comb.
- **Cards stand for foliated shoots**, not needle bundles: a spray on the branch's own needled part
  (`needle_cards` a side on the longest, every other shoot at the same spacing, `needle_length_m`
  0.45 m long, splayed 55° forward) and one on every lateral (cards 0.8 as long, 50°, drooping with
  the habit). A real branch carries 20–40 laterals a metre three or four fronds deep; the solver
  builds 10 and makes each lateral's cards wide enough to reach over the next one's, which is the
  budget's trade. Every card lifts out of its spray's plane by 10° at the crown's foot and 35° at its
  top — flat shade shoots, bushy sun shoots — so no crown is all edge-on to an eye at its height. The
  leader carries a tuft.
- **The seed leaves branches out**, on streams of their own: `missing_branches` (0.05) of the branch
  slots are empty, half of those leaving a broken stub a twelfth of the branch long;
  `sparse_whorls` (0.08) of the whorls lose each branch with even odds; and the lowest `dead_share`
  (0.12) of the crown's whorls are **dead** — bare wood four fifths as thick, broken at 35–90 % of
  their length, no needles (a spruce keeps its dead branches; a closed stand's recipe raises it).
  The fates are one draw a whorl and four a slot, whatever they decide, and a slot's laterals have a
  stream of their own, so a missing branch moves nothing else: the whorls draw exactly what they drew
  before G80.

Parts: the trunk (which owns the tuft) and every branch, dead or alive, the branch's tube and all
its sprays one part, a lateral's vertices weighted by their arc from the branch's base — 179 on the
default.

**What a pack's needle image should be**: one foliated shoot per image, its axis along *v*, about
25–35 % opaque and irregular, every needle two texels wide at 512 (conifer shoots, "What a pack's
needle image should be"). A periodic image defeats the build's coverage-preserving mips, which is
half of why the first forest's spruces read as bare branches.

**The budget** (default 40,000; 8,000 to 2,000,000). The default is 23,742 triangles at full detail
— bark 7,058, needles 16,684 — where before G80 it was 12,882 (5,510 and 7,372); the forest's three
recipes are 26,000 to 29,200 on their 40,000 ([the table](../experiments/conifer-shoots.md#the-budget)).
The ladder: branch rows 3, then 2; trunk sides 8; branch sides 3; the trunk's rows halved; the cards
halved repeatedly, on every spray; then the laterals **folded into their branch's own spray**, its
cards as long as the longest lateral (the merge the allometric crowns make, a detail rung, not a
structural one: no part moves); branch rows 1; trunk sides 6; and last the live branches' woody
tubes, their sprays then reaching to the trunk. A dead branch keeps a tube on every rung (one row of
three sides at the last), since nothing else stands for it. On 20,000 the default is 19,874 after
four rungs; on 8,000 it is 3,514 (laterals folded); everything at its most expensive (60 whorls of
10, six interwhorl branches, 20 laterals a metre of 2.5 m, 60 cards, 60 m) holds 8,000 with 4,066.

### The birch

G87 adds a full-detail path with visible terminal wood and independent, alternate 5 cm leaf
blades, each sampling the full leaf image. It first attempts the complete foliage and wood within
the requested source budget. If they do not fit, it retains the established aggregate-card ladder
below: a low-budget source mesh must not become bare just because physical leaves cost more.
Close inspection should use a source budget large enough for the full path; distance reduction
then uses the baked hierarchy. Aggregate cards are an explicit source approximation, not larger
botanical leaves. `card_length`, triangle counts and realised area expose the chosen tradeoff.

`engine.flora.Birch` (`src/birch.cpp`; version 2 since G81, 2026-10-22, grounded in
[the birch](../experiments/birch.md); recipes `birch.flora.json`, 14 m and two stems,
`birch-single.flora.json`, 18 m and one, and `birch-clump.flora.json`, a seeded clump). A silver
birch (Betula pendula): white stems, a narrow ovoid crown, and from its limbs the
[allometric recursion](#allometry) down to **slender pendulous branchlets** carrying small leaves
by the leaf area index. Version 1 was three named orders — 312 twigs of 9 cm cards on the default,
a realised LAI well under 1 — which the owner's forest read as a few limbs and strings of leaves.

**The stems.** `stems` names 1 to 5, or **0 lets the seed choose** (the default): from a splitmix64
stream of its own, a clump with `clump_chance` (0.3) and then one more stem at even odds at each
count up to `max_stems` (5) — half the clumps two stems, a quarter three. Because the count has its
own stream, **a seeded birch of n stems is byte for byte the birch with `stems = n`**, and
`birch_stems` says which a seed gives. A single stem stands at the origin, nearly upright. A
clump's stems are sprouts from one stump's root collar: their feet on a ring nine tenths of the
stoutest stem's radius out at the ground, each leaning outwards by about `stem_spread_deg` (the
stoutest half that) and straightening towards the light, the stoutest the tallest, the others 0.6
to 0.95 of its radius and shorter by elastic similarity (H ∝ D^(2/3)). Every stem is a
**load-bearing** part: the stoutest part 0, the others hanging from it at their feet.

**The crown.** `limbs` scaffold limbs on the stoutest stem (a clump's others in proportion to
their radius) leave it between `crown_base` and nine tenths of its height on the golden angle,
`limb_angle_deg` from the stem (the upper ones a half more upright), their lengths `limb_length` of
the stem's height along an ellipse widest two fifths of the way up the crown — the ovoid — their
ends arching down by a quarter of the droop. By the pipe rule the stem's top keeps `leader_share`
(0.35: the leader runs to the top) and the limbs share the rest. From every limb (`branches`
laterals) and every stem's top the recursion grows to the 1.5 mm, 0.6 m branchlet, and **the
branchlets hang**: a shoot of radius class `droop_class` or finer leaves its parent turned towards
the ground by `droop` × 80° (its parents' class half that, never past straight down; `CrownSpec`'s
`droop`, which no other species sets, so their bytes are as they were) — the default's terminal
twigs point 40° below level on average, and above level with no droop. The leaves are cards of the
species' own leaf (5 cm, `leaf_width` 0.7), as many as `leaf_area_index` (4.5) over the crown's
footprint asks, end to end along each side of a branchlet from its tip, lifted 30° out of the
spray's plane so a hanging branchlet's cards are not edge-on from half the azimuths. The default
reaches 4.3 m from its foot at 14 m. The bark is white (`bark`, cones on), its tubes' UVs in metres.
Parts: every stem and limb and the recursion's shoots of the part class and up (at most 160) — 42
on the default; the birch emits the structural table (stems load-bearing, the rest not) and builds
the [impostor chain](#the-impostor-chain) like the broadleaf (`card_bake_px`, `impostor_px`).

**The budget** (default 50,000; 8,000 to 2,000,000). A full crown is more leaves than any forest
budget holds — the default at full detail is 69,498 cards of 6.0 cm, 152,945 triangles — so **the
cards stand for more leaves rather than the crown losing them**: the leaves fill what the wood
leaves of the budget, two triangles a card, each card grown to keep the leaf area. While the cards
would have to be larger than twice the finest the branchlets hold, the wood gives way first (the
broadleaf's rungs: the finest wood folded, rows, cross-sections, the folded orders up to the part
class, the coarsest tubes); then cards up to four times the finest; only past that the broadleaf's
merge into branch-group cards, the halvings and the last rows. No rung moves a shoot or drops a
part, and every rung short of the merge keeps the LAI. The default is 49,997 (bark 13,949: no rung
walked; 18,024 cards of 11.7 cm, LAI 4.23 over 38.4 m²); on the forest's 36,000, 35,997 (bark
1,141, five rungs, 17,428 cards of 11.9 cm); a clump of two to five stems on 36,000 has cards of
13.8 to 15.7 cm and never merges; everything at its most expensive (five 30 m stems of 0.5 m, 24
limbs of 8 laterals, branching ratio 6, 1 mm branchlets, 30 cards) holds 8,000 with 6,941, merged.
The `birch` and `birch-single` rows of the harness table below are version 1's.

### The willow

`engine.flora.Willow` (`src/willow.cpp`; `willow.flora.json`, 11 m). A **short thick trunk** —
`trunk_height` of the tree, keeping three quarters of its girth to where it divides, load-bearing —
and `limbs` limbs from its top quarter, `limb_angle_deg` from vertical, **arching out and up** (a
turn towards the light), no longer than reaches the crown's top. `branches` branches along every
limb's outer half, out at 45°, straight for their first third and then **arching over** towards
the ground. From every branch hang `curtains` **curtains**: each leaves its branch out and 30° down
and then hangs — its axis turns towards the ground at 1.4 radians a metre times the sine between
it and plumb, so it turns fastest across the fall and slows as it hangs, the catenary-like droop of
a long thin twig — `curtain_length_m` long (±30%), **cut where it would reach the ground** (15 cm
above it, the axis truncated at its last row above; its steps stay even). A curtain is a spray of
`leaves` narrow cards a side, close along the twig (`leaf_angle_deg`, 22°), its plane facing out.
**Every curtain is its own part**, hanging from its branch, so a wind stage swings each on its own;
its structural row has no wood (radii zero). Parts: the trunk, the limbs, the branches, the
curtains — 253 on the default (210 curtains).

**The budget** (default 60,000; 12,000 to 2,000,000). The default is 28,441 at full detail (bark
4,921, leaves 23,520: 210 curtains of 28 cards a side) and the ladder walks the limbs' and branches'
rows, the cross-sections, the trunk's rows, then the cards — **the segment count of a curtain is
the ladder's**: a curtain of half the cards keeps its length, its cards twice as far apart — and the
last rows. On 12,000 the default is 7,530 (7 cards a side). The floor is 12,000 because a curtain
is never dropped: everything at its most expensive (12 limbs of 10 branches of 16 curtains, 1,920
of them) holds 12,000 with 9,954.

### The orchard tree

`engine.flora.Orchard` (`src/orchard.cpp`; `orchard-pruned.flora.json`, 4.5 m, and
`orchard-apple.flora.json`, 5.5 m, free, with fruit). An apple-like tree: a **short trunk**
(`trunk_height` of the tree), `limbs` **scaffold limbs** from its top evenly round it at
`limb_angle_deg` from vertical — an open vase — `branches` branches on each from three tenths of
the way out at 40°, and `twigs` short leafy spurs on each, every spur a spray of `leaves` cards a
side. **Pruned** (`pruned`, the default), the limbs run straight and stop under the crown's top and
every branch turns level, out from the trunk, so the crown is a **flat-topped vase** — the default is
8.4 m across and 4.2 m tall; **free**, the limbs and branches turn towards the light into a rounder,
taller crown (7.4 m across, 4.8 m tall from the same seed). `fruit` fruits on every spur (0 to 6) at
even stations along it, each an octahedron of `fruit_radius_m` hanging 2 cm under the spur, in a
**third primitive**, `fruit` (one-sided, cones on, the colour `fruit_red`/`fruit_green`/
`fruit_blue`), in its spur's part with its station's weight.

**The budget** (default 40,000; 8,000 to 2,000,000) and the broadleaf's ladder. The default is
11,954 at full detail (bark 2,954, leaves 9,000: 225 spurs of 10 cards a side); on 8,000, 5,686 (5
cards). **The fruit is never coarsened**, 8 triangles a fruit on every rung: two a spur add 3,600 to
the default, and a tree whose fruit a budget cannot hold is refused with the count (everything at
its most expensive with 6 fruits a spur needs 40,714); without fruit, everything at its most
expensive (8 limbs of 8 branches of 12 spurs, 30 cards) holds 8,000 with 5,536.

### What the harness said

**The `oak`, `ancient`, `ancient-rising` and `ancient-1m` rows below are the trees from before the
allometric crown (G73, 2026-10-20)**: their recipes now make different meshes (the oak 50,340
triangles at merge 0, the two elders 1,419,335 and 1,050,182, `ancient-1m` 454,886 on its first
merged rung), whose numbers are in [tree allometry](../experiments/tree-allometry.md), and since the
habits (G79, 2026-10-22) different again (the elders 606,450 and 386,241, `ancient-1m` 435,153 with
no rung walked; [megaflora habits](../experiments/megaflora-habits.md#what-the-recipes-come-to));
the rows are kept as what those trees measured. **The two spruce rows are from before the conifer's
shoots (G80, 2026-10-22)**: `spruce-tall` is now 23,450 triangles and `spruce-young` 4,062
([conifer shoots](../experiments/conifer-shoots.md#the-budget)); the harness has not been run on them
since.

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

**The second batch** (2026-10-19), the same measurements by the same procedure — `engine-content
flora`, `engine-content stats`, the coarse (1 px) and finest (0.05 px) cuts through engine-host at
640 x 640 from the fit orbit, FLIP from `engine-image compare`; a scratch script outside the
repository, msvc-debug, RTX 5090, captures under the GPU lock, none of it a timing. The thresholds
as above; the collapse is judged where the finest cut has at least 3 pairs per 1,000 object pixels,
which every row here has (4.2 to 142):

| Tree | Triangles | Clusters / levels | Islands | Seam vertices | Smallest island (texels) | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|---|---|
| `birch` | 18,796 | 575 / 10 | 338 | 4.6% | 2,323,280 | 0.532 (132 of 248) | **0.0056** | 36.5 / 0.993 |
| `birch-single` | 10,760 | 331 / 9 | 195 | 4.5% | 5,490,459 | 0.424 (61 of 144) | **0.0047** | 35.1 / 0.992 |
| `willow` | 28,441 | 806 / 10 | 253 | 4.1% | 7,255,769 | 0.561 (207 of 369) | **0.0125** | 31.1 / 0.983 |
| `orchard-pruned` | 11,954 | 385 / 10 | 256 | 5.9% | 1,857,452 | 0.597 (95 of 159) | **0.0044** | 37.5 / 0.995 |
| `orchard-apple` | 15,554 | 521 / 10 | 481 | 4.6% | 2,255,718 | 0.653 (147 of 225) | **0.0026** | 39.9 / 0.998 |
| `ancient` (300,000) | 196,301 | 5,631 / 13 | 2,179 | 4.3% | 36,909,816 | 0.313 (689 of 2,199) | **0.0100** | 33.9 / 0.987 |
| `ancient-rising` (300,000) | 176,912 | 5,081 / 13 | 1,961 | 4.3% | 24,434,176 | 0.296 (590 of 1,995) | **0.0074** | 35.2 / 0.991 |
| `ancient-1m` (1,000,000) | 945,961 | 26,081 / 16 | 5,379 | 1.2% | 27,234,368 | 0.160 (1,892 of 11,859) | **0.0063** | 38.5 / 0.993 |

Every recipe passes every threshold at its default budget. **What failed on the way**: the first
`orchard-apple` (two fruits a spur, 8 cards) drew 0.771 of its finest cut's pairs coarse — over the
0.75 — because a fruit is a closed octahedron of its own, a component the simplifier cannot
collapse into anything, so its clusters are on every level, as the palm's collar boots are; the
recipe now has one fruit a spur and 12 cards and draws 0.653. The willow's FLIP is the batch's
highest because its curtains are long thin strips seen edge-on from the orbit, the palms' lesson
again (a strip's coarse level moves its edge by a pixel, and a pixel is the strip).

**The megaflora's rungs** — the default ancient tree's parameters on smaller budgets:

| Budget | Triangles | Rungs walked | Clusters / levels | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|
| 300,000 | 196,301 | 0 | 5,631 / 13 | 0.313 | **0.0100** | 33.9 / 0.987 |
| 100,000 | 76,703 | 9 (8 cards) | 2,541 / 12 | 0.482 | **0.0095** | 33.7 / 0.988 |
| 30,000 | 26,303 | 12 (1 card) | 974 / 10 | 0.830 (fails) | **0.0018** | 42.2 / 0.998 |
| 12,000 | 11,448 | 14, the bottom | 592 / 9 | 1.000 (fails) | **0.0000** | identical |

The two coarsest rungs fail the collapse and pass the picture, and the reason is that they *are*
the far form already: a twig of one card a side is four triangles whose every collapse moves a
0.9–1.4 m card by more than this framing's pixel (a 100 m tree in 640 pixels is 0.2 m a pixel), so
at 1 px the DAG's coarse cut is its finest. The DAG still coarsens them at a distance, where its
error is under the threshold; the harness's question — does a dense mesh collapse at this framing
— is not one a mesh at its ladder's floor can answer yes to. Use the rungs for a forest's far trees
and the default or `ancient-1m` for the trees the camera walks under.

## Megaflora

`Ancient` version 5 adds `primary_length_scale` and `secondary_length_scale` (0.5–3, default 1).
They scale the first curved scaffold after a run and its first fork daughters independently.
The straight platform run, later fork lengths, renewal shoots, fine lateral lengths and leaf
dimensions do not inherit these multipliers. The enlarged wood and its relocated crown loads
enter the existing strength calculation; if overloaded, its common length cap still shortens the
whole limb system, including finer branches. `AncientLimb::asked_m` includes the expansion.
The village comparison uses 2 and 1.5, authored fantasy proportions rather than botanical
constants. Defaults preserve existing natural-tree proportions.

`engine.flora.Ancient` (`src/ancient.cpp`; `ancient-emergent.flora.json`, `ancient-spreading.flora.json`
and `ancient-mixed.flora.json`, one of each habit; `ancient.flora.json` and `ancient-rising.flora.json`,
the elder grove's two; `ancient-1m.flora.json`, 110 m on a million triangles): a tree large enough to
build a village in — the fantasy RPG's elven
villages span trees, with platforms, bridges, ramps and huts in and between them (roadmap G63) —
so its trunk is 4 to 25 m across, it stands 40 to 150 m, and its major limbs are 1 to 5 m thick.

**Why a species and not a scale regime on the broadleaf.** A `scale` field on `Broadleaf` would
have had to leave every byte of the oak where it is — the broadleaf's pin — while changing what its
trunk is (buttressed, hollowed, straight), what its limbs are (a straight run and then a crown limb),
and how many orders of branching it has (five, not three). Every one of those is a branch in the
broadleaf's code paths that the oak would then pass through, so the oak's bytes would rest on
every such branch being taken the old way at scale 1. A species of its own shares the kit (`kit.h`:
the axis, the tube, the spray, and the helpers added for this batch) and nothing else, so the oak
cannot move, and its ranges are its own. What it no longer has of its own is a twig and a leaf of
the tree's size: since G73 its twig is 0.8 m and 6 mm in radius and its leaf 0.3 m, the species'
on a 40 m tree and a 150 m one, and its size shows as more orders and more twigs
([allometry](#allometry)); until then a 100 m tree's twig was 5 m and its card 1.4 m.

**The trunk** is straight — a lean of up to 8° and no wander — so the structural rule holds for it
exactly; its foot is sunk a metre (`k_sink_m`) so the flare meets uneven ground. Its **core** tapers
linearly from `trunk_radius_m` to `taper` of it at the trunk's top (0.8 of the height, or 8% of it
above the highest limb, at most 0.9; a decurrent habit's trunk ends lower, below). **Buttresses**: `buttresses` fins evenly round the foot from a
seeded turn, each nudged by up to a quarter of the gap, each its own reach (`buttress_reach` of the
core ± 25%) and height (`buttress_height_m` ± 30%); the surface's radius is the core's times
(1 + Σ reach × (1 − h/height)² × (1 − x²)³), *x* the angle from the fin's ridge over its half-width
(0.12 of the gap), so a fin is a plate thickest at its ridge, sweeping in to the core by its height
— the default foot reaches 11 m from the axis on a 5 m core, and 1.4 buttress heights up the bark is
the core to a micrometre. **Hollows**: `hollows` doorway-sized cavities (`hollow_width_m` ×
`hollow_height_m`, `hollow_depth` of the core deep), the first with its floor at `hollow_sill_m`
and each next `hollow_spacing_m` above, each in the middle of a gap between two buttresses (every
other gap from a seeded one). The trunk is a grid of rows and columns at the plan's even steps
**with the doorways' sills, lintels and jambs and the fins' ridges and flanks pinned into it**, so a
doorway is the size asked on every rung of the ladder and a fin keeps its ridge however few columns
the trunk has; the seam sits in the widest stretch of circle between the doorways. A hollow is the
window's own vertices moved `depth` in along the doorway's axis — the back wall, following the
trunk's curve — and the rim duplicated (its own normals) with walls from the rim to the back: a
flat floor at the sill, a flat lintel, jambs parallel to the axis. `flora::ancient_hollows`
(`structure.h`) gives each one's sill, inward and up directions and size from the parameters alone,
which is what a hut placed in one reads; the test fires a segment through the doorway (open), on
into the back wall (closed) and down from inside onto the floor.

**The habit** (`habit`, G79; [megaflora habits](../experiments/megaflora-habits.md) has the
sources): the crown's architecture, and every limb field left at 0 is its.

| | `emergent` (the default) | `spreading` | `mixed` |
|---|---|---|---|
| Real form | the giant sequoia, an old Douglas-fir | the open-grown oak, the monkeypod | the two together; the tropical emergents come closest — the rarer, fantastical choice |
| Limbs (`limbs`) | 7 | 6 | 8 |
| The limb band (`limb_low_m`, `limb_high_m`) | 0.5 to 0.75 of the height | 0.2 to 0.3 | 0.55 to 0.7 |
| The trunk | excurrent: through to 0.8–0.9 of the height, a crown on its top | decurrent: ends 2% of the height above its highest limb, where it dissolves into them | decurrent |
| Crown width over height (`crown_spread`) | 0.6 | 1.5 | 0.9 |
| The lowest limb's radius (`limb_radius_m`) | 0.42 of the core there | 0.45 | 0.42 |
| The highest run's rise above the lowest's | 25° | 45° | 35° |
| Forks past the run (`forks`) | 2 | 4 | 4 |
| The limb past its run turns up by | 50° (× (0.6 + 0.4 `upturn`)) | 15° | 15° |
| Dead stubs on the clear bole | 4 | none | 3 |

A decurrent trunk tapers only as far as an excurrent one of the tree's height would have where it
stops — `taper` is then the core's share at the tree's top — so the limbs take the section it
still has. **A stated field keeps working**: an old recipe's `limb_low_m`, `limb_run_m` or
`limb_reach_m` is the band, the run or the length asked, and the habit fills in what it does not
say (`Ancient` version 4 made the six limb fields' defaults 0 and added `habit`, `crown_spread`,
`forks`, `wood_rupture_mpa` and `wood_density`; a version-3 file reads as it did, its unstated
limb fields the default habit's).

**The major limbs**: `limbs` of them through the band on the golden angle, each round the trunk by
up to 0.45 rad more from the seed, each its own vigour (±15% of its length), thickness (±10%) and
rise (±3°); the upper ones thinner by the habit's share. **The run** is straight — `limb_rise_deg`
above level for the lowest (default 4°) and the habit's rise more for the highest — from inside the
trunk (half the core radius out from its axis), tapering to 0.85 of its base: **that is where
platforms go**, straight, and load-bearing at 0.25 m and more. Asked, a run is `limb_run_m` (the
highest 0.65 of it); unasked it is the habit's share of the limb (0.25 to 0.3) and at least 4 m past
the bark, so every habit's lowest limb has one. **Past the run** the limb is a chain of named
segments: the first, then a fork into two daughters (at the first fork one time in five three) at
the end of every segment for `forks` levels, each daughter 0.8 (emergent) or 0.85 of its parent's
length. The daughters share their parent's section by the pipe rule (`area_exponent`), the greater
0.5 to 0.74 of it, and **the greater one deviates the less**: the two directions weighted by their
sections sum to the parent's (Murray 1927), the fork's whole angle 50° to 65° in a plane the habit
tilts from level (a spreading tree's daughters splay sideways, an emergent's one up and one out).
Every segment swings from side to side on a seeded wave by up to the habit's 14° to 35°, wanders by
half `wander_deg` a step, droops under its load over its first half and turns up over its second
([below](#limbs-a-wood-could-carry)); an inner segment tapers to 0.92 of its base, a last one to the
recursion's leader. One time in five to ten the last fork's lesser daughter is **a broken stub** —
three tenths of its length, blunt and bare — and an emergent's or a mixed tree's clear bole carries
the habit's few **dead stubs** of shed limbs, short and drooping, never in a hollow. From the last
segments the [allometric recursion](#allometry) grows (`branches` laterals from 0.15 of the way out,
each 2.5 × `branch_length` of the segment's own length, then self-similar orders by the pipe rule
down to the species' twig, 6 mm in radius and 0.8 m long, whose cards are leaves 0.3 m long); an
inner segment now carries four laterals from a crown rooted at 0.55 of its radius, with 1.2 times
the terminal crown's lateral length, and the runs carry renewed uprights (G85). The trunk's top carries a crown (an excurrent
trunk's) or a few shoots between its limbs (a decurrent one's).

**G79 measurements, before G85's renewal:** on the default 100 m tree, 7 limbs from 50 to 75 m up, 58 named segments past their runs (3 broken),
4 stubs, and **496 parts** — the trunk, the runs, the segments, the stubs and the recursion's shoots
of the part class and up; a crown 55 m across and 93 m tall. The spreading recipe
(`ancient-spreading.flora.json`, 50 m) is 70 m across and 39 m tall on six limbs, 231 segments; the
mixed (`ancient-mixed.flora.json`, 80 m) 67 by 81 m on eight, 308; `ancient-emergent.flora.json` is
a 100 m emergent of another seed. **The old straight-limbed tree is gone**: `ancient.flora.json`
(elder A) retains nine limbs from 30 to 62 m, a requested 24 m run and 36 m past it. G85 increases
its authored limb radius from 1.6 to 1.8 m to carry the denser crown while retaining an 8 m clear
walking span; its renewal stays at the trunk-side crotch. The mechanical cap still sizes every run.

### Limbs a wood could carry

**A limb is never longer than its wood could carry** ([megaflora
habits](../experiments/megaflora-habits.md#limbs-a-wood-could-carry) has the model's sources and the
numbers it comes to). By the pipe model a limb system carries about its base's section of wood out to
its tips, so the bending moment at the base of each of its parts — the run and every segment — is the
green wood's weight (`wood_density`, 1,000 kg/m³) times the sections times their horizontal lever
arms, summed as vectors over everything the part carries, with the crown the recursion grows past
each last segment counted as its section over 2.5 of its laterals' length a third of that on. The
stress at the part's base, 4M / (π r³), is held under the allowable: the green wood's modulus of
rupture (`wood_rupture_mpa`, 50 MPa, green white oak) times 0.6 for a decade's sustained load over a
static factor of safety of 2 — 15 MPa. **A limb asked for more is shortened, run and all, by one
factor**: lengths scale and sections stay, so moments scale by its square, and k = √(1 / peak) puts
the worst part on the limit. The limb is grown at its asked length, sized, and grown again at its
size (the draws were all made first, so a shortened limb is the same limb, shorter); the run never
falls below 2 m past the bark. Level and loaded, a segment **droops** — a turn down over its first
half of up to 14° at its wood's limit, in proportion to its load and to how level it is, and the turn
back up and beyond over its second: the elbow of an oak's long low limb, which is a limb's base angle
falling as it grew heavier and its tip recurving by reaction wood, not the elastic sag (centimetres
on a limb metres thick). `flora::ancient_limbs` (`structure.h`) gives each limb's height and base
radius, the length asked, the share the wood kept, its reach seen from above, its peak stress against
the allowable, and its segments and broken stubs, from the parameters alone; the test holds every
limb under its allowable stress. On the default tree no limb is shortened (the worst at 14.2 of 15
MPa); on the spreading recipe five of six are, to 0.64 to 0.86 of what they were asked; on the forest
village's anchor recipes (20 m runs and 28 m past them near level) every limb but one, the lowest to
0.81.

**The budget** (default 1,500,000; 20,000 to 4,000,000). The default 100 m tree is 321,954 triangles
at full detail — bark 160,046 (the trunk 96 columns plus the pinned ones by a row a 0.6 m, the runs 32 ×
12, every segment one row a 1.5 m and one side per 0.2 m of circumference up to 20 rows and 24 sides,
the stubs, the recursion's parts and its shoots of class 2 and up), leaves 161,908 — with no rung
walked; it was 1,419,335 until the habits, its wide crown's footprint carrying four times the leaves
(the leaf count is the leaf area index over the footprint). The ladder ([above](#the-budget-of-a-crown)):
the parts', the runs' and the segments' rows; the recursion's cross-sections; the trunk's columns and
the runs' and segments' sides; the folded wood class by class to the part class; the trunk's rows
halved; every order's sides again; the trunk to 40 columns and its rows halved again; then the
merges and the halvings; and last two rungs of everything's rows and sides (the fins' flank pins go,
their ridges stay). On 100,000 it is 50,882 after 10 rungs, and its floor, **20,000**, holds 18,114
after 12; everything at its most limbs and laterals holds it too (14,357 after 27 rungs, 746 parts):
the part cap is what bounds it. `ancient-1m` (110 m on a million triangles) is 435,153 with no rung
walked. **What it costs** (msvc-release, a shared machine, so upper bounds): `engine-content flora`
builds elder A's container in 4.2 s (22,800 clusters, 13 levels), the emergent recipe's in 2.7 s.

### The structural table

What a structure solver — platforms, bridges, ramps and huts in and between trees — needs beyond
the motion contract's pivot, direction, length and parent is **how thick each part is** and
**which parts may carry a structure** (`include/domain/flora/structure.h`). Every species of the
second batch emits it beside the motion contract, `Motion::structure`: one
`geometry::ClusterFileStructuralPart` (16 bytes) **per part, in id order** — the part's id, its
`base_radius` and `tip_radius` in metres, and `flags`:

| Flag | Meaning |
|---|---|
| `k_structural_load_bearing` (1) | a trunk, a stem, a major limb: a structure may hang from it; its base radius is at least the species' `load_bearing_min_radius` |
| `k_structural_straight` (2) | its axis is the segment from `pivot` along `direction` for `length`, to the millimetre |

**The rule a structure solver may rely on.** Records come parent-first (their order is the parts'),
so one pass meets every parent before its children. A part's axis is the segment from its pivot
along its direction for its length (`structural_point`), and its radius at arc *s* is linear from
`base_radius` at the pivot to `tip_radius` at the far end (`structural_radius`). That holds
**exactly for a part flagged straight** — the megaflora's trunk and every run; the test measures
every vertex of every straight tube at its structural radius from the segment to 2 mm — and a part
without the flag curves (it turns towards the light, wanders or hangs), its segment being its rest
tangent's, so **a structure anchors to straight load-bearing parts**. A radius never grows along a
part, nor from a parent into its child where the child leaves it. A part with no wood (a spray of
leaf cards, a willow's curtain) records radii of zero. The megaflora's trunk records its **core**:
its buttresses stand outside the core radius in the bottom `buttress_height_m` (up to 1.3 of it),
and its hollows cut into it where `ancient_hollows` says. The minimum radii a load-bearing part
reaches inside each species' ranges: birch 0.018 m (the thinnest clump stem of the thinnest birch,
0.03 m × 0.6), willow 0.08 m, orchard 0.04 m, ancient 0.25 m (since G79 a threshold: a run or a limb segment is load-bearing when
it is at least that at its base, so a limb's segments carry the flag as far out as they are that
thick); the default ancient tree's load-bearing parts are its 5 m core, its seven runs 1.2 m at the
lowest limb to 0.69 m at the highest (2.4 m to 1.4 m thick), every limb's first segment past its run
and its later segments down to 0.25 m — 56 parts.

**In the container** the table is **section 37**, `structural_parts`, resident, appended after the
motion contract and **written only when a species emits it**: the palm, the broadleaf and the
conifer emit none — the shared helper that opens a part (`Emitter::begin_part`) knows no radius, and
their solvers' code paths stay as they were, so their containers are the bytes they were — and an
imported mesh's container never has it. A reader refuses a record naming a part the motion table
does not have, or out of order. The default ancient tree's table is 496 rows (7,936 bytes); the
first 15 are what a village plan reads: the trunk (5.0 → 2.0 m), and seven runs, each followed by
the first segment of its limb past it (part 1 + 2i is limb i's run, the order `ancient_limbs`
reports them in); the limbs' later segments, the bole's stubs and the recursion come after.

### Walking a limb

A flora mesh is a static body of the scene's collision like any other mesh ([scene
collision](scene_collision.md), "What gets no body": a tree is not in that list), its proxy the
coarsest cut of its DAG within 5 cm, so a limb is walkable if a walker can get onto it.
`apps/engine_view/tests/walk_limb_view_tests.cpp` puts one there: on the committed
[`elder-grove`](../../content/test-scenes/elder-grove/README.md) (copied with its recipes into the
test's scratch directory), it solves each ancient tree's recipe, takes the lowest run from the
structural table, and starts a walker standing on it 2.5 m out from the bark, facing along it, W
held, replayed **offscreen** (msvc-debug, RTX 5090, 2026-10-22; the walker's defaults: capsule 1.8 m
by 0.3 m, 1.5 m/s, 40° slope limit, 0.35 m step). **The case holds the first 15 m, or as much of the
run as its wood left past the start less a metre** (since G79 a run is no longer than its wood could
carry, and elder A's asked 24 m keeps 15.0):

> **The numbers below are G79's (2026-10-22).** G85 (fa343854, 2026-10-23) raised elder A's limb
> radius from 1.6 to 1.8 m for the fuller crown and halved the automatic runs' share, so the wood now
> keeps a 13.64 m level run and the walk's figures moved; fa343854's measurement is in
> [tree growth rules](../experiments/tree-growth-rules.md) (the walk: 8.73 m moved at six seconds).
> This paragraph is kept as the G79 record until the case is re-measured and rewritten.

- **The near-level run** (elder A, 30.0 m up, 3.6° above level, 15.0 m kept of the 24 asked, 2.9 m
  thick tapering to 2.48): the walker walks the 10.5 m of it past its start at 1.5 m/s, **its feet
  within 1.3 mm of the bark** and 0.06 mm aside at its end — the case holds those 9.5 m to 15 cm.
  **At the run's end** it walks on, onto the limb's first segment past it, which meanders and turns
  up: its feet 0.2 m over the run's line 16 m out, it walks **off the side** at 17.5 m and falls
  32 m to the ground, then walks on across the grove until something stops it.
- **The 20° run** (elder B, 30.0 m up, 19.1° above level, 24 m long — its wood keeps all of it,
  a rising limb's lever being short — 2.7 m thick): it climbs at 1.34 m/s along the slope, but
  **drifts sideways off the crest** — 0.42 m aside after 13 m, 0.83 m after 17.6 m, its feet
  sinking 0.25 m below the crest line as it moves onto the limb's flank — and walks off its side at
  1.0 m aside and falls 39 m.

**What the walker lacks for ramps and platforms.** It has a 40° slope limit and a 0.35 m step and
**nothing else**: it does not refuse a ledge, so it walks off the end or the side of any limb or
platform at walking speed, and a structure that must keep it on needs a rail or a lip it cannot
step (over 0.35 m); a slope under the limit holds a walker standing still but not one walking
across a curved surface — on a round limb sloped at 20° it slides off the crest by about 5 cm a
metre, so **a ramp should be a flat-topped deck on the limb, not the bare limb**; a slope over 40°
(the limb past its run as it turns up) is one it slides back down; and it has no notion of a
platform moving or a limb swaying (every body is static, [scene collision](scene_collision.md),
"Nothing moves" — wind on the trees, S66, will need the collision to follow the deform or the
structures to be anchored to parts that do not sway). A near-level run is a floor as it stands: a
walker stays on its crest the length of it.

## The understory

**The shrub, herb and ground layers under the canopy** (roadmap G75, 2026-10-21; the research, the
constants and the measurements are [the understory](../experiments/understory.md)): four species a
forest's floor is made of, `engine.flora.Shrub`, `Fern`, `Tuft` and `Herb`, each a struct of the
flora schema with its ranges (`shrub_in_range`, `fern_in_range`, `tuft_in_range`, `herb_in_range`
refuse a field out of range by name), a solver file of its own (`src/shrub.cpp`, `fern.cpp`,
`tuft.cpp`, `herb.cpp`, over the kit) and the same `.flora.json` dispatch (`include/domain/flora/understory.h`).
Saplings are not a species: a tree species at a small `height_m` is its sapling since the allometric
crown (`oak-sapling.flora.json`, a 3 m broadleaf). Leaf litter is the ground's material, and moss,
lichen and fungi are roadmap G78's, grown over a host rather than standing on the ground.

**What every one of them shares.** **Part 0 is the root** — the stool, the rhizome's crown, the
tuft's base, the herb's root — a pivot at the ground with **no geometry**, which every other part
hangs from: so nothing of a fern, a tuft or a herb is the trunk the collision keeps, and the stem, the
fronds and the blades are all parts a walker's push bends. Each emits the motion contract and the
structural table (one row a part; the root's radius the stool's or the crown's, a frond's, a
blade's, a leaf's and a flower's zero, no part load-bearing). Its structure is grown once, at a fixed
resolution, and its ladder changes only detail — never a stem, a frond, a blade, a leaf or a flower
— so the parts and the table are the same bytes on every rung.

| Species | Default | Parts | Default triangles | Budget | Primitives |
|---|---|---|---|---|---|
| `shrub` | a 4 m hazel, 16 stems | the root, 16 stems, 17 recursion parts | 23,028 | 4,000–400,000, default 30,000 | bark (one-sided), leaves |
| `fern` | a 0.9 m male fern, 14 fronds | the root, a frond each | 1,736 | 500–100,000, default 4,000 | fronds |
| `tuft` | a 0.45 m hair-grass tussock, 120 blades | the root, a blade each | 1,080 | 1,500–50,000, default 4,000 | blades |
| `herb` | a 0.6 m daisy, 8 leaves, one head | the root, the stem, a leaf each, a flower each | 190 | 1,000–50,000, default 2,500 | leaf (the stem and leaves), flower |

### The shrub

`stems` basal stems (16) from a stool of `stool_radius_m` (0.25 m), their feet filling its disc as a
sunflower does with the stoutest at the middle, each leaning out by up to `stem_spread_deg` (28°) the
further out it stands and turning towards the light by `upturn` (0.4; negative arches the canes
back down, which is how `bramble.flora.json` is a shrub). The stoutest stem's radius is
`stem_radius_m`, or at its default of 0 the **allometric radius of the height**, 2.2 cm × (H / 4 m)^1.5
(H ∝ D^(2/3), as the broadleaf's trunk); the others are 0.55 to 1 of it and shorter by the same law.
**Every stem grows the allometric recursion** ([above](#allometry)) as a named root with `branches`
laterals along its upper 0.3 to 0.95, down to the species' twig (`twig_radius_m` 1.6 mm,
`twig_length_m` 0.45 m), and its leaves are one leaf a card at the species' size (`leaf_length_m`
0.10 m, `leaf_width` 0.8), their count `leaf_area_index` (2.5) over the footprint — so a 1 m hazel's
16 stems are twigs themselves, the 4 m default has 889 twigs over 5 orders and 9,190 cards (a
realised LAI of 2.16 over 29.4 m²), and a 5 m one 2,047 twigs over 6 orders, every card 0.10 m. The
budget's ladder is the broadleaf's at a shrub's scale (the finest wood, rows and cross-sections, the
folded classes, then the cards merged and halved, the last rows), and a stem's sides follow its girth as the recursion's tubes' do (a 6 mm cane is three sides round): on 12,000 the default is 5,655 and on its floor of 4,000, 2,059.

**Collision**: a stem whose radius at the ground is at least **`k_shrub_collider_radius_m`, 1.25 cm**
is flagged `k_structural_collider` (`structure.h`, `geometry::k_structural_collider`, 4) in its
structural row: it stops a walker and is never pushed; a thinner stem, the recursion's shoots and
the leaves are walked through and bent aside. The number is a green stem's cantilever under a
walker's 150 N push at a stride's deflection ([the understory](../experiments/understory.md#the-collision-threshold)):
a hazel's 2 to 8 cm stems stop a walker and a dogwood's thumb-thick ones do not. The default hazel's
16 stems all collide on its seed (a stem may be as thin as 0.55 × 2.2 = 1.21 cm, just under, and none drew so thin); `dogwood.flora.json` (9 mm) has none.

### The fern

`fronds` fronds (14) on the palm's crown rule at a tenth of its scale: each one's angle from vertical
`spread_deg` × √age (50°), the youngest nearly upright at the crown's middle, the oldest furthest out,
on the golden angle, rising from a ring of `crown_radius_m`, 0.75 to 1 of `frond_length_m` (0.9 m)
long with age, its rachis **arching under its weight** by `droop` × 90° (0.45), the older more. A
frond is **one spray** of cards (`build_spray_profile`, the kit's spray with a length per station):
the stipe (`stipe`, 0.22 of the frond) a ribbon of 4 mm cards on the same surface, then `pinnae`
(24) pairs of pinna cards at `pinna_angle_deg` (70°) whose length follows a lanceolate outline —
sin(π (0.15 + 0.85 u)) of `pinna_length` (0.15 of the frond), the longest two fifths up the blade.
The ladder halves the pinnae (down to three a side), never a frond: on 1,500 the default is 840
triangles. `bracken.flora.json` is three tall, wide fronds: a bracken stand is many such plants,
placed densely, which is placement's (G77).

### The grass tuft

`blades` blades (120) from a base of `base_radius_m` (4 cm), filling it as a sunflower does, the
inner ones upright and the outer leaning out to `spread_deg` (35°) and arching by `droop` × 90° (0.5),
the outer more — a tussock's fountain — each 0.6 to 1 of `blade_length_m` (0.45 m), `blade_width_m`
(4 mm) wide at its base with straight edges to 0.65 of its length and then a point (`build_blade`),
`segments` rows (5): 2 × segments − 1 triangles a blade. **Every blade is its own part**, so the wind
moves each and a walker parts them. `crossed` doubles each blade into two crossed strips. The ladder
drops the crossing, then the rows down to two, never a blade: 400 blades hold the 1,500 floor at
1,200 triangles.

### The herb

A stem from the root — a tube of `stem_radius_m` (2.5 mm) tapering to 0.45 of it, leaning by
`lean_deg` and straightening — with `leaves` leaf cards (8; a diamond each, widest at 0.35 of its
length, `build_leaf_card`) up its lower `leaf_top` (0.6), on the golden angle or in **opposite**
pairs each turned a right angle from the last (`opposite`, a nettle's), the lowest `leaf_length_m`
(7 cm) and the top half of it, each its own part; and `flowers` flowers, each its own part: one head
at the stem's top (`flowers` 1, `spike` 0) or a **spike** up its top `spike` share, nodding out 70°.
A flower is a fan of `petals` petal cards (16, `petal_length_m` 2 cm) at `petal_angle_deg` from its
axis (82°: a flat head; 22°: a foxglove's bell) and the wedges between them, which make the disc,
in a **flower** material of its own in the recipe's colour (`flower_red`/`green`/`blue`). The stem is
in the leaf material, two-sided, its UVs a corner of the leaf card. The ladder lowers the stem's rows
and sides, then halves the petals; never a leaf or a flower. `daisy.flora.json` is the default,
`foxglove.flora.json` a 1.2 m spike of 22 purple bells, `nettle.flora.json` a 1 m stem of 20 opposite
leaves and no flowers.

### Detail tiers

**Every flora kind has a detail tier** (the owner, 2026-10-20): a graphics preset will be able to
leave out whole kinds of plants (the detail-tiers row puts a `detail_tier` on a scene instance and a
cut-off in the render settings), so each kind says which tier it is, `engine.flora.DetailTier`:
`essential` (every preset), `standard` (all but the lowest), `rich` (the highest only). **What a
preset may drop never collides**: a kind whose instances carry a body is never `rich`, so a preset
never changes what a walker meets.

| Kind | Tier | Where it is declared |
|---|---|---|
| every tree species | `essential` | by rule (`flora_detail_tier`): a tree collides by its trunk, and a field would move every tree's pinned canonical parameters for a value that cannot differ |
| a shrub whose stems collide | `essential` (the default) | `Shrub::detail_tier`; `rich` is refused by name |
| a shrub a walker passes through | `standard` (its recipes say so) | `Shrub::detail_tier` |
| a fern, a tuft, a herb | `rich` (the default) | `Fern`/`Tuft`/`Herb::detail_tier`; any tier is allowed, since none collides |

The understory's tier is in its canonical parameters, so it is in the container's generator section
(`engine-content info` prints it) and in its source hash; `flora_detail_tier(file)` answers it for
any kind and `flora_collides(file)` says whether its instances carry a body. **The renderer honours
none of it yet**, and the scatter generator does not yet write a tier on the instances it places:
both are the detail-tiers row's, which reads the tier from here.

## The kit for companions

**What grows over a mesh rather than on the ground** — a vine's stems and leaves, a cover's moss
cushions, lichen tufts, brackets and mushroom caps — is the cover capability's ([cover](cover.md),
roadmap G76 and G78), and it is built with this module's kit through `include/domain/flora/companion.h`
(`src/companion.cpp`, 2026-10-21): `KitWriter` over a mesh and its `Motion`, `kit_begin_part` (a
motion record and a structural row), `kit_polyline_tube` (a tube along any polyline, a radius and a
bend weight a point, framed by parallel transport, its tip closed by a cone), `kit_leaf_card` (the
understory's leaf card) and `kit_dome` (a cushion, a cap, a bracket's shelf). They are thin public
wrappers over the kit's emitter, **added after everything else and changing none of it**: every
species' bytes, and `k_flora_version`, stay where they were. A companion emits the motion contract
with part 0 a root of no geometry and no part load-bearing, so the walker's collision keeps none of
it; a tree as a host offers a climber only the triangles of its colliding parts, its trunk and
load-bearing limbs (`assets::host_mesh_of`, read from sections 35 to 37).

## Materials

Every part samples a tiled PBR set, or draws the flat colour it always drew
([materials](materials.md) has the module, the pack and the container's side of it). A part's set
is, in this order:

1. **the recipe's**, when its `materials` block (`engine.flora.FloraMaterials`) names an image for
   the part — `bark` for the trunk, its collar and a tree's tubes; `leaf` for a palm's green fronds
   and a tree's leaves or needles; `skirt` for a palm's dead fronds; paths relative to the recipe;
2. else **the material pack's default for the part's role**: `palm/bark`, `palm/frond`,
   `palm/skirt`; `broadleaf/bark`, `broadleaf/leaf`; `conifer/bark`, `conifer/needles`; the understory's
   `shrub/bark`, `shrub/leaf`, `fern/frond`, `grass/blade` (a tuft's), `herb/leaf`, `herb/flower` (no recipe
   block names a flower: the pack's role or the flat colour); a species
   with no list of its own (the next ones) is `<species>/bark` and `<species>/leaf` for its first
   two materials (`flora::flora_role`);
3. else **none**: the flat colour, and one log line naming the role, once per run.

`wood` is recorded and keyed and drawn by nothing ([not yet](#not-yet)).

**The UVs per part**, which decide whether the set's repeat is a transform:

| Part | UVs | Transform |
|---|---|---|
| Palm trunk, apex dome, collar of bases | metres: *u* round at the mean radius, from the spine's frame and not from each ring's turned first column (the rings' turn wanders a few degrees a ring, and a *u* that followed it would twist the bark's grain with the sum of those turns up the trunk), *v* up the arc; the dome at its own size | `1 / tile_m` |
| Palm green and dead fronds | metres: *u* along the rachis, *v* across it (0.5 at the rachis) — the material lab turns its frond sets so the image's *u* runs along the frond | `1 / tile_m` |
| Broadleaf and conifer bark (`kit.cpp` tubes) | metres: *u* round, *v* along the axis | `1 / tile_m` |
| Broadleaf leaf and conifer needle cards | the whole card per image (`build_spray`) | none: the image whole |
| The understory's leaf, pinna, blade and petal cards | the whole card per image (`build_spray`, `build_spray_profile`, `build_blade`, `build_leaf_card`); a herb's stem a corner of its leaf card | none: the image whole |

The palm's fronds are geometric leaflets on one island a frond, not cards, so their set is a tiled
surface (the lab's frond sets are "longitudinal surface samples" at a `tile_m` of 0.12 m), and they
are in metres like the bark; a palm frond texture is not one picture of a frond. The trunk's UVs
were in circumferences and a frond's in its own length until `k_flora_version` 3, which is what
moved the palm's pins (2026-10-19).

**What a set must be.** Tiled (it wraps), sides powers of two, `tile_m` the surface one repeat
covers (0.05 to 20 m), the normal map OpenGL (+Y) as the lab writes it, roughness and metallic
separate greyscale images, height carried and read by nothing yet. A leaf or needle set is one leaf
per image. Roughness alone goes in glTF's metallic-roughness slot with a metallic factor of 0 —
bark, leaves and stone are not metal, and packing a metallic channel would need image io the solver
does not have ([materials](materials.md#what-the-container-records)). `normal_scale` is glTF's
normal strength (the lab's preview strengths are 0.4 for bark and 0.25 for fronds; the pack carries
them).

**Precision.** UVs reach the GPU as half floats (geometry.md), so a *v* of 8 to 16 m has a step of
7.8 mm: above 8 m a trunk row's *v* is off by up to 3.9 mm, the same for the whole row (a row is
one *v*), which stretches or squeezes the repeat between two rows about 10 cm apart by up to 4% —
an unevenness of the bark's grain up the top of the 11 and 14 m palms, not a seam. *u* stays under
4 m (a 2 mm step). Bands in the trunk's *v*, each a whole number of repeats from zero, are the fix
if a closer look ever needs one.

**The first pack on the palm** — the lab's Bark A, Green C and Dry B through the cache, the build
times, the `.tex` sizes and what the captures showed — is in
[materials](materials.md#the-first-pack-2026-10-19).

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
mixed with the species and `k_flora_version` — under the LOD options, no weld, and the page target,
and combined with the **material pack's key** when the build was given a pack with roles
(`assets::generated_mesh_build_key`): a palm drawn with a pack's defaults and one drawn flat are two
entries, and a build with no pack keys exactly as before packs existed. The source hash stays the
file's alone, so a scene that names a palm by its hash still finds it.

`engine-content flora <params.flora.json> <out.clusters> | --cache [--ddc <dir>] [--force]
[--materials <dir> | --no-materials]` is the command line over it ([apps](apps.md)): a named output,
or the derived-data cache entry the key addresses, skipped when it already records that source hash
and key; with a derived-data root it runs the **texture step** beside the container, so every image
the parts sample is a block-compressed `.tex` in `<ddc>/textures/` as a glTF's is
([content_build](content_build.md), [texture](texture.md)). It is the same function the
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
spruces and a palm on a nearly flat ground with a camera path round and into them. The second
batch's recipes are `birch.flora.json`, `birch-single.flora.json`, `birch-clump.flora.json`, `willow.flora.json`,
`orchard-pruned.flora.json`, `orchard-apple.flora.json`, `ancient.flora.json`,
`ancient-rising.flora.json` (90 m, its limbs' runs rising at 20° and more) and
`ancient-1m.flora.json`; [`content/test-scenes/elder-grove/`](../../content/test-scenes/elder-grove/README.md)
stands two ancient trees 60 m apart with four willows, eight birches and an orchard row of seven,
with a camera path from the ground up into the canopy and a walk start at a hollow. The understory's
recipes (G75) are `hazel`, `dogwood` and `bramble` (shrubs), `male-fern` and `bracken`, `hair-grass`
and `fescue` (tufts), `daisy`, `foxglove` and `nettle` (herbs), with `oak-sapling` (a 3 m broadleaf);
[`content/test-scenes/understory/`](../../content/test-scenes/understory/README.md) stands them on a
grove's floor — shrubs in the canopy's gaps, ferns and nettles in its shade, grasses and daisies in a
clearing, by three scatter layers, with foxgloves and brambles by hand — 415 instances and a walk at
eye height.

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

**The UVs in metres (`k_flora_version` 3, 2026-10-19)**, the same procedure (msvc-debug, RTX 5090,
captures under the GPU lock, none of it a timing). Only the UVs moved, and the simplifier weighs
them (geometry.md, "What the simplifier is given"), so the DAG moved a little:

| Palm | Triangles | Clusters / levels | Islands | Seam vertices | Smallest island (texels) | Collapse share | FLIP | PSNR / SSIM |
|---|---|---|---|---|---|---|---|---|
| `date-palm` (default) | 55,312 | 1,753 / 12 | 238 | 4.1% | 59,864 | 0.242 (193 of 799) | 0.0213 | 27.2 / 0.960 |
| `-tall` | 51,984 | 1,543 / 11 | 238 | 5.7% | 74,104 | 0.144 | **0.0181** | 26.9 / 0.963 |
| `-old` | 28,624 | 823 / 10 | 146 | 7.6% | 47,306 | 0.247 | **0.0105** | 29.4 / 0.979 |
| `-leaning` | 52,048 | 1,646 / 11 | 232 | 4.2% | 59,404 | 0.245 | 0.0285 | 25.9 / 0.948 |
| `-young` | 42,700 | 1,378 / 11 | 198 | 3.8% | 53,550 | 0.350 | 0.0395 | 24.7 / 0.922 |

Every palm passes and fails where it did at version 2, FLIP within 0.001 of it everywhere; the
smallest island grew because its area is now in square metres of surface rather than in
circumferences, and no build or stats run warned. The branching species did not move (their UVs
were already in metres and their pins held), so their table above stands.

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

- **After G79–G81 (2026-10-22)**: the emergent megaflora's crown is too thin for its references
  (G82); the conifer has no impostor chain, so a lone spruce at 60 m is still specks (G83); the
  forest's frame after its spruces and birches doubled and tripled their triangles is unmeasured
  (G84). **What the two consumer games should change** (neither repo was edited): the forest stress
  test (`forest_wildlife_poc`) needs a `conifer/needles` image that is one irregular foliated shoot
  25–35% opaque with needles at least 2 texels wide at 512 — its fishbone image is 17% opaque and
  periodic, which no mip chain keeps ([conifer shoots](../experiments/conifer-shoots.md)) — and its
  birch recipes should drop `stems: 1` and `limbs: 14` and vary their seeds, so the seed draws the
  clumps (`clump_chance` 0.5 or more for a cut-over or browsed stand); the fantasy RPG's anchor trees
  (`divine-rpg`, `anchor-a`/`anchor-b`) state their runs and reaches and so get an emergent crown
  whose wood shortens nearly every limb (the lowest keeps 0.8 of its 48 m), and want
  `"habit": "spreading"` or `"mixed"` with `limb_run_m` dropped (the habit gives about 10 m) or set
  to what `ancient_limbs` says the wood keeps, revalidating their structural part ids after.

- **The sets themselves** (G60's other half): the engine reads a recipe's block and a pack's
  roles ([materials](#materials)); the palm's three roles have the lab's candidates, and the oak's,
  the spruce's and the leaf cards' have nothing yet ([content/materials/README.md](../../content/materials/README.md)
  says what the lab must make). A leaf card's set is alpha-tested since R65 — a card set is a cutout at 0.5 ([materials](materials.md), [renderer](renderer.md#alpha-tested-geometry)), and `content/test-scenes/leaf-cards/` draws the oak's with a procedural leaf — so what the cards lack is the lab's image, not the mask.
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
  function and a ladder (a pine's tufts at the branch tips, a fan palm); bark that is not rings or
  tubes (plates, fissures, a megaflora's furrows) wants displacement or the height map of a set.
- **Textures by role for the second batch**: the birch's, willow's, orchard tree's and ancient
  tree's bark tubes are UV'd in metres and their cards whole, like the broadleaf's, but which set
  each part samples (the birch's white bark, the willow's curtains, the fruit) is the materials
  work's (G60), not these solvers'.
- **The canopy's leaves ride their parts** (G73): a broadleaf's or an ancient tree's twigs are no
  longer parts of their own — there are hundreds of thousands — so a leaf card sways with the finest
  part it grows from, at that part's twig stiffness (the renderer's "leaf" class is for parts with no
  wood, which these crowns no longer have); a leaf's own flutter is a per-vertex term the stage does
  not have yet.
- **Branch-group and whole-tree impostors** (R65, R89): the merged cards of a lower rung are opaque
  parallelograms a metre and more long until alpha-masked cluster textures baked from the solver's
  own finest geometry draw them as foliage, and octahedral impostors take the far field
  ([tree allometry](../experiments/tree-allometry.md#what-the-aaa-pipelines-do-with-a-million-leaves)).
- **The understory's images** (G60): `shrub/bark`, `shrub/leaf`, `fern/frond`, `grass/blade`,
  `herb/leaf` and `herb/flower` have no sets yet ([content/materials/README.md](../../content/materials/README.md)
  says what each must be); a pinna, a blade and a petal want an alpha mask (R65) to lose their
  polygon's outline.
- **The understory's placement** (G77): ferns under the canopy and grass in its gaps is a density
  field from the trees' light, which the scatter generator does not compute yet; the understory
  scene places by hand where its rules cannot.
- **Self-organising growth** (G74, proposed): bud fate by light, so crowns fill their light and
  apical control and shedding follow from age rather than from the recipe.
- **A walker on the same limb at every rung**: the collision proxy is the mesh's triangles by part,
  and between the default ancient tree's finest rung and its floor the limbs' surfaces move up to
  0.44 m (the runs 0.12 m); a proxy built from the structural table's tubes would not move at all
  ([tree allometry](../experiments/tree-allometry.md#what-it-costs)).
- **The structural table for the first three species**: the palm, the broadleaf and the conifer
  emit none, because the helper that opens their parts knows no radius and their pinned code paths
  were left alone. Adding it is a change to each solver and a new pin each.
- **A megaflora's hollows as parts**: a hollow is geometry in the trunk's part and is found through
  `ancient_hollows` from the parameters, not from the container; a structure solver reading only a
  container learns the trunk's core and not where its doorways are.
- **The palm's solver on the kit**: `palm.cpp` keeps its own copies of the f64 vector, the stream
  and the emitter that `kit.h` now holds for the branching species; moving it over is mechanical
  and must leave its pinned bytes alone.

## Determinism

The same parameters give the same bytes on every toolchain and at either CPU baseline: the solver
computes in f64 with IEEE `+ - * /` and `sqrt` only, contraction is off tree-wide (ADR-0035), every
sine and cosine is `det::sin`/`det::cos` (`core/math/deterministic.h`), its randomness is a
splitmix64 over the seed with one stream per part (so a draw added to the crown never moves the
trunk), and it runs on one thread. **Every draw is its own statement**: two draws in one call's
arguments are taken in the compiler's order — Clang left to right, MSVC and GCC right to left — and
that is how the first broadleaf hashed one way on Clang and another on the rest until the Linux
container caught it (2026-10-18). The default palm's mesh, the broadleaf's and the conifer's (mesh
and motion contract) are pinned by hash in `tests/flora_tests.cpp`, and the birch's, the willow's,
the orchard tree's (with and without fruit) and the ancient tree's (mesh, motion contract and
structural table) in `tests/species_tests.cpp`; a change that moves one bumps
`flora::k_flora_version` and the number in the same commit. The second batch was added without
moving the first three's pins: its helpers are appended to the kit and change none of the old
ones' output, and its species are new members of the flora file that the canonical form of a palm,
an oak or a spruce does not carry. **The impostor chain (R89)** moved neither the solver's pins nor `k_flora_version`: its cards are appended after the mesh the plain solve makes, and its two fields (`card_bake_px`, `impostor_px`, `@version(3)`) change the broadleaf's and the ancient tree's canonical parameters and so their keys; its atlases are pinned by hash in `tests/impostor_tests.cpp`, and both trees' whole containers, chain included, by engine-content's `determinism_tests.cpp`, whose rows GCC and Clang reproduce. **The conifer's shoots (G80) moved the conifer's pin and not
`k_flora_version`**, for the reason the allometric crown gives next: its new fields (`@version(2)`)
are in every conifer file's canonical parameters, so every conifer key changed anyway, a version-1
recipe's included, and no other species' did. **The allometric crown (G73) moved the broadleaf's and the
ancient tree's pins and not `k_flora_version`**: it appended fields to both species (`@version(2)`),
so every broadleaf and ancient file's canonical parameters — and with them its source hash and its
cache key — changed anyway, while a version bump would have moved every other species' container
(the palm's is pinned with its identity) for nothing. Its recursion draws from one stream per shoot
(its depth-first index), and the two species' containers are pinned besides, folded section by
section, in `apps/engine_content/tests/determinism_tests.cpp` (the broadleaf at its default, the
ancient tree on a 100,000 rung, so the merged cards are in the bytes), which GCC and Clang reproduce. **The understory (G75) moved no tree's pin and not `k_flora_version`**: its kit helpers are appended
(`build_spray_profile`, `build_blade`, `build_leaf_card`), its species are new members of the flora
file that no tree's canonical form carries, and a tree's detail tier is a rule rather than a field;
the four defaults are pinned in `tests/understory_tests.cpp`, one stream a stem, frond, blade or
leaf row, every draw its own statement. **The megaflora's habits (G79) moved the ancient tree's pins and
`k_flora_version` (5)**: its limbs are a different structure, so its hash, its parts and its
container row moved, and every flora container records the version, so the palm's two container
pins and the broadleaf's container row moved with no byte of their meshes moving. The ancient tree's
new draws are each their own statement and their own streams — the limbs' layout one stream with
five draws a limb, every named segment one stream (limb × 512 + its index) with nine, the bole's
stubs one stream with five a stub, made whether or not a stub has room — and a limb shortened by
its wood is grown again from the same draws, so a recipe's tree never depends on how the sizing
went. **The birch's version 2 (G81) moved the birch's pin and no other, and not `k_flora_version`**: it appended fields (`@version(2)`), so every birch file's canonical parameters and key changed anyway; the kit's one change, `CrownSpec::droop`, is zero for every other species and adds no draw, and the broadleaf's and the shrub's pins hold (the ancient tree's moved with G79, above). The default single birch on a 400,000 budget and a three-stem clump are pinned, and a seeded clump is held to the bytes of the birch naming its count.

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
- the conifer's shoots (G80): a long live branch has no needle inside two needled reaches of its
  tip, the dead branches none at all, and with no dead zone and nothing missing every branch is
  needled; a missing share and sparse whorls drop parts and leave broken stubs while every part that
  stays keeps its pivot and direction to the bit; a comb habit's needles hang further below their
  branches than a plate's; a version-1 recipe reads and solves in its budget; the new fields are
  refused out of range by name;
- the motion contract: a record per vertex, part ids dense with the trunk 0 and every parent
  before its children, every part reaching weight 0 at its root and 1 at its tip, and its weights
  an arc-length parameter (the centroids of its vertices binned by weight lie along it at the
  share their weight says, to 0.12); the same parts on every rung of the ladder; the mesh the same
  bytes whether the contract is asked for or not;
- the contract in the container: every cluster vertex's record is its source vertex's, the table
  and the stream round-trip through a file, the two sections are appended after the others, and
  with them cleared the container is byte for byte the one built before they existed;
- the birch, the willow, the orchard tree and the ancient tree (`tests/species_tests.cpp`): valid,
  in budget at their defaults with no rung walked and on smaller budgets down to their floors
  (everything at its most expensive included), the same motion parts and structural rows on every
  rung, one seed one tree and another seed another, pinned by hash, the mesh the same whether the
  contract is asked for or not; the structural table one row a part in order, radii never growing
  along a part or into a child, every load-bearing part at least its species' minimum; the birch's
  crown within 0.34 of its height of its foot, its branchlets hanging (and not with no droop), its realised LAI within 15% of the asked at full detail, on its default and on 36,000 for one to five stems, never merged there; a seeded clump byte for byte the birch naming its count, and the seed's clump share within 5% of its chance over 1,000 seeds;
  every willow curtain its own part, hanging, never under the ground; the orchard tree flat-topped
  and broader than tall when pruned and taller when free, its fruit a third one-sided primitive of
  eight triangles a fruit, and fruit a budget cannot hold refused; the ancient tree's trunk and runs
  straight to 2 mm of their structural radii, its runs in the habit's band, the lowest near level and
  none more than the habit's rise above it, load-bearing and at least a metre thick, its foot flaring
  past the core and the core exact above the buttresses, each hollow open through its doorway, closed
  at its back and floored; its habits (G79): the emergent's lowest run above 0.45 of the height and
  its crown under 0.65 of its height across, the spreading one's under 0.3 and over 1.1, the mixed
  one's high and wider than the emergent's, every habit's lowest run straight, near level and
  load-bearing, and every limb of every habit and of the forest village's anchor recipes under its
  wood's allowable stress, the anchor's lowest limb shortened; the table round-trips through section
  37 and the palm's container has none;
- the allometric crown (`tests/allometry_tests.cpp`): the same species at 8 and 32 m (the
  broadleaf) and at 45 and 100 m (the ancient tree) draws cards of `leaf_length_m` to the millimetre;
  the broadleaf at 6, 12 and 24 m has more orders as it grows, each within one of
  1 + log_Rb((R / r_twig)^Δ); the pipe rule holds at every node of the recursion to 2% at Δ 2 and 2.3
  (to 2e-7 in fact), and every terminal twig's radius is `twig_radius_m` to within one pipe step;
  the realised LAI, measured as the leaf cards' area over the convex hull of their vertices, is
  within a fifth of the field on the default broadleaf and ancient tree; a recipe setting the old
  fields (`sub_branches`, `twigs`, `leaves`) validates, builds and is capped by them; the parts are
  dense and parent-first, 230 on the default broadleaf and 496 on the default ancient tree
  (`flora_tests.cpp`, `species_tests.cpp`); and down the ladder every rung holds its budget, merges
  no less finely than the rung above, and keeps the part list and the structural table byte for
  byte, with the load-bearing surfaces' movement between the finest rung and the floor measured;
- the understory (`tests/understory_tests.cpp`): every species' default valid, in its budget with no
  rung walked, every material two-sided but a shrub's bark, the root part 0 with no geometry and
  every other part reaching weight 0 at its root and 1 at its tip, parents first, no part
  load-bearing; on smaller budgets the same parts and structural rows byte for byte; one seed one
  plant (pinned by hash) and another seed another; the shrub's cards `leaf_length_m` at 1, 4 and 5 m
  and its realised LAI within a fifth of the field; a stem flagged a collider exactly when it is a
  stem at least 1.25 cm thick, none on a 9 mm dogwood; a fern's parts its fronds and one, its
  triangles its fronds times its stations' cards; a tuft's parts its blades and one, the most blades
  crossed and at the budget's floor; a herb's parts its root, stem, leaves and flowers, the floor
  holding forty flowers of forty petals by halving them; a rich shrub whose stems collide refused by
  name and a thin one accepted; every kind's tier and whether it collides; the tier in the
  container's generator parameters; the understory's material roles;
- a parameter out of range is refused with its name;
- a `.flora.json` in a scene's mesh table becomes a mesh with the three materials through the
  cache — a hit the second time, the same clusters built in memory — and the oasis scene's first
  frame, captured offscreen, has its crowns' pixels green and its trunks' brown, the crowns above
  (`systems/renderer/tests/flora_scene_tests.cpp`);
- the default palm hashes to the committed number, and so does its container
  (`geometry::cluster_file_hash`, whose clusters are the other half of a palm's bytes); the
  trunk's clusters carry cones and no frond's does; the container records its identity and the
  generator section; the canonical parameters are the same for every spelling of one file;
- [materials](#materials), over a stand-in set the test names and a pack it parses in memory: the
  bark takes the recipe's images as the recipe spelled them, the fronds the pack's role (absolute,
  at the pack's repeat), the skirt nothing and its flat colour; roughness alone in the
  metallic-roughness slot with metallic 0; a transform of `1 / tile_m` on the parts in metres and
  none on a leaf card; a texture record per image; the pack in the build key and not in the source
  hash; the images and transforms through a file and back; a set's numbers out of range refused;
  with no block and no pack, no image at all. And through the renderer: the same palm loaded from
  a scene with a pack directory beside it builds its six images into the cache's textures
  (`flora_scene_tests.cpp`).

**Public API.** `include/domain/flora/palm.h` (the solver: `palm_in_range`, `solve_palm`,
`PalmReport`), `include/domain/flora/trees.h` (the branching species: `broadleaf_in_range`, `conifer_in_range`, `birch_in_range`, `willow_in_range`, `orchard_in_range`, `ancient_in_range`, `solve_broadleaf`, `solve_conifer`, `solve_birch`, `solve_willow`, `solve_orchard`, `solve_ancient`, `TreeReport`, and the allometric crowns' structure: `CrownShoot`, `broadleaf_crown`, `ancient_crown`, `birch_crown`; the birch's `solve_birch_chain` and its seeded stem count, `birch_stems`), `include/domain/flora/understory.h` (the understory: `shrub_in_range`, `fern_in_range`, `tuft_in_range`, `herb_in_range`, `shrub_collides`, `k_shrub_collider_radius_m`, `solve_shrub`, `solve_fern`, `solve_tuft`, `solve_herb`), `include/domain/flora/motion.h` (the motion contract: `Motion` with its `structure`, `MotionPart`,
`k_trunk_part`, `encode_bend_weight`, `decode_bend_weight`, `motion_records`), `include/domain/flora/structure.h` (the structural table: `StructuralPart`, `k_load_bearing`, `k_straight`, `k_collider`, `structural_point`, `structural_radius`, `load_bearing_min_radius`, `Hollow`, `ancient_hollows`), `include/domain/flora/flora.h` (the capability: `k_flora_version`,
`k_species_date_palm`, `k_species_broadleaf`, `k_species_conifer`, `k_species_birch`, `k_species_willow`, `k_species_orchard`, `k_species_ancient`, `k_species_shrub`, `k_species_fern`, `k_species_tuft`, `k_species_herb`, `solve_flora`, `flora_detail_tier`, `flora_collides`, `read_flora_file`, `canonical_flora_json`, `flora_source_hash`,
`flora_build_key` (with the pack), `flora_role`, `apply_flora_materials`, `build_flora_container`
(with the pack, and a branching species' `TreeReport`)). The mesh generator "flora" registers from `src/flora.cpp` and takes the pack from
the host's `assets::MeshGeneratorInputs`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `assets`, `geometry`,
`materials` (the sets), `texture` (the records saying how each image is built), `flora_schemas`.

**Testing.** `tools/dev.ps1 test -Filter flora`.

**Performance notes.** No hot path: the solver runs once per parameter set at content-build time,
in milliseconds for the default palm. What a palm costs to draw is its cluster DAG's.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | this capability stands alone |
| Component and event types | `domain/flora/schemas/flora.schema`: `Palm`, `Broadleaf`, `Conifer`, `Birch`, `Willow`, `Orchard`, `Ancient`, `DetailTier`, `Shrub`, `Fern`, `Tuft`, `Herb`, `MaterialSet`, `FloraMaterials`, `FloraFile` | parameters, not components: a palm has no runtime state |
| Tick scheduler entry | none | the solver runs at content-build time and has no tick |
| Render-graph passes | none | a palm is a cluster mesh like any other |
| Content-build derived step | `engine-content flora`, the mesh generator "flora" | [the build](#the-build): keyed by the canonical parameters, the species and `k_flora_version` |
| Protocol methods | none | not needed |
| Tunables | none | everything the solver reads is a parameter of the file |
| LOD policy | the cluster DAG's | a palm is one mesh; distance is the DAG's |
| Determinism | the same bytes on every toolchain | [above](#determinism) |
| Zero cost when unused | nothing runs unless something asks for a palm | plan 11 §11.10: absent work |
| Tests and size table | `tests/flora_tests.cpp`, `tests/species_tests.cpp`, `tests/allometry_tests.cpp`, `tests/understory_tests.cpp`, `tests/size_table.cpp` | written |
| Bench | none | no hot path |
| Removal proof | `ENGINE_WITH_FLORA`, off in the minimal build | works |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_FLORA=OFF`) drops the
module, its schema types and its tests; the module disappears from `build/<preset>/modules.json`
and is listed there under `disabled_capabilities`. Everything else must still build and pass.
