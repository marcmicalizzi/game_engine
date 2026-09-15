#pragma once

// On-disk form of a Document (docs/plan/03-data-model.md §3.2, ADR-0002):
//
//     <dir>/manifest.json      DocumentManifest: name, layer stack, edit layer, undo position
//     <dir>/layers/<name>.json one canonical LayerFile per layer
//     <dir>/journal.jsonl      one Patch per line, append-only until the redo tail is dropped
//
// Layer files always hold the current state, so a checkout is readable without replaying the
// journal, and the journal plus `undo_position` make undo and redo survive across processes:
// a CLI invocation can undo what a previous invocation committed. Paths go through io::Vfs,
// so a document may live under a mount ("content://levels/a") or at a native path.

#include <domain/doc/document.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::doc {

class DocumentStore {
 public:
  static constexpr std::string_view k_manifest_file = "manifest.json";
  static constexpr std::string_view k_layers_dir = "layers";
  static constexpr std::string_view k_journal_file = "journal.jsonl";
  static constexpr std::string_view k_default_layer = "base";

  static bool exists(const io::Vfs& vfs, std::string_view dir);

  // Creates `dir` with one Base layer named "base" and an empty journal. Fails when a manifest
  // is already there.
  static bool create(const io::Vfs& vfs, std::string_view dir, std::string_view name, Document& out,
                     DocumentManifest& manifest, std::string* error);
  // Loads the manifest, every layer in manifest order, and the journal.
  static bool load(const io::Vfs& vfs, std::string_view dir, Document& out,
                   DocumentManifest& manifest, std::string* error);
  // Writes every layer file and the manifest, each atomically. Refreshes `manifest.layers`
  // and `manifest.edit_layer` from the document; keeps its name and undo position.
  static bool save(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                   DocumentManifest& manifest, std::string* error);

  static bool append_journal(const io::Vfs& vfs, std::string_view dir, const Patch& patch,
                             std::string* error);
  // Rewrites the whole journal (after the redo tail was dropped).
  static bool write_journal(const io::Vfs& vfs, std::string_view dir,
                            std::span<const Patch> patches, std::string* error);
  static bool load_journal(const io::Vfs& vfs, std::string_view dir, Vector<Patch>& out,
                           std::string* error);

  // "<sanitized name>.json": characters outside [A-Za-z0-9_.-] become '_'.
  static std::string layer_file_name(std::string_view layer_name);
  static std::string path_of(std::string_view dir, std::string_view file);
};

}  // namespace engine::doc
