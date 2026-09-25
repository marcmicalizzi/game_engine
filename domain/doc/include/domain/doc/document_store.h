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
//
// **A save writes what changed and nothing else** (docs/subsystems/doc.md, "Saving"). The
// document says which layers changed since the last save and which records in them
// (`Document::changes`); the store remembers what it last wrote to, or read from, the directory —
// every file's length and a hash of its bytes, and each partitioned layer's index — so a layer no
// commit touched is not looked at, a partitioned one is looked at in the tiles its changed records
// were and are in, and a file whose canonical bytes are the ones already there is not written.
// What ends up on disk is byte for byte what writing every file would have left.
//
// **A save is all or nothing.** Before it touches a file of the document it writes one log,
// `save.pending`, holding every file it will write and every file it will remove, closed by a hash
// of the rest; then it writes each file once, in place, removes the others, and removes the log.
// A process that dies before the log is whole has touched nothing; one that dies after leaves a
// log that says what the directory is to hold, which `load` reads through (and never writes) and
// the next save carries out first. The journal is appended before the save, so a save that died
// leaves the commit as the first patch of the redo tail, and the manifest's undo position says
// which state the layer files hold: one or the other, never a mixture.

#include <domain/doc/document.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::doc {

// What one save() did, for callers that care what the store touched — and for the tests that
// assert a transaction rewrites one tile and not the layer.
struct SaveReport {
  u32 files_written = 0;  // layer, tile, index, and manifest files written this call
  u64 bytes_written = 0;  // what those files hold, summed
  u32 files_removed = 0;  // files of tiles, forms and layers the document no longer has
  u32 tiles_written = 0;
  u32 tiles_removed = 0;  // tile files that no longer have any records
  u32 tiles_total = 0;    // occupied tiles across every partitioned layer
  // The paths written and removed, under the document directory, in the order they were.
  Vector<std::string> written;
  Vector<std::string> removed;
};

// For the crash-safety tests: where a save dies. It carries out `stop_after` of its file operations
// — the log first, then each write and each removal, and the log's removal last — and stops at the
// next one as a killed process would, leaving the disk as it is and returning false; with `tear`,
// that next operation is left half done (half of the file's bytes written over it). Negative, the
// default, runs the save to the end.
struct SaveOptions {
  i32 stop_after = -1;
  bool tear = false;
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
  // The log of a save in progress. Never there at rest: a save removes it when it is done, and one
  // found there is a save that did not finish.
  static constexpr std::string_view k_save_log = "save.pending";

  // A manifest is there, or the log of a first save that did not finish.
  static bool exists(const io::Vfs& vfs, std::string_view dir);

  // Creates `dir` with one Base layer named "base" and an empty journal. Fails when a manifest
  // is already there.
  static bool create(const io::Vfs& vfs, std::string_view dir, std::string_view name, Document& out,
                     DocumentManifest& manifest, std::string* error);
  // Loads the manifest, every layer in manifest order, and the journal. A layer the manifest
  // gives a partition is read from its tile files, and a malformed index — a tile file the
  // index does not list, a record in the wrong tile, records out of order — fails the load
  // with a message naming the file. Leaves the document with nothing changed, except a layer
  // whose files are not byte for byte what a save would write, which the next save rewrites; and
  // leaves it knowing what it read, so that the next save to `dir` writes only what changes. A
  // whole save log is read through — the document is what that save would have left — and nothing
  // on disk is changed: a reader never writes.
  static bool load(const io::Vfs& vfs, std::string_view dir, Document& out,
                   DocumentManifest& manifest, std::string* error);
  // Writes what changed since the last save or load: the files of the layers `Document::changes`
  // names — for a partitioned layer, the tiles its changed records were and are in, and the index
  // when one changed tiles — and the manifest, each only when its canonical bytes differ from the
  // ones on disk. Refreshes `manifest.layers` and `manifest.edit_layer` from the document and keeps
  // its name and undo position. Files of layers the document no longer has, and of a layer's old
  // form, are removed. A document the store knows nothing of here (new, saved elsewhere, or
  // marked all dirty) is written whole, and whatever the old form or the manifest left is looked
  // for on disk. A save that did not finish is finished first. All or nothing, through the save
  // log. A successful save clears the document's changes.
  static bool save(const io::Vfs& vfs, std::string_view dir, Document& doc,
                   DocumentManifest& manifest, std::string* error, SaveReport* report = nullptr,
                   const SaveOptions& options = {});

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
