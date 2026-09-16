# geometry (domain)

**Purpose.** The cluster geometry format and its builder (docs/plan/04-renderer.md §4.3, ADR-0005). The cluster is the universal unit of rasterization, streaming, and acceleration-structure construction, so its layout is a contract shared by the content build, the mesh-shader and software-raster paths, the residency manager, and the acceleration-structure builder. This module owns that layout and the CPU-side builder over meshoptimizer.

**Format (v0).** `ClusterDesc` (32 bytes, GPU-mirrored): vertex and triangle offsets and counts plus a bounding sphere. `ClusterMesh` holds the descriptors, cluster-ordered vertex positions (source vertices shared between clusters are duplicated, and `vertex_source` remembers where each came from), and one packed `u32` per triangle holding three 8-bit local indices. Limits: at most 255 vertices and 512 triangles per cluster by the format; the builder default is 64 vertices and 124 triangles, which fits one mesh-shader workgroup.

**Builder.** `build_clusters` runs `meshopt_buildMeshlets`, `meshopt_optimizeMeshlet`, and `meshopt_computeMeshletBounds`, then lays the result out in the format above. `validate_clusters` checks the invariants tests rely on: ranges and limits, every source triangle present exactly once (by sorted corner triple), every vertex inside its cluster's sphere.

**Not yet.** Per-cluster 16-bit position quantization and packed attributes, normal cones for cluster backface culling, the LOD DAG with error bounds (meshoptimizer `clusterlod.h`), fixed-size pages ordered by DAG locality, and precomputed cluster-AS inputs. Each arrives with the consumer that needs it, starting with LOD selection and streaming in Phase 1.

**Public API.** `domain/geometry/cluster.h`: `ClusterDesc`, `ClusterBuildOptions`, `ClusterMesh`, `build_clusters`, `validate_clusters`.

**Depends on.** `base`, `containers`, `math`; meshoptimizer v1.2 (MIT, third_party/LICENSES.md).

**Testing.** `tools/dev.ps1 test -Filter geometry`: grids at the default and at tight limits, a single triangle, rejected inputs, packing helpers, and the size table. The gfx mesh-shader test consumes the format on the GPU.

**Performance notes.** The builder is a content-build step, not a frame-loop one. The GPU reads `ClusterDesc` and the packed triangle words directly through device addresses; positions are 12-byte floats until quantization lands, which will cut cluster vertex bandwidth by half.
