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

**Exclusion (G135).** A cell whose centre is over a terrain hole, in a stream's bed or banks, or under water is skipped ([scene_gen](scene_gen.md#exclusion)): litter on a bed or under a lake is not a fall of leaves. Nothing else of the cell's draws moves, and a scene with none of the three places every patch it did.

Ground reads use whole millimetres; positions are f64. IDs are nonzero row-major cell ordinals
in the entry window, checked against overflow before work. Seed, identity and ground determine
each tile independently of request order. Earlier footprints are restricted to anchors in the
same tile during both whole and streamed reads; neighbouring overhangs are not inferred. Meshes
remain resident while the host supplies their normal distance forms. `max_cells` limits work
per tile and rejects an excessive lattice at open, rather than dropping its tail.
`max_total_cells` (1048576 by default) also bounds a whole read before expansion.

**A whole read may run on the job system's performance pool, and by default does not** (T125,
2026-11-12; the tunable `litter.whole_read_jobs`: 1 one thread, the default; 0 the host's pool; a
larger number a cap on it). On the pool the window's tiles, in row-major order, go in bands of
whole rows, each band cut into contiguous runs, each run placed by one job into a list of its own,
and the lists joined in band and run order, never in the order the jobs finish; so the instances,
their ids rising, are the bytes one thread makes, and a refusal is the earliest tile's, the one the
serial walk met first. A band's mask tiles are read ahead on the calling thread in the level's
order, because the level reads through one window and jobs asking it from all over a band each
move it. Nothing is drawn from a shared stream: every draw is the cell's own hash.
`tests/litter_pool_tests.cpp` holds the bytes at one, four and eight threads, the whole pool and a
cap of three, and pins the bytes of the serial walk this replaced (`0x5a1878e19758bd3e`).

**Why one thread is the default: on a "tiles" ground the pool is slower.** Every height a cell asks
of a tile set's reader takes its lock twice (the pin and the unpin, `TileSet::tile`) and bumps a
shared counter, so concurrent cells queue on one mutex. Measured in `msvc-release` with
`engine_litter_bench` (the 1 km frame, 16,384 cells, medians of three; the machine was shared, 53 %
of its CPU used by other agents' work in the last run and up to 98 % in earlier ones, so every
number is an upper bound): one thread 38.2 ms, two 58.0, four 69.6, eight 90.0, sixteen 210.6,
the machine's whole pool 218.0. On the same ground read as a function with no lock
(`litter.expand.unlocked`) one thread took 5.1 ms, eight 4.1 and sixteen 4.3, and before the
mask read-ahead sixteen had taken 21 ms. So seven eighths of the entry's own cost on a tile set is
the reader's lookups, and until the tile set answers a point without its lock, the entry's work
stays on one thread, where it
costs about 2.4 us a cell (0.04 s for the bench's 16,384) and grows by a tenth on a frame three
times wider: the entry's own work is a function of its cells, not of the region.

**Three costs per cell were costs that should not be there**, and are gone with no byte moved: the
earlier footprints anchored in a tile are found once (a whole read sorts them by tile once and each
tile gathers the 3 x 3 tiles about it, keeping exactly those the one-tile scan keeps, in the host's
order), where every cell had scanned every earlier footprint of the scene; each mask tile is read
once by the run of tiles that reaches it (a `tile` call, or one run of a whole read, holding at
most 64), where every cell had copied its mask tile out of the level again (3,123 reads for 54
tiles in the pool test before, at most 216 after); and a cell standing on the ground it was decided
on reuses the five heights it read rather than asking the ground for them again.

**Where a cold load's minutes went** (the forest's engine needs 66). The entry's `open` asks the
ground for its masks when the layer names a substrate, and a `tiles` ground builds its mask level
on that first ask when the derived-data cache has none (`ensure_mask_level`, [tileset](tileset.md#the-mask-level)).
In the forest's scene the litter entry is the first to ask, so the startup trace put the cook,
which walks every mask tile of the heightfield's frame on one thread, under "placement litter":
763 s on v42 against 408 s on v41 for the same cells, because v42's frame is three times wider, and
815 s again after its archive was deleted, because a deleted archive is cooked again. The level's
key is its content (the set's hash, the block without its image paths and `resident_mib`, the
palette, the budget, the frame and the images' bytes), so the same masks cooked from two
directories are one entry and a second load cooks nothing (`tests/litter_masks_tests.cpp`). The
ask now has a startup stage of its own, "ground masks", so a trace names the cook. The cook itself
is the masks' to put on the pool.

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
containers, math, hash, json, schema, assets, geometry, materials, texture, scene_gen, deadwood,
litter_schemas, jobs (a whole read's pool) and profiling (the "ground masks" stage). The only read
of another solver is its public `deadwood::solve` and identity API. Where the tile-set capability
is in the build, the tests add `tests/litter_masks_tests.cpp` and the module has a bench,
`engine_litter_bench` (`litter.mask_cook`, `litter.expand.serial`, `litter.expand.pool`, each at a
frame one and three kilometres wide), both linking `tileset` as a test target may; the module
itself does not. Hosts link litter conditionally. Registration points are the mesh and placement registries
and schema descriptions. No protocol method. One tunable, `litter.whole_read_jobs` (how many jobs a
whole read runs on, 1 by default: "Placement"), which changes no byte; every other control is
recipe data.
Determinism is derived and cross-toolchain. The twig and tangle container pins follow the
deadwood solver they reuse: a change to deadwood's geometry or `k_deadwood_version` moves those
two (and only those two) and re-pins them in `determinism_tests.cpp` with the reason and commit,
as G104's deadwood v2 did. With the capability disabled, no cook or placement
runs; minimal configurations omit it. The shared inert catalogue row holds no runtime state.

## Not yet

Measured ecological deposition/decay, wind-driven redistribution, rainfall transport, species
mixes inferred from the canopy, deformable piles, arbitrary-surface contact, damaged-bark
species profiles, scanned default materials and a bespoke far appearance. Owner visual
acceptance is separate from automated correctness.
