#include "stored_state.h"

#include <core/base/macros.h>
#include <core/containers/flat_set.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/profiling/profile.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <thread>

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>     // _O_WRONLY, _O_CREAT, _O_BINARY
#include <io.h>        // _wsopen_s, _write, _chsize_s, _close
#include <share.h>     // _SH_DENYNO
#include <sys/stat.h>  // _S_IREAD, _S_IWRITE
#else
#include <fcntl.h>   // open
#include <unistd.h>  // write, ftruncate, close
#endif

namespace engine::doc {

namespace {

void set_error(std::string* error, std::string_view path, std::string_view what) {
  if (error == nullptr) return;
  error->assign(path);
  error->append(": ");
  error->append(what);
}

void set_error(std::string* error, std::string_view path, const schema::ReadContext& ctx) {
  if (error == nullptr) return;
  error->assign(path);
  error->append(": ");
  bool first = true;
  for (const auto& d : ctx.diagnostics) {
    if (!first) error->append("; ");
    first = false;
    if (!d.path.empty()) {
      error->append(d.path);
      error->append(": ");
    }
    error->append(d.message);
  }
}

// The canonical bytes of each kind of file. A save compares these against what it last wrote or
// read, and load compares what it read against them, so there is exactly one spelling of each.
std::string file_text(const LayerFile& file) { return write_json(schema::to_json(file)); }

std::string index_text(const LayerIndex& index) {
  std::string text = write_json(schema::to_json(index));
  text.push_back('\n');
  return text;
}

std::string manifest_text(const DocumentManifest& manifest) {
  std::string text = write_json(schema::to_json(manifest));
  text.push_back('\n');
  return text;
}

template <class T>
bool parse_schema_text(std::string_view text, const std::string& path, T& out, std::string* error) {
  JsonValue value;
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    set_error(error, path, r.message);
    return false;
  }
  schema::ReadContext ctx;
  out = T{};
  if (!schema::from_json(out, value, ctx)) {
    set_error(error, path, ctx);
    return false;
  }
  return true;
}

std::string hex_of(ObjectId id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

std::string join(std::string_view dir, std::string_view file) {
  return DocumentStore::path_of(dir, file);
}

// The paths of one layer's files under the document directory, in both forms.
struct LayerFiles {
  std::string single;   // layers/<layer>.json
  std::string dir;      // layers/<layer>
  std::string tiles;    // layers/<layer>/tiles
  std::string index;    // layers/<layer>/index.json
  std::string untiled;  // layers/<layer>/untiled.json

  std::string tile(std::string_view file) const { return join(tiles, file); }
};

LayerFiles files_of(std::string_view layer_name) {
  LayerFiles f;
  f.single = join(DocumentStore::k_layers_dir, DocumentStore::layer_file_name(layer_name));
  f.dir = join(DocumentStore::k_layers_dir, DocumentStore::layer_dir_name(layer_name));
  f.tiles = join(f.dir, DocumentStore::k_tiles_dir);
  f.index = join(f.dir, DocumentStore::k_index_file);
  f.untiled = join(f.dir, DocumentStore::k_untiled_file);
  return f;
}

// --- the index, kept rather than rebuilt ----------------------------------------------------

TileCoord coord_of(const TileRef& t) { return TileCoord{t.x, t.y}; }

// The tile's entry in `index`, or where it would go.
u32 tile_slot(const LayerIndex& index, TileCoord tile) {
  const auto at = std::lower_bound(index.tiles.begin(), index.tiles.end(), tile,
                                   [](const TileRef& t, TileCoord c) { return coord_of(t) < c; });
  return static_cast<u32>(at - index.tiles.begin());
}

const TileRef* find_tile(const LayerIndex& index, TileCoord tile) {
  const u32 slot = tile_slot(index, tile);
  return slot < index.tiles.size() && coord_of(index.tiles[slot]) == tile ? &index.tiles[slot]
                                                                          : nullptr;
}

void insert_sorted(Vector<ObjectId>& ids, ObjectId id) {
  const auto at = std::lower_bound(ids.begin(), ids.end(), id);
  const u32 pos = static_cast<u32>(at - ids.begin());
  if (pos < ids.size() && ids[pos] == id) return;
  ids.emplace(pos, id);
}

void erase_sorted(Vector<ObjectId>& ids, ObjectId id) {
  const auto at = std::lower_bound(ids.begin(), ids.end(), id);
  const u32 pos = static_cast<u32>(at - ids.begin());
  if (pos < ids.size() && ids[pos] == id) ids.erase_at(pos);
}

// Moves `id` in `index` from where it was filed to where it belongs now; either may be absent (a
// record that arrived, or left the layer). An emptied tile loses its entry, a new one gains one.
void refile(LayerIndex& index, ObjectId id, const std::optional<Place>& from,
            const std::optional<Place>& to) {
  if (from.has_value()) {
    if (from->tiled) {
      const u32 slot = tile_slot(index, from->tile);
      if (slot < index.tiles.size() && coord_of(index.tiles[slot]) == from->tile) {
        erase_sorted(index.tiles[slot].objects, id);
        if (index.tiles[slot].objects.empty()) index.tiles.erase_at(slot);
      }
    } else {
      erase_sorted(index.untiled, id);
    }
  }
  if (to.has_value()) {
    if (to->tiled) {
      const u32 slot = tile_slot(index, to->tile);
      if (slot >= index.tiles.size() || !(coord_of(index.tiles[slot]) == to->tile)) {
        TileRef ref;
        ref.x = to->tile.x;
        ref.y = to->tile.y;
        ref.file = tile_file_name(to->tile);
        index.tiles.emplace(slot, std::move(ref));
      }
      insert_sorted(index.tiles[slot].objects, id);
    } else {
      insert_sorted(index.untiled, id);
    }
  }
}

// The bounds build_layer_index gives: over the occupied tiles, all zero when there are none.
void recompute_bounds(LayerIndex& index) {
  index.min_x = index.min_y = index.max_x = index.max_y = 0;
  bool first = true;
  for (const TileRef& t : index.tiles) {
    index.min_x = first ? t.x : std::min(index.min_x, t.x);
    index.min_y = first ? t.y : std::min(index.min_y, t.y);
    index.max_x = first ? t.x : std::max(index.max_x, t.x);
    index.max_y = first ? t.y : std::max(index.max_y, t.y);
    first = false;
  }
}

void fill_places(StoredLayer& layer) {
  layer.places.clear();
  u32 count = layer.index.untiled.size();
  for (const TileRef& t : layer.index.tiles)
    count += t.objects.size();
  layer.places.reserve(count);
  for (const TileRef& t : layer.index.tiles) {
    for (const ObjectId id : t.objects)
      layer.places.insert_or_assign(id, Place{true, coord_of(t)});
  }
  for (const ObjectId id : layer.index.untiled)
    layer.places.insert_or_assign(id, Place{false, TileCoord{}});
}

// --- planning a save ------------------------------------------------------------------------

// A file's length before a write, when the store does not know it.
constexpr u64 k_unknown_size = ~u64{0};

struct PlannedWrite {
  std::string path;  // under the document directory
  std::string text;
  bool tile = false;
  u64 was_size = k_unknown_size;  // what the file held before, as far as the store knows
};

struct PlannedRemoval {
  std::string path;
  bool tile = false;
};

// One save: every file that has to change, decided before anything is touched.
class SavePlan {
 public:
  SavePlan(std::string root, StoredState& next, bool probe_all)
      : root_(std::move(root)), next_(next), probe_all_(probe_all) {}

  // Queues `text` for `path` unless the file already holds exactly these bytes.
  void consider(std::string path, std::string text, bool tile) {
    const StoredFile* f = next_.files.find_value(path);
    if (f != nullptr && *f == StoredFile::of(text)) return;
    const u64 was = f != nullptr ? f->size : k_unknown_size;
    writes_.push_back(PlannedWrite{std::move(path), std::move(text), tile, was});
  }
  // Queues the removal of a file the store knows is there.
  void remove_known(const std::string& path, bool tile) {
    if (next_.files.contains(path)) removals_.push_back(PlannedRemoval{path, tile});
  }
  // Queues the removal of a file that may be there although the store does not know it: what a
  // save asks the disk when it knows nothing of a layer's files.
  void remove_if_present(const std::string& path, bool tile) {
    if (next_.files.contains(path) || io::exists(native(path)))
      removals_.push_back(PlannedRemoval{path, tile});
  }
  // Queues the removal of a file a directory listing just found.
  void remove_listed(std::string path, bool tile) {
    removals_.push_back(PlannedRemoval{std::move(path), tile});
  }
  // A directory the form has even when it holds nothing (a partitioned layer's tiles/).
  void directory(std::string path) { directories_.push_back(std::move(path)); }

  std::string native(std::string_view path) const { return join(root_, path); }
  const std::string& root() const noexcept { return root_; }
  bool probe_all() const noexcept { return probe_all_; }
  StoredState& next() noexcept { return next_; }
  const Vector<PlannedWrite>& writes() const noexcept { return writes_; }
  const Vector<PlannedRemoval>& removals() const noexcept { return removals_; }
  const Vector<std::string>& directories() const noexcept { return directories_; }

 private:
  std::string root_;
  StoredState& next_;
  bool probe_all_ = false;
  Vector<PlannedWrite> writes_;
  Vector<PlannedRemoval> removals_;
  Vector<std::string> directories_;
};

// Every file of a partitioned layer the store knows of.
void remove_partition_known(SavePlan& plan, const LayerFiles& lf, const StoredLayer& was) {
  for (const TileRef& t : was.index.tiles)
    plan.remove_known(lf.tile(t.file), true);
  plan.remove_known(lf.untiled, false);
  plan.remove_known(lf.index, false);
}

// Every file of a partitioned layer that is on disk, for a layer the store knows nothing of. Only
// when there is an index, as it always was: tiles without one are not a partitioned layer.
void remove_partition_present(SavePlan& plan, const LayerFiles& lf) {
  if (!io::exists(plan.native(lf.index))) return;
  Vector<io::DirEntry> entries;
  if (io::list_directory(plan.native(lf.tiles), entries) == io::Status::Ok) {
    for (const io::DirEntry& e : entries) {
      if (!e.is_directory) plan.remove_listed(lf.tile(e.name), true);
    }
  }
  plan.remove_if_present(lf.untiled, false);
  plan.remove_if_present(lf.index, false);
}

// A single-file layer: its file, compared whole, and the other form's files if it had them.
void plan_single(SavePlan& plan, const Layer& layer, const LayerFiles& lf, const StoredLayer* was,
                 StoredLayer& next) {
  plan.consider(lf.single, layer.to_json_text(), false);
  if (was == nullptr) {
    remove_partition_present(plan, lf);
  } else if (was->partitioned) {
    remove_partition_known(plan, lf, *was);
  }
  next.partitioned = false;
  next.index = LayerIndex{};
  next.places.clear();
}

// A partitioned layer looked at whole: every file it should have, each compared with what is there,
// and every file it had that it should not.
void plan_partitioned_whole(SavePlan& plan, const Layer& layer, const LayerFiles& lf,
                            const StoredLayer* was, StoredLayer& next) {
  LayerIndex index = build_layer_index(layer);
  plan.directory(lf.tiles);
  for (const TileRef& t : index.tiles)
    plan.consider(lf.tile(t.file), file_text(layer.to_file(t.objects)), true);
  if (!index.untiled.empty())
    plan.consider(lf.untiled, file_text(layer.to_file(index.untiled)), false);
  plan.consider(lf.index, index_text(index), false);

  if (was == nullptr) {
    // Nothing known of this layer's files: ask the disk, as every save used to. Listing the tiles
    // rather than trusting an old index is what keeps "the same content writes the same set of
    // files" true whatever was left there.
    Vector<io::DirEntry> entries;
    if (io::list_directory(plan.native(lf.tiles), entries) == io::Status::Ok) {
      for (const io::DirEntry& e : entries) {
        TileCoord tile;
        if (e.is_directory) continue;
        if (parse_tile_file_name(e.name, tile) && find_tile(index, tile) != nullptr) continue;
        plan.remove_listed(lf.tile(e.name), true);
      }
    }
    if (index.untiled.empty()) plan.remove_if_present(lf.untiled, false);
    plan.remove_if_present(lf.single, false);
  } else if (was->partitioned) {
    for (const TileRef& t : was->index.tiles) {
      if (find_tile(index, coord_of(t)) == nullptr) plan.remove_known(lf.tile(t.file), true);
    }
    if (index.untiled.empty()) plan.remove_known(lf.untiled, false);
  } else {
    plan.remove_known(lf.single, false);
  }
  next.partitioned = true;
  next.index = std::move(index);
  fill_places(next);
}

// A partitioned layer after commands: the records they named, the tiles each was filed in and
// belongs in now, and the index only when a record changed tiles. `known` is updated in place.
void plan_partitioned_changes(SavePlan& plan, const Layer& layer, const LayerFiles& lf,
                              StoredLayer& known, const HashSet<ObjectId>& ids) {
  const LayerPartition& partition = layer.partition();
  FlatSet<TileCoord> affected;
  bool untiled_affected = false;
  bool moved = false;
  for (const ObjectId id : ids.keys()) {
    std::optional<Place> was;
    if (const Place* p = known.places.find_value(id)) was = *p;
    std::optional<Place> now;
    if (const ObjectRecord* r = layer.find(id)) {
      Place p;
      p.tiled = tile_of(*r, partition, p.tile);
      if (!p.tiled) p.tile = TileCoord{};
      now = p;
    }
    for (const std::optional<Place>* place : {&was, &now}) {
      if (!place->has_value()) continue;
      if ((*place)->tiled) {
        affected.insert((*place)->tile);
      } else {
        untiled_affected = true;
      }
    }
    if (was == now) continue;
    moved = true;
    refile(known.index, id, was, now);
    if (now.has_value()) {
      known.places.insert_or_assign(id, *now);
    } else {
      known.places.erase(id);
    }
  }
  if (moved) recompute_bounds(known.index);
  ENGINE_ASSERT(known.index == build_layer_index(layer),
                "doc store: the kept index drifted from the layer's own");

  for (const TileCoord tile : affected) {
    const std::string path = lf.tile(tile_file_name(tile));
    if (const TileRef* ref = find_tile(known.index, tile)) {
      plan.consider(path, file_text(layer.to_file(ref->objects)), true);
    } else {
      plan.remove_known(path, true);  // its last record left
    }
  }
  if (untiled_affected) {
    if (known.index.untiled.empty()) {
      plan.remove_known(lf.untiled, false);
    } else {
      plan.consider(lf.untiled, file_text(layer.to_file(known.index.untiled)), false);
    }
  }
  if (moved) plan.consider(lf.index, index_text(known.index), false);
}

// --- writing a file -------------------------------------------------------------------------
//
// What a file operation costs on this project's Windows machine (doc.fs.* in bench/doc_bench.cpp;
// docs/subsystems/doc.md, "Writing a file"). Writing over an existing file in place, opened for
// writing alone, at its own length, costs 0.5 ms whatever its size. Removing a file and making it
// again costs 1.2 ms. Replacing a file by renaming a new one over it costs 3 ms. Changing a file's
// length in place, truncating it before writing it, or opening it to read as well as write
// (stdio's "r+") costs 5-12 ms once the file is 16 KiB or more. The file system charges by the
// operation, and by its kind, not by the byte. So a save writes each file once, write-only, in
// place when its length stays or it is small and made again when not, and makes the save as a
// whole all or nothing with one log written first (below) instead of making each file atomic
// with a rename of its own.

// At and above this, a file whose length changes is removed and made again rather than cut or
// grown in place: 0.7 ms against 1.2 ms at 4 KiB, 4.8 ms against 1.2 ms at 16 KiB (doc.fs.
// overwrite_resize, doc.fs.recreate).
constexpr u64 k_recreate_from = 8 * 1024;

io::Status status_of_errno(int err) noexcept {
  switch (err) {
    case ENOENT: return io::Status::NotFound;
    case EACCES: return io::Status::PermissionDenied;
    case EISDIR: return io::Status::IsDirectory;
    case ENOTDIR: return io::Status::NotDirectory;
    default: return io::Status::IoError;
  }
}

// Writes `data` over the file — opened for writing alone and without truncating it, written from
// the start, cut to the new length, closed — creating it when it is not there. With `half`, stops
// after half the bytes, as a process killed in the middle would: the crash tests' torn write.
io::Status overwrite_once(const std::string& native, std::string_view data, bool half) {
  const usize n = half ? data.size() / 2 : data.size();
  bool ok = true;
#if ENGINE_PLATFORM_WINDOWS
  const std::filesystem::path p(
      std::u8string_view(reinterpret_cast<const char8_t*>(native.data()), native.size()));
  int fd = -1;
  // Shared, so a scanner or an indexer that opened the file after the last write does not make
  // this one fail.
  if (const errno_t e = _wsopen_s(&fd, p.c_str(), _O_WRONLY | _O_BINARY | _O_CREAT | _O_NOINHERIT,
                                  _SH_DENYNO, _S_IREAD | _S_IWRITE);
      e != 0)
    return status_of_errno(e);
  for (usize done = 0; ok && done < n;) {
    const usize chunk = std::min<usize>(n - done, usize{1} << 30);
    const int wrote = _write(fd, data.data() + done, static_cast<unsigned>(chunk));
    ok = wrote > 0;
    if (ok) done += static_cast<usize>(wrote);
  }
  if (!half) ok = _chsize_s(fd, static_cast<long long>(data.size())) == 0 && ok;
  ok = _close(fd) == 0 && ok;
#else
  const int fd = ::open(native.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
  if (fd < 0) return status_of_errno(errno);
  for (usize done = 0; ok && done < n;) {
    const ssize_t wrote = ::write(fd, data.data() + done, n - done);
    if (wrote < 0 && errno == EINTR) continue;
    ok = wrote > 0;
    if (ok) done += static_cast<usize>(wrote);
  }
  if (!half) ok = ::ftruncate(fd, static_cast<off_t>(data.size())) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
#endif
  return ok ? io::Status::Ok : io::Status::IoError;
}

// The same, making the file's directory when the write finds it missing — asking for every parent
// on every write is what `Vfs::write` does, and MSVC's create_directories creates each prefix in
// turn, which costs more than the write — and retrying briefly while something else holds the file.
// A file of k_recreate_from or more whose length changes (`was_size`, when the store knows it) is
// removed first and made again; the save log is what makes that as safe as a write over it.
io::Status overwrite_file(const std::string& native, std::string_view data,
                          u64 was_size = k_unknown_size, bool half = false) {
  if (was_size != k_unknown_size && was_size != data.size() &&
      std::max<u64>(was_size, data.size()) >= k_recreate_from)
    (void)io::remove_file(native);
  for (int attempt = 0;; ++attempt) {
    io::Status s = overwrite_once(native, data, half);
    if (s == io::Status::NotFound) {
      const std::string_view parent = io::parent_path(native);
      if (parent.empty() || io::make_directories(parent) != io::Status::Ok) return s;
      s = overwrite_once(native, data, half);
    }
    if (s != io::Status::PermissionDenied || attempt >= 40) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool ensure_directory(const std::string& native) {
  if (io::exists(native)) return true;
  return io::make_directories(native) == io::Status::Ok;
}

// --- the save log ---------------------------------------------------------------------------
//
// A save that changes anything first writes one file, `save.pending` in the document directory,
// holding every file it is about to write, whole, every file it is about to remove, and a last line
// with a hash of everything above it. Then it writes the files in place and removes the others, and
// last removes the log. A process that dies while writing the log leaves one whose last line is
// missing or wrong and has touched no file of the document: the previous state stands. One that
// dies after leaves a whole log, which says exactly what the directory is to hold: `load` reads the
// document through it, and the next save carries it out before anything else. Either way what is
// read is one state or the other, never a mixture of the two.

constexpr std::string_view k_log_header = "engine.doc.save 1\n";

// One file operation, of a plan or of a log: bytes to write over a file, or its removal.
struct FileOp {
  std::string_view path;  // under the document directory
  std::string_view text;
  bool remove = false;
  bool tile = false;
  u64 was_size = k_unknown_size;
};

std::string hex64(u64 v) {
  std::string out(16, '0');
  for (usize i = 16; i > 0; --i) {
    out[i - 1] = "0123456789abcdef"[v & 15u];
    v >>= 4;
  }
  return out;
}

std::string log_text(std::span<const FileOp> ops) {
  usize size = k_log_header.size() + 32;
  for (const FileOp& op : ops)
    size += op.path.size() + op.text.size() + 32;
  std::string out;
  out.reserve(size);
  out.append(k_log_header);
  for (const FileOp& op : ops) {
    if (op.remove) {
      out.append("remove ").append(op.path).push_back('\n');
      continue;
    }
    out.append("write ").append(op.path).push_back(' ');
    out.append(std::to_string(op.text.size())).push_back('\n');
    out.append(op.text).push_back('\n');
  }
  const u64 hash = hash_bytes(out.data(), out.size());
  out.append("end ").append(hex64(hash)).push_back('\n');
  return out;
}

// A path the log may name: relative, and staying inside the document directory.
bool inside(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos)
    return false;
  usize start = 0;
  while (start <= path.size()) {
    const usize slash = path.find('/', start);
    const std::string_view part = path.substr(
        start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

// The operations of a whole log, as views into `text`. False for anything else — a log a process
// died writing, or one nothing of this store wrote — which means no file was touched by its save.
bool parse_log(std::string_view text, Vector<FileOp>& out) {
  out.clear();
  if (!text.starts_with(k_log_header)) return false;
  usize pos = k_log_header.size();
  for (;;) {
    const usize nl = text.find('\n', pos);
    if (nl == std::string_view::npos) return false;
    const std::string_view line = text.substr(pos, nl - pos);
    if (line.starts_with("end ")) {
      return nl + 1 == text.size() && line.substr(4) == hex64(hash_bytes(text.data(), pos));
    }
    FileOp op;
    if (line.starts_with("remove ")) {
      op.remove = true;
      op.path = line.substr(7);
      pos = nl + 1;
    } else if (line.starts_with("write ")) {
      const std::string_view rest = line.substr(6);
      const usize space = rest.rfind(' ');
      if (space == std::string_view::npos || space + 1 == rest.size()) return false;
      u64 size = 0;
      for (const char c : rest.substr(space + 1)) {
        if (c < '0' || c > '9' || size > (u64{1} << 40)) return false;
        size = size * 10 + static_cast<u64>(c - '0');
      }
      if (text.size() - (nl + 1) < size + 1 || text[nl + 1 + size] != '\n') return false;
      op.path = rest.substr(0, space);
      op.text = text.substr(nl + 1, size);
      pos = nl + 1 + size + 1;
    } else {
      return false;
    }
    if (!inside(op.path)) return false;
    out.push_back(op);
  }
}

// The operations of a plan, in the order they are carried out: the layers' files, the removals,
// and the manifest last, which is the order a reader of a directory without a log would want.
Vector<FileOp> ops_of(const SavePlan& plan) {
  Vector<FileOp> ops;
  ops.reserve(plan.writes().size() + plan.removals().size());
  const PlannedWrite* manifest = nullptr;
  for (const PlannedWrite& w : plan.writes()) {
    if (w.path == DocumentStore::k_manifest_file) {
      manifest = &w;
      continue;
    }
    ops.push_back(FileOp{w.path, w.text, false, w.tile, w.was_size});
  }
  for (const PlannedRemoval& r : plan.removals())
    ops.push_back(FileOp{r.path, {}, true, r.tile});
  if (manifest != nullptr)
    ops.push_back(FileOp{manifest->path, manifest->text, false, false, manifest->was_size});
  return ops;
}

// Where a crash test makes a save die (SaveOptions): after `stop_after` operations, the log being
// the first and its removal the last.
class Mortality {
 public:
  explicit Mortality(const SaveOptions& options)
      : stop_after_(options.stop_after), tear_(options.tear) {}
  // True when the save dies instead of carrying out the next operation.
  bool dies() noexcept {
    if (stop_after_ < 0) return false;
    if (done_ == stop_after_) return true;
    ++done_;
    return false;
  }
  bool torn() const noexcept { return tear_; }

 private:
  i32 stop_after_ = -1;
  i32 done_ = 0;
  bool tear_ = false;
};

// Carries out file operations in order; false, naming the file, at the first that fails, or where
// `death` says the process died.
bool apply_ops(const std::string& root, std::string_view dir, std::span<const FileOp> ops,
               Mortality* death, std::string* error) {
  for (const FileOp& op : ops) {
    const std::string native = join(root, op.path);
    if (death != nullptr && death->dies()) {
      if (death->torn() && !op.remove) (void)overwrite_file(native, op.text, op.was_size, true);
      set_error(error, join(dir, op.path), "the save was stopped here (SaveOptions)");
      return false;
    }
    const io::Status s =
        op.remove ? io::remove_file(native) : overwrite_file(native, op.text, op.was_size);
    if (s != io::Status::Ok && !(op.remove && s == io::Status::NotFound)) {
      set_error(error, join(dir, op.path), io::status_name(s));
      return false;
    }
  }
  return true;
}

// Finishes what a save that did not finish began: carries out a whole log and removes it, or
// removes a torn one, whose save touched nothing. Only a process that is about to write the
// document does this; `load` reads through a log and never acts on it.
bool finish_pending(const std::string& root, std::string_view dir, std::string* error) {
  const std::string log_path = join(root, DocumentStore::k_save_log);
  std::string text;
  const io::Status s = io::read_file(log_path, text);
  if (s == io::Status::NotFound) return true;
  if (s != io::Status::Ok) {
    set_error(error, join(dir, DocumentStore::k_save_log), io::status_name(s));
    return false;
  }
  Vector<FileOp> ops;
  if (parse_log(text, ops) && !apply_ops(root, dir, ops, nullptr, error)) return false;
  if (const io::Status r = io::remove_file(log_path);
      r != io::Status::Ok && r != io::Status::NotFound) {
    set_error(error, join(dir, DocumentStore::k_save_log), io::status_name(r));
    return false;
  }
  return true;
}

// --- carrying a plan out --------------------------------------------------------------------

bool carry_out(const SavePlan& plan, std::string_view dir, const SaveOptions& options,
               std::string* error, SaveReport& report) {
  for (const std::string& d : plan.directories()) {
    if (!ensure_directory(plan.native(d))) {
      set_error(error, join(dir, d), "could not make the directory");
      return false;
    }
  }
  const Vector<FileOp> ops = ops_of(plan);
  if (ops.empty()) return true;

  Mortality death(options);
  const std::string log_path = plan.native(DocumentStore::k_save_log);
  const std::string log = log_text(ops);
  if (death.dies()) {
    if (death.torn()) (void)overwrite_file(log_path, log, k_unknown_size, /*half=*/true);
    set_error(error, join(dir, DocumentStore::k_save_log),
              "the save was stopped here (SaveOptions)");
    return false;
  }
  if (const io::Status s = overwrite_file(log_path, log); s != io::Status::Ok) {
    (void)io::remove_file(log_path);  // a log that is not whole names no save; tidy it anyway
    set_error(error, join(dir, DocumentStore::k_save_log), io::status_name(s));
    return false;
  }
  if (!apply_ops(plan.root(), dir, ops, &death, error)) return false;
  if (death.dies()) {
    set_error(error, join(dir, DocumentStore::k_save_log),
              "the save was stopped here (SaveOptions)");
    return false;
  }
  if (const io::Status s = io::remove_file(log_path); s != io::Status::Ok) {
    set_error(error, join(dir, DocumentStore::k_save_log), io::status_name(s));
    return false;
  }

  for (const FileOp& op : ops) {
    if (op.remove) {
      ++report.files_removed;
      if (op.tile) ++report.tiles_removed;
      report.removed.push_back(std::string(op.path));
    } else {
      ++report.files_written;
      report.bytes_written += op.text.size();
      if (op.tile) ++report.tiles_written;
      report.written.push_back(std::string(op.path));
    }
  }
  return true;
}

// What the store now knows: the files it wrote, and not the ones it removed.
void record(const SavePlan& plan, StoredState& next) {
  for (const PlannedWrite& w : plan.writes())
    next.files.insert_or_assign(w.path, StoredFile::of(w.text));
  for (const PlannedRemoval& r : plan.removals())
    next.files.erase(r.path);
}

// --- loading --------------------------------------------------------------------------------

// The document's files as `load` reads them: the disk, seen through the log of a save that did not
// finish when there is a whole one — what that save would have left — without writing anything, so
// that a reader never changes a directory another process may be saving into.
class DiskView {
 public:
  DiskView(const io::Vfs& vfs, std::string_view dir) : vfs_(vfs), dir_(dir) {}
  DiskView(const DiskView&) = delete;
  DiskView& operator=(const DiskView&) = delete;

  // Looks for a log. A whole one is read through from now on; a torn one is ignored, since its save
  // touched nothing. True when there is a log of either kind: the first save removes it.
  bool open_log() {
    if (vfs_.read(full(DocumentStore::k_save_log), log_) != io::Status::Ok) return false;
    Vector<FileOp> ops;
    if (!parse_log(log_, ops)) return true;
    for (const FileOp& op : ops)
      overlay_.insert_or_assign(std::string(op.path), op);
    return true;
  }

  std::string full(std::string_view path) const { return join(dir_, path); }

  io::Status read(const std::string& path, std::string& out) const {
    if (const FileOp* op = overlay_.find_value(path)) {
      if (op->remove) return io::Status::NotFound;
      out.assign(op->text);
      return io::Status::Ok;
    }
    return vfs_.read(full(path), out);
  }

  bool exists(const std::string& path) const {
    if (const FileOp* op = overlay_.find_value(path)) return !op->remove;
    return vfs_.exists(full(path));
  }

  io::Status list(const std::string& path, Vector<io::DirEntry>& out) const {
    io::Status s = vfs_.list(full(path), out);
    if (overlay_.empty()) return s;
    if (s == io::Status::NotFound) {
      out.clear();
    } else if (s != io::Status::Ok) {
      return s;
    }
    bool changed = false;
    for (u32 i = 0; i < overlay_.size(); ++i) {
      const std::string& p = overlay_.key_at(i);
      if (io::parent_path(p) != path) continue;
      const std::string_view name = io::file_name(p);
      u32 at = 0;
      while (at < out.size() && out[at].name != name)
        ++at;
      if (overlay_.value_at(i).remove) {
        if (at < out.size()) out.erase_at(at);
      } else if (at == out.size()) {
        io::DirEntry e;
        e.name = std::string(name);
        out.push_back(std::move(e));
      }
      changed = true;
    }
    if (!changed) return s;
    std::sort(out.begin(), out.end(),
              [](const io::DirEntry& a, const io::DirEntry& b) { return a.name < b.name; });
    return io::Status::Ok;
  }

 private:
  const io::Vfs& vfs_;
  std::string dir_;
  std::string log_;
  HashMap<std::string, FileOp> overlay_;  // views into log_
};

// Defined with the journal's other helpers, below.
bool parse_journal(std::string_view text, const std::string& path, Vector<Patch>& out,
                   Vector<u64>* ends, std::string* error);

// Reads one file of the document and remembers what it held.
bool read_file(const DiskView& disk, const std::string& path, std::string& out, StoredState& known,
               std::string* error) {
  if (const io::Status s = disk.read(path, out); s != io::Status::Ok) {
    set_error(error, disk.full(path), io::status_name(s));
    return false;
  }
  known.files.insert_or_assign(path, StoredFile::of(out));
  return true;
}

bool load_partitioned(const DiskView& disk, const LayerRef& ref, Layer& out, StoredState& known,
                      std::string* error) {
  const LayerFiles lf = files_of(ref.name);
  const std::string index_path = disk.full(lf.index);
  const std::string untiled_path = disk.full(lf.untiled);
  std::string text;
  LayerIndex index;
  if (!read_file(disk, lf.index, text, known, error) ||
      !parse_schema_text(text, index_path, index, error))
    return false;
  if (index.name != ref.name) {
    set_error(error, index_path,
              "index names layer '" + index.name + "', the manifest says '" + ref.name + "'");
    return false;
  }
  if (!(index.partition.tile_size > 0)) {
    set_error(error, index_path, "partition tile_size must be greater than zero");
    return false;
  }
  if (ref.partition.has_value() && !(*ref.partition == index.partition)) {
    set_error(error, index_path, "the index's partition differs from the manifest's");
    return false;
  }

  out = Layer(ref.name, ref.role);
  out.set_partition(index.partition);
  FlatSet<std::string_view> listed;
  for (u32 i = 0; i < index.tiles.size(); ++i) {
    const TileRef& t = index.tiles[i];
    const TileCoord tile{t.x, t.y};
    if (i > 0 && !(TileCoord{index.tiles[i - 1].x, index.tiles[i - 1].y} < tile)) {
      set_error(error, index_path, "tiles are not in (x, y) order at " + tile_file_name(tile));
      return false;
    }
    if (t.file != tile_file_name(tile)) {
      set_error(error, index_path,
                "tile " + tile_file_name(tile) + " is listed as '" + t.file + "'");
      return false;
    }
    listed.insert(std::string_view(t.file));

    const std::string path = disk.full(lf.tile(t.file));
    LayerFile file;
    if (!read_file(disk, lf.tile(t.file), text, known, error) ||
        !parse_schema_text(text, path, file, error))
      return false;
    if (file.name != ref.name) {
      set_error(error, path,
                "tile names layer '" + file.name + "', the manifest says '" + ref.name + "'");
      return false;
    }
    if (file.objects.size() != t.objects.size()) {
      set_error(error, path,
                "holds " + std::to_string(file.objects.size()) + " records, the index lists " +
                    std::to_string(t.objects.size()));
      return false;
    }
    for (u32 k = 0; k < file.objects.size(); ++k) {
      ObjectRecord& r = file.objects[k];
      if (!(r.id == t.objects[k])) {
        set_error(error, path, "record " + hex_of(r.id) + " is not the one the index lists there");
        return false;
      }
      if (k > 0 && !(t.objects[k - 1] < r.id)) {
        set_error(error, index_path, "records are not in id order at " + hex_of(r.id));
        return false;
      }
      TileCoord actual;
      if (!tile_of(r, index.partition, actual) || !(actual == tile)) {
        set_error(error, path, "record " + hex_of(r.id) + " does not fall in this tile");
        return false;
      }
      if (out.find(r.id) != nullptr) {
        set_error(error, path, "record " + hex_of(r.id) + " is already in another tile");
        return false;
      }
      out.set(std::move(r));
    }
  }

  Vector<io::DirEntry> entries;
  if (disk.list(lf.tiles, entries) == io::Status::Ok) {
    for (const io::DirEntry& e : entries) {
      if (e.is_directory || listed.contains(std::string_view(e.name))) continue;
      set_error(error, disk.full(lf.tile(e.name)), "a tile file the index does not list");
      return false;
    }
  }

  StoredLayer& stored = known.layers[ref.name];
  stored.partitioned = true;
  if (index.untiled.empty()) {
    if (disk.exists(lf.untiled)) {
      set_error(error, untiled_path, "present although the index lists no untiled records");
      return false;
    }
    stored.index = std::move(index);
    fill_places(stored);
    return true;
  }
  LayerFile untiled;
  if (!read_file(disk, lf.untiled, text, known, error) ||
      !parse_schema_text(text, untiled_path, untiled, error))
    return false;
  if (untiled.objects.size() != index.untiled.size()) {
    set_error(error, untiled_path,
              "holds " + std::to_string(untiled.objects.size()) + " records, the index lists " +
                  std::to_string(index.untiled.size()));
    return false;
  }
  for (u32 k = 0; k < untiled.objects.size(); ++k) {
    ObjectRecord& r = untiled.objects[k];
    if (!(r.id == index.untiled[k])) {
      set_error(error, untiled_path, "record " + hex_of(r.id) + " is not the one the index lists");
      return false;
    }
    TileCoord tile;
    if (tile_of(r, index.partition, tile)) {
      set_error(
          error, untiled_path,
          "record " + hex_of(r.id) + " has a position and belongs in " + tile_file_name(tile));
      return false;
    }
    if (out.find(r.id) != nullptr) {
      set_error(error, untiled_path, "record " + hex_of(r.id) + " is already in a tile");
      return false;
    }
    out.set(std::move(r));
  }
  stored.index = std::move(index);
  fill_places(stored);
  return true;
}

// True when every file of `layer` on disk holds the bytes a save would write for it. A file that
// does not — written by hand, or by something other than this store — is rewritten by the next
// save, exactly as a store that rewrote everything would have done.
bool canonical_on_disk(const Layer& layer, const StoredState& known) {
  auto same = [&](const std::string& path, std::string_view text) {
    const StoredFile* f = known.files.find_value(path);
    return f != nullptr && *f == StoredFile::of(text);
  };
  const LayerFiles lf = files_of(layer.name());
  if (!layer.partitioned()) return same(lf.single, layer.to_json_text());
  const StoredLayer* stored = known.layers.find_value(layer.name());
  if (stored == nullptr) return false;
  const LayerIndex& index = stored->index;
  if (!same(lf.index, index_text(build_layer_index(layer)))) return false;
  for (const TileRef& t : index.tiles) {
    if (!same(lf.tile(t.file), file_text(layer.to_file(t.objects)))) return false;
  }
  return index.untiled.empty() || same(lf.untiled, file_text(layer.to_file(index.untiled)));
}

}  // namespace

std::string DocumentStore::path_of(std::string_view dir, std::string_view file) {
  std::string p(dir);
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
  p.append(file);
  return p;
}

std::string DocumentStore::layer_dir_name(std::string_view layer_name) {
  std::string name;
  name.reserve(layer_name.size());
  for (const char c : layer_name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    name.push_back(ok ? c : '_');
  }
  if (name.empty()) name = "layer";
  return name;
}

std::string DocumentStore::layer_file_name(std::string_view layer_name) {
  std::string name = layer_dir_name(layer_name);
  name.append(".json");
  return name;
}

bool DocumentStore::exists(const io::Vfs& vfs, std::string_view dir) {
  // A save log alone is a document too: the very first save of one that died after writing it.
  return vfs.exists(path_of(dir, k_manifest_file)) || vfs.exists(path_of(dir, k_save_log));
}

bool DocumentStore::create(const io::Vfs& vfs, std::string_view dir, std::string_view name,
                           Document& out, DocumentManifest& manifest, std::string* error) {
  if (exists(vfs, dir)) {
    set_error(error, dir, "a document already exists here");
    return false;
  }
  out = Document{};
  out.add_layer(std::string(k_default_layer), LayerRole::Base);
  out.set_edit_layer(0);
  manifest = DocumentManifest{};
  manifest.name = std::string(name);
  manifest.undo_position = 0;
  if (!write_journal(vfs, dir, {}, error)) return false;
  if (!save(vfs, dir, out, manifest, error)) return false;
  out.stored_->journal_known = true;  // empty, as write_journal left it
  return true;
}

bool DocumentStore::save(const io::Vfs& vfs, std::string_view dir, Document& doc,
                         DocumentManifest& manifest, std::string* error, SaveReport* report,
                         const SaveOptions& options) {
  ENGINE_PROFILE_ZONE_NAMED("doc.store.save");
  SaveReport discarded;
  SaveReport& stats = report != nullptr ? *report : discarded;
  stats = SaveReport{};

  std::string root;
  if (const io::Status s = vfs.resolve(dir, root, /*for_write=*/true); s != io::Status::Ok) {
    set_error(error, dir, io::status_name(s));
    return false;
  }
  // What the store knows of this directory: everything, when this document was loaded from it or
  // saved to it last and nothing has said otherwise; nothing, and every file is written, when it
  // is new, was saved somewhere else, or was marked all dirty.
  const bool known = doc.stored_ != nullptr && doc.stored_->root == root;
  // A save that did not finish is finished before this one starts: the one `load` read through,
  // or, when nothing is known, one that may be there.
  if (!known || doc.stored_->log_pending) {
    if (!finish_pending(root, dir, error)) return false;
    if (known) doc.stored_->log_pending = false;
  }
  std::unique_ptr<StoredState> fresh;
  if (!known) {
    fresh = std::make_unique<StoredState>();
    fresh->root = root;
  }
  StoredState& next = known ? *doc.stored_ : *fresh;
  SavePlan plan(root, next, !known);

  const Document& document = doc;  // the const accessor: reading a layer is not an edit
  if (!known && !ensure_directory(join(root, k_layers_dir))) {
    set_error(error, join(dir, k_layers_dir), "could not make the directory");
    return false;
  }

  const Vector<LayerRef> previous = manifest.layers;
  manifest.layers.clear();
  for (u32 i = 0; i < document.layer_count(); ++i) {
    const Layer& layer = document.layer(i);
    const LayerChanges& changes = document.changes(i);
    const LayerFiles lf = files_of(layer.name());
    LayerRef ref;
    ref.name = layer.name();
    ref.role = layer.role();

    StoredLayer* was = next.layers.find_value(layer.name());
    const bool same_form = was != nullptr && was->partitioned == layer.partitioned() &&
                           (!layer.partitioned() || was->index.partition == layer.partition());
    if (layer.partitioned()) {
      ref.file = layer_dir_name(layer.name()) + "/" + std::string(k_index_file);
      ref.partition = layer.partition();
      if (!same_form || changes.whole) {
        StoredLayer rebuilt;
        plan_partitioned_whole(plan, layer, lf, was, rebuilt);
        next.layers.insert_or_assign(layer.name(), std::move(rebuilt));
      } else if (!changes.ids.empty()) {
        plan_partitioned_changes(plan, layer, lf, *was, changes.ids);
      }
      stats.tiles_total += next.layers.find_value(layer.name())->index.tiles.size();
    } else {
      ref.file = layer_file_name(layer.name());
      if (!same_form || changes.any()) {
        StoredLayer single;
        plan_single(plan, layer, lf, was, single);
        next.layers.insert_or_assign(layer.name(), std::move(single));
      }
    }
    manifest.layers.push_back(std::move(ref));
  }

  // Files of layers the document no longer has: the ones the store knows, or, when it knows
  // nothing, whatever the manifest it was handed named.
  Vector<std::string> gone;
  for (const std::string& name : next.layers.keys()) {
    if (document.find_layer(name) < 0) gone.push_back(name);
  }
  for (const std::string& name : gone) {
    const StoredLayer& was = *next.layers.find_value(name);
    const LayerFiles lf = files_of(name);
    if (was.partitioned) {
      remove_partition_known(plan, lf, was);
    } else {
      plan.remove_known(lf.single, false);
    }
    next.layers.erase(name);
  }
  if (!known) {
    for (const LayerRef& ref : previous) {
      if (document.find_layer(ref.name) >= 0) continue;
      const LayerFiles lf = files_of(ref.name);
      remove_partition_present(plan, lf);
      plan.remove_if_present(lf.single, false);
    }
  }

  manifest.edit_layer =
      document.layer_count() > 0 ? document.layer(document.edit_layer()).name() : "";
  plan.consider(std::string(k_manifest_file), manifest_text(manifest), false);

  if (!carry_out(plan, dir, options, error, stats)) {
    // The disk holds the old state and perhaps a log of this one: the next save knows nothing,
    // finishes the log if it is whole, and looks at everything.
    doc.stored_.reset();
    return false;
  }
  record(plan, next);
  if (!known) doc.stored_ = std::move(fresh);
  doc.clear_dirty();
  return true;
}

bool DocumentStore::repartition(const io::Vfs& vfs, std::string_view dir, Document& doc,
                                DocumentManifest& manifest, u32 layer_index,
                                const LayerPartition& partition, std::string* error,
                                SaveReport* report) {
  if (layer_index >= doc.layer_count()) {
    set_error(error, dir, "no layer " + std::to_string(layer_index));
    return false;
  }
  // Anything without a positive tile size is "one file", spelled one way.
  doc.set_layer_partition(layer_index, partition.tile_size > 0 ? partition : LayerPartition{});
  return save(vfs, dir, doc, manifest, error, report);
}

bool DocumentStore::load(const io::Vfs& vfs, std::string_view dir, Document& out,
                         DocumentManifest& manifest, std::string* error) {
  ENGINE_PROFILE_ZONE_NAMED("doc.store.load");
  auto known = std::make_unique<StoredState>();
  (void)vfs.resolve(dir, known->root);
  DiskView disk(vfs, dir);
  // A whole save log means a save stopped after it had begun changing files: what is read is what
  // it would have left, and the first save of this document finishes it on disk (or removes a torn
  // one, whose save touched nothing).
  known->log_pending = disk.open_log();
  const std::string manifest_path = path_of(dir, k_manifest_file);
  std::string text;
  if (!read_file(disk, std::string(k_manifest_file), text, *known, error) ||
      !parse_schema_text(text, manifest_path, manifest, error))
    return false;

  Document doc;
  for (const LayerRef& ref : manifest.layers) {
    Layer layer(ref.name, ref.role);
    if (ref.partition.has_value() && ref.partition->tile_size > 0) {
      if (!load_partitioned(disk, ref, layer, *known, error)) return false;
    } else {
      const std::string rel = join(k_layers_dir, ref.file);
      std::string layer_text;
      if (!read_file(disk, rel, layer_text, *known, error)) return false;
      schema::ReadContext layer_ctx;
      if (!Layer::from_json_text(layer_text, layer, layer_ctx)) {
        set_error(error, join(dir, rel), layer_ctx);
        return false;
      }
      StoredLayer& stored = known->layers[ref.name];
      stored.partitioned = false;
    }
    doc.add_layer(std::move(layer));
  }
  if (doc.layer_count() == 0) {
    set_error(error, manifest_path, "document has no layers");
    return false;
  }
  const i32 edit = doc.find_layer(manifest.edit_layer);
  doc.set_edit_layer(edit >= 0 ? static_cast<u32>(edit) : doc.layer_count() - 1);

  // The journal, and where each of its lines ends: what lets the next append cut a dropped redo
  // tail, or a line an append died in, off in place.
  Vector<Patch> journal;
  {
    const std::string journal_path = path_of(dir, k_journal_file);
    std::string journal_text;
    const io::Status s = vfs.read(journal_path, journal_text);
    if (s != io::Status::Ok && s != io::Status::NotFound) {
      set_error(error, journal_path, io::status_name(s));
      return false;
    }
    if (!parse_journal(journal_text, journal_path, journal, &known->journal_ends, error))
      return false;
    known->journal_known = true;
    known->journal_size = journal_text.size();
  }
  if (manifest.undo_position > journal.size()) manifest.undo_position = journal.size();
  doc.set_journal(std::move(journal));

  // What is in memory is what is on disk — except a layer whose files are not in the canonical
  // form, which the next save writes as it would have written them itself.
  doc.clear_dirty();
  for (u32 i = 0; i < doc.layer_count(); ++i) {
    const Document& read = doc;
    if (!canonical_on_disk(read.layer(i), *known)) doc.changes_[i].whole = true;
  }
  doc.stored_ = std::move(known);
  out = std::move(doc);
  return true;
}

// --- the journal and the files beside the document ------------------------------------------

namespace {

std::string journal_line(const Patch& patch) {
  std::string line = write_json(schema::to_json(patch), JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  return line;
}

// Appends, making the directory when the append finds it missing rather than asking first, which
// is what `Vfs::append` does and what costs more than the append on Windows.
io::Status append_native(const std::string& native, std::string_view text) {
  io::Status s = io::append_file(native, text);
  if (s == io::Status::NotFound) {
    const std::string_view parent = io::parent_path(native);
    if (!parent.empty() && io::make_directories(parent) == io::Status::Ok)
      s = io::append_file(native, text);
  }
  return s;
}

// Replaces a file whole and atomically, the same way about its directory.
io::Status replace_native(const std::string& native, std::string_view text) {
  io::Status s = io::write_file_atomic(native, text);
  if (s == io::Status::NotFound) {
    const std::string_view parent = io::parent_path(native);
    if (!parent.empty() && io::make_directories(parent) == io::Status::Ok)
      s = io::write_file_atomic(native, text);
  }
  return s;
}

// The journal's patches, and where the line of each ends. A last line with no newline is an append
// a process died in the middle of — the store writes a line and its newline in one write — so it
// is left out, whatever it holds: its commit's save never ran, and the manifest's undo position
// does not count it.
bool parse_journal(std::string_view text, const std::string& path, Vector<Patch>& out,
                   Vector<u64>* ends, std::string* error) {
  out.clear();
  if (ends != nullptr) ends->clear();
  usize pos = 0;
  u32 line_number = 0;
  while (pos < text.size()) {
    const usize nl = text.find('\n', pos);
    if (nl == std::string_view::npos) break;  // torn
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    JsonValue value;
    if (const JsonParseResult r = parse_json(line, value); !r.ok) {
      set_error(error, path, std::string("line ") + std::to_string(line_number) + ": " + r.message);
      return false;
    }
    Patch patch;
    schema::ReadContext ctx;
    if (!schema::from_json(patch, value, ctx)) {
      set_error(error, path, ctx);
      return false;
    }
    out.push_back(std::move(patch));
    if (ends != nullptr) ends->push_back(pos);
  }
  return true;
}

}  // namespace

bool DocumentStore::append_journal(const io::Vfs& vfs, std::string_view dir, const Patch& patch,
                                   std::string* error) {
  const std::string path = path_of(dir, k_journal_file);
  std::string native;
  io::Status s = vfs.resolve(path, native, /*for_write=*/true);
  if (s == io::Status::Ok) s = append_native(native, journal_line(patch));
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::append_journal(const io::Vfs& vfs, std::string_view dir, Document& doc,
                                   std::string* error) {
  if (doc.journal().empty()) return true;
  const std::string path = path_of(dir, k_journal_file);
  std::string root;
  if (const io::Status s = vfs.resolve(dir, root, /*for_write=*/true); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  const std::string native = join(root, k_journal_file);
  StoredState* known =
      doc.stored_ != nullptr && doc.stored_->root == root ? doc.stored_.get() : nullptr;
  const u32 kept = doc.journal().size() - 1;  // the patches the file should already hold
  if (known != nullptr && known->journal_known && known->journal_ends.size() >= kept) {
    // The file holds those patches and perhaps more: a redo tail this commit dropped, or a line an
    // append died in. Cut it after them, in place, then append the new line.
    const u64 cut = kept == 0 ? 0 : known->journal_ends[kept - 1];
    if (cut != known->journal_size) {
      std::error_code ec;
      std::filesystem::resize_file(
          std::filesystem::path(
              std::u8string_view(reinterpret_cast<const char8_t*>(native.data()), native.size())),
          cut, ec);
      if (ec) {
        set_error(error, path, ec.message());
        return false;
      }
      known->journal_ends.resize(kept);
      known->journal_size = cut;
    }
    const std::string line = journal_line(doc.journal().back());
    if (const io::Status s = append_native(native, line); s != io::Status::Ok) {
      known->journal_known = false;  // it may hold half the line now: write it whole next time
      set_error(error, path, io::status_name(s));
      return false;
    }
    known->journal_size = cut + line.size();
    known->journal_ends.push_back(known->journal_size);
    return true;
  }
  // Nothing known of the file: write it whole, as the document has it.
  std::string text;
  Vector<u64> ends;
  for (const Patch& patch : doc.journal()) {
    write_json(schema::to_json(patch), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
    ends.push_back(text.size());
  }
  if (const io::Status s = replace_native(native, text); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  if (known != nullptr) {
    known->journal_known = true;
    known->journal_ends = std::move(ends);
    known->journal_size = text.size();
  }
  return true;
}

bool DocumentStore::write_journal(const io::Vfs& vfs, std::string_view dir,
                                  std::span<const Patch> patches, std::string* error) {
  std::string text;
  for (const Patch& patch : patches) {
    write_json(schema::to_json(patch), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
  }
  const std::string path = path_of(dir, k_journal_file);
  std::string native;
  io::Status s = vfs.resolve(path, native, /*for_write=*/true);
  if (s == io::Status::Ok) s = replace_native(native, text);
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::load_journal(const io::Vfs& vfs, std::string_view dir, Vector<Patch>& out,
                                 std::string* error) {
  out.clear();
  const std::string path = path_of(dir, k_journal_file);
  std::string text;
  const io::Status s = vfs.read(path, text);
  if (s == io::Status::NotFound) return true;  // no journal yet
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return parse_journal(text, path, out, nullptr, error);
}

bool DocumentStore::write_side_file(const io::Vfs& vfs, std::string_view path,
                                    std::string_view text, std::string* error) {
  std::string native;
  io::Status s = vfs.resolve(path, native, /*for_write=*/true);
  if (s == io::Status::Ok) {
    std::string existing;
    if (io::read_file(native, existing) == io::Status::Ok && existing == text) return true;
    s = replace_native(native, text);
  }
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::append_side_file(const io::Vfs& vfs, std::string_view path,
                                     std::string_view text, std::string* error) {
  std::string native;
  io::Status s = vfs.resolve(path, native, /*for_write=*/true);
  if (s == io::Status::Ok) s = append_native(native, text);
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

}  // namespace engine::doc
