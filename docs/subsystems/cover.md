# cover (domain, capability)

**Purpose.** What grows **over** another mesh: climbing plants (roadmap G76) and organic cover —
moss, lichen, algae and fungi (G78) — on one host-surface foundation. A recipe names its **host** (a
rock, a wall, a tree: any mesh a scene names) and the capability makes the host's **companion**: its
own mesh in the host's frame, which the scene places with the host's transform. The host is never
edited and stays the bytes it was. The grounding — Luft's ivy generator, Hädrich et al.'s climbing
plants, the climbing strategies, where moss, lichen, algae and fungi grow, the graphics models and
the constants chosen — is [vine-cover](../experiments/vine-cover.md); this page is the rules.

**Owned data.** The recipes (`domain/cover/schemas/cover.schema`, `engine.cover`): `VineFile`
(`<name>.vine.json`: the host, a `HostSite`, a `Vine`) and `CoverFile` (`<name>.cover.json`: the
host, a `HostSite`, a `CoverGrowth`). The solvers' outputs are containers the caller writes; nothing
is global but the two mesh-generator registrations.

## Why this shape

- **One module, not the host query in `domain/geometry`** (ADR-0027). A closest-point BVH over a
  cluster mesh is generic — a decal or a footprint will want it — but today both of its readers are
  here, and a capability that grows `geometry` for its own use is the edge ADR-0027 keeps out of the
  tree. It lives in `host.h` and moves to `geometry` with its second reader.
- **A companion, never an edit.** The vine and the cover are meshes of their own placed with the
  host (the same translation, yaw, scale and `ground` flag), so a host's container, its cache entry
  and its pinned hashes never move, and a scene can show a host bare or overgrown by naming the
  companion or not. The cost: the scene author places the companion beside its host by hand, and a
  placement generator that places hosts (scatter, the ruins) does not yet place their companions.
- **Mesh generators with a host seam.** The scene reader (a non-capability) finds `vine` and
  `cover` by the file's name in the mesh-generator registry, as it finds `flora` and `rock`
  (assets.md, "Generated meshes"). What is new is the **host seam**
  (`assets::MeshGeneratorInputs::hosts`): a generator whose file names another mesh reaches it
  through two hooks the host app provides — `identify` (the host's source hash, cheaply) and `load`
  (its triangles, `assets::GeneratorHostMesh`) — implemented once, in content_build's
  `GeneratorHosts`, which engine-content and the renderer's scene reader both use, so either finds
  what the other built. A companion's **source hash covers its host's identity**
  (`MeshGeneratorDesc::source_hash_with`), so a changed host is a new key and never a stale vine; a
  host app with no seam is refused rather than given a key that cannot see the host.
- **The flora kit for the shapes** (`flora/companion.h`, additive): stems as tubes, leaves as cards,
  cushions and caps as domes, all through the species' emitter, so a companion carries the motion
  contract and the structural table the renderer's flora stage and the walker's collision already
  read.

## The host

`assets::GeneratorHostMesh` is what a solver reads: every triangle of the host's **finest level** in
its own mesh space, each with its material's **roughness** and whether a climber may **hold** it.
`assets::host_mesh_of` takes it from a container — level 0's clusters; `holds` from the motion
contract and the structural table, a triangle holding when all three corners are on part 0 or on a
part flagged load-bearing or a collider (the walker's collision's rule, scene_collision.md), so a
vine never grows over a leaf card — and `GeneratorHosts` from a glTF's import (every triangle
holding). The three host kinds a recipe may name: a generated mesh (`.rock.json`, `.flora.json`,
built in memory under the dependant's options), a `.clusters` container, a glTF or GLB; a host that
is itself a dependant is followed up to `content_build::k_host_depth` (4) levels.

`HostSurface` (host.h) welds the vertices by exact position (a cluster build's and a part
boundary's duplicates join), measures each triangle's normal and area and each welded vertex's
area-weighted normal, and keeps the welded adjacency. `HostBvh` is a median-split BVH (leaves of
four) for `closest` (Ericson's closest point on a triangle; over every triangle, or the holding
ones) and `occluded` (Möller–Trumbore against a segment).

### The exposure terms

Per triangle (`Exposure`, `host_exposure`), in the host's frame with the site's directions turned by
the placement's `yaw_deg` (the world's north is −z, sky.md):

| Term | What |
|---|---|
| `up` | the normal's y: 1 a top, 0 a wall, −1 an overhang |
| `north` | the normal along the world's north |
| `sun` | max(0, n·L), zero in the host's own shadow (a ray to the light), times 1 − the canopy's shade |
| `sky` | the cosine-weighted share of 32 fixed directions that no surface hides within `crevice_radius_m` (the ground counts): 1 open, 0 a deep crevice — the ambient-occlusion proxy, its complement the crevice-ness |
| `height` | above the site's ground |
| `water` | the site's distance to water |

The **site** (`HostSite`) is the recipe's: the light (azimuth from north and elevation; the
overgrown scene's recipes give the renderer's default sun), the ground's height, the water's
distance, the canopy's shade and the climate's wetness. **Not yet**: the content build does not read
them from a scene (the terrain under the host, the nearest water body, Beer–Lambert under the placed
canopy, G77's light field); a recipe says them.

## The vine

`solve_vine` (vine.h). **Seeds** on holding surface reaching into `seed_band_m` above the host's
base or the ground (a point drawn inside the band; never an underside), plus the recipe's anchors
snapped to the surface. **The walk**, Luft's: each step the direction is the normalized sum of the
last direction × `primary_weight`, up × `light_weight`, a random unit vector × `random_weight`, and —
where the closest holding surface is within `adhesion_distance_m` — towards it × `adhesion_weight` if
the rootlets hold (its roughness ≥ `hold_roughness`), else gravity × `gravity_weight` growing with
the run since the last hold. A **held** stem keeps to the surface's plane and its next point is the
closest point of the surface to the step, lifted a millimetre; a **leaning** one (on a surface too
smooth to hold) may leave the surface but not enter it; a **free** one stops after `max_free_m`
without a hold, or below the ground. A held step branches with `branch_chance`, the branch turned
`branch_angle_deg` in the surface's plane with 0.7 of what its parent has left. The walk never
depends on the budget.

**The mesh.** Part 0 is the vine's root, a pivot with no geometry. Every stem is a part (pivot at
its start, parent the stem it left), no structural flag; its tube's radius is the pipe model's,
`tip_radius · sqrt(tips it carries)` capped at `stem_radius_m`, tapering linearly to the tip
radius, a held point's ring lifted its radius off the surface. Leaves are two-sided cards on
petioles off the stem's outer side, alternate, facing out and up, every `leaf_spacing_m`. **Motion**:
a held vertex weighs 0 (it is fixed to the host); along a free run the weight rises with the run to
1 at a metre — the hanging tips are what the wind moves. **The budget**: the rows coarsen and the
leaves thin, alternately, until the triangles fit. Two primitives, `vine/stem` (one-sided) and
`vine/leaf` (two-sided).

**Climbing strategies.** The root climber is modelled; a rambler's leaning falls out of it on a
smooth host. Twining stems and tendrils round thin supports are not (vine-cover.md).

## The cover

`solve_cover` (growth.h). The host's holding triangles are **split to `sample_m`** (a point on a
shared edge from its two corners in weld order, so the split surface welds), so a wall of a dozen
triangles still gets patches; the exposure terms are read on the split surface with rays against
the host itself.

### The mask

Per split triangle, the suitabilities (`cover_suitability`; r is roughness, D the damp):

| Kind | Suitability | The ecology (vine-cover.md) |
|---|---|---|
| damp D | clamp(0.45 wetness + 0.25 e^(−height/0.5 m) + 0.2 (1 − sky) + 0.12 max(0, north) + 0.2 e^(−water/5 m) − 0.25 sun) | the base, shelter, the north face and water keep a surface wet; the sun dries it |
| moss | clamp(1.4 D) · (1 − 0.75 sun) · face · (0.35 + 0.65 r), face 0.7 + 0.3 up on a top or wall, 0.7 + 0.8 up (≥ 0.1) under an overhang | shade, damp, crevices and bases, upward faces; bark texture the strongest factor (the 1994 survey) |
| crustose lichen | (0.3 + 0.7 max(sun, 0.6 sky)) · (1 − 0.6 D) · (0.5 + 0.5 r) · (0.4 under an overhang) | exposed rock and bark, drier and sunnier than the moss (Killarney's zonation) |
| algae | clamp(1.2 D) · (1 − 0.9 sun) | a film on damp shade, smooth surfaces too |
| fungi | dead wood only: clamp(1.3 D) · (1 − 0.8 sun) | brackets and mushrooms on dead and dying wood |

Each is scaled by the kind's weight, its growth 1 − exp(−age / τ) (τ: algae 2, fungi 3, moss 8,
lichen 40 years) and its **colonies** (1 − patchiness + patchiness · a seeded two-octave value
noise at `patch_m`); moss takes its share, lichen (1 − moss) of its own, algae (1 − moss)(1 −
lichen). Carried to the welded vertices by area, smoothed `smoothing` Jacobi passes along the
surface (over the vertices that carry cover), and sharpened by smoothstep(0.1, 0.8), so a patch has
an edge. **Written per host vertex** into the companion's container as section 39 (`cover_mask`,
geometry.md): four unorm8 a word, moss in the low byte, then lichen, algae, fungus — the host
container's cluster-ordered vertices, or a glTF host's flattened ones.

### The shaped cover

Per split triangle and kind, `area × density × mask` pieces, the fraction by a seeded draw, so the
count follows the mask's integral: **moss cushions** (domes of `cushion_radius_m`, three variants of
6 to 8 sides), **fruticose lichen tufts** (three strap cards of `tuft_height_m`), **bracket fungi**
on steep faces (a flat half-dome reaching `bracket_reach_m` out) and **mushrooms** on upward ones (a
stalk and a cap) — each a copy of a few kit shapes turned to the surface. **Not instances**: the
pieces are merged into the companion's mesh, which costs their triangles once per host rather than
an instance each; a placement generator that instances a few generated meshes over a host is the
follow-up when a forest has thousands of hosts.

### The stand-in film, until R90

The smooth cover is a second material set blended under the mask: roadmap **R90**, not built here.
The stand-in: every split triangle whose strongest smooth kind (moss, lichen, algae) averages
`film_threshold` or more over its corners is drawn again `film_offset_m` (4 mm) off the surface along
the welded normals, in that kind's material (`moss/mat`, `lichen/crust`, `algae/film`). It is
coarse — a patch's edge is a triangle's at `sample_m` — and costs a triangle per covered sample. The
brief's other stand-in, baking the mask into the host's vertex colour, is not possible: no
material reads a vertex colour (the glTF import reads none, assets.md) and baking would edit the
host. **R90's design**: the resolve reads the mask through the attribute path — the companion's
section 39, uploaded beside the host's vertices in the host's vertex order, one u32 a vertex — and
a second material index per cluster (the cover set's), and blends the second set's base colour,
normal and roughness under the mask's weight per pixel, interpolated by the barycentrics the resolve
already has; the film's triangles then go away. It needs the material system's sets (G60) and one
attribute stream more in `gfx::MeshDesc`.

**The budget**: the film's threshold rises by 0.1 to 1.0, then the pieces' density halves, until the
triangles fit. Four primitives, one a kind: moss (the film and the cushions), lichen (two-sided:
the tufts are cards), algae, fungi.

## The build

`engine-content vine <recipe.vine.json> <out.clusters>|--cache` and `engine-content cover ...`
(`apps/engine_content/cover_commands.cpp`) read the host through `GeneratorHosts` under `build`'s
default options, build the companion and print one JSON line: the host, its identity, the keys, the
solver's report (stems, held, leaning and free steps, leaves, length, seed and tip heights; or the
kinds' mean cover, the film's triangles and the pieces' counts). `build` and `build-all` take a
recipe through the registry like any generated mesh, and a scene names one in its mesh table.

**LOD.** The cluster DAG's, one per primitive, cones off for a two-sided material.

## Collision

Vines and cover **never collide**, by the motion contract the walker's collision already reads
(scene_collision.md, "What gets no body"): part 0 of a companion is a root with no geometry, and
every vertex is on a later part with no structural flag, so every triangle is left out of the proxy.
A rock's moss does not trip a walker and a wall's ivy does not stop one.

## Determinism

The same recipe and host give the same bytes on every toolchain: f64 arithmetic with IEEE `+ − × /`
and `sqrt`, the deterministic sines and `exp`, splitmix64 streams per seed, per stem and per
triangle, the weld and the BVH by total orders, one thread. `k_cover_version` is in every source
hash; bump it with any change that moves a pinned companion's bytes. The vine on the test cube and
the two containers are pinned in `tests/cover_tests.cpp`.

## Not yet

A swaying host (a vine on a tree's limbs rides nothing; its vertices would have to ride the host's
parts, part by part, through the motion contract); twining and tendril climbers; the site read from
the scene; placement of companions by the generators that place hosts; the blended layer (R90);
cover that weathers the host's own material (Dorsey and Hanrahan's patinas).

## How to test

`tools/dev.ps1 test -Filter cover`: `tests/cover_tests.cpp` — the BVH's closest point against brute
force on a sphere and a cube, occlusion and the weld; a vine on a cube keeps within 2 mm of the
surface where it holds and climbs (tips at 2 m from seeds at 0.25 m); a rough host attaches more
than a smooth one (1,561 held steps against none, the smooth host's stems leaning and falling off);
the same seed is the same vine and its bytes are pinned; moss favours a shaded north crevice by the
ground over a sunlit smooth top (0.49 against 0.03) and lichen the reverse; the cushions' count
follows the mask (none at 2 years, 241 at 40, the integral's prediction within four sigma); and the
generators through the host seam, the key moving with the host, section 39 round-tripping, the
containers pinned. `scene_collision`'s companion test holds the collision rule, and engine-content's `tests/cover_tests.cpp` runs `vine` and `cover` end to end over a small boulder (built, a cache hit, `build` under the same source hash, a new key when the host's recipe changes, the mask's words; without the capability, the refusal). `tests/size_table.cpp`
pins `P3` and `Exposure`, the per-triangle records a host's memory is made of.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the
scheduler, or another capability's behaviour (flora gains an additive public kit header), and it can
be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(cover flora)` | declared; off when flora is off |
| Schema types | `domain/cover/schemas/cover.schema` | `VineFile`, `CoverFile` and their parts |
| Tick scheduler entry | none | not needed: the solvers run at content-build time |
| Render-graph passes | none | a companion is a cluster mesh; R90 will add the blend |
| Content-build derived step | the mesh generators `vine` and `cover` | keyed by recipe, version and host |
| Protocol methods | none | not needed |
| Tunables | none | every parameter is the recipe's |
| LOD policy | the cluster DAG's | per primitive |
| Determinism | content-build output, the same bytes everywhere | pinned |
| Zero cost when unused | nothing runs unless a scene names a recipe | the registry |
| Tests and size table | `tests/cover_tests.cpp`, `tests/size_table.cpp` | as above |
| Bench | none | no hot path |
| Removal proof | `ENGINE_WITH_COVER`, off in the minimal build | engine-content refuses `vine` and `cover` with a sentence |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_COVER=OFF`) drops the module and
its tests; a scene naming a `.vine.json` is refused by the registry's sentence.
