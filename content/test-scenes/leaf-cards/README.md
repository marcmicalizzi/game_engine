# leaf-cards

Four solver oaks (`engine.flora.Broadleaf`, the grove's recipe) whose leaf cards sample a
procedural leaf with its outline in alpha: the scene for alpha-tested geometry (roadmap R65,
[renderer](../../../docs/subsystems/renderer.md#alpha-tested-geometry)). A card set is a cutout
([materials](../../../docs/subsystems/materials.md)), so every leaf card draws, casts and traces
nothing where the leaf's alpha is below 0.5, and the content build keeps the leaf's coverage down
its mips and grows its colour under the cut ([texture](../../../docs/subsystems/texture.md#alpha-tested-masks)).

The leaf is **written by a command, not committed** (`.gitignore` keeps it out): a 512x512 TGA with
straight alpha — a pointed blade along the card's *u* (the twig axis), a stem at *u* = 0, a darker
midrib, and black under the cut, which is what the build's dilation is for. No lab image and no
`tools/generate.ps1` are involved; when the lab's `broadleaf/leaf` set arrives
([content/materials/README.md](../../materials/README.md)) it replaces this one by the same path.

```powershell
pwsh content/test-scenes/leaf-cards/make-leaf.ps1     # writes leaf.tga beside it, about 10 s
build/msvc-release/bin/engine-view --scene content/test-scenes/leaf-cards/scene.json --camera-path content/test-scenes/leaf-cards/camera-path.json --offscreen --benchmark leaf-cards.jsonl
```

Without `leaf.tga` the masked recipe's cards draw the flat colour, alpha-tested against the factor
alone, which keeps every pixel.

`scene-opaque.json` is the same four oaks from `oak-opaque.flora.json` — the recipe with no
materials block, so its cards are the flat-colour opaque parallelograms the grove draws — and is
the baseline the masked scene's costs in renderer.md are measured against. `camera-path.json` is the
grove's: once round at 34 m, then a walk in among the trunks.
