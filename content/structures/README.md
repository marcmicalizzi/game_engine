# content/structures

Building descriptions, each a `<name>.structure.json` file holding an `engine.structure.Recipe`
([structures](../../docs/subsystems/structures.md)). A scene names one as a mesh, as it names a
`.rock.json` or a `.flora.json`, and the mesh generator `structure` builds its container.

**This directory is the mesher's conformance corpus** (plan 14 §14.1): `structures_tests.cpp`
reads every file here and holds every opening clear where its description puts it, the wall solid
beside it, and every floor at its height. A mesher change may move triangles; it may not move a
room or a door, and this is where that is checked. Add a description that exercises something
new (a plan shape, a roof, a stair) in the change that makes the mesher able to draw it.

| File | What |
|---|---|
| `cot.structure.json` | The example: a one-room cot 6 by 4 m inside walls 60 cm thick, open to a thatched gable roof, a door and two windows, a hearth on one gable. The same numbers as `structures::example_cot()`; the test holds the two equal. |
| `loft.structure.json` | Two storeys under one roof: a hall and a parlour parted by an inner wall with a doorway, a straight stair up to a chamber in the roof, a window high in a gable. |
