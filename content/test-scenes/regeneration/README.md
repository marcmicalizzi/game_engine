# Regeneration specimen scene

The front row, left to right, shows maple, oak, birch, pine, spruce, beaked hazel and red-osier
dogwood juveniles. The back row shows a maple seedling group, suppressed spruce, pine leader
replacement, browsed hazel and an oak deflected around a spherical exclusion volume.

Recipes and their controls are in [the corpus](../../flora/regeneration/README.md).
`camera-path.json` frames the row; individual acceptance captures use saved cameras at
5, 20, 60 m and the far-form distance. The obstacle itself is not drawn by this scene.

Run `engine-view --scene content/test-scenes/regeneration/scene.json
--camera-path content/test-scenes/regeneration/camera-path.json --offscreen --frames 1
--capture <local-output.png> --time-of-day 12` under `tools/gpu-lock.ps1 run`.
Generated pictures and cooked meshes belong outside the repository.
