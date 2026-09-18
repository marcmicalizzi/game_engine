# Content

- `test-scenes/` deterministic reference scenes (representative and pathological), versioned with the engine.
- `golden/` golden images and metrics by scene × resolution × GPU class. `golden/local/` is ignored by git.
- `migration-corpus/` old documents and saves that every schema migration must still load.
- `samples/` permissively licensed sample assets (Khronos glTF samples, CC0 and CC-BY only), fetched by `tools/fetch-samples.ps1` at a pinned commit and ignored by git; `samples/LICENSES.md` records each model's license. `engine-view --mesh content/samples/Suzanne/Suzanne.gltf` renders one.

`test-scenes/`, `golden/`, and `migration-corpus/` are empty until their consumers exist.

## Attribution for the CC-BY sample models

`samples/LICENSES.md` is generated beside the assets, and both are git-ignored — so the
attribution that CC-BY actually requires would leave the repository with them. It is recorded
here as well, in the one file that is committed, because a licence condition is not something a
`.gitignore` may quietly drop.

Two of the fetched models are skinned and animated, which is what `engine-view --animate`
([apps](../docs/subsystems/apps.md)) needs, and both carry a CC-BY-4.0 term:

| Model | What | Artist | Licence |
|---|---|---|---|
| **Fox** | model | PixelMannen | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| **Fox** | rigging and animation | tomkranis | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| **Fox** | conversion to glTF | @AsoboStudio and @scurest | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| **RiggedFigure** | everything | Cesium | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |

Anything this project publishes that shows a picture of one of them — a document, an experiment
write-up, a screenshot in a release — carries that row. The models themselves are never
committed and never redistributed; `tools/fetch-samples.ps1` downloads them from
[KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) at the
commit the script pins, and refuses any model whose `metadata.json` names a licence outside the
allow list (CC0-1.0 and CC-BY-4.0).
