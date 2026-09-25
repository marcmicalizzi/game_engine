// What a save writes (docs/subsystems/doc.md, "Saving"): only the files whose bytes change, and
// exactly the bytes a store that wrote every file would have left.
//
// The reference below is the store as it was before it learned to write less — every layer's
// files and the manifest, written whole — spelled with nothing but the public API, so the two
// cannot share a mistake. The randomized case saves a document incrementally after every step of a
// seeded sequence of edits, undos, layer changes and repartitions, and compares every file on disk
// with the reference, byte for byte, and with a store that knew nothing and wrote everything.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <schemas/doc_test_types.h>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace engine;
using namespace engine::doc;

// Spelled from `engine::` because the generated schema namespace `engine::doc::test` makes a bare
// `test` ambiguous here.
using TempDir = engine::test::TempDir;

namespace {

namespace fs = std::filesystem;

const char* const k_placement = "engine.doc.test.Placement";

// Path under the document directory -> bytes.
using Files = std::map<std::string, std::string>;

ObjectId id_of(u32 n) { return Id128::from_parts(3, n + 1); }

Attribution who() {
  Attribution a;
  a.actor = "save-test";
  a.role = "test";
  a.task = "unit";
  a.timestamp_unix_ms = 1;
  return a;
}

LayerPartition tiles_of(f64 size) {
  LayerPartition p;
  p.property = "position";
  p.tile_size = size;
  return p;
}

JsonValue point(f64 x, f64 z) {
  JsonValue::Array a;
  a.push_back(JsonValue(x));
  a.push_back(JsonValue(0.0));
  a.push_back(JsonValue(z));
  return JsonValue(std::move(a));
}

// Every file of the document as the store wrote it before it wrote only what changed.
Files reference_files(const Document& doc, const DocumentManifest& saved) {
  Files out;
  DocumentManifest manifest;
  manifest.name = saved.name;
  manifest.undo_position = saved.undo_position;
  for (u32 i = 0; i < doc.layer_count(); ++i) {
    const Layer& layer = doc.layer(i);
    LayerRef ref;
    ref.name = layer.name();
    ref.role = layer.role();
    if (layer.partitioned()) {
      const std::string dir = "layers/" + DocumentStore::layer_dir_name(layer.name());
      ref.file = DocumentStore::layer_dir_name(layer.name()) + "/index.json";
      ref.partition = layer.partition();
      const LayerIndex index = build_layer_index(layer);
      out[dir + "/index.json"] = write_json(schema::to_json(index)) + "\n";
      for (const TileRef& t : index.tiles)
        out[dir + "/tiles/" + t.file] = write_json(schema::to_json(layer.to_file(t.objects)));
      if (!index.untiled.empty())
        out[dir + "/untiled.json"] = write_json(schema::to_json(layer.to_file(index.untiled)));
    } else {
      ref.file = DocumentStore::layer_file_name(layer.name());
      out["layers/" + ref.file] = layer.to_json_text();
    }
    manifest.layers.push_back(std::move(ref));
  }
  manifest.edit_layer = doc.layer(doc.edit_layer()).name();
  out["manifest.json"] = write_json(schema::to_json(manifest)) + "\n";
  return out;
}

std::string read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// Every file under `root` but the journal, by its path under root with forward slashes.
Files files_on_disk(const std::string& root, bool with_journal = false) {
  Files out;
  const fs::path base(root);
  for (const auto& e : fs::recursive_directory_iterator(base)) {
    if (!e.is_regular_file()) continue;
    std::string rel = fs::relative(e.path(), base).generic_string();
    if (!with_journal && rel == "journal.jsonl") continue;
    out[rel] = read_bytes(e.path());
  }
  return out;
}

using Times = std::map<std::string, fs::file_time_type>;

Times times_on_disk(const std::string& root) {
  Times out;
  const fs::path base(root);
  for (const auto& e : fs::recursive_directory_iterator(base)) {
    if (e.is_regular_file())
      out[fs::relative(e.path(), base).generic_string()] = fs::last_write_time(e.path());
  }
  return out;
}

std::string journal_text(const Document& doc) {
  std::string text;
  for (const Patch& p : doc.journal()) {
    write_json(schema::to_json(p), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
  }
  return text;
}

// The first key where two file sets differ, for a message that says where.
std::string first_difference(const Files& a, const Files& b) {
  for (const auto& [path, bytes] : a) {
    const auto it = b.find(path);
    if (it == b.end()) return path + " (only on disk)";
    if (it->second != bytes) return path + " (bytes differ)";
  }
  for (const auto& [path, bytes] : b) {
    if (a.find(path) == a.end()) return path + " (missing on disk)";
  }
  return {};
}

struct Rng {
  u64 state;
  u32 next(u32 bound) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>((state >> 33) % bound);
  }
};

// A session's commit, as domain/protocol does it: drop the redo tail, journal (the store cuts the
// tail off the file in place and appends), move the undo position. The caller saves.
void commit(const io::Vfs& vfs, const std::string& dir, Document& doc, DocumentManifest& manifest,
            Transaction& tx) {
  if (manifest.undo_position < doc.journal().size()) doc.truncate_journal(manifest.undo_position);
  const u32 before = doc.journal().size();
  REQUIRE(tx.commit());
  if (doc.journal().size() == before) return;  // nothing applied, nothing journaled
  std::string error;
  REQUIRE(DocumentStore::append_journal(vfs, dir, doc, &error));
  manifest.undo_position = doc.journal().size();
}

}  // namespace

TEST_CASE("doc save: incremental saves leave the bytes a full write would, step after step") {
  TempDir tmp("engine_doc_save_identity");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  // Two forms side by side, as a real document has them: a single-file base, a tiled world, a
  // single-file overrides layer and a tiled layer on a coarser grid.
  doc.add_layer("world", LayerRole::Feature);
  doc.set_layer_partition(1, tiles_of(16));
  doc.add_layer("overrides", LayerRole::Feature);
  doc.add_layer("props", LayerRole::Feature);
  doc.set_layer_partition(3, tiles_of(32));
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));

  // Every step reads the whole directory back, which is most of this case's time on Windows.
  constexpr u32 k_steps = 120;
  Rng rng{0x5eedu};
  u32 next_id = 0;
  u32 next_layer = 0;
  Vector<ObjectId> ids;  // every id ever created, live or not
  u32 incremental_writes = 0, full_writes = 0;

  auto random_point = [&]() {
    return point(static_cast<f64>(rng.next(160)) - 80.0, static_cast<f64>(rng.next(160)) - 80.0);
  };
  auto some_id = [&]() { return ids[rng.next(ids.size())]; };

  for (u32 step = 0; step < k_steps; ++step) {
    CAPTURE(step);
    doc.set_edit_layer(rng.next(doc.layer_count()));
    const u32 action = ids.empty() ? 0 : rng.next(20);
    bool saved = false;
    switch (action) {
      case 0:
      case 1:
      case 2:
      case 3: {  // create, placed or not
        const ObjectId id = id_of(next_id++);
        ids.push_back(id);
        JsonValue props = JsonValue::object();
        props.set("name", JsonValue("p" + std::to_string(next_id)));
        if (rng.next(4) != 0) props.set("position", random_point());
        Transaction tx = doc.begin(who());
        tx.apply(cmd_create(id, k_placement, ObjectId{}, std::move(props)), /*strict=*/false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 4:
      case 5:
      case 6: {  // a property, on this layer's record or as an override of one below
        Transaction tx = doc.begin(who());
        tx.apply(cmd_set(some_id(), "name", JsonValue("n" + std::to_string(step))), false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 7:
      case 8: {  // a move, often into another tile
        Transaction tx = doc.begin(who());
        tx.apply(cmd_set(some_id(), "position", random_point()), false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 9: {  // off the grid
        Transaction tx = doc.begin(who());
        tx.apply(cmd_clear(some_id(), "position"), false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 10: {  // delete, or drop this layer's record
        Transaction tx = doc.begin(who());
        tx.apply(rng.next(2) == 0 ? cmd_delete(some_id()) : cmd_remove_record(some_id()), false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 11: {  // a transaction that changes nothing: the record's own value again
        const ObjectId id = some_id();
        const Document& read = doc;
        const ObjectRecord* r = read.layer(read.edit_layer()).find(id);
        const JsonValue* name = r != nullptr ? r->properties.find_value("name") : nullptr;
        if (name == nullptr) break;
        Transaction tx = doc.begin(who());
        tx.apply(cmd_set(id, "name", *name), false);
        commit(vfs, dir, doc, manifest, tx);
        break;
      }
      case 12: {  // rolled back: the records are as they were, the save finds nothing to write
        Transaction tx = doc.begin(who());
        tx.apply(cmd_set(some_id(), "position", random_point()), false);
        tx.rollback();
        break;
      }
      case 13: {  // undo
        if (manifest.undo_position == 0) break;
        const Patch& patch = doc.journal()[manifest.undo_position - 1];
        if (doc.find_layer(patch.layer) < 0 || !doc.undo(patch)) break;
        --manifest.undo_position;
        break;
      }
      case 14: {  // redo
        if (manifest.undo_position >= doc.journal().size()) break;
        const Patch& patch = doc.journal()[manifest.undo_position];
        if (doc.find_layer(patch.layer) < 0 || !doc.redo(patch)) break;
        ++manifest.undo_position;
        break;
      }
      case 15: {  // a layer arrives, in either form
        const u32 index = doc.add_layer("extra" + std::to_string(next_layer++), LayerRole::Feature);
        if (rng.next(2) == 0) doc.set_layer_partition(index, tiles_of(8));
        break;
      }
      case 16: {  // a layer leaves, and a patch naming it can no longer be undone
        if (doc.layer_count() <= 2) break;
        REQUIRE(doc.remove_layer(1 + rng.next(doc.layer_count() - 1)));
        break;
      }
      case 17: {  // a layer changes form
        const u32 index = rng.next(doc.layer_count());
        const u32 form = rng.next(3);
        SaveReport report;
        REQUIRE(DocumentStore::repartition(vfs, dir, doc, manifest, index,
                                           form == 0 ? LayerPartition{} : tiles_of(form * 12.0),
                                           &error, &report));
        saved = true;
        break;
      }
      case 18: {  // behind the index's back, through the mutable accessor
        const u32 index = rng.next(doc.layer_count());
        ObjectRecord r;
        r.id = some_id();
        r.properties.insert_or_assign("name", JsonValue("mutable" + std::to_string(step)));
        r.properties.insert_or_assign("position", random_point());
        doc.layer(index).set(std::move(r));
        break;
      }
      default: {  // nothing at all: a save of an unchanged document writes nothing
        // The first save writes the manifest if this step moved the edit layer; the second has
        // nothing left to write.
        REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
        SaveReport report;
        REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
        CHECK(report.files_written == 0);
        CHECK(report.files_removed == 0);
        saved = true;
        break;
      }
    }
    if (!saved) {
      SaveReport report;
      REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
      incremental_writes += report.files_written;
    }

    const Files disk = files_on_disk(tmp.file("world"));
    const Files expected = reference_files(doc, manifest);
    CHECK_MESSAGE(disk == expected, "first difference: " << first_difference(disk, expected));
    std::string journal;
    REQUIRE(vfs.read("docs://world/journal.jsonl", journal) == io::Status::Ok);
    CHECK(journal == journal_text(doc));

    // Now and then, what the disk holds is loaded and carries on in place of what was in memory,
    // so the next steps start from what load learned rather than from what save remembered; and
    // a copy of it saved where nothing is known writes every file — the store that knew nothing —
    // which must come out the same.
    if (step % 20 == 19) {
      Document loaded;
      DocumentManifest loaded_manifest;
      REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
      CHECK_FALSE(loaded.dirty());
      REQUIRE(loaded.layer_count() == doc.layer_count());
      for (u32 i = 0; i < doc.layer_count(); ++i) {
        const Document& a = doc;
        const Document& b = loaded;
        CHECK(a.layer(i) == b.layer(i));
      }
      const std::string elsewhere = "docs://full" + std::to_string(step);
      Document copy;
      DocumentManifest copy_manifest;
      REQUIRE(DocumentStore::load(vfs, dir, copy, copy_manifest, &error));
      SaveReport full;
      REQUIRE(DocumentStore::save(vfs, elsewhere, copy, copy_manifest, &error, &full));
      full_writes += full.files_written;
      const Files everything = files_on_disk(tmp.file("full" + std::to_string(step)));
      CHECK_MESSAGE(everything == disk, "first difference: " << first_difference(everything, disk));

      doc = std::move(loaded);
      manifest = std::move(loaded_manifest);
    }
  }
  // The point of it: over the whole run the incremental saves wrote far fewer files than one
  // full write per step would have.
  MESSAGE("incremental saves wrote " << incremental_writes << " files over " << k_steps
                                     << " steps");
  CHECK(incremental_writes > 0);
  CHECK(full_writes > 0);
}

TEST_CASE("doc save: one record in one tile rewrites that tile, the journal and the manifest") {
  TempDir tmp("engine_doc_save_one_tile");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  {
    Layer world("world", LayerRole::Feature);
    world.set_partition(tiles_of(16));
    for (u32 i = 0; i < 64; ++i) {
      ObjectRecord r;
      r.id = id_of(i);
      r.type = k_placement;
      r.parent = ObjectId{};
      r.properties.insert_or_assign("name", JsonValue("p" + std::to_string(i)));
      r.properties.insert_or_assign(
          "position", point(static_cast<f64>(i % 4) * 16.0 + 1.0, static_cast<f64>(i / 16) * 16.0));
      world.set(std::move(r));
    }
    const u32 index = doc.add_layer(std::move(world));
    doc.set_layer_partition(index, tiles_of(16));
    Layer overrides("overrides", LayerRole::Feature);
    for (u32 i = 0; i < 64; i += 4) {
      ObjectRecord r;
      r.id = id_of(i);
      r.properties.insert_or_assign("name", JsonValue("renamed"));
      overrides.set(std::move(r));
    }
    doc.add_layer(std::move(overrides));
    doc.set_edit_layer(index);
  }
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  const Files before = files_on_disk(tmp.file("world"), /*with_journal=*/true);
  const Times before_times = times_on_disk(tmp.file("world"));
  REQUIRE(before.count("layers/world/tiles/2_1.json") == 1);
  // A rewrite shows as a later modification time even where the bytes come out the same.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // id 22 stands at (33, 16): tile (2, 1), with three others.
  {
    Transaction tx = doc.begin(who());
    REQUIRE(tx.apply(cmd_set(id_of(22), "name", JsonValue("moved nowhere"))));
    commit(vfs, dir, doc, manifest, tx);
  }
  SaveReport report;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
  CHECK(report.files_written == 2);
  CHECK(report.files_removed == 0);
  CHECK(report.tiles_written == 1);
  REQUIRE(report.written.size() == 2);
  CHECK(report.written[0] == "layers/world/tiles/2_1.json");
  CHECK(report.written[1] == "manifest.json");

  const Files after = files_on_disk(tmp.file("world"), /*with_journal=*/true);
  const Times after_times = times_on_disk(tmp.file("world"));
  std::set<std::string> changed;
  for (const auto& [path, bytes] : after) {
    const auto it = before.find(path);
    if (it == before.end() || it->second != bytes || before_times.at(path) != after_times.at(path))
      changed.insert(path);
  }
  CHECK(after.size() == before.size());
  CHECK(changed ==
        std::set<std::string>{"journal.jsonl", "layers/world/tiles/2_1.json", "manifest.json"});

  // A transaction that sets what is already there journals a patch and moves the undo position,
  // and the tile's bytes come out the same: the manifest alone is written.
  {
    Transaction tx = doc.begin(who());
    REQUIRE(tx.apply(cmd_set(id_of(22), "name", JsonValue("moved nowhere"))));
    commit(vfs, dir, doc, manifest, tx);
  }
  SaveReport same;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &same));
  REQUIRE(same.written.size() == 1);
  CHECK(same.written[0] == "manifest.json");

  // A move across the grid: the tile it left, the tile it joined, the index, the manifest.
  {
    Transaction tx = doc.begin(who());
    REQUIRE(tx.apply(cmd_set(id_of(22), "position", point(200, 200))));
    commit(vfs, dir, doc, manifest, tx);
  }
  SaveReport moved;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &moved));
  CHECK(std::set<std::string>(moved.written.begin(), moved.written.end()) ==
        std::set<std::string>{"layers/world/tiles/2_1.json", "layers/world/tiles/12_12.json",
                              "layers/world/index.json", "manifest.json"});
  CHECK(moved.files_removed == 0);

  // And nothing changed at all: nothing written, nothing removed.
  SaveReport nothing;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &nothing));
  CHECK(nothing.files_written == 0);
  CHECK(nothing.files_removed == 0);
  CHECK(files_on_disk(tmp.file("world")) == reference_files(doc, manifest));
}

namespace {

ObjectRecord placed(u32 n, std::optional<std::pair<f64, f64>> at) {
  ObjectRecord r;
  r.id = id_of(n);
  r.type = k_placement;
  r.parent = ObjectId{};
  r.properties.insert_or_assign("name", JsonValue("p" + std::to_string(n)));
  if (at.has_value()) r.properties.insert_or_assign("position", point(at->first, at->second));
  return r;
}

// State A, the same bytes every time: a single-file base, a tiled world of six tiles and two
// untiled records, a single-file overrides layer, and a tiled props layer on a coarser grid.
void build_before(const io::Vfs& vfs, const std::string& dir, Document& doc,
                  DocumentManifest& manifest) {
  std::string error;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  Layer world("world", LayerRole::Feature);
  for (u32 i = 0; i < 24; ++i)
    world.set(placed(i, std::make_pair(static_cast<f64>(i % 3) * 16.0 + 2.0,
                                       static_cast<f64>(i % 2) * 16.0 + 2.0)));
  world.set(placed(24, std::nullopt));
  world.set(placed(25, std::nullopt));
  const u32 w = doc.add_layer(std::move(world));
  doc.set_layer_partition(w, tiles_of(16));
  Layer overrides("overrides", LayerRole::Feature);
  for (u32 i = 0; i < 6; ++i) {
    ObjectRecord r;
    r.id = id_of(i);
    r.properties.insert_or_assign("name", JsonValue("renamed"));
    overrides.set(std::move(r));
  }
  doc.add_layer(std::move(overrides));
  Layer props("props", LayerRole::Feature);
  for (u32 i = 30; i < 38; ++i)
    props.set(placed(i, std::make_pair(static_cast<f64>(i) * 10.0, 5.0)));
  const u32 p = doc.add_layer(std::move(props));
  doc.set_layer_partition(p, tiles_of(32));
  doc.set_edit_layer(w);
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
}

// What turns A into B. Journaled: one commit to world that touches most of its files — every
// record of tile (0, 0) moved away, so its file goes; a record into a new tile; an untiled record
// placed and a placed one taken off the grid, so untiled.json and the index change — journaled as
// a session commits. Otherwise the changes the journal does not see: a layer added with records,
// overrides repartitioned into tiles, props removed.
void change(const io::Vfs& vfs, const std::string& dir, Document& doc, DocumentManifest& manifest,
            bool journaled) {
  if (journaled) {
    Transaction tx = doc.begin(who());
    for (u32 i = 0; i < 24; i += 6)  // i % 3 == 0 and i % 2 == 0: tile (0, 0)
      REQUIRE(tx.apply(cmd_set(id_of(i), "position", point(40.0, 40.0))));
    REQUIRE(tx.apply(cmd_set(id_of(1), "position", point(-100.0, 7.0))));
    REQUIRE(tx.apply(cmd_set(id_of(24), "position", point(20.0, 20.0))));
    REQUIRE(tx.apply(cmd_clear(id_of(5), "position")));
    REQUIRE(tx.apply(cmd_set(id_of(7), "name", JsonValue("edited"))));
    commit(vfs, dir, doc, manifest, tx);
    return;
  }
  Layer extra("extra", LayerRole::Feature);
  for (u32 i = 50; i < 54; ++i)
    extra.set(placed(i, std::make_pair(static_cast<f64>(i), 0.0)));
  const u32 e = doc.add_layer(std::move(extra));
  doc.set_layer_partition(e, tiles_of(8));
  doc.set_layer_partition(static_cast<u32>(doc.find_layer("overrides")), tiles_of(16));
  REQUIRE(doc.remove_layer(static_cast<u32>(doc.find_layer("props"))));
}

}  // namespace

TEST_CASE("doc save: a save stopped between any two files leaves one whole state or the other") {
  TempDir tmp("engine_doc_save_crash");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  std::string error;

  for (const bool journaled : {true, false}) {
    CAPTURE(journaled);
    // How many file operations the save of B makes: the log, its writes and removals, the log's
    // removal. Every one of them is a place to die.
    u32 operations = 0;
    {
      const std::string dir = "docs://probe" + std::to_string(journaled);
      Document doc;
      DocumentManifest manifest;
      build_before(vfs, dir, doc, manifest);
      change(vfs, dir, doc, manifest, journaled);
      SaveReport report;
      REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
      CHECK(report.files_written >= 4);
      CHECK(report.files_removed >= 1);
      operations = 2 + report.files_written + report.files_removed;
    }

    for (u32 stop = 0; stop < operations; ++stop) {
      for (const bool tear : {false, true}) {
        // A torn write is a torn log or a torn file of the document, and the journaled run meets
        // both; the structural one adds removals and new layers, which do not tear.
        if (tear && !journaled) continue;
        CAPTURE(stop);
        CAPTURE(tear);
        const std::string name = "run" + std::to_string(journaled) + "_" + std::to_string(stop) +
                                 "_" + std::to_string(tear);
        const std::string dir = "docs://" + name;
        Document doc;
        DocumentManifest manifest;
        build_before(vfs, dir, doc, manifest);
        const Files before = files_on_disk(tmp.file(name));
        const u32 before_position = manifest.undo_position;
        change(vfs, dir, doc, manifest, journaled);
        const Files after = reference_files(doc, manifest);
        REQUIRE(before != after);

        SaveOptions dies;
        dies.stop_after = static_cast<i32>(stop);
        dies.tear = tear;
        CHECK_FALSE(DocumentStore::save(vfs, dir, doc, manifest, &error, nullptr, dies));

        // Whatever was left, a load reads one whole state and changes nothing on disk doing it.
        const Files left = files_on_disk(tmp.file(name), /*with_journal=*/true);
        Document loaded;
        DocumentManifest loaded_manifest;
        REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
        CHECK(files_on_disk(tmp.file(name), /*with_journal=*/true) == left);
        const Files read = reference_files(loaded, loaded_manifest);
        // Nothing of the document is touched before the log is whole, so a save stopped at its
        // first operation — before the log, or halfway through it — leaves A; any later, B.
        const Files& expected = stop == 0 ? before : after;
        CHECK_MESSAGE(read == expected, "first difference: " << first_difference(read, expected));

        // The next save finishes what the stopped one began, and the directory is that state's,
        // byte for byte, with no log left behind.
        REQUIRE(DocumentStore::save(vfs, dir, loaded, loaded_manifest, &error));
        CHECK(files_on_disk(tmp.file(name)) == expected);
        CHECK_FALSE(vfs.exists(dir + "/" + std::string(DocumentStore::k_save_log)));

        // The journal was written before the save: when the save left A, the commit is the first
        // patch of the redo tail, and redoing it gives B.
        if (journaled && stop == 0) {
          REQUIRE(loaded.journal().size() == before_position + 1);
          CHECK(loaded_manifest.undo_position == before_position);
          REQUIRE(loaded.redo(loaded.journal().back()));
          ++loaded_manifest.undo_position;
          CHECK(reference_files(loaded, loaded_manifest) == after);
        }
      }
    }
  }
}

TEST_CASE("doc save: a save log is read through on a read-only mount, and finished by a writer") {
  TempDir tmp("engine_doc_save_readonly");
  std::string error;
  Files after;
  {
    io::Vfs vfs;
    REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
    Document doc;
    DocumentManifest manifest;
    build_before(vfs, "docs://world", doc, manifest);
    change(vfs, "docs://world", doc, manifest, /*journaled=*/true);
    after = reference_files(doc, manifest);
    SaveOptions dies;
    dies.stop_after = 3;  // the log and two files
    REQUIRE_FALSE(DocumentStore::save(vfs, "docs://world", doc, manifest, &error, nullptr, dies));
  }
  io::Vfs readonly;
  REQUIRE(readonly.mount("docs", tmp.path(), /*writable=*/false) == io::Status::Ok);
  CHECK(DocumentStore::exists(readonly, "docs://world"));
  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(readonly, "docs://world", loaded, loaded_manifest, &error));
  CHECK(reference_files(loaded, loaded_manifest) == after);
  CHECK(readonly.exists("docs://world/" + std::string(DocumentStore::k_save_log)));
  CHECK_FALSE(DocumentStore::save(readonly, "docs://world", loaded, loaded_manifest, &error));

  io::Vfs writable;
  REQUIRE(writable.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  Document writer;
  DocumentManifest writer_manifest;
  REQUIRE(DocumentStore::load(writable, "docs://world", writer, writer_manifest, &error));
  REQUIRE(DocumentStore::save(writable, "docs://world", writer, writer_manifest, &error));
  CHECK(files_on_disk(tmp.file("world")) == after);
  CHECK_FALSE(writable.exists("docs://world/" + std::string(DocumentStore::k_save_log)));
}

TEST_CASE("doc save: the journal is cut in place, never rewritten, and a torn line is dropped") {
  TempDir tmp("engine_doc_save_journal");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;
  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  auto edit = [&](u32 n) {
    Transaction tx = doc.begin(who());
    REQUIRE(tx.apply(cmd_create(id_of(n), k_placement)));
    commit(vfs, dir, doc, manifest, tx);
    REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  };
  auto journal_on_disk = [&]() {
    std::string text;
    REQUIRE(vfs.read("docs://world/journal.jsonl", text) == io::Status::Ok);
    return text;
  };
  for (u32 n = 0; n < 4; ++n)
    edit(n);
  CHECK(journal_on_disk() == journal_text(doc));

  // Two undone, then a new commit: the redo tail is cut off the file and the new line appended,
  // which leaves the bytes a rewrite would.
  REQUIRE(doc.undo(doc.journal()[3]));
  REQUIRE(doc.undo(doc.journal()[2]));
  manifest.undo_position = 2;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  edit(10);
  REQUIRE(doc.journal().size() == 3);
  CHECK(journal_on_disk() == journal_text(doc));

  // A process died appending: half a line, no newline. The load leaves it out, the document reads
  // as the patches before it, and the next commit cuts it off before appending its own.
  const std::string whole = journal_on_disk();
  const std::string line = journal_text(doc).substr(0, 40);
  REQUIRE(io::append_file(tmp.file("world/journal.jsonl"), line) == io::Status::Ok);
  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(loaded.journal().size() == 3);
  CHECK(journal_on_disk() == whole + line);  // a load does not repair: a reader never writes
  Vector<Patch> read;
  REQUIRE(DocumentStore::load_journal(vfs, dir, read, &error));
  CHECK(read.size() == 3);
  doc = std::move(loaded);
  manifest = std::move(loaded_manifest);
  edit(11);
  CHECK(journal_on_disk() == journal_text(doc));
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(loaded.journal().size() == 4);
}

TEST_CASE("doc save: a side file is written only when its bytes change") {
  TempDir tmp("engine_doc_side_file");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  std::string error;
  REQUIRE(DocumentStore::write_side_file(vfs, "docs://world/leases.json", "{}\n", &error));
  const auto first = fs::last_write_time(tmp.native_file("world/leases.json"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(DocumentStore::write_side_file(vfs, "docs://world/leases.json", "{}\n", &error));
  CHECK(fs::last_write_time(tmp.native_file("world/leases.json")) == first);
  REQUIRE(DocumentStore::write_side_file(vfs, "docs://world/leases.json", "{\"a\":1}\n", &error));
  CHECK(read_bytes(tmp.native_file("world/leases.json")) == "{\"a\":1}\n");
  // An append makes the directory it needs.
  REQUIRE(
      DocumentStore::append_side_file(vfs, "docs://world/proposals/p.base.jsonl", "x\n", &error));
  REQUIRE(
      DocumentStore::append_side_file(vfs, "docs://world/proposals/p.base.jsonl", "y\n", &error));
  CHECK(read_bytes(tmp.native_file("world/proposals/p.base.jsonl")) == "x\ny\n");
}

TEST_CASE("doc save: a file that is not in the canonical form is rewritten by the next save") {
  TempDir tmp("engine_doc_save_canonical");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.add_layer("notes", LayerRole::Feature);
  {
    Transaction tx = doc.begin(who());
    REQUIRE(tx.apply(cmd_create(id_of(1), k_placement)));
    REQUIRE(tx.commit());
  }
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));

  // Someone reformats base.json by hand: the same records, other bytes.
  std::string text;
  REQUIRE(vfs.read("docs://world/layers/base.json", text) == io::Status::Ok);
  std::string reformatted;
  for (const char c : text) {
    if (c != ' ') reformatted.push_back(c);
  }
  REQUIRE(reformatted != text);
  REQUIRE(io::write_file(tmp.file("world/layers/base.json"), reformatted) == io::Status::Ok);

  // A load reads it, and the next save — of a change to another layer — puts it back as a store
  // that rewrote everything would have left it.
  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(loaded.dirty());
  CHECK(loaded.changes(0).whole);
  CHECK_FALSE(loaded.changes(1).any());
  SaveReport report;
  REQUIRE(DocumentStore::save(vfs, dir, loaded, loaded_manifest, &error, &report));
  REQUIRE(report.written.size() == 1);
  CHECK(report.written[0] == "layers/base.json");
  CHECK(files_on_disk(tmp.file("world")) == reference_files(loaded, loaded_manifest));
}
