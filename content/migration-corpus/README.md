# The migration corpus

Old documents and saves that every schema migration must still load ([plan 03 §3.8](../../docs/plan/03-data-model.md#38-schema-evolution): "every migration is tested against it in CI; deleting a migration is forbidden while any corpus entry needs it"). Committed as fixtures, like `content/input-logs/`: each is a file set this directory describes, and a test loads every one on every build and holds it to what this page says it must produce.

`.gitattributes` here turns off every line-ending conversion: a save's manifest hashes every byte of its files, so a checkout that turned `\n` into `\r\n` would make every save refuse to load, naming the first file it changed.

## `saves/` — save games

A save is a directory the world capability writes and reads ([world](../../docs/subsystems/world.md#save-and-load), [ADR-0042](../../docs/adr/0042-a-save-is-the-store-the-document-and-the-drivers.md)): `save.json`, the manifest, naming every other file with its size and hash and every format with the version it was written at; `document/`, the session's document directory; `world.db`, the persistent store, a SQLite backup — the one binary file, 40 KB, five 8 KiB pages; and the player's `input.jsonl` and `input-map.json`. **One save per version of the format that has existed**, starting with the one the format began at. A save is never rewritten once committed — it is what a version wrote, and the point is that a newer build still reads it.

**The test** is `apps/engine_cli/tests/save_tests.cpp`, "save corpus: every save loads, migrates, and runs on as it was measured to". For every directory here, in a fresh engine-host, it runs `session.load_game` into its own scratch directory and checks the tick, the state hash the manifest recorded, the state hash of the loaded world, and every migration step; then runs one more game second with no new input — the player keeps the log its save kept, so a key held at the save stays held — and checks the state hash after it. The table in the test and the table below are the same numbers, and the test also fails when a directory here is not in its table or a row has no directory. The hashes are `world::state_hash` ([world](../../docs/subsystems/world.md#the-persistent-state-hash)) and are the same on MSVC, Clang and GCC. A build without the kinematics capability (the `*-no-ecs` presets) cannot move the carts these worlds hold, so it refuses every save naming `engine.kinematics.Mover`, which the test checks instead.

### What each save is

Every save so far holds the same world, the one the replay test runs ([world](../../docs/subsystems/world.md#save-and-load)): a document with one partitioned layer, `places`, on a 32 m grid — 48 `engine.world.Node` places on a strip of tiles from x = −2 to 13 and z = −1 to 1, and six `engine.kinematics.Mover` carts set to drive across the tiles' edges, one of them a place's child — streamed under rings of 1.5 and 3 tiles with the inner ring simulated, round two observers: one walking +x at 20 m/s, and the player, steered by a WASD log through a `move` action at 10 m/s, that has pressed W at tick 30 and D at tick 90, released W at 160, and still holds D. Each was taken after tick 300 (game time 5,000,100 µs), with 43 tiles active.

| Save | Written by | Formats it names | Loaded tick | Hash in its manifest | Hash once loaded | Migrations | Hash after one more second |
|---|---|---|---|---|---|---|---|
| `v1` | the first save format, 2026-09-25 | `engine.world.SaveManifest` 1, `store.tables` 1, `engine.input.log` 1, `engine.world.TileProjection` 1, `engine.world.WriteBack` 1, the document's `DocumentManifest`, `LayerFile`, `Patch` 1, `engine.world.Node` 1, `engine.kinematics.Mover` 1 | 300 | `8a6d7d078e00bcbf` | `8a6d7d078e00bcbf` | none | `65b302f6a95c001d` |

### Adding a version

When a change bumps a version a save names — a field added to the manifest's types, a store table migration, a record type's `@version` — the change also adds the save the new version writes, and the older saves stay:

1. Build, and write the current version's save with the test's own helper, which writes nothing without the variable:

   ```powershell
   $env:ENGINE_SAVE_CORPUS_OUT = "content/migration-corpus/saves/v<N>"
   build/msvc-debug/apps/engine_cli/engine_engine_cli_tests.exe -tc="save corpus: write*"
   ```

2. Run the corpus test and take the new save's numbers, and the older saves' new "once loaded" and "after one more second" hashes when the change moved them (a migration that adds a column moves the store's hash of every older save it migrates, and says so as a step).
3. Add a row to the table above and to `corpus()` in the test, with the same numbers, and check them in the Linux container (`tools/linux-build.ps1 -Test -Filter cli`) before committing: a hash that differs on one compiler is a determinism bug to fix, not a number to write down.
