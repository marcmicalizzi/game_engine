# soil (domain, capability)

Static eroded landforms: short scarps, gullies, undercut banks and spruce-reference
root plates with their pit and mound. The input is an `engine.soil.SoilFile`
(`*.soil.json`); output is ordinary cluster geometry plus an authored heightfield
cook. [Research and numeric choices](../experiments/soil-banks-tree-throw.md).

## Owned data and model

A recipe owns its dimensions, seed, source triangle budget and original local ground
samples. The cook is explicit: a moved feature or modified ground requires a new cook.
No soil erosion, windthrow mechanics or simulation state runs per frame.

A bank is a closed extrusion along x. Its top joins original ground at the back;
its front exposes organic, topsoil, subsoil and parent-material bands. The face
recedes beneath an undercut lip. Ends fade into undisturbed ground. A gully has
opposed faces and a depressed floor. The native heightfield is excavated beneath
the bank volume so it cannot fill the undercut cavity with a sloping terrain face.
The collider for the lip is the mesh's normal 5 cm DAG proxy, and the lower floor
is the native terrain collider. There is no new collision or renderer path.

Tree throw excavates an elliptical pit and deposits an adjacent smooth mound in the
actual heightfield. The upright soil plate is an irregular rock-solver body with
soil materials, rigid branching root spokes and hanging broken tips. A short fallen
bole continues behind it. These are morphology approximations, not a conserved
soil-volume reconstruction or a particular tree's uprooted root network.

Embedded stones call `rocks::solve_rock`; the soil capability declares its dependency.
The implementation pins rock solver version 8 at compile time. A rock-version change requires reviewing soil captures/pins and bumping `k_soil_version` before updating that assertion, so cached soil cannot silently retain old stones or plates. All woody roots here are rigid. Existing living-tree roots can be cooked against the
resulting terrain through their usual `scene_gen::Ground` seam; this does not move
existing trees or alter their root recipes automatically.

## Cook and scene workflow

`engine-content soil-cook <recipe.soil.json> <heightfield.json> <new-out-dir> <x> <y> <z>`
reads the original heightfield and writes a new ground header/grid, the fitted recipe,
provenance and a scene placing both halves. It refuses an existing output directory
and output inside a repository. All source data is left intact. The source grid must
have 25–250 mm spacing and cover the whole feature plus its apron. The feature is
axis-aligned at the given f64 origin; rotating, scaling or fitting its mesh separately
breaks the terrain match. The scene omits mesh fitting, preserving the solver frame.
The scene names a local `derived` cache. A source offset from the origin, a nonsquare source, or a source larger than the finite grid limit (4097 points), uses the existing world ground path (8 m tiles, 64/32/16 cells across three rings); that review scene requires the world capability. A centred square source uses the finite grid. The output scene is written last. A write error can leave an incomplete directory;
choose a new directory on retry.

The public `cook` is transactional in memory. `sample_ground` reads integer-millimetre
queries through `scene_gen::Ground`, subtracts the f64 anchor, and stores local f32
heights. The source key includes those samples and the solver version. Only terrain
points in the feature footprint change. Successive overlapping cooks are ordered
operations; they are not commutative. The scene can use the existing authored
placement streamer; this module adds no random regional placement generator.

## Budgets, LOD, materials and collision

Defaults: scarp 12,000, gully and undercut 16,000, tree throw 24,000 source triangles.
Explicit budgets are 2,000–200,000. The solver tries longitudinal cells of 0.08, 0.125,
0.2, 0.35 and 0.6 m, eight then four root sides, and 5,000 then 1,200 plate triangles.
Every root and stone survives; a budget too small for the coarsest form is refused.
Each stone has a 500-triangle source allowance. Terrain triangles belong to the
existing terrain budgets and are additional to this mesh budget.

Runtime LOD is the cluster DAG per material. Distance forms use the existing
`geometry::extract_lod_form`; these small opaque features need no foliage card chain.
Every triangle is rigid and collidable; no motion contract, alpha mask, extra shadow
pass or runtime allocation is added. The caster cost is the ordinary visible cut.

Roles: `soil/litter`, `soil/topsoil`, `soil/subsoil`, `soil/parent`, `wood/root`,
`rock/granite`. Packs use `materials::resolve_set` and `apply_set`; absent roles draw
explicit flat PBR fallback colours. UVs are local dominant-axis projection in metres (one UV unit per metre). Micrograins belong in material maps.
The plate and bank are ordinary hosts for the cover generator; a cover companion
owns section 40's mask, as for rocks. This solver does not invent moss coverage.

## Invariants and public API

`domain/soil/soil.h`: `in_range`, `read_file`, `canonical_json`, `source_hash`,
`triangle_budget`, `ground_height`, `terrain_delta`, `sample_ground`, `cook`, `solve`,
`build_container`, and the 20-byte cold `Report`. Invalid recipes name the field.
The IDL documents units and bounds; unknown JSON fields fail schema parsing.
Output uses deterministic hash streams and `det::sin/cos`, with contraction off.
The mesh registry and engine-content share one container builder and build key.

## Testing

`tools/dev.ps1 test -Filter soil`: source dimensions, all four kinds, budget refusal,
finite geometry, ground edits, unchanged data on refusal, distant ground samples,
registry and deterministic output. MSVC and Linux Clang 18 reproduce all 34 container sections for each kind and the cooked terrain hashes. Shell tests check directed edge pairing after exact welding. The engine-content test checks cook, refusal and container build. The authored studies in
[soil-features](../../content/test-scenes/soil-features/README.md) explain the captures.

## Capability contract (ADR-0027)

`ENGINE_WITH_SOIL`; `engine_module(... OPTIONAL WHOLE_ARCHIVE)`;
`engine_capability_requires(soil rocks tileset)`. Registrations: own schema library,
mesh generator `soil`, and host-level `soil-cook`. No scheduler, render pass,
protocol method or tunable. Unused: no mesh, cook, runtime state or frame work.
Only hosts link the capability. Turning it off removes the generator and its tests,
and the command reports that it needs the capability.

## Not yet

Process simulation, physically conserved excavated volume, fallen full-tree crowns,
automatic uprooting of living-tree assets, regional bank detection, moving terrain,
root wind motion, native terrain material-mask painting and automatic grass clearing.
The cook is a finite, explicit authored feature. Visual acceptance remains the owner's.

The far-origin study preserves the cook and geometry, but exposed a CSM fitting issue (F24): the shifted world review has no cascade casters. See the grounding page's far-origin evidence; the renderer is unchanged here.
