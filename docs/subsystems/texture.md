# texture (domain)

**Purpose.** The engine's built texture: an image as the GPU samples it — block-compressed, with its
whole mip chain, in the colour space its material slot needs — made once by the content build and
uploaded by the renderer without decoding anything ([07 §7.2](../plan/07-content-pipeline.md#72-source-formats-and-canonical-representations),
[§7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph)). The module owns the
`.tex` container, the mip filter, the block compression to BC1, BC3, BC4, BC5 and BC7, the
derived-data cache key that addresses a texture, and the per-slot defaults that say which texture a
material's image becomes. Until it existed the renderer decoded every PNG and JPEG at load and
uploaded it as RGBA8 at one level: four bytes a texel where BC7 takes one, a decode on every run,
and a far surface sampled at level 0 — aliasing that no filter downstream can take back.

**Owned data.** The `.tex` format (`texture_file.h`); the built texture's bytes, which are a
function of the source image's bytes and the build options and nothing else; the sRGB transfer
tables; the texture cache key and `k_texture_cache_version`; the roles-to-options table
(`material_textures.h`).

## The container

`.tex` is the cluster container's design applied to a texture ([ADR-0036](../adr/0036-built-textures-in-the-engines-own-container.md)
has why it is not KTX2): a 32-byte header (magic `TEXF`, version, flags, section count, total
bytes, a content hash over every byte after the header), a table of `{kind, element_size,
element_count, offset}` records, and payloads at 16-byte aligned offsets. Little-endian, no
pointers, every section an array of one fixed-size element. The kinds are append-only and a reader
skips one it does not know:

| Kind | Name | Element | What |
|---|---|---|---|
| 1 | `desc` | `TextureFileDesc`, 32 bytes, one | format, colour space, level-0 width and height, level count, flags (`k_texture_normal_map`, `k_texture_has_alpha`, `k_texture_alpha_coverage`), the source's channel count |
| 2 | `levels` | `TextureFileLevel`, 24 bytes, one per level | width, height, and the offset and length of the level's blocks in `data`; level 0 first |
| 3 | `data` | `u8` | every level's blocks back to back, each level at a 16-byte aligned offset, the padding zero |
| 4 | `source_hash` | `u64`, two | the source image's content hash (`hash_bytes` over its encoded bytes as read) and the build key over it and the options |
| 5 | `normal_variance` | `u8` | a mipped normal map's normal-variance chain (R97): BC4 UNORM of one less the mean unit normal's length, one level per level of the map, each 16-byte aligned ([below](#normal-variance-r97)) |

Stochastic terrain sampling (R96, 2026-10-26) was expected to need a section here — a histogram
transform's inverse look-up tables per image and mip, as Heitz and Neyret's and Burley's methods
do — and **needs none**: its variance-preserving blend works on the texels as they are, about means
the terrain's preparation already computes, so a stochastic layer samples the same `.tex` a tiled
one does and no kind was taken ([stochastic terrain sampling](../experiments/stochastic-terrain-sampling-2026-10-26.md)
has the measured comparison).

A level is whole blocks even when its sides are not multiples of four (a 2x1 level of BC7 is one
16-byte block), which is what Vulkan's copy expects. The reader refuses, each with its own
sentence and an empty result: a truncated file, the wrong magic or version, a section past the end,
a content hash that does not match, a missing required section, a format this build does not know,
and a chain that does not validate — a level whose extent is not `max(1, side >> level)`, whose
byte count is not `texture_level_bytes`, whose offset is unaligned or outside the payload, or a BC4
or BC5 texture claiming sRGB (neither has an sRGB form in any API), or a side past 16,384 (T190:
the build's own `k_max_side`, so every built texture reads unchanged; a valid 128 KiB strip of
65,536 x 4 BC4 texels used to be handed to the device past its `maxImageDimension2D`, 16,384 on
the baseline cards and 4,096 Vulkan's floor). A side under 16,384 can still pass a device whose
limit is lower: `gfx` holds every 2D image it makes to the device's own `maxImageDimension2D`
(T191, `image_extent_fits` in `create_image_2d` and `create_sampled_image_levels`, so both
uploads), refused with the image's extent and the limit in the message. `write_texture_file` refuses
the same and writes through `io::write_file_atomic`.

## Formats per slot, and why

A texture's options are decided by the material slots that name its image
(`options_for_roles`), and an image two slots name takes the first of this list it has:

| Slot | Format | Colour space | Why |
|---|---|---|---|
| base colour, emissive | BC7 | sRGB | colour a person looks at: BC7 is the only desktop format that keeps it at one byte a texel without BC1's 5:6:5 banding, and the sRGB view makes sampling return linear |
| normal | BC5 | linear | x and y at eight bits each through two independent BC4 blocks — twice BC1's precision per channel for the same byte a texel; z is rebuilt in the shader as `sqrt(1 - x² - y²)` (`material.slang`, `k_material_normal_rg`), so the level-0 vectors are renormalized before they are stored |
| metallic-roughness | BC7 | linear | glTF packs roughness in G and metallic in B, and exporters put occlusion in R of the same image ("ORM"), which the FlightHelmet's fifteen images all do: BC7 keeps the three channels where the shader reads them at BC5's size, serves both slots from one texture, and needs no swizzle a BC5 of G and B would |
| occlusion alone | BC4 | linear | one channel, R, at half of BC7's size |

`engine-content texture` takes the choice by flag instead: `--format auto` is BC5 for a normal
map, BC4 for a one-channel `--linear` image and BC7 otherwise; `bc1`, `bc3`, `bc4`, `bc5`, `bc7` and
`rgba8` (uncompressed, the escape hatch) are explicit, and BC4 or BC5 with `--srgb` is refused rather
than stored linear behind the caller's back. BC1 and BC3 are written only when asked for: at the
same byte a texel BC7 is better than BC3 everywhere, and BC1's half byte is a trade a slot has not
needed yet.

## The mip chain

`build_mip_chain` makes the full chain to 1x1 (or level 0 alone with `mips = false`). Each level's
sides are the previous level's halved and rounded down, never below one — what every GPU computes —
and each level is filtered **from the previous level's floats**, not from its 8-bit rounding, by a
separable **Mitchell–Netravali** cubic (B = C = 1/3) stretched to the exact ratio of the two sizes,
so an odd side is filtered rather than a column dropped.

**A tap past an edge reads what the texture's sampler reads there** (`TextureBuildOptions::edge_x`
and `edge_y`, `EdgeMode`): the edge texel again for a clamped texture, the other side of the image
for a repeating one, the reflection for a mirrored one (GL's `MIRRORED_REPEAT`: index −1 reads texel
0, −2 texel 1). The filter's support is two destination texels each way, so at every level the
outermost texels are made partly of what lies past the edge, and a repeating sampler will put the
*other side of the image* beside them at a tile seam. Filtered as clamped, a tiling brick's level 3
smears the edge row's mortar into the seam instead of the next tile's bricks: the tiles still meet,
but the join is a line of wrong texels at every distance where that level is read. Filtered as
repeating, every level is the downsampled *periodic* image, and a seam is no different from any other
column — the test builds a tiling image shifted by two texels and gets level 1 back shifted by one,
byte for byte, which clamped edges fail. The edges come from the samplers: `image_edges` takes, per
image, the wrap every slot that names it agrees on along each axis, and **clamp where they disagree**
(a repeating base colour that is also a clamped occlusion map), the one choice that bleeds nothing
from the far edge into a texture that does not tile. They pack into bits 8..11 of the options word
with clamp as zero, so every word written before them — and the table below — means what it did,
and the key moves with the wrap: an image the glTF names with no sampler repeats, which is every
image of the Khronos samples, so each is a new entry after this change (its old, clamped entry is
not wrong for a clamped sampler and simply goes unread). `engine-content texture --wrap` takes them
by hand. Three modes:

- **sRGB colour is filtered in linear light.** Averaging sRGB codes darkens every edge between a
  light and a dark region: a black-and-white checkerboard averages to code 128, which is 22% of the
  light, where the eye and a lens average it to half, code 188. Level 0 is the source's own bytes
  and every further level is decoded to linear, filtered, and encoded back.
- **Data is filtered as stored** (metallic-roughness, occlusion, alpha).
- **Normal maps are filtered as vectors**: decoded to −1..1, filtered, and put back on the unit
  sphere at every level, level 0 included (a vector the file's 8-bit quantization left short
  reconstructs the wrong z in a two-channel format); a vector that filters to nothing becomes +z.

The kernel is a polynomial rather than a windowed sinc for the determinism rule below. Its small
negative lobes can take a value a hair outside the source's range; the quantizer clamps.

## Normal variance (R97)

A mipped normal-map build at cache version 2 or later also writes section 5, `normal_variance`:
`1 - |mean of the unit source normals|` over each level's footprint — Toksvig's shortening — as a
BC4 UNORM chain with the map's extents and the map's 16-byte level alignment, so it needs no level
table of its own (`TextureData::normal_variance_level_bytes`). An unmipped map resolves every
texel it has, so its build writes none, and nothing but a normal map ever carries one; the reader
refuses a chain whose size is not the map's chain's, or one on a texture that is not a normal map.
An older file simply has none, and an older reader skips the kind.

**Why a section and not the map's own length.** The map's mips are renormalized and BC5 stores two
channels and rebuilds the third, so the shortening a mip of averaged normals would show is thrown
away twice before the shader sees it (Hill 2011 makes the same point about two-channel encodings).
The chain is built beside the map from the same decoded source (`build_normal_variance_chain`,
`mips.cpp`): the means are of unit **source** normals, never of the encoded blocks, and never
renormalized between levels. The filter is the map's Mitchell kernel with its negative lobes
removed and the rest renormalized — a distribution's mean needs non-negative weights — on the map's
edge modes. Level 0 is zero (one normal per texel), and a constant map is zero at every level. The
statistic does not depend on the material's roughness or normal strength, so materials sharing a
map share its chain. It is built with the same arithmetic as the mips (`std::sqrt`, which every
toolchain rounds the same, and no other library call), so the determinism table below holds it.

**What it costs.** Half the BC5 chain's block bytes per normal map, plus the small levels'
alignment: on the forest proof of concept's v18 scene, 167 chains in 83 MB beside 403 MB of textures. Built once per
image, keyed with the map.

**In the renderer** (`renderer.material.normal_filter`, renderer.md "Normal filtering"): the
chain is uploaded in the bindless slot **immediately after** its normal map — the upload checks the
adjacency rather than ever sampling another image — and shared with the map by its key; material
flag 8192 (`gfx::k_material_normal_variance`) says the slot is there, so `ResolveMaterial` stays
112 bytes. The read uses the normal map's sampler and UV derivatives, mips and anisotropy alike.
The shortening becomes a variance scaled by the normal strength squared and widens the GGX
roughness in the shared material code ([normal filtering](../experiments/normal-filtering.md) has
the formula, its sources and its limits). The reference path tracer reads level zero, whose
variance is zero: its many rays a pixel do the integration the chain stands in for.

**The cache version** went 1 to 2 for it (2026-10-27): every encoded image's build key changed, so
the determinism table's PNG container hash moved with the key and its two normal-map container
hashes with the added section; all eleven block-payload hashes stayed as they were.

## Block compression

### Encoders

`encode_level` encodes one level with the vendored encoders (`cmake/EngineTexture.cmake`,
[third-party licences](../../third_party/LICENSES.md)): **BC7** through Rich Geldreich's `bc7enc`
(modes 1, 5, 6 and 7, uber level 1, perceptual YCbCr weights for sRGB colour and plain RGB weights
for data), **BC1, BC3, BC4, BC5** through his `rgbcx` (quality level 10 for BC1 and BC3, the
high-quality searches for BC4 and BC5). A partial block at a level's edge repeats the edge texels.
The encoders are initialized once per process (their tables are globals). The block rows of a level
are spread over the job system's performance pool, one row a job at most, and every block is
written by exactly one of them. `decode_level` goes the other way (`rgbcx`'s decoders and
`bc7decomp`) for the tool's error figure and the tests.

## Alpha-tested masks

A material whose glTF `alphaMode` is `MASK` tests its base colour's alpha against `alphaCutoff`
and draws nothing below it (roadmap R65; the renderer's half is [renderer](renderer.md#alpha-tested-geometry)).
**The mask is the base colour's alpha, not a slot of its own**: glTF has no other place for it,
every exporter and the lab's leaf sets already put it there, BC7 keeps it at no cost in bytes (a
BC7 block is 16 bytes with or without alpha), and the base colour is the one texture every reader
of a masked surface — the rasterizers' masked variants, the ray loops, the path tracer — already
has an address for. A separate one-channel BC4 mask would be half BC7's size *of the mask alone*,
but it is a second texture fetch per masked pixel and a second record per material, for no
coverage it does not also need here.

The container's record for that image (`cluster_texture_records`) carries the material's cutoff in
its packed options (`TextureBuildOptions::alpha_cutoff`, bits 16..23, `alpha_cutoff_code`: the
8-bit code, rounded, clamped to 1..255), and a build given a cutoff does two things the plain build
does not, and says it did them with `k_texture_alpha_coverage` in the file's flags:

- **The colour under the cut is dilated** (`dilate_under_mask`, `k_alpha_dilation_texels` = 8
  rings): every texel below the cutoff takes the average colour of its passing (or already filled)
  8-neighbours, ring by ring. An editor exporting straight alpha leaves black or white under the
  cut; the bilinear filter and the mip chain both mix that into the leaf's edge, which is the dark
  halo a cutout gets at distance. Eight rings cover a level-3 texel's footprint. Integer
  arithmetic, so the bytes are the same everywhere.
- **Every level keeps level 0's coverage at the cutoff** (Castaño, ["Computing Alpha
  Mipmaps"](https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/), 2010;
  the method *The Witness* shipped; re-read 2026-11-18 for G11, which this closes: level 0's
  coverage at the test value, a bisection per level for the reference value that gives the same
  coverage, the level's alpha scaled by the test value over it — the rule below, which measures
  the coverage on the filtered surface where the article counts texels). A mask filtered as stored loses its thin parts down the chain:
  a needle a texel wide is half alpha one level down and is cut away whole, so a conifer goes bald
  with distance. `preserve_alpha_coverage` scales the level's alpha by `cutoff / t`, rounded down
  and clamped, for the 8-bit `t` at which the level's coverage is closest to level 0's. **The
  coverage is the share of the level's bilinearly filtered surface at the cutoff, not its share of
  texels** (`alpha_coverage_filtered`, G203): sixteen samples a texel, four a side at eighths of
  it, each the blend a bilinear tap makes there with the texel's neighbours on that side, read past
  an edge as the sampler's wrap reads it, in integers (the weights are eighths). Until 2026-11-14
  the build held the texel count, and a texel scaled *just* to the cutoff among lower neighbours
  counts as covered and draws nothing: every blend with a neighbour is under the cutoff. On the
  needle test's chain the count-held levels drew, under a linear filter, as little as a third of
  level 0's coverage (the test restates the old rule and measures it), and in the engine the
  hardware rasterizer drew a conifer's crown a quarter to a third thinner than the rays at 300 m
  ([the measurement](../experiments/conifer-crown-coverage.md)). The coverage falls as `t` rises,
  so `t` is found by bisection, each trial the scaled level measured whole: the clamp at 255 takes
  back coverage the scale alone would give, and only the measurement knows how much. A level the
  cutoff itself holds as well as any scale is left byte for byte. **The share is measured after
  the encoder**, decoded: BC7 moves an alpha by a code or two and a filtered level holds many
  texels within a code of the cutoff, so a scale exact before the encoder was 3.4% off after it on
  the needle test; the miss is fed back into the target and the level rescaled from its unscaled
  alpha, at most four times (`k_coverage_passes`), until it lands within half a percent. Only a
  mask pays the extra encodes and the measurements (nine or ten of the level a pass).
- **The readers sample it where it was held.** A ray tests level 0. A rasterizer tests one
  trilinear tap at the level of the pixel footprint's *shorter* axis, with no anisotropy
  (`mask_keeps`, [renderer](renderer.md#alpha-tested-geometry)): an anisotropic tap averages up to
  sixteen taps of that level along the footprint, an average no build scaled, and a thin mask
  seen at a slant falls under the cutoff whole; one tap is a point sample of a held level, and
  passes in level 0's share.
- **One texel holds all or nothing**, so the 1 × 1 level is stored beside the cutoff — at it when
  the coverage rounds to all, one code under it when it rounds to nothing — where its blend with
  the 2 × 2 level is decided by that level, which holds the share. **The pass kept is the closest
  one**, not the last: a small level steps over the coverage and back.

The alternative the literature offers is a **signed-distance mask** (Green, "Improved
Alpha-Tested Magnification for Vector Textures and Special Effects", SIGGRAPH 2007): the alpha
channel stores the distance to the edge, which filters linearly and keeps a sharp edge under
magnification. It needs the source to be a shape rather than a painted alpha, and a different test
in every reader; coverage scaling works on any painted mask with the test unchanged, which is why
it was chosen. **No cache-version bump**: the cutoff is part of the options and so of the key, so a
masked image keys a texture of its own and every other image keys exactly what it did — the ten
committed hashes did not move. **A mask's chain has a version of its own**, `k_alpha_mask_version`
(2 since the filtered coverage), mixed into the key of a build with a cutoff and of no other: a
change to how a mask is made rebuilds the masks of a cache and leaves every other texture's key
where it was. A container that *recorded* a mask's key (an image it carries; a flora tree's images
are named by path and keyed by the host) keeps naming the old texture until the container is built
again. `engine-content texture <image> <out.tex> --alpha-cutoff <code>` builds an image as a
material testing it at that 8-bit code would, and its summary's `mask` block lists each level's
filtered coverage and texel share as decoded: level 0's is how opaque the image is, the number an
author of a needle or leaf image wants ([flora](flora.md#the-conifer)).

A level too small to hold the share — a 4x4 level of a 14% mask holds 2 texels or 3 — lands within
two texels of it; such a level is a tree a few pixels across. A perfectly periodic mask (needles at
one spacing) filters to one uniform alpha at the level whose texel spans the period, and no scale
cuts a uniform level at a fraction; real masks are not periodic, and the test's are irregular for
that reason.
## Determinism

**A `.tex` is a function of its source's bytes and its options — not of the compiler, the
instruction set, the C library or the thread count** — which is the rule the cluster container
keeps ([geometry](geometry.md#the-same-bytes-from-every-toolchain), [ADR-0035](../adr/0035-no-floating-point-contraction.md))
and the reason the cache key names nothing about the machine. What holds it:

- **No C library transcendental in a decision.** The sRGB curve needs `pow`, whose last bit is the
  platform's, so it is two committed tables: the 256 decoded values and the 255 thresholds between
  consecutive codes (the linear value whose encoding is exactly n + ½ over 255), both taken once in
  double precision. Encoding is a binary search over the thresholds, so it is exact rounding of the
  true curve up to the tables' own float rounding, and decoding then encoding is the identity on
  every code. The test checks every entry against `std::pow` to within an ulp — a check that the
  table was typed right, not a dependency on the library.
- **The filter is a polynomial**, evaluated with IEEE arithmetic in one order; with contraction off
  tree-wide every compiler computes the same floats. Normalizing a normal is a division by a
  `sqrt`, which IEEE requires to be correctly rounded.
- **The encoders call nothing but `sqrtf`, `fabsf`, `floor` and `floorf`**, all correctly rounded,
  and their sorts sort integers with the element's index packed into the low bits — total orders.
  That was read in the source before they were chosen, and it is checked rather than trusted.
- **Parallel work writes fixed slots.** The filter's rows, the encoder's block rows and the content
  build's textures are each written by exactly one job, and nothing is merged in completion order.

`tests/determinism_tests.cpp` builds integer-made images through every format and mode — BC7 with
perceptual and with linear weights, BC1, BC3, BC4, BC5 through the normal-map path, RGBA8 without
mips, and a PNG through the decoder with its identity recorded — once serially and once on a
three-worker pool, and compares each file's content hash and its block payload's hash with a table
**taken on MSVC** (`msvc-debug`; `msvc-release` gives the same). A mismatch on another toolchain is a
bug until shown otherwise; only a deliberate change to the builder replaces the table, together with
a bump of `k_texture_cache_version`, and the failure prints the replacement.

**Verdict across toolchains** (2026-09-24): the table taken on MSVC 14.51 is reproduced byte for byte
by `msvc-release`, by GCC 13 at v3 (`linux-gcc-release`) and at v2 (`linux-gcc-release-v2`), and by
Clang 18 at v3 (`linux-clang-debug`) in the local Linux container. The encoders needed no
constraint — no mode was switched off and no integer path was forced — because contraction was the
only toolchain difference they had exposure to, and ADR-0035 had already removed it.

## The derived-data cache and the container's records

A texture lives at `<ddc>/textures/<key>.tex`, `key = texture_cache_key(source hash, options)`: the
content hash of the source's **encoded** bytes as read (the PNG or JPEG file, or the bytes a GLB
embeds) mixed with the packed options and `k_texture_cache_version`. The source hash is the one a
`.clusters` container's image record already carried (`ClusterFileImage::hash`), deliberately, so
the step taken when containers began carrying embedded images is the one this replaces rather than
one it undoes ([geometry](geometry.md#embedded-images-the-container-carries-them)).

A `.clusters` container records, per image, where its built texture is: **section kind 32,
`textures`** (`geometry::ClusterFileTexture`, 24 bytes: key, source hash, packed options, roles),
parallel to the image paths and written for every mesh. `fill_cluster_texture_records` fills it;
`engine-content build` and engine-view's own cache writer both call it, because the two apps share
cache entries byte for byte. **An image the container carries records its key; an image named by
path records its options and no key.** A container is addressed by its glTF and buffers
(`assets::source_mesh_hash`), not by the image files beside it — that is why a texture can be
repainted without rebuilding the mesh — so a key taken over a file's bytes at build time would go
stale under an unchanged container key, and the container would be a function of something its key
does not cover. A reader hashes the file as it is now and takes the key from the recorded options:
a repainted image finds its new texture or none, never the old one. `k_cluster_cache_version` went
to 14 with the section, because an entry without records would be drawn by decoding forever and
`build-all` would skip it forever, and to 15 when the records' options gained the edges the image's
samplers wrap with ("The mip chain"), because an entry built at 14 names textures whose mips were
filtered as clamped.

## Content classes

**Section 6, `content_classes`** (C62; [content classes](../content-classes.md#how-it-travels-through-derived-data),
ADR-0073 decision 3): **the source image's own class**, read from the image's sidecar
(`<stem>.provenance.json` beside an image named by path) through the classes module's one reader,
and recorded as that module's bytes ([content_class](content_class.md#what-derived-data-carries-c62)),
which this module keeps opaque (`TextureData::content_classes`). A streamed tail of the chain
(`read_texture_file_levels`, `texture_levels_from`) keeps it: it is the same image.

- **Written only when some level is above `none`**, so every texture built today, and every
  unclassified one built later, is the bytes it was; no golden hash, pin or
  `k_texture_cache_version` moves.
- **The key holds it only then**: the content build's texture step takes
  `content_class::classified_key(texture_cache_key(source hash, options), class_hash)`, which is the
  key itself for an unclassified image. So a classified image's texture is an entry of its own, and
  one image is still one entry whoever samples it.
- **The image's alone.** A texture is not built from the meshes that sample it, so it inherits
  nothing from them, and its key holds nothing about who samples it. An image a container embeds has
  no sidecar of its own: its class is the source's, in the container's section 54
  ([geometry](geometry.md#content-classes)), and its texture carries none. The instance's decision
  joins the two at load (C64). **A scene's load reads each sidecar before it takes either key**
  ([renderer](renderer.md#content-classes-at-load)): every image named by path of a mesh it reads,
  and every glTF it loads, through the content build's own reader (`read_source_classes`), so a
  sidecar means one thing to both and a key moves by the same class hash, materials included. An
  image whose `.tex` was built before its sidecar existed is asked for under the key its class
  moved, misses, and is built from its file by `run_texture_step`, which reads the sidecar again
  and writes section 6 under that same key; a glTF's cluster cache key is moved the same way, so a
  mesh built before its sidecar existed misses its classless container. Nothing of that name
  beside the source is `none` and keeps the key and the entry it had; a sidecar there that cannot
  be read, a link or junction that leads nowhere included, is the unreadable class and never
  `none`, as `sidecar_present` reads it in the build. Test "content classes at load: a sidecar that
  comes after the cache decides, not the old entry". **Not done**: section 32's records
  (`material_textures.cpp`) still hold the unclassified key, which the load replaces, and a mesh
  admitted after the load (a tile's, a placement's form, through `load_placement_textures`) keys
  its images by `texture_cache_key` alone until it comes through the door (C67).
- **One file, one answer** (C69): a section 6 whose elements are not bytes is not the classes
  module's record, and both readers refuse it through one check (`classes_section` in
  `texture_file.cpp`): the whole-file reader and the streamed tail's (`read_texture_file_levels`),
  which once skipped such a section and returned the texture with no class, which is `none`. Test
  "texture file: a classes section of elements that are not bytes is refused by both".
- Test "texture file: content classes are section 6, written only when classified" (the plain
  fixture's hash unchanged with the field empty, the bytes round-tripped, a tail keeping them).
  **Built 2026-11-18** in `msvc-debug`, its tests passing with no device and every existing determinism pin unchanged.

## In the renderer

`systems/renderer`'s texture upload (`gpu_scene.cpp`, `find_built_textures`) takes a mesh's built
textures when the load reads the cache (`--no-cache` reads none) and **every** image the resolve
samples — base colour, metallic-roughness, normal, occlusion, emissive — has one whose recorded
identity matches; it uploads them as stored with `gfx::upload_image_2d_levels`, samples them through
mipmapping samplers (below), and sets `k_material_mipped` on the mesh's materials (and
`k_material_normal_rg` where the normal map is BC5). Otherwise the mesh takes the path it always
took — decode, RGBA8 at one level, level-0 reads — and the flags stay zero. It is all or nothing per
mesh, so no material mixes the two. A device without `textureCompressionBC` draws the decoded path
with a warning naming the feature (every desktop GPU has it; `engine-cli gpu.adapters` lists the
row). `engine-view`'s summary says what happened: `"textures":{"built":n,"decoded":m,"bytes":b,…}`,
the bytes being what the allocator gave the images.

**One texture per distinct `.tex` per scene.** The build key is also how the renderer tells two
meshes' images apart: a mesh whose image has a key the scene already uploaded — every member of a kit
that embeds one atlas — takes that texture's bindless slot instead of uploading another copy, and a
decoded image is keyed the same way by its encoded bytes and the format it goes up as. The key is
the content rather than the path for the reason the cache is addressed by content: an embedded image
has no path, and a path names neither the same bytes everywhere nor different bytes only. E33's
ashlar kit went from 99 textures and 1,440 MiB on the device to 6 and 80 MiB, the picture the same
bytes ([renderer](renderer.md#one-upload-per-distinct-image)); the summary's `distinct`, `shared` and
`bytes_saved` say so for any scene.

**Whoever fills the cache builds the textures.** The renderer's load through the cache runs the
content build's texture step (`content_build::run_texture_step`) for whatever of its meshes'
textures is missing — beside a container it writes when it clusters a glTF itself, and on a hit
([renderer](renderer.md#what-the-load-reads)) — so a scene on a cold cache samples built, mipmapped
textures from its first frame. Until 2026-09-24 only engine-content built them, and a scene flown
without running it first drew them decoded at one level and shimmered at a distance.

### Trusted, not read

**A load trusts a built texture from its header** (since 2026-10-24, roadmap W75). The renderer's
load takes every distinct key its meshes' records name once (`prepare_scene_textures`, scene.cpp)
and asks `probe_texture_file` for it: the 32-byte header, the section table and the `SourceHash`
section, read by range — a few hundred bytes, not the payload — the file's size held to the
header's `total_bytes`, and the recorded source hash and build key compared with the ones the key
was taken from. A `.tex` that passes is used without reading its source; one that is missing, torn
(a size other than its header's) or recorded from other bytes is built again, once, and trusted
from then on. The content hash is not checked by the probe: the upload's full read
(`read_texture_file`) checks it, and a file whose payload fails it draws through the decode with the
reason, as any unreadable texture does. The probe replaced `read_texture_file_identity` on the
load's path, which read and hashed the whole file to answer the same question, so a warm forest
read every `.tex` twice. `engine-content build-all` keeps the full check: the cook is where a
corrupt entry should be found and rebuilt, the load is where it should cost nothing.

**Where the key of an image named by path comes from.** The container still records no key for it
(above), and the load does not write one in: it finds the image's hash through the load's table of
its source images (`content_build::ImageIdentities`, [content_build](content_build.md#one-hash-per-image-and-a-packs-from-its-manifest)),
**one identity per distinct file for the whole load**, and a material pack's file from the pack's
index without a read. The keys it found go into each mesh's in-memory records, so the upload reads
them and never hashes a source again. A file covered by nothing the load reads — a glTF's sibling
PNG, a recipe's own set — is still hashed as it is now, once a load, which is what keeps a repainted
one finding its new texture.

## Sized by the asset

**Why** (roadmap G14, 2026-11-15). A static prop's textures are paid whole: the forest's village
furnished with 37 generated props cost +1,078 MiB of GPU memory in its small scene, three 2,048
images a prop, a 20 cm salt box included, for +0.24 ms of frame. Whatever the prop's size, the
renderer uploaded every level of every image it named, and the levels a view never samples are the
largest: level 0 is three quarters of a chain. Any game made of bought or generated assets pays it,
and the dev machine's 32 GB card hides it; a player's has 8 to 12.

**The measure** (`texel_density.h`, `measure_cluster_density`). For each image a mesh's materials
sample, over the mesh's own level-0 triangles: their surface area, their UV area (through the
`KHR_texture_transform` scale the renderer draws the material with), and per triangle the
**smaller singular value of the map from the triangle's plane to its UVs** — UV units a metre
along the least stretched direction, which is the axis a 16x anisotropic sampler takes its level
from ([samplers](#samplers-and-texture-transforms)). The density reported is the one all but 1% of
the surface (by area) reaches, so one sliver an exporter squeezed in its atlas does not hold a whole
prop's texture at full size; times the image's shorter side it is **texels a metre at level 0**.
Nothing about the generator is read: any asset with UVs has the measure. `engine-content info`
prints it per image (below).

**What a frame can show.** A pixel at angle φ off the axis of a rectilinear camera spans `cos²φ / f`
radians along the radial direction and `cos φ / f` across it, where `f` is the pixels a radian at the
centre; on a surface facing the ray at distance `r`, its smaller footprint is `r cos²φ / f`. So a
surface never holds more than `f / (r cos²φ_max)` pixels a metre, and a level whose texels are no
denser than that is never read finer than the anisotropic sampler's minor axis. At 3840 x 2160 with
engine-view's default 55° vertical field, `f = 1080 cot 27.5° = 2,074.7` pixels a radian, the
frame's corner is φ = 46.7° (cos² = 0.470), and:

| Nearest approach `r` | Centre, texels a metre (`f / r`) | Corner (`f / (0.470 r)`) |
|---|---|---|
| 0.3 m | 6,916 | 14,713 |
| 0.5 m | 4,149 | 8,828 |
| 1 m | 2,075 | 4,414 |
| 2 m | 1,037 | 2,207 |

At 1920 x 1080 every figure halves. A sampler reads level `k` and coarser wherever level 0 holds at
least `2^k` texels a pixel, so with `D` the corner figure for a stated nearest distance, **the
`floor(log2(d0 / D))` top levels of an image whose level 0 is `d0` texels a metre are never
sampled at that distance or farther, and leaving them out changes no pixel** (trilinear filtering
blends `floor(λ)` and the level after it, both kept).

What that buys depends on how the asset spends its image, which is why it is measured and not
assumed:

| A 2,048 image on | `d0`, texels a metre | Levels left out, 4K, 0.5 m / 1 m / 2 m | At 1080p |
|---|---|---|---|
| one face of a 20 cm box, the whole image | 10,240 | 0 / 1 / 2 | 1 / 2 / 3 |
| a 20 cm box, all six faces in one atlas at 70% use | 3,498 | 0 / 0 / 0 | 0 / 0 / 1 |
| a 1 m chest in one atlas at 70% use | 699 | 0 / 0 / 0 | 0 / 0 / 0 |

So a generated prop whose atlas covers it whole at 2,048 is **not** over-dense for a walker's 4K
view at half a metre: the cap leaves nothing out of it, and says so. It pays on a mapping denser than
its view — a face-mapped or tiled small prop, a 4,096 image on a hand-sized object, anything a scene
only ever shows from a distance (a shelf, a yard, a roof) — and at the resolutions players run.
One level out is three quarters of the texture's memory; two, fifteen sixteenths.

**The cap** (`TextureBuildOptions::needed_side`). A power of two: the texels along the image's
shorter side that the stated density needs, `D · scale / uv_per_m` rounded up (`needed_side()`;
`scale` is the mesh's largest load-time instance's largest axis scale times its fit; which meshes
are capped at all is under "Who states the density"), and the build keeps the
smallest level of the source's chain whose shorter side is at least that (`texture_levels_dropped`).
**The levels kept are the uncapped texture's own levels, byte for byte**: the chain is filtered from
the source exactly as it always is, a mask's levels are held to the source's level-0 coverage as
before, and the levels above are not stored, so wherever a sampler reads level `k` or coarser the
picture is the uncapped one's exactly (the test builds both and compares the blocks). It packs into
bits 12..15 of the options word, zero for none, so **a texture that states no cap keeps its key and
nothing that does not ask is rebuilt**; one that does is a new key beside the old entry, which is
not wrong and simply goes unread. `k_texture_cache_version` did not move.

**Who states the density.** The renderer's load, from the scene: a mesh's own
`Mesh.texel_density` (schema `engine.scene.Mesh` version 2; negative caps none of its textures), else
the host's `renderer.textures.texel_density` (`SceneDesc::texel_density` for a caller that sets it).
**The default is 0, no cap**, for three reasons: the right density is the viewer's (its resolution,
its field and how near its camera comes), which a scene file cannot know and a host or a game's
quality preset can; a non-zero default would put a new key on every over-dense texture of every
cache on the machine and rebuild them on the next load, the forest's included; and below the stated
distance a capped texture is blurrier than the source, which only the author can accept. The
`Scene` root, at its ceiling of twenty fields, takes no new one: the density is the host's, the
override the asset's. The load measures every mesh the scene's own instances place, takes its
largest instance's scale, and gives the texture pass a need per image (`SourceMesh::texture_caps`);
**an image several meshes share takes the largest need, and a mesh that states none needs it
whole**. The load's report counts `capped` references.

**The scene's density caps only a mesh whose every placement the load knows** (`cap_scene_textures`,
scene.cpp; mended 2026-11-16 after the review of G14, which found a mesh the streamed tail placed at
twice its static instance's scale losing a level it read). A cap sized from the load-time instances
is only safe when nothing after the load places the mesh larger, so a mesh is **open**, and the
scene's density does not cap it, when the load is told that something else may place it:

- a streamed placement entry's resident mesh (`SceneDesc::streamed`): the world's tiles place it
  through the tail at whatever scale the generator picks;
- a prop (`SceneDesc::props`): a game module's `prop_place` takes a scale;
- an actor's mesh (`AuthoredStreaming::keep`, the scene's declared actors): `actor_scale` may grow
  it up to four times;
- a procedural mesh (an empty path), the terrain a world's ground tiles draw.

A placement entry a whole read expands is not among them: its instances are the scene's, scales
and all, before the cap runs. **A scene with no instances of its own caps nothing**: the load's
default identity instance of every mesh (and a grid's copies) is the load's invention, not a
placement anyone stated. **A mesh's own `Mesh.texel_density` is the author's word for that mesh**
and caps it even when it is open, read at its largest load-time instance's scale and, for an open
mesh, never under 1: the density is stated for the mesh at its fitted size, and an author who
places it larger states it for that. `texture_tests.cpp` holds each case ("the scene's density caps
only a mesh whose every placement the load knows") and draws the tail's placement at twice the
scale against the uncapped scene, byte for byte ("a mesh the tail places at twice the scale").

**What an asset costs** (`engine-content info`, its `cost` object): `geometry_bytes` (the
container less the images it carries), `texture_bytes` (every sampled image's whole built chain in
the format its slots ask for, what a load uploads), and per image its size, format, `built_bytes`,
`surface_m2`, `uv_area`, `uv_per_m`, `uv_per_m_mean`, and `texels_per_m` (and its mean) at level 0
and scale 1. With the table above that is a budget read before any scene is loaded.

**Triangles are not capped yet.** The cluster DAG already draws only what the pixel needs; what is
paid whole is the finest level in memory. Measured on a 27,918-triangle, 20 cm sphere (the densest
of the village's props is 28k): see [the measurement](#what-a-cap-saves-and-what-geometry-costs-beside-it-g14) below — its geometry is a
small fraction of what its textures cost, so a floor by size (the finest level a stated nearest
distance can show) is left on roadmap G14 until a scene shows geometry, not images, as the bulk.

## Textures that arrive after the load

**Built** (R147, 2026-11-11; designed by R145 the night before). A mesh that arrives with its tile
([renderer](renderer.md#meshes-that-come-and-go)) drew its materials' constants, because every
texture of a scene was uploaded by the load, each with a submission the load waits for; so no
solver could move to the tile contract without losing its rock or its leaves. Now a tile's mesh
brings its textures: found or built off the frame, uploaded without a wait inside the commit's byte
allowance, shared by content with the load's and with each other, and released with the last mesh
that names them. The code is `gpu_scene_textures.cpp` (the table, the upload, the release, and the
one function that makes a material), `load_placement_textures` in `scene.cpp` (the build's half),
`world::TileMeshes::admit_textures` (the pace), and `gfx::create_sampled_image_levels` with
`gfx::record_image_levels_upload` (the upload's two halves).

**Whole textures by reference, not pages by feedback.** The published practice has two shapes.
Virtual texturing keeps one page table for the world's texels, renders what it wants into a
feedback buffer, and streams pages into a fixed pool (Barrett, "Sparse Virtual Textures", GDC 2008;
van Waveren, "id Tech 5 Challenges", SIGGRAPH 2009; Chen, "Adaptive Virtual Texture Rendering in
Far Cry 4", GDC 2015). Mip streaming keeps whole textures, each resident down to some level, in a
fixed pool ordered by what the view needs (Unreal Engine's texture streaming and its pool size),
and asset systems hold what several users share by reference count (Unity's Addressables). *None of
these was re-read for this row either, by the night that designed it or the afternoon that built
it; they are cited as known, for the shape of the two families and nothing else, and no number on
this page is taken from them.* The simple scheme is enough for a tile's meshes for three reasons: a
tile's mesh arrives at its ring's distance with a material set a solver chose by role, so the
textures are few and shared (a region of ledges names one rock set); residency is already decided
above the renderer, per tile, with a linger and a budget, and a texture has no residency of its own
to decide — it lives exactly as long as a mesh that names it; and a page table costs an indirection
in every sample of the resolve at 3840 × 2160 with no upscaler, for a problem — unique texels over
the whole world — this engine does not have, since its ground is layered by rule and its meshes
tile. It is **not** enough where the count is the cost: the forest proof of concept's 5,392 built
textures are 3.3 GB at load (T73's measurement, 0.61 MiB each on average), and by-reference
residency cuts what is resident but still uploads each texture whole; there the next lever is the
chain's own shape — the small levels first, the top ones under a pool — which the container was
laid out for ("Not yet").

**What a texture costs.** Chains as `texture_level_bytes` gives them, checked against the engine's
own derived-data cache (40 built textures: 28 of 5.33 MiB, 5 of 21.33 MiB, 5 of 1.33 MiB), and the
first row again by `mesh_texture_tests.cpp`, which stages one:

| Texture | Levels | Bytes |
|---|---|---|
| 2048², BC7 or BC5 (16 bytes a block) | 12 | 5,592,432 (5.33 MiB) |
| 2048², BC4, a normal map's variance chain | 12 | 2,796,216 (2.67 MiB) |
| 1024², BC7 or BC5 | 11 | 1,398,128 (1.33 MiB) |
| 4096², BC7 or BC5 | 13 | 22,369,648 (21.33 MiB) |
| **A 2K set**: base colour, metallic-roughness, normal and its variance | | **19,573,512 (18.67 MiB)** |

**What is built, in the order a texture travels.**

1. **Found and read off the frame.** `renderer::load_placement_mesh` — the build a tile's mesh
   already has, on the job system's efficiency pool — runs the load's texture pass for the one
   mesh when `PlacementMeshOptions::textures` is set (the world sets it for a host that draws; the
   collision leaves it off). It is the load's own function, `prepare_scene_textures`, over a list
   of one, not a copy of its rule, run on the building thread with no pool of its own — threads are
   made only when a `.tex` has to be built (T129: a pool of two made and joined per mesh kept 4 MiB
   of commit a thread for the life of the process, 6.2 GB on the forest's staging;
   [the measurement](../experiments/memory-commit-accounting.md#who-made-the-unnamed-commit)): every image a material samples gets its `.tex` key (the
   container's record for an image the mesh carries, the load's identity of the file for one named
   by path), its `.tex` is trusted from its header or built by the content build's own step, and
   then its bytes are read — unless the scene says it holds that key
   (`PlacementMeshOptions::texture_resident`, an advisory ask any thread may make, which
   `GpuScene::texture_resident` answers from a sorted list under a mutex; a texture let go between
   the build and the admission is read again by the admitter, the one case a commit reads a file).
   Without the ask every waiting mesh of a region of ledges would hold its own copy of the one rock
   set in host memory. **All or none per mesh**, as the load's rule is ("In the renderer"): one
   image without a built texture and the mesh draws its constants, with the reason in
   `PlacementMeshData::textures_why`. The pass makes a pool of two threads for its length, because
   it wants one and the thread it runs on may be a worker of the host's pool. Two tiles' meshes
   that name one new set are built at once on two workers and both may write the entry — the same
   bytes, each by a rename — so a read that fails is asked again, twice, before the mesh is left
   with its constants (`tile_mesh_textures_tests.cpp` builds two such rocks on a pool).
2. **Admitted one texture at a time, within the commit's allowance, before its mesh**
   (`world::TileMeshes::admit_textures`). The allowance is bytes staged a commit
   (`world.meshes.admission_kib`, 4,096 by default), the textures' and the meshes' together, and a
   texture larger than all of it goes alone — so **the default admits one 2K texture a commit**
   (a normal goes with its variance: 8 MiB), a 2K set takes three commits, and its mesh the one
   after: four frames, 67 ms at 60 Hz, during which the tile draws what it drew. **What a 2K
   texture costs the committing thread is 0.66 ms**: one image made and 5,592,432 bytes copied into
   staging, the best of three in `msvc-release` (0.97 ms in `msvc-debug`) on an RTX 5090 beside
   eleven other agents' builds and test runs (`mesh_texture_tests.cpp`, 2026-11-11), so an upper
   bound. **The default stays.** A whole set in one commit would be about 2.3 ms of the frame's
   thread in one frame (18.67 MiB at that rate), which shows at 120 Hz, to save three frames of a
   wait on a tile that is
   already waiting for its mesh's build and is drawing its last ring's form meanwhile; the three
   frames show nowhere. What would move it is a measured walk where the wait is seen (the first
   converted solver), not this number.
3. **Uploaded without a wait.** `gfx::upload_image_2d_levels` is two halves:
   `create_sampled_image_levels` (the image and its chain, nothing in it) and
   `record_image_levels_upload` (the transition to a copy target, a copy a level, the transition
   to what shaders read), recorded by the next frame in the "terrain upload" pass that copies an
   admitted mesh's streams, out of the same staging ring (32 MiB; a staging the ring has no room
   for is a buffer of its own, as a mesh's is). The image is one the render graph never sees,
   which is why its two barriers are recorded by that function and not derived. The load's upload
   is the same two halves round a submission it waits for.
4. **Shared by content.** One table for the scene, keyed by the `.tex` build key — the key the
   load already shares by — holding the load's built textures (pinned: no count, never released)
   and the admitted ones with a count of who holds each. `GpuScene::acquire_texture` takes a
   reference and uploads only when the table has none of that key, whether the load or another
   tile's mesh brought it. `admit_mesh` takes one for each texture its materials sample; the
   caller's own, taken to have the texture in before the mesh, goes back once the mesh is in.
5. **Released with the last mesh.** `retire_mesh` gives the mesh's references back; a texture at
   zero leaves the table at once, and its image, its view and its pair go four recorded frames
   later (`k_joint_slots` + 1: one more than can be in flight, so every frame that could have
   sampled it is done). **One wanted again before then comes back without an upload**
   (`TextureUse::revived`): its image is still there. The mesh's own linger
   (`world.meshes.linger`) is the texture's: a mesh kept warm keeps its textures.
6. **Bindless slots from a reserved run.** A scene with a mesh room reserves
   2 × `renderer.textures.slots` consecutive sampled-image slots when it is made (1,024 textures:
   2,048 of the set's 16,384), a pair a texture, because the resolve reads a normal map's variance
   at the slot after its normal's and a free list of single slots cannot promise that; a free pair
   holds a 1 × 1 placeholder. They are consecutive because nothing has freed a descriptor when the
   scene is made, and the layout refuses a scene where they are not. **The capacity is the pairs
   and a byte budget** (`renderer.textures.budget_mib`, 1,024: fifty-four 2K sets), counted in what
   the allocator gave the admitted textures (`TextureUse::bytes`; the load's are the load's). A
   texture on its way out is above the budget for its four frames (`leaving_bytes`): counting it
   would make a refusal depend on what left a moment ago, and the picture on the order of arrival.
7. **A refusal draws the constants and names its bound.** No pair free, the budget passed, a
   device without `textureCompressionBC`, or no room at all: `acquire_texture` says which, the
   world gives back what the mesh's earlier textures held, the mesh is admitted with its
   materials' constants, and the log says it once per change with the bound's name
   (`a tile's mesh draws its materials' constants: a texture was refused … bound=…`).

**A mesh waits for its textures.** R145's brief asked that a mesh never wait unless the pop is
worse than the wait. It is: a material's constants are its factors, which for a textured material
are white and 1.0, so a rock drawn before its textures is a white rock that turns brown, over its
whole surface, and an alpha-tested mesh is its cards drawn solid until the coverage arrives. The
wait is four frames at the default allowance, on a tile that is already waiting for its mesh's
build and is drawing what it drew before.

**Alpha-tested materials.** The masked pipelines are chosen when the scene is made, from the
materials it can see then (`GpuScene::has_masked`). A generator whose tiles may hand an alpha-tested
material says so in its descriptor (`scene_gen::PlacementGeneratorDesc::tile_meshes_masked`, beside
`tile_meshes`), and the scene draws through the masked pipelines from its first frame; the mesh's
alpha-tested materials then draw their coverage on every rasterizer and on the ray path, because
every masked reader takes the material table and the per-cluster material indices by address, and
an admitted mesh's are in them. A masked material handed without the declaration is drawn opaque,
said by name. **Its shadow**: the opaque shadow proxy's coverage table (R109) and the opacity
micromaps (R120) are baked at load over the load's own clusters, so an admitted masked mesh has
neither — its clusters read coverage zero in the table, which is "never solid", and have no
micromap — and a shadow ray tests the mask's texel at every candidate triangle. That is the right
shadow, texel for texel, at the any-hit cost the two bakes exist to cut; **what it costs in a frame
has not been measured**, since it needs foliage handed by a tile, and what would give an admitted
mesh the bakes is running each over the admitted clusters in the commit that admits them (both are
one dispatch over a list of clusters; the proxy's table already has the room's clusters in it).

**One function makes a material.** `GpuScene::fill_material` builds a `gfx::ResolveMaterial`'s
common part — factors, the five slots, the flags a built texture sets, a sampler per textured slot,
the UV transform, the alpha mask — and `fill_stochastic` the stochastic means, for the load's
meshes and an admitted mesh's alike; the load's inline block is gone. Anything else lets the two
drift, and "a streamed scene is the scene read whole" is the test that would find it. **Stochastic
sampling** on an admitted mesh is drawn only where the load already has a stochastic material: the
resolve's stochastic variant is chosen from the load's, as the masked pipelines are, and a second
declaration was not added for it; elsewhere such a material draws plain, said by name.

**The tests**: `systems/renderer/tests/mesh_texture_tests.cpp` on a device (seconds in
`msvc-debug`, the wait for the GPU lock aside) — textured rocks that arrived one by one against the
scene read whole and against another order of arrival through other slots after a rock and its map
came and went, `0 depths, 0 colours, 0 ids over 9102 rock pixels` each, and 6,781 of those pixels
another colour when the same rocks draw their constants (2.2 s); a shared texture uploaded once,
the bytes before, with and after, the leaving list's four frames, the revival, and what admitting a
rock whose textures are resident allocates (5 allocations, no device buffer; 0.9 s); both refusals
by name and the mesh drawn from its constants (1.6 s); a 2K texture's admission timed, and the byte
budget's refusal (1.4 s); an alpha-tested rock against the whole read, `0 depths, 0 colours, 0 ids
over 8565 rock pixels`, and 537 more rock pixels when a scene not told to expect one draws it solid
(1.6 s); and on the ray path the textured rocks against the whole read, again `0, 0, 0`, with the
alpha-tested cut within 3 % of the rasterizer's (8,718 traced pixels against 8,565) and 384 short
of the solid rock (2.6 s). `systems/world/tests/tile_mesh_textures_tests.cpp`, no device (0.7 s for
its four cases): a commit under an allowance of 1,500 bytes stages 1,392, then 2,112 alone, then
the mesh; a second rock of the same maps arrives in one commit with no upload and no read; the
references by table and by mesh; a refused texture ends the wait; two rocks built on a job system
arrive with their textures; a mesh let go while it waited leaves nothing held.

**What it leaves.** The recording frame's allocation count through a flight (the committing
thread's is counted above; `engine_view.frame_loop` has no scene whose tiles hand meshes yet); the
p99 of a walk over activating tiles; the bakes for an admitted alpha-tested mesh, above; a
stochastic declaration; and textures for **authored** meshes by the same residency, which is where
the forest's count is.

## Levels resident by what the frame samples

**Why** (roadmap R172, 2026-11-16; [ADR-0065](../adr/0065-texture-levels-resident-by-bound-and-feedback.md),
proposed). Every level of every built texture is uploaded at load and held for the scene's life
("In the renderer"), and G14 showed that capping the chain at the cook does not take the memory back
for assets whose atlases are not over-dense ("Sized by the asset"). What does is not holding a
level no pixel reads. A player's card has 8 to 12 GB where the development card has 32, so this is
memory first.

**The measurement** (`renderer.textures.feedback=<file>`, off by default;
`systems/renderer/src/texture_feedback.h`, `shaders/texture_feedback.slang`). A frame pass of the
renderer's own takes every pixel of every view the way the resolve does (`reconstruct_screen_kept`,
then `screen_uv_gradients_at` through the material's scale and transform) and works out, for each
textured slot of the pixel's material, the level a 16x anisotropic trilinear sample reads first:
with `rho` the UV's change per pixel in the texture's own texels along x and y, `lambda =
log2(max(rho_max / 16, rho_min))`, Vulkan's anisotropic level of detail, and the finest level read
is `max(0, floor(lambda))`. It keeps the smallest per bindless slot (an atomic minimum of a
fixed-point code, once per wave where the wave's lanes name one slot, as the D3D12 sampler feedback
specification advises coalescing), copies the table into a host-visible buffer and folds it one
frame slot later into a JSON line a frame: the bytes resident, the bytes of the levels this frame
asked for, and what a residency that holds what was asked within the last 0.5, 2 and 8 s would hold
and would have had to bring in that frame; and at the end a line per texture with the finest level
any frame asked for. The layered ground's and the water's own lookups are not the material's five
slots at its UV and are left out (those materials are skipped), a deformed instance is measured at
its rest pose, and the rays' explicit level-0 reads are not counted (below). Being a frame pass, it
draws the frame through the frame passes' linear image and the encode (renderer.md, "Frame
passes"), so a frame time taken with it on is not the frame's cost. A frame's lines are kept in a
16 MiB reserve and written once when the renderer is destroyed.

**What it measured** (2026-11-16, `msvc-release`, RTX 5090, `--raster rt --shadows rt`, the
bounce on, `--offscreen`, the session unlocked, the machine shared with other agents' builds and
GPU tests; the numbers are counts and bytes, which the machine's load does not move; the
flythrough's path flown at 60 frames a second of its own clock, repeats included):

| Scene, resolution | Textures, resident | Never asked by any frame | One frame asks (median / most) | Held by an 8 s hold (median / most) | Brought in under it (mean, most in a frame) |
|---|---|---|---|---|---|
| village interior walk, 3840 x 2160 | 48, 168.2 MiB | 55.9 % | 12.0 / 60.7 MiB | 44.4 / 66.0 MiB | 1.60 MiB/s, 14.0 MiB |
| village interior walk, 1920 x 1080 | 48, 168.2 MiB | 57.7 % | 1.9 / 46.7 MiB | 11.1 / 65.6 MiB | 1.36 MiB/s, 14.0 MiB |
| oasis-palms flythrough, 3840 x 2160 | 11, 28.2 MiB | 0 % | 1.9 / 28.2 MiB | 28.2 / 28.2 MiB | 0.51 MiB/s, 14.0 MiB |
| oasis-palms flythrough, 1920 x 1080 | 11, 28.2 MiB | 0 % | 0.3 / 23.6 MiB | 10.7 / 28.2 MiB | 0.75 MiB/s, 17.5 MiB |
| static-world, 3840 x 2160 | 5, 0.7 MiB | 0 % | 0.3 / 0.7 MiB | 0.6 / 0.7 MiB | 0.01 MiB/s |
| static-world, 1920 x 1080 | 5, 0.7 MiB | 0 % | 0.2 / 0.7 MiB | 0.2 / 0.7 MiB | 0.01 MiB/s |

The flythrough passes close to everything it shows, so over its whole path every one of its eleven
textures (the palm pack's bark, frond and skirt at 2,048 and 1,024 and the sand) is read at level 0
somewhere — but a median frame asks for 6.8 % of the bytes at 4K and 1.2 % at 1080p: what is needed
*now* is small even where nothing is unneeded *ever*, which is the bound's case (below), not the
feedback's. **The pass's cost**: the frame passes' zone (this pass plus the encode the linear image
needs) was a median 0.35 ms of the village's 6.5 ms frame at 4K (p99 0.56) and 0.30 ms of the
oasis's 3.3 ms, 0.09 to 0.12 ms at 1080p, beside other agents' GPU work, so upper bounds.

Of the village's 48 textures (21 at 2,048, 24 at 1,024: the generated plaster, timber, stone and
thatch sets and their normal maps), 14 were never sampled anywhere on the walk (the sets of the
houses the walk never saw from inside), 12 were asked no finer than level 2 at 4K (level 3 at
1080p), and 19 reached level 0. The finest level asked moves slowly: in the median frame no
texture's changes, at the 99th percentile three do, and the most in one frame was 34 (turning
through a door). static-world, without the two downloaded samples (`tools/fetch-samples.ps1` was
not run: nothing is downloaded by an agent), holds only its kit's four 256-texel textures and one
512, every one read at level 0: a scene of small textures has nothing to give, and says so.

**The design** ([ADR-0065](../adr/0065-texture-levels-resident-by-bound-and-feedback.md) has the
alternatives). The published shapes are four: **virtual texturing**, a page table over the world's
texels, a feedback buffer drawn at a fraction of the screen and a fixed pool of pages (Barrett,
"Sparse Virtual Textures", GDC 2008; van Waveren, "id Tech 5 Challenges", SIGGRAPH 2009; Chen,
"Adaptive Virtual Texture Rendering in Far Cry 4", GDC 2015; cited as known, not re-read);
**sampler feedback streaming**, D3D12's MinMip feedback map, which records with an atomic minimum
the most detailed mip each region of a tiled texture was asked for, is read back and reset as often
as each frame, loads on a lower value and unloads "after a period without use" (Microsoft, "Sampler
Feedback" specification, read 2026-11-16); **mip streaming of whole textures**, Unreal Engine's,
which works out each texture's wanted resolution from its primitives' world bounds and texel world
sizes against the view, and when the pool cannot hold them lowers textures "one mip at a time",
non-visible and least recently seen first (Epic Games, "Texture Streaming Overview", read
2026-11-16); and **sparse residency**, Vulkan's sparse images bound tile by tile with a mip tail the
driver chooses. This engine takes the third's shape with the second's signal, because:

- the resolve already has each pixel's exact UV derivatives from its own triangle ("Mip selection
  without quads"), so a feedback is one more pass over what it reads, with no extra raster;
- a material names a bindless slot, so a texture resident from level `r` is an image whose level 0
  is the chain's level `r`, sampled through the same `SampleGrad` with no shader change (a sampler
  on the smaller image reads its level `max(lambda - r, 0)`, which is level `max(lambda, r)` of the
  whole); a page table's indirection would be paid by every sample of the resolve at 3840 x 2160 for
  unique texels the engine does not have ("Textures that arrive after the load");
- a `.tex` stores each level at its own offset ("The container"), so a promotion reads by range
  only the levels it adds.

**Which levels stay.**

1. **The bound releases what no pixel can read, exactly.** Per texture, the finest level a pixel
   can read: from each mesh's measured density at its **least** over every triangle of every
   level ("Sized by the asset"'s measure; the 1 % figure is the cap's, an author's choice, not an
   automatic one; the least texels a metre is where a pixel magnifies most), at
   each live instance's scale, against the nearest any observer comes to the instance within the
   lead — the frame-corner formula of "Sized by the asset", `floor(log2(d0 / D))` levels out. It is
   evaluated over the instances that exist now, so a mesh a world's tail places larger raises it
   when it arrives, and G14's "open" meshes need no rule of their own. An imported asset gets it
   from its own triangles, with nothing stated.
2. **The feedback trims only under pressure.** Between the bound and the floor lie the levels the
   bound allows and no frame read: in the village, more than half the bytes. They are trimmed only
   when the pool is over its ceiling, a texture at a time to the finest level asked within the
   hold, least recently asked first. The production form samples one pixel in sixteen a frame,
   rotating (the D3D12 specification reports that discarding over 99 % of the writes still gave
   reliable data), and is not turned on until its cost over a flythrough at 3840 x 2160 is measured.
3. **The floor**: every texture keeps its levels of 128 texels a side and smaller, 0.4 % of a 2K
   chain, so a far lookup and a ray's level-0 read never find nothing; ADR-0063's floor names
   these ("the coarsest texture levels the far tier samples").

**Ahead of need, and the failure picture.** The bound is taken at the observer's place a lead
ahead (ADR-0063 decision 3's segment), the lead being the measured time from wanted to drawn: a
range read of the added levels on a worker, then admission inside R147's commit allowance (4 MiB a
frame by default: one 2K level 0 a frame). Under the 8 s hold the village asked 1.4 to 1.6 MiB/s on
average and 14 MiB in its worst frame — four frames of the allowance, 67 ms at 60 Hz — during
which the textures concerned draw their resident levels: softer for a few frames, never a wait.
The bound brings most of that in before the frame that needs it, since a walker's lead reaches a
surface before the eye's distance does. A settled view promotes nothing, so the steady frame loop
allocates nothing; a promotion allocates an image, as a tile's admitted texture does.

**What a card with 8 GB keeps.** The bound alone (no pressure): what the observers can resolve,
which for the village at 4K is the 74 MiB the walk's frames asked for between them plus the levels
the walk's closest approaches allow and no frame read; at 1080p every bound falls by a level, a quarter of the bytes.
Under a ceiling: the floor for every texture (0.4 %), and above it what the frames asked within the
hold, least recently asked trimmed first: the village's 8 s hold is 11 MiB at 1080p. **An imported
asset** with no statement of its own gets all three: its own measured density for the bound, the
feedback, the floor.

**The readers that name level 0.** The rays' mask tests (`mask_keeps_level0`, alpha_mask.slang),
the opacity micromap bake, the bounce's hit colour (`gi_update.slang`) and the reference path tracer
read level 0 explicitly; on a smaller image that is its resident top. So an alpha-tested material's
base colour is held whole until the rays take a level from a ray cone; the bounce reads the resident
top, a mean of the texels it read before (and the bounce is not repeatable frame to frame anyway);
the reference path tracer loads whole.

**How a promotion swaps in.** A bindless descriptor a frame in flight reads may not be rewritten
(Vulkan's `UPDATE_UNUSED_WHILE_PENDING` rule), so a managed texture holds two slot pairs: a
promotion writes the new image's descriptors into the idle pair, rewrites the material entries that
name the texture through the frame's "terrain upload" pass (the material table is imported as last
read everywhere, so its barrier waits for the frames in flight), and the old image goes
`k_joint_slots + 1` recorded frames later, as a leaving tile texture's does.

**ADR-0063's open decisions, and where each plugs in.** None is presumed. *The budget's form*
gives this pool its ceiling, whichever number it is; the bound needs no ceiling at all. *Being
outrun*: a promotion backlog whose drain time passes the lead is the texture form of flight, and
the wanted levels fall back to the bound at the observer's place now. *Fast travel*: covered, the
pool releases to the floor and promotes at the destination with the whole machine; shown, a preload
observer's bound counts as any observer's.

**Decided** (2026-11-17, the coordinator's judgement under the owner's delegation; ADR-0065,
accepted): the shape above; **the floor is `renderer.textures.floor`, 128 texels a side by
default** at the top tier (about 22 KB a BC7 texture; the failure picture after a cut, and the
smallest that does not read as a blur on a near prop at 4K; a lower tier may take 64, which stage 2
sets by measurement under an 8 GB budget); **the hold is a minimum age, not a clock** (under no
pressure nothing is trimmed; under pressure least recently asked first and only as far as the
ceiling needs, nothing younger than the hold). Revisit for a single unique texture past about 8K a
side, or sparse residency measuring well on the three test cards.

### Stage 1: the bound, built

**What it does** (R172's second leg, 2026-11-17; `systems/renderer/src/gpu_scene_texture_bound.cpp`,
`texture_bound_state.h`, `include/systems/renderer/texture_bound.h`; `renderer.textures.bound`,
**off by default**, for the reason "What it measured" gives). It changes no pixel, so it needs no
ceiling and decides nothing ADR-0063 leaves open.

1. **At load** (`bound_scene_textures`, scene.cpp) every mesh with built textures that nothing
   after the load may place — the cap's own rule (`open_scene_meshes`: not a streamed entry's
   resident mesh, a prop, an actor or a procedural mesh) — gets each image's **least density over
   every triangle of every level** of its cluster DAG (`texture::least_cluster_density`: a
   simplified level's triangles are drawn too and may stretch further than any source triangle) and
   the sphere round its vertices. The upload (`texture_bound_plan`, gpu_scene_texture_bound.cpp)
   holds a key by level only if **every** use of it can be: not an alpha-tested material's base
   colour (the rays' mask tests and the micromap bake read level 0 at any distance), not a mesh an
   instance deforms (the density is the rest pose's), not the terrain's (its layers sample by their
   own rule) nor a water body's. Such a texture is uploaded **from its floor** — its levels of 128
   texels a side and smaller, 1/64 of a 1K chain's blocks — into a pair of a reserved run of
   bindless slots (one pair a texture and 64 spare, never more than a quarter of the set's free
   slots: past that, keys are uploaded whole). A tile's mesh that names such a key is not offered
   the load's copy (`load_textures_` leaves it out) and brings its own whole one.
2. **Each recorded frame** (`GpuScene::texture_bound_frame`, called by `record_frame` before
   `terrain_prepare`): when the eye has moved half the last lead, the view changed, or 60 frames
   passed, the bound is worked out — per mesh the least `log2(r cos^2 phi_max / (f s))` over its
   instances, `r` the eye's distance to the instance's sphere less the lead (never under 1 cm),
   `s` the instance's largest axis scale, `f` and `phi_max` the camera's pixels a radian and the
   frame's corner; per texture `floor(log2 texels a metre + that − margin)`, the finest over its
   meshes. **The lead** is `renderer.textures.lead_m` (2 m) plus the last frame's step times
   `renderer.textures.lead_frames` (30). **A step longer than `renderer.textures.jump_m`** (1 m in
   one frame) is a teleport the host did not call a cut — a camera path's repeat, a jump to a
   marker — and is taken as one: the frame settles, and the step does not stretch the lead (the
   first pair, 2026-11-17, took a path's repeat as a velocity, stretched the lead over the whole
   scene and brought every level of the village and the oasis in). **The margin** (`renderer.textures.bound_margin`, 1.53125
   levels) is derived, not tuned: Vulkan lets an implementation take each `rho` from its components'
   maximum (as little as 1/sqrt 2 of the length: half a level) and round the anisotropic sample
   count up to a whole number (`rho_max / ceil(rho_max / rho_min)` is no less than `rho_min / 2`:
   one level), and the measure reads the float vertices where the rasterizers read the 16-bit grid
   (1/32). The evaluation walks the instance table once; it allocates nothing.
3. **A promotion** reads the texture's levels from the new top down by range on the bound's one
   reader thread (`texture::read_texture_file_levels`: the header, the tables and only those
   levels' blocks and variance; the load read the file whole and checked its content hash), then on
   the recording thread, inside `renderer.textures.promote_kib` a frame (4,096; one larger goes
   alone): a new image in a free pair, its levels staged into the frame's "terrain upload" pass, and
   the material records that name the texture rewritten to the new pair by copies in the same pass
   (the material table is imported as last read everywhere, so the copy waits for the frames in
   flight, which still read the old record and the old image). The old image and pair go
   `k_joint_slots + 1` recorded frames later. **A settle** — the first frame, a frame with
   `FrameDesc::history_reset`, and the frame after `texture_bound_whole` (the reference path tracer
   reads level 0 and asks for it) — reads and admits every wanted level on the recording thread
   before the frame draws: a cut's frame is a load's. With `renderer.textures.bound_wait` (tests) a
   frame whose eye can read a level not yet in brings it in itself; otherwise the frame draws the
   resident levels and counts itself **late** (`TextureBoundUse::late`).
4. **What it does not do**: release a level when the eye walks away (the levels a walk brought in
   stay until the scene goes: a demotion is the same swap the other way, and belongs with stage 2's
   trim, which decides when a release pays), and hold textures that arrive with a tile's mesh
   (they are uploaded whole, as before).

**The readers that name level 0**, as above: the alpha-tested base colours are not held by level
at all; the bounce's hit colour (`gi_update.slang`'s `sample_material`) reads the resident top of
the others, a mean of what it read before; the reference path tracer settles every texture whole
first. **With the bounce off the picture is the whole scene's to the byte**; with it on, the bounce
is not repeatable frame to frame anyway.

**What it measured** (2026-11-17, `msvc-release` copied out of the tree, against main's
`b66\bin-main` flown the same way, RTX 5090, `--raster rt --shadows rt`, the bounce on,
`--offscreen`; **the Windows session was locked for every flight on both sides**, and other agents
held 48 to 81 % of the processors, so frame times are upper bounds and only the pair's difference is
read):

| Flight | Texture bytes at the end, main / bound | Frame median, main / bound | Held by the bound | The settle (first frame) |
|---|---|---|---|---|
| village interior walk, 3840 x 2160 | 168.2 / 168.2 MiB | 6.02 / 6.01 ms | 33 of 48, all 33 at level 0 from the first frame | 168 ms (328 ms in a second flight) |
| village interior walk, 1920 x 1080 | 168.2 / 168.2 MiB | 2.60 / 2.59 ms | the same | 195 ms (163 ms) |
| oasis-palms flythrough, 3840 x 2160 | 28.2 / 28.2 MiB | 2.92 / 2.92 ms | — | — |
| oasis-palms flythrough, 1920 x 1080 | 28.2 / 28.2 MiB | 1.30 / 1.30 ms | — | — |

(The village's figures are from `FlythroughTextures` version 3, the oasis's from the bytes; the
village's 33 held textures were all promoted by the settle, none after it and none late, so the
bound made **no admission off the frame** on either walk, and the recording thread's longest
`terrain` stage on the bound's side, 60 ms, is the terrain tiles' own: main's reaches 42 ms on the
same walk.)

**The bound alone keeps every level of these scenes by the end of their walks**, at both
resolutions. Not a fault of the bound: the village's generated plaster, timber, stone and thatch
sets are **shared by every house** and tiled at about a thousand texels a metre, so a texture is as
fine as its nearest instance wants, and at 3840 x 2160 the nearest house wants level 0 from about
30 m (the margin, the frame's corner and the sphere's nearest point together keep the bound about
2.5 levels finer than a pixel at the centre reads); the interiors the first leg's feedback found
never sampled are behind walls a few metres off, which a bound by distance cannot see. The oasis's
palm cards are alpha-tested and its sand is the terrain's, neither held by level. What the bound
does save is **the load**: the village uploads 24 KB a texture where a whole 2K chain is 5.3 MB, and
its first frame then brings in what the start can read (the settle above, which a cut pays too).
A promotion off the frame costs the recording thread 0.44 ms for a 1K texture in `msvc-debug` (the
test's walk); the walks above made none, so a 2K texture's admission, with its image and its
staging made in that frame, is not measured at 4K yet. Allocations after the first 60 frames: 46 on both
sides (the village's own), none added; `engine_view.frame_loop` passes (0 allocations and 0 device
buffers in every second flight).

**What a card with 8 GB keeps.** With the bound alone, on these scenes: everything, 168 MiB for the
village at either resolution, the same as without it. The memory is in what no frame looks at, not
in what no frame could resolve, so it is stage 2's: the first leg's 8 s hold measured a median of
44 MiB at 4K and 11 MiB at 1080p for the same walk. **Hence off by default**: the bound earns its
first frame and its settles where textures are distinct and far — a city, a prop library seen from
a road, a scene at 1080p with 4K sources — and on its own changes nothing a player of these scenes
would see in their memory. Turned on, it changes no pixel (the tests above).

**Not built yet** (the production feedback, the trim and the demotion are stage 2's, below): a
persistent staging for promotions,
which would take the image's staging out of the frame that admits it; the bound for textures that
arrive with a tile's mesh; and a measurement of each card's anisotropic level of detail, which could
take a level off the margin where a card's rounding is shown to be the specification's.

### Stage 2: the trim under a ceiling

**What it does** (R172's third leg, 2026-11-17; `gpu_scene_texture_bound.cpp`, the residency half
of `texture_feedback.cpp`; ADR-0065 decision 3 and its settlement). Off unless the scene's textures
have a ceiling, **`renderer.textures.ceiling_mib`** (MiB of texture the scene may hold, 0 by
default: no ceiling, nothing trimmed, the load's textures and the load's picture to the byte). The
ceiling is this row's own tunable so that ADR-0063's three open decisions stay open: whichever form
that ADR's budget takes, this pool's share of it is the number to put here (below, "Where ADR-0063's
budget plugs in"). With a ceiling and the bound off, the load uploads every managed texture whole
into a pair of the run, exactly as stage 1 would with no level left out; with both, the load uploads
from the floor and the bound brings in what an observer can read, as before.

1. **The feedback, always on under a ceiling.** The measurement pass in its production form: one
   pixel in sixteen of every view's visibility buffer — the pixel at a phase of each 4x4 tile that
   steps through the tile's sixteen in an ordered-dither order, so consecutive frames read pixels far
   apart and sixteen frames read every pixel once — over the run of slots the residency manages
   only (a table of `2 x pairs` words, cleared, written by an atomic minimum, copied back), recorded
   by `record_frame` after the resolve in any view mode and **not** as a frame pass, so it draws
   nothing through the linear image. It runs at every frame under a ceiling, not only over it,
   because a trim needs the history before the pressure comes. Its fold, one frame slot later, maps
   each pair to the texture it held when that frame was recorded (a pair keeps its texture while it
   leaves, and one given out after the folded frame is not read), adds the pair's resident top to
   the resident image's lambda (`GetDimensions` reads the resident image: the chain's level is
   `floor(lambda) + top`), and stamps the residency's clock on that level and every coarser one. So
   **the history is the texture's, not the slot's**, and survives every promotion and demotion (the
   measurement's own list now keeps a load's record by its place, which a swap never moves, where it
   used to key it by slot and lost it at the first promotion).
2. **The clock** is the frames' own, `FrameDesc::dt_s` or 1/60 s where the host steps none (a
   flythrough's fixed frames), so a hold in seconds means the same in a test, a flight and a game.
   A level's age is the time since the feedback last asked for it or a coarser-first ask reached it,
   **or since it was brought in**, whichever is later; the load brings every level in at 0.
3. **The trim** (`texture_bound_frame`, after the promotions are asked): only when the scene's texture
   bytes are over the ceiling (less what demotions already in flight will give back). Candidates are
   each managed texture's levels from its resident top down to the floor, while each is at least
   `renderer.textures.hold_s` old (8 s; a minimum age, not a clock); since an ask of a level is an ask
   of every coarser one, a texture's levels are oldest at the top, so the candidates sorted by last
   ask (then texture, then level) take each texture's levels from the top down, least recently asked
   first, and the walk stops as soon as the levels taken cover the excess: **only as far as the
   ceiling needs**. Each texture the plan touched is demoted once, to its planned top, by the same
   path a promotion takes the other way: its levels from the new top read by range on the reader
   thread, a new smaller image in a free pair, the material records rewritten in the frame's upload
   pass, the old image gone `k_joint_slots + 1` frames later. **A demotion is dropped on arrival if
   any level it would release was asked or brought in within the hold since it was planned**
   (`TextureBoundUse::refused_in_hold`), so what goes is never younger than the hold. Never below the
   floor (`renderer.textures.floor`, 128 texels a side).
4. **The return.** Under a ceiling a texture wants what the feedback asked: where a fold asked for a
   level finer than the resident top (`asked_late`, counted per texture and frame), a promotion to it
   is asked off the frame, **a level further ahead where the asks are getting finer** (the trend
   between the last two asked frames: a walker closing on a wall), and, with the bound on, **the
   bound's lead where the pool has room for it** under the ceiling — never finer than the bound. The
   level arrives through stage 1's promotion path inside `renderer.textures.promote_kib` a frame;
   until it does the texture draws its resident levels, softer, never absent and never a wait. A
   promotion is as young as an ask, so the hold keeps what just came back.
5. **No allocation in the steady frame loop.** The plan's candidate list and per-texture plan are
   reserved at the load; the feedback's buffers are made at the renderer's creation; a settled view
   demotes and promotes nothing. A demotion allocates its image and its staging in the frame that
   admits it, as a promotion does.

**What the feedback costs, and what one pixel in sixteen misses** (2026-11-17, `msvc-release`
copied out of the tree, RTX 5090, the village's interior walk, `--raster rt --shadows rt`, the
bounce on, `--offscreen`; **the Windows session was locked** for every flight, other agents held 31
to 41 % of the processors). The residency's own pass, timed by its zone over every frame of the
walk under a ceiling the pool never reached (100,000 MiB): **0.044 ms a frame at 3840 x 2160 (the
most 0.089 ms) and 0.019 ms at 1920 x 1080 (the most 0.21 ms, a frame beside other agents' GPU
work)**, under the 0.1 ms ADR-0065 set as the line past which the feedback would have to ride on the
resolve's own derivatives. The frame medians, against the same code with no ceiling: 6.009 against
6.052 ms at 4K, 2.556 against 2.585 ms at 1080p, the feedback's own time and nothing else. So it
runs at every frame under a ceiling, not only under pressure. Against every pixel, over the same
walk (the measurement runs both patterns in one frame; 8,829 measured frames at 4K, 10,186 at
1080p, repeats included):

| | 3840 x 2160 | 1920 x 1080 |
|---|---|---|
| textures asked in a frame (median) | 25 | 25 |
| textures every pixel asked and the sparse pattern missed, a frame (median / p99 / most) | 0 / 3 / 3 | 0 / 3 / 7 |
| frames with any such miss | 1,183 (13 %) | 438 (4 %) |
| textures the sparse pattern saw coarser, a frame (median / p99 / most) | 0 / 3 / 6 | 0 / 3 / 6 |
| an 8 s hold's bytes, every pixel / sparse (median) | 44.4 / 44.4 MiB | 3.9 / 3.9 MiB |
| frames where the sparse 8 s hold keeps a texture coarser than every pixel's | 443 (5.0 %, at most 3 textures) | 156 (1.5 %, at most 6) |
| bytes any frame asked for, every pixel / sparse | 74.2 / 74.2 MiB | 71.1 / 71.1 MiB |

What the sparse pattern misses in a frame is a texture a handful of pixels show (a sliver at the
frame's edge, a far window), and an 8 s hold hides almost all of it: the hold driven by it keeps the
same bytes, and in one frame in twenty at 4K keeps up to three textures a level coarser than every
pixel would — which only matters under pressure, and then those pixels draw a level softer until the
next ask, which the hold's minimum age makes rare. Over the whole walk it asks for exactly what
every pixel does.

**The failure picture, measured** (the same machine and session state; the measurement on, which
counts every pixel whose finest asked level is finer than its texture's resident top, by how many
levels). Under a ceiling of 24 MiB — below what the village's 8 s hold keeps at 4K (44 MiB), so
every frame of the walk is under pressure — the interior walk at 3840 x 2160 made 261 demotions
giving back 346 MiB and 147 promotions, ended at 95.3 MiB of the whole 168.2 (the most at once
130.4 MiB), and drew **1.88 million pixel-frames one level softer than they asked, 0.0046 % of the
walk's textured pixel-frames**: 262 frames in 48 episodes, the median episode 5 frames (83 ms) and
the longest 11, the worst frame 2.2 % of its textured pixels; none two levels or more; no demotion
was dropped for an ask inside the hold. The recording thread's worst frame of admissions was 1.54 ms
(2K promotions with their staging made in the frame) and of demotions 0.66 ms. A turn round on the
spot (`village` lane start, the same ceiling, flown three times over with identical counts each
time):

- **round and back within the hold** (4 s facing behind): **nothing** — not one pixel softer, since
  every level the view asked for was asked 4 s before;
- **back after a minute**: during the half-second turn, 13 frames softer, all but one pixel-frame in
  a thousand by one level: 39 % of the textured pixels for 3 frames (the wall the turn reaches first,
  whose top levels went after 8 s unasked), 11 % for 5, 2.7 % for 4, 0.07 % for 1; by the time the
  view settles forward it is the whole picture;
- **onto a view not seen for longer than the hold** (the first turn of each repeat): up to 56 % of
  the textured pixels one or two levels softer for 6 frames, and three levels or more on at most
  38,000 pixels for 7 frames.

What a person sees: the worst frame of the return after a minute (path frame 4,395, captured with
and without the ceiling) shows no difference I could find at full size — the softer pixels are the
plaster and thatch of the house at the frame's edge some 20 m off, where one level of a 1,024-texel
set is below what a 4K pixel resolves there — and within a tenth of a second it is the whole
picture again; never a hole or a wait, and nothing at all for a glance back within the hold. Under
no pressure (any ceiling the pool does not reach) there is nothing to see: the trim never runs.

**What a smaller card keeps** (the same machine and state; ceilings from what the rest of the
engine holds, `--memory-report`'s device-local usage at exit on main less the textures: the village
3,749 MiB at 4K and 3,597 at 1080p, of which the ray-tracing structures are 2,099 and the tiles 600;
the oasis 707 and 683; a card's room taken as 0.97 of it, the share this card's memory budget
reports, less 512 MiB for the desktop). An 8 GB card leaves the village 3,685 MiB of texture at 4K
and the oasis 6,727: no pressure, nothing trimmed. **A 4 GB card holds less than the rest of the
village at either resolution**, so its ceiling is no room at all (1 MiB: everything under pressure,
the textures trimmed to what the hold keeps); the oasis leaves 2,754 MiB and its textures (all
alpha-tested cards and the terrain's, none managed) are untouched. Flown as pairs, `--raster rt
--shadows rt`, the bounce on, session locked, other agents at 4 to 29 % of the processors:

| Flight | main | before (stage 1) | 8 GB | 4 GB |
|---|---|---|---|---|
| village 4K: median / p99 / most, ms | 6.000 / 7.13 / 49.8 | 6.021 / 7.60 / 66.5 | 6.062 / 7.26 / 45.3 | 6.060 / 7.66 / 59.7 |
| village 4K: texture bytes at the end | 168.2 MiB | 168.2 | 168.2 | 95.3 (261 demotions, 147 promotions) |
| village 1080p: median / p99 / most, ms | 2.551 / 3.97 / 34.9 | 2.554 / 4.24 / 34.5 | 2.581 / 4.11 / 34.0 | 2.605 / 4.33 / 34.5 |
| village 1080p: texture bytes at the end | 168.2 MiB | 168.2 | 168.2 | 94.0 (228, 198) |
| oasis 4K: median / p99, ms | 2.909 / 3.42 | 2.951 / 3.51 | 2.974 / 3.55 | 2.910 / 3.42 |
| oasis 1080p: median / p99, ms | 1.289 / 1.62 | 1.287 / 1.61 | 1.293 / 1.63 | 1.295 / 1.67 |

The ceiling costs the feedback's 0.04 ms at 4K and 0.03 ms at 1080p and the trim nothing more that
the medians show; the worst frames are the walk's own (its loads and turns, main's included). The
recording thread's worst admission on the 4 GB walk was 2.51 ms at 4K (a 2K texture's image and
staging made in the frame) and its worst demotion 0.47 ms. **What the trim does not yet do**: the
driver's device-local usage at the end of the 4 GB flights is main's to the MiB (3,917 at 4K), since
the trimmed images leave free space inside the allocator's blocks, which the engine's next
allocations take but the driver does not get back; on a card short of memory the trim makes room
for the rest of the engine, and does not yet shrink the process. And on these scenes the textures
are a twentieth of what fills a 4 GB card at 4K; the structures are most of it.

**Tested by** (`systems/renderer/tests/texture_tests.cpp`, through `test_device.h`): the trim keeps
what was asked within the hold and the floor (a 1 MiB ceiling releases level 0 alone and no earlier
than the hold; a 10 KiB one goes to the floor and no further); with no ceiling, or one the pool
fits under, every capture and the texture bytes are the load's; a trim then a return ends on the
whole picture byte for byte.

**Where ADR-0063's budget plugs in.** The ceiling is texture bytes on the device, the pool's figure
in ADR-0063's sense. Under *the budget's form* still open there, a governor sets this number from
whatever budget is chosen (a device budget less what the other pools hold, or a share), at the load
and again whenever it moves; a card's figure below ("What a smaller card keeps") is derived the
first way, from what the rest of the engine holds in `--memory-report`. Nothing here presumes which.

### Stage 3: the bytes go back to the device

**Why** (W167, W47's stage 1, 2026-11-23; [the residency design](../experiments/residency-manager-design.md#stages-in-build-order), row 1). The owner reads a launch's commit as a budget like frame time, and on Windows every device byte is a byte of the process's commit. Stage 2's trim released levels but not bytes: a trimmed image's range stayed inside the allocator's shared blocks, so the driver's usage for the process stood where main's did. This stage is what the design's table asks of row 1: the held textures' allocator in chunks of their own, so a trim's bytes leave the driver's usage; the ceiling from a device budget where one is named; and a persistent staging for promotions, so that a promotion allocates nothing in a steady frame.

1. **Chunks.** Every image the residency holds — the load's upload of a held texture, a promotion's, a demotion's, its variance chain beside it — is placed in the held textures' own `gfx::ImageChunks` ([gfx](gfx.md#images-in-chunks)) of `renderer.textures.chunk_mib` (64 MiB, read once per load): the fullest chunk it fits in, a chunk of its own when it is larger than one, and a chunk goes back to the device with its last image (one empty chunk is kept as a spare). The textures the residency does not hold (an alpha-tested base colour, a deformed or open mesh's, the terrain's and the water's) are the allocator's, as before.
2. **A sparse chunk is drained.** A trim leaves its holes wherever the trimmed images were, so a chunk rarely empties of itself. Where the holes in the chunks that hold anything pass one and a half chunks, the sparsest of them is marked draining — but only where what it holds fits in the other chunks' holes, since a move that made a chunk would give nothing back — and every texture with an image in it is **moved**: its levels from the same top read again by range on the reader thread and re-placed through the promotion's swap, inside the same `renderer.textures.promote_kib` a frame, the material records rewritten behind the frames in flight and the old image gone `k_joint_slots + 1` frames later. A move changes no level and no pixel (a texel of the new image is the same block from the same file), is counted (`TextureBoundUse::moves`, `moved_bytes`, `drains`), and is dropped on arrival where the texture was swapped meanwhile or its chunk is no longer draining. One chunk drains at a time. Moving rather than compacting in place is the swap path's: a bindless descriptor a frame in flight reads may not be rewritten, and the swap already waits that out.
3. **The ceiling from a device budget.** `renderer.textures.device_budget_mib` (0 by default: none named) is the device the process may hold; with no `ceiling_mib` named it sets the ceiling to the budget less what everything else holds on the device — the driver's usage for the process (`gfx::Device::memory_budget`, VK_EXT_memory_budget) less the held textures' chunks and the other textures' images — worked out at the first frame, at a cut and every 10 s of the frames' clock, a driver query no more often than that. The budget is this tunable until ADR-0063's form settles ([the design's question 1](../experiments/residency-manager-design.md#open-questions-for-the-owner)); a governor that sets it is that ADR's.
4. **A persistent staging.** A scene whose textures are held makes the terrain's staging ring (`renderer.terrain.staging_mib`) even where it has no rings and no mesh room, so a promotion, a demotion or a move stages its levels there and makes no buffer; one larger than the ring makes a buffer of its own, counted as before. The reads a frame asks carry a pointer to the texture's path, so asking one allocates nothing on the frame's thread.
5. **A tile's mesh shares a held texture** (under a ceiling alone). Stage 1 kept a held texture from the meshes tiles hand ("a tile's mesh that names such a key is not offered the load's copy"), because the bound does not see a tile's instances. Under a ceiling with the bound off nothing needs it to: the feedback reads every pixel that samples the pair, whoever drew it. So the load's held textures are offered to admitted meshes at their **current** pair (`GpuScene::texture_bound_slot`, through `find_texture`), an admitted mesh's records that name a held pair are kept with the texture's own (`texture_bound_admitted`, given back by `texture_bound_retired`), and a swap rewrites them with the rest; a record whose mesh's whole block of records is copied in the same frame is written into that block's staged bytes instead of a second copy, since two copies into the same bytes in one pass are not ordered. With the bound on the old rule stands. **Why it had to change**: on the forest staging under main's ceiling every tile mesh naming a held texture read and uploaded its own whole copy — the world's first activation of its 1,256 tiles dispatched for 101.8 s against 4.3 s, the process's commit rose to 49.9 GB (the machine's free commit to 0.2 GB), only 1,566 of the load's 5,440 textures were offered to tiles, and a tile mesh the room's `renderer.textures.budget_mib` (1 GiB) refuses draws its materials' constants.
6. **The reader holds at most 64 MiB of reads not yet admitted** (or four frames' allowance, whichever is more; one read always goes when nothing is in hand): a trim plans thousands of demotions at once and the admission takes `promote_kib` a frame.

**The default is main's**: with no ceiling and no budget named, nothing above runs and a scene is main's bytes and main's picture; the chunks exist only where the residency does.

**Tested by** (`texture_tests.cpp`, "a trim gives its chunk back, and a trim or a promotion allocates nothing"): the quad's 1,024-texel map under a 10 KiB ceiling in 1 MiB chunks; a glance away shorter than the hold and back reads nothing again; then two cycles of a trim to the floor and a promotion back, the second counted: the whole chain's own chunk goes back (one live chunk of 1 MiB, one freed, and the driver's usage for the process down by at least half the chain's chunk), and every frame of the cycle, the trim's and the promotion's included, makes no engine allocation and no device buffer on the frame's thread. "a device budget names the texture ceiling from what the rest holds": a 1 MiB budget sets the least ceiling and the map goes to its floor, a 64 GiB one trims nothing. `mesh_texture_tests.cpp`, "under a texture ceiling a tile mesh shares the load's held texture": four tile rocks under a 1 KiB ceiling upload only the base colour map the load does not hold, the trim swaps the held ones while frames run, and the last frame is the scene read whole byte for byte. `image_chunks_tests.cpp` in gfx holds the pool itself.

## Samplers and texture transforms

**Why.** Until 2026-09-24 every material sampled through one clamped sampler and the UVs as the mesh
carried them, so a tiling texture could not tile: a brick texture meant to repeat eight times across
a 10 m wall drew once, stretched, and the clamp smeared its edge texels over everything past UV 1.
That was the one thing the ruined-wall kits of [07 §7.7](../plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content)
could not do without, since their surface detail is tileable PBR at a declared texel density.

**What the file says reaches the pixel.** The importer reads each slot's glTF sampler (wrap s and t,
magnification and minification filters) and `KHR_texture_transform` (offset, rotation, scale) into
`assets::Material::sampling` ([assets](assets.md)); the container carries them in section 33
([geometry](geometry.md#how-a-material-samples-its-images)), a container from before it reading the
glTF defaults — repeat, linear, no transform; and the renderer makes **one bindless sampler per
distinct (wrap s, wrap t, filters, mipmapped) combination** the scene's textured slots ask for,
made when the first slot asks and shared by every material after it (`GpuScene::material_samplers`).
A built texture's sampler samples the whole chain, blends levels as the file asks (linear unless a
`*_MIPMAP_NEAREST` filter says otherwise), and is 16x anisotropic where the device has
`samplerAnisotropy` and the minification filter is linear — the pipeline's trilinear-anisotropic
sampler, now with the file's wrap. A decoded texture's sampler reads level 0 alone, as the decoded
path always has, with the file's wrap and filters. `NEAREST` magnification is honoured, so a
pixel-art texture stays crisp; a glTF `NEAREST` or `LINEAR` minification filter, which asks for no
mip chain, is not, because level 0 alone aliases and the chain is always built
([geometry](geometry.md#how-a-material-samples-its-images)).

**Every slot names its own sampler** — the material table's `sampler` for the base colour and two
16-bit halves of two words for the other four — because a sampler is an index and costs the table
eight bytes. **The UV transform is one per material**, the base colour's (or the first textured
slot's): 24 bytes a slot on every material would have taken the table from 112 bytes to 208, and
every exporter the corpus came from writes one mapping for all of a material's textures. The
container keeps the transform per slot, so a renderer that carries more needs no rebuild; a material
whose slots disagree is counted (`GpuScene::transform_conflicts`) and logged, and draws every slot
with the one it kept. The transform is the extension's `T · R · S`: with c and s the rotation's
cosine and sine, `u' = c·sx·u + s·sy·v + ox` and `v' = −s·sx·u + c·sy·v + oy`, a counter-clockwise
turn of the image as it appears (v runs down it). `gfx::set_uv_transform` computes it on the CPU and
leaves `k_material_uv_transform` clear for the identity, so a material without one reads its UVs with
no arithmetic; with one, the resolve transforms the UV after `uv_scale` and the derivatives by the
matrix's linear part, so a texture tiled eight times selects its mip level for eight tiles.

**The mips follow the wrap** ("The mip chain" above), which is what makes a tiling texture tile at a
distance too and not only up close.

## Emissive

The emissive factor times the emissive texture (sRGB colour, so the sample is linear) is **radiance
the surface gives off**: the resolve adds it to the pixel after the lighting, touched by no shadow
and no occlusion, and the reference path tracer adds it wherever a ray lands — the camera ray sees it
directly, and a bounce ray picks it up as light arriving from that direction — through the same
`sample_material` in `material.slang`, so the two read the same texel. **v1 is radiance only**:
nothing samples emissive triangles as lights (next-event estimation towards them, with the MIS
weight it would need against the BSDF ray, is the step after area lights), so an emitter lights its
surroundings in the reference by being hit and not at all in the real-time picture, whose lighting
is the sun, the analytic lights and the sky until Phase 2. Before this, a material with an emissive
texture drew with no emission at all — the resolve had no slot, and full factor everywhere would have
been a bigger lie than none — which the Lantern's glass was.

## Occlusion

The occlusion texture's red channel with glTF's strength, `1 + strength · (r − 1)`, multiplies **the
resolve's indirect term and nothing else**. Today that term is the hemisphere of sky above and lit
ground below ([renderer](renderer.md#the-sky-above-the-ground-below)) — the resolve's whole
ambient, until Phase 2's indirect lighting exists — so an occlusion map darkens the ambient in
creases and leaves the sun, the point lights and the emission alone: it is a baked estimate of how
much of the hemisphere a point sees, and direct light's shadows already answer that for direct
light. **It is never applied in the path tracer**, which computes that visibility by tracing and
would count it twice. When Phase 2's indirect lighting arrives the map multiplies that instead,
still never the direct term. `--view occlusion` (`gfx::ResolveMode::Occlusion`) shows the value
unencoded — a captured byte is the occlusion times 255, 1 where the material has none — beside
`--view albedo`, which is how a test or a person checks what the map says without the lights in the
way. An ORM image (occlusion in red beside roughness and metallic) is one texture serving both
slots, as it already was.

### Mip selection without quads

A visibility-buffer resolve is a fullscreen pass, so the 2x2 quad a derivative instruction
differences holds pixels of **other** triangles wherever an edge crosses it — other meshes, other UV
islands — and `ddx(uv)` there is garbage. The resolve instead computes the UV's derivatives from the
hit triangle itself (`screen_uv_gradients`, `material.slang`). The pixel's barycentrics are where
its ray meets the triangle, `b_i = e_i / E` with the homogeneous edge functions
`e_i = (c_j × c_k) · p` of the corners' clip-space (x, y, w) and the pixel's direction
`p = (ndc.x, ndc.y, 1)`, and `E = Σ e_j` ([gfx](gfx.md), "Where a pixel meets its triangle"); `e` and
`E` are linear in `p`, so `db_i = (n_i - b_i N) · dp / E` with `n_i = c_j × c_k` and `N = Σ n_j`;
and `duv = Σ db_i uv_i`, taken as `db_1 (uv_1 - uv_0) + db_2 (uv_2 - uv_0)` because the derivatives
sum to zero. No w is divided by, so a triangle reaching behind the camera filters as rightly as one
in front of it. That is one more pass over three vertices the resolve has just read, only for a
mipmapped material, and `SampleGrad` then chooses the level and the anisotropy. Derivatives are per
pixel of the visibility buffer: a Panini view's output pixels are not those, and its textures are
filtered for the source it is resampled from.

## What it measured

All on the development desktop (i9-10980XE-class, 36 logical CPUs; RTX 5090), `msvc-release`, on the
FlightHelmet sample (fifteen PNGs: twelve 2048x2048, three 1024x1024, 45.1 MB). The machine was
shared with other agents' builds throughout: the CPU was 17–49% busy before each build and the GPU
held by another agent's lock between runs, so times are upper bounds.

**Size.** The fifteen `.tex` files are **71.3 MB** against the PNGs' 45.1 MB on disk (a 2048² BC7 or
BC5 chain is 5.59 MB; a PNG of a photographed texture compresses better than a GPU block format,
which has to be randomly addressable). On the GPU it is the other way: see VRAM below.

**Quality**, level 0 against the level the encoder was given, over the channels the format keeps:
BC7 sRGB base colour 49.4–54.9 dB (worst texel 11–65 codes), BC7 linear ORM 46.9–50.0 dB (worst
33–171: a block whose R, G and B are unrelated maps is BC7's hard case), BC5 normals 45.5–53.6 dB
(worst 16–22), and the Lenses' constant normal map exact.

**Build time per image**, `engine-content texture` on one 2048² image, `--jobs 1` and the default
(35 workers): base colour (BC7 sRGB) **2.71 s and 0.71 s**; normal (BC5) **4.91 s and 0.71 s**;
ORM (BC7 linear) **2.07 s and 0.70 s**. Of that, the PNG decode is 0.11–0.14 s and the mip chain
0.05–0.14 s; the rest is the encoder, and BC5's high-quality BC4 search is the slowest of the
three. CPU 17% busy.

**The whole FlightHelmet** (`engine-content build --cache`, container and fifteen textures, a fresh
root each time), wall time by `--jobs`: 1: **31.2 s**; 2: 30.3 s; 4: 19.8 s; 8: 11.6 s; 16: 6.6 s;
default (35): **3.8 s** — 8.3x on 35 workers with the CPU 18–22% busy beforehand. The container is
0.2 s of it. The first version built the textures one after another, each spread over the pool by
block rows, and stopped at 8.5 s on 35 workers: the decode and the small levels are serial within a
texture. One job per texture, each nesting its block rows on the same pool, took it to 3.8 s.

**VRAM.** Decoded, the fifteen images take **213.9 MB** of device memory — RGBA8, one level each,
no mips at all. Built they take **71.3 MB** with every level (`"textures":{"bytes"}` in
engine-view's summary, what the allocator gave the images): **3.0x less while adding the whole
chain**, and 4x less at level 0 alone, one byte a texel for BC7 and BC5 against four. The process's
device-local use as `VK_EXT_memory_budget` reports it fell from 780 to 524 MiB at 1920x1080 and from
1030 to 646 MiB at 3840x2160 — more than the 136 MB the images account for; the rest is not
attributed here.

**The resolve**, `gpu_ms.resolve` in milliseconds, the lit view with ray-traced shadows, 600 frames
windowed at a fixed orbit distance, two runs each (they agreed within 0.0005 ms). *Before* is the
parent commit's binary; *decoded* is this one with `--no-cache`, the path a mesh without built
textures takes; *built* reads the fifteen `.tex` files:

| frame | orbit | before | decoded | built |
|---|---|---|---|---|
| 1920x1080 | 10 | 0.0914 | 0.0908 | 0.0933 |
| 1920x1080 | 20 | 0.0569 | 0.0557 | 0.0556 |
| 1920x1080 | 60 | 0.0280 | 0.0277 | 0.0277 |
| 3840x2160 | 10 | 0.2885 | 0.2865 | 0.2951 |
| 3840x2160 | 20 | 0.1800 | 0.1762 | 0.1773 |
| 3840x2160 | 60 | 0.0795 | 0.0814 | 0.0770 |

**Against the parent commit the resolve moved by no more than 3% either way**, and the
direction is the one the shape of the change predicts: up close (orbit 10), where every path
samples level 0 anyway, the built path pays 2–3% for the derivative pass over the hit triangle's
vertices; far away (orbit 60 at 4K) it reads small levels and saves 3% against the parent and 5%
against the decoded path. The FlightHelmet's resolve is
not texture-bandwidth bound on this card, so what the pipeline buys here is the memory, the
filtering, and the load time — not milliseconds. Taken under the GPU lock (the card 1–16% busy
before each run) while other agents' builds held 15–100% of the CPU, so by the harness's rule these
are upper bounds; the GPU-timed pass does not wait on the CPU.

**The pictures.** The decoded path's capture (1280x720, orbit 12, the lit view) is **byte-identical**
to the parent commit's: the flags are zero and the resolve takes exactly the old branch. Built
against decoded, lit: PSNR **42.0 dB**, SSIM 0.992, FLIP mean 0.0069 (p95 0.032); the albedo view
alone: PSNR 42.7 dB, FLIP mean 0.0064. The difference sits on silhouettes and fine detail, which is
where filtering through the mips and not aliasing level 0 is supposed to put it — it is not all
compression error.

**In the renderer's tests** (`systems/renderer/tests/texture_tests.cpp`, RTX 5090): level 0 built
against decoded **49.3 dB**, worst channel 7 codes; the one-texel checkerboard at 7.8 texels a pixel
reads a mean of **187 with no spread** built, against 182.6 with a spread of 32.9 decoded; and the
level the sampler used lands **0.05–0.07 of a level** above what the finite difference predicts at
all four distances (the bias the next paragraph explains, not the derivatives).

**The blend between two levels is the driver's.** Swept on the RTX 5090 with the driver's defaults,
a level of detail a fraction f of the way from level L to L + 1 reads as `min(1, 2f)` of the way —
the level of detail the derivatives chose is right (the probe reads within 0.02 of L at whole
levels), and the driver then shortens the trilinear blend, NVIDIA's "trilinear optimization". It
is a control panel setting, it is legal filtering, and it is not the engine's to override; the
renderer's test therefore probes just past whole levels, where it tests the derivatives and not the
driver's blend, and anyone comparing filtered pictures across vendors should expect the difference.

### What the samplers, emission and occlusion cost

Measured 2026-09-24 on the RTX 5090, `msvc-release`, offscreen (`engine-view --benchmark` over a
still camera path, 301 frames × 3 repeats after the warm-up, ray-traced shadows), the base
commit's binary (*before*) against this one's (*after*), interleaved run by run under the GPU lock.
The machine was not quiet: other processes held 1–28% of the CPU at the samples around each run
and the GPU was 0–9% busy before each, so these are upper bounds, but before and after ran under
the same conditions minutes apart.

**Samplers.** The FlightHelmet (seven materials, fifteen built textures) draws through **one**
material sampler, and so does the Lantern (four decoded images, the emissive one now among them):
every file of the sample corpus names the glTF default sampler or none, and one combination is
one sampler however many materials share it. The fifteen `.tex` files are 71.3 MB either way; the
edges change which texels a level holds, not how many.

**The resolve**, `gpu_ms.resolve` median in milliseconds, cameras 0.9, 1.8 and 5.4 m from the
helmet (it fills the frame at the first):

| frame | camera | before | after | change |
|---|---|---|---|---|
| 1920×1080 | 0.9 m | 0.0704 | 0.0733 | +4.2% |
| 1920×1080 | 1.8 m | 0.0382 | 0.0394 | +2.9% |
| 1920×1080 | 5.4 m | 0.0265 | 0.0269 | +1.6% |
| 3840×2160 | 0.9 m | 0.2135 | 0.2247 | +5.3% |
| 3840×2160 | 1.8 m | 0.1051 | 0.1100 | +4.7% |
| 3840×2160 | 5.4 m | 0.0549 | 0.0561 | +2.2% |

**Up to 5% of a pass that is a fifth of a millisecond at 4K**, and it scales with the covered
pixels, as a per-pixel cost does. About half of it is applying the occlusion: the same build with
the occlusion multiply taken out cost +2.7% and +3–4% at the two nearest 4K cameras, so the rest —
the material record grew from 64 to 112 bytes and every textured slot picks its own sampler — is
about 3%. Reading an ORM material's occlusion out of the metallic-roughness texel already fetched,
rather than fetching it again, was tried and measured no better (+4.8% and +6.4% against the
table's +4.2% and +5.3%, within run-to-run spread): the second fetch hits the texel the first just
brought in, so it was not kept.

**The pictures.** The FlightHelmet lit (1280×720, orbit 12), before against after: PSNR 39.2 dB,
SSIM 0.993, FLIP mean 0.017 — the occlusion darkening the sky term in the creases, which is the
change and not an error. The albedo view: PSNR 81 dB, the repeating mip edges moving almost nothing
on a model whose UVs stay inside their atlas. The Lantern lit: FLIP 0.0065, all of it the glass,
which glows now and was dark before. Against the reference (`tools/ci/reference-compare.ps1`, the
whole corpus, 1024 samples) every scene stays inside its band and the two that changed moved
towards the reference: `flighthelmet` 0.0172 (0.0190 in [renderer](renderer.md#reference-renderer)'s
table; the occlusion is a crude stand-in for the hemisphere visibility the reference traces, and
it is the right direction), `thin-geometry` 0.0048 (0.0051; the Lantern's emission now in both
pictures), `helmet-grid` 0.0052, `heightfield` 0.0348, `wide-frustum` 0.0014, `shredded-atlas`
0.0575 unchanged.

### What a cap saves, and what geometry costs beside it (G14)

2026-11-15, `msvc-debug` at b95676a7 plus G14, RTX 5090, the machine shared with other agents' builds
and GPU tests (no quiet wait; every figure below is from one binary, so the load is the same for
both sides of a pair). The fixture is generated in a scratch directory, never committed: eight
20 cm boxes, every face the whole of its box's own 2,048 base colour, and a 2,048 normal map the
eight share — nine distinct textures at 10,240 texels a metre. Flown at 3840 x 2160, `--raster rt
--shadows rt`, the bounce off so the pair can be exact, from 2 m (the nearest face 1.9 m away, whose
corner figure above is 2,323 texels a metre: a cap of 512, two levels out), warm runs:

| | `renderer.textures.texel_density` 0 (main's keys) | 2,323 |
|---|---|---|
| textures on the device (`place.textures_mib`) | 50.7 MiB | 3.2 MiB |
| device-local usage | 446.4 MiB | 382.4 MiB |
| device-local blocks committed / used | 265.3 / 220.1 MiB | 201.3 / 172.6 MiB |
| process commit at exit (peak) | 622.7 MiB (691.1) | 560.6 MiB (629.1) |
| pixels differing at 3840 x 2160 | — | **0 of 8,294,400** |

The textures fall to a sixteenth, as two levels out should; the device's usage falls by the
allocator's blocks it no longer needs. Every sampled texel lies on a level both builds share, so the
picture is the same bytes.

**Geometry beside it.** A 27,918-triangle, 20 cm sphere (the village's densest prop is 28k) builds
to a 1.63 MB container: 639 clusters over ten DAG levels, 301 of them level 0, about 58 bytes a
source triangle for every level and about half of that for level 0. The same prop with three 2,048
images samples 16.8 MB of built textures, so its finest level is about 5% of what it costs and the
whole DAG under 10%. A triangle floor by size would buy at most that fraction, and it is not built.

## Public API

- `domain/texture/texture_file.h` — `TextureFormat`, `ColorSpace`, `k_texture_normal_map`,
  `k_texture_has_alpha`, `k_texture_alpha_coverage`, `texture_format_name`, `color_space_name`, `parse_texture_format`,
  `texture_format_known`, `texture_block_extent`, `texture_block_bytes`, `texture_level_bytes`,
  `texture_full_level_count`, `texture_level_extent`, `TextureSection`, `texture_section_name`,
  `TextureFileHeader`, `TextureFileSection`, `TextureFileDesc`, `TextureFileLevel`, `TextureData`
  (`level_bytes`), `validate_texture`, `write_texture_file`, `read_texture_file`,
  `read_texture_file_memory`, `read_texture_file_identity`, `read_texture_file_table`,
  `probe_texture_file` (the identity and the size from the header, by range: what a load trusts a
  built texture by), `texture_file_hash`, `is_texture_file`.
- `domain/texture/texture_build.h` — `k_texture_cache_version`, `FormatChoice`,
  `EdgeMode`, `TextureBuildOptions` (with `edge_x`, `edge_y` and `needed_side`),
  `k_max_needed_side`, `texture_levels_dropped`, `format_choice_name`,
  `parse_format_choice`, `edge_mode_name`, `parse_edge_mode`, `pack_texture_options`,
  `unpack_texture_options`, `resolve_texture_format`, `texture_cache_key`, `texture_cache_path`,
  `TextureBuildReport`, `build_texture`, `build_texture_from_encoded`, `k_alpha_dilation_texels`,
  `alpha_coverage`, `dilate_under_mask`, `preserve_alpha_coverage`, and the pieces:
  `srgb8_to_linear`, `linear_to_srgb8`, `srgb_decode_table`, `srgb_threshold_table`, `MipMode`,
  `mip_kernel`, `MipLevel`, `build_mip_chain`, `encode_level`, `decode_level`.
- `domain/texture/material_textures.h` — `k_role_*`, `image_roles`, `options_for_roles`, `image_edges`,
  `alpha_cutoff_code`,
  `fill_cluster_texture_records`, `cluster_texture_records`.
- `domain/texture/texel_density.h` — `k_density_surface_share`, `ImageDensity`,
  `measure_image_density`, `measure_cluster_density`, `needed_side` ("Sized by the asset").

## Depends on

`base`, `containers`, `math`, `hash`, `time`, `jobs`, `io`, `image` (the decode), `geometry` (the
container record the glue fills, and the levels the density is measured over); `bc7enc` (the
vendored encoders, MIT or public domain).

## Testing

`tools/dev.ps1 test -Filter texture`:

- `texture_file_tests.cpp` — a 37x21 BC7 chain (odd sides: partial blocks at every level) written,
  read back field by field, read from memory, and its identity read alone, and probed — the identity
  and the size from the header, a file one byte short or long or not a texture refused, a flipped
  payload byte left to the full read; every payload and level
  aligned; an inserted section of an unknown kind skipped; a texture with no identity reading zero;
  the writer refusing a wrong extent, a wrong byte count, BC5 claiming sRGB and a chain longer than
  the image allows; and a truncated file, a wrong magic, a wrong version, a section past the end, a
  flipped byte, a required section renamed away and a format from the future each refused with a
  sentence of its own. **The tail from a level** (R172): from every level of the colour chain and
  of a normal map with its variance chain, `read_texture_file_levels` reads by range exactly what
  `texture_levels_from` cuts from the whole read, block for block, and a level past the chain is
  refused by both.
- **The bound on a device** (`systems/renderer/tests/texture_tests.cpp`, through `test_device.h`;
  R172 stage 1, 2026-11-17, RTX 5090, `msvc-debug`): the quad's 1,024-texel map at 1.5, 5.8, 11
  and 30 m in a scene loaded whole and one the bound holds, ids, depth and colour **identical** at
  each; the load holds 24,064 bytes (the floor) where the whole chain is 1,400,320, and after the
  first frame's settle 1,400,320 at 1.5 and 5.8 m, 351,744 at 11 m, the floor alone at 30 m (settle
  24.5, 18.9, 5.0 and 0 ms in `msvc-debug`); a walk of 48 frames from 12 m to 1.2 m, 0.225 m a
  frame, identical frame for frame to the whole scene's: the first frame's settle brings level 1,
  the lead asks for level 0 off the frame long before frame 36, where a pixel first reads below the
  floor, and it lands with nothing late (2 promotions, 5 evaluations, the admission 0.37 ms). The feedback's own test passes with the bound on (the
  records' serial counts promotions, so the feedback follows a texture to its new pair).
- `mips_tests.cpp` — chain sides and counts at seven sizes; the sRGB tables within an ulp of the
  transfer function and every code round-tripping; a black-and-white checker averaging to sRGB 188
  (not 128) and to data 128; constant images constant at every level in every mode; normal maps
  unit at every level with a short source vector put back; an odd side filtered; repeating edges
  commuting with a cyclic shift (a 16x8 image moved two texels has level 1 moved one, byte for byte,
  in sRGB and linear, where clamped edges do not) and mirrored edges equal to repeating the image
  beside its reflection; the kernel's values and partition of unity; and the same bytes on a
  four-worker pool.
- `blocks_tests.cpp` — reference BC4 and BC1 decoders written from the format's definition: BC4
  exact on every constant block and on gradients whose values lie on a block's palette, BC5 as two
  independent BC4 channels (and the library's decoder agreeing, with blue zero), BC1 exact on a
  5:6:5 colour and BC3's alpha exact on its palette; BC7 held to a bound on each region of a mixed
  image (gradient above 45 dB and no channel more than 6 codes off, hard-edged tiles above 48 dB,
  per-texel alpha above 30 dB, white noise above 14 dB); and the same blocks on a pool.
- `material_textures_tests.cpp` — roles and options per slot, an ORM image serving two slots, the
  records keying an embedded image and leaving a file's key open (with repeating edges, the glTF
  default), an image's edges taken from the samplers that name it and clamped where two disagree,
  the options word round-tripping every format, flag and edge combination and refusing an unknown
  edge mode or bit, the `auto` rules and the sRGB refusal, and the cache key moving with every
  input.
- `determinism_tests.cpp` — above; eleven cases, two with repeating and mirrored edges and the last
  an alpha-tested mask at cutoff 128.
- `alpha_mask_tests.cpp` — a 256x256 mask of irregular needles one to three texels wide (14% at
  the cutoff) built as BC7 with and without its cutoff: decoded, every level of 8x8 or more holds level
  0's coverage within 2% with it and the smaller ones within two texels, while the same chain filtered
  as stored drifts by all 14 points (the needles vanish); the dilation leaves alpha and passing texels
  alone and gives a texel under the cut the leaf's green; the cutoff packs into bits 16..23 and none
  packs to the word it always did.
- `size_table.cpp` — the header, the section record, the description and the level record.

End to end: `apps/engine_content/tests/texture_tests.cpp` ([apps](apps.md)) and
`systems/renderer/tests/texture_tests.cpp` ([renderer](renderer.md)) — the latter includes the wrap
modes: a glTF quad with UVs from 0 to 3 and a 2x2 texture of four colours through a nearest sampler
shows nine whole tiles under repeat, every other tile reflected under mirror, and one tile then the
edge texel under clamp, cell by cell, from the decoded image and from the built texture; and UVs
from 0 to 1 with a `KHR_texture_transform` scale of 3 show the same nine tiles. The emissive and
occlusion slots and the transform's rotation are held to the CPU reference in
`domain/gfx/tests/attributes_tests.cpp` ([gfx](gfx.md)).

## Not yet

- **Cube maps, arrays, volume textures and HDR** (BC6H from EXR sources, which 07 §7.2 names): the
  format has room — a new format value and `desc` words — and nothing consumes them yet.
- **A signed-distance mask** (Green 2007), which would keep a leaf's edge sharp under
  magnification; "Alpha-tested masks" says why coverage scaling came first.
- **Streaming.** Textures are uploaded whole, all levels, at load; the chain is laid out so a
  streamer can fetch the small levels first, but no residency manager asks. Residency by reference
  for a tile's meshes is built ("Textures that arrive after the load"); residency by level is
  measured and designed, not built ("Levels resident by what the frame samples").
- **A UV transform per slot on the GPU.** The container keeps one per slot and the material table
  one per material ("Samplers and texture transforms"); a material whose slots disagree draws with
  the base colour's and is counted.
- **`KHR_materials_emissive_strength`**, which scales the emissive factor past 1 for HDR emitters:
  not read, so such an emitter draws at its factor.
- **Emissive surfaces as lights.** Radiance only in both integrators ("Emissive"); next-event
  estimation towards emissive triangles belongs with area lights.
- **A KTX2 exporter** ([ADR-0036](../adr/0036-built-textures-in-the-engines-own-container.md)).
- **`domain/atlas`'s rebake** still uses `std::pow` for its sRGB tables, which is one reason a
  repacked container is not yet the same bytes everywhere ([atlas](atlas.md#determinism)); these
  tables are the fix waiting for it.

A cached mesh may carry embedded source images and texture keys from an older
builder. Scene cache preparation derives the current key from each embedded source
identity and options, just as it does for path images, and updates the loaded mesh's
records in memory. It does not rewrite the source container. Without this step,
a texture cache version bump would never reach cached terrain normal maps. Direct
reads of old `.tex` files, and scenes loaded without cache preparation, retain the
optional-section fallback.
