#include <domain/doc/document_store.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::doc;

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// this one's documents while it saves them (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

Attribution who() {
  Attribution a;
  a.actor = "test";
  a.role = "author";
  a.timestamp_unix_ms = 1;
  return a;
}

}  // namespace

TEST_CASE("doc store: layer file names are sanitized") {
  CHECK(DocumentStore::layer_file_name("base") == "base.json");
  CHECK(DocumentStore::layer_file_name("quest/ch 1:a") == "quest_ch_1_a.json");
  CHECK(DocumentStore::layer_file_name("") == "layer.json");
  CHECK(DocumentStore::path_of("a/b", "c.json") == "a/b/c.json");
  CHECK(DocumentStore::path_of("a/b/", "c.json") == "a/b/c.json");
  CHECK(DocumentStore::path_of("content://docs/x", "manifest.json") ==
        "content://docs/x/manifest.json");
}

TEST_CASE("doc store: create, save, and load round trip through a mount") {
  TempDir tmp("engine_doc_store");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  CHECK_FALSE(DocumentStore::exists(vfs, dir));
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  CHECK(error.empty());
  CHECK(DocumentStore::exists(vfs, dir));
  CHECK(manifest.name == "World");
  REQUIRE(manifest.layers.size() == 1);
  CHECK(manifest.layers[0].name == "base");
  CHECK(manifest.layers[0].file == "base.json");
  CHECK(manifest.edit_layer == "base");
  CHECK_FALSE(DocumentStore::create(vfs, dir, "Again", doc, manifest, &error));
  CHECK(error.find("already exists") != std::string::npos);

  // Edit: a second layer, objects in both, one committed patch.
  doc.add_layer("quest", LayerRole::Feature);
  const ObjectId a = Id128::from_parts(1, 1);
  const ObjectId b = Id128::from_parts(1, 2);
  {
    Transaction tx = doc.begin(who());
    CHECK(tx.apply(cmd_create(a, "engine.content.AssetProvenance")));
    CHECK(tx.apply(cmd_set(a, "generator", JsonValue("gen"))));
    CHECK(tx.commit());
  }
  doc.set_edit_layer(1);
  {
    Transaction tx = doc.begin(who());
    CHECK(tx.apply(cmd_create(b, "engine.content.AssetProvenance", a)));
    CHECK(tx.commit());
  }
  manifest.undo_position = 2;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  REQUIRE(DocumentStore::write_journal(vfs, dir, doc.journal(), &error));
  REQUIRE(manifest.layers.size() == 2);
  CHECK(manifest.layers[1].name == "quest");
  CHECK(manifest.layers[1].role == LayerRole::Feature);
  CHECK(manifest.edit_layer == "quest");
  CHECK(vfs.exists("docs://world/layers/quest.json"));
  CHECK(vfs.exists("docs://world/journal.jsonl"));

  // Append a third patch directly.
  doc.set_edit_layer(0);
  {
    Transaction tx = doc.begin(who());
    CHECK(tx.apply(cmd_set(a, "prompt_hash", JsonValue(u64{7}))));
    CHECK(tx.commit());
  }
  REQUIRE(DocumentStore::append_journal(vfs, dir, doc.journal().back(), &error));
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));

  Document loaded;
  DocumentManifest loaded_manifest;
  error.clear();
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(error.empty());
  CHECK(loaded_manifest.name == "World");
  CHECK(loaded_manifest.undo_position == 2);
  REQUIRE(loaded.layer_count() == 2);
  CHECK(loaded.layer(0) == doc.layer(0));
  CHECK(loaded.layer(1) == doc.layer(1));
  CHECK(loaded.edit_layer() == 0);
  REQUIRE(loaded.journal().size() == 3);
  CHECK(loaded.journal() == doc.journal());
  CHECK(loaded.exists(b));
  CHECK(*loaded.property(a, "prompt_hash") == JsonValue(u64{7}));

  // Layer files are canonical: saving again is byte-identical.
  std::string before;
  REQUIRE(vfs.read("docs://world/layers/base.json", before) == io::Status::Ok);
  REQUIRE(DocumentStore::save(vfs, dir, loaded, loaded_manifest, &error));
  std::string after;
  REQUIRE(vfs.read("docs://world/layers/base.json", after) == io::Status::Ok);
  CHECK(before == after);

  Vector<Patch> journal;
  REQUIRE(DocumentStore::load_journal(vfs, dir, journal, &error));
  CHECK(journal.size() == 3);
  REQUIRE(
      DocumentStore::write_journal(vfs, dir, std::span<const Patch>(journal.data(), 1), &error));
  REQUIRE(DocumentStore::load_journal(vfs, dir, journal, &error));
  CHECK(journal.size() == 1);
}

TEST_CASE("doc store: load failures are reported with paths") {
  TempDir tmp("engine_doc_store");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  Document doc;
  DocumentManifest manifest;
  std::string error;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://missing", doc, manifest, &error));
  CHECK(error.find("manifest.json") != std::string::npos);
  CHECK(error.find("not found") != std::string::npos);

  REQUIRE(vfs.write("docs://broken/manifest.json", "{not json") == io::Status::Ok);
  CHECK_FALSE(DocumentStore::load(vfs, "docs://broken", doc, manifest, &error));
  CHECK(error.find("broken/manifest.json") != std::string::npos);

  REQUIRE(vfs.write("docs://nolayers/manifest.json",
                    "{\"name\":\"x\",\"layers\":[],\"edit_layer\":\"\",\"undo_position\":0}") ==
          io::Status::Ok);
  CHECK_FALSE(DocumentStore::load(vfs, "docs://nolayers", doc, manifest, &error));
  CHECK(error.find("no layers") != std::string::npos);

  REQUIRE(vfs.write("docs://badjournal/manifest.json",
                    "{\"name\":\"x\",\"layers\":[{\"name\":\"base\",\"role\":\"Base\",\"file\":"
                    "\"base.json\"}],"
                    "\"edit_layer\":\"base\",\"undo_position\":0}") == io::Status::Ok);
  REQUIRE(vfs.write("docs://badjournal/layers/base.json",
                    "{\"name\":\"base\",\"objects\":[],\"role\":\"Base\"}") == io::Status::Ok);
  REQUIRE(vfs.write("docs://badjournal/journal.jsonl", "{}\n{oops\n") == io::Status::Ok);
  CHECK_FALSE(DocumentStore::load(vfs, "docs://badjournal", doc, manifest, &error));
  CHECK(error.find("journal.jsonl") != std::string::npos);
  CHECK(error.find("line 2") != std::string::npos);
}
