# R188: instances in a hierarchy

*Status 2026-11-19, batch 70: **stage 1 is built**, behind `renderer.cull.nodes` (off), as [nodes before instances](../subsystems/renderer.md#nodes-before-instances) describes; where it departs from the design below is said there and in "Stage 1 as built" at the end. Stages 0, 2 and 3 are not built.*

*2026-11-18, batch 69. A design, nothing built: the row ran while the batch's gate held the machine, so no binary was flown and every number below is read from a record that exists (named where it is used) or computed from one in a scratch script that only computes; a number that needs a run is marked **to measure** with the command that takes it. The research was read on this run, from the sources' own text where it could be had (the list at the end says how far each was read).*

## The question

R16 and R173 put a pass over the instances in front of every cull run ([the cull culls instances first](../subsystems/renderer.md#the-cull-culls-instances-first), [the measurement](cull-by-instance.md)): a thread an instance tests the sphere round its mesh's clusters, and only the survivors' pairs are culled. R16's row closed with "not done: a hierarchy over the instances themselves, for a world of millions of instances; the flat pass is most of the 0.236 ms at 0.33 M". This page asks what a level above the instance buys, where it comes from, what it must keep byte-identical, and in what order it is built.

## What the cull costs now

**Traced, the grid of rocks** (R173's page; 3840 × 2160, `--raster rt --shadows rt`, 400 frames, the session locked, 2026-11-18 00:05 to 00:12; the cull's medians in ms, every frame of the three culling the same set):

| Pairs (instances) | the pass over pairs, searching | reading the table | instances first |
|---:|---:|---:|---:|
| 1.01 M (19,880) | 0.052 | 0.035 | 0.019 |
| 8.04 M (157,608) | 0.325 | 0.181 | 0.112 |
| 16.74 M (328,328) | 0.747 | 0.354 | 0.236 |

**Rasterized with the cascades, the same grid** — R173's page says this pair "was not flown"; it was, after the row handed back, and its records are in the row's scratch (`<scratch>\b68\R173\runs\r16-csm\`, 2026-11-18 00:37 to 00:39, the session locked, the processors 65 to 100 % busy with others' builds, `bin-r173`, which is the instance pass before R173's fix; the fix measured as costing nothing, [cull-by-instance](cull-by-instance.md#the-reviews-finding-and-a-second-one-under-it-2026-11-18)). Medians in ms, `cull` the camera's two occlusion runs, `shadow_cull` the four cascades', the sum of every timed zone last; every frame of the three binaries culled the same visible and shadow pairs:

| Pairs | cull: search / table / instances first | shadow cull: search / table / instances first | the zones' sum |
|---:|---:|---:|---:|
| 1.01 M | 0.070 / 0.052 / **0.024** | 0.117 / 0.081 / **0.026** | 1.634 / 1.486 / 1.264 |
| 8.04 M | 0.444 / 0.266 / **0.147** | 0.844 / 0.423 / **0.128** | 5.094 / 3.472 / 2.347 |
| 16.74 M | 0.953 / 0.503 / **0.290** | 1.848 / 0.900 / **0.256** | 9.484 / 5.740 / 3.407 |

`streamed-forest` traced (0.43 M pairs, 5,507 instances): 0.036 / 0.027 / 0.014 ms. **Both tables were flown on the RTX 5090 between the display driver's change (Tuesday 16:41) and this batch's base, so they are indicative and are flown again on the base by stage 0** (the batch's rule for evidence from that window). What they say is already plain: at the cap the instance pass took **0.64 ms** off the cascades and 0.21 ms off the camera's runs, and what is left with the cascades (0.546 ms) is more than what is left traced (0.236).

**The forest is at the grid's instance count, not at its pairs.** The forest staging (`v41`) flown on batch 67's main along the owner's drop (`<scratch>\b68\T172\loads\D1-main\frames.jsonl`, 2,461 frames, hand-off cells on): **297,464 instances at the median and 374,044 at the most**, 3.33 M pairs at the median and 7.39 M at the most, 15.8 pairs an instance at the median, 2.9 % of the pairs visible. Before the hand-off cells the staging's grounded walk peaked at 14.4 M pairs (batch 65's build; the staging's own migration notes), and the forest's trail walk at 16.76 M handed whole against 8.05 M with cells of 4 m ([world](../subsystems/world.md#a-hand-off-placement-is-held-by-its-band)). So the forest's instance passes cost what the grid's do at 16.7 M pairs (328,328 instances), whatever its pairs; it is the instance count, not the pair count, that a level above the instance divides. A village is 2,789 instances and 142,400 pairs ([village](../subsystems/village.md)): nothing here is for it.

**How it grows with the view.** The forest's tiles are 64 m with rings at 1.25, 3, 10 and 20 tiles. On T172's cut of the staging (`<scratch>\b68\T172\loads\W1-mend`, the world log's last update) the rings held 4, 28, 263 and 264 tiles and 15,020, 19,610, 78,032 and 31,542 instances: 3,755, 700, 297 and 119 instances a tile from the eye out. A whole outer ring at that density is about 942 tiles and 112,000 instances, and the four rings about 232,000 with the prefix beside them. **Instances grow with the area within reach**: an outer ring of 40 tiles (2.56 km) at the outer ring's density holds about 561,000 instances by itself, 680,000 in all, at about 9.4 pairs an instance there — 5.3 M pairs, still under the cap. The cap (2^24 pairs, the owner's, not moved here) bounds instances only through pairs: about 1.7 M instances at the far rings' 9.4 pairs, and 16.7 M if every instance were one cluster (a kit of boxes). A flat pass over instances therefore grows with the square of the view distance until the cap, and is run once per cull run: once traced, six times with occlusion and four cascades.

## Where the remainder goes

The cull's zone counts the instance pass, its fold and the item pass together (`CullInstances::add_instance_pass` times them under the run's zone), so how the 0.236 ms divides **was not measured**. What can be said from the records and a model (`<scratch>\b69\R188\model.py`, which computes only):

- **The grid's frustum keeps about 24 % of its instances** (55° vertical at 16:9, the camera at the grid's centre: 23.8 to 24.0 % at all three sizes). Each survivor of 51 pairs is two items, 64 lanes. So after the instance pass the item pass runs about 5.0 M lanes at the cap, against 16.7 M threads before, plus 0.33 M instance threads: 32 % of the pass over pairs.
- **If a lane costs what a thread of the table's pass costs** (both start with their instance known, one load or none), the items are about 0.32 × 0.354 = 0.11 ms of the 0.236, and the instance pass and its fold the other 0.12.
- **The cascades bound an instance thread's cost**: their four instance passes and their few survivors (118,629 shadow pairs at the median) are 0.256 ms, at most 0.195 ns an instance thread. The traced camera's one instance pass, whose outside instances test the caster volumes, is then about 0.06 ms at the cap: a quarter of the 0.236, not most of it.

So R16's "the flat pass is most of the 0.236 ms" is not what the records support: traced, the larger share is likely the survivors' whole DAGs — 51 lanes for a rock that draws one or two clusters — and the instance pass a quarter to a half. With the cascades and occlusion it is the other way: six instance passes over every instance a frame, most of whose instances survive nothing, are most of the 0.546 ms. **Stage 0 measures the split before anything is built.**

## What the published renderers do

Read on this run:

- **Haar and Aaltonen, "GPU-Driven Rendering Pipelines"** (SIGGRAPH 2015, Advances in Real-Time Rendering; the deck's text extracted). Assassin's Creed Unity's pipeline begins with "COARSE FRUSTUM CULLING - CPU" — "CPU quad tree culling" — then on the GPU "INSTANCE CULLING (FRUSTUM/OCCLUSION)", "CLUSTER CHUNK EXPANSION", "CLUSTER CULLING (FRUSTUM/OCCLUSION/TRIANGLE BACKFACE)". So their hierarchy above the instance is a coarse spatial structure culled first, and the GPU then culls instances flat. RedLynx's half describes two-phase occlusion in which the second phase tests only "culled objects & clusters" of the first, and its "torture" scene of 250,000 moving objects measured object culling and LOD at 0.28 ms in the first phase and 0.26 ms in the second, cluster culling 0.09 and 0.04 ms (Xbox One, 1080p). (R173's page marked this deck not read; it is read now.)
- **Wihlidal, "Optimizing the Graphics Pipeline with Compute"** (GDC 2016; read in a hosted transcript of the slides, the author's own PDF did not download): "Coarse cull on CPU, refine on GPU"; clusters of triangles rejected coarsely against the view, the frustum and the Hi-Z, with the cone test, then triangles. No hierarchy over instances on the GPU.
- **Karis, Stubbe and Wihlidal, "Nanite: A Deep Dive"** (SIGGRAPH 2021 Advances; the deck's text and speaker notes extracted). The instance cull is flat — "For each view we can determine the visible instances in a single dispatch" — and the deck says linear scaling in instances "can be ok … We can handle a million instances easily", while "Linear scaling in triangles is not ok". The hierarchy it builds is *below* the instance: a BVH8 over each mesh's cluster groups whose nodes hold the "Max of children's ParentError", because "Typically, for a given view, there are orders of magnitude more clusters that are too detailed than there are clusters that are not detailed enough". The two-pass occlusion carries "Occluded instances" and occluded nodes and clusters from the main pass to the post pass, and "frustum and LOD culling is only done in the first pass".
- **Unreal Engine 5.4's scene culling** (read in a site that reproduces the engine's console variables with source excerpts, not in Epic's documentation): `r.SceneCulling.MinCellSize` "Set the minimum cell size & level in the hierarchy, rounded to nearest POT. Clamps the level for object footprints", default 4096 (40.96 m); its buffers are `CellHeaders`, `ItemChunks`, `Items`, `CellBlockData`, `ExplicitCellBounds`; `r.Nanite.UseSceneInstanceHierarchy` defaults to 1, and "The main pass instances are produced on the GPU if the hierarchy is active". So the later Nanite does put a level above its instances: a loose grid of power-of-two cells sized by an object's footprint, with chunks of items in each, culled on the GPU before the instances.
- **NVIDIA's `vk_lod_clusters`**: "A thread represents one instance", as R173 read it; not re-read here.

**What fits a world already cut into tiles.** Every source with a level above the instance makes it spatial and coarse — a quad tree, a grid of cells — and none makes it deep: one level, and then the instances flat. The engine's world already places each tile's instances in a run of slots of their own, and since G205 each hand-off cell's (`TileLayout`, [instances that come and go](../subsystems/renderer.md#instances-that-come-and-go)): the spatial grouping UE builds with its hash is the layout the renderer already holds. What the sources add that the engine lacks is Nanite's and RedLynx's second phase over what the first rejected by occlusion alone, and Nanite's hierarchy below the instance, which is where the traced remainder likely is.

## The design

### A node is a run of at most 64 slots of one block

A **node** is up to 64 consecutive instance slots that belong to one block of the layout — a tile's run, a hand-off cell's run, or a run of the fixed prefix — with a sphere round every instance's tested sphere. A block of 70 slots is two nodes; a hand-off cell of 4 m is one, with a sphere a few metres across. A node never spans two blocks, because two blocks adjacent in slot order may be kilometres apart: on the grid, where slot order is row order, runs of 64 slots in that order kept 38 to 79 % of their instances against 24.8 to 28 % for squares of 8 × 8 instances, against 24 % for the instances themselves (model.py). The prefix is cut into nodes at load in slot order, a node ended where the next instance's 64 m cell is more than one cell from the node's first, and where the terrain level changes (below). 64 is the instance pass's workgroup: a surviving node is one workgroup of the instance pass, a lane a slot, and the pass from there on is the one built.

The CPU keeps the nodes beside the layout: a `NodeLayout` that gives each block its node ids when it is placed, keeps them while the block lives (kept, rewritten in place, moved by T164's moves) and frees them with it, from a free list grown by half again like every other table; and a node id a slot (4 bytes) for the slots that move alone (`move_instances`). A node's entry is 32 bytes: its sphere in its anchor's frame (`float3` centre, radius), its first slot, and a packed word (live count, terrain level, the largest caster-policy cascade of its instances, and whether every instance carries the near hand-off role). Its anchor is its first live instance's `WorldCell`, read from that instance when the node is tested, so the entry holds no world position (ADR-0053).

### The renderer measures its nodes; the world does not hand it boxes

The hand-off cells already keep a box of their placements' reach ([world](../subsystems/world.md#a-hand-off-placement-is-held-by-its-band)), and a tile's extent is known to the world. Neither is used: the instance pass's sphere is measured on the GPU from the cluster table (`mesh_bounds_main`) and moved by the instance's own arithmetic, and a node's sphere must contain *that* sphere, in the GPU's floats, for the culled set not to change. A box from the world would need an argument that two programs agree to the ulp, and it would hold only for what the world places — an imported world, a scene read whole and a game's moved props have no world. So the node is measured where the instance is: a workgroup a node, a lane an instance, each lane taking its instance's sphere exactly as `instance_may_survive` does (`instance_point` on the mesh's sphere, `bound.w × scale_max × 1.0001 + padding`) but from the anchor's frame instead of the eye's, and a reduction to the box's centre and the farthest reach, as `mesh_bounds_main` reduces clusters. The renderer stays general: the nodes follow from slots and instances, which every consumer has.

### Why the node's sphere holds each instance's

The instance pass tests `r_i = bound.w × s × 1.0001 + padding_i + |c_i| × 1e-5 + 1e-4` round `c_i`, both in the eye's frame. The node stores `R = max_i (|c'_i − m| + bound.w × s × 1.0001 + padding_i + 1e-4) × (1 + 1e-5) + 1e-4` round `m`, primes for the anchor's frame, and its test uses `R + |c_m| × 2e-5` round `c_m`, the centre moved to the eye's frame as an instance's translation is. Since `|c_i| ≤ |c_m| + |c_i − c_m|` and `|c_i − c_m| ≤ R`, the instance's distance-dependent slack is at most `(|c_m| + R) × 1e-5`, which the factor on `R` and the first `1e-5` of `|c_m|` cover; its constant `1e-4` is inside the maximum; the second `1e-5` of `|c_m|` covers rounding the node's centre into the eye's frame where the instance's was rounded separately (a few ulps of `|c_m|`, about 6e-8 each, against 170 here), and the last `1e-4` the reduction's own rounding. A terrain instance's level padding moves with the frame (`TerrainLevelDesc::padding`), so it is not measured: a node holds one level's instances only and its test adds that level's padding when the frame's terrain table names one. The derivation goes in the shader's comment beside the instance pass's, and the debug guard below checks it on every frame of every GPU test.

### The tests at a node

The instance pass's own, on the node's sphere, each monotone in the sphere ([the comment in cluster_cull.slang](../subsystems/renderer.md#the-cull-culls-instances-first) has why each is): a node with no live instance (holes and freed blocks only) emits nothing; the cascade's caster policy, by the node's largest cascade (an instance casting into every cascade counts as the largest); the frustum, and outside it the caster volumes when the run keeps casters, and inside it a cascade's own volumes; the Hi-Z where the run culls by it and neither streams nor counts, by `sphere_nearest_depth` and the footprint, as the instance pass. One test the instance pass does not have yet joins both: **the near form's band** (`handoff_outside`), a node whose every instance carries the near role and whose sphere lies wholly at or past the band's end from the camera, which is `distance − radius ≥ to` and so monotone too. Nothing is tested by LOD: the instance pass tests none (every instance draws some cluster of its DAG), and a range a node could test exists only for the hand-off's two forms.

What a node never tests is what has a side effect in the pair pass: a pair that streams asks for its pages before its occlusion test, and the occlusion term's counters count an occluded pair's LOD, so a run that streams or counts culls no node by the Hi-Z — the instance pass's rule.

### The passes of a run

Per cull run, in front of the instance pass: **the node pass**, a thread a node over the frame's node table (about 10,000 nodes on the forest's 375,000 instances, two to a tile of 72 slots and one to a hand-off cell; a folded dispatch of their count), appending the survivors' ids one atomic a wave; a fold of that count; then **the instance pass from node items**, a 64-lane workgroup a surviving node, each lane `instance_may_survive` on its slot exactly as today and the items appended as today; then the instance pass's fold and the item pass, unchanged. The node pass for a group of runs (the camera's blocks, the cascades) is one dispatch looping over the runs, as the instance pass is. Nodes are measured, on the frames that need it, before the first node pass (below). Every pass declares what its shader reads and writes, the node table's read included (batch 68's rule); the node items, their count and their dispatch are graph buffers like `cull items`.

### What keeps a node's sphere true

A stale node sphere drops an instance the pass would have drawn, so everything that changes an instance's tested sphere re-measures its node before the frame's first node pass:

- **A slot written**: a tile's change, T164's moved blocks, a moved instance (`move_instances` in a scene read whole rides the record upload; in a dynamic scene the flip). The table sets are flipped in turn, so each set keeps its own node spheres, and the nodes of the slots a set's pending runs write are listed for that set as the runs are (the slots' node ids), measured when the frame flips to it (`GpuScene::prepare_tables`), after its copies and before any cull. The pairs numbered again (T154) move no slot and re-measure nothing.
- **A mesh's sphere changed**: on a frame that measures the meshes' spheres (`bounds_due`: a written mesh table, a relayout, the first frame), every node is measured again. A ring's chunk shown or hidden writes the mesh table, so a walk pays a full measure on those frames (the rings uploaded on 73 of the 901 frames of T172's flight over its cut of the staging): one pass over every instance, about what one instance pass costs. Narrowing it to the nodes holding the written meshes' instances is a later step, measured first.
- **A relayout or a set made again**: every node of the new set.

**The guard** (debug builds and every GPU test): after the node pass, a pass over every live instance computes its sphere as the instance pass does and its node's as the node pass does, in the eye's frame, and counts any instance not inside its node's into a word the tests read back and require to be zero. It turns a missed re-measure — the fault class this row adds — from a picture that loses an instance on one frame into a failing count on every frame that has one.

### Cascades and traced casters

The cascades' runs take the node pass like the camera's: each tests its own frustum and volumes and the caster policy, with the camera's LOD left to the pairs as now. A caster outside every frustum still casts: a node outside the frustum survives when its sphere meets a caster volume, exactly as an instance does, and the traced chain's casters are the pairs those instances keep. The cascades are where the node pass pays most: on the grid at the cap their four instance passes over 328,328 instances are most of 0.256 ms for 118,629 shadow pairs, and a node pass tests 5,184 squares or their equivalent four times.

### Occlusion's second pass

Today the second occlusion run's instance pass tests every instance again against the new Hi-Z. With nodes it takes, as Nanite's post pass and RedLynx's second phase do, only the nodes the first run kept or rejected by the Hi-Z alone: the frustum, the volumes and the caster policy are the same in both runs of a view, so a node the first run rejected by them the second rejects too. The first run appends its Hi-Z-rejected nodes to a list of their own, and the second run's node pass is over its survivors and that list. A later stage, behind the first, because it is the hardware path's alone.

### R174's keep order

The ray tracing chain keeps whole instances by a rank laid out per slot on the CPU (`GpuScene::lay_out_rt_order`), not in the order the cull appends them, and the visible list's order within a run is already the atomics' ([the visibility buffer](../subsystems/gfx.md)). Nodes change that append order and nothing the chain ranks by, so they neither help nor hinder R174. If R174 ranks by distance from the eye, a node's distance is a coarse key it could bucket by without a sort; that is R174's choice.

### Memory

32 bytes a node a table set, a node id a slot on the CPU (4 bytes), and per run a node item (4 bytes) a node and its count and dispatch. On the forest's 375,000 instances in about 5,000 tiles: about 10,000 nodes, 320 KB a set, under 1 MB with three sets and every run's items. At the cap with one-cluster instances (16.7 M slots, 262,144 nodes) it is 8.4 MB a set and 6.3 MB of node items for six runs, beside the 41 MB the instance pass's items already hold there ([what it costs](../subsystems/renderer.md#the-cull-culls-instances-first)).

### What stays byte-identical

The culled set — the drawn pairs by draw, the casters, the shadow pairs, the built structures — and so every picture: a node rejects only what each of its instances would have rejected (above), and every survivor's instances go on through the instance pass and the pair pass unchanged. The pair numbering, the tie rule and the ids are untouched; only the visible list's order within a run, which nothing drawn may depend on, changes.

## Below the instance: the survivors' DAGs

Traced, a surviving rock spends 64 lanes on 51 clusters to draw one or two. That is Nanite's "orders of magnitude more clusters that are too detailed", and its answer — a node over a mesh's clusters holding the largest parent error, rejected when even that is precise enough — applies here one level deep: an **item's** LOD sphere and largest parent error, measured per mesh like the meshes' spheres, and an item rejected when every cluster of it is already finer than the cut needs. It is not this row's design: the cut is the cross-fade's band and the occlusion-aware threshold's scale as well as `lod_selects`, so the item's test has to be the least threshold any of them can give, and a mistake changes the cut and the picture. It is proposed as a row of its own (unnumbered, below), measured by stage 0 first. Beside it, smaller: **items packed across instances** — the forest's median instance has 16 pairs, so its 32-lane item runs half empty.

## Stages

| Stage | What | Cost | Measured by | Must keep / pins it |
|---|---|---|---|---|
| 0 | **Measure the split**: the instance pass and its fold timed as zones of their own (`cull_instances`, `shadow_cull_instances`) and each run's survivors and items read back into the frame record. No picture change. | S | The grid's three scenes and the forest's cut, traced and with the cascades, on the base: `engine-view --offscreen --warmup 60 --frames 400 --width 3840 --height 2160 --scene <scratch>\b68\R173\scene\g16m.json --camera-path <scratch>\b68\R173\grid-spin.json --raster rt --shadows rt --benchmark <out>.jsonl` and the same with `--raster hw --shadows csm`, g1m and g8m alike; the cut, `--scene <scratch>\b68\T172\scene-cut-drop.json` with T172's `D1-main` flags, one lease each | Every existing case; the frame record's new fields in `apps.md` |
| 1 | **Nodes**: `NodeLayout` beside `TileLayout`, the node table a set, the measure, the node pass, the instance pass from node items, the guard; `renderer.cull.nodes`, off until its pair is flown, then on | M | Stage 0's flights, this build with `renderer.cull.nodes` false and true, alternated three times in one lease; the cull's and the shadow cull's medians and p99, the culled set compared frame by frame. Estimated (not measured) at the cap: most of the cascades' 0.256 ms, part of the camera's two instance passes, traced a few hundredths at most | `dynamic instances: the cull draws the same with instances culled first and not`, run with nodes on and off; the off-axis dumbbell case with its two cubes as two instances of one node; **new**: a node whose instances span two 64 m cells, the scene moved by whole cells draws the same bytes; a moved instance carried out of its node's old sphere is drawn on the next frame (the moving-instances case with nodes on); a ring's chunk shown and hidden; the guard's count zero in all of them |
| 2 | **The second occlusion pass retests** only the first's survivors and its Hi-Z rejections | S | The hw/csm grid and forest pairs as stage 1 | `flythrough: occlusion culling changes no pixel on the path, and culls what is hidden`; the occlusion test's identical buffers |
| 3 | **The near band at the instance and the node** (`handoff_outside` on their spheres) | S | A scene with near roles and `world.placements.handoff_cell_m=0` (G205's ridge, its tail of 5.96 M pairs whole) | `handoff_tests.cpp`; the cells' picture against whole, as G205 compared it |

**Proposed rows, unnumbered**: the item's LOD bound below the instance (M, after stage 0, the traced remainder); items packed across instances (S); the measure narrowed to the nodes holding a written mesh's instances (S, if stage 1's flights show the full measure on ring frames).

## What it declines

- **A deep hierarchy** (a BVH over instances, a quad tree of nodes): every source keeps one coarse level and then flat instances, and at the forest's 10,000 nodes a node pass is a few microseconds; a second level waits for a scene of millions of instances, which only one-cluster kits reach under the cap.
- **The world's boxes as nodes**: general purpose and exactness, above.
- **Culling on the CPU** (Haar's quad tree, Wihlidal's "coarse cull on CPU"): the views, the cascades and the caster volumes are on the GPU and the frame's thread is the one the walk stalls on; the node pass costs the GPU microseconds.
- **Moving the cap or the pair numbering**: the cap is the owner's, and nodes are over slots, not pairs.

## Open questions for the owner

1. **Whether stage 1 goes on by default where it buys little.** Traced at 16:9, the estimate is a few hundredths of a millisecond at the cap; on the owner's surround (11520 × 2160) the frustum keeps more and a node less. One code path for every mode is simpler to keep right. *Leaning: on everywhere once its pair holds the set to the frame.*
2. **Whether the next row is below the instance**, the cut's own hierarchy, which the traced remainder points at and which can change the picture if it is wrong. *Leaning: yes, after stage 0 says the items are the larger part, with `lod_selects` and the cross-fade pinned against it.*
3. **The node's size**: 64 slots a block's run, one workgroup. A cell of 4 m is often one node of a few instances. *Leaning: 64, revisited only if stage 1's node counts say so.*

## Sources

- Ulrich Haar and Sebastian Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015 Advances in Real-Time Rendering in Games course (advances.realtimerendering.com, the 2015 deck): read whole as extracted text; the pipeline overview, the two-phase occlusion and the benchmark slides.
- Graham Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016: read in a hosted transcript of the slides (not the author's PDF, which did not download); the culling overview, cluster culling and the measured slides.
- Brian Karis, Rune Stubbe and Graham Wihlidal, "Nanite: A Deep Dive", SIGGRAPH 2021 Advances (advances.realtimerendering.com, the 2021 deck): read as extracted text with its notes; the GPU-driven pipeline, sub-linear scaling, hierarchical LOD selection and culling, two-pass occlusion and the culling dataflow.
- Unreal Engine 5.4.4's console variables `r.SceneCulling.MinCellSize` and `r.Nanite.UseSceneInstanceHierarchy`, read in a third-party site reproducing their help text and source excerpts; Epic's own documentation of the structure **not read** (none found).
- NVIDIA `vk_lod_clusters`, `traversal_init.comp.glsl`: as read by R173 ([cull-by-instance](cull-by-instance.md)); not re-read.

## Stage 1 as built

*2026-11-19, batch 70 (b70-R188-1).* Built as staged above, behind `renderer.cull.nodes` (off), with these departures, each because it keeps the GPU scene and the tile layout unaware of nodes or the picture exact with less to keep in step:

- **The CPU says only where a node's slots are.** Its live count, its largest cascade policy, its terrain level and its anchor are measured on the GPU with its sphere (`cull_node_measure_main`), from the same instances the instance pass reads, so no CPU field can disagree with the table. The near-role bit is left to stage 3.
- **The layout is the renderer's, read from the scene's public surface** (`NodeLayout`): the prefix cut once from its records, the tail compared after each change with `TileLayout::blocks` by first slot and count. A block kept or rewritten with the same count keeps its ids; one moved by T164 or laid out again gets new ones, since its slots moved. No hunk in `gpu_scene.cpp` or `tile_layout.cpp`.
- **What a set measures is read from what the frame copies into it** (`TableUpdate::copies`, the record upload's instance regions) and the ids the layout changed since that set was last current, rather than node ids listed into each set as the runs are; a carried set, a growth of the node table, the meshes' spheres measured again, and the nodes switched on measure every node.
- **A tail block's run is not cut by terrain level**: a node whose live instances are on two levels is never culled (`k_cull_node_mixed_terrain`), which keeps it exact; the prefix is cut by level as designed.
- **Stage 0's zones are not built**, so the node passes are timed inside the cull's zones with the instance pass, as R16's were.