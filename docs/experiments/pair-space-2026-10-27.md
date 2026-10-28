# The pair space: a decision packet (2026-10-27)

Roadmap R107, item 4. **No code changed.** This page is the inventory the owner needs to decide
whether the visibility id should name more than 2^24 (instance, cluster) pairs, and what the one
cheap way of doing it — a 25-bit pair with a 7-bit triangle — would touch. The id layout is the
owner's call; nothing here changes it.

## Why the question comes up

The visibility buffer is one `u64` a pixel: the reversed-Z depth's float bits in the high word and
`pair << 8 | triangle` in the low ([gfx](../subsystems/gfx.md), "The tie rule"; AGENTS.md,
"Visibility buffer"). `pair` is the pair's index in the scene's pair space (`pair_of` in
`domain/gfx/shaders/scene.slang`), so the space is 24 bits: `renderer::k_max_pairs` = 16,777,216.
The forest proof of concept's v16, v17 and v18 expanded forest at 11520×2160 admits
**16,770,201** pairs and **withholds seven outer-ring tiles** (`world.placements`: "the pair budget
withholds tiles' placements withheld=7 nearest_ring=3 pairs=16770201 budget=16777216"). At
1920×1080 nothing is withheld. [renderer](../subsystems/renderer.md#instances-that-come-and-go)
calls the limit "a stated refusal", and the refusal works as designed: the farthest tiles go first.

What the 16.77 M are, on the v18 traced run (the summary's `pair_breakdown`, measured here on
2026-10-27 with this branch's engine-view, the same numbers as the proof of concept's own report):

| part | pairs | note |
|---|---:|---|
| the prefix (the file's own instances) | 59,497 | |
| the terrain levels' reserved slots | 323,312 | ten levels, 66,116 live |
| **fixed reservation** | **382,809** | |
| the tail, live | 15,614,855 | rings 0–3: 537,303 / 4,759,984 / 5,879,944 / 4,437,624 |
| the tail's slack (block size classes) | 772,537 | `tail_hole_pairs`: what `block_capacity` rounds each tile up to, at most an eighth |
| **total** | **16,770,201** | 7,015 short of 2^24 |
| of it, static forms (built once, never in the chain) | 10,317,565 | |
| of it, untraced (grass) | 21,615 | |

Seven ring-3 tiles are about 76,000 pairs (ring 3 averages 10,877 a tile). **The tail's own slack
is ten times that.** So there are two different questions, and only the second is the layout:

1. *Should admission charge the last tiles their exact pairs rather than their size class?* The
   layout already packs tight when the classes' slack is what passes the limit
   ([renderer](../subsystems/renderer.md#instances-that-come-and-go), "Compacted"), but the placements
   consumer admits a tile at `reserved_tile_pairs` — its class — so it stops while 772,537 pairs
   are slack. Admitting against the tight pack at the limit would draw these seven tiles today,
   with no id change, and buys at most the slack (4.6% of the space). That is the placements
   consumer's and the tile layout's code (`systems/world/src/placement_tiles.cpp`,
   `systems/renderer/src/tile_layout.cpp`), not R107's; it is recorded here as the cheaper option.
2. *Should the space be larger?* Only that needs the layout, and it is the rest of this page.

## The proposal on the table: `pair << 7 | triangle`

Clusters hold at most **124** triangles (`geometry::ClusterBuildOptions::max_triangles` and
`ClusterLodOptions::max_triangles`, both 124 by default; the mesh shader's output is sized 124). A
7-bit triangle field names 0–127, so it holds every cluster the engine builds by default and frees
one bit for the pair: 2^25 = 33,554,432 pairs. The cost is a **cap of 128 triangles a cluster**
where the builders today accept up to 256 (the DAG builder, content build's manifest and the
protocol's `max_triangles`) or 512 (the flat cluster builder).

## Inventory: every writer, reader and bound of the layout

Paths are from the repository root; line numbers are main at dad55f90.

### Writers of the word (each would become `<< 7` and `& 0x7f`)

| where | expression | note |
|---|---|---|
| `domain/gfx/shaders/scene.slang:611` | `pair_of(instance, mesh, cluster)` | the pair itself; layout-free. Its comment (:605) says "high 24 bits" |
| `domain/gfx/shaders/cluster_mesh.slang:90, :112, :183` | `(v.pair << 8) \| (primitive_id & 0xff)` | `fs_main` (the R32 id the mesh-shader tests read), `fs_visibility`, `fs_visibility_masked` |
| `domain/gfx/shaders/cluster_sw_raster.slang:93` | `(pair_of(…) << 8) \| (t & 0xff)` | one thread a triangle, `[numthreads(128,1,1)]`: already never rasterizes a triangle past 127 |
| `domain/gfx/shaders/cluster_vertex.slang:59, :115` | `(pair_of(…) << 8) \| (triangle & 0xff)` | the capacity draw |
| `domain/gfx/shaders/cluster_vertex_indexed.slang:94, :182` | `id_base = (pair << 8) - record.z` then `id_base + SV_PrimitiveID` | exact only while the triangle within its cluster is under the field's width; with 7 bits, `(pair << 7)` and under 128 |
| `domain/gfx/shaders/cluster_vertex_indexed.slang:140, :230` | `(pair << 8) \| (triangle & 0xff)` | the indexed path's fallback draws |
| `domain/gfx/shaders/ray_visibility.slang:92` | `hit_key`: `(pair_of(…) << 8) \| (primitive & 0xff)` | the ray path; its tie scan (:204) keeps `max(key)`, lexicographic on (pair, triangle), which any pair-high split keeps |

### Readers of the word

| where | expression | note |
|---|---|---|
| `domain/gfx/shaders/visibility_resolve.slang:726–742` | `pair = id >> 8`, debug colour from `id & 0xff` | the resolve; the `_rt`, `_terrain` and `_water` variants include this file |
| `visibility_resolve.slang:754, 775, 792, 813, 834, 901` | `id & 0xff` as the triangle | to `reconstruct_screen`, `fetch_triangle`, `water_pixel` |
| `domain/gfx/shaders/cluster_vertex.slang:133, :147`; `cluster_vertex_indexed.slang:191, 238, 252, 260` | `fade_keeps(masks, id >> 8, …)` | the cross-fade reads the pair back out of the word |
| `domain/gfx/shaders/reconstruct_probe.slang:48–50` | `id_bits >> 8`, `id_bits & 0xffu` | test-only shader |
| `systems/renderer/src/scene_renderer.cpp:4744, :4753` | `pair = id >> 8`; `id & 0xffu` | `read_visibility`, **the one C++ decoder**: every capture's id channel goes through it, so the capture *file* (`instance`, `cluster`, `triangle` as three `u32`s, `capture.cpp`, the protocol's `render.capture`) is already layout-free |
| `domain/gfx/shaders/path_trace.slang:291` | `CommittedPrimitiveIndex() & 0xff` | masks a hit's primitive, writes no id; harmless at 7 bits but should follow |

Not readers of the id, though they shift by 8: the 8-bit local vertex indices packed three to a
triangle (`material.slang:174`, `alpha_mask.slang:57–59`, `geometry/cluster.h:348`), the hash-to-unit
helpers (water, ground detail, terrain material, display, fire, volumetrics), and the Hi-Z and the
volumetrics, which read the depth word only.

### The vertex path's own `slot << 8 | local` (not the id, but bounded by it)

`vertex_expand.slang:49–55` writes `slot << 8 | local vertex` indices and
`cluster_vertex_indexed.slang:87, :175` decodes `index >> 8` / `index & 0xff`. The low byte is a
**local vertex** (at most 255; 64 by default), not a triangle, so the 7-bit proposal leaves it alone.
But `slot` is a survivor's place in its run's `VertexDrawRecord` array, one record per pair
(`cluster_cull.h:302–311`), and [gfx](../subsystems/gfx.md) says "the slot itself cannot outgrow 24
bits, because a run holds at most the scene's pairs, which the visibility id's 24 bits already
bound". **With 25-bit pairs that sentence stops being true**: a run past 2^24 survivors would wrap
`slot << 8` in a 32-bit index. It needs its own bound — survivors past 2^24 in a run go to the
capacity draw, as an index-budget overflow already does — and a test at that size.

### Bounds that are the id layout

| where | what |
|---|---|
| `systems/renderer/include/systems/renderer/gpu_scene.h:74–77` | `k_max_pairs = 1u << 24` |
| `systems/renderer/include/systems/renderer/pair_account.h:21` | `PairAccount::limit = 1u << 24` — **a second literal**, not `k_max_pairs` |
| `systems/renderer/src/gpu_scene.cpp:501–504, :2854–2857` | refusals with the number in their sentence ("the visibility id names at most 16777216") |
| `systems/renderer/src/scene.cpp:2588–2601`, `tile_layout.cpp:241, :310` | `pairs_after`, the layout's checks against `max_pairs` |
| `systems/world/include/systems/world/placement_tiles.h:66`, `placement_tiles.cpp:283–285, :472` | the placements consumer's `max_pairs` (default `k_max_pairs`), `fixed_pairs` |
| `apps/engine_view/world_view.cpp:89–90, :328` | engine-view hands the scene's `pair_limit()` to the consumer and reports it |
| `systems/world/schemas/world_tiles.schema:157` | `TileWorldSummary.pair_limit` (a reported number, `u64`; no format change) |
| `schemas/protocol.schema:880` | `pair_breakdown: json` carries the account's `"limit"` (no `pair_limit` field of its own) |
| `domain/content_build/include/domain/content_build/content_build.h:130–132` | `k_max_clusters = 1ull << 24` ("`cluster << 8 \| triangle`"), the content build's `geometry.cluster_budget` rule and `content.cluster_budget` in `ops_methods.cpp:2563–2566` |

### Bounds that are 2^24 by coincidence (unchanged by the proposal)

`gfx::DeviceFeatures::cluster_max_geometry_index` (16,777,215 on the RTX 5090: the device's CLAS
geometry index), `clas_records.slang`'s 24-bit record fields, `gfx::k_cluster_cache_geometry`
(bit 23) and `k_cluster_cache_caster` (bit 22) — the traced chain's geometry-index space, which
R100 made compact (it names records, not pairs) — and `gpu_scene.cpp:4231`'s "needs all 24 bits".
A 25-bit pair space does not touch the chain: its index space is the frame's records, bounded by
the capacity (`renderer.md`, "The ray tracing chain's index space").

### The 128 cap: what it costs the builders and the cache

- `domain/geometry/include/domain/geometry/cluster.h:302` (`ClusterBuildOptions::max_triangles`,
  124, validated "multiple of 4 up to 512" at `cluster.cpp:375–379`) and
  `cluster_lod.h:57` (`ClusterLodOptions::max_triangles`, 124, "4..256", refused outside at
  `cluster_lod.cpp:1012–1015`): both validations would refuse past 128.
- `domain/content_build/src/content_build.cpp:110–113` (`options_in_range`, 4..256) and the
  protocol's `max_triangles: u32 = 124` (`schemas/protocol.schema:1423`, `ops_methods.cpp:1020`):
  the same refusal, with its message.
- **Nothing built today changes.** Every default is 124, the renderer reads its triangle count from
  `ClusterLodOptions{}.max_triangles` (`gpu_scene.cpp:452`), and no manifest in the tree sets
  `max_triangles`. The derived-data key already hashes `options.max_triangles`
  (`cluster_file.cpp:1336–1338`), so a container built at a value the cap now refuses keys
  differently from any that is still allowed; **no `k_cluster_cache_version` bump** (16 today) is
  needed for the cap itself. A bump is needed only if a container built at 129–256 triangles could
  be *read* by a scene: those would have to be refused at load (a cluster with more than 128
  triangles names triangles the id cannot), which is a check in the container reader the proposal
  would add. Not checked: containers under `D:\workspace\game_engine_assets` or the proof of
  concept's caches built with a non-default `max_triangles`.
- Where 124 is pinned: `cluster_mesh.slang:39` (`k_max_triangles`, the mesh stage's output; a 128
  cap could raise it to 128 since its workgroup is already 128 wide),
  `cluster_acceleration.h:42` (`max_triangles_per_cluster = 124`), `rt_cluster_tests.cpp:108`
  (`props.max_triangles_per_cluster >= 124`), `geometry/tests/cluster_tests.cpp:67, :109`,
  `content_build_tests.cpp:178` (257 refused), `protocol_tests.cpp:653` (the default).

### Tests that pin the layout

No test compares an id word or an id capture with a stored golden: every id comparison is
**relative** (hardware against software, mesh against vertex, ray against raster, with and without
occlusion — `visibility_tests.cpp:295–310`, `vertex_path_tests.cpp:333, 953–963`,
`scene_tests.cpp:650, 847–868, 966`, `occlusion_tests.cpp:462`, and every renderer test that compares
two captures' decoded triples). Those catch a writer that disagrees with another writer, not a
layout that all of them changed together. What decodes the word itself, and would fail or misread if
it were not updated with the layout:

- `domain/gfx/tests/mesh_shader_tests.cpp:166–167` (`id >> 8`, `id & 0xff`; checks every triangle is hit),
- `domain/gfx/tests/cull_tests.cpp:337` (`>> 8` must be in the expected cluster set),
- `domain/gfx/tests/ground_detail_tests.cpp:809–811` (`clusters[id >> 8]`, `id & 0xffu`),
- `domain/gfx/tests/scene_tests.cpp:665–676` (`instance_of_id`, `id >> 8`),
- `reconstruct_tests.cpp` through `reconstruct_probe.slang:49–50`.

`visibility_tests.cpp:306` (`(… >> 8) >= cluster_count` as a bad-id check) would **not** notice a
7-bit split. The pair limit's own number is pinned by `authored_scene_tests.cpp:98` (the sentence
"… of the 16777216 the visibility id names"), `tile_layout_tests.cpp:101–287` (a literal `1u << 24`)
and `apps/engine_cli/tests/render_tests.cpp:220` (a skip guard); `dynamic_instances_tests.cpp:683–694`
uses `k_max_pairs` by name. **What no test has**: a scene past 2^24 pairs. A change to 25 bits would
need one (a surround of a grid whose pairs pass 2^24, drawn by every rasterizer and the ray path,
ids decoded and compared, and the vertex path's slot past 2^24), and its memory is the reason there
is none today — the cull dispatches a thread a pair and the per-pair tables are four to eight bytes
a pair a view.

### The pages that state the layout

AGENTS.md ("Visibility buffer", and the vertex path's `slot << 8 | local`), `docs/subsystems/gfx.md`
(:104 the id, :106 the tie rule, :229 the indexed draw and "the slot cannot outgrow 24 bits"; :82 is
stale already — it still says "cluster index in the high 24 bits"), `docs/subsystems/renderer.md`
("Capture channels, and the id encoding", "The 2^24-pair limit stays a stated refusal", "The pair
accounting"), `docs/subsystems/apps.md:274` (`geometry.cluster_budget`), `world.md:19, 58, 219`,
`geometry.md:142, 329`, ADR-0040, and plan pages 04 §183, 07 §99, 10 §114. The comment in
`systems/renderer/include/systems/renderer/capture.h:8–9` still says `visible_index << 8`, which has
been stale since the id became the pair.

## What each choice costs, in one place

| | admission at the exact pairs | `pair << 7 \| triangle` (2^25) |
|---|---|---|
| pairs gained | the slack: 772,537 on this frame (4.6%) | 16,777,216 (2×) |
| the forest's seven tiles | drawn | drawn |
| code | the placements consumer and the tile layout | 8 shaders, 1 C++ decoder, 2 limit literals, 3 builder validations, the vertex path's slot bound |
| formats | none | none on disk or on the wire (captures are decoded triples); the cluster cap refuses what is built at 129+ |
| what it costs at run time | nothing | the per-pair buffers and the cull's dispatch grow with the pairs a scene actually admits: nothing until a scene uses them |
| tests to add | the consumer admitting a tile at its exact pairs at the limit | a scene past 2^24 on every rasterizer and the ray path; the slot bound |
| the owner's decision needed | no | yes: the cluster cap |

R107 recommends neither; the owner asked for the inventory. Two things are worth saying beside it:
the forest is 7,015 pairs short of the limit with ten million pairs in **static forms** (rigid
distance forms the chain builds once), so the pair space is mostly the far rings' forms, and a
scene that wants more distance has more of them; and the 2× of a 25th bit is the last cheap one —
a 6-bit triangle (64-triangle clusters) would halve every cluster and double the DAG's nodes, which
is not a trade the renderer's measurements support.
