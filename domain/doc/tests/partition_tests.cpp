// Tile-partitioned layer files (docs/plan/03-data-model.md §3.7, docs/subsystems/doc.md): the
// file layout, its canonical bytes, what a save rewrites, and what a malformed index does.
#include <core/json/json.h>
#include <domain/doc/document_store.h>
#include <domain/doc/merge.h>
#include <domain/doc/partition.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <schemas/doc_test_types.h>
#include <string>

using namespace engine;
using namespace engine::doc;

namespace {

// A type with a `position`, one with a `transform`, one with neither: how a type opts into a
// tile grid when the partition names no property (domain/doc/tests/doc_test_types.schema).
const char* const k_placement = "engine.doc.test.Placement";
const char* const k_fact = "engine.doc.test.Fact";

struct TempDir {
  std::string path;
  explicit TempDir(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = io::normalize_path(p.string());
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
};

ObjectId id_of(u32 n) { return Id128::from_parts(1, n + 1); }

LayerPartition partition_of(std::string property, f64 tile_size) {
  LayerPartition p;
  p.property = std::move(property);
  p.tile_size = tile_size;
  return p;
}

JsonValue point(f64 x, f64 z) {
  JsonValue::Array a;
  a.push_back(JsonValue(x));
  a.push_back(JsonValue(0.0));
  a.push_back(JsonValue(z));
  return JsonValue(std::move(a));
}

ObjectRecord placed(u32 n, f64 x, f64 z) {
  ObjectRecord r;
  r.id = id_of(n);
  r.type = k_placement;
  r.parent = ObjectId{};
  r.properties.insert_or_assign("position", point(x, z));
  return r;
}

Attribution who() {
  Attribution a;
  a.actor = "partition-test";
  a.role = "test";
  a.task = "unit";
  a.timestamp_unix_ms = 1;
  return a;
}

u32 count_files(const std::string& dir) {
  u32 n = 0;
  const std::filesystem::path p(dir);
  if (!std::filesystem::exists(p)) return 0;
  for (const auto& entry : std::filesystem::directory_iterator(p)) {
    if (entry.is_regular_file()) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("doc partition: tile coordinates, file names, and the property a type opts in with") {
  const LayerPartition named = partition_of("position", 64);
  TileCoord tile;

  CHECK(tile_file_name(TileCoord{0, 0}) == "0_0.json");
  CHECK(tile_file_name(TileCoord{-1, 2}) == "-1_2.json");
  CHECK(parse_tile_file_name("-1_2.json", tile));
  CHECK(tile == TileCoord{-1, 2});
  CHECK(parse_tile_file_name("3_-4", tile));
  CHECK(tile == TileCoord{3, -4});
  CHECK_FALSE(parse_tile_file_name("01_0.json", tile));  // one spelling per tile
  CHECK_FALSE(parse_tile_file_name("-0_0.json", tile));
  CHECK_FALSE(parse_tile_file_name("0.json", tile));
  CHECK_FALSE(parse_tile_file_name("a_0.json", tile));

  // Floor, so the tile below the origin is -1 and the grid has no seam at zero.
  CHECK(tile_of(placed(0, 0, 0), named, tile));
  CHECK(tile == TileCoord{0, 0});
  CHECK(tile_of(placed(0, 63.9, -0.1), named, tile));
  CHECK(tile == TileCoord{0, -1});
  CHECK(tile_of(placed(0, -64, 128), named, tile));
  CHECK(tile == TileCoord{-1, 2});

  // Three components are the horizontal plane (x, z); two are (x, y).
  ObjectRecord flat = placed(1, 0, 0);
  JsonValue::Array a;
  a.push_back(JsonValue(200.0));
  a.push_back(JsonValue(-200.0));
  flat.properties.insert_or_assign("position", JsonValue(std::move(a)));
  CHECK(tile_of(flat, named, tile));
  CHECK(tile == TileCoord{3, -4});

  // An object position, and a transform with the position one member in.
  ObjectRecord object_form = placed(2, 0, 0);
  JsonValue o = JsonValue::object();
  o.set("x", JsonValue(65.0));
  o.set("y", JsonValue(9.0));
  o.set("z", JsonValue(65.0));
  object_form.properties.insert_or_assign("position", std::move(o));
  CHECK(tile_of(object_form, named, tile));
  CHECK(tile == TileCoord{1, 1});

  ObjectRecord transform = placed(3, 0, 0);
  JsonValue t = JsonValue::object();
  t.set("translation", point(-1, -1));
  t.set("scale", JsonValue(2.0));
  transform.properties.insert_or_assign("position", std::move(t));
  CHECK(tile_of(transform, named, tile));
  CHECK(tile == TileCoord{-1, -1});

  // No position, a position that is not one, and a partition that is off.
  ObjectRecord bare;
  bare.id = id_of(4);
  bare.type = k_placement;
  CHECK_FALSE(tile_of(bare, named, tile));
  ObjectRecord nonsense = placed(5, 0, 0);
  nonsense.properties.insert_or_assign("position", JsonValue("over there"));
  CHECK_FALSE(tile_of(nonsense, named, tile));
  CHECK_FALSE(tile_of(placed(6, 0, 0), LayerPartition{}, tile));

  // With no property named, a type opts in by declaring one: Placement has a `position`,
  // Fixture a `transform`, Fact neither.
  const LayerPartition automatic = partition_of("", 64);
  CHECK(position_property(placed(7, 0, 0), automatic) == "position");
  CHECK(tile_of(placed(7, 128, 128), automatic, tile));
  CHECK(tile == TileCoord{2, 2});

  ObjectRecord fixture;
  fixture.id = id_of(8);
  fixture.type = "engine.doc.test.Fixture";
  JsonValue placement = JsonValue::object();
  placement.set("translation", point(-300, 300));
  fixture.properties.insert_or_assign("transform", std::move(placement));
  CHECK(position_property(fixture, automatic) == "transform");
  CHECK(tile_of(fixture, automatic, tile));
  CHECK(tile == TileCoord{-5, 4});

  ObjectRecord fact;
  fact.id = id_of(9);
  fact.type = k_fact;
  fact.properties.insert_or_assign("position", point(0, 0));  // not a field of the type
  CHECK(position_property(fact, automatic).empty());
  CHECK_FALSE(tile_of(fact, automatic, tile));

  // An override-only record has no type to ask, so the automatic form leaves it untiled; a
  // partition that names the property tiles it like any other record.
  ObjectRecord untyped;
  untyped.id = id_of(10);
  untyped.properties.insert_or_assign("position", point(0, 0));
  CHECK(position_property(untyped, automatic).empty());
  CHECK_FALSE(tile_of(untyped, automatic, tile));
  CHECK(tile_of(untyped, named, tile));
  CHECK(tile == TileCoord{0, 0});
}

TEST_CASE("doc partition: the file layout and its canonical bytes") {
  TempDir tmp("engine_doc_partition_layout");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path, /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.set_layer_partition(0, partition_of("position", 64));

  Layer& base = doc.layer(0);
  base.set(placed(0, 1, 1));     // tile 0_0
  base.set(placed(1, 2, 2));     // tile 0_0
  base.set(placed(2, 70, -10));  // tile 1_-1
  ObjectRecord global;           // a type with no place on the grid: untiled
  global.id = id_of(3);
  global.type = k_fact;
  global.properties.insert_or_assign("claim", JsonValue("the Ironbound founded Hearth"));
  base.set(std::move(global));
  doc.rebuild_index();
  doc.mark_all_dirty();

  SaveReport report;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &report));
  CHECK(error.empty());
  REQUIRE(manifest.layers.size() == 1);
  CHECK(manifest.layers[0].file == "base/index.json");
  REQUIRE(manifest.layers[0].partition.has_value());
  CHECK(manifest.layers[0].partition->property == "position");
  CHECK(manifest.layers[0].partition->tile_size == 64);
  CHECK(report.tiles_total == 2);
  CHECK(report.tiles_written == 2);
  CHECK(vfs.exists("docs://world/layers/base/index.json"));
  CHECK(vfs.exists("docs://world/layers/base/tiles/0_0.json"));
  CHECK(vfs.exists("docs://world/layers/base/tiles/1_-1.json"));
  CHECK(vfs.exists("docs://world/layers/base/untiled.json"));
  CHECK_FALSE(vfs.exists("docs://world/layers/base.json"));

  // A tile file is a LayerFile restricted to its records: the same header, the same record
  // form, the same key order as a single-file layer.
  std::string text;
  REQUIRE(vfs.read("docs://world/layers/base/tiles/1_-1.json", text) == io::Status::Ok);
  CHECK(text ==
        "{\n"
        "  \"name\": \"base\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"deleted\": false,\n"
        "      \"id\": \"00000000000000010000000000000003\",\n"
        "      \"parent\": \"00000000000000000000000000000000\",\n"
        "      \"properties\": {\n"
        "        \"position\": [\n"
        "          70.0,\n"
        "          0.0,\n"
        "          -10.0\n"
        "        ]\n"
        "      },\n"
        "      \"type\": \"engine.doc.test.Placement\"\n"
        "    }\n"
        "  ],\n"
        "  \"role\": \"Base\"\n"
        "}");

  REQUIRE(vfs.read("docs://world/layers/base/index.json", text) == io::Status::Ok);
  CHECK(text ==
        "{\n"
        "  \"max_x\": 1,\n"
        "  \"max_y\": 0,\n"
        "  \"min_x\": 0,\n"
        "  \"min_y\": -1,\n"
        "  \"name\": \"base\",\n"
        "  \"partition\": {\n"
        "    \"property\": \"position\",\n"
        "    \"tile_size\": 64.0\n"
        "  },\n"
        "  \"role\": \"Base\",\n"
        "  \"tiles\": [\n"
        "    {\n"
        "      \"file\": \"0_0.json\",\n"
        "      \"objects\": [\n"
        "        \"00000000000000010000000000000001\",\n"
        "        \"00000000000000010000000000000002\"\n"
        "      ],\n"
        "      \"x\": 0,\n"
        "      \"y\": 0\n"
        "    },\n"
        "    {\n"
        "      \"file\": \"1_-1.json\",\n"
        "      \"objects\": [\n"
        "        \"00000000000000010000000000000003\"\n"
        "      ],\n"
        "      \"x\": 1,\n"
        "      \"y\": -1\n"
        "    }\n"
        "  ],\n"
        "  \"untiled\": [\n"
        "    \"00000000000000010000000000000004\"\n"
        "  ]\n"
        "}\n");

  // Saving the same content again writes the same bytes and no tiles.
  const std::string before = text;
  SaveReport again;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &again));
  REQUIRE(vfs.read("docs://world/layers/base/index.json", text) == io::Status::Ok);
  CHECK(text == before);
  CHECK(again.tiles_written == 0);
  CHECK(again.files_written == 1);  // the manifest, and nothing else
}

TEST_CASE("doc partition: a round trip against the single-file form, record by record") {
  TempDir tmp("engine_doc_partition_round_trip");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path, /*writable=*/true) == io::Status::Ok);
  std::string error;

  // The same content saved both ways.
  auto build = [](Document& doc) {
    Layer& base = doc.layer(0);
    for (u32 i = 0; i < 40; ++i) {
      ObjectRecord r = placed(i, static_cast<f64>(i) * 17.0 - 200.0,
                              static_cast<f64>((i * 7) % 23) * 19.0 - 150.0);
      r.properties.insert_or_assign("name", JsonValue("place" + std::to_string(i)));
      base.set(std::move(r));
    }
    ObjectRecord global;
    global.id = id_of(100);
    global.type = k_placement;
    base.set(std::move(global));
    doc.rebuild_index();
    doc.mark_all_dirty();
  };

  Document plain;
  DocumentManifest plain_manifest;
  REQUIRE(DocumentStore::create(vfs, "docs://plain", "Plain", plain, plain_manifest, &error));
  build(plain);
  REQUIRE(DocumentStore::save(vfs, "docs://plain", plain, plain_manifest, &error));

  Document tiled;
  DocumentManifest tiled_manifest;
  REQUIRE(DocumentStore::create(vfs, "docs://tiled", "Tiled", tiled, tiled_manifest, &error));
  build(tiled);
  tiled.set_layer_partition(0, partition_of("position", 64));
  REQUIRE(DocumentStore::save(vfs, "docs://tiled", tiled, tiled_manifest, &error));
  CHECK(count_files(tmp.path + "/tiled/layers/base/tiles") == tile_count(tiled.layer(0)));

  Document loaded_plain, loaded_tiled;
  DocumentManifest m1, m2;
  REQUIRE(DocumentStore::load(vfs, "docs://plain", loaded_plain, m1, &error));
  REQUIRE(DocumentStore::load(vfs, "docs://tiled", loaded_tiled, m2, &error));
  CHECK(loaded_tiled.layer(0).partitioned());
  CHECK(loaded_tiled.layer(0).partition().tile_size == 64);
  CHECK_FALSE(loaded_plain.layer(0).partitioned());

  const Layer& a = loaded_plain.layer(0);
  const Layer& b = loaded_tiled.layer(0);
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() == 41);
  for (auto [id, record] : a.records()) {
    const ObjectRecord* other = b.find(id);
    REQUIRE(other != nullptr);
    CHECK(record == *other);
  }
  // The partition is storage form, not content, so the layers are equal all the same.
  CHECK(a == b);
  CHECK(loaded_tiled.objects() == loaded_plain.objects());
  CHECK(loaded_tiled.validate_index());
}

TEST_CASE("doc partition: a transaction rewrites the tile it touched and no other") {
  TempDir tmp("engine_doc_partition_dirty");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path, /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.set_layer_partition(0, partition_of("position", 64));
  {
    Layer& base = doc.layer(0);
    for (u32 i = 0; i < 12; ++i)
      base.set(placed(i, static_cast<f64>(i) * 64.0 + 8.0, 8.0));
    doc.rebuild_index();
    doc.mark_all_dirty();
  }
  SaveReport first;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &first));
  CHECK(first.tiles_total == 12);
  CHECK(first.tiles_written == 12);

  // One property of one object: one tile.
  {
    Transaction tx = doc.begin(who());
    CHECK(tx.apply(cmd_set(id_of(5), "name", JsonValue("moved-in"))));
    CHECK(tx.commit());
  }
  SaveReport second;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &second));
  CHECK(second.tiles_written == 1);
  CHECK(second.tiles_removed == 0);
  CHECK(second.files_written == 2);  // the tile and the manifest; the index did not change

  // Moving an object across the grid rewrites the tile it left and the one it joined.
  {
    Transaction tx = doc.begin(who());
    CHECK(tx.apply(cmd_set(id_of(5), "position", point(1000, 1000))));
    CHECK(tx.commit());
  }
  SaveReport third;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &third));
  CHECK(third.tiles_written == 1);  // the tile it joined
  CHECK(third.tiles_removed == 1);  // tile 5_0 held only that object
  CHECK(third.tiles_total == 12);
  CHECK_FALSE(vfs.exists("docs://world/layers/base/tiles/5_0.json"));
  CHECK(vfs.exists("docs://world/layers/base/tiles/15_15.json"));

  // Undo is per document and knows nothing about tiles; the files follow it back.
  REQUIRE(doc.undo(doc.journal().back()));
  SaveReport fourth;
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error, &fourth));
  CHECK(fourth.tiles_written == 1);
  CHECK(fourth.tiles_removed == 1);
  CHECK(vfs.exists("docs://world/layers/base/tiles/5_0.json"));
  CHECK_FALSE(vfs.exists("docs://world/layers/base/tiles/15_15.json"));

  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(loaded.layer(0) == doc.layer(0));
}

TEST_CASE("doc partition: repartition converts a layer both ways") {
  TempDir tmp("engine_doc_repartition");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path, /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  {
    Layer& base = doc.layer(0);
    for (u32 i = 0; i < 9; ++i)
      base.set(placed(i, static_cast<f64>(i) * 100.0, 0));
    doc.rebuild_index();
    doc.mark_all_dirty();
  }
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));
  CHECK(vfs.exists("docs://world/layers/base.json"));

  // One file to tiles: the single file goes, the tiles arrive, and the manifest says so.
  SaveReport to_tiles;
  REQUIRE(DocumentStore::repartition(vfs, dir, doc, manifest, 0, partition_of("position", 128),
                                     &error, &to_tiles));
  CHECK_FALSE(vfs.exists("docs://world/layers/base.json"));
  CHECK(vfs.exists("docs://world/layers/base/index.json"));
  CHECK(to_tiles.tiles_written == to_tiles.tiles_total);
  CHECK(to_tiles.tiles_total == 7);  // 0..800 over a 128 grid
  REQUIRE(manifest.layers[0].partition.has_value());

  Document loaded;
  DocumentManifest loaded_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, loaded, loaded_manifest, &error));
  CHECK(loaded.layer(0) == doc.layer(0));

  // And back: the tiles go, the single file returns.
  SaveReport to_file;
  REQUIRE(
      DocumentStore::repartition(vfs, dir, doc, manifest, 0, LayerPartition{}, &error, &to_file));
  CHECK(vfs.exists("docs://world/layers/base.json"));
  CHECK_FALSE(vfs.exists("docs://world/layers/base/index.json"));
  CHECK(count_files(tmp.path + "/world/layers/base/tiles") == 0);
  CHECK_FALSE(manifest.layers[0].partition.has_value());
  CHECK(to_file.tiles_removed == 7);

  Document back;
  DocumentManifest back_manifest;
  REQUIRE(DocumentStore::load(vfs, dir, back, back_manifest, &error));
  CHECK(back.layer(0) == doc.layer(0));
  CHECK_FALSE(back.layer(0).partitioned());
}

TEST_CASE("doc partition: a malformed index fails the load with a message") {
  TempDir tmp("engine_doc_partition_malformed");
  io::Vfs vfs;
  REQUIRE(vfs.mount("docs", tmp.path, /*writable=*/true) == io::Status::Ok);
  const std::string dir = "docs://world";
  std::string error;

  Document doc;
  DocumentManifest manifest;
  REQUIRE(DocumentStore::create(vfs, dir, "World", doc, manifest, &error));
  doc.set_layer_partition(0, partition_of("position", 64));
  {
    Layer& base = doc.layer(0);
    base.set(placed(0, 8, 8));    // tile 0_0
    base.set(placed(1, 200, 8));  // tile 3_0
    doc.rebuild_index();
    doc.mark_all_dirty();
  }
  REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &error));

  auto load_fails = [&](const char* what) {
    Document out;
    DocumentManifest out_manifest;
    error.clear();
    const bool ok = DocumentStore::load(vfs, dir, out, out_manifest, &error);
    INFO("after " << what << ": " << error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    return error;
  };
  auto restore = [&] {
    std::string ignored;
    doc.mark_all_dirty();
    REQUIRE(DocumentStore::save(vfs, dir, doc, manifest, &ignored));
    Document out;
    DocumentManifest out_manifest;
    REQUIRE(DocumentStore::load(vfs, dir, out, out_manifest, &ignored));
  };

  // A tile holding a record the index puts somewhere else.
  std::string text;
  REQUIRE(vfs.read("docs://world/layers/base/tiles/0_0.json", text) == io::Status::Ok);
  REQUIRE(vfs.write("docs://world/layers/base/tiles/3_0.json", text) == io::Status::Ok);
  {
    const std::string message = load_fails("a record the index lists elsewhere");
    CHECK(message.find("3_0.json") != std::string::npos);
    CHECK(message.find("is not the one the index lists there") != std::string::npos);
  }
  restore();

  // A record whose position no longer agrees with the tile it is filed under: the check that
  // makes the layout a fact about the records rather than a convention.
  std::string moved;
  REQUIRE(vfs.read("docs://world/layers/base/tiles/3_0.json", moved) == io::Status::Ok);
  const usize where = moved.find("200.0");
  REQUIRE(where != std::string::npos);
  moved.replace(where, 5, "8.0");
  REQUIRE(vfs.write("docs://world/layers/base/tiles/3_0.json", moved) == io::Status::Ok);
  {
    const std::string message = load_fails("a record moved out of its tile");
    CHECK(message.find("3_0.json") != std::string::npos);
    CHECK(message.find("does not fall in this tile") != std::string::npos);
  }
  restore();

  // A tile file nothing lists.
  REQUIRE(vfs.write("docs://world/layers/base/tiles/9_9.json", text) == io::Status::Ok);
  CHECK(load_fails("an unlisted tile file").find("the index does not list") != std::string::npos);
  REQUIRE(vfs.remove("docs://world/layers/base/tiles/9_9.json") == io::Status::Ok);

  // An index that is not JSON, and one that does not parse as a LayerIndex.
  REQUIRE(vfs.write("docs://world/layers/base/index.json", "{ nope") == io::Status::Ok);
  CHECK(load_fails("a torn index").find("index.json") != std::string::npos);
  REQUIRE(vfs.write("docs://world/layers/base/index.json",
                    R"({"name":"base","role":"Base","partition":{"property":"position",)"
                    R"("tile_size":0},"tiles":[],"untiled":[]})") == io::Status::Ok);
  CHECK(load_fails("a zero tile size").find("tile_size") != std::string::npos);
  REQUIRE(vfs.write("docs://world/layers/base/index.json",
                    R"({"name":"other","role":"Base","partition":{"property":"position",)"
                    R"("tile_size":64},"tiles":[],"untiled":[]})") == io::Status::Ok);
  CHECK(load_fails("an index for another layer").find("names layer 'other'") != std::string::npos);
  restore();

  // An index whose partition is not the manifest's.
  REQUIRE(vfs.read("docs://world/manifest.json", text) == io::Status::Ok);
  std::string patched = text;
  const usize at = patched.find("\"tile_size\": 64.0");
  REQUIRE(at != std::string::npos);
  patched.replace(at, std::string("\"tile_size\": 64.0").size(), "\"tile_size\": 32.0");
  REQUIRE(vfs.write("docs://world/manifest.json", patched) == io::Status::Ok);
  CHECK(load_fails("a manifest that disagrees").find("differs from the manifest") !=
        std::string::npos);
}

TEST_CASE("doc partition: a partitioned layer merges like any other") {
  // Tiles are a file layout. In memory a partitioned layer is an ordinary layer, so the diff
  // and the three-way merge see the records and nothing else.
  Layer base("shared", LayerRole::Feature);
  base.set_partition(partition_of("position", 64));
  base.set(placed(0, 10, 10));
  base.set(placed(1, 300, 10));

  Layer ours = base;
  ours.ensure(id_of(0)).properties.insert_or_assign("name", JsonValue("ours"));
  Layer theirs = base;
  theirs.ensure(id_of(1)).properties.insert_or_assign("name", JsonValue("theirs"));
  theirs.set(placed(2, 900, 900));

  MergeResult merged;
  std::string error;
  REQUIRE(merge_layers(base, ours, theirs, merged, &error));
  CHECK(merged.conflicts.empty());
  CHECK(merged.merged.size() == 3);
  CHECK(merged.merged.find(id_of(0))->properties.find_value("name")->as_string() == "ours");
  CHECK(merged.merged.find(id_of(1))->properties.find_value("name")->as_string() == "theirs");
  // The merged layer takes base's storage form along with its name and role, so merging a
  // partitioned layer gives a partitioned layer, and the object theirs added lands on the grid.
  CHECK(merged.merged.partitioned());

  const LayerIndex index = build_layer_index(merged.merged);
  CHECK(index.tiles.size() == 3);
  CHECK(index.untiled.empty());
  CHECK(index.min_x == 0);
  CHECK(index.max_x == 14);
}
