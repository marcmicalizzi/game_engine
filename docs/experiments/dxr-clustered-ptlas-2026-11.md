# DXR's clustered geometry, partitioned TLAS and indirect builds, against this chain (R146, 2026-11-11)

Roadmap R146. On 2026-10-28 Microsoft published the DirectX 12 Agility SDK 1.722 preview with three
additions to DirectX Raytracing — clustered geometry, a partitioned top-level acceleration structure
(PTLAS) and indirect (GPU-driven) acceleration structure operations — and the press called it the
largest DXR change since 2018. The owner asked what it means for an engine that is Vulkan only and
already traces its cluster cut. This page is a reading, not a measurement: **nothing was built or
run for it** but `vulkaninfo`, and every figure about this engine is quoted from a page that
measured it. Estimates are marked as estimates.

**The answer.** The preview is D3D12 catching up with the two NVIDIA Vulkan extensions this chain
was designed around, and bringing AMD and Intel drivers with it — on D3D12 only. Of its three
features the chain has had two in effect since ADR-0025 and R94/R101 (cluster structures and
templates; builds whose records and counts never leave the device). **The one it lacks is the
partitioned top level**, and the extension for it is on this machine's driver today. On a world of
250,000 instance slots it is worth, as an estimate, between a quarter of a millisecond gained and
half a millisecond lost, and no bytes: the top-level build it replaces is 0.3 to 0.5 ms of a 3.5 ms
chain, and the 2.9 GB are cluster structures reserved at the driver's most, which no part of the
preview touches. So: nothing to adopt now, one small measurement worth having before the slots
grow, and one thing to watch — a cross-vendor cluster extension for Vulkan, which is what would
change the AMD and Intel answer and which was not found in the registry.

## What the preview adds

All three arrive through one pair of calls: `GetRTASOperationPrebuildInfo` sizes a batch and
`ExecuteIndirectRTASOperations` runs it, with the arguments and (optionally) their count read from
GPU memory. They are experimental features: preview headers and runtime, a preview driver,
Developer Mode, `D3D12EnableExperimentalFeatures` before the device, the cap
`ClustersAndPTLASSupported`, and Shader Model 6.10, itself in preview until the Agility SDK 1.621
retail release the announcement expects in early December. The functional specification (DXR part
2) is v0.38 of 2026-09-08 and says the features are in preview; it gives no retail date.

| feature | what it is | shape |
|---|---|---|
| clustered geometry | a cluster-level structure (CLAS) is one indexed mesh of at most 256 triangles and 256 vertices; a cluster BLAS is built from a list of CLAS addresses, so a CLAS is built once and referenced by any number of bottom-level structures; **templates** hold the topology-only part of a build and are instantiated with new positions, which is the answer to deformation, since clustered geometry has no refit by design | operation types `BUILD_CLAS_FROM_TRIANGLES`, `BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES`, `INSTANTIATE_CLUSTER_TEMPLATES`, `BUILD_BLAS_FROM_CLAS`, `MOVE_CLUSTER_OBJECTS`; modes implicit destinations, explicit destinations, get sizes; a 32-bit `ClusterID()` in hit shaders and `CandidateClusterId`/`CommittedClusterId` on a ray query built with `RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY`; float and normalized-integer positions and a fixed-point `COMPRESSED1` |
| partitioned TLAS | instances are assigned to persistent partitions; a build rebuilds only the partitions holding an instance that was written, and joins the partitions into one top level; a **global partition** holds what moves every frame, each instance as if in a partition of its own; a **partition translation** moves every instance of a partition without rewriting it, for large worlds | one operation type, `PARTITIONED_TLAS`, fed three kinds of argument: write instance (the whole record, with its partition and an optional explicit box), update instance (the bottom-level address only, no partition rebuild), translate partition; flags fast trace, fast build, enable partition translation; the specification suggests 100 to 1,000 instances a partition and says overlapping partitions cost trace time |
| indirect operations | the arguments of all of the above, and their counts, live in device memory; operations in one call may run in any order, so dependent stages (clusters, then cluster BLAS, then the top level) are separate calls with barriers between | the classic BLAS and TLAS builds stay CPU-driven; making them indirect is listed under possible future features, beside multi-level instancing |

**What it requires.** No new hardware beyond existing ray tracing, by Microsoft's statement; a
driver. As listed on 2026-10-28: NVIDIA, every RTX card (NVIDIA's whitepaper names Game Ready
621.01); AMD, the Radeon RX 9000 series on a developer preview driver (26.20.12.01); Intel, Arc
B-series and Core Ultra 200V and Series 3 on a beta driver; Qualcomm planned for the first half of
2027; WARP in preview; older GPUs to be determined.

**What Microsoft's samples measured, and against what.**

| sample | baseline | result | what it is not |
|---|---|---|---|
| `D3D12RaytracingClusteredGeometry`, Radeon RX 9070, one configuration | a "traditional" path that gives every animated copy its own bottom-level structure and rebuilds it from triangles each frame; the sample's README says this stresses independent construction and is "not optimized instancing", and that a real application would share one structure among identically deformed instances | 1.2 → 79 frames a second (the 66×); structure preparation 788 → 3.11 ms a frame; tracing 72.6 → 9.27 ms; structures 7,519 → 774 MiB; **scratch up**, 1.83 → 526 MiB static and 8.25 → 179 MiB animated | a game's gain: the post says so itself, in capitals. This engine has been on the right-hand side of that comparison since ADR-0025, whose own measure of clusters against one KHR geometry a cluster on the RTX 5090 was 2.9 to 3.3 times less memory and 1.2 to 2.6 times faster builds |
| `D3D12RaytracingPartitionedTLAS` (NVIDIA's `vk_partitioned_tlas` ported), GPU not stated | a full top-level rebuild a frame over 169,231 dominoes and 1,220,247 static instances on a 50×50 grid of partitions | 2.88 → 0.79 ms with 4,702 localized writes; tracing somewhat slower, by an amount the post does not give | a floor to scale from: 0.79 ms is what a few thousand writes cost there, and it is more than this engine's whole top-level build today |

The full rebuild in that sample is 2.1 ns an instance (2.88 ms over 1,389,478). This engine's is the
same order: `tlas` was 0.31 ms at the median and 0.42 at p95 on the owner's v34 walk at 3840×2160,
0.32 to 0.48 in the six cells that page flew ([the frame's cost map](frame-cost-map-2026-11-07.md)),
over a table that the `blas` zone of the time (2.2 ms at about 10 ns a slot) puts near 220,000
slots — an inference, the page does not give the count: 1.4 to 2.2 ns a slot.

## What Vulkan has

| feature | Vulkan today | status |
|---|---|---|
| clustered geometry | `VK_NV_cluster_acceleration_structure` (extension 570, revision 4 of 2025-07-16, which added opacity micromaps in clusters): the same operations, modes and templates, counts read from a device address | NVIDIA only, not ratified. The preview's D3D12 operations and modes mirror it one for one |
| partitioned TLAS | `VK_NV_partitioned_acceleration_structure` (extension 571, revision 1, 2025-01-09): `vkCmdBuildPartitionedAccelerationStructuresNV` with write instance, update instance and write partition translation, the global partition index, `enablePartitionTranslation`, a `maxPartitionCount` property, and **its own descriptor type** (`VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV`); shaders name it as they name a top-level structure | NVIDIA only, not ratified; no serialization |
| indirect builds | built into both NV extensions. The KHR indirect build (`accelerationStructureIndirectBuild`) is **false** on this driver, so a classic TLAS or BLAS build takes its count from the host | — |
| AMD | `VK_AMDX_dense_geometry_format` (provisional, Vulkan 1.4.324, August 2025): pre-compressed geometry handed to an ordinary BLAS build. It is a storage format, not a cluster structure: the page read says nothing of building a block once and referencing it from many structures, nor of GPU-driven builds. A multivendor version with Samsung was reported in May 2026 (a secondary report, not read further) | AMD only |
| Intel | nothing found | — |
| cross-vendor (KHR or EXT) | **nothing found in the registry**: the specification's change log, read from 1.4.368 (2026-11-06) back to mid-2025, adds no extension named for clusters or partitions under any prefix but NV, and Khronos's Vulkan news for September to November 2026 carries no statement about either. **One loose end**: NVIDIA's Linux driver 615.71.09 (2026-09-09) was announced, on Khronos's own news page among others, as adding `VK_EXT_cluster_acceleration_structure`. No such extension has a reference page (the URL is a 404), the change log does not name it, and this machine's newer Windows driver does not expose it. It is either a slip for the NV name or a driver ahead of a specification that is not public; nobody here has seen that driver's extension list | — |

**This machine** (`vulkaninfo` 1.4.341, RTX 5090, driver 617.42, device API 1.4.351) exposes
`VK_NV_cluster_acceleration_structure` revision 4, **`VK_NV_partitioned_acceleration_structure`
revision 1**, `VK_KHR_acceleration_structure` 13, `VK_KHR_ray_query`, `VK_KHR_opacity_micromap` 1
and `VK_EXT_opacity_micromap` 2; no KHR, EXT, AMD or Intel extension named for clusters, partitions
or dense geometry. That `vulkaninfo` prints no property block for either NV extension, so
`maxPartitionCount` on this card is not known from it.

## Where this engine stands

| preview feature | here | how |
|---|---|---|
| CLAS built once, referenced by many bottom-level structures | **have** (NVIDIA) | the resident cluster structure cache (R101) and one structure a mesh for the rigid distance forms (R94), [renderer](../subsystems/renderer.md#the-resident-cluster-structure-cache) |
| templates instantiated from deformed positions | **have**, optional (`--rt-templates`), packed to their size | `instantiate_cluster_templates`, `clas_records.slang`'s instantiate mode |
| explicit destinations and a sizes pass | **have**: the forms and the templates are sized and then built packed, the cache's misses go to fixed slots; the frame's own set is implicit, reserved at 8,832 bytes a cluster with micromaps | [what a structure of the frame's own set reserves](rt-small-instances-2026-11-10.md#what-a-structure-of-the-frames-own-set-reserves) |
| moving cluster objects (compaction) | not used | `cluster_acceleration.h` has no move; it is the tool the cache's packing would want, and it is in the NV extension already |
| GPU-driven builds | **have, in effect** | the cull's visible list becomes records, counts and bottom-level records in compute passes; the cluster and cluster-BLAS builds read their counts from the device (since R143 the BLAS count too); the top-level records' transforms, masks and addresses are written by a dispatch (`tlas_references.slang`). The one host count is the classic TLAS build's, which is handed every slot, inactive ones null |
| **partitioned TLAS** | **lack** | `gfx::build_tlas` is a whole KHR build (`k_build_fast_trace`) over every instance slot every frame, after a dispatch that rewrites every slot's 64-byte record |

So the belief the brief came with is confirmed: the partitioned top level is the gap, and the only
one. Two smaller differences are not gaps in the chain: D3D12's cluster is 256 triangles and 256
vertices where this format's is 124 and 64 (a content-format choice, and the pair space's question,
not the API's), and its cluster id is 32 bits where ADR-0025 carries the cluster in the 24-bit
geometry index.

### What a partitioned top level would take off, at 250,000 slots (an estimate)

The chain it would join, on the v40 world at 3840×2160 after R143
([the measurement](rt-small-instances-2026-11-10.md#the-pairs-before-and-after)): `rt` 3.51 ms at
the median, of it `clas` 2.27 and `blas` 0.39, 2,856 MiB.

**Milliseconds.** What a PTLAS can remove is the `tlas` zone and most of the "tlas instances"
dispatch. The first is 0.3 to 0.5 ms at this size by the per-slot rate above (v40's own `tlas` zone
is in its frame records but on no page); the second writes 16 MB a frame and has no zone of its
own on any page, so it is guessed here at 0.1 ms. **The ceiling is about 0.4 to 0.6 ms, 11 to 17%
of the chain.** What it would add every frame, from how this renderer works:

- **A translation for every partition, every frame the eye moves.** The frame's origin is the eye
  ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)), so every instance's
  translation changes every frame, which is exactly what a partitioned structure is built not to
  be told. Partition translation is the feature for it: a partition a 64 m world cell, the stored
  record the instance's cell-local 3x4 (what `InstanceDesc` already holds), the translation the
  cell from the eye. v40's 219,166 slots over the 1,300 to 1,700 tiles a ring of 20 tiles holds
  are 130 to 170 a partition, inside the specification's 100 to 1,000. The DXR specification
  calls a translation a relatively light update and gives no number; the top level over the
  partitions is presumably rejoined each time.
- **An address update for every instance the frame built**: the one bottom-level build has implicit
  destinations, so the 13,000 to 24,000 structures a frame builds (`blas_built`) change address
  every frame. Those instances would sit in the global partition or take an update each, and an
  update without an explicit box refits its partition.
- **Writes for what streamed in or out**, which is the case the samples measure.

Microsoft's sample paid 0.79 ms for 4,702 writes. Nothing read says how that scales, so the build
side here lands, as an estimate, between 0.15 and 0.8 ms against today's 0.3 to 0.5: **from 0.3 ms
better to 0.4 ms worse**. And the trace side counts against it: the post says a partitioned
structure traces somewhat slower, and this renderer's primary rays alone are 13.8 ms at the median
at 3840×2160 with the sun's rays inside an 18.7 ms resolve
([the cost map](frame-cost-map-2026-11-07.md)), so each percent of traversal is 0.14 ms or more and
three percent is the whole ceiling. **At 250,000 slots the expectation is about nothing, more
likely a small loss.** The rate says where that changes: a full rebuild at 1.4 to 2.2 ns a slot is
1 ms near 500,000 to 700,000 slots and 3 to 4 ms at two million, and the largest table went from
47,849 slots (the endless desert, 2026-10-04) to 219,166 (v40, 2026-11-10).

**Bytes.** None, and probably a few tens of megabytes more. The top level and its records are a
few hundred bytes an instance — 16 MB of records and, as an estimate, 50 to 75 MB of structure at
250,000 slots, under 3% of the chain — and a partitioned one keeps all of it and adds the
partitions' own structures and, with translation enabled, more (both specifications say so without
a figure). The 2.9 GB are elsewhere: the frame's own cluster set reserved at the driver's most
(1,024 MiB at that run's budget), the cache's slots at the same most, the micromaps, the forms.
D3D12's sizing query is conservative in the same way, so the preview changes none of it; what
would is R143's list (the cache's slots and the frame's set packed to what they hold), which needs
only what the NV extension already has: a sizes mode, explicit destinations, moving objects.

**Two costs of adopting it that are not milliseconds.** The traced cut is held to the rasterized
one word for word, and that rests on `instance_from_eye` being the rasterizers' arithmetic
operation for operation; with a partition translation the driver does the last addition, in an
order of its own, so the match has to be re-proved or the tolerance restated. And the structure is
a different descriptor type, so the bindless set's binding 3 (`g_scenes[]`) needs a second binding
or a second layout. Whether a ray query (as against a pipeline) may trace one was not confirmed in
what was read.

## The non-NVIDIA and baseline paths

**Today.** ADR-0025's fallback — a KHR bottom-level structure with one geometry a visible cluster —
is built and traced in `domain/gfx`'s tests and is not wired into the renderer
([renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory), "The KHR fallback has nothing
per frame to size"). Everything measured on this page's chain is the NVIDIA path. The two baseline
test machines (a Maxwell Titan X and a Pascal Titan Xp) have no ray queries by the project's pages
and draw shadows from the cascaded maps; nothing here concerns them, and Microsoft lists older
GPUs as undetermined.

**What the preview changes for AMD and Intel**: on Vulkan, nothing yet. It is evidence that both
vendors now have a driver implementation of cluster structures, templates and a partitioned top
level, behind D3D12, on their current cards — which removes the usual reason a cross-vendor Vulkan
extension does not exist — but no such extension was found, and this engine is Vulkan only by
decision. **What a cross-vendor cluster extension would change**: the fallback stops being a
second design. The cache, the static forms, the templates and the per-frame records would run on
an RX 9000 or an Arc B-series as they do on RTX, instead of a per-frame KHR build that ADR-0025
expects to lag the raster cut and need bias. How much faster or smaller than the KHR path on those
cards is not known: the only AMD numbers are the sample's, against a baseline built to lose, and
the project owns no AMD or Intel card with ray tracing. ADR-0025's "revisit when" names this
event; its first half (a DXR draft) has now happened and decides nothing for Vulkan.

## Recommendation, in rows

1. **Now — measure, do not adopt.** *PTLAS against the per-frame TLAS, in `domain/gfx`'s tests
   only* (an experiment the size of E2): the same instance table built both ways on the RTX 5090 at
   250,000, 1 million and 4 million slots, a partition a 64 m cell with partition translation, the
   frame's own instances in the global partition; build ms, trace ms on the ray visibility test,
   bytes, `maxPartitionCount`, whether ray queries trace it, and whether the ids still match the
   rasterizers' word for word. Small, and it turns this page's estimate into a number before the
   slot count makes the question urgent.
2. **Now — put v40's `tlas` and "tlas instances" on a page**: the zones exist in the frame record
   (R82 asks for the same export); a walk's median and p99 at 1920×1080 and 3840×2160 say what the
   ceiling really is. Extra-small; it can ride with row 1.
3. **Adopt PTLAS when** row 1 shows a net gain with the trace cost counted, or `tlas` passes about
   1 ms a frame on a shipping world (near half a million slots by today's rate): partitions by
   cell, translation by the eye, a second bindless binding. Medium, NVIDIA only until row 4.
4. **When a cross-vendor Vulkan cluster extension ships**: put it behind
   `gfx/cluster_acceleration.h` beside the NV backend, run E2 again on an AMD or Intel card
   (borrowed, or by `tools/package-tests.ps1`), and supersede ADR-0025 with the result; the KHR
   fallback then needs wiring only for cards older than the extension.
5. **Watch**: the Vulkan registry and change log for a cluster or partitioned extension under EXT
   or KHR, and what NVIDIA 615.71.09's "EXT" was (one `vulkaninfo` on a Linux machine with that
   driver settles it); DXR part 2 leaving preview; the multivendor Dense Geometry Format; and
   NVIDIA's `dx12_forest_ray_tracing` sample when it is published, the nearest public thing to
   this engine's forest (60 million plants on 25 km², partitions re-cut around the camera).
6. **Unchanged by any of this**: the chain's bytes are the reservation's, and the rows R143 left
   (the cache's slots at their size, the frame's set packed or at a lower most) are still the
   memory work.

## Sources

Every web page was read through a fetch tool that returns an extraction of the page, not its
text, so API names and numbers are as that tool reported them and wording is paraphrased.

- Microsoft, DirectX Developer Blog, "[PREVIEW] Raytracing: Clustered Geometry, Partitioned TLAS,
  Indirect Acceleration Structure Builds" (2026-10-28), whole page: the features, the requirements,
  both samples' numbers. "Announcing Agility SDK 1.620 Retail and 1.722 Preview" (same day), whole
  page: the drivers, Shader Model 6.10's timing. Neither calls it the largest change since 2018;
  that is the press's phrase (search results only).
- microsoft/DirectX-Specs, `d3d/Raytracing2.md`, v0.38: all three windows of the file through the
  tool (282,219 characters), asked for the operations, limits, PTLAS arguments and flags, HLSL,
  future features and the change log.
- microsoft/DirectX-Graphics-Samples, the READMEs of `D3D12RaytracingClusteredGeometry` (what the
  traditional baseline is) and `D3D12RaytracingPartitionedTLAS` (the scene's counts); the linked
  benchmark and walkthrough documents were not read.
- NVIDIA-RTX/Whitepapers, `dx_clusters_and_ptlas/README.md`, whole: the driver number, the forest
  sample's description; it has no measurements.
- Khronos: the reference page and the proposal of `VK_NV_partitioned_acceleration_structure`, the
  reference page and the proposal of `VK_NV_cluster_acceleration_structure` (each whole);
  `Vulkan-Docs/ChangeLog.adoc`, its first 100,000 characters (1.4.368 back to June 2025); the
  Vulkan news tag page for September to November 2026 and the 615.71.09 item; the extensions
  appendix, first 100,000 characters only, which settled nothing.
- nvpro-samples/vk_partitioned_tlas, README: the update modes, the indirect build.
- AMD GPUOpen, "AMD releases Vulkan support for Dense Geometry Format" (2025-08-05), whole. The
  Samsung partnership: a search result's summary only.
- `vulkaninfo` on this machine, 2026-11-11 (text output, kept in the agent's scratch, not
  committed).
- From memory, not read for this page and not relied on: that the NV cluster extension has its own
  32-bit cluster id built-in and a position truncation setting.

**Not found**: a Khronos statement on cross-vendor cluster or partitioned structures; any Intel
Vulkan extension for either; a number for PTLAS's trace cost, for a partition translation's cost,
or for `maxPartitionCount`; the GPU of the domino sample's 2.88 and 0.79 ms.
