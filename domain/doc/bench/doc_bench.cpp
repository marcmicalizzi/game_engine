// What the composed index buys (docs/subsystems/doc.md, docs/plan/03-data-model.md §3.2).
//
// The ".linear" benchmarks are the composition this module used to do: walk every layer for
// every question, and answer objects() and children() by resolving every id. They live here and
// in the tests, never in the API, so the comparison stays honest and the API has one way to ask.
//
// What the numbers say (docs/subsystems/doc.md): objects() and children() were quadratic and are
// not any more; resolve() and property() are a wash over a shallow stack, because a hash lookup
// into a large map costs about what the two FlatMap searches it replaces cost. Keep both halves
// in the table rather than reporting only the wins.
//
// The "doc.store.*" benchmarks at the end are what a commit costs on disk: a transaction, its
// journal line and the save, on two declared document shapes, with the files and bytes each save
// wrote as the items and bytes columns. The "doc.fs.*" ones are the file operations a save is
// made of, one at a time and at four sizes, which is how to tell whether this file system charges
// by the operation or by the byte.
#include <core/ids/id128.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>
#include <foundation/bench/bench.h>
#include <foundation/io/vfs.h>

#include <test_temp_dir.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <schemas/provenance.h>
#include <string>

#if ENGINE_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_type = "engine.content.AssetProvenance";
constexpr u32 k_fanout = 8;     // children per parent
constexpr u32 k_probes = 1024;  // ids or parents visited per timed iteration

ObjectId id_of(u32 n) { return Id128::from_parts(1, n + 1); }

// A document of `count` objects in a two-layer stack: a base layer holding the tree, and a
// feature layer overriding one property of every fourth object, which is what makes composition
// more than a single lookup.
Document make_document(u32 count) {
  Document d;
  Layer base("base", LayerRole::Base);
  Layer feature("feature", LayerRole::Feature);
  for (u32 i = 0; i < count; ++i) {
    ObjectRecord r;
    r.id = id_of(i);
    r.type = k_type;
    r.parent = i < k_fanout ? ObjectId{} : id_of(i / k_fanout);
    r.properties.insert_or_assign("generator", JsonValue("base-gen"));
    r.properties.insert_or_assign(
        "position",
        JsonValue(JsonValue::Array{JsonValue(static_cast<f64>(i % 997) * 3.5), JsonValue(0.0),
                                   JsonValue(static_cast<f64>(i / 997) * 3.5)}));
    base.set(std::move(r));
    if (i % 4 != 0) continue;
    ObjectRecord o;
    o.id = id_of(i);
    o.properties.insert_or_assign("license", JsonValue("MIT"));
    feature.set(std::move(o));
  }
  d.add_layer(std::move(base));
  d.add_layer(std::move(feature));
  d.rebuild_index();
  return d;
}

// ---- the linear composition, kept out of the API ---------------------------------------------

bool linear_resolve(const Document& d, ObjectId id, ResolvedObject& out) {
  out = ResolvedObject{};
  out.id = id;
  bool defined = false;
  u32 defining = 0;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r != nullptr && !r->type.empty()) {
      defined = true;
      defining = i;
      out.type = r->type;
    }
  }
  if (!defined) return false;
  out.defining_layer = defining;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r == nullptr) continue;
    if (r->parent.has_value()) out.parent = *r->parent;
    if (r->deleted && i >= defining) out.deleted = true;
    for (auto [name, value] : r->properties)
      out.properties.insert_or_assign(std::string_view(name), &value);
  }
  return true;
}

const JsonValue* linear_property(const Document& d, ObjectId id, std::string_view name) {
  const JsonValue* found = nullptr;
  bool defined = false;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r == nullptr) continue;
    if (!r->type.empty()) defined = true;
    if (const JsonValue* v = r->properties.find_value(name)) found = v;
  }
  return defined ? found : nullptr;
}

Vector<ObjectId> linear_objects(const Document& d) {
  Vector<ObjectId> ids;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    for (auto [id, record] : d.layer(i).records()) {
      if (!record.type.empty()) ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  Vector<ObjectId> out;
  out.reserve(ids.size());
  for (u32 i = 0; i < ids.size(); ++i) {
    if (i > 0 && ids[i] == ids[i - 1]) continue;
    ResolvedObject r;
    if (linear_resolve(d, ids[i], r) && !r.deleted) out.push_back(ids[i]);
  }
  return out;
}

Vector<ObjectId> linear_children(const Document& d, ObjectId parent) {
  Vector<ObjectId> out;
  for (const ObjectId id : linear_objects(d)) {
    ResolvedObject r;
    if (linear_resolve(d, id, r) && r.parent == parent) out.push_back(id);
  }
  return out;
}

}  // namespace

// ---- resolve ---------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(resolve_index, "doc.resolve.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i) {
      ResolvedObject r;
      if (d.resolve(id_of((i * 7919) % count), r)) sum += r.properties.size();
    }
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

ENGINE_BENCH_ARGS(resolve_linear, "doc.resolve.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i) {
      ResolvedObject r;
      if (linear_resolve(d, id_of((i * 7919) % count), r)) sum += r.properties.size();
    }
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

// ---- one composed property -------------------------------------------------------------------
//
// resolve() composes a whole property map and allocates for it, which hides what the index does;
// property() is the same lookup without that, so this is where the layer walk shows up.

ENGINE_BENCH_ARGS(property_index, "doc.property.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 found = 0;
    for (u32 i = 0; i < k_probes; ++i)
      found += d.property(id_of((i * 7919) % count), "generator") != nullptr ? 1 : 0;
    bench::keep(found);
  }
  state.set_items(k_probes);
}

ENGINE_BENCH_ARGS(property_linear, "doc.property.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 found = 0;
    for (u32 i = 0; i < k_probes; ++i)
      found += linear_property(d, id_of((i * 7919) % count), "generator") != nullptr ? 1 : 0;
    bench::keep(found);
  }
  state.set_items(k_probes);
}

// ---- objects ---------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(objects_index, "doc.objects.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running())
    bench::keep(d.objects().size());
  state.set_items(count);
}

ENGINE_BENCH_ARGS(objects_linear, "doc.objects.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running())
    bench::keep(linear_objects(d).size());
  state.set_items(count);
}

// ---- children --------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(children_index, "doc.children.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i)
      sum += d.children(id_of((i * 7919) % count)).size();
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

// One parent per iteration: the linear form walks the whole document for each one, so asking it
// for a thousand parents at 100k records takes minutes.
ENGINE_BENCH_ARGS(children_linear, "doc.children.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  u32 probe = 0;
  while (state.keep_running()) {
    bench::keep(linear_children(d, id_of((probe++ * 7919) % count)).size());
  }
  state.set_items(1);
}

// ---- what maintaining the index costs ----------------------------------------------------------

// Setting a property on a record the edit layer already has: the precondition check, the value
// assignment, and the index maintenance, without the cost of growing the layer's FlatMap, which
// belongs to the container and would swamp the rest.
ENGINE_BENCH_ARGS(apply_set_property, "doc.apply.set_property", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  Document d = make_document(count);
  d.set_edit_layer(1);
  u32 n = 0;
  for (u32 i = 0; i < k_probes; ++i)  // setup: every probed record exists in the edit layer
    d.apply(cmd_set(id_of((i * 7919) % count), "prompt_hash", JsonValue(u64{0})), nullptr, nullptr);
  while (state.keep_running()) {
    for (u32 i = 0; i < k_probes; ++i) {
      const ObjectId id = id_of((i * 7919) % count);
      d.apply(cmd_set(id, "prompt_hash", JsonValue(static_cast<u64>(n++))), nullptr, nullptr);
    }
  }
  state.set_items(k_probes);
}

// ---- partitioning ------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(build_index_tiles, "doc.partition.build_index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  Document d = make_document(count);
  LayerPartition partition;
  partition.property = "position";
  partition.tile_size = 64;
  d.set_layer_partition(0, partition);
  const Document& document = d;
  while (state.keep_running())
    bench::keep(build_layer_index(document.layer(0)).tiles.size());
  state.set_items(count);
}

// ---- the store: what a commit costs on disk ----------------------------------------------------
//
// Two document shapes, each built once per measurement in a scratch directory of its own:
//
//   store      eight layers and 10^4 records: `world`, partitioned into 8 x 8 tiles of 64 m and
//              holding 8,000 placements, and seven single-file feature layers of 286 overrides.
//   e12        E12's document (docs/experiments/e12-proposal-layers-and-leases.md): an empty
//              `base`, `world` tiled 10 m a side with six props in each of its 8 x 8 tiles, and
//              one proposal layer per agent over it — the argument — stored in world's form and
//              holding the ten overrides an agent has made in its block.
//
// A commit is what `Session::commit_commands` does for one `doc.apply`: a transaction of one
// SetProperty on the named layer, the journal line, the manifest's undo position, and the save.
// The items column is the files the commit wrote (the journal line counts as one) and the bytes
// column what they held, both averaged over the iterations.

namespace {

constexpr u32 k_grid = 8;  // tiles a side, in both shapes

struct StoreShape {
  f64 tile = 64.0;
  u32 world_records = 8000;
  u32 plain_layers = 7;      // single-file feature layers
  u32 plain_records = 286;   // overrides in each
  u32 proposals = 0;         // partitioned proposal layers over world
  u32 proposal_records = 0;  // overrides in each
};

// A tenth of the records under CTest's smoke run, which asks whether the rows run, not what they
// cost: building and saving 10^4 records five times is seconds in a debug build.
StoreShape declared_shape() {
  StoreShape s;
  if (bench::smoke_mode()) {
    s.world_records /= 10;
    s.plain_records /= 10;
  }
  return s;
}

StoreShape e12_shape(u32 agents) {
  StoreShape s;
  s.tile = 10.0;
  s.world_records = k_grid * k_grid * 6;
  s.plain_layers = 0;
  s.plain_records = 0;
  s.proposals = agents;
  s.proposal_records = 10;
  return s;
}

ObjectId store_id(u32 n) { return Id128::from_parts(7, n + 1); }

// A record of `world`, placed in tile n % 64 so consecutive ids spread over the grid.
ObjectRecord placement(u32 n, f64 tile) {
  const u32 t = n % (k_grid * k_grid);
  const f64 inside = static_cast<f64>((n / (k_grid * k_grid)) % 97) / 97.0 * (tile * 0.9);
  const f64 x = static_cast<f64>(t % k_grid) * tile + inside + tile * 0.05;
  const f64 z = static_cast<f64>(t / k_grid) * tile + inside + tile * 0.05;
  ObjectRecord r;
  r.id = store_id(n);
  r.type = k_type;
  r.parent = ObjectId{};
  r.properties.insert_or_assign("generator", JsonValue("prop-" + std::to_string(n)));
  r.properties.insert_or_assign(
      "position", JsonValue(JsonValue::Array{JsonValue(x), JsonValue(0.0), JsonValue(z)}));
  return r;
}

ObjectRecord override_of(u32 n, std::string_view value) {
  ObjectRecord r;
  r.id = store_id(n);
  r.properties.insert_or_assign("license", JsonValue(std::string(value)));
  return r;
}

// Builds the shape in memory and saves it whole, as a fresh document would be, so the measured
// commits start from a directory the store wrote itself.
struct StoreFixture {
  engine::test::TempDir tmp{"engine_doc_store_bench"};
  io::Vfs vfs;
  std::string dir = "docs://world";
  Document doc;
  DocumentManifest manifest;
  u32 world = 0;
  u32 first_plain = 0;
  u32 first_proposal = 0;
  StoreShape shape;

  explicit StoreFixture(const StoreShape& s) : shape(s) {
    (void)vfs.mount("docs", tmp.path(), /*writable=*/true);
    std::string error;
    ENGINE_VERIFY(DocumentStore::create(vfs, dir, "Bench", doc, manifest, &error),
                  "doc store bench: create failed");
    LayerPartition partition;
    partition.property = "position";
    partition.tile_size = s.tile;

    Layer world_layer("world", LayerRole::Feature);
    for (u32 i = 0; i < s.world_records; ++i)
      world_layer.set(placement(i, s.tile));
    world = doc.add_layer(std::move(world_layer));
    doc.set_layer_partition(world, partition);

    first_plain = doc.layer_count();
    for (u32 l = 0; l < s.plain_layers; ++l) {
      Layer layer("feature" + std::to_string(l), LayerRole::Feature);
      for (u32 k = 0; k < s.plain_records; ++k)
        layer.set(override_of((l * s.plain_records + k) * 3 % s.world_records, "MIT"));
      doc.add_layer(std::move(layer));
    }
    first_proposal = doc.layer_count();
    for (u32 p = 0; p < s.proposals; ++p) {
      Layer layer("proposal" + std::to_string(p), LayerRole::Proposal);
      // Ten overrides of props in the agent's own tile (p % 64). Half of them moved the prop, so
      // carry a position and are filed in a tile of the proposal's own; the other half set only
      // a property and are filed in its untiled.json.
      for (u32 k = 0; k < s.proposal_records; ++k) {
        const u32 n = (p + k * k_grid * k_grid) % s.world_records;
        ObjectRecord r = override_of(n, "draft");
        if (k % 2 == 0) {
          r.properties.insert_or_assign("position",
                                        *placement(n, s.tile).properties.find_value("position"));
        }
        layer.set(std::move(r));
      }
      const u32 index = doc.add_layer(std::move(layer));
      doc.set_layer_partition(index, partition);
    }
    doc.set_edit_layer(world);
    ENGINE_VERIFY(DocumentStore::save(vfs, dir, doc, manifest, &error),
                  "doc store bench: first save failed");
  }

  // One doc.apply: a SetProperty on `layer`'s record for `id`, journaled and saved. Adds the files
  // and bytes the commit wrote.
  void commit(u32 layer, ObjectId id, const char* property, u64 value, u64& files, u64& bytes) {
    doc.set_edit_layer(layer);
    Attribution who;
    who.actor = "bench";
    who.role = "bench";
    who.timestamp_unix_ms = 1;
    {
      Transaction tx = doc.begin(who);
      tx.apply(cmd_set(id, property, JsonValue(value)), /*strict=*/false);
      tx.commit();
    }
    std::string error;
    ENGINE_VERIFY(DocumentStore::append_journal(vfs, dir, doc, &error),
                  "doc store bench: journal append failed");
    manifest.undo_position = doc.journal().size();
    SaveReport report;
    ENGINE_VERIFY(DocumentStore::save(vfs, dir, doc, manifest, &error, &report),
                  "doc store bench: save failed");
    const std::string line =
        write_json(schema::to_json(doc.journal().back()), JsonWriteOptions{.pretty = false});
    files += report.files_written + 1u;
    bytes += report.bytes_written + line.size() + 1u;
  }
};

void report_io(bench::State& state, u64 files, u64 bytes, u64 count) {
  if (count == 0) return;
  state.set_items((files + count / 2) / count);
  state.set_bytes((bytes + count / 2) / count);
}

}  // namespace

// A commit to one record of the partitioned `world` layer: the edit that touches one tile.
ENGINE_BENCH(store_commit_tile, "doc.store.commit.tile") {
  StoreFixture f(declared_shape());
  u64 files = 0, bytes = 0, commits = 0;
  while (state.keep_running()) {
    const u32 n = static_cast<u32>((commits * 7919) % f.shape.world_records);
    f.commit(f.world, store_id(n), "generator", commits, files, bytes);
    ++commits;
  }
  report_io(state, files, bytes, commits);
}

// A commit to one record of a single-file feature layer: the edit that rewrites a whole layer file.
ENGINE_BENCH(store_commit_plain, "doc.store.commit.plain") {
  StoreFixture f(declared_shape());
  u64 files = 0, bytes = 0, commits = 0;
  while (state.keep_running()) {
    const u32 k = static_cast<u32>(commits % f.shape.plain_records);
    f.commit(f.first_plain, store_id(k * 3 % f.shape.world_records), "license", commits, files,
             bytes);
    ++commits;
  }
  report_io(state, files, bytes, commits);
}

// A save with nothing to save: what `doc.save` costs, and the floor under every commit.
ENGINE_BENCH(store_save_unchanged, "doc.store.save.unchanged") {
  StoreFixture f(declared_shape());
  u64 files = 0, bytes = 0, saves = 0;
  std::string error;
  while (state.keep_running()) {
    SaveReport report;
    ENGINE_VERIFY(DocumentStore::save(f.vfs, f.dir, f.doc, f.manifest, &error, &report),
                  "doc store bench: save failed");
    files += report.files_written;
    bytes += report.bytes_written;
    ++saves;
  }
  report_io(state, files, bytes, saves);
}

// E12's edit: one agent's commit to its own proposal layer, with 4 or 16 agents keeping one each.
ENGINE_BENCH_ARGS(store_commit_e12, "doc.store.commit.e12", 4, 16) {
  const u32 agents = static_cast<u32>(state.arg());
  StoreFixture f(e12_shape(agents));
  u64 files = 0, bytes = 0, commits = 0;
  while (state.keep_running()) {
    const u32 p = static_cast<u32>(commits % agents);
    const u32 k = static_cast<u32>((commits / agents) % f.shape.proposal_records);
    const u32 n = (p + k * k_grid * k_grid) % f.shape.world_records;
    f.commit(f.first_proposal + p, store_id(n), "license", commits, files, bytes);
    ++commits;
  }
  report_io(state, files, bytes, commits);
}

// ---- the file operations a save is made of -----------------------------------------------------
//
// One operation per iteration on a native path in a scratch directory, at 64 B, 4 KiB, 64 KiB and
// 1 MiB. A cost that barely moves from 64 B to 64 KiB is charged per operation; one that grows
// with the size is charged per byte.

namespace {

std::string payload(i64 size) { return std::string(static_cast<usize>(size), 'x'); }

}  // namespace

// What `Vfs::write` does: a sibling temporary file, written and closed, renamed over the target.
ENGINE_BENCH_ARGS(fs_write_atomic, "doc.fs.write_atomic", 64, 4096, 65536, 1048576) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  (void)io::write_file(path, data);
  while (state.keep_running())
    bench::keep(io::write_file_atomic(path, data));
  state.set_items(1);
  state.set_bytes(data.size());
}

// What the store does (document_store.cpp, overwrite_once): the file opened for writing alone and
// without truncating it, written from the start, cut to the new length, closed. Spelled again here,
// since the store keeps it private; the two must stay the same calls.
ENGINE_BENCH_ARGS(fs_overwrite, "doc.fs.overwrite", 64, 4096, 65536, 1048576) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  (void)io::write_file(path, data);
  while (state.keep_running()) {
#if ENGINE_PLATFORM_WINDOWS
    int fd = -1;
    if (_wsopen_s(&fd, std::filesystem::path(path).c_str(),
                  _O_WRONLY | _O_BINARY | _O_CREAT | _O_NOINHERIT, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE) != 0)
      continue;
    bench::keep(_write(fd, data.data(), static_cast<unsigned>(data.size())));
    bench::keep(_chsize_s(fd, static_cast<long long>(data.size())));
    bench::keep(_close(fd));
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) continue;
    bench::keep(::write(fd, data.data(), data.size()));
    bench::keep(::ftruncate(fd, static_cast<off_t>(data.size())));
    bench::keep(::close(fd));
#endif
  }
  state.set_items(1);
  state.set_bytes(data.size());
}

namespace {

// The store's write, of `n` bytes of `data`.
bool overwrite(const std::string& path, const std::string& data, usize n) {
#if ENGINE_PLATFORM_WINDOWS
  int fd = -1;
  if (_wsopen_s(&fd, std::filesystem::path(path).c_str(),
                _O_WRONLY | _O_BINARY | _O_CREAT | _O_NOINHERIT, _SH_DENYNO,
                _S_IREAD | _S_IWRITE) != 0)
    return false;
  bool ok = _write(fd, data.data(), static_cast<unsigned>(n)) == static_cast<int>(n);
  ok = _chsize_s(fd, static_cast<long long>(n)) == 0 && ok;
  return _close(fd) == 0 && ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
  if (fd < 0) return false;
  bool ok = ::write(fd, data.data(), n) == static_cast<ssize_t>(n);
  ok = ::ftruncate(fd, static_cast<off_t>(n)) == 0 && ok;
  return ::close(fd) == 0 && ok;
#endif
}

}  // namespace

// A save's file changes length with nearly every edit — a digit more, a name shorter. The same
// write as `doc.fs.overwrite`, a byte shorter and a byte longer by turns.
ENGINE_BENCH_ARGS(fs_overwrite_resize, "doc.fs.overwrite_resize", 4096, 16384, 65536) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  (void)io::write_file(path, data);
  u64 n = 0;
  while (state.keep_running())
    bench::keep(overwrite(path, data, data.size() - (n++ % 2)));
  state.set_items(1);
  state.set_bytes(data.size());
}

// The file removed and made again, which a save's log makes as safe as a write over it.
ENGINE_BENCH_ARGS(fs_recreate, "doc.fs.recreate", 4096, 16384, 65536) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  (void)io::write_file(path, data);
  u64 n = 0;
  while (state.keep_running()) {
    bench::keep(io::remove_file(path));
    bench::keep(overwrite(path, data, data.size() - (n++ % 2)));
  }
  state.set_items(1);
  state.set_bytes(data.size());
}

// The same through stdio's "r+b": opened for reading as well as writing, which is what the store
// first did and what cost 12 ms a write at 64 KiB and above on Windows (docs/subsystems/doc.md).
ENGINE_BENCH_ARGS(fs_overwrite_rw, "doc.fs.overwrite_rw", 64, 4096, 65536, 1048576) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  (void)io::write_file(path, data);
  while (state.keep_running()) {
#if ENGINE_PLATFORM_WINDOWS
    std::FILE* f = _wfsopen(std::filesystem::path(path).c_str(), L"r+b", _SH_DENYNO);
#else
    std::FILE* f = std::fopen(path.c_str(), "r+b");
#endif
    if (f == nullptr) continue;
    bench::keep(std::fwrite(data.data(), 1, data.size(), f));
    bench::keep(std::fclose(f));
  }
  state.set_items(1);
  state.set_bytes(data.size());
}

// The same bytes written over the file after truncating it: opened with "wb", written, closed.
ENGINE_BENCH_ARGS(fs_write_direct, "doc.fs.write_direct", 64, 4096, 65536, 1048576) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  const std::string data = payload(state.arg());
  while (state.keep_running())
    bench::keep(io::write_file(path, data));
  state.set_items(1);
  state.set_bytes(data.size());
}

// A rename over an existing file, alone: the second half of an atomic write.
ENGINE_BENCH(fs_rename, "doc.fs.rename") {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string a = tmp.file("a.json");
  const std::string b = tmp.file("b.json");
  (void)io::write_file(a, "{}");
  (void)io::write_file(b, "{}");
  u64 n = 0;
  while (state.keep_running()) {
    // Back and forth, recreating the one that moved, outside the timing.
    bench::keep(io::rename_path(a, b));
    state.pause_timing();
    (void)io::write_file(a, "{}");
    state.resume_timing();
    ++n;
  }
  bench::keep(n);
  state.set_items(1);
}

// What the journal does: one line appended to a file that is opened and closed around it.
ENGINE_BENCH_ARGS(fs_append, "doc.fs.append", 64, 4096) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("journal.jsonl");
  const std::string data = payload(state.arg());
  u64 appended = 0;
  while (state.keep_running()) {
    bench::keep(io::append_file(path, data));
    // A journal of a hundred thousand lines is not what is being measured; start it again.
    if (++appended % 4096 == 0) {
      state.pause_timing();
      (void)io::remove_file(path);
      state.resume_timing();
    }
  }
  state.set_items(1);
  state.set_bytes(data.size());
}

ENGINE_BENCH_ARGS(fs_read, "doc.fs.read", 64, 4096, 65536, 1048576) {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  (void)io::write_file(path, payload(state.arg()));
  std::string out;
  while (state.keep_running()) {
    bench::keep(io::read_file(path, out));
    bench::keep(out.size());
  }
  state.set_items(1);
  state.set_bytes(static_cast<u64>(state.arg()));
}

// The questions a save asks without writing: does a file exist, what is in a directory of 64
// files, and does a directory exist (what `Vfs::write` asks about every parent).
ENGINE_BENCH(fs_exists, "doc.fs.exists") {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("file.json");
  (void)io::write_file(path, "{}");
  while (state.keep_running())
    bench::keep(io::exists(path));
  state.set_items(1);
}

ENGINE_BENCH(fs_list, "doc.fs.list") {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  for (u32 i = 0; i < 64; ++i)
    (void)io::write_file(tmp.file(std::to_string(i) + "_0.json"), "{}");
  Vector<io::DirEntry> entries;
  while (state.keep_running()) {
    bench::keep(io::list_directory(tmp.path(), entries));
    bench::keep(entries.size());
  }
  state.set_items(1);
}

ENGINE_BENCH(fs_make_directories, "doc.fs.make_directories") {
  engine::test::TempDir tmp("engine_doc_fs_bench");
  const std::string path = tmp.file("layers/world/tiles");
  (void)io::make_directories(path);
  while (state.keep_running())
    bench::keep(io::make_directories(path));
  state.set_items(1);
}
