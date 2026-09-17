#pragma once

// On-disk form of a Document (docs/plan/03-data-model.md §3.2, §3.7, ADR-0002):
//
//     <dir>/manifest.json      DocumentManifest: name, layer stack, edit layer, undo position
//     <dir>/journal.jsonl      one Patch per line, append-only until the redo tail is dropped
//     <dir>/layers/<name>.json one canonical LayerFile, for a layer with no partition
//     <dir>/layers/<name>/     for a layer with one: index.json, tiles/<x>_<y>.json, untiled.json
//
// Layer files always hold the current state, so a checkout is readable without replaying the
// journal, and the journal plus `undo_position` make undo and redo survive across processes:
// a CLI invocation can undo what a previous invocation committed. Paths go through io::Vfs,
// so a document may live under a mount ("content://levels/a") or at a native path.
//
// A layer's form is its own (`Layer::partition`, persisted in the manifest), so the two live
// side by side in one document and `repartition` converts a layer between them. Tiles change
// nothing above the file layer: the journal is per document, undo and redo replay commands,
// and a partitioned layer is an ordinary layer in memory that diffs and merges like any other.
// Saving after a transaction rewrites only the tiles whose records changed (Document::dirty).

#include <domain/doc/document.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::doc {

// What one save() wrote, for callers that care what the store touched — and for the test that
// asserts a second save rewrites one tile and not the layer.
struct SaveReport {
  u32 files_written = 0;  // layer, tile, index, and manifest files written this call
  u32 tiles_written = 0;
  u32 tiles_removed = 0;  // tile files that no longer have any records
  u32 tiles_total = 0;    // occupied tiles across every partitioned layer
};

class DocumentStore {
 public:
  static constexpr std::string_view k_manifest_file = "manifest.json";
  static constexpr std::string_view k_layers_dir = "layers";
  static constexpr std::string_view k_journal_file = "journal.jsonl";
  static constexpr std::string_view k_default_layer = "base";
  static constexpr std::string_view k_index_file = "index.json";
  static constexpr std::string_view k_tiles_dir = "tiles";
  static constexpr std::string_view k_untiled_file = "untiled.json";

  static bool exists(const io::Vfs& vfs, std::string_view dir);

  // Creates `dir` with one Base layer named "base" and an empty journal. Fails when a manifest
  // is already there.
  static bool create(const io::Vfs& vfs, std::string_view dir, std::string_view name, Document& out,
                     DocumentManifest& manifest, std::string* error);
  // Loads the manifest, every layer in manifest order, and the journal. A layer the manifest
  // gives a partition is read from its tile files, and a malformed index — a tile file the
  // index does not list, a record in the wrong tile, records out of order — fails the load
  // with a message naming the file. Leaves the document with nothing dirty.
  static bool load(const io::Vfs& vfs, std::string_view dir, Document& out,
                   DocumentManifest& manifest, std::string* error);
  // Writes the layers and the manifest, each file atomically. Refreshes `manifest.layers` and
  // `manifest.edit_layer` from the document; keeps its name and undo position. Files of layers
  // the document no longer has are removed. For a partitioned layer only the tiles that changed
  // are rewritten, which is what `Document::dirty()` is for; a successful save clears it.
  static bool save(const io::Vfs& vfs, std::string_view dir, Document& doc,
                   DocumentManifest& manifest, std::string* error, SaveReport* report = nullptr);

  // Converts a layer between the two forms and rewrites it: a partition with a positive
  // tile_size makes it tiles, anything else makes it a single file. The records are untouched,
  // so the journal, undo, and redo are too. The old form's files are removed.
  static bool repartition(const io::Vfs& vfs, std::string_view dir, Document& doc,
                          DocumentManifest& manifest, u32 layer_index,
                          const LayerPartition& partition, std::string* error,
                          SaveReport* report = nullptr);

  static bool append_journal(const io::Vfs& vfs, std::string_view dir, const Patch& patch,
                             std::string* error);
  // Rewrites the whole journal (after the redo tail was dropped).
  static bool write_journal(const io::Vfs& vfs, std::string_view dir,
                            std::span<const Patch> patches, std::string* error);
  static bool load_journal(const io::Vfs& vfs, std::string_view dir, Vector<Patch>& out,
                           std::string* error);

  // "<sanitized name>.json": characters outside [A-Za-z0-9_.-] become '_'.
  static std::string layer_file_name(std::string_view layer_name);
  // The same sanitized name without the extension: the directory a partitioned layer lives in.
  static std::string layer_dir_name(std::string_view layer_name);
  static std::string path_of(std::string_view dir, std::string_view file);
};

}  // namespace engine::doc
