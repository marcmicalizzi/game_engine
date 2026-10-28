# content/rocks

Rock recipes for the rock solver ([rocks](../../docs/subsystems/rocks.md), roadmap G62): each
`<name>.rock.json` is an `engine.rocks.RockFile` — a shape and its parameters — that a scene's mesh
table, a scatter layer's kind or `engine-view --mesh` names like a glTF, built through the
derived-data cache. Nothing binary lives here: the meshes are solved from these numbers.

| Recipe | Shape | What it is | Triangles (budget) |
|---|---|---|---|
| `granite-boulder.rock.json` | Boulder | a 2.6 × 1.8 × 2.1 m granite block with three weathered joint sets | 76,800 (80,000) |
| `sandstone-boulder.rock.json` | Boulder | a 3.2 × 2 × 2.4 m sandstone block with unequal 0.3 m beds and granular loss | 110,592 (120,000) |
| `basalt-outcrop.rock.json` | Outcrop | seven basalt columns sharing joint weathering in a 9 × 5.2 × 7 m envelope | 258,048 (320,000) |
| `granite-scree.rock.json` | Scree | fourteen sharp granite fragments up to 0.44 m over a 2.8 × 2 m patch | 24,384 (32,000) |
| `jointed-pinnacles.rock.json` | Outcrop | eight tall jointed granite bodies, a formation study inspired by the owner's photographs | 352,512 (400,000) |
| `shore-rubble.rock.json` | Scree | forty-eight angular blocks over a 7 × 5 m patch, one reusable group | 265,344 (320,000) |
| `sandstone-escarpment.rock.json` | Cliff | a sandstone face 298 m along a gentle bend, 24 to 44 m high, undercut, a talus skirt and thirty fallen blocks ([rocks](../../docs/subsystems/rocks.md#cliffs)) | 162,064 (250,000) |

G94 adds eight lithology studies: `slate-slab`, `gneiss-folded`, `schist-flaked`,
`obsidian-fractured`, `limestone-dissolved`, `shale-rubble`, `marble-weathered`, and
`foliated-cliff` (each `.rock.json`). Their budgets are 120,000–280,000 triangles and their
`geology` blocks deliberately exaggerate different combinations for comparison. They use flat
PBR constants so the geometry can be inspected separately from texture detail. Add a material
set or a pack role to texture them. These are authoring studies, not a photorealistic asset pack.

G96 adds `gneiss-veined`, `granite-minerals` and `sandstone-grain`, each a `.rock.json` pairing
the corresponding existing geometry with a 2048-square baked PBR atlas. These examples show
folded mineral bands, cross-cutting pale veins and granular colour/roughness/normal variation.
Use `material.surface_maps.resolution` (256/512/1024/2048) to trade texture detail and memory
independently of source geometry quality and runtime LOD. Delete the block to return to flat
materials or tiled texture packs. See [baked surfaces](../../docs/subsystems/rocks.md#baked-geological-surfaces-g96-solver-version-5).

The original G62/G71 recipes passed E10's four thresholds (rocks.md, "What the harness taught");
that historical result does not qualify later revisions. G89 review is in
[rock structure](../../docs/experiments/rock-structure.md); G94 is in
[rock lithology](../../docs/experiments/rock-lithology.md). Each original recipe's mesh is
pinned by hash in `domain/rocks/tests/rocks_tests.cpp`: change a number here and that test says
so — update the pin in the same commit. [`content/test-scenes/rock-field/`](../test-scenes/rock-field/README.md)
places the four original boulder/outcrop/scree recipes. The two new groups can be placed as one
mesh instance each. Grouping is an authoring convenience, not a rockfall-settling simulation:
individual bodies remain closed and may overlap. `quality: "Low"` or `"Medium"` reduces
the source budget to 10% or 35%; `"High"` retains it. Unresolved detail fades. Runtime distance
detail uses the cluster DAG. `surface.strength: 0` disables the structural surface field, while
geological process controls remain independent.

G94's standard 1 px runtime LOD picture check passes 12 of 14 studies. `jointed-pinnacles` and
`limestone-dissolved` need 0.5 px in the measured view to pass the same image-error limit, while
still drawing only 36–38% of their finest visible cluster pairs. See the grounding page for the
full comparison and limitations; source `quality` does not automatically change the renderer cut.

G97 adds `granite-rind` and `obsidian-rind`: explicit weathered coatings over exposed fracture
faces. The brown granite coating is darker than its substrate; the obsidian coating is lighter
and rougher than its glossy breaks. These are authored contrasts, not calibrated specimens.
Change `material.surface_maps.rind` colour, roughness, amount and grain scale independently;
remove it for the same geometry and mineral field without a coating. Both keep the 2048 texture
budget. [Rind controls](../../docs/subsystems/rocks.md#weathered-exteriors-and-fresh-breaks-g97).
