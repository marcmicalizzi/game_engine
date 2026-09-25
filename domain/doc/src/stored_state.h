#pragma once

// What DocumentStore knows of the directory it keeps a document in (docs/subsystems/doc.md,
// "Saving"). Private to the module: Document holds it, only the store reads or writes it.
//
// A save compares what it would write against this rather than against the disk, so a layer no
// commit touched costs it nothing — no serialization, no read of its index, not even a question to
// the file system — and a file whose bytes would be the ones already there is not written. The
// store fills it from what it wrote, and `load` from what it read, so it describes the disk as long
// as nothing else writes the directory; `Document::mark_all_dirty` drops it for a caller that knows
// something did.

#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>

#include <string>
#include <string_view>

namespace engine::doc {

// A file as it is on disk: its length and a hash of its bytes. Enough to tell whether the bytes a
// save would write are the ones already there without reading them back; the length is there so
// that two files would have to collide on both.
struct StoredFile {
  u64 size = 0;
  u64 hash = 0;

  static StoredFile of(std::string_view bytes) noexcept {
    return StoredFile{bytes.size(), hash_bytes(bytes.data(), bytes.size())};
  }
  friend bool operator==(StoredFile, StoredFile) noexcept = default;
};

// Where a partitioned layer's index files one record: a tile, or untiled.json.
struct Place {
  bool tiled = false;
  TileCoord tile;

  friend bool operator==(Place, Place) noexcept = default;
};

// One layer's form on disk, by the layer's name.
struct StoredLayer {
  bool partitioned = false;
  // Partitioned: the index as index.json holds it, and where it files each id — what a save
  // needs to know which tile a changed record left, and to write the next index without
  // rebuilding it from the whole layer.
  LayerIndex index;
  HashMap<ObjectId, Place> places;
};

struct StoredState {
  // The native directory this describes. A save to any other directory knows nothing.
  std::string root;
  // Every file of the document the store wrote or read, by its path under `root` with forward
  // slashes: manifest.json, layers/<layer>.json, layers/<layer>/index.json, .../tiles/<x>_<y>.json,
  // .../untiled.json. The journal is not here; it is only ever appended to.
  HashMap<std::string, StoredFile> files;
  HashMap<std::string, StoredLayer> layers;
  // `load` found the save log of a save that did not finish: a whole one, which it read the
  // document through, so the files on disk are not yet what `files` says; or a torn one, which it
  // ignored. The next save finishes the first and removes the second before it does anything else.
  bool log_pending = false;
  // `load` found the journal ending in a line with no newline — an append a process died in — and
  // left it out: the journal is valid up to this many bytes, and the next append cuts the rest
  // first. Zero when the journal is whole.
  u64 journal_valid_bytes = 0;
  bool journal_torn = false;
};

}  // namespace engine::doc
