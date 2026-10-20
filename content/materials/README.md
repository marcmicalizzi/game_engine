# content/materials: the material pack

The default tiled PBR sets the tree and rock solvers fall back to, one per **role**, when a recipe
names no set of its own for a part ([materials](../../docs/subsystems/materials.md),
[flora](../../docs/subsystems/flora.md#materials), [rocks](../../docs/subsystems/rocks.md#uvs-and-materials)).
Everything here but this file is **unpacked by `tools/fetch-materials.ps1` and ignored by git**:
the sets are generated images, and generated images are never committed (AGENTS.md,
[content generation](../../docs/content-generation.md#material-packs)). A checkout without a pack
draws every palm, tree and rock in the flat colours it always had, with one log line per role.

## Fetching

```powershell
tools/fetch-materials.ps1 -Source <zip path or URL> -Expect <sha256>
```

The zip must hash to `-Expect` (or to the hash pinned in the script, once the owner hosts a
default), and every map to what `pack.json` says, or nothing is unpacked. What was here is replaced;
this README stays. **No default is hosted yet**: the first pack is built locally under
`D:\workspace\game_engine_local\packages\` and fetched from there.

The repository's own hosts find the pack here, beside the derived-data cache: `engine-content
flora`, `rocks`, `build` and `build-all`, and the renderer when it looks up `<repo>/ddc`
(engine-view, engine-host). Elsewhere: `engine-content ... --materials <dir>` and `engine-view
--materials <dir>`; `--no-materials` for a flat build. A pack is in a generated mesh's cache key, so
fetching one rebuilds the generated meshes (a second each) and their textures.

## Packing

```powershell
tools/pack-materials.ps1 -From <staging dir> -Out <zip> [-Name <name>] [-Version <date>] [-Licence <text>]
```

The staging directory is laid out by role, `<From>/<species>/<part>/`, each with the five maps —
`base_color.png` (or the lab's `basecolor.png`), `normal.png`, `roughness.png`, `metallic.png`,
`height.png` — the lab's `material.json` (`tile_m` or `tile_width_m`, `normal_convention`, the
normal strength as `normal_scale` or `normal_strength_preview`) and a `provenance.json`:

```json
{"models": [{"name": "StableMaterials", "revision": "4d452731..."}, {"name": "PBRify UpscalerV4", "revision": "v4"}],
 "seed": 81231, "settings": {"steps": 50, "guidance": 7.5, "tileable": true}}
```

It refuses a missing map, a side that is not a power of two, a normal map that is not OpenGL (+Y),
and a model with no licence line in the script (StableMaterials and PBRify today; the Krea pass is
not, until its terms are read). Stage and pack outside the repository.

## pack.json

```json
{
  "format": "engine.materials.pack/1",
  "name": "palm-defaults", "version": "2026-10-19", "licence": "...",
  "roles": {
    "palm/bark": {
      "tile_m": 0.35, "normal_scale": 0.4, "normal_convention": "opengl",
      "maps": {
        "base_color": {"file": "palm/bark/base_color.png", "sha256": "...", "width": 2048, "height": 2048},
        "normal": {"file": "palm/bark/normal.png", ...},
        "roughness": {...}, "metallic": {...}, "height": {...}
      },
      "provenance": {"models": [...], "seed": 81231, "settings": {...}, "lab_id": "bark-a", "lab_prompt": "..."},
      "licence": "StableMaterials: OpenRAIL-M ... PBRify UpscalerV4: CC0-1.0 ..."
    }
  }
}
```

The engine reads `format`, `name`, `version`, and per role `tile_m`, `normal_scale`,
`normal_convention` and the five `file`s; the hashes, sizes, provenance and licences are for
`fetch-materials.ps1` and for people. `LICENSES.md` beside it lists every role with the models that
made it and their licence lines, and goes wherever the pack goes (the game SDK, a game package).

## The roles, and what the lab must make for each

Every set: **tileable** in both directions, sides **powers of two**, base colour sRGB, the other four
linear 8-bit greyscale (normal RGB), the normal map **OpenGL tangent space (+Y)**, the height map
relative and uncalibrated (it is carried, not drawn yet), metallic an authored zero for every part
below (bark, leaves and stone are not metal; the engine multiplies it by zero anyway). `tile_m` is
the surface one repeat covers, in metres, and the UVs it lands on are below.

| Role | Lands on | UVs | Resolution | `tile_m` | State |
|---|---|---|---|---|---|
| `palm/bark` | the trunk, its apex and the collar of frond bases | metres, *u* round, *v* up | 2K | 0.35 (proposed) | the lab's candidates (2026-10-19); Bark A the starting pick |
| `palm/frond` | the green fronds (geometric leaflets, one island a frond) | metres, *u* **along the rachis**, *v* across | 1K | 0.12 (proposed) | candidates, turned so the image's *u* runs along the frond; Green C the pick |
| `palm/skirt` | the dead fronds | as the fronds | 1K | 0.12 (proposed) | candidates; Dry B the pick |
| `broadleaf/bark` | the oak's trunk, limbs and branches (tubes) | metres, *u* round, *v* along | 2K | about 0.5: a broadleaf's ridges are a few centimetres apart | **not made** |
| `broadleaf/leaf` | the oak's leaf cards, **one leaf per image, whole** | the card: *u* along the twig, *v* across, 0 to 1 | 512 or 1K | unused (cards take the image whole) | **not made**; wants an alpha mask, which R65 will read |
| `conifer/bark` | the spruce's trunk and branches | metres, as the broadleaf | 2K | about 0.3: thin, flaking plates | **not made** |
| `conifer/needles` | the spruce's needle sprays, **one spray per image** | the card, as the leaf | 512 or 1K | unused | **not made**; wants alpha, as the leaf |
| `rock/granite` | boulders, outcrops and scree of granite | the box projection's charts in **whole repeats** | 2K | 1.0, or the size of the grain wanted; a small repeat raises the UVs | **not made** |
| `rock/sandstone` | sandstone | as granite | 2K | 1.0 | **not made**; the lab's generic set A stood in for the first proof |
| `rock/basalt` | basalt | as granite | 2K | 1.0 | **not made** |
| `rock/limestone` | limestone | as granite | 2K | 1.0 | **not made** |

A species the solver gains later (a birch, a willow) is `<species>/bark` and `<species>/leaf` by the
same scheme. What reads as **wrong** on the mesh, which the lab cannot see without it: a bark set
with features larger than a third of its repeat repeats visibly up a 14 m trunk (the palm candidates'
"repeated marks remain visible" note), a frond set whose grain runs across the image's *u* runs
across the leaflets, and a rock set with a strong single direction shows the box projection's six
charts. The lab's preview normal strengths (0.4 bark, 0.25 fronds) are carried as `normal_scale`;
a set baked at full strength says 1.
