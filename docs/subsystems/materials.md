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
- **a card is a cutout**: a `card` set's base colour makes the material alpha-tested (glTF MASK) at
  the default cutoff 0.5, because a leaf card's image is the whole leaf and its alpha the leaf's
  outline; the content build then keeps that image's coverage down its mips and dilates its colour
  under the cut ([texture](texture.md#alpha-tested-masks)), and the renderer draws, casts and traces
  it through the mask ([renderer](renderer.md#alpha-tested-geometry)). An image with no alpha passes
  everywhere and draws what an opaque card drew;
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
the engine reads the files and the numbers — and **takes the manifest's text as the pin of its files'
bytes**: `Pack::hash` is over that text, so a load that hashed a pack's file once keeps the answer in
the derived-data root under the pack's hash and never reads it again while the pack is the same
(roadmap W75; [content_build](content_build.md#one-hash-per-image-and-a-packs-from-its-manifest)). A
pack is changed by repacking it, which moves its hash; one edited in place without its manifest is
caught only where a missing texture is built from it. `pack_at(dir)` reads a directory's pack once per
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

## The first pack (2026-10-19)

Made and fetched locally, nothing committed: the material lab's three palm picks — Bark A as
`palm/bark` (2K, `tile_m` 0.35, normal strength 0.4; StableMaterials then PBRify's upscale), Green
C as `palm/frond` (1K, 0.12, 0.25; StableMaterials, PBRify rejected by the lab for its mottling)
and Dry B as `palm/skirt` (1K, 0.12, 0.25; StableMaterials and PBRify) — and the lab's generic set A
as `rock/sandstone` (2K, 0.5; StableMaterials, a "warm beige limestone" prompt, ungraded) for the
rock half of the proof. The staging script copied the files the lab's `materials.json` names and
checked every one against the lab's own SHA-256 before packing; `pack-materials.ps1` made
`materials-palm-2026-10-19.zip` (22.7 MB, 20 maps) under `D:\workspace\game_engine_local\packages\`
and `fetch-materials.ps1` unpacked it into `content/materials/` against its hash.

`engine-content flora content/flora/date-palm.flora.json --ddc <scratch>` found the pack beside the
repository, built the palm with nine images and their textures in **4.9 s** cold (msvc-debug, the
texture step's nine BC7/BC5 builds 19.2 s of job time spread over the pool; CPU 43% busy with other
work when it started) and in **0.76 s** with the textures already built; the sandstone boulder with
three images in **4.4 s** cold. The `.tex` files: the bark's three 2K maps 5.59 MB each (BC7 colour,
BC7 roughness, BC5 normal, twelve levels; PSNR 53.7, 57.1, 56.1 dB against their sources), each
frond and skirt map 1.40 MB at 1K (eleven levels), **25.2 MB for the palm's nine** from 9.5 MB of
PNG, and 16.8 MB for the boulder's three.

**What the captures showed** (offscreen through engine-host from the built container and the
scratch cache, 800 × 600, shadows off, under the GPU lock; captures in the scratch directory, not
committed). The trunk reads grey-brown with the bark's fine vertical fibre and its **repeated dark
marks every 0.35 m up the trunk** — the lab's own note on Bark A ("repeated marks remain visible")
holds on the mesh, as a regular slash pattern a few rings apart; from 3 m the mip chain softens the
fibre where the decode of level 0 alone (the same container with no cache) shows it sharp and
shimmering, which is the texture step doing its job. The collar's boots carry the bark too. The
green fronds read dark olive and the skirt muted tan, the colours the lab graded them to; at every
distance captured the leaflets are a few pixels wide, so the fronds' grain along the rachis is not
resolvable — the rachis takes the set's colour evenly, and whether the grain runs the right way
waits for a capture close enough to see it. The flat palm and the textured one have the same
silhouette and nearly the same overall colour from 15 m. The **sandstone boulder** shows the set's
normal map well (cracks and pits over the bedding bands) but its base colour is a **saturated
yellow**: set A is the lab's raw, ungraded output, unlike the palm picks, and is a stand-in for the
pipe, not a sandstone.

## Not yet

- **The sets themselves**: the oak's, the spruce's, the leaf and needle cards' and the four rocks'
  ([content/materials/README.md](../../content/materials/README.md) says what each needs), and a
  hosted default pack whose hash `fetch-materials.ps1` pins.
- **Roughness alone costs a BC7 texture.** The metallic-roughness slot is built BC7 linear
  ([texture](texture.md), "Formats per slot") because a glTF packs two or three channels there; a
  greyscale roughness needs one, and BC4 would halve it (the palm's three roughness maps are 8.4 MB
  of its 25.2). A slot record that says "one channel" — or packing roughness and metallic at build
  time — is the fix; it changes texture keys, so it is texture's to decide.
- **Height and metallic** are carried and read by nothing: parallax or displacement wants the
  height, and a metal part (a rusted can) the metallic channel packed into B.
- **A host's pack over the protocol**: engine-host finds the repository's pack only when its cache
  root is looked up; `render.load` has no `materials` field yet, so a client that names its own
  `ddc` draws generated meshes flat.
- **Per-role keys**: the pack's key is over the whole manifest, so changing one role rebuilds every
  generated mesh (a second each, and the textures are keyed by their bytes, so none of them
  rebuild).

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
alone, metallic zero, the transform on metres, none on a card, a card alpha-tested at 0.5, shared images, an empty set a
no-op); the override order; a pack read and refused for a bad role, format, convention, missing
map and `tile_m` out of range, its key moving with its directory; `pack_at` once per directory and
none where there is none. The solvers' own tests hold them to it ([flora](flora.md), [rocks](rocks.md)),
`flora_scene_tests.cpp` takes a palm with a pack through the renderer's cache into built textures,
and `tools/materials-pack.Tests.ps1` (`tools.materials_pack`) runs the two scripts.

**Performance notes.** Nothing here is in a frame: a set is applied once per container build, and
a pack is read once per process.
