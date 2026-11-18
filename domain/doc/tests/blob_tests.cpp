// The blob store beside a document (blob_store.h; docs/subsystems/doc.md, "Blobs"): the same bytes
// the same name and file, a read checked against the name, a reference found wherever it stands in
// a value, a save that writes no blob, a merge that treats a reference as a value, and — the property
// the store exists for — a seeded run of puts, references, clears, undos, redos, dropped redo tails,
// saves, reloads and collections after which every name the document or its journal can still reach
// is stored and readable, and nothing else is.
#include <domain/doc/blob_store.h>
#include <domain/doc/document_store.h>
#include <domain/doc/merge.h>

#include <core/schema/json_reflect.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

using namespace engine;
using namespace engine::doc;

using TempDir = test::TempDir;

namespace {

Attribution who() {
  Attribution a;
  a.actor = "test";
  a.role = "author";
  a.timestamp_unix_ms = 1;
  return a;
}

JsonValue ref_json(const BlobRef& ref) { return schema::to_json(ref); }

std::string bytes_of(u32 seed, u32 size) {
  std::string out(size, '\0');
  u32 x = seed * 2654435761u + 1;
  for (char& c : out) {
    x = x * 1664525u + 1013904223u;
    c = static_cast<char>(x >> 24);
  }
  return out;
}

HashSet<std::string> reachable(const Document& doc) {
  HashSet<std::string> names;
  BlobStore::references(doc, names);
  return names;
}

}  // namespace

TEST_CASE("blobs: the same bytes are the same name, the same file, and are read back checked") {
  TempDir tmp("engine_doc_blobs");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  const std::string grid = bytes_of(7, 100000);
  BlobRef a;
  bool created = false;
  REQUIRE(BlobStore::put(vfs, dir, grid, "engine.terrain.grid/1", a, &created, &error));
  CHECK(created);
  CHECK(BlobStore::is_name(a.blob));
  CHECK(a.blob == BlobStore::name_of(grid));
  CHECK(a.size == grid.size());
  CHECK(BlobStore::path_of(dir, a.blob) ==
        "docs://world/blobs/" + a.blob.substr(0, 2) + "/" + a.blob.substr(2));

  // Again, under another media: one blob, not written again.
  BlobRef b;
  REQUIRE(BlobStore::put(vfs, dir, grid, "application/octet-stream", b, &created, &error));
  CHECK_FALSE(created);
  CHECK(b.blob == a.blob);
  std::string on_disk;
  REQUIRE(vfs.read(BlobStore::path_of(dir, a.blob), on_disk) == io::Status::Ok);
  CHECK(on_disk == grid);  // the file is the bytes and nothing else

  std::string back;
  REQUIRE(BlobStore::read(vfs, dir, a.blob, a.size, back, &error));
  CHECK(back == grid);
  CHECK(BlobStore::has(vfs, dir, a.blob, a.size));
  CHECK_FALSE(BlobStore::has(vfs, dir, a.blob, a.size + 1));
  CHECK_FALSE(BlobStore::read(vfs, dir, a.blob, a.size + 1, back, &error));
  CHECK(error.find("damaged") != std::string::npos);

  // A damaged file is refused by name, and the next put of the same bytes repairs it.
  std::string bad = grid;
  bad[5000] = static_cast<char>(bad[5000] ^ 1);
  REQUIRE(vfs.write(BlobStore::path_of(dir, a.blob), bad) == io::Status::Ok);
  CHECK_FALSE(BlobStore::read(vfs, dir, a.blob, a.size, back, &error));
  CHECK(error.find("does not hash to its name") != std::string::npos);
  REQUIRE(BlobStore::put(vfs, dir, grid, "engine.terrain.grid/1", b, &created, &error));
  CHECK(created);
  CHECK(BlobStore::read(vfs, dir, a.blob, a.size, back, &error));

  // Names that are not names.
  CHECK_FALSE(BlobStore::is_name(a.blob.substr(1)));
  CHECK_FALSE(BlobStore::is_name(std::string(64, 'G')));
  std::string upper = a.blob;
  for (char& c : upper)
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  if (upper != a.blob) CHECK_FALSE(BlobStore::is_name(upper));
  CHECK_FALSE(BlobStore::read(vfs, dir, "nope", 0, back, &error));
  CHECK_FALSE(BlobStore::read(vfs, dir, std::string(64, '0'), 0, back, &error));
  CHECK(error.find("not in the document's store") != std::string::npos);

  Vector<StoredBlob> listed;
  REQUIRE(BlobStore::list(vfs, dir, listed, &error));
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].name == a.blob);
  CHECK(listed[0].size == grid.size());
}

// Batch 68's review, B3: a put of bytes already stored wrote nothing and left the file's time where
// it was, so a blob left unnamed past the grace period, put again by a client about to name it, was
// collected before the client's commit, which was then refused for a blob it had just put.
TEST_CASE("blobs: a put of bytes already stored starts the collection's grace period again") {
  TempDir tmp("engine_doc_blob_reput");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  BlobRef ref;
  bool created = false;
  REQUIRE(BlobStore::put(vfs, dir, "a painted mask", "test", ref, &created, &error));
  REQUIRE(created);
  // Two hours old, and named by nothing: a collection with the default hour takes it.
  const std::string file =
      tmp.path() + "/world/blobs/" + ref.blob.substr(0, 2) + "/" + ref.blob.substr(2);
  std::error_code ec;
  std::filesystem::last_write_time(
      std::filesystem::path(file),
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(2), ec);
  REQUIRE_MESSAGE(!ec, ec.message());
  BlobCollectOptions hour;
  hour.min_age_ms = 3'600'000;
  hour.dry_run = true;
  BlobCollectReport report;
  REQUIRE(BlobStore::collect(vfs, dir, doc, {}, hour, report, &error));
  REQUIRE(report.removed.size() == 1);

  // Put again by a client about to name it: not new, and young again.
  REQUIRE(BlobStore::put(vfs, dir, "a painted mask", "test", ref, &created, &error));
  CHECK_FALSE(created);
  hour.dry_run = false;
  REQUIRE(BlobStore::collect(vfs, dir, doc, {}, hour, report, &error));
  CHECK(report.removed.empty());
  CHECK(report.kept_young == 1);
  CHECK(BlobStore::has(vfs, dir, ref.blob));
}

TEST_CASE("blobs: a reference is found wherever it stands in a value, and only a name counts") {
  const std::string n1 = BlobStore::name_of("one");
  const std::string n2 = BlobStore::name_of("two");
  const std::string n3 = BlobStore::name_of("three");
  JsonValue value = JsonValue::object();
  JsonValue& o = value;
  BlobRef r1;
  r1.blob = n1;
  r1.size = 3;
  o.set("grid", ref_json(r1));
  JsonValue layers = JsonValue::array();
  JsonValue mask = JsonValue::object();
  mask.set("blob", JsonValue(n2));
  JsonValue nested = JsonValue::object();
  nested.set("blob", JsonValue(n3));
  mask.set("preview", std::move(nested));
  layers.push_back(std::move(mask));
  JsonValue not_a_name = JsonValue::object();
  not_a_name.set("blob", JsonValue("short"));
  layers.push_back(std::move(not_a_name));
  o.set("layers", std::move(layers));
  o.set("blob", JsonValue(42));  // a number is not a name

  HashSet<std::string> names;
  BlobStore::references(value, names);
  CHECK(names.size() == 3);
  CHECK(names.contains(n1));
  CHECK(names.contains(n2));
  CHECK(names.contains(n3));
}

TEST_CASE("blobs: a save writes no blob, and a tile names its blob by the hash alone") {
  TempDir tmp("engine_doc_blob_save");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  LayerPartition grid;
  grid.property = "position";
  grid.tile_size = 64.0;
  doc.set_layer_partition(0, grid);

  BlobRef ref;
  REQUIRE(BlobStore::put(vfs, dir, bytes_of(3, 5000), "image/raw", ref, nullptr, &error));
  const ObjectId id = Id128::from_parts(9, 1);
  {
    Transaction tx = doc.begin(who());
    JsonValue props = JsonValue::object();
    JsonValue pos = JsonValue::array();
    pos.push_back(JsonValue(10.0));
    pos.push_back(JsonValue(0.0));
    pos.push_back(JsonValue(10.0));
    props.set("position", std::move(pos));
    props.set("mask", ref_json(ref));
    REQUIRE(tx.apply(cmd_create(id, "engine.doc.test.Placement", {}, std::move(props))));
    REQUIRE(tx.commit());
  }
  manifest.undo_position = doc.journal().size();
  SaveReport report;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
  for (const std::string& path : report.written)
    CHECK(path.find("blobs") == std::string::npos);

  // Moved to another tile: the tile files change, the blob and its name do not.
  {
    Transaction tx = doc.begin(who());
    JsonValue pos = JsonValue::array();
    pos.push_back(JsonValue(200.0));
    pos.push_back(JsonValue(0.0));
    pos.push_back(JsonValue(-90.0));
    REQUIRE(tx.apply(cmd_set(id, "position", std::move(pos))));
    REQUIRE(tx.commit());
  }
  manifest.undo_position = doc.journal().size();
  report = SaveReport{};
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
  CHECK(report.tiles_written >= 1);
  for (const std::string& path : report.written)
    CHECK(path.find("blobs") == std::string::npos);

  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  const JsonValue* mask = loaded.property(id, "mask");
  REQUIRE(mask != nullptr);
  BlobRef back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, *mask, ctx));
  CHECK(back.blob == ref.blob);
  std::string bytes;
  CHECK(BlobStore::read(vfs, dir, back.blob, back.size, bytes, &error));
}

TEST_CASE("blobs: a merge treats a reference as a value") {
  const ObjectId id = Id128::from_parts(5, 5);
  BlobRef r0, r1, r2;
  r0.blob = BlobStore::name_of("zero");
  r1.blob = BlobStore::name_of("one");
  r2.blob = BlobStore::name_of("two");
  const auto layer_with = [&](const BlobRef& r) {
    Layer layer("terrain", LayerRole::Base);
    ObjectRecord rec;
    rec.id = id;
    rec.type = "engine.doc.test.Placement";
    rec.properties["grid"] = ref_json(r);
    layer.set(std::move(rec));
    return layer;
  };
  const Layer base = layer_with(r0);
  MergeResult merged;
  std::string error;
  // One side changed it: that side's reference.
  REQUIRE(merge_layers(base, base, layer_with(r1), merged, &error));
  CHECK(merged.conflicts.empty());
  CHECK(*merged.merged.find(id)->properties.find_value("grid") == ref_json(r1));
  // Both to the same blob: no conflict.
  REQUIRE(merge_layers(base, layer_with(r1), layer_with(r1), merged, &error));
  CHECK(merged.conflicts.empty());
  // Both to different blobs: the property conflict, with the three references as its values.
  REQUIRE(merge_layers(base, layer_with(r1), layer_with(r2), merged, &error));
  REQUIRE(merged.conflicts.size() == 1);
  CHECK(merged.conflicts[0].kind == MergeConflict::PropertyBothChanged);
  CHECK(merged.conflicts[0].property == "grid");
  CHECK(merged.conflicts[0].ours == ref_json(r1));
  CHECK(merged.conflicts[0].theirs == ref_json(r2));
}

TEST_CASE("blobs: seeded puts, references, undo, redo, saves and collections keep what can come back") {
  TempDir tmp("engine_doc_blob_property");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.add_layer("paint", LayerRole::Feature);

  constexpr u32 k_objects = 6;
  {
    Transaction tx = doc.begin(who());
    for (u32 i = 0; i < k_objects; ++i)
      REQUIRE(tx.apply(cmd_create(Id128::from_parts(2, i + 1), "engine.doc.test.Placement")));
    REQUIRE(tx.commit());
  }
  u32 position = doc.journal().size();

  std::mt19937 rng(0xB10Bu);
  // Every blob ever put, collected or not, with what makes its bytes again.
  struct Made {
    BlobRef ref;
    u32 seed = 0;
    u32 size = 0;
  };
  Vector<Made> made;
  BlobCollectOptions now;
  now.min_age_ms = 0;
  u32 collections = 0, removed_total = 0, undos = 0, redos = 0, drops = 0;

  const auto commit = [&](const Command& c) {
    // A new commit drops the redo tail, as a session does.
    doc.truncate_journal(position);
    Transaction tx = doc.begin(who());
    const bool ok = tx.apply(c, false);
    if (ok) {
      REQUIRE(tx.commit());
      position = doc.journal().size();
    }
  };

  for (u32 step = 0; step < 400; ++step) {
    const u32 op = rng() % 100;
    const ObjectId target = Id128::from_parts(2, rng() % k_objects + 1);
    doc.set_edit_layer(rng() % 2);
    if (op < 20) {
      Made m;
      m.seed = rng() % 40;
      m.size = 1 + rng() % 3000;
      REQUIRE(BlobStore::put(vfs, dir, bytes_of(m.seed, m.size), "test", m.ref, nullptr, &error));
      made.push_back(m);
    } else if (op < 45 && !made.empty()) {
      // Reference a blob the store holds; a collected one is put back first, as a client would
      // have to (a session refuses a command naming a blob it does not hold), to the same name.
      const Made m = made[rng() % made.size()];
      const BlobRef ref = m.ref;
      if (!BlobStore::has(vfs, dir, ref.blob, ref.size)) {
        BlobRef again;
        REQUIRE(BlobStore::put(vfs, dir, bytes_of(m.seed, m.size), "test", again, nullptr,
                               &error));
        CHECK(again.blob == ref.blob);
      }
      JsonValue value = JsonValue::array();
      value.push_back(ref_json(ref));
      commit(cmd_set(target, rng() % 2 ? "mask" : "grid", std::move(value)));
    } else if (op < 55) {
      commit(cmd_clear(target, rng() % 2 ? "mask" : "grid"));
    } else if (op < 68 && position > 1) {
      REQUIRE(doc.undo(doc.journal()[position - 1]));
      --position;
      ++undos;
    } else if (op < 76 && position < doc.journal().size()) {
      REQUIRE(doc.redo(doc.journal()[position]));
      ++position;
      ++redos;
    } else if (op < 80 && position < doc.journal().size()) {
      // A commit after undo drops the redo tail: references only it held become collectable.
      doc.truncate_journal(position);
      ++drops;
    } else if (op < 90) {
      manifest.undo_position = position;
      REQUIRE(DocumentStore::append_journal(vfs, dir, doc, &error));
      REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
    } else {
      BlobCollectReport report;
      REQUIRE(BlobStore::collect(vfs, dir, doc, {}, now, report, &error));
      ++collections;
      removed_total += report.removed.size();
      // Nothing the document or its journal can still reach went, and nothing else stayed.
      const HashSet<std::string> names = reachable(doc);
      Vector<StoredBlob> stored;
      REQUIRE(BlobStore::list(vfs, dir, stored, &error));
      for (const StoredBlob& s : stored)
        CHECK(names.contains(s.name));
      for (const std::string& name : names) {
        std::string bytes;
        CHECK(BlobStore::read(vfs, dir, name, 0, bytes, &error));
      }
      CHECK(report.missing.empty());
    }
    // Every reference the composed document holds now reads back, whatever was collected.
    for (u32 i = 0; i < k_objects; ++i) {
      for (const char* name : {"mask", "grid"}) {
        const JsonValue* v = doc.property(Id128::from_parts(2, i + 1), name);
        if (v == nullptr) continue;
        HashSet<std::string> names;
        BlobStore::references(*v, names);
        for (const std::string& n : names)
          CHECK(BlobStore::has(vfs, dir, n));
      }
    }
  }
  // Undo everything there is: every reference it brings back is still stored.
  while (position > 1) {
    REQUIRE(doc.undo(doc.journal()[position - 1]));
    --position;
    const HashSet<std::string> names = reachable(doc);
    for (const std::string& n : names)
      CHECK(BlobStore::has(vfs, dir, n));
  }
  // A reload sees the journal the store wrote and collects the same.
  MESSAGE("collections " << collections << ", removed " << removed_total << ", undos " << undos
                         << ", redos " << redos << ", tails dropped " << drops);
  CHECK(collections > 10);
  CHECK(removed_total > 0);

  // The grace period: a fresh blob nothing names is kept by a collection with the default age.
  BlobRef fresh;
  REQUIRE(BlobStore::put(vfs, dir, "fresh bytes", "test", fresh, nullptr, &error));
  BlobCollectReport young;
  REQUIRE(BlobStore::collect(vfs, dir, doc, {}, BlobCollectOptions{}, young, &error));
  CHECK(BlobStore::has(vfs, dir, fresh.blob));
  CHECK(young.kept_young >= 1);
  // A root the caller adds (a proposal's merge base) keeps a blob too.
  JsonValue root = JsonValue::object();
  root.set("blob", JsonValue(fresh.blob));
  BlobCollectReport rooted;
  REQUIRE(BlobStore::collect(vfs, dir, doc, std::span<const JsonValue>(&root, 1), now, rooted,
                             &error));
  CHECK(BlobStore::has(vfs, dir, fresh.blob));
  // A dry run removes nothing and says what it would.
  BlobCollectOptions dry = now;
  dry.dry_run = true;
  BlobCollectReport would;
  REQUIRE(BlobStore::collect(vfs, dir, doc, {}, dry, would, &error));
  CHECK(BlobStore::has(vfs, dir, fresh.blob));
  bool named = false;
  for (const std::string& n : would.removed)
    named = named || n == fresh.blob;
  CHECK(named);
}
