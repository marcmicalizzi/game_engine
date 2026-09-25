# desert-dunes

The [desert overlook](../desert-overlook/README.md)'s terrain drawn by the terrain capability's dune generator ([terrain](../../../docs/subsystems/terrain.md), [ADR-0043](../../../docs/adr/0043-dunes-as-a-function-of-time.md)) instead of the waves: the same 5.1 km, seed, dune height and wavelength, ridges and oasis basin, and `"generator": "Dunes"` at three years of game time (`"time": 94608000`). It has no meshes, so it needs no samples, and it is flown along the overlook's own camera path (`camera_path` points at it), whose keys hold heights above the terrain and so follow the new ground.

It is the scene for the owner's visual acceptance of the dunes, in an interactive fly-through on a machine with a GPU:

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-dunes/scene.json --interactive
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-dunes/scene.json --frames 1 --capture dunes.png
build/msvc-release/bin/engine-content terrain content/test-scenes/desert-dunes/scene.json --tile 0,0 --time 94608000
```

**What to look at.** Transverse draa two wavelengths apart with crest segments riding on them and sparse barchans on the flats; slip faces on the downwind side at the angle of repose after the windy season and rounded after a calm one; crests bowing round the two ridges (the lattice is held back upwind of rock); flat sand in the basin. **Changing `time`** moves the dunes and nothing else: in this wind (200 m² a year over 8 m dunes) the draa move about 25 m a year, the crest segments about 55 and the barchans about 77, downwind of a prevailing direction the seed draws — set it a day, a month and a year apart and compare. What the generator cannot do yet is written in [terrain](../../../docs/subsystems/terrain.md#not-yet), the first item of which — slip faces steeper than the angle of repose where two bands' coincide — is the thing most likely to be visible.

Nothing here is pinned by a test that needs a GPU; `engine-content terrain` reads the file in `apps/engine_content/tests/terrain_tests.cpp`.
