# overgrown

Three hosts and their companions (roadmap G76 and G78; [cover](../../../docs/subsystems/cover.md),
grounded in [vine-cover](../../../docs/experiments/vine-cover.md)): a granite boulder from the rock
solver (`content/rocks/granite-boulder.rock.json`), a 10 m wall of the ruins' kit of boxes
(`kit/section-10m-h100.glb`) and the oak from the flora solver (`content/flora/oak.flora.json`),
each with ivy grown over it (`content/cover/ivy-*.vine.json`) and moss, lichen and algae solved over
it (`content/cover/moss-*.cover.json`). Every companion is placed with its host's translation and
`ground` flag, and every recipe's site gives the renderer's default sun (azimuth 138.4° from north,
48.9° up). Ten instances with the terrain, 7,294 pairs; `camera-path.json` walks round the three at
eye height for 30 s, past the wall's sunny south face and its shaded north one.

The wall's kit is generated, never committed (AGENTS.md: never commit binary assets), and `kit/` is
ignored beside this file:

```powershell
build/msvc-release/bin/engine-content ruins-kit content/test-scenes/overgrown/kit
build/msvc-release/bin/engine-view --scene content/test-scenes/overgrown/scene.json --camera-path content/test-scenes/overgrown/camera-path.json --offscreen --benchmark overgrown.jsonl
```

**What it costs** (msvc-debug, RTX 5090, 1920×1080, traced shadows, 2026-10-21; the machine was not
quiet — `machine_state` recorded 50% CPU from other processes and the GPU at 6–7% from others — so
the numbers are upper bounds): GPU total 0.54 ms median and 0.72 ms p95 along the path, frame 0.83
ms median, the deform pass 0.022 ms. The companions' triangles and what each solver made are in
[vine-cover](../../../docs/experiments/vine-cover.md#the-overgrown-scene-what-it-costs).
