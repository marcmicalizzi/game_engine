// The bridge from a document to a renderer scene, with no GPU (docs/subsystems/doc_scene.md): which
// records become instances and in what order, how meshes are shared, how a placement is placed,
// and what each kind of change asks of the viewport.

#include <core/json/json.h>
#include <domain/doc/document.h>
#include <systems/doc_scene/doc_scene.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

namespace {

Id128 id_of(u64 n) { return Id128{0x10, n}; }

JsonValue json(const char* text) {
  JsonValue v;
  REQUIRE(parse_json(text, v).ok);
  return v;
}

void create(doc::Document& d, Id128 id, std::string_view type, const char* properties) {
  Vector<doc::Diagnostic> diags;
  REQUIRE(
      d.apply(doc::cmd_create(id, std::string(type), Id128{}, json(properties)), nullptr, &diags));
  REQUIRE(diags.empty());
}

void set(doc::Document& d, Id128 id, const char* name, const char* value) {
  Vector<doc::Diagnostic> diags;
  REQUIRE(d.apply(doc::cmd_set(id, name, json(value)), nullptr, &diags));
}

// Three placements of two meshes, out of id order, and a record of another type between them.
void fill(doc::Document& d) {
  d.add_layer("base", doc::LayerRole::Base);
  create(d, id_of(3), doc_scene::k_mesh_placement,
         R"({"name":"c","mesh":"props/a.glb","translation":[10,0,-4],"yaw_deg":90})");
  create(d, id_of(1), doc_scene::k_mesh_placement,
         R"({"name":"a","mesh":"props/a.glb","translation":[1,2,3],"scale":[2,2,2]})");
  create(d, id_of(2), "engine.world.Node", R"({"name":"yard","position":[0,0,0]})");
  create(d, id_of(4), doc_scene::k_mesh_placement,
         R"({"name":"b","mesh":"D:/meshes/b.clusters","translation":[0,0,420000.5]})");
}

}  // namespace

TEST_CASE("doc_scene: placements become instances in id order, meshes shared by path") {
  doc::Document d;
  fill(d);
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE_MESSAGE(doc_scene::read_document_scene(d, "docs/world", scene, error), error);
  REQUIRE(scene.records.size() == 3);
  CHECK(scene.records[0] == id_of(1));
  CHECK(scene.records[1] == id_of(3));
  CHECK(scene.records[2] == id_of(4));
  REQUIRE(scene.desc.meshes.size() == 2);
  CHECK(scene.desc.meshes[0] == "docs/world/props/a.glb");  // relative to the document
  CHECK(scene.desc.meshes[1] == "D:/meshes/b.clusters");    // absolute, as written
  REQUIRE(scene.desc.instances.size() == 3);
  CHECK(scene.desc.instances[0].mesh == 0);
  CHECK(scene.desc.instances[1].mesh == 0);
  CHECK(scene.desc.instances[2].mesh == 1);
  // The world position whole in `origin`, as the scene reader places an `Instance`.
  CHECK(scene.desc.instances[0].origin.x == 1.0);
  CHECK(scene.desc.instances[0].origin.y == 2.0);
  CHECK(scene.desc.instances[0].transform.scale.x == 2.0f);
  CHECK(scene.desc.instances[0].transform.position.x == 0.0f);
  CHECK(scene.desc.instances[2].origin.z == 420000.5);  // f64: no float on the way
  // 90 degrees of yaw: +x turns to -z.
  const Vec3 x = rotate(scene.desc.instances[1].transform.rotation, Vec3{1.0f, 0.0f, 0.0f});
  CHECK(x.z == doctest::Approx(-1.0f).epsilon(1e-5));
  CHECK(scene.instance_of(id_of(3)) == 1);
  CHECK(scene.instance_of(id_of(2)) == ~0u);  // the Node is not the viewport's
  CHECK(scene.revision == d.revision());
}

TEST_CASE("doc_scene: a translation is a move, a new mesh or a new record is a rebuild") {
  doc::Document d;
  fill(d);
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE(doc_scene::read_document_scene(d, "", scene, error));

  doc_scene::SceneChanges changes;
  REQUIRE(doc_scene::changes_since(d, scene, changes, error));
  CHECK(changes.moved.empty());
  CHECK_FALSE(changes.rebuild);

  set(d, id_of(3), "translation", "[11,0,-4]");
  set(d, id_of(2), "name", "\"the yard\"");  // another type: nothing for the viewport
  REQUIRE(doc_scene::changes_since(d, scene, changes, error));
  CHECK_FALSE(changes.rebuild);
  REQUIRE(changes.moved.size() == 1);
  CHECK(changes.moved[0] == 1);
  CHECK(changes.placements[0].origin.x == 11.0);
  CHECK(scene.desc.instances[1].origin.x == 11.0);  // the description follows
  CHECK(scene.revision == d.revision());

  set(d, id_of(1), "mesh", "\"props/other.glb\"");
  REQUIRE(doc_scene::changes_since(d, scene, changes, error));
  CHECK(changes.rebuild);

  REQUIRE(doc_scene::read_document_scene(d, "", scene, error));
  create(d, id_of(9), doc_scene::k_mesh_placement, R"({"mesh":"props/a.glb"})");
  REQUIRE(doc_scene::changes_since(d, scene, changes, error));
  CHECK(changes.rebuild);

  REQUIRE(doc_scene::read_document_scene(d, "", scene, error));
  Vector<doc::Diagnostic> diags;
  REQUIRE(d.apply(doc::cmd_delete(id_of(4)), nullptr, &diags));
  REQUIRE(doc_scene::changes_since(d, scene, changes, error));
  CHECK(changes.rebuild);
}

TEST_CASE("doc_scene: a document with no placements draws the stand-in ground and picks nothing") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  create(d, id_of(2), "engine.world.Node", R"({"name":"yard","position":[0,0,0]})");
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE(doc_scene::read_document_scene(d, "", scene, error));
  REQUIRE(scene.desc.meshes.size() == 1);
  CHECK(scene.desc.meshes[0].empty());
  REQUIRE(scene.records.size() == 1);
  CHECK(scene.records[0].is_null());
  CHECK(scene.instance_of(Id128{}) == ~0u);
}

TEST_CASE("doc_scene: a placement with no mesh is refused, naming the record") {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  create(d, id_of(5), doc_scene::k_mesh_placement, R"({"name":"nothing","translation":[0,0,0]})");
  doc_scene::DocumentScene scene;
  std::string error;
  CHECK_FALSE(doc_scene::read_document_scene(d, "", scene, error));
  CHECK(error.find("names no mesh") != std::string::npos);
}
