# test-scenes

The versioned scene corpus of [04 §4.8](../../docs/plan/04-renderer.md#48-reference-renderer-and-objective-optimization): the scenes the real-time path is compared against the reference path tracer on, and the per-scene error each comparison is allowed. `tools/ci/reference-compare.ps1` runs every file here through `render.evaluate` and fails when a scene's FLIP exceeds its threshold. See [renderer](../../docs/subsystems/renderer.md#reference-renderer) for what the reference is and [apps](../../docs/subsystems/apps.md) for the method.

**One file is one scene, and it holds everything about it**: what to load, what to draw it with, where the camera is, how many samples the reference spends, and how far the two pictures may differ. Splitting the threshold into a second file beside the scene was the other option and it buys nothing: a threshold is a property of *this scene at this resolution from this camera*, and a scene whose camera moved needs a new threshold in the same commit.

```json
{
  "format": "engine.test-scene.v1",
  "name": "flighthelmet",
  "description": "one Khronos FlightHelmet: textured dielectrics, metal buckles, normal maps",
  "requires": ["content/samples/FlightHelmet/FlightHelmet.gltf"],
  "load": { "mesh": "content/samples/FlightHelmet/FlightHelmet.gltf" },
  "settings": { "raster": "hw", "shadows": "rt" },
  "orbit": { "distance": 22 },
  "width": 640, "height": 480, "frame": 0,
  "reference": { "spp": 1024, "bounces": 3, "batch": 32 },
  "thresholds": { "flip_mean": 0.10, "flip_p95": 0.30 }
}
```

| Field | What it is |
|---|---|
| `requires` | files that must exist for the scene to run, relative to the repository root. A missing one **skips** the scene rather than failing it: the Khronos samples are fetched by `tools/fetch-samples.ps1` and git ignores them, so a fresh clone has none of them. |
| `load` | merged into `render.load`'s parameters — `mesh`, `scene`, `grid`, `grid_instances`, `ddc`, `cache`. |
| `settings` | `render.load`'s `RenderSettings`. **The reference traces the acceleration structures the real-time frame builds**, so a scene has to ask for settings that build them: `"shadows":"rt"` (or `"raster":"rt"`). |
| `orbit` / `camera` | the camera, in either of `render.evaluate`'s two forms. |
| `width`, `height`, `frame` | the resolution the comparison is fixed at, and the frame number that positions the orbiting lights. |
| `reference` | `spp`, `bounces`, `finest`, `seed`, `batch`. The samples are chosen from the measured noise floor ([renderer](../../docs/subsystems/renderer.md#reference-renderer)). |
| `thresholds` | `flip_mean` and `flip_p95` the comparison must stay under. |

**The thresholds are large, and that is the point.** The real-time path has **no global illumination at all** — one hemisphere ambient term where the reference integrates a hemisphere and carries light that bounced off the rest of the scene — so the two pictures cannot meet, and a threshold set where they would meet would only mean "this gate never passes". They are set from today's measurement with margin, and what they catch is a **regression**: a change that makes the real-time picture worse than it is now. The report says how much of each scene's error is indirect light by comparing the same real-time picture against the reference at a single bounce as well, so a threshold that looks alarming can be read rather than guessed at.

**Today's six.** The pathological scenes §4.8 also asks for — 10⁶ emitters, a mirror room, 10⁵ instances, foliage walls — are open work, and so is a surround (`"views":"surround3"`) case: the reference renders one view.

**One of them compares two different cuts, and the reason is a lesson.** A corpus that only ever renders the real-time path and the reference at the *same* LOD threshold cannot see a LOD defect at all: both sides draw the same wrong triangles. `shredded-atlas` sets `"reference": {"finest": true}`, so the reference traces the source geometry while the real-time path draws a coarse cut of it, and the threshold is over the difference between them. Any scene here can be turned into that kind of gate with one field; this is the first one that is.

| Scene | What it is for | Needs a sample |
|---|---|---|
| `heightfield` | the scene the engine can build with nothing at all, so this one runs on a fresh clone and on a machine with no content | no |
| `shredded-atlas` | the procedural UV-atlas stress fixture ([geometry](../../docs/subsystems/geometry.md#the-shredded-atlas)) drawn at a **coarse** LOD threshold against a reference traced from the **finest** geometry (`"reference":{"finest":true}`). It is the only scene here that compares two different cuts, which is the comparison a LOD defect survives — every other scene compares the same cut to itself, and that is how simplification across atlas islands went unnoticed until 2026-09-19 | no |
| `flighthelmet` | textured dielectrics, metal, normal maps, a self-shadowing shape | yes |
| `helmet-grid` | 3×3 instances: a deep LOD cut, many bottom-level structures, inter-object shadows | yes |
| `thin-geometry` | the Khronos Lantern's thin frame and rope, where triangles fall under a pixel and a rasterized edge and a traced one disagree | yes |
| `wide-frustum` | 1920×360, a 48:9 frustum: the aspect the project owner plays at, in one view | yes |
