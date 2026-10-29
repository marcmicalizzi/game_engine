# Wetland study

`scene.json` shows eight forms in rows at authored size, plus dead reeds.
`patches.json` demonstrates irregular boundaries. All geometry is procedural.
Floating blades sit at .502 m: a form study without simulated water.

`camera-path.json` views the row centre from 5, 20, 60 and 120 metres.
Use this branch's `engine-view --offscreen --capture`. Save copied recipes,
cameras and captures under `D:/workspace/game_engine_local/wetland/2026-10-29/`.
See [wetland](../../../docs/subsystems/wetland.md) and
[research](../../../docs/experiments/wetland-plants.md).

`pond.json` places floating leaves, arrowhead and cattail on the existing water capability's static .5 m surface. It demonstrates the anchor/depth convention; it adds no water simulation.

Use `--water-quality low` for the groundless pond capture; the default high tier
needs a different study setup. Arrowhead's root is at .35 m in the shallow-water
example, while the two floating-leaf anchors are at zero.

