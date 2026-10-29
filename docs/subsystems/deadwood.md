# deadwood (domain, capability)

**Purpose.** Cook tree remains from an existing species recipe, retaining its geometry and
parameters. The six `RemainKind` values are `fallen`, `snag`, `snapped`, `stump`, `log`, `limb`.
The [grounding page](../experiments/deadwood.md) distinguishes forestry observations from authored
defaults and states every approximation. G101 is implemented and validated; owner visual acceptance
remains separate from the automated checks.

**Owned data.** `engine.deadwood.DeadwoodFile`, its support capsules and the pure solver. It imports
`engine.flora.FloraFile` and `RootGround` rather than creating another species/terrain vocabulary.
The source tree is never modified. A `.deadwood.json` registers the `deadwood` mesh generator.

## Recipe and pose

```json
{
  "tree": {"species": "oak", "tree": {"seed": 17, "height_m": 12}},
  "kind": "fallen", "seed": 41,
  "pitch_deg": 90, "roll_deg": 25, "yaw_deg": 10,
  "branches": 1, "foliage": 0.6, "bark": 0.8
}
```

`tree` is the complete existing flora recipe, including its species-specific legacy blocks,
maturity controls, seed and material sets. `branches` and `foliage` are independent 0–1 retention
controls; omitted/-1 selects the kind's default. Complete connected leaf units are retained or
removed: individual blades in modern trees, connected sprays in older representations. A missing
branch drops its descendants. `source_part` selects the root of a detached limb; a bad part is
refused. Part IDs come from the source recipe and can change when that recipe changes.

`start_m`/`end_m` select an axial interval. Snapped stems and stumps expose an irregular cross-section;
logs expose both ends when a positive start is given. `splinter_m=0` is a flat cut. The source
mesh's tessellation bounds fracture detail. Bark loss assigns coherent UV patches to exposed
wood; it does not model separate bark thickness. `hollow` adds an inward wall and annular end
rims for bole-only logs/stumps. `collapse` compresses a horizontal log and opens a hollow roof.

The cook applies roll about the source axis, pitch about x, then yaw about y. Detached limbs
first align their selected source axis to y. **The resulting mesh already has that pose.** Place it
at the site's origin without applying its angles again. Translation is a normal scene `WorldPos`;
no absolute world coordinates enter source geometry. For a differently tilted or supported copy,
cook a new recipe so its underside damage follows that pose. Yaw-only reuse on identical flat
terrain is possible when support data rotates with it; arbitrary terrain requires a new cook.

`ground` is an optional local heightfield in the posed frame; absent means `ground_y_m`'s plane.
The cook does not extrapolate. `settle` puts the retained bole against that ground; `lift_m` then
adds height, or is the origin height when settling is disabled. Underlying branches bend in a
bounded quadratic shape and are clipped before contacts they cannot clear. Their children inherit
the parent's fitted displacement. A newly clipped fracture can still intersect even when the
original vertices clear; that branch is then removed at its attachment with its descendants.
Ground/support penetration remaining in the bole or finished caps is an error.

`supports` supplies capsules for other tree trunks or limbs in the same local site coordinates.
`require_support` additionally rejects a pose that touches none. This validates authored leaning
poses; it does not solve the lean angle, discover a scene tree or simulate a fall. Wood surface
samples are vertices and edge midpoints; foliage units are rejected by vertex contact. Narrow
obstacles require sufficient source tessellation.

## Geometry, materials and collision

The cook clips source triangles and closes boundary loops with exposed wood. A hollow instead
duplicates inward-facing wood and joins the boundary rims. Original overlapping tree joins remain
overlapping; this is not a watertight Boolean union of the entire source tree. New normals and UV
islands cover cut faces. Bark/foliage retain source UVs; inner wood and caps use metre-based UVs.

`tree.materials.bark` and `leaf`, and the source species' pack roles, are reused. The existing
`tree.materials.wood` slot now supplies fracture, exposed and inner wood here; the fallback role is
`<species>/wood`, then a declared flat stand-in. No texture is generated or copied into the repo.

Each material builds its own normal cluster DAG, with cones only for one-sided surfaces. Paging,
the 16-bit grid, normals/cones and texture records follow the standard geometry/material path.
The container keeps two parentless motion parts with zero weights: wood is a collider, foliage is
not. It keeps the source catalogue species to use existing eligibility rules. The rendered pose
is fixed, though the existing renderer still allocates two pose records and treats it as flora;
eliminating that cost requires a shared rigid-collider motion policy and is not silently assumed.
Collision uses the normal welded DAG cut at 5 cm. A cover host sees wood, not leaves; the cover
capability produces its own section-40 mask when a companion is cooked over the remains.

## Budgets and distance

The source tree selects its own detail through its existing recipe/profile budget. The remains
recipe additionally caps **all emitted source triangles**, including caps and inner walls. Initial
defaults: stumps 8,000; logs 20,000; detached limbs 60,000; snags 100,000; full falls/snaps the source
count plus 16,384 closure triangles. Explicit caps are 128–4,000,000. These are provisional and
adjustable. Exceeding one fails with the count; it never silently removes a structural branch to
meet a graphics budget. Reduce source detail or increase the explicit cap.

Near and far geometry use the DAG's projected-error cut. A consumer can use
`geometry::extract_lod_form` for ring assets. No new far impostor is claimed: using the living
tree's canopy bake after damage would resurrect missing branches/leaves. Fresh leafy falls retain
the generic foliage cut and need a damage-specific bake if large distant populations require it.
The 6 m fresh-oak fixture has 10,586 triangles, 561 DAG clusters and nine levels. At 20 m/960x640,
its normal 1 px cut draws 198 clusters; coarse-versus-finest FLIP is .000487 at 20 m and .001269
at 60 m. This is sufficient for the fixture, not a large-population impostor claim. See the
[measurement table](../experiments/deadwood.md#validation-and-measurements).
All cascades and traced shadows follow the source species policy. Pair/shadow acceptance belongs
to scene measurements, not a source-triangle assertion.

## Public API and invariants

`domain/deadwood/deadwood.h`: `read_file`, `in_range`, `canonical_json`, `source_hash`, `solve`,
`build_container`, `Report`, `k_deadwood_version`. Schema comments carry field units/ranges.
`engine-content build asset.deadwood.json out.clusters` and scene mesh paths use the registry.

- Same canonical recipe gives the same mesh/container across supported toolchains.
- Damage never edits source geometry or moves the living tree's hash pins.
- Pose, terrain, supports, source version and damage version participate in cache identity.
- A failed solve leaves mesh and motion empty; bad input, uncovered ground and insufficient
  budgets are named refusals.
- Foliage is optional independently of branch retention; zero means zero foliage triangles.
- All weights are zero, so contact damage cannot drift under runtime wind.

## Capability contract

Scaffolded by `tools/new-capability.ps1`; optional `ENGINE_WITH_DEADWOOD`, requiring optional flora
through `engine_capability_requires`. No core, foundation, scheduler, render pass or other solver
implementation changes. Hosts link the registry module conditionally. The schema library imports
flora through a local schemac command, generating only deadwood types. Public reads from flora are
`solve_flora`, recipe validation/canonicalization, material application and motion-record mapping.
No tick, frame allocation, socket, model invocation or external service. Unused means no cook.

## Testing and limitations

`tools/dev.ps1 test -Filter deadwood` exercises all forms, contact, independent leaf/branch loss,
hollows, collapse, deterministic repetition, source preservation, validation, cache identity and
container/host eligibility. Required handover runs the affected Windows set and matching Linux
Clang container; source hash pins and capture results live on the grounding page.

Not yet: dynamic falls, decay over time, species-calibrated deadwood mechanics, automatic leaning
pose search, root plates/soil clods, loose bark strips, subtriangle collision guarantees, bespoke
damaged-tree impostors, and automatic POC placement. The POC repository is read-only to this work.
