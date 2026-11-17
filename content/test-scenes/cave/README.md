# cave

**One passage under a shaft at noon: the world-space cache's darkest case** (roadmap G213, R171;
[renderer](../../../docs/subsystems/renderer.md#indirect-light-the-world-space-cache),
[caves](../../../docs/subsystems/caves.md)). Light reaches the passage only through a hole 3 m
across at the top of a 4 m shaft, so a cosine ray from its walls finds the light it carries
seldom, and the bounce's cells average a few heavy samples among many dark ones: the case where
the cache's grain and its change from frame to frame are largest (R171's three legs measured it
here, on a copy of this scene kept in scratch).

## What is in it

- `passage.cave.json`: one `engine.caves.passage` (seed 7) — the entrance at the origin, a
  vertical shaft of 4 m, then a passage 5 m wide and 3 m high along `(3, -5.5, 0)`,
  `(9, -6, 0)`, `(15, -6.5, 1.5)`, rough to 4 %. Built at load by the caves capability's mesh
  generator; no ground, so the passage stands in open air and only its shaft lets light in.
- `scene.json`: that one instance, and a sky at 40° north on the longest day. **Pass
  `--time-of-day 12`**: the scene has no terrain to give it an hour, and noon is the sun nearly
  down the shaft.
- `held.json`: twelve seconds held in the passage 9 m from the shaft, looking down its wall. Nothing
  moves, so every change between frames is the picture's own.
- `walk.json`: six seconds from under the shaft into the passage at a walk, turning to the held
  view's wall, then six held there (markers at the stop, frame 360, and the end).

## Flying it

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/cave/scene.json `
  --camera-path content/test-scenes/cave/held.json --time-of-day 12 --offscreen `
  --width 3840 --height 2160 --raster rt --shadows rt --tunable renderer.gi.mode=bounce `
  --temporal cave-held.json --temporal-from 360
```

`--temporal` writes the change between consecutive displayed frames from frame 360 on, by luma
band, and each frame's mean level (renderer.md, "The change between frames, as the eye sees it");
the walk is read from frame 0 and its level series after the stop at frame 360. A change to the
cache is flown here and on the village's two interior paths
(`../village/interior-held.json`, `../village/interior-door.json`).
