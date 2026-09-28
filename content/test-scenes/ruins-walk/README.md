# ruins-walk

One ruined building of the **kit of boxes** ([ruins](../../../docs/subsystems/ruins.md#the-synthetic-kit)) on tile (0, 0) — world seed 7, 32 m tiles, the wind from +x — over a gentle sand floor of the renderer's own waves (40 cm high, 30 m long, 128 m a side at 1 m). It is the scene the walk mode's end-to-end case walks into ([apps](../../../docs/subsystems/apps.md#walking)): `apps/engine_view/tests/walk_ruins_view_tests.cpp` copies this file into its scratch directory, writes the kit beside it, reads the scene to find one of the building's full-height walls, and starts a walker five metres outside it, facing it, holding W for six seconds: where the build has physics the walker stops at the wall's face; with the ground only, it walks through.

The kit is generated, never committed (AGENTS.md: never commit binary assets). To look at the scene yourself:

```powershell
build/msvc-release/bin/engine-content ruins-kit content/test-scenes/ruins-walk/kit
build/msvc-release/bin/engine-view --scene content/test-scenes/ruins-walk/scene.json --interactive --walk --start 16,20,40 0,0
```

`kit/` is ignored beside this file (`.gitignore`), so the generated members stay out of the tree.
