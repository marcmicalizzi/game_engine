// The document's authored ground (docs/subsystems/tileset.md, "The record"; doc_scene.md): an
// `engine.terrain.Heightfield` record read into the scene's terrain — the `tiles` provider over its
// heightfield, its set built into the derived-data cache through the registry on the first read
// and a hit after — changed (its source, origin or scale a rebuild, its name nothing), deleted,
// doubled and broken. Linked with the tile-set capability, as a host is.
#include <core/json/json.h>
#include <domain/doc/document.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>
#include <systems/doc_scene/doc_scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;

namespace {

Id128 id_of(u64 n) { return Id128{0x20, n}; }

JsonValue json(const std::string& text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

void create(doc::Document& d, Id128 id, std::string_view type, const std::string& properties) {
  Vector<doc::Diagnostic> diags;
  REQUIRE(
      d.apply(doc::cmd_create(id, std::string(type), Id128{}, json(properties)), nullptr, &diags));
  REQUIRE(diags.empty());
}

void set(doc::Document& d, Id128 id, const char* name, const std::string& value) {
  Vector<doc::Diagnostic> diags;
  REQUIRE(d.apply(doc::cmd_set(id, name, json(value)), nullptr, &diags));
}

void remove(doc::Document& d, Id128 id) {
  Vector<doc::Diagnostic> diags;
  REQUIRE(d.apply(doc::cmd_delete(id), nullptr, &diags));
}

// A document directory with a repository marker, so its derived-data root is `<dir>/ddc` (the
// cache `geometry::find_ddc_root` finds), and two heightfields in it.
struct World {
  test::TempDir tmp{"engine_doc_scene_heightfield"};
  doc::Document d;
  World() {
    REQUIRE(io::write_file(tmp.file("AGENTS.md"), "a test's repository root\n") == io::Status::Ok);
    std::string error;
    REQUIRE_MESSAGE(tileset::write_heightfield(
                        tmp.file("terrain/ridge.json"),
                        tileset::synth_heightfield(tileset::SynthKind::ridge, 65, 1000, 3), error),
                    error);
    REQUIRE_MESSAGE(tileset::write_heightfield(
                        tmp.file("terrain/bowl.json"),
                        tileset::synth_heightfield(tileset::SynthKind::basin, 33, 1000), error),
                    error);
    d.add_layer("base", doc::LayerRole::Base);
  }
};

}  // namespace

TEST_CASE("doc_scene: a heightfield record is the scene's terrain, its set built on a miss") {
  World w;
  create(w.d, id_of(1), doc_scene::k_heightfield,
         R"({"name":"range","source":"terrain/ridge.json","origin":[-32,0,-16]})");
  create(w.d, id_of(2), doc_scene::k_mesh_placement,
         R"({"name":"hut","mesh":"props/hut.glb","translation":[1,0,1]})");
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  CHECK(scene.terrain_record == id_of(1));
  REQUIRE(scene.terrain.has_value());
  CHECK(scene.terrain->provider == "tiles");
  REQUIRE(scene.terrain->tiles.has_value());
  CHECK(scene.terrain->tiles->heightfield == w.tmp.file("terrain/ridge.json"));
  CHECK(scene.terrain->tiles->origin.x == -32.0);
  CHECK(scene.terrain->tiles->origin.z == -16.0);
  // The scene's grid covers the heightfield round the origin: 64 m from x -32 and z -16 reaches
  // 48 m, at its 1 m spacing.
  CHECK(scene.terrain->extent == 48.0f);
  CHECK(scene.terrain->size == 97);
  CHECK(scene.desc.terrain.enabled);
  CHECK(scene.desc.terrain.provider == "tiles");
  CHECK(scene.desc.terrain.extent == 48.0f);
  // The placement is still an instance; the ground is no record's instance.
  REQUIRE(scene.records.size() == 1);
  CHECK(scene.records[0] == id_of(2));

  // The set the read built is in the document's derived-data cache, and the content step's key
  // finds it there: a hit.
  tileset::BuildOptions options;
  options.offset_x_mm = -32000;
  options.offset_z_mm = -16000;
  tileset::BuildResult again;
  REQUIRE_MESSAGE(tileset::ensure_tileset(w.tmp.file("terrain/ridge.json"), options, "",
                                          w.tmp.file("ddc"), false, again, error),
                  error);
  CHECK(again.hit);
  // A second read builds nothing.
  doc_scene::DocumentScene second;
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), second, error), error);
  CHECK(second.terrain_record == id_of(1));
}

TEST_CASE("doc_scene: a heightfield record's changes, and the refusals") {
  World w;
  create(w.d, id_of(1), doc_scene::k_heightfield,
         R"({"name":"range","source":"terrain/ridge.json"})");
  create(w.d, id_of(2), doc_scene::k_mesh_placement,
         R"({"name":"hut","mesh":"props/hut.glb","translation":[1,0,1]})");
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  doc_scene::SceneChanges changes;

  // Its name is not drawn: nothing.
  set(w.d, id_of(1), "name", R"("the range")");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK_FALSE(changes.rebuild);
  CHECK(changes.moved.empty());
  // A placement still moves, beside it.
  set(w.d, id_of(2), "translation", "[2,0,1]");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK_FALSE(changes.rebuild);
  CHECK(changes.moved.size() == 1);
  // Its source, its origin and its vertical scale are the ground: a rebuild each.
  set(w.d, id_of(1), "source", R"("terrain/bowl.json")");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  CHECK(scene.terrain->tiles->heightfield == w.tmp.file("terrain/bowl.json"));
  set(w.d, id_of(1), "origin", "[16,0,0]");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  set(w.d, id_of(1), "vertical_scale", "2");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  // Materials are part of the native ground, carried to the viewport and rebuilt on edit.
  set(w.d, id_of(1), "materials", R"({"layers":[{"name":"soil","tint":[0.2,0.3,0.1]}]})");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  REQUIRE(scene.desc.terrain.materials.has_value());
  CHECK(scene.desc.terrain.materials->layers[0].name == "soil");
  // Deleted: a rebuild, and the scene has no ground.
  remove(w.d, id_of(1));
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  CHECK(scene.terrain_record.is_null());
  CHECK_FALSE(scene.terrain.has_value());
  CHECK_FALSE(scene.desc.terrain.enabled);
  // Created where there was none: a rebuild.
  create(w.d, id_of(3), doc_scene::k_heightfield,
         R"({"name":"bowl","source":"terrain/bowl.json"})");
  REQUIRE(doc_scene::changes_since(w.d, scene, changes, error));
  CHECK(changes.rebuild);
  REQUIRE_MESSAGE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error), error);
  CHECK(scene.terrain_record == id_of(3));

  // Two are refused, naming both.
  create(w.d, id_of(4), doc_scene::k_heightfield,
         R"({"name":"second","source":"terrain/ridge.json"})");
  CHECK_FALSE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error));
  CHECK(error.find("two engine.terrain.Heightfield records") != std::string::npos);
  CHECK(error.find("00000000000000200000000000000003") != std::string::npos);
  CHECK(error.find("00000000000000200000000000000004") != std::string::npos);
  remove(w.d, id_of(4));
  // A source that is not there, and an origin between the source's points, each a sentence.
  set(w.d, id_of(3), "source", R"("terrain/none.json")");
  CHECK_FALSE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error));
  CHECK(error.find("does not read as a heightfield header") != std::string::npos);
  set(w.d, id_of(3), "source", R"("terrain/bowl.json")");
  set(w.d, id_of(3), "origin", "[0.5,0,0]");
  CHECK_FALSE(doc_scene::read_document_scene(w.d, w.tmp.path(), scene, error));
  CHECK(error.find("whole number of its spacings") != std::string::npos);
}
