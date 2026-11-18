// A document directory someone else made (T187, from T13's reading,
// docs/experiments/fuzzing-design.md): `session.open` loads any path a client names. A layer's
// `file` is held under the document by the rule `doc.place_asset` holds an asset to, and a `$parent`
// cycle is refused at load rather than handed to the index, the materializer and flecs. Each file
// is text a function returns, so the fuzz harness can lift it as a seed.

#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::doc;

namespace hostile_store {

// A manifest of one single-file layer, "base", whose `file` is `file` (JSON-escaped by the caller).
std::string manifest_naming(const std::string& file) {
  return R"({"name":"x","layers":[{"name":"base","role":"Base","file":")" + file +
         R"("}],"edit_layer":"base","undo_position":0})";
}

std::string empty_layer() { return R"({"name":"base","role":"Base","objects":[]})"; }

// A layer of two objects, each the other's parent.
std::string layer_with_parent_cycle() {
  return R"({"name":"base","role":"Base","objects":[)"
         R"({"id":"00000000000000000000000000000001","type":"engine.doc.test.Placement",)"
         R"("parent":"00000000000000000000000000000002","properties":{}},)"
         R"({"id":"00000000000000000000000000000002","type":"engine.doc.test.Placement",)"
         R"("parent":"00000000000000000000000000000001","properties":{}}]})";
}

// A layer whose `file` climbs out of the document's directory.
std::string manifest_climbing_out() { return manifest_naming("../../outside.json"); }

}  // namespace hostile_store

TEST_CASE("doc store hostile: a layer file outside the document is refused by the rule alone") {
  const test::TempDir tmp("engine_doc_hostile_file");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  // A valid layer where the climbing path lands, so a reader that followed it would load.
  REQUIRE(vfs.write("docs://outside.json", hostile_store::empty_layer()) == io::Status::Ok);
  REQUIRE(vfs.write("docs://d/manifest.json", hostile_store::manifest_climbing_out()) ==
          io::Status::Ok);
  Document doc;
  DocumentManifest manifest;
  std::string error;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error));
  CHECK(error.find("layer 'base'") != std::string::npos);
  CHECK(error.find("outside.json") == std::string::npos);  // the file system's answer is not given
  CHECK(error.find("not found") == std::string::npos);

  // A path the rule refuses that names nothing at all gets the same sentence.
  REQUIRE(vfs.write("docs://e/manifest.json", hostile_store::manifest_naming("../nowhere.json")) ==
          io::Status::Ok);
  std::string other;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://e", doc, manifest, &other));
  CHECK(other.find("layer 'base'") != std::string::npos);
  CHECK(other.find("not found") == std::string::npos);

  // The same layer under the document loads.
  REQUIRE(vfs.write("docs://f/manifest.json", hostile_store::manifest_naming("base.json")) ==
          io::Status::Ok);
  REQUIRE(vfs.write("docs://f/layers/base.json", hostile_store::empty_layer()) == io::Status::Ok);
  REQUIRE_MESSAGE(DocumentStore::load(vfs, "docs://f", doc, manifest, &error), error);
}

TEST_CASE("doc store hostile: a $parent cycle is refused at load") {
  const test::TempDir tmp("engine_doc_hostile_cycle");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  REQUIRE(vfs.write("docs://d/manifest.json", hostile_store::manifest_naming("base.json")) ==
          io::Status::Ok);
  REQUIRE(vfs.write("docs://d/layers/base.json", hostile_store::layer_with_parent_cycle()) ==
          io::Status::Ok);
  Document doc;
  DocumentManifest manifest;
  std::string error;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error));
  CHECK(error.find("cycle") != std::string::npos);
  CHECK(error.find("00000000000000000000000000000001") != std::string::npos);
}
