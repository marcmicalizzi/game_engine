# Content

- `test-scenes/` deterministic reference scenes (representative and pathological), versioned with the engine.
- `golden/` golden images and metrics by scene × resolution × GPU class. `golden/local/` is ignored by git.
- `migration-corpus/` old documents and saves that every schema migration must still load.
- `samples/` permissively licensed sample assets (Khronos glTF samples, CC0 and CC-BY only), fetched by `tools/fetch-samples.ps1` at a pinned commit and ignored by git; `samples/LICENSES.md` records each model's license. `engine-view --mesh content/samples/Suzanne/Suzanne.gltf` renders one.

`test-scenes/`, `golden/`, and `migration-corpus/` are empty until their consumers exist.
