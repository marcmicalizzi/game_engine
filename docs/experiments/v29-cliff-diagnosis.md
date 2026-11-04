# G132: v29 foothill cliffs and repeated materials

2026-11-04. Diagnosis on the packaged `efe7e9c5` runtime; no solver or POC input changes.
The worktree was one design commit ahead of an older main, so a fast-forward was impossible:
the original ridge design is preserved at `codex/rock-structure-design-preserved` (`7fc55b4c`)
and was replayed on `efe7e9c5` as `e38057a5`. Shared main and its release build were untouched.

## Evidence and reproduction

Input: `D:/workspace/forest_wildlife_poc/reports/quality-v29/ledge-scene.json`,
`ledge-close-path.json`, `ledge-close.png` and its report. The first of five cliff recipes is
`world/v29/recipes/foothill-ledge-0.rock.json`. Authoring is in `scripts/build-quality-v29.py`.
All diagnostic scripts, copied variants, GLBs, captures, logs and reports are outside the repo:
`D:/workspace/game_engine_local/rocks/2026-11-04/v29-diagnosis/`.

The local `capture.ps1` and `capture-followup.ps1` reproduce the POC review command using its
packaged `runtime/efe7e9c5/bin/engine-view.exe`, original camera, hw raster, CSM, 1920x1080,
90 requested frames, fixed 15:00 and no point lights. Outputs use the diagnostic directory,
not the POC report paths. GPU runs use `tools/gpu-lock.ps1 run`; no builds or broad gates ran.
Machine: RTX 5090, i9-10980XE, NVIDIA 617.42. The baseline reported other CPU use 15.1% and
GPU use 4%; subsequent runs also reported contention. These are visual checks, **not quiet
performance measurements**, and their frame times are not presented as solver costs.

| Control | Result |
|---|---|
| `baseline.png`: unmodified recipe, 1 px LOD | Reproduces the long wall and rectangular apron lobes. |
| `finest.png`: same recipe, 0.01 px LOD | Same dominant wall/apron silhouette; visible cluster pairs rise from 448 to 834. Runtime simplification is not the main explanation. This is a finer cut, not proof of every source triangle being selected. |
| `no-talus.png`: only ledge 0 sets `talus=0`, `talus_blocks=0` | Apron rectangles disappear. The wall and detached-looking shadow remain. |
| `detail.png`: no-talus copy with 400,000 source budget | More source detail, but still the same dominant continuous wall. Increasing tessellation alone does not supply outcrop composition. |
| `neutral.png`: original recipe, `--view normals` | Surface orientation varies across beds/blocks; the front is not literally a featureless plane. Strong repeated colour/normal detail obscures this shallow structure in the shaded view. |
| `local-band.png`: shortened/lowered copy | A narrow exposed lens, not an accepted natural outcrop. Shows that smaller dimensions alone are insufficient when relief retreats into the existing terrain. |
| `terrain-only.png`: remove all five meshes and instances | The conspicuous detached-looking block shadow remains. It is not cast by any of these cliff meshes or their debris. Its terrain/CSM origin needs separate renderer diagnosis. |

`original-leaves.png` is **not** a valid source-leaf control: `--no-cull` was accepted but CSM
forced culling back on, recorded in stderr and `summary.cull=true`. Do not use that image to
claim a leaf render. The follow-up `leaves-unshadowed.png` with shadows off is also invalid as a leaf control: the renderer forces culling for a deformed scene (`summary.cull=true`). The matched `default-unshadowed.png` is retained for reference. No full-leaf result is claimed. An initial script also passed a single PowerShell string as character
arguments, producing usage exit 2 before rendering; array wrapping fixed that invocation.

## Geometry: authoring and solver both contribute

The author traces about 120 m of a contour, gives every point at least 5 m of height and uses
a smooth 5..8..5 m sine envelope. Thus the face never disappears into the hill along its span.
The 1.5 m standoff and a batter chosen as the minimum ratio over an uphill profile favour a
continuous exposed face. The code does use run/rise, not an accidentally inverted slope.
However it does not select intervals where a resistant bed naturally emerges and then submerges.
The unbroken cap and regular exposure are authoring consequences, not cured by PBR or finer LOD.

The original recipe is Sandstone, relief 1.25 m, beds 1.5 m, depth 24 m, burial 2 m and budget
110,000. Native export reports 84,440 triangles. The no-talus copy reports 69,936 triangles,
all in the face box. The selected 0.5 m rung has 243 along cells, 20 vertical cells and 48 depth
cells: `4 * (243*20 + 243*48 + 20*48) = 69,936`. Only `2*243*20 = 9,720` triangles (13.9%)
are on the carved front; the back, top, bottom and ends consume the rest. The large buried
depth is expensive even where hidden. The 1.5 m bed/joint field is at half strength under
`fade_for(wavelength, cell) = clamp((wavelength/cell - 2)/2, 0, 1)`.

The 400,000-budget no-talus copy selects 0.25 m cells and emits 279,200 triangles. It resolves
more detail but retains the prescribed long cap and shallow relief. G127's large-scale field
reserves only 35% of the relief envelope, here 0.4375 m before filtering; a low-amplitude noise
field is not an authorable set of projecting buttresses and recessed gullies. The bottom-up
support constraint further limits outward ledges. Treat the box topology, shared resolution
for hidden closure and visible front, and limited macro-shape controls as solver gaps.

For low foothill exposures, an immediate authoring change is to disable the legacy skirt and
its blocks (`talus=0`, `talus_blocks=0`) and author separate source-connected debris where
needed through G128. Select actual exposure intervals from the terrain profile, vary the
visible top/foot, and bury the ends by lowering their whole section into terrain. Lowering
`height` alone cannot reach zero: the current contract has a 1 m minimum. Choose the buried
back depth from actual containment needs, not a universal 24 m; reducing it without checking
the uphill ground can expose the back or cap.

The diagnostic `local-band.rock.json` uses points 8..16, heights
`[1,1.6,2.4,2.9,3.5,3,2.2,1.5,1]`, depth 6 m, relief 2 m, bed 1 m, no debris and the original
110,000 budget. It emits 57,160 triangles at 0.1875 m cells. Much of its carved surface retreats
inside the untouched terrain, leaving a thin lens. **It is a failed authoring probe, not a
recipe to copy into the POC.** Standoff is a mean-plane offset; it does not guarantee that the
final carved face is outside the terrain.

The concrete missing solver contract is an exposed-band solve: terrain-aware placement of the
**finished** carved front and its termination, explicit exposed intervals/crest retreat, and
separate front-versus-buried-closure allocation. It must report terrain occlusion/contact failures
instead of requiring repeated manual pushes of an arbitrary closed box. It need not change the
existing wall-like `Cliff` path or replace the separate [ridge proposal](rock-ridges.md).

## Why the apron is rectangular

The legacy skirt remains a closed ruled strip. `skirt_station` extends its inner slope toward
the cliff's buried depth: `foot + heap_height + (depth - 1) * tan(angle)`, capped near the crest.
Consequently `talus=0.08` does not bound every visible apron vertex to 8% of cliff height.
Long smooth slope patches intersect a continuous wall, and patch gating makes repeated lobes.
Terrain draping fixes ground contact; it does not create piles of fragments, natural deposit
boundaries or a rock/soil transition. The no-talus control confirms these rectangles belong to
this skirt. G128 deposits are the better authoring route; this diagnosis does not claim to have
visually qualified G128 on this particular ledge.

## PBR repetition: a separate rendering extension

`RockMaterial` v4 exposes tiled maps and `tile_m`, and the unique `surface_maps` path rejects
cliffs. G127's smooth UV perturbation varies phase, but cannot remove repeated motifs from the
1.8 m photographed tile. Increasing that repeat size would change perceived rock/grain scale
and leave the large motif. Terrain's stochastic layer flag is handled by
`domain/gfx/shaders/terrain_material.slang`; it is not a supported switch in a cliff recipe or
ordinary material-pack role.

The first material extension should expose coherent stochastic **fine-detail** sampling for
rock materials while retaining coarse formation structure separately. Grounding:
[Deliot and Heitz, Procedural Stochastic Textures by Tiling and Blending](https://eheitzresearch.wordpress.com/738-2/)
addresses repetition and the importance of filtering the blend through mip levels. The terrain
implementation already coordinates maps and derivatives and corrects rotated normal slopes;
factor reusable code rather than silently sending mesh materials through terrain layer slots.
This is an engineering proposal, not a claim of exact histogram preservation by the existing
terrain approximation.

Required contract and tests:

* One stable lattice, seed, shifts, rotations and blend weights for albedo, normal, roughness
  and any height map. Preserve each slot's proper colour/data decoding. Rotate UV gradients
  and normal slopes consistently and apply unresolved normal variance once after blending.
* Stable formation/object coordinates through instancing, origin rebasing and LOD. Cliff UVs
  reset every 16 repeats; whole-repeat shifts preserve ordinary tiles but **not** a stochastic
  lattice. Preserve chart origins or derive stable formation projections instead of hashing
  the reset UV. Tests must cross a chart boundary and compare both sides of a bent face.
* Avoid unconstrained tile rotations for strongly directional bedding/veins. Keep stratigraphy,
  mineral bands, runoff stains and fresh-break/weathered masks in a coherent coarse field or
  unique atlas; randomize a directionless grain/detail layer underneath it. A generic boulder
  tile with strong large marks may remain unsuitable even after stochastic blending.
* Add schema, material/container/cache metadata and the mesh resolve path together. A new recipe
  field alone cannot activate it. Qualify near/far mip transitions, neutral normal maps, rotated
  directional normals, chart seams and low graphics tiers. Price the three stochastic texture
  samples per projection/channel; triplanar sampling multiplies that again. There is no measured
  frame-cost claim for the extension yet.

A unique cliff atlas plus fine detail is the complementary path: bake coarse colour, roughness
and geological masks in the same coordinates as the geometry, then compose budgeted stochastic
fine relief. One small atlas for an entire 120 m face cannot provide climbing-distance grain.
This needs explicit texel density/page budgets and a detail-layer contract, neither supplied by
today's radial-only `surface_maps`. It is not solved by changing the recipe's resolution field.

## Distant mountain structure

At 1080 rows and a 60-degree vertical field of view, a perpendicular 1.5 m feature at 1 km is
about 1.4 pixels (`1080/(2*tan(30 degrees)) * 1.5/1000`). Foreshortening reduces it further.
Fine beds and grain should filter away; forcing them into distant geometry produces aliasing.
Use physically distinct broad buttresses, major joints, stratigraphic bands, silhouette breaks
and coherent large-scale material variation for range views. These macro features must survive
source-quality reduction and then be checked against the runtime screen-error cut. Do not simply
scale every small texture or geological spacing up to make it visible.

The current coarse relief field is a useful floor, but cannot author a connected arête/horn,
arbitrary deeply recessed headwall or large resistant bed exposures. G131 addresses one connected
ridge; G132's exposed-band and material gaps are separate. No distant mountain capture was run
in this diagnosis, so this is a design requirement, not a claimed distant-view fix.
