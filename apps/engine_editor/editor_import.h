#pragma once

// engine-editor's asset import, the file half (docs/subsystems/apps.md, "Adding assets"; A39): a
// mesh file — a glTF, a GLB or a built `.clusters` container — and, for a `.gltf`, the buffers and
// images it names beside it, copied into the document's own `assets/<stem>/` so the document names
// its meshes by paths relative to itself (`engine.scene.MeshPlacement.mesh`, doc_scene.md) and a
// document directory moved or committed carries what it draws. `editor.import_mesh` then creates
// the placement in one transaction (editor_methods.cpp); nothing here touches a document.
//
// **Every check comes before the first byte is written**: the extension, the file, the loader's
// own verdict on it (`assets::load_gltf`, `geometry::read_cluster_file`), every file a `.gltf`
// names, and every destination — so a refusal leaves the document's directory as it was. A
// destination that already holds the same bytes is reused (importing a file twice copies it
// once); one that holds other bytes is refused, never overwritten.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>

#include <string>

namespace engine::editor {

struct ImportedFiles {
  std::string mesh;            // what the record's `mesh` names: "assets/<stem>/<file>"
  std::string stem;            // the file's name without its extension: the record's default name
  Vector<std::string> copied;  // written, relative to the document's directory
  Vector<std::string> reused;  // already there with the same bytes
};

// Copies `source` (and a `.gltf`'s external buffers and images) under `doc_dir`/assets/<stem>/.
// False, with the reason in `error` and nothing written, when the file is missing, is not a
// `.gltf`, `.glb` or `.clusters`, is refused by its loader, names a file it does not have or one
// outside its own directory, or would overwrite a different file.
bool import_mesh_files(const std::string& source, const std::string& doc_dir, ImportedFiles& out,
                       std::string& error);

// Removes what `import_mesh_files` copied (not what it reused): a transaction that was refused
// after the copy leaves no files behind either.
void remove_imported(const std::string& doc_dir, const ImportedFiles& files) noexcept;

// Where an import with no `at` stands: where the eye's line of sight from `eye` towards `target`
// meets the ground plane y = 0 within 50 m, and otherwise 10 m along it.
WorldPos default_import_position(WorldPos eye, WorldPos target) noexcept;

}  // namespace engine::editor
