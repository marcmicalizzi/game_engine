# water (systems)

**Purpose.** Water surfaces in a scene — the oases of Desert Survival first ([plan 13 §13.1](../plan/13-reference-consumer-games.md#oases), "water with reflection and refraction"; roadmap S63, P8) and the "Fluids and water surfaces" row of the capability inventory ([plan 05 §5.15](../plan/05-simulation.md#515-capability-inventory)). A scene's `water` block (`engine.scene.WaterBody`) names bodies — a circle or a star-shaped polygon at a level — and each becomes **an opaque mesh with a water material**, drawn by the rasterizers like any instance and shaded by the material resolve's water branch; a walker wades in it and stops at the chest. It deliberately does not yet refract what stands under the surface, let anyone swim, float anything, or run a ripple simulation: that is stage 2, roadmap S86.

**Why this shape: opaque with a branch, not a forward pass.** The renderer is a visibility buffer: every surface is a triangle id in one 64-bit word per pixel, shaded once by the resolve, and the plan's forward and transparent pass (plan 04 §4.3 step 7) is not built. A water surface that is transparent needs that pass, a second depth, and an order — weeks of renderer for one material. But what the eye sees of a shallow pool is three things the resolve can evaluate at the pixel with what it already has: the **sky** reflected along the view (the sky's model is a function of a direction, `sky.slang`), the **sun's glitter** (the GGX lobe the resolve already evaluates, at a low roughness on a wave normal), and the **bottom's colour fading with depth** (which a vertex attribute can carry: the depth under every vertex is known when the mesh is built). So a body is a flat mesh in the visibility buffer, and a material class tells the resolve to shade it as water. Everything downstream is unchanged: the cull, the four rasterizers, the occlusion, the shadows (the surface receives the sun's shadow as any surface does; it also casts one, onto the ground under itself, which nobody sees), the LOD DAG and the ray path. What it costs is what opaque water cannot show — the rocks and the bottom's own texture under the surface, seen bent through it — which is refraction and needs the forward pass; S86 rows it.

**Why a capability, and how it reaches the renderer.** The renderer is not a capability and the water is (ADR-0027), so the renderer may not call it. The registration point is the scene-generator registry (ADR-0046): the capability registers the **placement generator `"water"`**, and the renderer's scene reader (`systems/renderer/src/scene_water.cpp`, one call in `read_scene_file`, after the placement entries and before the terrain, which stays the last mesh) hands it the block as `{"bodies": [...]}`. Its `expand` returns one mesh and one placement per body, in the block's order — mesh *k* and instance *k* are body *k*, the one contract the reader relies on. A build without the capability (`ENGINE_WITH_WATER=OFF`, `msvc-minimal`) refuses a scene with a `water` block with a sentence naming the switch; a scene without the block is read exactly as before, byte for byte. The **material** is the renderer's and gfx's, not the capability's: `gfx::k_material_water` is a class of the material table, `gfx::set_water_material` packs a body's numbers into it, and the resolve's branch is `domain/gfx/shaders/water.slang`.

**Owned data.** A body as read and checked (`water::Body`, `read_body`); its mesh (`build_mesh`) and the GLB file it is written to; the depth at a point (`depth_at`). Nothing at run time: no state per tick, no GPU resource.

## The block

`engine.scene.WaterBody` (schemas/scene.schema, `Scene.water`, scene version 8):

| Field | Default | Meaning |
|---|---|---|
| `name` | | what the mesh is called (`water/<name>`) |
| `level_m` | 0 | the surface's height, a world y (f64) |
| `shape` | | exactly one of `circle: {center, radius_m}` and `polygon: [worldpos...]` (x and z read); a polygon must be star-shaped from its centroid, in either winding |
| `shore_m` | 2 | how far in from the edge the surface fades from the bottom alone to reflecting |
| `shallow`, `deep` | pale green-blue, dark blue-green | linear RGB over a shallow bottom and where the bottom is gone |
| `depth_fade_m` | 3 | the depth at which the bottom has faded to `deep` |
| `wave_amplitude_m`, `wave_length_m` | 0.02, 2.5 | the largest wave; 0 amplitude is a mirror |
| `chop` | 0.5 | how far the shorter waves turn from the main direction, [0, 1] |
| `wind_follow` | true | the waves travel along the ground's wind (`GroundProvider::wind` at the scene's time) when it has one |
| `direction_deg` | 0 | otherwise, about +y from +x towards +z |
| `roughness` | 0.08 | the surface's GGX roughness, [0.02, 1] |
| `seed` | 1 | the waves' phases |

Checked when the scene is read; a body that cannot be drawn (no shape or both, a radius of 0, a polygon of two points or not star-shaped, a roughness or chop out of range) refuses the file with a sentence naming the body.

## The mesh

**Rings to a fan.** The outline is sampled every `spacing` metres along its edge — a circle at angles from a polynomial sine of the module's own (`water::sin_cos`, + − × ÷ only, so the points are the same bits on every toolchain), a polygon along each edge — and copies of that edge scaled towards the centre stand `spacing` apart along the ray through each sample, closed by a fan at the centre. Every ring has the edge's vertex count, so the strips between rings are quads with no stitching, and a vertex's distance from the edge is `(1 - t) |b|`: exact for a circle, and for a star-shaped polygon the distance along the ray from the centre, which is what the shore fade is drawn by. A grid clipped to the outline was the alternative; it puts slivers or snapped vertices along the shore, which is exactly where the fade needs clean triangles. The rings' cost is density near the centre (every ring has the edge's count), which the LOD DAG simplifies away.

**Spacing and budget.** `spacing = clamp(shore_m / 3, 0.25, 1)` m: three vertices across the shore fade. **A 20 m pool** with the default shore: 0.667 m, 95 samples a ring, 15 rings, **1,426 vertices and 2,755 triangles**; the oasis pool (shore 1.5 m, 0.5 m): 126 a ring, 20 rings, 2,521 vertices, 4,914 triangles, 111 clusters. A body whose mesh would pass `k_max_vertices` (65,536) has its spacing grown by a quarter until it fits — a 4 km lake is 45,136 vertices at 23.7 m — which the fade then draws coarser; no limit is hidden.

**What a vertex carries.** Positions are metres from the body's centre at height 0; the instance stands at the centre and the level (`SceneInstance::origin`, ADR-0053), unrotated. The normal is +y and every triangle is wound to face it (the mesh is built with normal cones on: a pool faces up). **The UV is (the distance from the edge, the depth under the vertex)**, metres: the depth is the level less the ground's surface under the vertex (`scene_gen::Ground::surface`, the provider's heights at the whole millimetre), clamped at zero where the ground stands above the water — the shore. The UV, not a vertex colour, because the glTF import carries `TEXCOORD_0` and no colours (AGENTS.md, glTF import), and the half-float UV holds a depth of a few metres to a millimetre.

**Stored through the derived-data cache.** The mesh is written as a GLB (positions, normals, `TEXCOORD_0`, one material named `water` whose base colour is `shallow`) under `water-<hash of its bytes>.glb` in `<repo>/ddc/water` (found from the scene's directory, as the scatter capability's rocks are), or `water/` beside a scene outside any checkout; a file already there under its name is the right one, and the renderer's cluster cache builds and keys the `.clusters` from it like any mesh. The name is the bytes' hash, so it covers the body's parameters, the ground under it and `k_mesh_version` alike. **The depth is the load's**: a body over migrating dunes keeps the depths of the moment the scene was read (the moving ground's `stand`, when a host has one) until it is read again.

**LOD.** The mesh is cluster geometry, so the DAG applies: far away a pool is a few clusters of large flat triangles, whose interpolated depth is coarser — the fade is the near field's.

## Shading

*(Stage 1, this commit: the material class and its packing; the surface draws its shallow colour as a plain dielectric. The branch below is stage 2.)*

The material entry (`gfx::set_water_material`): `albedo` = shallow, roughness; `emissive` = 0; `uv_transform` = deep, `depth_fade_m`; `uv_offset` = amplitude, wavelength, chop, `shore_m`; `normal_scale`, `occlusion_strength` = the waves' direction (x, z); `sampler` = the seed; `flags` = `k_material_water`. Those are the slots an untextured material with no UV transform never reads, so every reader that is not the branch — the reference path tracer, the albedo view — sees an untextured dielectric of the shallow colour, the UVs passing through unscaled.

## Wading

*(Stage 3.)* The water's mesh is not a collision surface: `scene_collision` gives no body to a mesh whose `renderer::SceneMeshInfo::water` is set (it keys meshes by index, and this is the mark it reads, as it reads `terrain_mesh`), so the walker stands on the ground under the surface.

## Determinism

The mesh is a function of the block and the ground in f32/f64 + − × ÷ and sqrt (the GLB bytes are pinned by `water_tests.cpp`'s hash, taken on MSVC). The waves will be a function of the body-local point and the frame's clock. `depth_at` is a function of the position and the scene.

**Public API.** `include/systems/water/water.h`.

**Depends on.** `base`, `containers`, `math`, `hash`, `json`, `schema`, `schemas`, `io`, `log`, `geometry`, `scene_gen`. No renderer.

**Testing.** `tools/dev.ps1 test -Filter water`. `tests/water_tests.cpp`, no device: the sine against the C library's to 1e-14; the block read and every refusal; inside, edge and outside for a circle and a polygon; `depth_at` inside, outside, at the shore and over two bodies; the mesh over a synthetic bowl — vertex and triangle counts, every normal up and every triangle facing +y, every vertex's depth the level less the ground to the bit, its edge distance, the edge ring dry; a 20 m pool's budget and a lake's grown spacing; a polygon's edge on its outline; the GLB as the importer reads it, the same bytes twice and a pinned hash; the generator through the registry; the material's packing. `tests/water_scene_tests.cpp`: a scene's block read by the renderer (one mesh and one instance a body before the terrain, the body and its shading on the mesh's info, a fixed direction, the load carrying it), a scene without the block and with an empty one the scene it was, a polygon body, a refusal's sentence; the oasis pool's ground (the level under every crest 25 m out and under the whole edge, two metres deep at the centre); and on a GPU the oasis pool captured with and without the block — **every differing pixel is a pixel of the pool** (its instance in the ids channel), the same frame twice the same bytes, and two hours of sun on changes the pool's pixels.

**Performance notes.** Nothing per frame on the CPU. The mesh is built once per scene read (a 20 m pool: 1,426 ground heights).

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone; its tests link the terrain, sky and scatter capabilities for the oasis scene |
| Component and event types | none | a body is scene data (`engine.scene.WaterBody`, the scene format's), not entity state |
| Tick scheduler entry | none | nothing ticks: the waves are a function of the frame's clock, drawn by the resolve |
| Render-graph passes | none | the surface is an instance; its shading is a material class of the resolve |
| Content-build derived step | the GLB per body, clustered by the renderer's cache | done |
| Scene generator | placement generator `"water"` | done |
| Protocol methods | none | not needed |
| Tunables | none | every number is the body's |
| LOD policy | the cluster DAG of its mesh | done |
| Determinism | derived: drawn, and `depth_at` a function of position | done |
| Zero cost when unused | a scene with no block calls nothing; no material carries the flag | done |
| Tests and size table | `tests/water_tests.cpp`, `tests/water_scene_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | none: no hot path on the CPU | |
| Removal proof | `ENGINE_WITH_WATER`, off in the minimal build | works: a block is refused with a sentence |

**Removing it.** `cmake --preset msvc-minimal` (or `-DENGINE_WITH_WATER=OFF`) drops the module and its tests; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else builds and passes, and a scene with a `water` block is refused.

## Not yet

Refraction of what is under the surface (the forward pass, plan 04 §4.3 step 7), swimming, buoyancy (a fluid volume and a submerged-volume query), a ripple map stepped by a wave equation, the moon's and the point lights' glitter, depths that follow a moving ground, polygons that are not star-shaped: roadmap S86.
