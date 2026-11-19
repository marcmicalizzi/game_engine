#include "archive.h"

#include <core/hash/hash.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace engine::tileset::archive {

namespace {

constexpr u64 k_align = io::k_unbuffered_alignment;

u64 align_down(u64 v) noexcept { return v - v % k_align; }
u64 align_up(u64 v) noexcept { return (v + k_align - 1) / k_align * k_align; }

// A buffer whose start is a whole sector: what an unbuffered read lands in. False, and nothing
// staged, past `k_max_staged_bytes`: the storage's length is a `u32`, and a length cast down to it
// (T194) was a buffer of a few KiB for a read of 4 GiB.
struct Aligned {
  Vector<u8> storage;
  u8* data = nullptr;
  bool reserve(u64 bytes) {
    if (bytes > k_max_staged_bytes) return false;
    storage.resize(static_cast<u32>(bytes + k_align));
    const auto at = reinterpret_cast<std::uintptr_t>(storage.data());
    data = storage.data() + (k_align - at % k_align) % k_align;
    return true;
  }
};

// `bytes` at `offset` into `out`, through whole sectors when the file is unbuffered.
bool read_span(const io::FileHandle& file, u64 offset, u64 bytes, void* out) {
  if (bytes == 0) return true;
  u64 got = 0;
  if (!file.unbuffered())
    return file.read_at(offset, out, bytes, got) == io::Status::Ok && got == bytes;
  if (!stage_fits(offset, bytes)) return false;
  const u64 from = align_down(offset);
  const u64 length = align_up(offset + bytes) - from;
  Aligned staging;
  if (!staging.reserve(length)) return false;
  if (file.read_at(from, staging.data, length, got) != io::Status::Ok ||
      got < offset + bytes - from)
    return false;
  std::memcpy(out, staging.data + (offset - from), static_cast<usize>(bytes));
  return true;
}

// **The table checked before it is used** (tileset.md, "The table read from the view"): it is bytes
// from a file, so every range it names is held to the file's own `size` with arithmetic that cannot
// wrap — the table itself from `toc_offset` to the end, and each entry's record inside [header,
// table) — and the entries sorted. An empty string, or the sentence that refuses it.
std::string check_table(const Header& h, u64 size, u32 record_bytes) {
  const u64 table_bytes = u64{h.count} * sizeof(Entry);
  if (h.toc_offset < sizeof(Header) || h.toc_offset > size || size - h.toc_offset != table_bytes)
    return "its table (" + std::to_string(h.count) + " entries at " + std::to_string(h.toc_offset) +
           ") does not end where its " + std::to_string(size) + " bytes do";
  if (h.toc_offset - sizeof(Header) < record_bytes && h.count > 0)
    return "its table names records the file has no room for";
  return {};
}

std::string check_entries(const u8* table, u32 n, u64 toc_offset, u32 record_bytes) {
  // Every record lies in [sizeof(Header), toc_offset): offset >= the header and offset <= the last
  // place a whole record still fits, which `check_table` showed is not below the header.
  const u64 last = toc_offset - record_bytes;
  u64 previous = 0;
  for (u32 i = 0; i < n; ++i) {
    Entry e;
    std::memcpy(&e, table + u64{i} * sizeof(Entry), sizeof(Entry));
    // Both refusals open with the sentence T190's hostile-archive case reads.
    const auto named = [&] {
      return "its table is not sorted records within the file: entry " + std::to_string(i) +
             " (tile " + std::to_string(e.tx) + ", " + std::to_string(e.tz) + ")";
    };
    if (e.offset < sizeof(Header) || e.offset > last || e.bytes != record_bytes)
      return named() + " names bytes " + std::to_string(e.offset) + " to " +
             std::to_string(e.offset + e.bytes) + ", outside its records (" +
             std::to_string(sizeof(Header)) + " to " + std::to_string(toc_offset) + ")";
    const u64 key = tile_key(e.tx, e.tz);
    if (i > 0 && key <= previous) return named() + " is out of order";
    previous = key;
  }
  return {};
}

}  // namespace

bool plain_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name)
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
  return true;
}

// ---- the writer ---------------------------------------------------------------------------------

Writer::Writer(std::string_view record, i64 spacing_mm, u32 record_bytes) {
  std::memcpy(header_.record, record.data(), std::min<usize>(record.size(), 4));
  header_.spacing_mm = spacing_mm;
  header_.record_bytes = record_bytes;
  header_.records_hash = k_hash_seed;
  data_.assign(sizeof(Header), '\0');
}

bool Writer::add(i32 tx, i32 tz, std::string_view bytes) {
  return add(tx, tz, bytes, hash_bytes(bytes.data(), bytes.size()));
}

bool Writer::add(i32 tx, i32 tz, std::string_view bytes, u64 bytes_hash) {
  if (bytes.size() != header_.record_bytes) return false;
  if (!toc_.empty() && tile_key(tx, tz) <= tile_key(toc_.back().tx, toc_.back().tz)) return false;
  Entry e;
  e.tx = tx;
  e.tz = tz;
  e.offset = data_.size();
  e.bytes = header_.record_bytes;
  toc_.push_back(e);
  data_.append(bytes.data(), bytes.size());
  header_.records_hash = hash_combine(header_.records_hash, bytes_hash);
  return true;
}

bool Writer::write(const std::string& path, u64& file_bytes, std::string& error) {
  header_.count = toc_.size();
  header_.toc_offset = data_.size();
  std::string out = data_;
  out.append(reinterpret_cast<const char*>(toc_.data()), toc_.size() * sizeof(Entry));
  std::memcpy(out.data(), &header_, sizeof(header_));
  if (io::write_file_atomic(path, out) != io::Status::Ok) {
    error = "'" + path + "' could not be written";
    return false;
  }
  file_bytes = out.size();
  return true;
}

// ---- the reader ---------------------------------------------------------------------------------

bool Reader::open(const std::string& path, std::string_view record, i64 spacing_mm,
                  u32 record_bytes, u32 count, io::ReadMode mode, std::string& error,
                  std::span<const u8> view) {
  close();
  const io::Status opened = file_.open(path, mode);
  if (opened != io::Status::Ok) {
    error = "'" + path + "' could not be opened (" + io::status_name(opened) + ")";
    return false;
  }
  const auto refuse = [&](const std::string& why) {
    error = "'" + path + "' is not this level's archive: " + why;
    close();
    return false;
  };
  // With a view, everything is read from it, so the header, the table and every record are one
  // file's even if the path was replaced between the two opens; without one, from the file.
  const bool mapped = !view.empty();
  const u64 size = mapped ? view.size() : file_.size();
  if (size < sizeof(Header)) return refuse("it is shorter than its header");
  if (mapped)
    std::memcpy(&header_, view.data(), sizeof(Header));
  else if (!read_span(file_, 0, sizeof(Header), &header_))
    return refuse("its header does not read");
  if (std::memcmp(header_.magic, "ETAR", 4) != 0 || header_.version != k_archive_version)
    return refuse("not an archive of version " + std::to_string(k_archive_version));
  // At most the tiles the manifest lists: one the table lacks is refused when it is asked, as a
  // missing tile file was, rather than the whole level at open.
  if (record.size() != 4 || std::memcmp(header_.record, record.data(), 4) != 0 ||
      header_.spacing_mm != spacing_mm || header_.record_bytes != record_bytes ||
      header_.count > count)
    return refuse("its records are not the " + std::to_string(count) + " of " +
                  std::to_string(record_bytes) + " bytes at " + std::to_string(spacing_mm) +
                  " mm its manifest lists");
  if (const std::string why = check_table(header_, size, record_bytes); !why.empty())
    return refuse(why);
  const u32 n = header_.count;
  // **The table where it lies** (sweep-P1, after T182): a mapped table is read in place, so the
  // level commits nothing for it (24 bytes a tile: 14.5 MB for the forest's level); a table read
  // from the file is copied, straight into place.
  if (mapped) {
    table_ = view.data() + header_.toc_offset;
  } else {
    toc_.resize_exact(n);
    if (!read_span(file_, header_.toc_offset, u64{n} * sizeof(Entry), toc_.data()))
      return refuse("its table does not read");
    table_ = reinterpret_cast<const u8*>(toc_.data());
  }
  // **An entry's range is checked without a sum that can wrap** (T190): `offset` is the file's u64,
  // so `offset + record_bytes` of an offset near 2^64 wrapped to a few bytes, passed, and named a
  // record 2^64 bytes away; a read through the file then failed, but a reader of a mapped view of
  // the archive (b69-T182's `MaskLevel`) adds the offset to the view's base. `check_entries` holds
  // each offset to `toc_offset - record_bytes`, a difference `check_table` showed cannot wrap.
  count_ = n;
  if (const std::string why = check_entries(table_, n, header_.toc_offset, record_bytes);
      !why.empty())
    return refuse(why);
  return true;
}

void Reader::close() noexcept {
  file_.close();
  header_ = Header{};
  table_ = nullptr;
  count_ = 0;
  toc_ = Vector<Entry>{};
}

u32 Reader::find(i32 tx, i32 tz) const noexcept {
  const u64 key = tile_key(tx, tz);
  u32 lo = 0;
  u32 hi = count_;
  while (lo < hi) {
    const u32 mid = lo + (hi - lo) / 2;
    const u64 k = key_at(mid);
    if (k == key) return mid;
    if (k < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return k_absent;
}

namespace {

struct Run {
  u32 first = 0;
  u32 count = 0;
  u64 lead = 0;  // bytes before the first record in what is read (an unbuffered run's alignment)
  bool staged = false;
};

struct Batch {
  std::span<Fetch> fetches;
  const Vector<Run>* runs = nullptr;
  const Vector<io::AsyncRangeRead>* requests = nullptr;
  u64 record_bytes = 0;
  Landed landed = nullptr;
  void* context = nullptr;
};

void run_landed(void* p, u32 index) noexcept {
  const Batch& b = *static_cast<const Batch*>(p);
  const Run& run = (*b.runs)[index];
  const io::AsyncRangeRead& read = (*b.requests)[index];
  const u8* from = static_cast<const u8*>(read.dst) + run.lead;
  for (u32 k = 0; k < run.count; ++k) {
    Fetch& f = b.fetches[run.first + k];
    const u64 end = run.lead + (u64{k} + 1) * b.record_bytes;
    f.ok = read.status == io::Status::Ok && read.read >= end;
    if (f.ok && run.staged) std::memcpy(f.dst, from + u64{k} * b.record_bytes, b.record_bytes);
  }
  if (b.landed != nullptr) b.landed(b.context, run.first, run.count);
}

}  // namespace

void Reader::fetch(std::span<Fetch> fetches, const FetchOptions& options, Landed landed,
                   void* context, FetchStats* stats) const noexcept {
  if (stats != nullptr) *stats = FetchStats{};
  const u32 n = static_cast<u32>(fetches.size());
  if (n == 0) return;
  const u64 rb = header_.record_bytes;
  const bool unbuffered = file_.unbuffered();
  const u32 depth = std::clamp<u32>(options.depth, 1, io::k_max_queue_depth);
  Vector<Run> runs;
  Vector<io::AsyncRangeRead> requests;
  Vector<u64> lengths;  // what each run reads, alignment included
  u64 slot_bytes = 0;
  for (u32 i = 0; i < n;) {
    Run run;
    run.first = i;
    run.count = 1;
    bool contiguous = true;
    const u64 offset = offset_at(fetches[i].entry);
    while (i + run.count < n) {
      const Fetch& next = fetches[i + run.count];
      if (offset_at(next.entry) != offset + u64{run.count} * rb) break;
      if ((u64{run.count} + 1) * rb > options.run_bytes) break;
      contiguous = contiguous && static_cast<const u8*>(next.dst) ==
                                     static_cast<const u8*>(fetches[i + run.count - 1].dst) + rb;
      ++run.count;
    }
    const u64 bytes = u64{run.count} * rb;
    io::AsyncRangeRead request;
    request.file = &file_;
    if (unbuffered) {
      request.offset = align_down(offset);
      run.lead = offset - request.offset;
      request.bytes = align_up(offset + bytes) - request.offset;
      run.staged = true;
    } else {
      request.offset = offset;
      request.bytes = bytes;
      run.staged = !(run.count == 1 || contiguous);
      if (!run.staged) request.dst = fetches[i].dst;
    }
    if (run.staged) slot_bytes = std::max(slot_bytes, request.bytes);
    runs.push_back(run);
    requests.push_back(request);
    i += run.count;
  }
  // The ring: request r lands in slot r % depth, which `read_queued` reuses only after request r's
  // completion has copied out of it.
  const u32 slots = std::min<u32>(depth, runs.size());
  Aligned staging;
  if (slot_bytes > 0) {
    slot_bytes = align_up(slot_bytes);
    // A run is at most `run_bytes` plus a sector each side, far inside the stage's limit.
    if (!staging.reserve(slot_bytes * slots)) {
      for (Fetch& f : fetches)
        f.ok = false;
      return;
    }
    for (u32 r = 0; r < runs.size(); ++r)
      if (runs[r].staged) requests[r].dst = staging.data + u64{r % slots} * slot_bytes;
  }
  Batch batch{fetches, &runs, &requests, rb, landed, context};
  io::QueuedReadStats queued;
  (void)io::read_queued(std::span<io::AsyncRangeRead>(requests.data(), requests.size()), depth,
                        &run_landed, &batch, &queued);
  if (stats != nullptr) {
    stats->runs = runs.size();
    stats->most_in_flight = queued.most_in_flight;
    for (const io::AsyncRangeRead& r : requests)
      stats->bytes += r.read;
  }
}

}  // namespace engine::tileset::archive
