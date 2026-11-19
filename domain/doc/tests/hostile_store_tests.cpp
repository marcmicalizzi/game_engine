// A document directory someone else made (T187, from T13's reading,
// docs/experiments/fuzzing-design.md): `session.open` loads any path a client names. A layer's
// `file` is held under the document by the rule `doc.place_asset` holds an asset to, and a
// `$parent` cycle is refused at load rather than handed to the index, the materializer and flecs.
// Each file is text a function returns, so the fuzz harness can lift it as a seed.

#include <domain/doc/document.h>
#include <domain/doc/document_store.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

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

// A directory link at `link` to `target` (T190): a symbolic link where the platform lets this
// process make one, else (Windows without the privilege) a junction, which any user may make.
// False when neither could be made.
bool make_directory_link(const std::string& target, const std::string& link) {
  std::error_code ec;
  std::filesystem::create_directory_symlink(target, link, ec);
  if (!ec) return true;
#ifdef _WIN32
  std::string command = "cmd /c mklink /J \"" + link + "\" \"" + target + "\" >nul 2>nul";
  for (char& c : command)
    if (c == '/') c = '\\';
  return std::system(command.c_str()) == 0 && std::filesystem::exists(link);
#else
  return false;
#endif
}

}  // namespace hostile_store

TEST_CASE(
    "doc store hostile: a layer file under the document by name but out by a link is refused") {
  const test::TempDir tmp("engine_doc_hostile_link");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  // A valid layer outside the document, and one inside it, each behind a directory link.
  REQUIRE(vfs.write("docs://outside/base.json", hostile_store::empty_layer()) == io::Status::Ok);
  REQUIRE(vfs.write("docs://d/manifest.json", hostile_store::manifest_naming("sub/base.json")) ==
          io::Status::Ok);
  REQUIRE(vfs.write("docs://d/kept/base.json", hostile_store::empty_layer()) == io::Status::Ok);
  REQUIRE(vfs.make_directories("docs://d/layers") == io::Status::Ok);
  REQUIRE(hostile_store::make_directory_link(tmp.file("outside"), tmp.file("d/layers/sub")));

  Document doc;
  DocumentManifest manifest;
  std::string error;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error));
  CHECK(error.find("through a link") != std::string::npos);
  CHECK(error.find("outside") == std::string::npos);  // where it leads is not told

  // The same manifest with the link pointing inside the document loads: the resolved path is
  // judged, not the link's presence.
  std::error_code ec;
  std::filesystem::remove(tmp.file("d/layers/sub"), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(hostile_store::make_directory_link(tmp.file("d/kept"), tmp.file("d/layers/sub")));
  REQUIRE_MESSAGE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error), error);
}

TEST_CASE("doc store hostile: a save does not write through a link the load never read") {
  // T191: a layer that did not exist at load gets a directory of its own at the next save, and a
  // document someone else made can hold that directory already, as a link out of it.
  const test::TempDir tmp("engine_doc_hostile_save_link");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  std::string error;
  {
    Document made;
    DocumentManifest made_manifest;
    REQUIRE_MESSAGE(DocumentStore::create(vfs, "docs://d", "D", made, made_manifest, &error),
                    error);
  }
  REQUIRE(vfs.make_directories("docs://outside") == io::Status::Ok);
  REQUIRE(hostile_store::make_directory_link(tmp.file("outside"), tmp.file("d/layers/placements")));

  Document doc;
  DocumentManifest manifest;
  REQUIRE_MESSAGE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error), error);
  const u32 placements = doc.add_layer("placements", LayerRole::Feature);
  LayerPartition grid;
  grid.property = "position";
  grid.tile_size = 32.0;
  doc.set_layer_partition(placements, grid);
  CHECK_FALSE(DocumentStore::save(vfs, "docs://d", doc, manifest, &error));
  CHECK(error.find("through a link") != std::string::npos);
  CHECK(error.find("outside") == std::string::npos);  // where it leads is not told
  Vector<io::DirEntry> written;
  REQUIRE(io::list_directory(tmp.file("outside"), written) == io::Status::Ok);
  CHECK(written.empty());  // nothing landed there, the save's log included
  CHECK_FALSE(std::filesystem::exists(tmp.file("d/save.pending")));

  // The same save with the directory a plain one writes the layer.
  std::error_code ec;
  std::filesystem::remove(tmp.file("d/layers/placements"), ec);
  REQUIRE_FALSE(ec);
  REQUIRE_MESSAGE(DocumentStore::save(vfs, "docs://d", doc, manifest, &error), error);
  CHECK(std::filesystem::exists(tmp.file("d/layers/placements/index.json")));

  // A journal that is a link out of the document is refused at load, before the appends that
  // follow a load would write through it. A file link needs the privilege on Windows: said, and
  // not run, where the process lacks it.
  REQUIRE(vfs.write("docs://outside/journal.jsonl", "") == io::Status::Ok);
  std::filesystem::remove(tmp.file("d/journal.jsonl"), ec);
  std::filesystem::create_symlink(tmp.file("outside/journal.jsonl"), tmp.file("d/journal.jsonl"),
                                  ec);
  if (ec) {
    MESSAGE("no file link could be made here, so the journal's case does not run: "
            << ec.message());
    return;
  }
  CHECK_FALSE(DocumentStore::load(vfs, "docs://d", doc, manifest, &error));
  CHECK(error.find("through a link") != std::string::npos);
}

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
