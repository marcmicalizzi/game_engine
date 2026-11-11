# matgen (domain)

**Purpose.** Tiled PBR sets by role, **generated**: a recipe of whole numbers builds the four
images a material pack's set holds — base colour, normal, roughness, height — for the roles the
engine's solvers ask a pack for and no pack has: a gravel bed, a sand bar, scree, rock faces,
ground floors. A photographed or diffusion-made pack is the owner's to make
([materials](materials.md), [content generation](../content-generation.md#material-packs)); a
deterministic, research-grounded one is the engine's, and with it a checkout that has fetched
nothing still draws gravel as gravel instead of a flat colour (roadmap G148; the sources and each
number against them: [generated material sets](../experiments/generated-material-sets.md)).

**Why this shape.** Three decisions:

- *A set is derived data, never a committed image.* A recipe (`engine.matgen.Set`) is integers
  only, so its canonical JSON is the same bytes everywhere and `set_key` (that text and
  `k_matgen_version`) addresses what it builds. `engine-content matgen` writes sets into a pack
  directory under the git-ignored `content/materials/`; the repository holds the generator and
  the recipes' defaults, as code.
- *It attaches where a pack does.* Every solver already resolves a part's set by role through
  `materials::resolve_set` over a `materials::Pack`. A generated set is a role in a pack whose
  directory the lookup reads **behind** the fetched one (`materials::pack_at`, below), so no
  solver, host or renderer file changed to use one, and a fetched role always wins.
- *The generator knows nothing of the image's edges.* Every clast, crystal, joint and leaf lives
  on the torus of the tile; see "A set tiles by construction".

It is a capability with no registrar: nothing links it but `engine-content` and its own tests, so
`ENGINE_WITH_MATGEN=OFF` removes the command and nothing else.

## How a set is built

`build_set(set, images, stats, error, window)` validates the recipe, fills a field of texels
(linear albedo, roughness, height in metres) by the recipe's kind, and finishes it the same way
for every kind:

- **Hollows.** The height is blurred (a wrapped box filter, twice, in integers) and a texel
  lying under the surface around it is darkened by `cavity_permille` times how far under it is
  (full at the kind's own depth: 0.6 of the median clast's thickness, a rock's relief, 4 mm of a
  floor). The resolve has no occlusion at this scale, and a bed whose gaps are as bright as its
  tops reads as a painted floor; a photographed set carries the same shade. *Traded:* it is shade
  baked into albedo, right for sky light and wrong under a low sun, and a knob so a renderer that
  gains micro-occlusion can turn it off.
- **Wetness.** `wet_permille` multiplies albedo by `1 - 0.5 wet` and moves roughness toward
  `0.2 + 0.25 roughness`.
- **The images.** Base colour through the texture build's own sRGB tables
  (`texture::linear_to_srgb8`); the normal from the height's wrapped central differences in
  metres per metre, OpenGL (+Y up the image), so its strength is the surface's real slope and
  `normal_scale` stays 1; roughness; height over the set's own range (`SetStats::height_min_m`,
  `height_max_m`).

### A set tiles by construction

A noise takes a lattice coordinate of any size and wraps its nodes by its own period; a clast,
a thallus or a leaf has its centre in integer sub-texels (a 256th of a texel) and every texel it
touches is wrapped; a joint trace is a level of a phase that winds the torus a whole number of
times; the hollows' blur wraps. No function knows where the image starts. `Window` moves that
start by whole texels, and **the test builds each kind from a window half a tile and three
texels over and requires every image to be the first build's bytes, rotated — zero bytes
different**. That is exact, not close, because every coordinate is reduced before it is used:
texels a side are a power of two, so a lattice coordinate is exact; a cell's point is taken
within its own cell; offsets from a centre are integer differences; the blur sums integers.
The seam is then a column like any other, and a second test says so directly (the step across
the edge lies inside the range of the steps within).

## Granular sets

A bed of clasts ([grounding](../experiments/generated-material-sets.md#a-bed-of-clasts)).

- *Sizes.* The recipe states what a pebble count would read — `d50_um`, `sorting_milliphi`,
  cut `truncate_milliphi` either side — which is the distribution **by area shown**. Clasts are
  drawn **by number** from the same normal in phi moved down by 2 s squared (s the sorting in
  ln D), until their plan areas sum to `cover_permille` of the tile; the cut is raised to three
  texels and lowered to a third of the tile where the image cannot hold the stated one.
- *Form.* The intermediate axis b is the drawn size; a = b over a draw from the `ba` range, c =
  b times a draw from the `cb` range; a clast lies on its a-b plane. The outline is the unit
  circle of the clast's frame cut by a polygon of five to eight edges standing at the circle for
  a well rounded clast and at the inscribed polygon for a very angular one; the top is an
  ellipsoid's cap, or a bevelled plateau broken by one ridge. Both go from block to pebble
  between roundness 0.15 and 0.65, not evenly over the scale: the roundness classes are
  logarithmic, and a "rounded" pebble has no faces left (an even mix left the default bed's
  pebbles faceted like cut stones, seen in its normal map).
- *Fabric.* The a-axis lies across the flow (the set's +u) for `alignment_permille` of the
  clasts; each top leans by the imbrication dip (half to one and a half times `imbrication_deg`)
  down the flow and by up to `scatter_deg` about any axis, capped at what the clast's thickness
  can show.
- *Placing.* Largest first, each at hashed positions on the torus, accepted where at most two of
  thirteen probes of its outline are already taken (clasts lean on each other); one that finds no
  place in 24 tries goes to a lower layer, seen down the gaps. The surface is the highest top at
  each texel.
- *Between the clasts.* A matrix of grains (`matrix_um`) of the same lithologies, a quarter of a
  clast's thickness down — or, with `openwork_permille`, voids: the matrix deeper and darker,
  and that share of the drawn clasts laid again, elsewhere, a layer down, because what shows
  down a gap in a talus is more blocks. `cover_permille` is the plan area drawn, as a share of
  the tile; what shows is less by the clasts' overlap (`SetStats::clast_cover`: 0.80 of the
  default bed for 1.0 drawn).
- *Colour.* Each clast is one lithology of the mix by share, its tone its own, with a grain
  inside it (`speckle`), a broad mottle, and for some a pale vein straight across.

`SetStats` reports the count over every texel that shows a clast (`d16_mm`, `d50_mm`, `d84_mm`),
the share that is matrix, the clasts drawn and those in each layer, and the drawn clasts' mean
maximum projection sphericity.

## Rock sets

A rock face ([grounding](../experiments/generated-material-sets.md#a-rock-face)): crystals as a
cellular pattern at `grain_um`, each a mineral of the mix by modal share; relief of `relief_um`
half range from five octaves; up to two joint sets, the second across the first, each trace
shown along part of its length; a weathering rind over `rind_permille` of the face (the crystals'
contrast muted toward the rock's mean, tinted, stained) against fresh breaks a flake lower, the
patches cut from a low noise at the share's quantile so the share is exact; pits; bedding
laminae; and crustose lichen as lobed thalli, a Poisson cover of `lichen_permille`, on the rind
only. **Lichen by exposure is a parameter of the set, not a mask**: one set is one exposure, and
a mask by aspect over a mesh is the cover capability's.

## Ground sets

A floor ([grounding](../experiments/generated-material-sets.md#a-floor)): the parts of the
sward's floor bake ([flora](flora.md#the-ground-under-the-sward-g141)) as sets — moss (shoot tips
in cushions) and soil (crumbs) split at the moss share's quantile, dead leaf fragments as a
Poisson cover of `thatch_permille`, live leaves with a midrib and tapered tips as one of
`live_permille`. `SetStats` reports the three covers.

## The defaults

`default_sets()`, sorted by role; `engine-content matgen` builds them all.

| Role | Kind | Tile | What | Asked for by |
|---|---|---|---|---|
| `gravel/bed` | granular | 1 m | D50 45 mm, 1 phi, rounded, imbricated, wet through (a bed lies under its water), five lithologies | a channel's bed ([channels](channels.md)) |
| `sand/bar` | granular | 0.5 m | sand with a 12 % cover of granules and small pebbles, dry | a channel's bars |
| `rock/scree` | granular | 2 m | D50 110 mm, angular, openwork, no fabric | named for scree and deposits; see "Not yet" |
| `rock/granite` | rock | 1.5 m | a monzogranite, 3.5 mm, 92 % rind (an old face), 12 % lichen | boulders, cliffs, deposits, a soil profile's stones ([rocks](rocks.md)) |
| `rock/granite_wet` | rock | 1.5 m | the same wet through, a darker rind, 4 % lichen | a recipe that names it |
| `rock/basalt` | rock | 1.5 m | a dark groundmass, vesicles, a rusty rind | `RockType::Basalt` |
| `rock/limestone` | rock | 2 m | micrite, solution pits, a grey rind, 15 % lichen | `RockType::Limestone`, the caves |
| `rock/sandstone` | rock | 1.5 m | buff grains, twelve laminae, one joint set | `RockType::Sandstone` |
| `ground/soil`, `ground/moss`, `ground/thatch` | ground | 0.5 m | the sward floor's three parts | the sward's cook (`--sward-assets`) |
| `ground/turf` | ground | 0.5 m | live leaves over thatch over moss and soil | a terrain layer that names it |

Rock sets are `stochastic` (R131: a rock set is laid over cliffs many times its repeat, as
`tools/pack-materials.ps1` defaults every `rock/*` role); the granular and ground sets are
tiled, `rock/scree` included, because three offsets of a bed of clasts blended is no bed.

## The command

`engine-content matgen [--out <pack dir>] [--texels <n>] [--role <role>]... [--set <recipe.json>]... [--force]`

builds every default (or the roles and recipe files named) into a pack directory: a role's
directory holds `base_color.png`, `normal.png`, `roughness.png`, `height.png`, a 4 by 4
`metallic.png` of zeros (a pack role names five maps) and `set.json`, which records the key the
images were built for, the recipe and what the build measured. **A set whose `set.json` records
the recipe's key and whose five files exist is not rebuilt**; the sidecar is written last, so a
run that died leaves no key. `pack.json` beside them is a pack's manifest, each role with its
key under `provenance`, so the pack's hash — and every generated mesh's build key — moves when a
recipe or the generator does. One JSON line a set and one for the pack.

### The default

`--out` defaults to `<repo>/content/materials/generated`. `materials::pack_at(dir)` reads
`dir/pack.json` and, behind it, `dir/generated/pack.json` (`materials::merge_packs`): a role the
fetched pack has is the fetched pack's, every other role the generated one's, and where there is
no fetched pack the generated one is the pack. Every host finds its default pack through
`pack_at(default_pack_dir(...))`, so after one `engine-content matgen` a worktree's rocks,
channels, soil and caves resolve their roles with no flag. The textures are built from the PNGs
by the texture step as any pack's are (`<ddc>/textures/<key>.tex`).

**What that leaves** (the hosts' files were not this row's): the sets are built by a command,
not on demand — a scene read does not call the generator when a role is missing; `tools/fetch-materials.ps1`
replaces the destination's contents, so fetching a pack removes `generated/` and the command is
run again (seconds, below); and a caller that names its own pack directory (every test) gets the
generated sets only by building them there.

## Same bytes everywhere

Every decision is a hash, an integer, IEEE `+ - * /`, `sqrt`, `floor` or the engine's own
`det::exp`, `det::log`, `det::sin`, `det::cos`; the sRGB encode is the texture build's committed
table. The build is single-threaded, so there is no thread count for the bytes to depend on.
`matgen_tests.cpp` pins the four images' hash for six defaults at 256 texels, taken on MSVC.
Nothing was needed that `core/math/deterministic.h` lacks: a normal draw is Box and Muller's
from `det::log` and `det::cos`, a cube root is `det::exp(det::log(x) / 3)`, a tangent
`det::sin / det::cos`.

## Budgets

A set costs nothing in a frame and nothing per tile: it is built once per recipe, by a command,
and from then on it is a pack's images — three textures a role the renderer uploads (colour,
roughness and normal; height is carried), 1.4 MB each at 1,024 squared and 5.6 MB at 2,048 in
the `.tex` container (the figures [materials](materials.md#the-first-pack-2026-10-19) measured
for a pack's maps of those sizes), shared by every mesh that samples the role. Nothing scales
with the world's size. What a build costs is under "What it measured".

## What it measured

`engine-content matgen --force` in `msvc-release`, every default, 2026-11-10 23:27 to 23:31 on
the shared machine (RTX 5090 box, 36 threads): **not a quiet run** — about twelve agents were
building and testing, the processors read 65 % busy at the end and free memory fell under 2 GB
at the start, so each figure is an upper bound. One thread; `build` is `build_set`, `write` the
five PNGs.

| Set | 1,024: build ms | write ms | PNG bytes | 2,048: build ms | write ms | PNG bytes |
|---|---|---|---|---|---|---|
| `gravel/bed` | 873 | 426 | 4,975,020 | 5,117 | 1,470 | 16,698,883 |
| `sand/bar` | 1,276 | 833 | 4,255,260 | 2,691 | 1,246 | 13,420,868 |
| `rock/scree` | 1,552 | 813 | 4,880,654 | 3,664 | 1,344 | 17,910,380 |
| `rock/granite` | 1,818 | 778 | 3,044,649 | 7,181 | 1,099 | 9,220,140 |
| `rock/granite_wet` | 2,294 | 614 | 2,812,091 | 7,174 | 1,155 | 8,468,409 |
| `rock/basalt` | 1,696 | 338 | 3,008,115 | 8,826 | 1,222 | 10,145,953 |
| `rock/limestone` | 2,598 | 572 | 3,459,416 | 9,344 | 1,492 | 11,728,415 |
| `rock/sandstone` | 2,554 | 746 | 2,911,626 | 6,866 | 1,167 | 9,887,204 |
| `ground/moss` | 1,013 | 357 | 4,796,841 | 5,815 | 1,733 | 15,933,246 |
| `ground/soil` | 983 | 315 | 5,227,616 | 5,973 | 6,132 | 20,619,558 |
| `ground/thatch` | 1,494 | 332 | 5,621,561 | 6,044 | 1,467 | 20,830,493 |
| `ground/turf` | 1,424 | 386 | 6,300,743 | 6,167 | 1,461 | 22,902,104 |

The whole command: **28.8 s and 51.3 MB of PNG at 1,024, 98.1 s and 177.8 MB at 2,048**, twelve
sets, 73 files; run again with nothing changed, 3.3 s and 1.8 s (it reads twelve sidecars). An
earlier run of the same binary at 1,024 beside heavier load took 67 s, which is how far these
move with the machine. The rock kind is the dearest a texel (1.7 to 2.2 microseconds at 2,048):
some eighty lattice hashes in f64 for its crystals, relief, stain, joints and patches; nobody
has tried to make it cheaper, because it is paid once. Laying a floor's leaves walked each
leaf's whole bounding square at first (17.9 s for the thatch at 2,048); walking only the texels
near the leaf's line made it 6.0 s and the same bytes.

Resident while a set is built: 61 bytes a texel (the field's 20, the hollows' three integer
planes, the granular kind's top, id and taken planes, the four images) — 64 MB at 1,024 squared,
256 MB at 2,048 — all freed when the images are written.

**Owned data.** The recipe type (`schemas/matgen.schema`), the default recipes, `k_matgen_version`.
The pack directory `engine-content matgen` writes is derived data.

**Invariants.**

- A set built from any window is the first window's bytes rotated.
- The grid count over a bed reads the recipe's D50 and sorting; a rock's fresh share and a
  floor's covers are the recipe's.
- A recipe out of range is refused with the field named, and builds nothing.
- The key moves with any field of the recipe and with `k_matgen_version`.
- The same recipe builds the same bytes on every toolchain.

**Public API.** `include/domain/matgen/matgen.h`: `Set` and its parts (from the schema),
`Window`, `SetImages`, `SetStats`, `read_set`, `validate_set`, `canonical_json`, `set_key`,
`build_set`, `default_sets`, `default_set`, `k_matgen_version`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `log`, `texture` (the sRGB
tables; the mip chain in the tests), `matgen_schemas`. No capability.

**Testing.** `tools/dev.ps1 test -Filter matgen`: the rotation test over seven defaults; the
seam's step; the grid count over five seeds; scree against the bed; a rock's shares and a
floor's covers over three seeds each; every default's mean albedo and roughness inside its
material's range; a mip chain's mean; a recipe read, refused and keyed; the pins (17 s in
`msvc-debug`, the longest case 4.4 s: twelve defaults built at 256). `materials`' tests hold
`merge_packs` and `pack_at`'s generated pack; `apps/engine_content/tests/matgen_tests.cpp` runs
the command (two roles built, read back through `pack_at`, built again and skipped, a third role
joining the manifest, the refusals; 1.1 s). Benchmarks: `tools/dev.ps1 bench -Filter 'matgen.*'`.

**Performance notes.** Nothing here is in a frame: a set is built once per recipe by the
command. Measured figures are in "What it measured" above.

## Not yet

- **Built on demand.** A scene read that finds a role missing could call `build_set` through a
  registration point and the derived-data cache, as a tile's mesh is built; it needs the hosts'
  lookup (`systems/renderer/src/scene.cpp`, `domain/content_build`), which this row did not edit.
- **`rock/scree` has no caller.** The rocks capability resolves scree and deposits by the
  rock's type (`rock/granite`...), so a scree mesh takes the rock face's set; the set here is for
  a ground layer or a recipe that names it until the deposits ask for a granular role.
- **Lithology from the formation.** The bed's mix and the scree's rock are the recipe's, not the
  scene's formation's; a recipe per catchment is the hook.
- **Height is carried and unread** by generated meshes ([materials](materials.md), "Not yet"); a
  bed of cobbles wants parallax or displacement at one metre.
- **Roughness is one channel in a BC7 slot**, as every pack's.
- **Fall sorting, ripples, a wet margin**: one set is one state; a bar's ripples and the sorting
  down a cone are the placement's or a later kind's.
- The numbers marked not verified on the grounding page.

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Schema types | `domain/matgen/schemas/matgen.schema` | the recipe |
| Tick scheduler entry | none | nothing runs in a tick |
| Render-graph passes | none | a set reaches the renderer as a pack's images |
| Content-build derived step | `engine-content matgen`, keyed by `set_key` | a pack directory |
| Protocol methods | none | |
| Tunables | none | every parameter is the recipe's, and in the key |
| LOD policy | the texture build's mip chain; the far form is the set's mean | held by a test |
| Determinism | derived; the same bytes from every toolchain | pinned |
| Zero cost unused | no registrar, no static state | |
