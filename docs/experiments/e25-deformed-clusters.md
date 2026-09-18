# E25: the deformed-vertex pool, and cluster templates for deforming geometry

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), [09 §9.6](../plan/09-testing-profiling.md#96-deformable-volume-experiments)):** does transferring deformation to a clustered surface cost the **LOD cut** rather than the source mesh, and do cluster acceleration-structure templates make a deforming mesh's ray tracing geometry cheap enough to build every frame?
- **Date:** 2026-09-17. **GPU:** NVIDIA GeForce RTX 5090, driver 610.88, Vulkan 1.4.341, Windows 11, `msvc-debug`.
- **Decision:** the per-frame deformed-vertex pool of [04 §4.3](../plan/04-renderer.md#43-geometry) is built as described there, with the layout below; cluster templates are the ray-side path for deformed instances on NVIDIA and per-frame rebuilds remain the fallback. The cost rule holds with room to spare; the crack rule turned out to be the interesting result.
- **Why now, out of order:** E25 is a Phase 1 spike because the pool's layout and the per-vertex binding slot in the cluster pages cannot be retrofitted once assets are built. The solver, the cage, and the asset side ([05 §5.14](../plan/05-simulation.md#514-deformable-volumes), [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets)) do not exist yet, so what follows measures the **renderer half** against procedural deformers, and the last section says plainly what that cannot tell us.

## Setup

The pool as built ([gfx](../subsystems/gfx.md)): a per-frame device buffer of `float3` positions, a `gfx::DeformDesc` per deformed instance giving it a block as long as its mesh's cluster-ordered vertex range, and `deform.slang` — one 128-thread workgroup per entry of the cull pass's visible list, dispatched by `vkCmdDispatchIndirect` from a copy of that list's own count word. Every position reader (the mesh, vertex, and software rasterizers; the resolve's reconstruction; the cluster acceleration structure records) takes a deformed instance's positions from the pool instead of the mesh's 16-bit grid.

- **Meshes.** `FlightHelmet.gltf` (94,722 triangles, 2,348 clusters, 10 LOD levels) and the procedural heightfield at `--grid 1025` (2,097,152 triangles, 46,626 clusters, 16 levels). **22× apart in source triangles**, which is the point: the pass must not notice.
- **Cut sizes**, roughly 250 / 1,000 / 5,000 clusters. **Orbit distance alone does not get there.** Approaching the heightfield makes the LOD cut finer and pulls the terrain out of the frustum at about the same rate, so the cut saturates near 500 clusters at 800×600 whatever the distance (measured: orbit 10 → 496, orbit 5 → 488, orbit 3 → 460). The knob that moves the cut on a fixed camera is the screen-space error threshold, so the sweep fixes `--orbit 22` at 1920×1080 and varies `--lod`; the helmet reaches its large cuts as 64 instances (`--grid-instances 8`), which is also the case [E25's scenario](../plan/09-testing-profiling.md#96-deformable-volume-experiments) cares about.
- **Deformers.** `wave` (a sinusoid along the vertex normal) and `lattice` (a 3×3×3 trilinear free-form-deformation cage over the mesh's own quantization box), at the default amplitude of 0.02 of the mesh's bounds; `identity`, which writes the rest pose, is the correctness control.
- **Ray side.** `--raster rt` builds the frame's geometry from the cut two ways: **rebuild**, the existing per-frame cluster acceleration structure chain with each record's vertex address pointing into the pool, and **templates** (`--rt-templates`), one `OP_TYPE_BUILD_TRIANGLE_CLUSTER_TEMPLATE_NV` per cluster built once at load from the rest pose, instantiated each frame with `OP_TYPE_INSTANTIATE_TRIANGLE_CLUSTER_NV` from the pool.
- **Where.** `domain/gfx/tests/deform_tests.cpp` (correctness, and the small-cut build comparison) and `engine-view` with `--deform`, `--rt-templates` and `--shadows off` (the sweeps). GPU times are timestamp queries averaged over a run's frames. `--shadows off` because the shadow chain would otherwise build acceleration structures in every raster mode, and the question here is what the pool pass and the cluster structures cost on their own.

## Results

### The pass costs the cut, not the mesh

1920×1080, `--orbit 22`, `gpu_ms.deform` (the indirect dispatch alone, both runs when occlusion culling splits the cut).

| Mesh | source triangles | cut (clusters) | `wave` | `lattice` |
|---|---|---|---|---|
| heightfield `--grid 1025` | 2,097,152 | 266 | 0.0096 ms | 0.0107 ms |
| | | 1,042 | 0.0105 ms | 0.0100 ms |
| | | 4,363 | 0.0132 ms | 0.0139 ms |
| FlightHelmet × 64 instances | 94,722 | 396 | 0.0096 ms | — |
| | | 986 | 0.0105 ms | — |
| | | 4,831 | 0.0131 ms | — |

- **Slope: 0.9 µs per 1,000 visible clusters** on a fixed cost of 9.4 µs (heightfield, 0.88 ns per cluster; helmet, 0.79 ns per cluster). The fixed cost is the dispatch and the barrier the timestamp pair brackets, not the mesh.
- **The two meshes agree to within 1% at every cut size**, 22× apart in source triangles and 20× apart in clusters. That is the claim E25 exists to test, and it passes with the measurement being almost too easy: at 5,000 clusters (about 320,000 vertices) the pass writes 3.8 MB in 13 µs, ~290 GB/s on a card that does about six times that, so the pool is nowhere near a bandwidth limit and quantizing it would buy nothing.
- The two deformers cost the same within noise. `lattice` evaluates 27 control points and eight trilinear weights per vertex; `wave` decodes an octahedral normal and takes a sine. Neither is what the pass is limited by.

### Rebuilding the cluster structures against instantiating templates

1920×1080, `--orbit 22`, `--raster rt --deform wave`, `--shadows off`. `clas` is the cluster acceleration structure build or instantiation alone; `rt` is the whole chain (records, ranges, emit, that build, the per-instance cluster bottom-level builds, and the top-level build).

| Scene | cut | rebuild `clas` | instantiate `clas` | ratio | whole `rt` chain | `trace` |
|---|---|---|---|---|---|---|
| heightfield, 46,626 clusters | 267 | 0.0656 ms | 0.0142 ms | **4.6×** | 0.150 → 0.105 ms | 0.053 → 0.055 ms |
| | 1,048 | 0.0743 ms | 0.0163 ms | **4.6×** | 0.252 → 0.187 ms | 0.055 → 0.057 ms |
| | 4,559 | 0.1798 ms | 0.0608 ms | **3.0×** | 0.354 → 0.227 ms | 0.062 → 0.068 ms |
| FlightHelmet, 2,348 clusters | 289 | 0.0674 ms | 0.0162 ms | **4.2×** | 0.160 → 0.108 ms | 0.043 → 0.043 ms |
| | 648 | 0.0812 ms | 0.0156 ms | **5.2×** | 0.232 → 0.153 ms | 0.043 → 0.044 ms |
| | 978 | 0.0754 ms | 0.0162 ms | **4.7×** | 0.258 → 0.198 ms | 0.043 → 0.044 ms |

- **Templates are 3× to 5× cheaper per frame** than rebuilding the same cut, and the saving is the whole difference in the chain: everything downstream — the records, the cluster bottom-level builds, the top-level build, the traversal — is identical, and tracing against an instantiated structure costs the same as tracing against a rebuilt one (within 2%, and within 10% at the largest cut where the trace also has more geometry to walk).
- **Template memory is per cluster of the whole mesh, paid once.** 46,626 templates for the heightfield take 25,026,560 B (**537 B per cluster**); 2,348 for the helmet take 1,158,272 B (**493 B per cluster**). Against the ~1,750 B per cluster a CLAS takes ([E2](e2-cluster-acceleration.md)), a template is about 30% of a built cluster, so templating a whole mesh costs roughly a third of what one frame's structures for that whole mesh would.
- At the smallest cuts both builds are dominated by fixed cost (0.0656 ms to rebuild 267 clusters and 0.0743 ms to rebuild 1,048), which is why the ratio is flattest there and why the instantiate time barely moves at all between 267 and 1,048 clusters.
- The deform pass itself is 6% to 9% of the `rt` chain and about 4% of a 0.23 ms frame: **deformation is not what a deforming mesh costs. Its acceleration structures are.**

### The picture

From `deform_tests.cpp`, one mesh with a deformed and a rigid instance at the same transform, 320×240:

| Comparison | result |
|---|---|
| identity deformer, deformed against rigid, vertex path | coverage identical, 0 id mismatches of 12,313 covered pixels, depth within 9.3e-10 |
| identity, software rasterizer | coverage identical, 0 id mismatches, depth within 1.3e-8 |
| identity, mesh shaders | coverage identical, 0 id mismatches, depth within 9.3e-10 |
| pool slots the cut did not name | all 23,513 of 25,135 untouched (sentinel intact); all 1,622 in the cut written |
| wave deformer, raster against a KHR ray path built from the same pool | coverage identical, 16 id mismatches of 12,475, depth within 2.8e-6 |
| wave, deformed against rigid | 12,942 pixel words differ over a 12,475-pixel silhouette — the deformation reaches every reader |
| rebuilt CLAS against instantiated templates, traced | **0 coverage differences, 0 id differences, depth identical** |

The depths under the identity deformer are not bit-identical and cannot be: the rigid path dequantizes inside the rasterizer while the deformed path reads the float the pool pass stored, so two compilations of the same expression round the multiply and the add differently in the last bit (1,391 of 12,313 pixels, at most 1.3e-8 of a reversed-Z depth around 0.005). Slang's `precise` on that expression changed nothing, so the test asserts identical coverage and ids with a depth tolerance rather than pretending to bit-equality. At engine-view scale the same holds: `--raster hw` and `--raster rt` on a deformed FlightHelmet at 900×700 draw the same picture, and `--deform identity` reports exactly the cut `--deform none` reports (the engine-view end-to-end test asserts that).

### The crack rule, which is the finding worth keeping

A deformed position has to be a function of quantities the cluster build guarantees **every copy of a surface point** shares. Position is one: duplicates across clusters hold the same quantized bytes, and the LOD DAG locks group-boundary positions across levels. **A vertex normal is not.** Welded duplicates at a hard edge are separate vertices with separate normals, and a simplified level recomputes normals even where it locks positions.

The two deformers show it directly. At 5% amplitude on the FlightHelmet, `--deform lattice` — a function of the rest position alone — leaves the model whole, warped as a cage would warp it. `--deform wave`, which displaces along the vertex normal, tears the surface into separated cluster patches: along the helmet's hard edges, along its LOD-group boundaries, and even across the corner of the flat wooden plinth, where the top face's normal and the side face's normal pull the shared edge two ways. A finer cut does not help, because the seams are where surfaces meet and not where the cut is.

So E25's "no crack at any cluster or LOD boundary" criterion is a constraint on the **binding**, not on the pass: the per-vertex cage binding of [07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets) has to be derived from the vertex's position and stored per vertex so that duplicates and coarser levels agree, and the transfer must not read anything the simplifier is free to recompute. That is cheap to guarantee at build time and impossible to repair at run time, which is exactly the kind of thing this spike existed to find before assets are built.

## What this decides

1. **The pool stays unquantized `float3`, per frame, indexed by the scene-wide vertex index.** 12 bytes a vertex against the grid's 6 is not a cost worth paying attention to at cut scale, and the pass is three orders of magnitude away from a bandwidth limit.
2. **A deformed instance's block covers its mesh's whole cluster-ordered vertex range.** That is what keeps a cluster's vertices contiguous in the pool, which is what lets a cluster acceleration structure record — build or instantiate — point straight at them with no gather and no copy. `DeformDesc::pool_offset` is therefore biased by the mesh's first vertex so that it adds to the scene-wide index.
3. **Templates are the ray-side path for deformed instances on NVIDIA**, with per-frame rebuilds kept as the fallback for everything else (and for the KHR path, which has no equivalent). They cost a third of a CLAS in resident memory per cluster of the mesh and save 3× to 5× of the per-frame build.
4. **The cost rule holds** and the deform pass is not where a deforming mesh's frame goes. Budgeting attention for [05 §5.14](../plan/05-simulation.md#514-deformable-volumes) belongs on the acceleration structures and on the solver, not on the transfer.
5. **The cage binding must be a function of position.** See above.

## Caveats: what a procedural deformer does not tell us

- **The deformers read nothing.** `wave` reads one packed normal per vertex and `lattice` reads no per-vertex data at all; both are closed-form functions of the rest position. A real transfer reads a per-vertex cage binding (a cell index and weights) and the tick's cage state, which is bandwidth and an indirection the measurements here do not pay. **The pass times are a lower bound**, and the first thing to re-measure once the binding exists.
- **No skinning.** The same pool is meant to carry skinning first and cage displacement on top ([04 §4.3](../plan/04-renderer.md#43-geometry)); only one deformer per instance has been run.
- **No strain channel.** The optional parallel array of per-vertex cage strain is not built, so nothing here says what it costs or whether the pool's stride can stay as it is when a volume declares it.
- **No FLIP against deforming the full mesh**, and no cage: E25's own pass criteria about crack-freedom and FLIP under a 512-element cage stay open. What is measured is the weaker statement that a position-only deformer does not crack and a normal-direction one does.
- **The pool is allocated, not budgeted.** Every deformed instance gets its mesh's whole vertex range: 33 MB for one instance of the 2.1M-triangle heightfield, 110 MB for 64 FlightHelmets. A real system must suballocate what the cut needs, and nothing here measures that allocator.
- **`--deform` deforms every instance of the scene**, which is not a scene anybody ships; a real frame mixes rigid and deformed instances and the rigid ones cost nothing extra (they are skipped in the pass and take the unchanged grid path in every reader).
- **One GPU, one driver.** The templates path needs `VK_NV_cluster_acceleration_structure`; AMD and Intel will rebuild or refit, and neither has been measured because no such machine is reachable. The Titan X and Titan Xp baseline machines have no ray tracing at all.
- **`msvc-debug`, and timestamps at ALL_COMMANDS.** The GPU times include the barrier wait each zone brackets, which is most of the 9.4 µs fixed cost of the pool pass and part of every build number here.
- **The cut sizes are threshold-driven, not distance-driven**, for the reason in the setup; the camera sweep from 0.5 m to 50 m that E25's scenario asks for needs an asset with a usable depth range and is not this heightfield.
