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
| 1 | `desc` | `TextureFileDesc`, 32 bytes, one | format, colour space, level-0 width and height, level count, flags (`k_texture_normal_map`, `k_texture_has_alpha`), the source's channel count |
| 2 | `levels` | `TextureFileLevel`, 24 bytes, one per level | width, height, and the offset and length of the level's blocks in `data`; level 0 first |
| 3 | `data` | `u8` | every level's blocks back to back, each level at a 16-byte aligned offset, the padding zero |
| 4 | `source_hash` | `u64`, two | the source image's content hash (`hash_bytes` over its encoded bytes as read) and the build key over it and the options |

A level is whole blocks even when its sides are not multiples of four (a 2x1 level of BC7 is one
16-byte block), which is what Vulkan's copy expects. The reader refuses, each with its own
sentence and an empty result: a truncated file, the wrong magic or version, a section past the end,
a content hash that does not match, a missing required section, a format this build does not know,
and a chain that does not validate — a level whose extent is not `max(1, side >> level)`, whose
byte count is not `texture_level_bytes`, whose offset is unaligned or outside the payload, or a BC4
or BC5 texture claiming sRGB (neither has an sRGB form in any API). `write_texture_file` refuses
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
so an odd side is filtered rather than a column dropped. The edges clamp, which is the renderer's
sampler's address mode. Three modes:

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

## Block compression

`encode_level` encodes one level with the vendored encoders (`cmake/EngineTexture.cmake`,
[third-party licences](../../third_party/LICENSES.md)): **BC7** through Rich Geldreich's `bc7enc`
(modes 1, 5, 6 and 7, uber level 1, perceptual YCbCr weights for sRGB colour and plain RGB weights
for data), **BC1, BC3, BC4, BC5** through his `rgbcx` (quality level 10 for BC1 and BC3, the
high-quality searches for BC4 and BC5). A partial block at a level's edge repeats the edge texels.
The encoders are initialized once per process (their tables are globals). The block rows of a level
are spread over the job system's performance pool, one row a job at most, and every block is
written by exactly one of them. `decode_level` goes the other way (`rgbcx`'s decoders and
`bc7decomp`) for the tool's error figure and the tests.

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
`build-all` would skip it forever.

## In the renderer

`systems/renderer`'s texture upload (`gpu_scene.cpp`, `find_built_textures`) takes a mesh's built
textures when the load reads the cache (`--no-cache` reads none) and **every** image the resolve
samples — base colour, metallic-roughness, normal — has one whose recorded identity matches; it
uploads them as stored with `gfx::upload_image_2d_levels`, samples them through
`gfx::create_mip_sampler` (trilinear, 16x anisotropic where the device has `samplerAnisotropy`,
clamped like the old sampler), and sets `k_material_mipped` on the mesh's materials (and
`k_material_normal_rg` where the normal map is BC5). Otherwise the mesh takes exactly the path it
always took — decode, RGBA8 at one level, the linear sampler, level-0 reads — and the flags stay
zero, which keeps that picture byte for byte what it was. It is all or nothing per mesh, so no
material mixes the two samplers. A device without `textureCompressionBC` draws the decoded path
with a warning naming the feature (every desktop GPU has it; `engine-cli gpu.adapters` lists the
row). `engine-view`'s summary says what happened: `"textures":{"built":n,"decoded":m,"bytes":b}`,
the bytes being what the allocator gave the images.

### Mip selection without quads

A visibility-buffer resolve is a fullscreen pass, so the 2x2 quad a derivative instruction
differences holds pixels of **other** triangles wherever an edge crosses it — other meshes, other UV
islands — and `ddx(uv)` there is garbage. The resolve instead computes the UV's derivatives from the
hit triangle itself (`screen_uv_gradients`, `material.slang`): screen-space barycentrics are affine
in the pixel, `l_i = edge(s_j, s_k, p) / area`, so their derivatives are the constant
`dl_i/dx = -(s_k.y - s_j.y) / area` and `dl_i/dy = (s_k.x - s_j.x) / area`; the perspective-correct
ones are `b_i = q_i / Q` with `q_i = l_i / w_i`, so `db_i = (dq_i - b_i dQ) / Q`; and
`duv = Σ db_i uv_i`. That is one more pass over three vertices the resolve has just read, only for a
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

## Public API

- `domain/texture/texture_file.h` — `TextureFormat`, `ColorSpace`, `k_texture_normal_map`,
  `k_texture_has_alpha`, `texture_format_name`, `color_space_name`, `parse_texture_format`,
  `texture_format_known`, `texture_block_extent`, `texture_block_bytes`, `texture_level_bytes`,
  `texture_full_level_count`, `texture_level_extent`, `TextureSection`, `texture_section_name`,
  `TextureFileHeader`, `TextureFileSection`, `TextureFileDesc`, `TextureFileLevel`, `TextureData`
  (`level_bytes`), `validate_texture`, `write_texture_file`, `read_texture_file`,
  `read_texture_file_memory`, `read_texture_file_identity`, `read_texture_file_table`,
  `texture_file_hash`, `is_texture_file`.
- `domain/texture/texture_build.h` — `k_texture_cache_version`, `FormatChoice`,
  `TextureBuildOptions`, `format_choice_name`, `parse_format_choice`, `pack_texture_options`,
  `unpack_texture_options`, `resolve_texture_format`, `texture_cache_key`, `texture_cache_path`,
  `TextureBuildReport`, `build_texture`, `build_texture_from_encoded`, and the pieces:
  `srgb8_to_linear`, `linear_to_srgb8`, `srgb_decode_table`, `srgb_threshold_table`, `MipMode`,
  `mip_kernel`, `MipLevel`, `build_mip_chain`, `encode_level`, `decode_level`.
- `domain/texture/material_textures.h` — `k_role_*`, `image_roles`, `options_for_roles`,
  `fill_cluster_texture_records`, `cluster_texture_records`.

## Depends on

`base`, `containers`, `hash`, `time`, `jobs`, `io`, `image` (the decode), `geometry` (the container
record the glue fills); `bc7enc` (the vendored encoders, MIT or public domain).

## Testing

`tools/dev.ps1 test -Filter texture`:

- `texture_file_tests.cpp` — a 37x21 BC7 chain (odd sides: partial blocks at every level) written,
  read back field by field, read from memory, and its identity read alone; every payload and level
  aligned; an inserted section of an unknown kind skipped; a texture with no identity reading zero;
  the writer refusing a wrong extent, a wrong byte count, BC5 claiming sRGB and a chain longer than
  the image allows; and a truncated file, a wrong magic, a wrong version, a section past the end, a
  flipped byte, a required section renamed away and a format from the future each refused with a
  sentence of its own.
- `mips_tests.cpp` — chain sides and counts at seven sizes; the sRGB tables within an ulp of the
  transfer function and every code round-tripping; a black-and-white checker averaging to sRGB 188
  (not 128) and to data 128; constant images constant at every level in every mode; normal maps
  unit at every level with a short source vector put back; an odd side filtered; the kernel's values
  and partition of unity; and the same bytes on a four-worker pool.
- `blocks_tests.cpp` — reference BC4 and BC1 decoders written from the format's definition: BC4
  exact on every constant block and on gradients whose values lie on a block's palette, BC5 as two
  independent BC4 channels (and the library's decoder agreeing, with blue zero), BC1 exact on a
  5:6:5 colour and BC3's alpha exact on its palette; BC7 held to a bound on each region of a mixed
  image (gradient above 45 dB and no channel more than 6 codes off, hard-edged tiles above 48 dB,
  per-texel alpha above 30 dB, white noise above 14 dB); and the same blocks on a pool.
- `material_textures_tests.cpp` — roles and options per slot, an ORM image serving two slots, the
  records keying an embedded image and leaving a file's key open, the options word round-tripping
  and refusing unknown bits, the `auto` rules and the sRGB refusal, and the cache key moving with
  every input.
- `determinism_tests.cpp` — above.
- `size_table.cpp` — the header, the section record, the description and the level record.

End to end: `apps/engine_content/tests/texture_tests.cpp` ([apps](apps.md)) and
`systems/renderer/tests/texture_tests.cpp` ([renderer](renderer.md)).

## Not yet

- **Cube maps, arrays, volume textures and HDR** (BC6H from EXR sources, which 07 §7.2 names): the
  format has room — a new format value and `desc` words — and nothing consumes them yet.
- **Alpha coverage preservation.** A masked material's mips average alpha, so a cutout thins with
  distance; the fix (scaling each level's alpha to keep the fraction above the cutoff) needs the
  cutoff, which is the material's and not the image's.
- **Streaming.** Textures are uploaded whole, all levels, at load; the chain is laid out so a
  streamer can fetch the small levels first, but no residency manager asks.
- **The occlusion and emissive textures** are built but not sampled: the resolve has no slot for
  them yet ([renderer](renderer.md)).
- **glTF sampler wrap modes.** The renderer's samplers clamp, as the decoded path always did; the
  mip filter clamps to match. A tiling texture wants repeat in both.
- **A KTX2 exporter** ([ADR-0036](../adr/0036-built-textures-in-the-engines-own-container.md)).
- **`domain/atlas`'s rebake** still uses `std::pow` for its sRGB tables, which is one reason a
  repacked container is not yet the same bytes everywhere ([atlas](atlas.md#determinism)); these
  tables are the fix waiting for it.
