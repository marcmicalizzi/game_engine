# litter (domain, capability)

Static forest-floor litter: curled white-oak leaves, white-pine needle beds and cones, bark
flakes, thin wood fragments and small branch tangles. The research, chosen values and explicit
approximations are in [forest litter](../experiments/forest-litter.md), roadmap G103.

## Owned data and model

`engine.litter.Recipe` and `Layer` live in `domain/litter/schemas/litter.schema`. A
`<name>.litter.json` is a registered generated mesh, built through `engine-content build` and
normal scene loading. No runtime system, scheduler entry or render pass is added.

A patch hashes each particle independently, distributes its centre inside a disc, and raises
it within a quadratic pile profile. Leaves are lobed sheets bent along and across their midrib.
Needles are tapered strips. Cones are overlapping unarmed scales around an elongated axis.
Bark has two skins and closed edges. Shapes are static, with no falling-leaf simulation.
All particles keep their physical size as source detail changes.

Small wood is **the existing deadwood geometry**. `wood_recipe` returns the exact source recipe
passed to `deadwood::solve`: a cut thin oak stem for twig fragments, several small leafless falls
for a tangle. It uses the same source growth, cut faces, fracture relief and ground seating.
Litter only centres and yaws the result. The recipe can also be built as a standalone deadwood
asset. This is a small-stem analogue; it does not claim a measured distal-twig distribution.
Living-tree and deadwood outputs are unchanged.

## Budget and distance

| Kind | Default particles | Source budget | Detail ladder |
|---|---:|---:|---|
| leaf | 64 | 4096 | 12, 6, 3 longitudinal segments, two strips |
| needles | 400 | 4800 | 3, 2, 1 segments |
| cone | 5 | 4096 | 8, 6, 4 rows of eight scales |
| bark | 20 | 2048 | 12, 6, 3 segments, skins and edges |
| twig | 7 | 12000 | source deadwood geometry |
| tangle | 4 | 24000 | source deadwood geometry |

An explicit count is never silently reduced. The finest rung fitting the full patch is used;
if none fits, the solve fails with `triangle_budget`. The standard cluster DAG provides runtime
LOD, and the world's existing `extract_lod_form` path bounds distant pairs. Needle beds add one
authored terminal level: a chord quad per needle, at .5 mm error (the maximum deviation of the
2 mm quadratic bend from its chord). This retains coverage that ordinary decimation erased;
the default bed therefore retains 800 triangles at distance. There is no bespoke far card or
ground-texture replacement in this version. Near/mid/far cuts require capture review;
the source budget alone does not prove a scene fits the pair or frame budgets.

The shared geometry catalogue has one `litter` entry: rigid (`none` motion), zero compliance,
no traced shadows and only the nearest sun cascade, in the rest pose. One flag-controlled
structural record expresses `collider` (default false), with zero bend weights. Fine litter
therefore does not trip a walker; opting in uses the normal welded 5 cm DAG proxy. Nothing
adds per-frame state or allocations. No other catalogue entry changes.

## Placement

A scene entry `{"generator":"litter","params":{...}}` supplies mesh recipe paths, an inclusive
tile window, cell edge and probability controls. Cells divide the host tile. Jitter stays inside
each cell; `extent_m` must enclose every supplied mesh and be at most .35 cell. It is an authoring
contract, not a radius measured by this generator. At most one patch per cell leaves exposed
soil even at probability one. The renderer retains its normal pair-cap validation.

The acceptance probability is background plus a depression term (four neighbouring heights
minus centre) and a linear band outside earlier obstacle circles. Inside an obstacle's radius
plus patch extent, placement is excluded. An optional W72 substrate palette weight multiplies
the probability; requesting masks when none exist fails. Mask read failures are errors, not
empty patches. Steep ground is omitted by the explicit `max_slope` policy. The accepted patch
is tilted to the standing ground's tangent plane. This approximates curvature and can penetrate
strongly curved ground; use smaller authored patches there.

Ground reads use whole millimetres; positions are f64. IDs are nonzero row-major cell ordinals
in the entry window, checked against overflow before work. Seed, identity and ground determine
each tile independently of request order. Earlier footprints are restricted to anchors in the
same tile during both whole and streamed reads; neighbouring overhangs are not inferred. Meshes
remain resident while the host supplies their normal distance forms. `max_cells` limits work
per tile and rejects an excessive lattice at open, rather than dropping its tail.
`max_total_cells` (1048576 by default) also bounds a whole read before expansion.

## Materials and invariants

Pack roles: `litter/leaf`, `litter/needles`, `litter/cone`, `litter/bark`, `litter/wood` for both
wood kinds. No pack gives an explicitly authored dry brown, nonmetallic, rough surface.
Leaves and needles use continuous local 0..1 UVs; bark backs, cores and scales use a repeated
local detail layout; wood preserves its source UVs. These are
tiled detail maps, not a whole-leaf image layout. No binary texture is committed. The standard
material/texture pipeline handles the pack, mips and compression. Thin sheets are two-sided,
with cones disabled; wood/bark remain one-sided. Positions are quantized and cones fitted by
the normal cluster builder after all mesh edits.

A successful solve is finite, nonempty, within budget and above its local ground plane. Failure
leaves mesh/container output empty. The cache key includes canonical recipe, litter version,
and for wood the full deadwood source identity (including flora version); packs enter the
normal generated build key. No decision uses timestamps or libc trigonometry.

## Public API and tests

`domain/litter/litter.h`: `Profile`, `Report`, `profile`, `in_range`, `read_file`, `canonical_json`,
`source_hash`, `wood_recipe`, `solve`, `build_container`, `k_litter_version`.
`tools/dev.ps1 test -Filter litter` tests dimensions against source ranges, all kinds, budget
refusal, source reuse, determinism, schema rejection, tile order, gaps, hollows and obstacles.
`engine_content` also cooks a needle recipe through the executable, reads the written container
and checks its 800-triangle terminal form. The measured near/mid/far cuts and memory figures
are in the [grounding and validation page](../experiments/forest-litter.md#validation-and-measurements-2026-10-29).
The cold report/profile are pinned at 16 bytes each. No new hot type.

[Recipes](../../content/litter/README.md) and [study scenes](../../content/test-scenes/forest-litter/README.md)
are text fixtures. Outputs/cameras/captures belong under the external local root. The forest POC
is read-only; use a derived external scene to review integration.

## Capability contract

Scaffolded with `new-capability.ps1`. `ENGINE_WITH_LITTER` is optional; it requires `deadwood`
(and transitively flora), explicitly declared in the capability graph. Dependencies: base,
containers, math, hash, json, schema, assets, geometry, materials, texture, scene_gen, deadwood
and litter_schemas. The only read of another solver is its public `deadwood::solve` and identity
API. Hosts link litter conditionally. Registration points are the mesh and placement registries
and schema descriptions. No protocol method or tunable: all controls are recipe data.
Determinism is derived and cross-toolchain. With the capability disabled, no cook or placement
runs; minimal configurations omit it. The shared inert catalogue row holds no runtime state.

## Not yet

Measured ecological deposition/decay, wind-driven redistribution, rainfall transport, species
mixes inferred from the canopy, deformable piles, arbitrary-surface contact, damaged-bark
species profiles, scanned default materials and a bespoke far appearance. Owner visual
acceptance is separate from automated correctness.
