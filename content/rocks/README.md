# content/rocks

Rock recipes for the rock solver ([rocks](../../docs/subsystems/rocks.md), roadmap G62): each
`<name>.rock.json` is an `engine.rocks.RockFile` — a shape and its parameters — that a scene's mesh
table, a scatter layer's kind or `engine-view --mesh` names like a glTF, built through the
derived-data cache. Nothing binary lives here: the meshes are solved from these numbers.

| Recipe | Shape | What it is | Triangles (budget) |
|---|---|---|---|
| `granite-boulder.rock.json` | Boulder | a 2.6 × 1.8 × 2.1 m granite block with three weathered joint sets | 76,800 (80,000) |
| `sandstone-boulder.rock.json` | Boulder | a 3.2 × 2 × 2.4 m sandstone block with nine beds and fine fracture relief | 110,592 (120,000) |
| `basalt-outcrop.rock.json` | Outcrop | seven basalt columns sharing joint weathering in a 9 × 5.2 × 7 m envelope | 258,048 (320,000) |
| `granite-scree.rock.json` | Scree | fourteen sharp granite fragments up to 0.44 m over a 2.8 × 2 m patch | 24,384 (32,000) |
| `jointed-pinnacles.rock.json` | Outcrop | eight tall jointed granite bodies, a formation study inspired by the owner's photographs | 352,512 (400,000) |
| `shore-rubble.rock.json` | Scree | forty-eight angular blocks over a 7 × 5 m patch, one reusable group | 265,344 (320,000) |
| `sandstone-escarpment.rock.json` | Cliff | a sandstone face 298 m along a gentle bend, 24 to 44 m high, undercut, a talus skirt and thirty fallen blocks ([rocks](../../docs/subsystems/rocks.md#cliffs)) | 162,064 (250,000) |

The original G62/G71 recipes passed E10's four thresholds (rocks.md, "What the harness taught");
that historical result does not qualify the G89 revisions. Current review and validation are in
[rock structure](../../docs/experiments/rock-structure.md). Each one's mesh is
pinned by hash in `domain/rocks/tests/rocks_tests.cpp`: change a number here and that test says
so — update the pin in the same commit. [`content/test-scenes/rock-field/`](../test-scenes/rock-field/README.md)
places the four original boulder/outcrop/scree recipes. The two new groups can be placed as one
mesh instance each. Grouping is an authoring convenience, not a rockfall-settling simulation:
individual bodies remain closed and may overlap. `surface.strength: 0` and a smaller explicit
triangle budget provide simple source meshes; runtime distance detail uses the cluster DAG.
