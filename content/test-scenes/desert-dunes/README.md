# desert-dunes

The [desert overlook](../desert-overlook/README.md)'s terrain drawn by the terrain capability's dune generator ([terrain](../../../docs/subsystems/terrain.md), [ADR-0043](../../../docs/adr/0043-dunes-as-a-function-of-time.md)) instead of the waves: the same 5.1 km, seed, ridges and oasis basin, `"generator": "Dunes"` at three years of game time (`"time": 94608000`), and since the owner's first flight ([third session](../../../docs/experiments/third-interactive-session-2026-09-25.md): "the dunes feel small") the **erg profile's band table** of [desert-erg](../desert-erg/README.md) — mega-draa 80–200 m about 2.4 km apart, draa and crests on their flanks, barchans on the floors, waves everywhere and open floors between — in place of the three bands the dune height alone used to give. It has no meshes, so it needs no samples, and it is flown along the overlook's own camera path (`camera_path` points at it), whose keys hold heights above the terrain and so follow the new ground.

It is the scene for the owner's visual acceptance of the dunes, in an interactive fly-through on a machine with a GPU:

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-dunes/scene.json --interactive
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-dunes/scene.json --frames 1 --capture dunes.png
build/msvc-release/bin/engine-content terrain content/test-scenes/desert-dunes/scene.json --tile 0,0 --time 94608000
```

**What to look at.** The erg's scales at once over the overlook's ground: two or three mega-draa standing 100–200 m over the floors, their long slip faces downwind and in shade, draa and crest segments on their flanks, barchans and waves on the floors, and roughly two fifths of the ground within half a metre of the floor; crests bowing round the ridges (the lattice is held back upwind of rock), which the tall dunes now dwarf; flat sand in the basin. The camera path was laid for the old, 8 m field: its keys hold heights above the ground, so it follows the new one, but it now climbs a mega-draa's flank near its middle. **Changing `time`** moves the dunes and nothing else: the mega-draa about a metre a year, the waves hundreds of metres (the erg's README has the table). What the generator cannot do yet is written in [terrain](../../../docs/subsystems/terrain.md#not-yet).

Nothing here is pinned by a test that needs a GPU; `engine-content terrain` reads the file in `apps/engine_content/tests/terrain_tests.cpp`.
