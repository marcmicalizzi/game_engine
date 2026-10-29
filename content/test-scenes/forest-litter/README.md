# Forest litter study

`study.json` lays out six kinds at metre scale on flat ground. `scene.json` places mixed piles
between stumps, with hollows and obstacle-edge bias; visible soil separates the finite patches.
`camera-path.json` moves from 5 through 20 to 60 m, at low to moderate viewing height.
All source recipes are in [content/litter](../../litter/README.md).

Build this worktree's `engine-view`, then use `--offscreen --scene <scene.json> --camera-path
<camera-path.json> --capture <external-output>.png`. Keep captures, exact camera, recipe and
measurement logs under `D:/workspace/game_engine_local/captures/<date>/forest-litter/`.
The mixed scene's 1.3 m extent is conservative for its small falls; changing source dimensions
requires checking that bound again. No POC script or scene is modified.
