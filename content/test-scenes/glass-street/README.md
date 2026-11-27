# glass-street

**A street between two walls of glass** (roadmap R71, [reflections on smooth surfaces](../../../docs/subsystems/renderer.md#reflections-on-smooth-surfaces); the Toronto proof of concept's gap that curtain-wall glass reflected a flat sky tint): the fixture of a smooth surface reflecting the scene, with nothing in it the reflection does not need.

- **The street**: one unit cube (`pavement.gltf`, outward faces) scaled to a slab 60 m x 80 m, 0.2 m thick, its top at y = 0, roughness 0.9.
- **The east panes** (`glass.gltf`): a wall 10 m high and 28 m long whose face is the plane x = 7, roughness **0.06**, a dark base colour (0.04, 0.05, 0.06) — glass as an opaque dielectric, its reflection the whole of what it shows.
- **The honed and the rough stone** (`fade.gltf`, `rough.gltf`): two panels 10 m long continuing the same wall up the street, grey (0.35, 0.35, 0.37) at roughness **0.15**, inside the reflection's fade (0.10 to 0.20), and **0.45**, past it, so the picture shows the fade and a surface that keeps the ambient.
- **The west panes**: the same glass facing them across the 14 m street, 48 m long. No view of the camera path sees it; it is in the east panes' reflection.
- **The red box** (`red.gltf`, 2 m x 3 m x 2 m, red paint at 0.7): behind the eye's start and outside its view, in the east panes' reflection — the object a reflection limited to the frame's cut would lose.
- **The sky**: the sky capability's on day 140 at 43.65 N (Toronto's latitude) its epoch midnight: the cases draw it at **10:00** (`FrameDesc::sky_time_s`, `--time-of-day 10`), the morning sun from the east on the red box's face towards the east panes, which stand in their own shade; the cost was flown at 14:00, the panes in the sun.
- **The camera path** (`camera-path.json`): held at 1.7 m in the street looking at the east panes 25 degrees up the street, the panes over about half of the frame, for four seconds with a marker at frame 120.

The cubes are lantern-street's (`block.gltf`, the same 912-byte buffer inline), each with its own material, and the scene is text, written by a scratch script outside the tree from the numbers above. Written for this repository and under its licence (Apache-2.0, the root `LICENSE`), as everything in the tree is unless a `LICENSES.md` beside it says otherwise.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/glass-street/scene.json --offscreen --width 3840 --height 2160 --raster rt --shadows rt --audio off --time-of-day 10 --camera-path content/test-scenes/glass-street/camera-path.json --tunable renderer.reflect.surfaces=2 --benchmark glass.jsonl
```