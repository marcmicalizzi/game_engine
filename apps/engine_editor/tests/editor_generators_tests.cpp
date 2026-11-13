// engine-editor carries the build's generators (roadmap T79; docs/subsystems/scene_gen.md, "Which
// generators a host carries"). Until T79 the editor's hand-written list lacked `caves` and `soil`,
// under a comment saying it matched engine-view's and engine-host's, so a document placing a
// `.cave.json` or a `.soil.json` mesh was refused by the editor alone with the registry's sentence
// ("... which this build does not carry"). This reads such a document the way the viewport does
// (`doc_scene::read_document_scene`, then `renderer::load_scene`), with no window and no device.
//
// Whether the build has the two is the build's list (ENGINE_HOST_GENERATORS, written by
// engine_host_generators() from every module tagged GENERATOR), not this target's link: a host
// that dropped one fails here rather than skipping.

#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>
#include <foundation/io/vfs.h>
#include <systems/doc_scene/doc_scene.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>
#include <string_view>

#ifndef ENGINE_HOST_GENERATORS
#error \
    "ENGINE_HOST_GENERATORS is defined by engine_host_generators() (cmake/EngineGenerators.cmake)"
#endif

using namespace engine;

namespace {

bool build_tags(std::string_view tag) {
  const std::string_view list = ENGINE_HOST_GENERATORS;
  usize start = 0;
  while (start <= list.size()) {
    const usize comma = list.find(',', start);
    const usize end = comma == std::string_view::npos ? list.size() : comma;
    if (list.substr(start, end - start) == tag) return true;
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return false;
}

// A short passage (caves.schema's ranges): a 2 m hole, a 3 m shaft, two spans in and down, coarse
// rings and no speleothems, so the build is a fraction of a second in a debug build.
constexpr const char* k_passage =
    R"({"format":"engine.caves.passage","entrance_radius_m":2,"shaft_m":3,)"
    R"("points":[[4,-5,0],[9,-8,1]],"segments":[{"width_m":2.5}],)"
    R"("ring_vertices":8,"ring_spacing_m":1})";
// A bare scarp (no roots, no stones) at the soil schema's smallest source budget, which cannot hold
// the default roots and stones.
constexpr const char* k_scarp = R"({"kind":"Scarp","triangle_budget":2000,"roots":0,"stones":0})";

void place(doc::Document& document, u64 serial, const char* mesh, f64 x) {
  JsonValue props = JsonValue::object();
  props["mesh"] = JsonValue(mesh);
  JsonValue translation = JsonValue::array();
  translation.push_back(JsonValue(x));
  translation.push_back(JsonValue(0.0));
  translation.push_back(JsonValue(0.0));
  props["translation"] = translation;
  REQUIRE(document.apply(doc::cmd_create(Id128{0x79, serial},
                                         std::string(doc_scene::k_mesh_placement), Id128{}, props),
                         nullptr, nullptr));
}

}  // namespace

TEST_CASE("generators: engine-editor reads a document placing a cave and a soil feature") {
  const bool caves = build_tags("mesh:cave");
  const bool soil = build_tags("mesh:soil");
  if (!caves && !soil) {
    MESSAGE("this configuration builds neither the caves nor the soil capability");
    return;
  }
  const test::TempDir tmp("editor_generators");
  doc::Document document;
  document.add_layer("base", doc::LayerRole::Base);
  if (caves) {
    REQUIRE(io::write_file(tmp.file("passage.cave.json"), k_passage) == io::Status::Ok);
    place(document, 1, "passage.cave.json", 0.0);
  }
  if (soil) {
    REQUIRE(io::write_file(tmp.file("scarp.soil.json"), k_scarp) == io::Status::Ok);
    place(document, 2, "scarp.soil.json", 20.0);
  }
  doc_scene::DocumentScene scene;
  std::string error;
  REQUIRE_MESSAGE(doc_scene::read_document_scene(document, tmp.path(), {}, scene, error), error);
  REQUIRE(scene.desc.meshes.size() == (caves ? 1u : 0u) + (soil ? 1u : 0u));
  renderer::SceneDesc desc = scene.desc;
  desc.ddc = tmp.file("ddc");
  renderer::SceneData data;
  // On main at 2d867dd2 this failed with "<dir>/passage.cave.json is a mesh made by the generator
  // "cave", which this build does not carry: ..." and the editor's list of what it has.
  REQUIRE_MESSAGE(renderer::load_scene(desc, data, error), error);
  CHECK(data.parts.size() == scene.desc.meshes.size());
  CHECK_FALSE(data.lod.mesh.clusters.empty());
}
