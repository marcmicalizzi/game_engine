# The static-asset world: what imported assets get from the features built for generated ones

Roadmap T130, 2026-11-14. The owner, 2026-11-13: "we're still building a general purpose high
performance high fidelity game engine ... a consumer of the engine may not want to use any of [the
solvers], for example, they may want to use entirely static assets, and that should work just as
well." The minimal build proves nothing *links* a solver; nothing proved that a world of imported
meshes and hand placement *draws as well* as a generated one, because every scene the gate flew was
a generator's and the features built for the forest were only ever looked at on the forest. This page
is the scene that does, and the audit it was built for.

## The scene

[`content/test-scenes/static-world/`](../../content/test-scenes/static-world/README.md): a 600 m
ground **mesh**, seven trees, ten shrubs and forty grass clumps of alpha-tested branch cards, six
boulders, two Khronos Lanterns and the FlightHelmet, every one an instance written in the file at a
height written in the file. No `terrain`, no `scatters`, no `placements`, no `world`, no `sky`, no
water. The kit is plain glTF 2.0 written by `engine-content static-kit` (the pinned Khronos set has
nothing alpha-tested worth the name), carrying nothing a generator adds; the tree is shaped as a
bought tree is — a trunk, six limbs and 117 cards of 1.8 m by 1.3 m, three crossed planes at each of
39 points, each card a twig of 18 leaves cut by its alpha, **31.5 % of the card's texels at or over
the cut**. It is flown at every gate (`static-1080p-rt` in `content/budgets/frame-budgets.json`) and
end to end through the four rasterizers in every configuration, `msvc-minimal` included
(`apps/engine_view/tests/static_world_view_tests.cpp`).

**It loads and draws in every mode; nothing in the renderer had to change.** No crash and no refusal
stood in its way. What it showed is a list of features that do nothing for it, below.

## The audit

One row per feature of the renderer, `gfx` and `geometry` that was added for a generated world (the
three the coordinator read on 2026-11-13 first, then the rest from [renderer](../subsystems/renderer.md)'s
sections and `systems/renderer/src/settings.cpp`'s tunables). "This scene gets" is from its own
load's log lines and summary and from the captures; "costs here" is GPU milliseconds measured on it
with the feature on against off (the measurements are [below](#what-it-costs-with-nothing-to-do));
**a constant that is a solver's default is named in bold.**

| Feature | What it keys on | What this scene gets | What a generated scene gets | Costs here with nothing to do | What would make it work for an imported asset |
|---|---|---|---|---|---|
| **The crown prior** (R69 stage 7; `systems/renderer/src/crown_priors.cpp`, `domain/gfx/include/domain/gfx/crown_prior.h`) | alpha-tested triangles of a mesh whose mean area is under `k_crown_card_max_m2` = **0.05 m²** (the solver's leaf cards; a far form's merged cards are larger), in a crown at least `k_crown_min_height_m` 1.5 m tall; leaf area = triangles × `k_crown_card_fill` = **0.7, the tree solver's default broadleaf `leaf_width`** | **nothing**: the tree's cards average 1.17 m² a triangle, so it has no prior; its leaves are lit by the cache as walls are | a 16-cell transmittance grid per tree form (Monsi–Saeki, G = 0.5), a foliage pixel lit through its own crown's shade | see the table below (`crown_prior` on against off) | leaf area from the card's **own alpha coverage**, which the shadow proxy's `alpha_coverage` bake already measures per cluster at load; the area cap dropped for a coverage-weighted area (a merged far card's coverage is its own too); the fill measured, never assumed: this card's is 0.315, so the solver's 0.7 would have counted 2.2 times its leaf |
| **The bounce's cache for foliage** (stage 8: a foliage cell remembers 64 frames, a crown's hit reads its crown's level) | `crown > 0`, i.e. the crown prior | nothing: the cards are surfaces with faces in the cache | foliage cells of no face, a longer memory, a hit on foliage read through its crown | inside the prior's row | follows from the crown prior's row |
| **Sway** (S66, the deformed-vertex pool's flora stage; `deform.slang` `flora_vertex`) | the container's motion contract, sections 35–37 (`motion_parts`, `motion_vertices`, `structural_parts`), which only the flora, cover and deadwood generators write; stiffness from the **closed species list** `geometry::flora_species_info` (`domain/geometry/flora_species.h`) | **rigid**, and silently: `a tree drawn rigid` is written only for a mesh that has a contract the stage cannot read, so an imported tree with none gets no line at all | parts bent by the frame's wind and gusts and pushed by the walker, posed on the CPU per part | 0 (no deformed instance: `deform` is not timed) | **a motion table an imported mesh can carry**: per-vertex part and bend weight (a glTF `extras` block or a sidecar named by the scene's `Mesh`, or derived at build from height above the origin and distance from the trunk's axis), parts from the glTF node tree, and stiffness, natural frequency and damping as numbers on the mesh rather than a species name looked up in a list |
| **Grass's field kind** (R95, "The field kind") | the species catalogue's `field` kind, through the same contract | rigid grass | blades posed by the pool pass alone from the wind field | 0 | the same table with a `field` kind and its stiffness as data |
| **The far field at rest** (R152) | the field kind | nothing (no field) | sward past 120 m stands, takes no pool block, references resident structures | 0 | nothing until grass can sway |
| **The caster policy** (R95) | the species the container's generator section names (`geometry::FloraCasterPolicy`) | the trees' defaults: every caster traced and in every cascade | per species: untraced, fewer cascades, rest pose in the cascades | 0 | the policy as fields of the scene's `Mesh` |
| **Seating by footprint** (G201) | a container's seat, section 42 (flora and deadwood), and the scene's terrain | nothing: no seat and no terrain | a tree lowered until its skirt is under the lowest ground its rim stands over | 0 | a seat measured at build from any mesh's own lowest ring; and a ground the reader can query (next row) |
| **Instances off the ground** (T74) | a `terrain` block: `read_scene_file` compares each `ground: false` instance with `ground_at_world` | **nothing**: a ground that is a mesh is not a ground to the reader, so `load.instances_floating` and `instances_buried` read 0 whatever the placements are | one warning a scene with counts and the worst offender | 0 | a scene `Mesh` marked as the ground, queried by a ray down through its clusters |
| **The impostor chain** (R89) and **the far forest** (R111) | section 38, the flora baker's branch-group cards and whole-tree star; the far forest also needs a streamed world and prefers that chain | the DAG's own simplification of the cards: a far tree is its coarsest collapsed cards, and nothing past a streamed world's ring | baked atlases, a star at a distance, the far forest past the ring | 0 | an impostor bake in the content build for **any** alpha-tested mesh (the flora baker's atlas pass over arbitrary geometry), keyed on the mesh being masked, not on a generator having made it |
| **The foliage threshold and the cross-fade** (R91) | section 39, `lod_coverage`, the coverage errors the flora baker measures between its levels; the cross-fade runs only where a coverage error is finite (`cluster_cull.slang`, `own_fading`) | geometric LOD only, and **every card switch pops**: no coverage, so no fade | a card judged by its coverage at the pixel, switched under a dithered 10 % band | `lod.fade` 0 against 0.1: the table below | coverage errors measured by the content build for any masked mesh's levels (render each level's mask and difference them, as the flora baker does for its own) |
| **Occlusion-aware LOD** (R118) | the coverage threshold alone by default (`renderer.lod.occluded_foliage_only`), and off by default (`occluded_scale` 0) | nothing even when on: the cards have no coverage to move | a crown behind nearer leaves drawn coarser | `occluded_scale` 4 against 0: the table below | follows from the coverage errors |
| **The opaque shadow proxy** (R109) | a masked cluster's alpha coverage, **measured on the GPU from its own mask at load** (`GpuScene::bake_shadow_coverage`) | works as for a generated tree (off by default: `opaque_coverage` 0) | the same | `opaque_coverage` 0.5 against 0: the table below | nothing: this is the pattern the rows above should follow |
| **Opacity micromaps** (R120) | any alpha-tested triangle under traced shadows | **works**: `opacity micromaps baked triangles=358` (every card and grass triangle) | the same | `shadow.micromaps` off against on: the table below (what they buy here) | nothing |
| **The resolve by pixel class** (R151) | `scene.has_masked()` | works (and is off by default) | the same | not measured | nothing |
| **The sun's masks by region** (R139) | the receiver's distance from the eye | works | the same | not separated | nothing |
| **Stochastic mesh materials** (R131) | a material's sampling word (section 33) set to stochastic, which only the generators' recipes write (cliffs, caves, channels, bridges) | **the 600 m ground repeats its 4 m texture 150 times**, plainly tiled in every picture; a glTF material has no way to ask for the stochastic sampler | a large face that does not repeat | 0 | a per-material stochastic flag a glTF can carry (an `extras` field, or the scene's `Mesh` naming it per material) |
| **The ground's materials** (native terrain materials, by place, formation bands, relief, the sand close up, the far ground and its rings) | a `terrain` block | the ground mesh's one glTF texture | layered, placed, distance-blended ground | 0 (no terrain pass is timed: `terrain_upload` 0) | out of this row's reach: a mesh ground is a mesh; its author paints it |
| **Deposits and the wet film** (G190/G191, S102) | the rocks generators' placements; the water bodies' shore over the terrain | nothing | debris at a cliff's foot, a wet band along a shore | `water.wet_film` false against true: the table below | not a feature an imported asset needs; the film would need the next row |
| **The water's shore and depth** (S97–S102) | the body's depth map, built from `scene_gen::Ground`, the scene's terrain | not exercised: a body over a mesh ground would find no bottom (the ground the map reads is the terrain's) | a depth map, shoaling, swash, foam | — | the same queryable mesh ground as the off-ground row |
| **Static structures for rigid forms** (R94) and **the resident cluster structure cache** | forms are a streamed world's distance forms; the cache is any rigid mesh | the cache, yes (`cluster structure cache ready meshes=7`); forms, no (nothing is streamed) | both | inside `rt` | nothing |
| **The sky** (ADR-0048) | a `sky` block and the sky capability | the stand-in sky and sun, as a scene that names none always had | the earth's air, sun, moon and stars at an hour | 0 | nothing generator-keyed: an imported world may name the earth sky like any other |

**What the table says.** The features that measure something from the asset itself — the micromaps,
the shadow proxy's coverage, the resolve's classes, the structure cache — work for an imported asset
exactly as for a generated one. The ones that read something a generator wrote — a motion table, a
species, a seat, an impostor chain, coverage errors, a sampling word — do nothing for it, and two of
them do nothing *silently* (an imported tree is rigid with no line said; a mesh ground is never
checked for floating instances). The crown prior is the third kind: it measures the asset, but with
a solver's constants (0.05 m², 0.7), so it refuses the cards every bought tree has.

## What it costs with nothing to do

Measured on the static world itself, `msvc-release` of this branch, RTX 5090, the committed camera
path flown once (1,441 frames at its 60 fps) after 60 warm-up frames, 3840 × 2160, `--raster rt
--shadows rt --tunable renderer.gi.mode=bounce` (the owner's performance target), `ENGINE_DEFAULT_MATERIALS=0`
as the gate flies. Each row is one flight, with the baseline flown four times between them so the
pair is read against the nearest baseline.

(the table follows from the flights)
