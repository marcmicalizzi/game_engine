# content/rocks

Rock recipes for the rock solver ([rocks](../../docs/subsystems/rocks.md), roadmap G62): each
`<name>.rock.json` is an `engine.rocks.RockFile` — a shape and its parameters — that a scene's mesh
table, a scatter layer's kind or `engine-view --mesh` names like a glTF, built through the
derived-data cache. Nothing binary lives here: the meshes are solved from these numbers.

| Recipe | Shape | What it is | Triangles (budget) |
|---|---|---|---|
| `granite-boulder.rock.json` | Boulder | a 2.6 × 1.8 × 2.1 m granite block, three joint sets, rounded edges | 19,200 (20,000) |
| `sandstone-boulder.rock.json` | Boulder | a 3.2 × 2 × 2.4 m sandstone loaf with nine beds of uneven depth | 19,200 (24,000) |
| `basalt-outcrop.rock.json` | Outcrop | seven basalt columns, hexagonal and flat-topped, on one bedding plane, 9 × 5.2 × 7 m | 50,304 (60,000) |
| `granite-scree.rock.json` | Scree | fourteen sharp granite fragments up to 0.44 m over a 2.8 × 2 m patch | 13,152 (16,000) |

Each passes E10's four thresholds (rocks.md, "What the harness taught"), and each one's mesh is
pinned by hash in `domain/rocks/tests/rocks_tests.cpp`: change a number here and that test says
so — update the pin in the same commit. [`content/test-scenes/rock-field/`](../test-scenes/rock-field/README.md)
places all four.
