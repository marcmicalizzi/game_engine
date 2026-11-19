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

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace engine::tileset::archive {

inline constexpr u32 k_archive_version = 1;

// **An archive a manifest names is a file beside it** (T194, fuzzing-design.md's fourth list): the
// builds write `<spacing>.masks` and the height levels' names, one component each, and a reader
// joins the name to the manifest's directory. A name that is empty, `.` or `..`, or holds a
// separator, a drive's colon or a NUL would have read a file anywhere the process can: refused.
bool plain_name(std::string_view name) noexcept;

// The key a tile is filed under: z, then x, both signed, so the archive's order is the order the
// builds write in (rows of x in order of z) and the order the renderer asks the masks in.
constexpr u64 tile_key(i32 tx, i32 tz) noexcept {
  return (u64{static_cast<u32>(tz) ^ 0x8000'0000u} << 32) | (static_cast<u32>(tx) ^ 0x8000'0000u);
}

// The most an unbuffered read stages at once (a `Vector<u8>` of `u32` length, a sector of slack):
// a table larger than this is refused rather than staged in a buffer its length was cut to fit
// (T194: the length was cast to `u32`, so a table of 2^32 + 8 bytes staged 4,104).
inline constexpr u64 k_max_staged_bytes = 0xFFFF'FFFFull - 2 * io::k_unbuffered_alignment;
// Whether an unbuffered read of `bytes` at `offset`, widened to whole sectors, can be staged.
constexpr bool stage_fits(u64 offset, u64 bytes) noexcept {
  if (bytes > k_max_staged_bytes || offset > ~u64{0} - bytes - io::k_unbuffered_alignment)
    return false;
  const u64 from = offset - offset % io::k_unbuffered_alignment;
  const u64 end = (offset + bytes + io::k_unbuffered_alignment - 1) / io::k_unbuffered_alignment *
                  io::k_unbuffered_alignment;
  return end - from <= k_max_staged_bytes;
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
  // The same with the record's `hash_bytes` given, computed where the record was made (T128: the
  // mask cook hashes its records on the pool, so the writer's thread only appends them).
  bool add(i32 tx, i32 tz, std::string_view bytes, u64 bytes_hash);
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
  // tiles it lists (at most that many). False, with a sentence, for a file that is not that
  // archive, a table naming a range outside the file among them. `view`, when not empty, is the
  // same file mapped read-only (`io::MappedFile::bytes`), kept mapped while this reader is open:
  // the header and the table are read from it, every range checked against its size, and the
  // table used where it lies, with no copy (tileset.md, "The table read from the view").
  bool open(const std::string& path, std::string_view record, i64 spacing_mm, u32 record_bytes,
            u32 count, io::ReadMode mode, std::string& error, std::span<const u8> view = {});
  void close() noexcept;
  bool is_open() const noexcept { return file_.valid(); }
  const Header& header() const noexcept { return header_; }
  // The table: `entries()` of them, the i-th copied out of wherever it lies (an entry in a view
  // need not be aligned: a record's size sets where the table starts).
  u32 entries() const noexcept { return count_; }
  Entry entry(u32 i) const noexcept {
    Entry e;
    std::memcpy(&e, table_ + u64{i} * sizeof(Entry), sizeof(Entry));
    return e;
  }
  // Whether the table is read in place from the view rather than copied (no commit for it).
  bool table_in_view() const noexcept { return count_ > 0 && toc_.empty(); }
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
  Vector<Entry> toc_;          // the table copied, where it is not read in place
  const u8* table_ = nullptr;  // the table: `toc_`'s bytes, or the view's own
  u32 count_ = 0;
  // The tile key of entry i, read without copying the rest of it.
  u64 key_at(u32 i) const noexcept {
    i32 xz[2];
    std::memcpy(xz, table_ + u64{i} * sizeof(Entry), sizeof(xz));
    return tile_key(xz[0], xz[1]);
  }
  u64 offset_at(u32 i) const noexcept {
    u64 o;
    std::memcpy(&o, table_ + u64{i} * sizeof(Entry) + offsetof(Entry, offset), sizeof(o));
    return o;
  }
};

}  // namespace engine::tileset::archive
