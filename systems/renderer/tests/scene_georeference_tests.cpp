// Where on the Earth a scene is, at the scene reader (W186; scene_gen.md, "Where on the Earth a
// scene is"): the file's block carried to the `SceneDesc` as it was given, a malformed one refusing
// the file with its path, and a file with none carrying none. No device.

#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr const char* k_ground = R"("terrain":{"size":17,"extent":16,"seed":1})";

}  // namespace

TEST_CASE("scene georeference: carried by the reader, refused when malformed, absent when absent") {
  const test::TempDir tmp("renderer_scene_georeference");
  const std::string with = tmp.file("with.json");
  REQUIRE(io::write_file(with, std::string(R"({"format":"engine.scene.v1","name":"geo",)") +
                                   k_ground +
                                   R"(,"georeference":{"horizontal_crs":"EPSG:2952",
            "vertical_crs":"EPSG:6647","origin_easting":314400,"origin_northing":4833900,
            "origin_height":76,"east":"PlusX","north":"MinusZ"}})") == io::Status::Ok);
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(with, desc, error), error);
  REQUIRE(desc.georeference.has_value());
  CHECK(desc.georeference->horizontal_crs == "EPSG:2952");
  CHECK(desc.georeference->origin_northing == 4833900.0);
  CHECK(desc.georeference->north == scene::EngineAxis::MinusZ);

  // East and north on one axis: the file is refused, naming itself and the block.
  const std::string bad = tmp.file("bad.json");
  REQUIRE(io::write_file(bad, std::string(R"({"format":"engine.scene.v1","name":"geo",)") +
                                  k_ground +
                                  R"(,"georeference":{"horizontal_crs":"EPSG:2952",
            "vertical_crs":"EPSG:6647","east":"PlusX","north":"MinusX"}})") == io::Status::Ok);
  SceneDesc refused;
  error.clear();
  CHECK_FALSE(read_scene_file(bad, refused, error));
  CHECK_MESSAGE(error.find(bad) == 0, error);
  CHECK_MESSAGE(error.find("georeference") != std::string::npos, error);

  // No block: none carried.
  const std::string plain = tmp.file("plain.json");
  const std::string plain_text =
      std::string(R"({"format":"engine.scene.v1","name":"plain",)") + k_ground + "}";
  REQUIRE(io::write_file(plain, plain_text) == io::Status::Ok);
  SceneDesc none;
  REQUIRE_MESSAGE(read_scene_file(plain, none, error), error);
  CHECK_FALSE(none.georeference.has_value());
}
