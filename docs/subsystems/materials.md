# materials (domain)

**Purpose.** The tiled PBR sets a generated mesh's parts sample, and the **material packs** that
hold their defaults. A tree's bark, a palm's fronds and a boulder's stone are not drawn from an
artist's textures painted onto a UV layout: the solvers ([flora](flora.md), [rocks](rocks.md)) lay
UVs in metres or in whole repeats, and a set — five images and the surface one repeat covers —
tiles over them. This module is the one place a set is described, checked, looked up by role in a
pack, and written into the `.clusters` container's materials, so the two solvers reach the renderer
by one code path rather than two copies of it (G60, the owner's decision of 2026-10-19).

**Why a pack, and why outside the tree.** The owner's material lab (StableMaterials and PBRify;
[content generation](../content-generation.md#material-packs)) makes sets permissive enough to ship
as every species' and rock type's default, overridable per recipe. The repository takes no binary
and no generated image (AGENTS.md), so the defaults travel as a zip with a manifest, fetched by
`tools/fetch-materials.ps1` into the git-ignored `content/materials/` with its licence beside it —
exactly as `tools/fetch-samples.ps1` brings the Khronos models into `content/samples/`. A checkout
without a pack draws what it always drew.

## A set

`materials::MaterialSet`: the paths of a base colour, a tangent-space normal map (OpenGL, +Y), a
roughness, a metallic and a height image, any empty for none, the set's `tile_m` (0.05 to 20 m) and
its `normal_scale` (glTF's `normalTexture.scale`, 0 to 4). A recipe's block is a set with paths
relative to the recipe (`engine.flora.MaterialSet`, `engine.rocks.RockMaterial`, the same fields); a
pack's role is a set with absolute paths. `names_images` is the test of whether a recipe's block
replaces the pack's default: a block that only sets `tile_m` names no set.

**The override order**, `resolve_set`: the recipe's set when it names an image, else the pack's set
for the part's **role**, else none — the flat colour, and one log line naming the role, once per
role per process, so a scene of a hundred palms says it once.

**Roles** are `<species or rock>/<part>`, lower case: `palm/bark`, `palm/frond`, `palm/skirt`,
`broadleaf/bark`, `broadleaf/leaf`, `conifer/bark`, `conifer/needles`, `rock/granite`,
`rock/sandstone`, `rock/basalt`, `rock/limestone`. A new species follows the scheme
(`flora::flora_role`, `rocks::rock_role`).

## What the container records

`apply_set` writes a set into one material of a `geometry::ClusterFileData`:

- the base colour in the base-colour slot, the factor white (the image is the colour);
- **roughness alone in glTF's metallic-roughness slot**, the roughness factor 1 and the **metallic
  factor 0**. glTF reads roughness from G and metallic from B; a greyscale image decodes with
  G = B = the grey, and the zero factor makes the B read as no metal. Bark, leaves and stone are not
  metal, and packing a real metallic channel into B would need the solvers to decode and encode
  images, which they do not do (no io, and no image work at solve time);
- the normal map in the normal slot with the set's `normal_scale`;
- metallic and height **not at all**: carried in the recipe and the pack, read by nothing yet;
- for a part whose UVs are in metres (`UvLayout::metres`), a texture transform of scale
  `1 / tile_m` on every slot it fills (`geometry::encode_material_sampling`); a part in whole
  repeats already (`repeats`, a rock's charts) or a card that takes its image whole (`card`) gets
  none;
- each image once in `image_paths`, shared by every part that samples it, with an empty `images`
  entry (named by path, no carried bytes).

The solver then fills the **texture records** (`texture::fill_cluster_texture_records`, section
32): each image's roles and build options and a zero key, since an image named by path is not the
container's bytes — the host's texture step hashes the file. That is what lets
`engine-content flora --cache`, `engine-content rocks --cache`, `build`, `build-all` and the
renderer's scene reader build every image into `<ddc>/textures/<key>.tex` beside the container,
block-compressed and mipmapped like a glTF's ([texture](texture.md), [content_build](content_build.md));
without a derived-data root the renderer decodes the PNGs as it always could.

## A pack

A directory holding `pack.json` and the maps, one directory per role:

```json
{
  "format": "engine.materials.pack/1",
  "name": "palm-defaults",
  "version": "2026-10-19",
  "licence": "...",
  "roles": {
    "palm/bark": {
      "tile_m": 0.35, "normal_scale": 0.4, "normal_convention": "opengl",
      "maps": {
        "base_color": {"file": "palm/bark/base_color.png", "sha256": "...", "width": 2048, "height": 2048},
        "normal": {...}, "roughness": {...}, "metallic": {...}, "height": {...}
      },
      "provenance": {"models": [{"name": "StableMaterials", "revision": "..."}], "seed": 81231, ...},
      "licence": "StableMaterials: OpenRAIL-M ..."
    }
  }
}
```

`parse_pack` reads it whole or refuses it with the role named: a format word other than
`engine.materials.pack/1`, a role that is not `<word>/<word>`, a missing `tile_m` or map, a file
outside the pack, a convention other than `opengl` (the renderer reads glTF's tangent space; a
DirectX map lit from below is the packer's to flip, not a guess here) or a number out of range. The
SHA-256s, resolutions, provenance and licences are for `tools/fetch-materials.ps1` and for people;
the engine reads the files and the numbers. `pack_at(dir)` reads a directory's pack once per
process and keeps it (null for no `pack.json`, said once in the log); `default_pack_dir(start)` is
`<repo>/content/materials` beside the `<repo>/ddc` that `geometry::find_ddc_root` finds.

**A pack is in the build key, not the source hash.** `assets::generated_mesh_build_key` combines
`pack_key` — a hash over `pack.json`'s text and the pack's directory — into the generated mesh's
cache key when the pack has roles, so a palm built with a pack's defaults and one built flat are two
entries, a changed or moved pack is a new key (its images' paths are in the container), and a build
with no pack keys exactly as before packs existed. The source hash stays the recipe's alone, so a
scene that names a mesh by its hash still finds it. A changed pack rebuilds every generated mesh,
which costs a second each; keying per role would save that and is not worth its complexity yet.

**Where the hosts find it.** `engine-content flora` and `rocks` take `--materials <dir>` and
`--no-materials`, and default to the repository's; `build` and `build-all` use the repository's for
a generated mesh; the renderer's `SceneDesc::materials` names one, and is filled with the
repository's **only when the cache root is looked up too** — the repository's defaults go with the
repository's cache, and a caller that names its own derived-data root (every test does) names its
pack as well or draws flat.

## Invariants

- A set with no image changes nothing; a set's images are shared, never repeated, in
  `image_paths`; the transform is `1 / tile_m` on metres and the identity otherwise.
- The recipe's set wins over the pack's, the pack's over none, per part.
- A pack is read whole or refused; the same text in another directory is another key.
- `pack_at` reads a directory once per process.

**Public API.** `include/domain/materials/materials.h`: `MaterialSet`, `names_images`,
`set_in_range`, `UvLayout`, `apply_set`, `PackRole`, `Pack`, `parse_pack`, `pack_at`,
`default_pack_dir`, `find_role`, `pack_key`, `resolve_set`, `k_pack_format`, `k_tile_min_m`,
`k_tile_max_m`, `k_normal_scale_max`.

**Depends on.** `base`, `containers`, `hash`, `json`, `log`, `io` (`pack_at` alone), `geometry`
(the container's material records). Not a capability: `assets`' generator inputs carry a pack, and
a module that is not a capability may not depend on one (ADR-0027), so this sits below `assets`;
the flora and rocks capabilities link it.

**Testing.** `tools/dev.ps1 test -Filter materials`: a set into a container's slots (roughness
alone, metallic zero, the transform on metres, none on a card, shared images, an empty set a
no-op); the override order; a pack read and refused for a bad role, format, convention, missing
map and `tile_m` out of range, its key moving with its directory; `pack_at` once per directory and
none where there is none. The solvers' own tests hold them to it ([flora](flora.md), [rocks](rocks.md)),
`flora_scene_tests.cpp` takes a palm with a pack through the renderer's cache into built textures,
and `tools/materials-pack.Tests.ps1` (`tools.materials_pack`) runs the two scripts.

**Performance notes.** Nothing here is in a frame: a set is applied once per container build, and
a pack is read once per process.
