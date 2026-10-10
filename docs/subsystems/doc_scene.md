# doc_scene: a document's placements as a renderer scene (systems)

**Purpose.** The bridge from a `doc::Document` to what the renderer draws, and from a transaction to the instances it moved. It is what engine-editor's viewport is built on ([apps](apps.md#engine-editor-the-editors-first-slice)), and what engine-host's `render.load` of a session will be built on when the protocol grows one.

**Why a module of its own, above both sides.** The renderer reads scenes and must not depend on the document model — its journal, undo and ids ([scene.schema](../../schemas/scene.schema), the header) — and the document model knows nothing of drawing. Nor is the bridge an app's: every host that draws a document needs the same mapping, and a feature only the editor's GUI could reach would be a protocol bug ([06 §6.13](../plan/06-agent-tooling.md#613-human-developer-tooling)). So it sits in `systems/`, depends on `doc` and `renderer`, and holds nothing but the mapping. It is not a capability: it is the engine's one mapping from placement records to instances, with no switch.

## The record type

`engine.scene.MeshPlacement` (`schemas/scene.schema`, `@kind(record)`): `name`, `mesh` (a glTF, GLB or `.clusters` path, relative to the document's directory or absolute), `translation` (`worldpos`, metres), `rotation` (`quat`), `yaw_deg`, `scale` (`vec3?`). It is the scene file's `Instance` as a document record — the "move to documents is an export" that file's header promised — with one difference a document forces: it has no table of meshes to index, so the mesh is named by its path. There was no record type that referred to a mesh before it; `engine.world.Node` and the capabilities' records place things but draw nothing.

## The mapping

- **One live placement is one instance.** `read_document_scene` walks `Document::objects()`, which is sorted by id, so the instance order — and with it every pair and every visibility id — is a function of the document's content alone, never of the order records were made in.
- **One distinct path is one mesh**, in the order the records first name it; every record naming it shares it, so a hundred crates are one mesh's clusters.
- **Placed exactly as a scene file's `Instance`** (`renderer/src/scene.cpp`): the world position whole in `SceneInstance::origin` (ADR-0053: no float on the way to the cell), `yaw_deg` about +y after `rotation`, `scale` when given. A record's unknown properties are left alone — the document validates its records against their types (`Document::validate`), and a viewport that refused to draw a record over a property it does not read would hide the record, not the problem. A placement whose properties do not read as the type, or that names no mesh, is refused with the record's id.
- **A document with no placements** is described as the procedural ground alone, whose instance is no record's (`records[i]` null): the viewport still draws, and a pick finds nothing.

## Changes

`changes_since` reads the document's change feed (`Document::changed_since`) from the revision the scene was read at and advances it:

| Change | What the viewport does |
|---|---|
| a placement's `translation`, `rotation`, `yaw_deg` or `scale` | **move** the instance in place (`SceneRenderer::move_instances`): its pairs, mesh and material stay, the next frame draws it where it now stands |
| a placement created, deleted, retyped, or given another `mesh` | **rebuild**: the mesh table or the instance set changed, and with it the pairs |
| a record of any other type | nothing |
| the feed no longer reaches back that far | rebuild |

A rebuild reloads through the derived-data cache, so its cost is the GPU scene's upload, not a content build.

## Public API

`systems/doc_scene/doc_scene.h`: `k_mesh_placement`, `DocumentScene` (`desc`, `records`, `mesh_paths`, `revision`, `instance_of`), `read_document_scene`, `placement_instance`, `SceneChanges`, `changes_since`.

## Testing

`systems/doc_scene/tests/doc_scene_tests.cpp`, CPU only, on a document built in memory: placements in id order with a `Node` between them ignored, meshes shared by path and resolved against the document's directory (an absolute path kept), placement as `Instance` (the origin in f64 420 km out, the yaw, the scale); a translation is a move and the description follows it, another type's edit is nothing, a new mesh, a new placement and a deletion are rebuilds; a document with no placements is the stand-in ground with nothing to pick; a placement with no mesh is refused naming the record. The GPU half — picking and moving through a real frame — is engine-editor's test.

## Not yet

- **`render.load` of a session.** engine-host's `render.load` takes a mesh, a scene file or a procedural name; taking `{"session": ...}` through this module, with `render.capture`'s ids channel answering in record ids, is what makes the editor's viewport reachable over the protocol. Not built in the first slice.
- **Records that place other things**: a scatter, a ruin, a terrain as document records are the rest of the scene file's types; only a placed mesh is a record today.
- **Hierarchy**: a placement's document parent does not transform it (as `Transform` does not propagate through `ChildOf`, [world.schema](../../schemas/world.schema)); positions are world positions.
- **A dynamic scene's tail.** Moves go through `move_instances`, which needs a scene read whole; a streamed world's instances would move through `set_dynamic_instances` instead.
