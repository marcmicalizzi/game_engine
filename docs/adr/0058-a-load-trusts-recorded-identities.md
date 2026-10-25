# ADR-0058: A load trusts the identities its derived data and a pack's manifest record

- **Status:** Proposed
- **Date:** 2026-10-24
- **Plan references:** docs/plan/07-content-pipeline.md §7.3; docs/plan/04-renderer.md §4.9
- **Docs touched:** [renderer](../subsystems/renderer.md#what-the-load-reads), [texture](../subsystems/texture.md#trusted-not-read), [content_build](../subsystems/content_build.md#one-hash-per-image-and-a-packs-from-its-manifest), [materials](../subsystems/materials.md#a-pack), [gfx](../subsystems/gfx.md#the-pipeline-cache)

## Context

The forest proof of concept's load read 4.4 GB before its first frame from 832 MB of derived data
([startup trace](../experiments/startup-trace-forest-2026-10-24.md)). 2.7 GB of it was the source
images, read and hashed once per mesh that named them to find the built texture their bytes address,
and again by the upload, although every texture was already built; and every `.tex` was read whole
twice, once to check its identity and once to upload it. The owner's rule for the loading pipeline
is that a cooked scene reads derived data only and that its load does not grow with the world.

A built texture is addressed by the hash of its image's encoded bytes. An image the container
carries is covered by the container's identity, so its key is recorded; an image named by path is
deliberately outside it (a container is keyed by its glTF or its recipe, so a texture can be
repainted without rebuilding the mesh), so nothing recorded its key and every reader hashed the
file. The alternatives: record a key for path-named images at build (stale under an unchanged
container key, and a container-format change with a rebuild of every cache); decide freshness from
a file's size and time (forbidden: nothing decides from a timestamp); or find something the load
already reads that pins the bytes.

## Decision

1. **A built texture is trusted from its header.** A load uses a `.tex` whose header, section table
   and identity section say it was built from this key's source hash and options and whose size is
   the size its header says (`texture::probe_texture_file`), without reading the payload or the
   source. The upload's full read still checks the content hash; a file that fails it draws through
   the decode. The cook (`engine-content build-all`) keeps the full check.
2. **A material pack's manifest pins its files.** `pack.json` names every map with its SHA-256 and a
   pack is fetched and verified as a unit, and `materials::Pack::hash` is over that text; so the
   `hash_bytes` of a file the pack names is derived data keyed by the pack's hash
   (`<ddc>/textures/packs/<pack hash>.json`), written the first time a load hashes the file and read
   instead of the file from then on. A pack is changed by repacking it, which moves its hash.
3. **Everything else is hashed once per load.** A file nothing the load reads covers — a glTF's
   sibling image, a recipe's own set — is read and hashed as it is now, once per distinct path per
   load (`content_build::ImageIdentities`), so a repainted one still finds its new texture or none.
4. **The driver's compiled pipelines are derived data too.** The device's `VkPipelineCache` lives in
   `<ddc>/pipeline-cache/`, used only when its header names this device, vendor and
   `pipelineCacheUUID`.

## Consequences

- A cooked scene whose images are embedded or a pack's opens no source image: the forest reads 0 of
  the 2.7 GB, and its load scene stage fell from 4.8 s to 0.4 s warm.
- A pack file edited in place without its manifest draws its old texture until a missing texture is
  built from it, where the mismatch is said and the bytes win. That is the price of decision 2, and
  it is the pack's contract rather than a new one: the manifest's hashes were already the pack's
  identity for its fetch.
- A `.tex` whose payload was damaged after it was written, with its header intact, is no longer
  rebuilt by the load; the upload refuses it and the mesh draws decoded, and the cook rebuilds it.
- No container format changed and no `k_cluster_cache_version` moved; existing caches keep working
  and gain the index on their first load.
- Forbidden: trusting a recorded identity for a file that nothing the load reads covers.

## Revisit when

A shipped build carries no sources at all (then decision 3 has nothing to hash, and the cook must
record every key it trusted); a pack format without per-file hashes appears; or a texture is found
drawn from a `.tex` that its header misdescribes.
