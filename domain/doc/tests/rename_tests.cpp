// A rename is an alias (roadmap D28; plan 03 §3.8, "Rename is an alias, never delete-and-add";
// docs/subsystems/doc.md, "A renamed type or field"). A document written before a type or a field
// was renamed opens, its records — definitions and overrides — take the new names, and its next
// save writes them; the migration corpus holds one such document and the bytes it saves to.
//
// The types are `doc_test_types.schema`'s `Marker` (formerly `engine.doc.test.Beacon`, its `label`
// formerly `title`) and `Glow` (its `strength` formerly `power`).
#include <core/json/json.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::doc;

namespace {

constexpr const char* k_old_type = "engine.doc.test.Beacon";
constexpr const char* k_new_type = "engine.doc.test.Marker";

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

std::string environment(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A document directory's files by relative path, the journal and the save log left out: what a save
// of the document's content writes.
std::map<std::string, std::string> document_files(const std::filesystem::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string relative = std::filesystem::relative(entry.path(), dir).generic_string();
    if (relative == "journal.jsonl" || relative == "save.pending") continue;
    out[relative] = file_bytes(entry.path());
  }
  return out;
}

// The corpus's renamed document: `before/` as a build before the rename wrote it, `after/` what
// this build saves it to.
std::string corpus_root() {
  return test::data_path(ENGINE_SOURCE_DIR "/content/migration-corpus/documents/renamed",
                         "content/migration-corpus/documents/renamed");
}

const ObjectId k_lamp = Id128::from_parts(0x28, 1);
const ObjectId k_buoy = Id128::from_parts(0x28, 2);
const ObjectId k_mast = Id128::from_parts(0x28, 3);
const ObjectId k_fact = Id128::from_parts(0x28, 4);
const ObjectId k_orphan = Id128::from_parts(0x28, 5);

// The document a build before the rename wrote: three `Beacon`s with a `title`, two of them glowing
// with a `power`, in a layer of 32 m tiles; and a single-file layer above it holding a `Fact`, an
// override of one beacon's `title` and of another's glow, and an override of an object no layer
// defines that sets a `title` too.
void build_before(Document& doc) {
  Layer places("places", LayerRole::Feature);
  places.set(record(k_lamp, k_old_type,
                    R"({"title": "lamp", "position": [5.5, 0.0, 3.25], "glow": {"power": 2.5}})"));
  places.set(record(k_buoy, k_old_type, R"({"title": "buoy", "position": [40.0, 1.0, -7.5]})"));
  places.set(record(k_mast, k_old_type,
                    R"({"title": "mast", "position": [-12.25, 0.0, 70.0], "glow": {}})"));
  const u32 p = doc.add_layer(std::move(places));
  doc.set_layer_partition(p, LayerPartition{"position", 32.0});
  Layer notes("notes", LayerRole::Feature);
  notes.set(record(k_fact, "engine.doc.test.Fact", R"({"claim": "the lamp is lit"})"));
  notes.set(record(k_lamp, "", R"({"title": "harbour lamp"})"));
  notes.set(record(k_mast, "", R"({"glow": {"power": 0.5}})"));
  notes.set(record(k_orphan, "", R"({"title": "nobody's"})"));
  const u32 n = doc.add_layer(std::move(notes));
  doc.set_edit_layer(n);
}

// What a build before the rename stamped in each layer file's type table and this one cannot (it
// has no type named `Beacon`): `engine.doc.test.Beacon` at version 1 wherever a definition is one.
void stamp_old_type(const std::filesystem::path& dir) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir / "layers")) {
    if (!entry.is_regular_file() || entry.path().filename() == "index.json") continue;
    JsonValue file = json(file_bytes(entry.path()));
    bool holds = false;
    for (usize i = 0; i < file["objects"].size(); ++i) {
      const JsonValue* type = file["objects"][i].find("type");
      holds = holds || (type != nullptr && type->as_string() == k_old_type);
    }
    if (!holds) continue;
    JsonValue v = JsonValue::object();
    v.set("name", JsonValue(k_old_type));
    v.set("version", JsonValue(u64{1}));
    JsonValue& table = file["types"];
    JsonValue sorted = JsonValue::array();
    bool placed = false;
    for (usize i = 0; i < table.size(); ++i) {
      if (!placed && std::string_view(k_old_type) < table[i]["name"].as_string()) {
        sorted.push_back(v);
        placed = true;
      }
      sorted.push_back(table[i]);
    }
    if (!placed) sorted.push_back(v);
    table = std::move(sorted);
    std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
    out << write_json(file);
  }
}

}  // namespace

// A definition is renamed as its file is read, before anything asks for its tile: with no property
// named by the partition, a record's tile is read from its type's position field, and a type the
// registry knows only by a former name has none.
TEST_CASE("rename: a definition takes its type's and its fields' new names as its file is read") {
  LayerFile file;
  file.name = "places";
  file.objects.push_back(record(k_lamp, k_old_type,
                                R"({"title": "lamp", "position": [40.0, 0.0, -7.5],
                                    "glow": {"power": 2.5}})"));
  const LayerPartition by_type{"", 32.0};
  TileCoord tile;
  CHECK_FALSE(tile_of(file.objects[0], by_type, tile));  // no type of that name: untiled

  std::string why;
  REQUIRE(rename_defining_records(file, why));
  const ObjectRecord& r = file.objects[0];
  CHECK(r.type == k_new_type);
  CHECK(r.properties.find_value(std::string_view("title")) == nullptr);
  REQUIRE(r.properties.find_value(std::string_view("label")) != nullptr);
  CHECK(r.properties.find_value(std::string_view("label"))->as_string() == "lamp");
  CHECK(*r.properties.find_value(std::string_view("glow")) == json(R"({"strength": 2.5})"));
  REQUIRE(tile_of(r, by_type, tile));
  CHECK(tile == TileCoord{1, -1});

  // Renamed twice is renamed once: the new names are the new names.
  LayerFile again = file;
  REQUIRE(rename_defining_records(again, why));
  CHECK(again.objects[0] == file.objects[0]);

  // A type this build does not have is data, as it always was; one field under two names is refused
  // naming the layer, the record and both.
  LayerFile foreign;
  foreign.name = "places";
  foreign.objects.push_back(record(k_buoy, "engine.doc.test.Nowhere", R"({"title": "x"})"));
  REQUIRE(rename_defining_records(foreign, why));
  CHECK(foreign.objects[0].properties.find_value(std::string_view("title")) != nullptr);
  LayerFile both;
  both.name = "places";
  both.objects.push_back(record(k_buoy, k_old_type, R"({"title": "a", "label": "b"})"));
  CHECK_FALSE(rename_defining_records(both, why));
  CAPTURE(why);
  CHECK(why.find("places/") == 0);
  CHECK(why.find("carries both 'title' and 'label'") != std::string::npos);

  // A layer file read whole goes through the same rename, and a type table naming the former name
  // is held to the renamed type's version: newer than this build's is refused.
  Layer read("x", LayerRole::Base);
  schema::ReadContext ctx;
  const std::string old_file =
      R"({"name": "x", "role": "Base", "types": [{"name": "engine.doc.test.Beacon", "version": 1}],
          "objects": [{"id": "00000000000000280000000000000001", "type": "engine.doc.test.Beacon",
                       "properties": {"title": "t", "position": [0, 0, 0]}}]})";
  REQUIRE(Layer::from_json_text(old_file, read, ctx));
  REQUIRE(read.find(k_lamp) != nullptr);
  CHECK(read.find(k_lamp)->type == k_new_type);
  const std::string newer =
      std::string(old_file).replace(old_file.find("\"version\": 1"), 12, "\"version\": 2");
  schema::ReadContext newer_ctx;
  CHECK_FALSE(Layer::from_json_text(newer, read, newer_ctx));
  CHECK(newer_ctx.diagnostics[0].message.find("engine.doc.test.Beacon records at version 2") !=
        std::string::npos);
}

// The whole load: definitions renamed as their tiles are read, overrides by the type the composed
// index gives them, an override of an object no layer defines kept as written and reported, and the
// next save writing every renamed file in the new names — once.
TEST_CASE("rename: a document written before a rename opens in the new names and saves them") {
  test::TempDir tmp("doc_rename");
  REQUIRE(tmp.ok());
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://renamed";
  std::string error;
  {
    Document doc;
    DocumentManifest manifest;
    REQUIRE(DocumentStore::create(vfs, dir, "renamed", doc, manifest, &error));
    build_before(doc);
    REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  }
  stamp_old_type(std::filesystem::path(tmp.path()) / "renamed");

  Document doc;
  DocumentManifest manifest;
  REQUIRE_MESSAGE(DocumentStore::load(vfs, dir, doc, manifest, &error), error);
  CHECK(doc.type_of(k_lamp) == k_new_type);
  CHECK(doc.property(k_lamp, "title") == nullptr);
  REQUIRE(doc.property(k_lamp, "label") != nullptr);
  CHECK(doc.property(k_lamp, "label")->as_string() == "harbour lamp");  // the override's, renamed
  CHECK(*doc.property(k_mast, "glow") == json(R"({"strength": 0.5})"));
  TileCoord buoy_tile;
  REQUIRE(doc.object_tile(k_buoy, buoy_tile));
  CHECK(buoy_tile == TileCoord{1, -1});
  REQUIRE(doc.kept_names().size() == 1);
  CHECK(doc.kept_names()[0].layer == "notes");
  CHECK(doc.kept_names()[0].id == k_orphan);
  CHECK(doc.kept_names()[0].property == "title");
  const Layer& notes = std::as_const(doc).layer(static_cast<u32>(doc.find_layer("notes")));
  CHECK(notes.find(k_orphan)->properties.find_value(std::string_view("title")) != nullptr);
  CHECK(doc.validate_index());
  Vector<Diagnostic> diagnostics;
  doc.validate(diagnostics);
  for (const Diagnostic& d : diagnostics)  // only the orphan, which was an error before the rename
    CHECK_MESSAGE(d.path.find("notes/") == 0, d.path << ": " << d.message);

  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  const auto once = document_files(std::filesystem::path(tmp.path()) / "renamed");
  for (const auto& [path, bytes] : once) {
    CAPTURE(path);
    CHECK(bytes.find("engine.doc.test.Beacon") == std::string::npos);
    CHECK(bytes.find("\"power\"") == std::string::npos);
  }
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  CHECK(document_files(std::filesystem::path(tmp.path()) / "renamed") == once);

  // One field under both names in an override is refused by name, as in a definition.
  {
    Document bad;
    DocumentManifest bad_manifest;
    REQUIRE(DocumentStore::create(vfs, "docs://both", "both", bad, bad_manifest, &error));
    build_before(bad);
    Layer& notes_layer = bad.layer(static_cast<u32>(bad.find_layer("notes")));
    notes_layer.find(k_lamp)->properties.insert_or_assign("label", JsonValue("again"));
    REQUIRE(DocumentStore::save(vfs, "docs://both", bad, bad_manifest, &error));
  }
  Document refused;
  DocumentManifest refused_manifest;
  CHECK_FALSE(DocumentStore::load(vfs, "docs://both", refused, refused_manifest, &error));
  CHECK(error.find("carries both 'title' and 'label'") != std::string::npos);
}

// **The corpus's renamed document** (content/migration-corpus/README.md, "The documents"):
// `before/` opens and saves to `after/` byte for byte — the same bytes on every toolchain, since a
// layer file is canonical JSON whose numbers are the shortest text of their doubles — and `after/`
// saves to itself.
TEST_CASE("rename: the corpus's renamed document saves to the new names, byte for byte") {
  const std::filesystem::path root = corpus_root();
  if (!test::path_exists(root.string())) {
    REQUIRE_MESSAGE(!test::bundle_root().empty(), "the migration corpus is missing: " << root);
    MESSAGE("not in this bundle: " << root);
    return;
  }
  test::TempDir tmp("doc_rename_corpus");
  REQUIRE(tmp.ok());
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path(), /*writable=*/true) == io::Status::Ok);
  for (const char* name : {"before", "after"}) {
    CAPTURE(name);
    std::filesystem::copy(root / name, std::filesystem::path(tmp.path()) / name,
                          std::filesystem::copy_options::recursive);
    const std::string dir = std::string("docs://") + name;
    Document doc;
    DocumentManifest manifest;
    std::string error;
    REQUIRE_MESSAGE(DocumentStore::load(vfs, dir, doc, manifest, &error), error);
    REQUIRE(doc.kept_names().size() == 1);
    CHECK(doc.kept_names()[0].id == k_orphan);
    REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
    CHECK(document_files(std::filesystem::path(tmp.path()) / name) ==
          document_files(root / "after"));
  }
}

// Writes the corpus's `before/` when `ENGINE_DOC_CORPUS_RENAMED_OUT` names a directory, and
// `after/` beside it (content/migration-corpus/README.md, "The documents"); does nothing without
// it, so the suite never writes into the repository. `before/` is this build's save of the document
// in the old names with the type tables a build before the rename stamped; `after/` is that, opened
// and saved.
TEST_CASE("rename: write the corpus's renamed document when asked") {
  const std::string out = environment("ENGINE_DOC_CORPUS_RENAMED_OUT");
  if (out.empty()) return;
  io::Vfs vfs;
  REQUIRE(vfs.mount("out", out, /*writable=*/true) == io::Status::Ok);
  std::string error;
  {
    Document doc;
    DocumentManifest manifest;
    REQUIRE_MESSAGE(DocumentStore::create(vfs, "out://before", "renamed", doc, manifest, &error),
                    error);
    build_before(doc);
    REQUIRE(DocumentStore::save(vfs, "out://before", doc, manifest, &error));
  }
  stamp_old_type(std::filesystem::path(out) / "before");
  std::filesystem::copy(std::filesystem::path(out) / "before", std::filesystem::path(out) / "after",
                        std::filesystem::copy_options::recursive);
  Document doc;
  DocumentManifest manifest;
  REQUIRE_MESSAGE(DocumentStore::load(vfs, "out://after", doc, manifest, &error), error);
  REQUIRE(DocumentStore::save(vfs, "out://after", doc, manifest, &error));
  MESSAGE("wrote " << out << "/before and /after");
}
