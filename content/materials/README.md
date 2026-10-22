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
| `broadleaf/leaf` | the oak's leaf cards, **one leaf per image, whole** | the card: *u* along the twig, *v* across, 0 to 1 | 512 or 1K | unused (cards take the image whole) | **not made**; **the mask is the base colour's alpha** (RGBA PNG, straight alpha: 255 on the leaf, 0 off it, a texel or two of soft edge), cut at **0.5**; the colour under the cut may be anything, because the build dilates the leaf's colour into it — leave it black rather than painting a halo |
| `conifer/bark` | the spruce's trunk and branches | metres, as the broadleaf | 2K | about 0.3: thin, flaking plates | **not made** |
| `conifer/needles` | the spruce's foliated-shoot cards, **one shoot per image** (G80): a twig with its side shoots and needles all round them, its axis along *v* from the card's base | the card, as the leaf | 512 or 1K | unused | **not made**; a mask as the leaf's, cut at 0.5: about **25–35 % of the square opaque**, nearly solid inside the shoot's outline, every needle at least two texels wide at 512, and **irregular** (needles of varied length, angle and spacing) — a periodic fishbone of thin needles (17 % opaque) is what made the first forest's spruces read as bare branches, since the build's coverage-preserving mips cannot keep a periodic mask ([conifer shoots](../../docs/experiments/conifer-shoots.md#what-a-packs-needle-image-should-be)) |
| `shrub/bark` | a shrub's stems and shoots (hazel's smooth, lenticelled grey-brown bark) | metres, *u* round, *v* along | 1K | about 0.15: a stem is a few centimetres round | **not made** |
| `shrub/leaf` | a shrub's leaf cards, **one leaf per image, whole** (a hazel leaf: broadly ovate, toothed, 6–12 cm) | the card, as the broadleaf's leaf | 512 | unused | **not made**; wants alpha (R65) |
| `fern/frond` | a fern's pinna cards and its stipe's ribbon, **one pinna per image, whole** (a male fern's pinna: lanceolate, cut into pinnules) | the card: *u* along the rachis (a card a unit), *v* out along the pinna | 512 | unused | **not made**; the pinnules are the image's, so it wants alpha (R65) |
| `grass/blade` | a grass tuft's blades, **one blade per image, whole** (linear, ribbed, tapering to a point; base green to a paler tip) | the card: *u* across the blade, *v* from base (0) to tip (1) | 256 × 1K | unused | **not made**; wants alpha (R65) for the tip |
| `herb/leaf` | a herb's leaf cards and its stem (a corner of the card), **one leaf per image, whole** | the card: *u* across, *v* from base (0) to tip (1) | 512 | unused | **not made** |
| `herb/flower` | a herb's petal cards, **one petal per image, whole**, white or pale so the recipe's colour can tint it (a daisy's ray, a foxglove's bell lobe) | the card: *u* across, *v* from the flower's centre (0) to the petal's tip (1) | 256 | unused | **not made**; no recipe block names it, so the pack's role or the flat colour |
| `rock/granite` | boulders, outcrops and scree of granite | the box projection's charts in **whole repeats** | 2K | 1.0, or the size of the grain wanted; a small repeat raises the UVs | **not made** |
| `rock/sandstone` | sandstone | as granite | 2K | 1.0 | **not made**; the lab's generic set A stood in for the first proof |
| `rock/basalt` | basalt | as granite | 2K | 1.0 | **not made** |
| `rock/limestone` | limestone | as granite | 2K | 1.0 | **not made** |
| `vine/stem` | a climbing plant's stems ([cover](../../docs/subsystems/cover.md#the-vine): ivy's young green-brown stems with their rootlets) | metres, *u* round, *v* along, as a shrub's bark | 1K | about 0.1: a stem is a centimetre or two round | **not made** |
| `vine/leaf` | a vine's leaf cards, **one leaf per image, whole** (ivy's juvenile leaf: palmately three- to five-lobed, 4–10 cm, dark glossy green) | the card | 512 | unused | **not made**; wants alpha (R65) |
| `moss/mat` | the cover's moss film and cushions ([cover](../../docs/subsystems/cover.md#the-cover)): a mat of moss shoots | metres over the host's surface | 1K | about 0.3 | **not made**; R90 will blend it under the mask instead of drawing a film |
| `lichen/crust` | crustose lichen's crust and fruticose lichen's strap cards: a pale grey-green crust with its fruiting bodies | metres | 1K | about 0.2 | **not made** |
| `algae/film` | algae's green film on damp, shaded bark and stone | metres | 512 | about 0.5 | **not made** |
| `fungus/cap` | bracket fungi and mushroom caps: a zoned bracket's top (Trametes), a cap's | metres over the dome | 512 | about 0.05 | **not made** |

**What a masked image is** (roadmap R65; [texture](../../docs/subsystems/texture.md#alpha-tested-masks)): a card role's base colour is alpha-tested — the engine draws, shadows and traces nothing where its alpha is below 0.5 — so the lab delivers the leaf or the spray **with its outline in alpha** and nothing else changes: no separate mask image, no premultiplied colour. The content build keeps the share of the image at or above 0.5 at every mip (so a far tree does not thin), and grows the leaf's colour eight texels into the cut (so a far tree has no dark halo). content/test-scenes/leaf-cards/ builds a procedural stand-in with the command in its README, for checking a set's orientation and cut before the lab's arrives.

A species the solver gains later (a birch, a willow) is `<species>/bark` and `<species>/leaf` by the
same scheme. **A leaf card's set is one leaf per image at the species' size, for every species**
(since the allometric crown, G73, 2026-10-20): the ancient tree's card is a 0.3 m leaf, no longer a
1.4 m leaf cluster, so `ancient/leaf` is drawn as a single leaf like `broadleaf/leaf`. The cards a
low budget merges into branch-group cards will take a baked image of the sub-tree (R89), not a lab
set. What reads as **wrong** on the mesh, which the lab cannot see without it: a bark set
with features larger than a third of its repeat repeats visibly up a 14 m trunk (the palm candidates'
"repeated marks remain visible" note), a frond set whose grain runs across the image's *u* runs
across the leaflets, and a rock set with a strong single direction shows the box projection's six
charts. The lab's preview normal strengths (0.4 bark, 0.25 fronds) are carried as `normal_scale`;
a set baked at full strength says 1.
