# Wetland plants (domain)

Wetland builds small stands of sedges, rushes, reeds, cattails, arrowheads and two
floating-leaf forms, including dead and broken shoots. It owns plant recipes,
patch distributions and their geometry. This is a static content solver;
water elevation and ecological bands are authored inputs.

The [grounding record](../experiments/wetland-plants.md) gives botanical sources,
dimensional envelopes, authored choices and budget trades. Folded blades,
tubular culms, branched reed panicles and notched floating leaves retain their
silhouettes without changing existing flora outputs.

## Model and recipes

[Example recipes](../../content/wetland/README.md) use `engine.wetland.Plant`
from `domain/wetland/schemas/wetland.schema`. Names, budgets and motion traits
live in geometry's flora catalogue; the wetland profile owns dimensions/counts.

| Kind | Reference form | Shoots | Default triangle budget |
|---|---|---:|---:|
| sedge | Carex stricta, spreading | 48 | 6000 |
| tussock | Carex stricta, raised crown | 64 | 8000 |
| rush | Juncus effusus, lateral heads | 40 | 6000 |
| reed | Phragmites australis, alternating blades | 8 | 8000 |
| cattail | Typha latifolia, basal straps | 8 | 8000 |
| arrowhead | Sagittaria latifolia, sagittate blades | 8 | 4000 |
| water_lily | Nymphaea odorata, round floating blades | 6 | 2000 |
| pond_lily | Nuphar variegata, elongated blades | 6 | 2000 |

Zero `shoots` or `triangle_budget` selects the profile. Dead shoots retain
parts, become brown, and a separately seeded fraction breaks shorter.
`spread` changes root spread, not leaf size. Floating blades lie at
`water_depth_m + .002` above the anchor. All recipes have `collider: false`;
true is refused because these flexible/floating forms are not walking barriers.

## Geometry, materials and distance ladder

Budgets include source geometry plus six far-card triangles. Detail rungs use
6/4/3 tube sides and 6/4/2 ribbon rows; sedge culms always have three sides.
Lily fans use 24/16/10 sectors. Shoot counts and motion parts never shrink.
An impossible floor is an error naming the required count.

The cluster builder caps generic simplification at one micrometre, then carries
the full source cut until one authored six-triangle level: three orthogonal views of the complete stand. Flora's CPU
baker produces a 256-square colour/alpha atlas and normal atlas, plus roughness
when supplied by a pack. One-texel borders protect atlas sampling. Transitions
carry measured coverage difference floored at half-extent (roughly ten pixels
at the default foliage threshold), and geometric half-extent plus half-texel
error; both material DAGs share a transition sphere. Distance forms use the
ordinary `geometry::extract_lod_form` contract.

Materials resolve `<species>/dead` and `<species>/leaf` pack roles.
Leaves use 0..1 UVs; tubes map circumference/length to one repeat; crowns use
angular/vertical repeats. Fallback colours are brown/green, roughness .7,
nonmetallic and double sided. Pack colour, normal and roughness pixels are
baked and included in source identity. A pack naming images requires host
image reads; missing inputs are errors. Generated bitmaps are never committed.

Normal pages and the 16-bit mesh grid apply. Double-sided geometry has no
normal cones. Plants are not attachment hosts and carry no cover-host mask.

## Motion and collision

Emergents reuse field motion: one parentless part per shoot, plus static crown
and far-card parts. Weights rise from roots; reed leaves inherit stem station.
Every structural row is flagless. Catalogue compliance is .12 for sedge/tussock
and .08 for other emergents: authored visual responses, not measured moduli.
Floating plants use `none` and zero weights; far cards also have zero weights.

Every kind casts only in the nearest sun cascade, from rest pose, and is absent
from traced shadows. Field motion has no CPU pose array per instance; the existing
deformation pool pays for the visible cut. Field/none policy and structural rows
make the plants noncolliding without runtime special cases.

## Patch placement

The `wetland` placement registry reads `engine.wetland.Patch`.
A jittered finite lattice is clipped to an ellipse with smooth seeded boundary
recession and interior gaps. This is authored distribution, not fitted ecology.
Water-minus-ground depth, slope (central difference over .5 m), probability and
earlier footprints plus clearance gate candidates. Optional `mask_layer`
multiplies occupancy by cooked palette weight. Requested missing/invalid masks
and non-finite ground are errors; outside mask extent is empty. A tile read failure
inside the mask extent is an error.

Positions remain f64, with integer-millimetre ground queries. Random decisions
use local coordinates, retaining detail far from the origin. IDs enumerate
candidate rows and are emitted in increasing order. Tiles visit overlapping
rows only. Request order changes nothing. Optional matching `far_meshes`
swap mesh index at ring 1 onward, preserving IDs and transforms; otherwise
the normal container DAG still coarsens.

Floating anchors are `water_y_m - floating_depth_m`; the depth must match
their mesh recipes. Eligibility still uses actual ground. Stalks do not fit
bathymetry. Emergent anchors use ground. The window must fit
`candidate_budget` (default 100000, maximum one million); excess is refused,
never truncated. Hosts also enforce the scene pair cap. Authors must size
scenes and cook distance forms accordingly.

## Invariants and public API

`wetland.h` exposes profiles, validation/canonicalisation, `solve`, and
`build_container`. Validation precedes heavy allocations. Failed builds clear
geometry. Output changes bump `k_wetland_version`, currently 4.

Tests cover grounded dimensions, finite nondegenerate geometry, budgets and
stable parts, container handover and motion, deterministic section identities,
and tile/whole agreement. Profile is 28 bytes and Report 24 bytes, pinned in the
size table. Placement uses the existing 64-byte contract.

Run `tools/dev.ps1 test -Filter wetland` for focused checks and
`tools/dev.ps1 test -Affected` before handover. The
[study scene](../../content/test-scenes/wetland/README.md) shows each form and
patch edges; the grounding record holds measurements and capture review.

## Capability contract (ADR-0027)

| Registration point | Contract |
|---|---|
| Removal | `ENGINE_WITH_WETLAND=OFF`, minimal, or flora-off |
| Dependency | `engine_capability_requires(wetland flora)` |
| Mesh generator | `.wetland.json`, canonical recipe/image hash |
| Placement generator | `wetland`, whole and streamed tiles |
| Schema | `engine.wetland.Plant`, `engine.wetland.Patch` |
| Derived content | Generic mesh build and distance forms |
| Shared flora API | Public companion shapes, motion records, CPU baker |
| Runtime passes, scheduler, protocol, tunables | None |
| Zero cost unused | No tick or scene state without a recipe/entry |

Hosts link conditionally. No core, foundation, renderer, scheduler or existing
flora implementation changes. Catalogue IDs append, preserving earlier values.

## Not yet

No flowers, individual seeds, rhizomes, seasons, hydrology, wave following,
bed-fitting, biomass calibration or measured elasticity. Mortality and density
are author controls. Coarse geometry uses three planes rather than a multi-angle
depth impostor. Artistic acceptance is the owner's decision after captures.

Dead material is opaque (including its pack role); live leaf material may use image alpha. This matches the baker's bark/leaf contract, so a dead-material alpha channel cannot create a different near/far silhouette.
