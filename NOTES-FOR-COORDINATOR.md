# Notes for the coordinator — LOD simplification across UV seams

Branch: this worktree. Nothing pushed, `main` untouched.

## The diagnosis, confirmed in the code

The coordinator's diagnosis was right and is **worse than stated**. `build_cluster_lod`
(`domain/geometry/src/cluster_lod.cpp`) filled meshoptimizer's `clodMesh` with the indices, the
vertex count and the positions, and left `vertex_attributes`, `attribute_weights`,
`attribute_count`, `attribute_protect_mask` and `vertex_lock` at zero. Two independent failures
followed:

1. **No attribute term in the error metric.** With `attribute_count == 0` the simplifier's
   quadrics are positions only, so a collapse that destroys the UVs and leaves the silhouette
   alone scores as free.
2. **No topological barrier either, and this is the one that was not obvious.**
   `clodDefaultConfig` sets `simplify_permissive = true`, and `meshopt_SimplifyPermissive` is
   documented as *"allow collapses across attribute discontinuities, except for vertices that are
   tagged with `meshopt_SimplifyVertex_Protect`"*. clusterlod's own header says "make sure to use
   `attribute_protect_mask` if this is set!". So the builder was running in the one mode that
   requires seam tags, with none.

One correction to the brief: `weld_vertices` is **not** the culprit — it keys on position, normal,
UV and skin binding together, so the seam duplicates arrive intact. The position-only remap that
erases them is *clusterlod's own*, for connectivity, and it is correct and necessary; permissive
mode without tags is what makes crossing that connectivity free.

## The fix

`ClusterLodOptions` gained five fields: `normal_weight` (**0**), `uv_weight` (0.5), and three
`SeamRule`s (`uv_seams = protect`, `normal_seams = none`, `skin_seams = protect`). The builder
passes a five-float attribute record per source vertex (normal, UV) with weights, and a
`vertex_lock` array tagging every vertex that shares a position with another but disagrees about
the UV / the normal / the skin binding. `k_cluster_cache_version` is **7**; all five fields are in
`cluster_cache_key`.

**The weights were nearly a second bug, and the measurement is the most useful thing here.** The
obvious design — normals and UVs both at 0.5, which is what meshoptimizer's own cluster-LOD sample
does and what this change shipped with for an hour — broke the renderer's streaming tests. Sweeping
the weight with the seam rule fixed:

| `normal_weight` | heightfield cut at 30 units | at 120 units | shredded torus | worst UV error |
|---|---|---|---|---|
| 0 | 16,367 tri | 2,041 | 4,602 tri | 8.0 texels |
| 0.1 | 21,010 | 2,038 | 4,602 | 8.0 |
| 0.25 | **32,768** (all of it) | 4,084 | 4,602 | 8.0 |
| 0.5 | **32,768** | 8,177 | 4,602 | 8.0 |

The atlas column **does not move at all**: the weights contribute nothing to the defect, and
`uv_seams = protect` does the whole job. The normal weight meanwhile costs up to 4× the triangles
at a fixed threshold, because a normal delta is absolute while the position error shrinks with the
group. So it is off by default — otherwise `--lod 1` would quietly mean something different for
every scene, every corpus threshold and every number in the docs. The UV weight stays at 0.5
because it is free on everything measured and is the only term that can see a collapse that
*shears* the texture without moving the surface.

**The error the cut uses needed no second term.** meshoptimizer's `result_error` is computed from
the same collapse cost it ranks by, and that cost already includes the attribute quadric when
attributes are supplied — so the attribute error lands in `ClusterLodDesc::own_error`/`parent_error`
automatically, in the units `lod_selects` and `cluster_cull.slang` already project. Nothing in the
cut, the GPU pass or the page layout changed.

## The measurement

`measure_lod_attribute_error` (`domain/geometry/include/domain/geometry/cluster_lod.h`) is the CPU
metric, no GPU: sample the level-0 surface, closest point on the cut, compare interpolated UVs in
texels of a 4096 atlas and normals in degrees. Fixed stride, so it is deterministic.

On the procedural shredded-atlas fixture, matched triangle budget:

| build | UV mean | p99 | max | samples over 8 texels |
|---|---|---|---|---|
| positions only (before) | 263 texels | 2,747 | 4,449 | **30.7%** |
| attributes + protected seams | 2.6 | 8.0 | 10.1 | **1.0%** |

Seam-rule cost (same fixture): `none` 9 levels / 34 triangles at the coarsest cut; `protect` 8 / 66;
`lock` 3 / 2,700. `lock` is not "stricter", it is *stuck* — which is why `protect` is the default.

And as a picture, from the renderer's own test (384×384, RTX 5090, coarse against finest on one
camera, cone culling off so the two counts compare cuts and nothing else):

| build | cut | FLIP mean | p95 | PSNR | SSIM |
|---|---|---|---|---|---|
| position-only | 64 of 140 pairs | 0.140 | 0.761 | 18.8 dB | 0.811 |
| seam-aware | 63 of 140 pairs | 0.0092 | 0.035 | 39.0 dB | 0.996 |

The cuts are the **same size**. All of that difference is texture.

## Evidence on the four real models

`msvc-release`, RTX 5090, machine shared (15–18% CPU and 8–13% GPU in other processes; the picture
numbers are unaffected, the times are upper bounds), every figure twice. Coarse (`--lod 1`) against
**each mesh's own leaves**, 640×960. Nothing derived from the owner's two models is committed.

| Model | before | after | seams only (`--uv-weight 0`) |
|---|---|---|---|
| Meshy character (539,596 tri, 697 islands, 17.4% seam vertices) | 356 pairs, 0.0241 FLIP, 29.6 dB | 510, **0.0115**, **38.1 dB** | 524, 0.0111, 38.5 dB |
| Meshy rigged (224,783 tri, 501 islands, 37.5% seam) | 280, 0.0223, 28.6 dB | 690, **0.0083**, **39.7 dB** | 694, 0.0082, 39.9 dB |
| FlightHelmet (94,722 tri, 239 islands, 27.5% seam) | 387, 0.0174, 34.0 dB | 393, 0.0173, 34.1 dB | 394, 0.0174, 34.1 dB |
| Suzanne (3,936 tri, 3 islands, **0** seam vertices) | already at its leaves at `--lod 1` | | |

- **The finest picture is byte-identical** on all four (FLIP 0, SSIM 1): the change moved coarse
  levels and nothing else.
- **More triangles do not buy the old build out of it.** Position-only at **569** pairs on the
  static model reaches 32.3 dB where seam-aware at **510** reaches 38.1; on the rigged model 774
  pairs reach 35.4 dB against 690 pairs at 39.7. It is the wrong texture on the right surface, and
  a budget cannot fix that.
- **`--uv-weight 0` matches the default to within 0.4 dB everywhere**, confirming on real content
  what the fixture said: the seam rule is the fix, the weights are not.
- **Suzanne has no seams at all**, so the rule tags nothing and the container is the same to a few
  hundred bytes — the fix costs nothing where there is nothing to protect.
- **The rigged model holds in a pose.** Through `--animate`, bind pose and frame 20 of the clip:
  before 0.0223 / 28.6 dB and 0.0084 / 32.1 dB; after 0.0083 / 39.7 dB and **0.0030 / 40.7 dB**.
  The leaf-level pictures are byte-identical in both poses, which is the answer to the skin
  question: protecting a weight split changes the coarse levels and changes the deformation of the
  finest surface not at all. (The owner's note about texture artefacts in the clothing is not a
  factor — every comparison here is coarse against that same mesh's own leaves, so a source
  artefact appears identically on both sides and cancels.)

## A bug this measurement found, worth its own look

`SceneDesc`'s single-mesh load path built its `ClusterMeshPart` by hand and left
`first_leaf_cluster` at **zero**, while a `.clusters` container is in page order (coarse first,
leaves last). Everything that asks a part where its leaves are therefore got the coarsest clusters
— including the camera, which frames the leaves' bounds. Two builds of one mesh differing only in
their *coarse* levels were drawn at two different sizes, and their finest pictures disagreed by
**0.23 FLIP** where they draw identical triangles. Fixed in `systems/renderer/src/scene.cpp` with a
regression test. It is pre-existing and independent of the seam work; it only became visible
because this was the first time two DAGs of one mesh were compared.

## Files touched (kept inside the agreed scope)

- `domain/geometry/**` — the fix, the metric, the procedural fixture (`stress_mesh.h/.cpp`), tests.
- `docs/subsystems/geometry.md`, `docs/plan/04-renderer.md` §4.3, `docs/plan/07-content-pipeline.md`
  §7.4, `docs/plan/10-roadmap-risks.md` (E10 row).
- Later commits: `apps/engine_content`, `systems/renderer/src/scene.cpp`, `content/test-scenes`,
  `docs/subsystems/apps.md`, `docs/subsystems/renderer.md`.

**Not touched**, as agreed: `cmake/EnginePhysics.cmake`, `CMakePresets.json`,
`cmake/EngineOptions.cmake`, `core/platform`, `domain/gfx/src/device.cpp`, `bindless.*`, the
`gpu.adapters` method, `tools/package-tests.ps1`,
`systems/renderer/src/{streaming,gpu_scene,page_source}.cpp`,
`domain/gfx/src/cluster_acceleration.cpp`, `tools/ci/linux.Dockerfile`, `tools/linux-build.ps1`.

Note: `tools/dev.ps1 format` formats the whole tree. It was run and touched only my own files, so
the tree was already clean — but anyone running it with uncommitted work elsewhere should check
`git status` afterwards.

## The corpus gate

`content/test-scenes/shredded-atlas.json`, run through `tools/ci/reference-compare.ps1`, twice with
identical numbers: **0.0575 FLIP (p95 0.1870), 32.2 dB**. With the seam rule off the same scene
gives **0.1231 (p95 0.6481), 20.8 dB**. The thresholds are 0.09 / 0.30 — between the two, so the
regression the scene exists for fails the gate rather than clearing it.

It is the only corpus scene whose reference traces the *finest* geometry while the real-time path
draws a coarse cut, which is why it is the only one that can move when simplification changes. The
other five compare the same cut to itself and are structurally blind to this class of defect.

## Things worth a second opinion

- **`normal_weight = 0` is a judgement, not a measurement.** The measurement says it is expensive
  and does nothing for *this* defect; whether shading accuracy is worth 28% (at 0.1) is the owner's
  call, and the table above is the price list. The lever is per-build and in the cache key, so a
  hero asset can pay it without the terrain doing so.
- **`normal_seams` is off by default.** The argument is in geometry.md: merging a hard edge is a
  bounded shading change that the position metric already resists on a mechanical model, while a UV
  seam is a change of *address* that no weight can price. If hard-edge damage shows up, this is the
  switch.
- **Residual limit, unfixable here:** below some island size no seam-respecting simplification can
  be right, because the island is smaller than a triangle at that level. Both generated characters
  already have an island whose UV area rounds to **zero** texels of a 4,096 atlas (six triangles on
  one, a single triangle on the other). The honest fix is in texture space — atlas repack, or
  per-level baked textures. Recorded as a follow-up in `geometry.md` and plan 07 §7.4, with the
  numbers a validator would threshold already reported by `engine-content stats`.
- **Skin bindings as a weighted attribute is *not* done and the reasoning is in geometry.md.** Joint
  indices are categorical so a quadric over them is meaningless; the four weights are continuous
  but largely redundant with position on a well-authored skin; and clusterlod keeps original
  vertices, so a coarse cluster always carries an authored binding. What is left unprotected is a
  collapse between two *different* positions with very different weights, bounded by the geometric
  error the cut already uses. Measured on the rigged model it did not show — but n = 1, and a
  character with a hard weight boundary (a belt, a shoulder pad) is the case that would.
