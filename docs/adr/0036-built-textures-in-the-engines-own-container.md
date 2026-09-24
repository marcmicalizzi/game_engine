# ADR-0036: Built textures go in the engine's own sectioned container, not KTX2

- **Status:** Accepted
- **Date:** 2026-09-24
- **Plan references:** docs/plan/07-content-pipeline.md §7.2 (textures: "PNG/EXR source → KTX2 (BC7/BC5/BC4) derived") and §7.3 (every transform a derived node, addressed by its inputs, the same bytes from every machine). Builds on [ADR-0014](0014-apache-2-license-and-dependency-policy.md) (permissive dependencies only) and [ADR-0035](0035-no-floating-point-contraction.md) (the content build's bytes are a function of its input).
- **Docs touched:** [texture](../subsystems/texture.md) (new), [geometry](../subsystems/geometry.md) (section kind 32), [gfx](../subsystems/gfx.md), [renderer](../subsystems/renderer.md), [apps](../subsystems/apps.md), [image](../subsystems/image.md), `docs/subsystems/README.md`, [07 §7.2/§7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph), `third_party/LICENSES.md`

## Context

Plan 07 §7.2 names the texture node's output as **KTX2** holding BC7, BC5 and BC4. When the node was
built (2026-09-24) the question was whether to write KTX2 or a file of the engine's own, and three
facts decided it.

1. **Every other derived artifact is a sectioned, append-only container.** The `.clusters` file
   (`geometry/cluster_file.h`) is a 32-byte header, a table of `{kind, element_size, element_count,
   offset}` records and 16-byte aligned payloads, with a content hash over everything after the
   header, kinds that are never renumbered, and readers that skip a kind they do not know. That is
   what lets a new build add data without breaking an old reader, lets `engine-content info` hash
   each section so two machines' outputs are compared section by section, and lets the
   determinism tests pin a table of section hashes. KTX2 is a different shape: a fixed header, a
   level index, a data-format descriptor (the Khronos DFD, itself a small language), a key/value
   block and optional supercompression. Adding data to it means key/value entries, and a KTX2
   reader has no notion of "a section this build does not know".
2. **The derived-data cache reads an identity out of every entry.** An incremental build decides
   "up to date" from the source hash and the build key the entry records, never from a timestamp
   (AGENTS.md). KTX2 has no field for that; it would be a key/value pair the engine defines and
   every other KTX2 tool ignores — which is to say, the engine's own format inside someone else's.
3. **Nothing reads KTX2 here.** The renderer uploads blocks through `gfx::upload_image_2d_levels`;
   no tool in the tree or in the owner's workflow consumes KTX2, and writing one well means
   either libktx (a large dependency with its own Basis Universal and zstd trees) or a DFD writer
   of our own, for an interchange nobody asked for.

The alternatives were **KTX2 through libktx** (rejected on 1–3 and on weight: libktx is permissive,
but it brings encoders and a transcoder the build does not use), **KTX2 written by hand** (the DFD
and the level index are small, but they are a second layout to keep right for no reader), and
**the engine's container**, chosen.

## Decision

1. A built texture is a **`.tex`** file (`domain/texture/texture_file.h`): the cluster container's
   layout with magic `TEXF`, and four append-only section kinds — 1 `desc` (format, colour space,
   size, level count, flags), 2 `levels` (width, height, offset and length of each level in the
   payload), 3 `data` (every level's blocks, each at a 16-byte aligned offset), 4 `source_hash`
   (the source image's content hash and the build key). A reader skips a kind it does not know;
   removing a required kind or changing an element's layout is a version bump.
2. It lives in the derived-data cache at `<ddc>/textures/<key>.tex`, where `key =
   texture_cache_key(hash of the source's encoded bytes, packed build options)` mixed with
   `k_texture_cache_version`. It is never committed.
3. It is **the same bytes from every toolchain and every thread count**, pinned by committed
   section hashes (`domain/texture/tests/determinism_tests.cpp`) that CI's GCC and Clang presets
   have to reproduce; the block encoders (`bc7enc`, `rgbcx`, vendored by commit, MIT or public
   domain) were chosen partly because they can be.
4. A `.clusters` container names each image's built texture in section kind 32, `textures`
   (`geometry::ClusterFileTexture`), so the renderer finds it from the container alone.
5. **A KTX2 exporter is a function of a `.tex`**, and is the way to answer a tool that wants KTX2
   when one appears: the formats are the Vulkan block formats either way, so it is a header and a
   level index written from the `desc` and `levels` sections, with no re-encoding.

## Consequences

- One container design, one set of checks and messages, one `info`, one determinism rule for every
  derived artifact the engine writes. The texture reader is the cluster reader's code in another
  shape, and the unknown-kind, truncation and hash tests are the same tests.
- The texture step's identity rule is the mesh step's: a texture is rebuilt when its source bytes
  or options change, and not otherwise.
- There is no interchange format for built textures until an exporter is written. Nothing needs
  one today; third-party viewers cannot open a `.tex` (the tool's `info` describes one, and the
  renderer's albedo view shows one).
- Plan 07 §7.2 is amended by a status note rather than by editing its table's intent: the node
  exists, its output is `.tex`, and the formats are the ones the table named plus BC1 and BC3 for
  whoever asks for them by flag.

## Revisit when

- A consumer outside the engine needs built textures (a DCC round trip, a web viewer, a platform
  whose SDK wants KTX2): write the exporter, keep the container.
- Supercompression (zstd or RDO-tuned BC over a general compressor) is measured to matter for
  package size or streaming bandwidth: it is a section kind in this container, not a reason to
  change containers.
- A platform needs a format KTX2 describes and the `desc` section cannot (ASTC, ETC2 for mobile):
  a new format value is the answer first.
