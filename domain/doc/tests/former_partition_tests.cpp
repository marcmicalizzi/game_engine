// A layer partitioned by a field that was renamed (roadmap D41; docs/subsystems/doc.md, "A renamed
// type or field"): written before the rename, it opens tiled by the field's new name and saves to
// the bytes a layer partitioned after the rename writes; a layer whose types disagree is refused
// by name. The types are `former_names.schema`'s `Post` (its `spot` formerly `place`) and `Stall`
// (still `place`); the documents are written as a build before the rename wrote them — records set
// through the layer, which renames nothing.
#include <core/json/json.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::doc;

namespace {
JsonValue json(std::string_view text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

ObjectRecord record(ObjectId id, std::string type, std::string_view properties) {
  ObjectRecord r;
  r.id = id;
  r.type = std::move(type);
  const JsonValue p = json(properties);
  for (auto [key, value] : p.as_object())
    r.properties.insert_or_assign(key, value);
  return r;
}
std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A document's layer files by relative path: what a save of its records writes.
std::map<std::string, std::string> layer_files(const std::filesystem::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir / "layers")) {
    if (!entry.is_regular_file()) continue;
    out[std::filesystem::relative(entry.path(), dir).generic_string()] = file_bytes(entry.path());
  }
  return out;
}
const ObjectId k_gate = Id128::from_parts(0xD41, 1);
const ObjectId k_well = Id128::from_parts(0xD41, 2);

}  // namespace

TEST_CASE("former names: a layer partitioned by a renamed field follows it") {
  test::TempDir tmp("engine_doc_former_partition");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  std::string error;
  LayerPartition grid;
  grid.tile_size = 32.0;

  // Written before the rename: `posts` is tiled by `place`, and so are its records.
  const std::string old_dir = "docs://old";
  {
    Document doc;
    DocumentManifest manifest;
    REQUIRE(DocumentStore::create(vfs, old_dir, "World", doc, manifest, &error));
    const u32 posts = doc.add_layer("posts", LayerRole::Feature);
    grid.property = "place";
    doc.set_layer_partition(posts, grid);
    doc.layer(posts).set(record(k_gate, "engine.doc.test.Post",
                                R"({"name": "gate", "place": [5, 0, 5]})"));
    doc.layer(posts).set(record(k_well, "engine.doc.test.Post",
                                R"({"name": "well", "place": [100, 0, -40]})"));
    REQUIRE_MESSAGE(DocumentStore::save(vfs, old_dir, doc, manifest, &error), error);
  }
  // The same content authored after the rename: tiled by `spot`.
  const std::string new_dir = "docs://new";
  {
    Document doc;
    DocumentManifest manifest;
    REQUIRE(DocumentStore::create(vfs, new_dir, "World", doc, manifest, &error));
    const u32 posts = doc.add_layer("posts", LayerRole::Feature);
    grid.property = "spot";
    doc.set_layer_partition(posts, grid);
    doc.layer(posts).set(record(k_gate, "engine.doc.test.Post",
                                R"({"name": "gate", "spot": [5, 0, 5]})"));
    doc.layer(posts).set(record(k_well, "engine.doc.test.Post",
                                R"({"name": "well", "spot": [100, 0, -40]})"));
    REQUIRE_MESSAGE(DocumentStore::save(vfs, new_dir, doc, manifest, &error), error);
  }

  // Opened after the rename: tiled by `spot`, every record where it was, and the next save writes
  // the bytes the layer authored after the rename has.
  Document doc;
  DocumentManifest manifest;
  REQUIRE_MESSAGE(DocumentStore::load(vfs, old_dir, doc, manifest, &error), error);
  const Document& read = doc;
  const i32 posts = read.find_layer("posts");
  REQUIRE(posts >= 0);
  CHECK(read.layer(static_cast<u32>(posts)).partition().property == "spot");
  TileCoord tile;
  REQUIRE(read.object_tile(k_well, tile));
  CHECK(tile == TileCoord{3, -2});
  REQUIRE_MESSAGE(DocumentStore::save(vfs, old_dir, doc, manifest, &error), error);
  const std::filesystem::path root(tmp.path());
  CHECK(layer_files(root / "old") == layer_files(root / "new"));

  // A layer whose types disagree — a stall still calls its position `place` — is refused, naming
  // both types and the new name.
  const std::string mixed_dir = "docs://mixed";
  {
    Document mixed;
    DocumentManifest mixed_manifest;
    REQUIRE(DocumentStore::create(vfs, mixed_dir, "World", mixed, mixed_manifest, &error));
    const u32 layer = mixed.add_layer("posts", LayerRole::Feature);
    grid.property = "place";
    mixed.set_layer_partition(layer, grid);
    mixed.layer(layer).set(record(k_gate, "engine.doc.test.Post",
                                  R"({"name": "gate", "place": [5, 0, 5]})"));
    mixed.layer(layer).set(record(k_well, "engine.doc.test.Stall",
                                  R"({"name": "well", "place": [6, 0, 6]})"));
    REQUIRE_MESSAGE(DocumentStore::save(vfs, mixed_dir, mixed, mixed_manifest, &error), error);
  }
  Document refused;
  DocumentManifest refused_manifest;
  CHECK_FALSE(DocumentStore::load(vfs, mixed_dir, refused, refused_manifest, &error));
  CHECK(error.find("tiled by 'place'") != std::string::npos);
  CHECK(error.find("engine.doc.test.Post renamed 'spot'") != std::string::npos);
  CHECK(error.find("engine.doc.test.Stall") != std::string::npos);
}
