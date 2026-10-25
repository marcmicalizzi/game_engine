#pragma once

// **A level's tiles in one file** (docs/subsystems/tileset.md, "The archive", roadmap W76): a
// 64-byte header, every tile's record back to back — each the bytes its own file held before, its
// 32-byte header and its payload — and a table of contents at the end, one entry a tile, sorted by
// `tile_key` (z, then x), so a tile is found by a binary search of a table read once at open and a
// row of tiles lies contiguous in the file. The height levels (`<spacing_mm>.tiles`, records
// `ETIL`) and the mask level (`<spacing_mm>.masks`, records `EMSK`) are the same container.
//
// Internal to the capability: the format is the manifest's business, and nothing outside this
// module reads it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::tileset::archive {

inline constexpr u32 k_archive_version = 1;

// The key a tile is filed under: z, then x, both signed, so the archive's order is the order the
// builds write in (rows of x in order of z) and the order the renderer asks the masks in.
constexpr u64 tile_key(i32 tx, i32 tz) noexcept {
  return (u64{static_cast<u32>(tz) ^ 0x8000'0000u} << 32) | (static_cast<u32>(tx) ^ 0x8000'0000u);
}

struct Header {
  char magic[4] = {'E', 'T', 'A', 'R'};
  u32 version = k_archive_version;
  char record[4] = {};  // what every record is: "ETIL" (a height tile) or "EMSK" (a mask tile)
  u32 count = 0;
  i64 spacing_mm = 0;
  u64 toc_offset = 0;    // `count` entries, to the end of the file
  u64 records_hash = 0;  // each record's `hash_bytes`, combined in order from `k_hash_seed`
  u32 record_bytes = 0;  // every record's size
  u32 reserved0 = 0;
  u64 reserved1 = 0;
  u64 reserved2 = 0;
};
static_assert(sizeof(Header) == 64);

struct Entry {
  i32 tx = 0;
  i32 tz = 0;
  u64 offset = 0;
  u32 bytes = 0;
  u32 reserved = 0;
};
static_assert(sizeof(Entry) == 24);

// Builds an archive in memory and writes it whole, atomically (`io::write_file_atomic`): a reader
// sees the old archive or the new one, never a mix.
class Writer {
 public:
  Writer(std::string_view record, i64 spacing_mm, u32 record_bytes);
  // Appends a record. False for one out of `tile_key` order or not `record_bytes` long.
  bool add(i32 tx, i32 tz, std::string_view bytes);
  u32 count() const noexcept { return toc_.size(); }
  u64 records_hash() const noexcept { return header_.records_hash; }
  // The archive's bytes: header, records, table.
  bool write(const std::string& path, u64& file_bytes, std::string& error);

 private:
  Header header_;
  std::string data_;
  Vector<Entry> toc_;
};

// One record to read: the table's entry and where its bytes go (`record_bytes` of them).
struct Fetch {
  u32 entry = 0;
  void* dst = nullptr;
  bool ok = false;  // set by `Reader::fetch`: the record arrived whole
};

struct FetchOptions {
  u32 depth = 32;              // runs in flight at once (`io::read_queued`)
  u64 run_bytes = 256 * 1024;  // records adjacent in the file are read as one run up to this long
};

struct FetchStats {
  u32 runs = 0;
  u32 most_in_flight = 0;
  u64 bytes = 0;  // read from the file, alignment included
};

// Called as each run lands: fetches [first, first + count) are done (each `ok` or not).
using Landed = void (*)(void* context, u32 first, u32 count) noexcept;

// An archive opened for reading: its header and table read and checked, its file held open, and
// every record addressed by offset from then on.
class Reader {
 public:
  // `record` and `record_bytes` are what the caller's manifest says every record is; `count` the
  // tiles it lists (the table must hold exactly that many). False, with a sentence, for a file
  // that is not that archive.
  bool open(const std::string& path, std::string_view record, i64 spacing_mm, u32 record_bytes,
            u32 count, io::ReadMode mode, std::string& error);
  void close() noexcept;
  bool is_open() const noexcept { return file_.valid(); }
  const Header& header() const noexcept { return header_; }
  std::span<const Entry> table() const noexcept { return {toc_.data(), toc_.size()}; }
  // The table's index of (tx, tz), or `k_absent`: a binary search.
  static constexpr u32 k_absent = ~u32{0};
  u32 find(i32 tx, i32 tz) const noexcept;

  // **Reads every fetch's record as one batch** (sorted by entry, ascending): records adjacent in
  // the file are coalesced into runs of at most `run_bytes`, up to `depth` runs in flight from the
  // calling thread, and `landed` runs as each run lands. A run of one record, or of records whose
  // destinations lie contiguous, is read straight into them; any other run (and every run of an
  // unbuffered file, whose ranges must be whole sectors) through a ring of `depth` staging buffers.
  void fetch(std::span<Fetch> fetches, const FetchOptions& options, Landed landed, void* context,
             FetchStats* stats = nullptr) const noexcept;

 private:
  io::FileHandle file_;
  Header header_;
  Vector<Entry> toc_;
};

}  // namespace engine::tileset::archive
