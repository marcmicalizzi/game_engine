#pragma once

// A document's placed meshes as a renderer scene (docs/subsystems/doc_scene.md): the bridge from
// `doc::Document` to `renderer::SceneDesc`, and from a transaction to the instances it moved.
//
// **Why a module of its own.** The renderer reads scenes and must not depend on the document model
// (schemas/scene.schema, the header), and the document must not know the renderer; and the bridge
// is not an app's, because every host that draws a document needs the same one — the editor's
// viewport today, engine-host's `render.load` of a session tomorrow (06 §6.13: a feature only the
// GUI can reach is a protocol bug). So it sits above both, in `systems/`, and holds nothing but the
// mapping: one live `engine.scene.MeshPlacement` record is one instance, its `mesh` path one of the
// scene's meshes (shared by every record naming the same path), its `translation`, `rotation`,
// `yaw_deg` and `scale` placed exactly as a scene file's `Instance` is.
//
// **What a change costs.** `changes_since` reads the document's change feed from the revision the
// scene was read at: a placement whose transform changed is an instance to move
// (`renderer::SceneRenderer::move_instances`, no reload); a placement created, deleted, retyped or
// pointed at another mesh means the scene's mesh table or its pairs change, which is a rebuild.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <domain/doc/document.h>
#include <systems/renderer/scene.h>

#include <string>
#include <string_view>

namespace engine::doc_scene {

// The record type the viewport draws (schemas/scene.schema).
inline constexpr std::string_view k_mesh_placement = "engine.scene.MeshPlacement";

struct DocumentScene {
  // The renderer's description: one mesh per distinct path in the order the records first name
  // them (resolved against the document's directory), one instance per placement in id order. A
  // document with no placements is described as the procedural ground alone (one empty mesh path),
  // whose instance is no record's, so the viewport still has something to draw and nothing to pick.
  renderer::SceneDesc desc;
  // Instance i's record; the null id for the stand-in ground's.
  Vector<Id128> records;
  // The paths as the records spell them, parallel to `desc.meshes`.
  Vector<std::string> mesh_paths;
  // The document's revision when it was read, where `changes_since` starts.
  u64 revision = 0;

  // The instance a record is, or ~0 when it is none.
  u32 instance_of(Id128 id) const noexcept;
};

// Reads every live placement of `document` into `out`. `base_dir` is what relative mesh paths are
// relative to: the document's directory. False, with `error` naming the record, for a placement
// whose properties do not read as the schema type or whose mesh is empty.
bool read_document_scene(const doc::Document& document, const std::string& base_dir,
                         DocumentScene& out, std::string& error);

// One placement record's properties as an instance of `scene`'s meshes: its mesh index found by
// path. False, with `error`, when the record is not a live placement, does not read, or names a
// mesh `scene` does not have (which is a rebuild, not a move).
bool placement_instance(const doc::Document& document, Id128 id, const DocumentScene& scene,
                        renderer::SceneInstance& out, std::string& error);

// What the document's changes since `scene.revision` ask of the viewport.
struct SceneChanges {
  Vector<u32> moved;                           // instances to place again
  Vector<renderer::SceneInstance> placements;  // parallel to `moved`
  bool rebuild = false;                        // the mesh table or the instance set changed
};

// Reads the change feed from `scene.revision` and advances it to the document's. A record of
// another type is not the viewport's and is ignored. `rebuild` also when the feed no longer reaches
// back that far (`doc::Document::changed_since` false).
bool changes_since(const doc::Document& document, DocumentScene& scene, SceneChanges& out,
                   std::string& error);

}  // namespace engine::doc_scene
