# 07 — Asset and Content Pipeline

## 7.1 Principles

- Source formats are open interchange formats. Engine-specific data is derived, cached, and content-addressed.
- Every asset carries provenance and license metadata; the build refuses assets without it.
- Validation is automatic and structured; agents receive machine-readable failures with rule IDs.
- Procedural generation handles volume; ML generation handles novelty; agents direct both and evaluate the results.

## 7.2 Source formats and canonical representations

| Type | Source | Notes |
|---|---|---|
| Meshes, skins, animations, PBR materials | glTF 2.0 with KHR extensions | Universal; every generator and DCC tool exports it; agents know it well |
| Textures | PNG/EXR source → KTX2 (BC7/BC5/BC4) derived | |
| Terrain | heightfield EXR + splat masks | Blob-referenced from the document |
| Splines, masks, placements | document objects | |
| Audio | WAV/FLAC source → Vorbis/Opus derived | |
| Structural graphs, fracture chunks | document objects + derived chunk meshes | |
| Interop | USD via LightUSD (read/write); FBX via an import-only converter | |

Canonical engine representations, all derived: cluster pages, AS build inputs, compressed textures, navmesh tiles, support graphs, deformable cages and their surface bindings, compressed animation clips, material shader permutations, semantic summaries, visual mips.

## 7.3 Content build (the derived-data graph)

- Every transform is a `derived` node ([03-data-model §3.6](03-data-model.md#36-dependencies-and-invalidation)): pure, sandboxed with tracked reads, versioned by function hash so a code change invalidates its outputs automatically.
- Parallel, out-of-process, remote-cache capable through the shared content-addressed store.
- Hot path: an editor edit triggers incremental rebuild of the affected nodes and hot-reload into running sessions per asset. Full rebuilds happen only when a function version changes.
- Build failures are structured diagnostics (asset ID, rule ID, severity, suggested fix) consumed identically by agents and the editor.

## 7.4 Validation rules (automatic)

- **Geometry**: manifoldness where required, degenerate triangles, scale (bounding box against the declared category's size range), pivot placement, up axis, UV coverage and overlap, texel-density range, cluster error budget, triangle budget per category, collision presence and fit (convex hull to mesh volume ratio), open edges.
- **Materials**: texture resolution against on-screen size class, missing maps, energy conservation, naming.
- **Rigs and animation**: standard-skeleton conformance, bone count, skin-weight normalization, foot-sliding metric, root-motion consistency.
- **Deformable volumes**: cage covers the render mesh, attachments resolve to existing bones or bodies, layers partition the volume, parameters in range ([§7.10](#710-deformable-volume-assets)).
- **Placement**: floating or interpenetrating objects, below terrain, unreachable by nav, occlusion of required landmarks, style-guide conformance.
- **Performance**: per-asset cost estimate (clusters at typical distance, AS memory, texture memory) against budgets by category.
- **Provenance**: metadata present and license in the allowlist.
- **Visual**: embedding distance to the approved reference board (soft warning), palette conformance.

## 7.5 Provenance and licensing metadata

```
AssetProvenance {
  generator, model_id, model_version, prompt_hash, seed
  inputs[]           content hashes
  operator           human or agent id
  created_at
  license            SPDX id or project-defined id
  license_terms_url
  commercial_ok, attribution_required
  derived_from[]     asset ids
}
```

Stored beside the asset in the document and carried through derived data. The build fails if `license` is not in the project allowlist. A shipping report lists every attribution requirement.

## 7.6 Procedural generation (volume content)

Engine-provided generators are `derived` nodes whose parameters are document objects, so agents tune parameters and evaluate results rather than writing generators:

- **Terrain**: erosion and hydrology from control heightmaps plus biome rules.
- **Scatter**: density maps × rules (slope, altitude, distance to roads and water, exclusion zones).
- **Roads and rivers**: spline networks from a graph, terrain-conforming, with intersections.
- **Buildings**: shape grammar plus modular kit assembly (kits are generated or authored assets); interiors from room graphs.
- **Clutter**: rule-based per room or street type.
- **Fracture and support graphs**: automatic from mesh plus material category.

Agents author rules and kits; the generator instantiates; agents review captures.

## 7.7 ML asset generation (novelty content)

A uniform **generator service** interface: `generate(kind, spec, constraints) → candidates[] with provenance`, pluggable backends (local or remote), asynchronous, cached by spec hash.

- **Textures and materials**: diffusion-based PBR material generation; tileable; validated for roughness and energy plausibility.
- **Meshes**: image- or text-to-mesh models produce usable props today with variable quality. Automated validation plus a repair chain (remesh, decimate, UV, bake) is mandatory. Measure the pass rate (experiment **E10**) before depending on it.
- **Concept and reference**: image models for style boards, used for embedding comparison, never shipped.
- **Speech**: TTS with per-character voice profiles, generated at build time for authored lines. Runtime TTS only at LLM Tier ≥ 2.
- **Characters**: **standardize first**. A standard skeleton, a parametric body, modular clothing and hair slots, and a face-rig standard. Generation fills slots (textures, clothing meshes conforming to templates) rather than producing whole rigged characters. Whole-character generation is an experiment, not a dependency.
- **Animation**: motion-capture libraries plus retargeting; generative motion later.

## 7.8 Runtime LLM: interface now, implementation deferred

The runtime-model idea stays in the plan as an optional way to add life to a game (contextual reactions, long-tail dialogue) when the player's hardware has spare capacity, never as something the game depends on. Everything in this section is governed by [12-ai-usage-policy](12-ai-usage-policy.md).

Interface: `Resolve(request) → intent`, where the request carries structured context, a constrained set of candidate options, and an output grammar. Tier 0 implements it with utility scoring and authored tables. Tiers 1–4 replace the resolver. Results are cached by context hash and logged as events with `origin = LLM` ([03-data-model §3.5](03-data-model.md#35-persistent-world-state)).

Constraints: inference runs on whatever the budget system grants. On the GPU when the VRAM budget has headroom after rendering (the residency manager treats model weights as one more page type, at the lowest priority), otherwise on the CPU efficiency-core pool at low priority. Batched; latency of 100 ms or more is acceptable; speculative pre-generation when a player approaches an NPC; grammar-constrained decoding so outputs are always valid; a content filter on generated text; nothing in core state depends on it. Models are third-party open-weights models under licenses that permit redistribution and commercial use, used as published.

**Long-tail authoring.** Playtest telemetry records every runtime resolution. A development-time job clusters recurring contexts, and a development-time agent *authors* deterministic rules and dialogue for them as ordinary game content with provenance. The next build handles those cases at Tier 0, and over time the runtime model handles only the genuine long tail. This is content authoring and caching: no model is trained, fine-tuned, or distilled on any of it, and no dataset of model outputs is assembled for training ([12 §12.2](12-ai-usage-policy.md#122-rules)).

## 7.9 Text and localization

All player-facing strings are document objects with IDs. Generated text is an object with provenance. Localization is a layer. TTS derives from these objects.

## 7.10 Deformable volume assets

What an asset carries for the deformable-volume primitive of [05 §5.14](05-simulation.md#514-deformable-volumes). The source mesh stays a plain glTF mesh; everything here is document properties on the asset plus painted regions, so a deformable asset is an ordinary asset with one more property block.

| Property | Units | Default | Notes |
|---|---|---|---|
| `cage.kind` | `lattice` \| `shell` \| `tet` | `lattice` | `tet` is not in v1 |
| `cage.target_elements` | count | 512 | the generator's budget; adaptive refinement spends it where curvature and thinness need it |
| `cage.min_feature` | m | 0.02 | smallest feature the cage must resolve; sets the finest cell |
| `material.stiffness` | kPa | 200 | per layer; fat ≈ 20, muscle ≈ 200, cartilage ≈ 5,000 as starting points. The solver takes the compliance 1/E |
| `material.damping` | 1/s | 4 | higher settles faster and looks deader |
| `material.density` | kg/m³ | 1,000 | element mass is density × element volume |
| `material.volume_preservation` | 0–1 | 0.8 | weight of the volume constraint; 1 is incompressible |
| `material.anisotropy` | axis + ratio | none (1.0) | fibre direction and the stiffness ratio along it, for muscle |
| `layer.region` | element selection | — | layers partition the volume; every element is in exactly one |
| `layer.bond` | `bonded` \| `sliding` \| `separable(F)` | `bonded` | behavior at a boundary with a neighbouring layer; `separable` takes a force in N |
| `cavity.region` | element selection | — | material-free region; its boundary carries a pressure constraint |
| `cavity.pressure` | kPa, relative | 0 | closed-volume pressure |
| `attachment.target` | bone id \| body id \| `free` | `free` | must resolve in the asset's own skeleton or rigid structure |
| `attachment.stiffness` | kPa | 5,000 | a stiff spring, not a weld; 0 is free |
| `limits.max_strain` | ratio | 0.5 | per-element clamp the solver never exceeds |
| `limits.max_velocity` | m/s | 20 | per-element velocity clamp |
| `contact.surface` | mesh ref \| `cage` | `cage` | a coarse authored contact surface when the cage is too coarse. Never the render mesh |
| `sim_lod.max_tier` / `min_tier` | tier (T0–T4) | T0 / T4 | caps; a cosmetic cushion may forbid the CPU tiers, a gameplay creature may forbid T3 |
| `gameplay_relevant` | bool | false | true puts the cage in the fixed-step tick and the sim hash ([05 §5.10](05-simulation.md#510-determinism-and-replay)) |
| `binding.max_influences` | count | 8 | cage elements per render vertex (a lattice cell's corners) |

**Cage generation is a `derived` step.** `mesh + deformable properties → signed distance field → adaptive lattice → binding` is one node in the dependency substrate ([03 §3.6](03-data-model.md#36-dependencies-and-invalidation)), keyed by the source mesh hash, the property block, and the function version, cached in the derived-data store like clusters are. Authored overrides (a painted region's element size, a region's material, a cavity, an attachment set) are document objects and therefore inputs to the node, so editing one rebuilds that asset and nothing else. Nothing about the cage is hand-built in a DCC tool; the default path is generation, and an override exists where the default is wrong.

**Surface binding ships with the clusters.** Each render vertex of the affected clusters is bound to its cage element and weights during the cluster build ([04 §4.3](04-renderer.md#43-geometry)) and written into the cluster page's vertex stream as a fixed 12 B per vertex (cell index plus packed local coordinates). Binding therefore streams, evicts, and pages with the geometry that needs it, adds no residency type, and costs the resident cut rather than the source asset. Every level of the cluster DAG binds to the same cage, so a geometry LOD change never changes the deformation.

**Validation** (rule IDs in the structured diagnostics of [§7.3](#73-content-build-the-derived-data-graph)):

- The cage encloses the render mesh at the bind pose with a margin. A render vertex outside every cell fails and the diagnostic names the vertices.
- Every attachment resolves to a bone in the asset's skeleton or a body in its rigid structure; standard-skeleton conformance is already checked in [§7.4](#74-validation-rules-automatic).
- Layers partition the volume: every element in exactly one layer, none unassigned, none overlapping. Cavities are closed and do not touch the outer boundary.
- Material parameters are in range, and the stiffness ratio across a `bonded` boundary is under the conditioning limit E23 measures — an ill-conditioned XPBD layer stack is a warning with a number, not a mystery at run time.
- Per-asset cost estimate (cage elements at T0, binding bytes, solver microseconds from the category's cost model) against the category budget, like every other asset ([§7.4](#74-validation-rules-automatic), Performance).
