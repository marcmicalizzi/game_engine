# Soil feature studies

The recipes here are input to `engine-content soil-cook`. Generated terrain, meshes,
scenes, provenance and captures belong outside the repository:
`D:/workspace/game_engine_local/soil/2026-10-28/`.

First make a 32 m square source with 125 mm spacing:

```powershell
engine-content terrain-synth <local>/source.json --kind flat --size 257 --spacing-mm 125 --origin-mm -16000,-16000
engine-content soil-cook content/test-scenes/soil-features/undercut.soil.json <local>/source.json <local>/undercut 0 0 0
```

Repeat for scarp, gully, tree-throw and weathered-throw. Each cook supplies a scene
with the feature and its actual terrain excavation. Open its scene offscreen in
engine-view and capture at 5, 20 and 60 m, sun azimuth/elevation 120,35 degrees,
with the camera and recipe saved beside the captures. Inspect the undercut from
below its lip as well as above it. The source has no textures; a material pack can
supply the six roles documented in [soil](../../../docs/subsystems/soil.md).
The POC's source terrain and scripts are not modified by these studies.

The recorded study also includes ground-only/feature benchmark pairs, a 20 m finest-threshold comparison, an underside view, and a 64,000 km shifted undercut. See [measurements and known limits](../../../docs/experiments/soil-banks-tree-throw.md#validation); the far-origin view exposes F24's missing CSM casters.
