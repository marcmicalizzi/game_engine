# ADR-0061: A tile's meshes arrive with the tile, and are resident by reference

- **Status:** Proposed
- **Date:** 2026-11-10
- **Plan references:** docs/plan/04-renderer.md §4.3 (geometry residency), docs/plan/02-architecture.md §2.8 (capabilities attach through registration points)
- **Docs touched:** `docs/subsystems/scene_gen.md` ("A tile's meshes", "The formation and the ground before its cuts"), `docs/subsystems/world.md` ("A tile's meshes: residency by reference"), `docs/subsystems/renderer.md` ("Meshes that come and go")

## Context

A placement generator (ADR-0046) had one way to give a streamed scene a mesh: list it in `meshes`, read once when the scene is, resident for the run. `GpuScene` said the same from its side: a scene is a fixed prefix and a tail the caller replaces between frames, and "a tail is instances and nothing else". That was right for kits (the ruins' blocks, a sward's patches) and wrong for everything whose output is a tile's own. On 2026-11-10 four solver rows met it in one day: the ledges solved every tile of a region at `open` and kept every mesh resident (about 3.2 s and 94 KB a tile for a square kilometre); the deposits moved their solve behind the mesh generator registry but still wrote every recipe at `open`; the walls could not hand a merged mesh a tile and became a kit of 39 modules, 29 MiB an entry, with visible repetition; the channel bed has no cobbles. The forest proof of concept loads 7,124 mesh records and 5,481 textures at start, 10.3 s, resident for the run, and asked for the same thing in two sentences: a solver's output should not have to exist before its tile does, and a mesh should come in with the first tile that names it and leave with the last.

The alternatives on the table:

1. **Keep `meshes`, make `open` cheaper** (plan a region lazily, write recipes lazily). It moves the cost and keeps the residency: every mesh of every tile ever visited stays, and `open` still has to enumerate.
2. **Per-mesh device buffers**, allocated when a mesh arrives. Every reader of geometry then needs a buffer address per mesh: `gfx::MeshDesc` is full at 64 bytes and read per position, `gfx::ClusterDrawParams` is full at its 128-byte push limit, and the ray tracing chain's build records address one vertex buffer.
3. **Relocation**: grow the scene's buffers and move everything when a mesh does not fit. A wait for the device and a copy of all resident geometry, on the frame that wanted a tile.
4. **Reserved room in the scene's own buffers**, as the terrain rings' slots already have, with the residency decided above the renderer.

## Decision

1. **A tile hands its meshes.** `scene_gen::PlacementGeneratorDesc::tile` fills `Placements::meshes` with the meshes its placements name beyond the generator's resident list; a placement indexes the resident meshes first and then the tile's. A mesh a solver makes is a **recipe** (`PlacementMesh::recipe`): the bytes of the mesh generator's file its path names, built by the host through the mesh generator registry and the derived-data cache, keyed by the generator's `source_hash` of them. `tile` builds nothing. `expand` lists everything, recipes included, and a scene read whole is the same scene.
2. **`meshes` stays, narrowed** to what must be resident from the start (a kit most tiles name). It may be null. A generator says with `tile_meshes` that its tiles hand meshes, which is what makes a host reserve room.
3. **Residency is by reference, above the renderer** (`world::TileMeshes`): an entry per distinct mesh, built when the first held tile names it — on the job system, never in `tile` — resident while any held tile names it, retired after the last, after a linger counted in commits. An entry takes a mesh slot when first named, so an instance names its scene mesh before the mesh is built and nothing is renumbered.
4. **The GPU scene keeps room** (`GpuScene::MeshRoom`, alternative 4): mesh records, a cluster arena, and the vertices and triangles the arena owns at a cluster's worst case each, plus a material arena, in the scene's one set of buffers, sized by `renderer.meshes.budget_mib`. Admission is staged copies the next frame records after every frame in flight; retirement frees the runs at once. Nothing waits for the device.
5. **A tile waits for its meshes**, drawing what it drew before, and the renderer refuses a tail that names a slot with no mesh admitted. The room's refusal withholds the farthest tiles first: a nearer waiting tile takes the room of the farthest drawn one.
6. **Nothing drawn depends on the order meshes arrived in.** An id names (instance, cluster of its mesh, triangle); a mesh's slot and its place in the arena are in no id; a mesh's bytes are its recipe's.

## Consequences

- A solver can be written as "plan above the tile, realise in it": `open` plans, `tile` hands a recipe, and a region nobody is near costs its entry. Ledges, deposits, walls and the channel bed each have the steps to move in R142's hand-back.
- Room is device memory held whether or not a mesh is in it. That is the price of alternative 4 and why it is a budget with a refusal, and why a scene with no such generator reserves none.
- The arena's layout by clusters wastes the slack of clusters that are not full; the budget counts the worst case, so a refusal is a statement about clusters alone.
- **Not carried by an admitted mesh yet**: textures (it draws its materials' constants), coverage errors, a motion contract, cluster templates, static structures, the resident cluster cache. A paged scene has no room. The collision does not yet build proxies for a tile's meshes. Each is a row.
- *2026-11-11 (R145):* the collision now builds proxies for a tile's meshes, keyed by identity and resident by its own tiles (`docs/subsystems/scene_collision.md`, "A tile's meshes"); textures are designed and not built (`docs/subsystems/texture.md`, "Textures that arrive after the load"). The rest of the list above stands.
- *2026-11-11 (R147):* an admitted mesh now carries its textures — one table by `.tex` build key shared with the load's, references by mesh, uploads without a wait inside the commit's byte allowance, the mesh waiting for them — and its alpha-tested materials draw their coverage where a generator declared them (`tile_meshes_masked`) (`docs/subsystems/texture.md`, "Textures that arrive after the load"). Still not carried: coverage errors, a motion contract, cluster templates, static structures, the resident cluster cache, and for an alpha-tested mesh the shadow proxy's coverage table and the opacity micromaps.
- A generator's recipe must be a function of the plan and the tile, and one recipe must go under one path; two recipes under one path are refused in a whole read.
- Forbidden from here: solving a tile's mesh in `open` to list it in `meshes`, and writing a recipe to disk for the host to read back.

## Revisit when

- Textures have a residency of their own: then a kit's members can be handed by reference too and `meshes` can go.
- A scene wants more admitted geometry than a reserved arena can hold on the smallest supported card (the room is a fixed fraction of 12 GB there): then the page pool, which already streams by cluster page under a budget, should carry admitted meshes as pages, and the arena's worst-case layout goes with it.
- The ray tracing chain keeps resident structures for instanced meshes (R143): an admitted mesh should take one on admission and give it back on retirement.
