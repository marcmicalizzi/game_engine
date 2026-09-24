# Content

- `test-scenes/` deterministic reference scenes (representative and pathological), versioned with the engine, each holding what to load, how to draw it, how many samples the reference spends, and the FLIP it is allowed to differ by. `tools/ci/reference-compare.ps1` is the gate over them; `test-scenes/README.md` is the format. The benchmark scenes of plan 09 §9.4 — a scene, a camera path and the frames to read their numbers against — are subdirectories, starting with `test-scenes/desert-overlook/`.
- `golden/` golden images and metrics by scene × resolution × GPU class. `golden/local/` is ignored by git.
- `migration-corpus/` old documents and saves that every schema migration must still load.
- `generation/` subject lists for `tools/generate.ps1` ([docs/content-generation.md](../docs/content-generation.md)): the text an image model is asked for, with a name, a category and a rough size per subject. The lists are committed so a generated set can be regenerated; **what they generate never is** — images, meshes and their provenance sidecars live under the local root outside the repository. `e10-desert-props.json` is experiment E10's twenty Desert Survival props.
- `input-logs/` recordings of input, committed as fixtures: real devices nobody here owns (`engine-input probe`), replayed by `foundation/input`'s corpus test, and in `sessions/` recorded `engine-view --interactive` sessions — so far one synthetic session and the camera trajectory it flies, the replay test's fixture. `input-logs/README.md` says what each file is.
- `input-maps/` action maps as data: `engine-view.json` is the interactive camera's default bindings, byte for byte what engine-view compiles in. `input-maps/README.md` says why changing one stops recorded sessions replaying.
- `samples/` permissively licensed sample assets (Khronos glTF samples, CC0 and CC-BY only), fetched by `tools/fetch-samples.ps1` at a pinned commit and ignored by git; `samples/LICENSES.md` records each model's license. `engine-view --mesh content/samples/Suzanne/Suzanne.gltf` renders one.

`golden/` and `migration-corpus/` are empty until their consumers exist.

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

Four more carry **morph targets**, which is what the morph stream, `engine-view --morph` and
`--morph-animate` need a real file for ([geometry](../docs/subsystems/geometry.md), "Morph
channels"). Two are CC0 and need no attribution; the other two are CC-BY-4.0:

| Model | What | Artist | Owner | Licence |
|---|---|---|---|---|
| **AnimatedMorphCube** | everything | Microsoft | Public | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| **MorphPrimitivesTest** | everything | ft-lab | ft-lab | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| **MorphPrimitivesTest** | Draco compression (the `glTF-Draco` variant, which is not fetched) | Frank Galligan | Frank Galligan | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| **MorphStressTest** | everything | Ed Mackey | Analytical Graphics, Inc. | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| **SimpleMorph** | everything | Marco Hutter | Public | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |

`AnimatedMorphSphere`, the model usually named beside these, is not in
`glTF-Sample-Assets` at the pinned commit — it stayed behind in the retired glTF-Sample-Models
repository — so it is absent rather than refused.

Anything this project publishes that shows a picture of one of them — a document, an experiment
write-up, a screenshot in a release — carries that row. The models themselves are never
committed and never redistributed; `tools/fetch-samples.ps1` downloads them from
[KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) at the
commit the script pins, and refuses any model whose `metadata.json` names a licence outside the
allow list (CC0-1.0 and CC-BY-4.0).
