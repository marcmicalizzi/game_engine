// Counts a file states that size an allocation (T187, from T13's reading,
// docs/experiments/fuzzing-design.md): a client names a scene to `render.load` and a camera path to
// `render.benchmark`, and each count is now refused past a stated bound, with the bound in the
// message. Each file is built as text by a function the fuzz harness can lift as a seed. CPU only.

#include <systems/renderer/camera_path.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace hostile_scene {

// A scene of one scatter of `count` instances of a mesh, standing at its own height (no terrain).
std::string scatter_scene(u64 count) {
  return R"({"format":"engine.scene.v1","name":"hostile",)"
         R"("meshes":[{"name":"m","path":"m.clusters"}],)"
         R"("scatters":[{"mesh":0,"name":"many","center":[0,0,0],"radius_max":10,"count":)" +
         std::to_string(count) + R"(,"seed":1,"ground":false}]})";
}

// One more instance than a scene can ever draw: the visibility id holds 2^24 (instance, cluster)
// pairs (`k_max_pairs`), and every instance is at least one. Before the bound the read pushed
// each one (about 2.5 GB in a debug build) and the GPU scene refused the scene later.
std::string scatter_count_past_bound() { return scatter_scene(u64{k_max_pairs} + 1); }

// A path of two keys ten million seconds apart at 1,000 frames a second: 10^10 + 1 frames, which
// `frame_count` wrapped in `u32` and a flight reserved by.
std::string camera_path_frames_past_bound() {
  return R"({"format":"engine.camera-path.v1","fps":1000,"keys":[)"
         R"({"time":0,"position":[0,1,0],"target":[0,1,-1],"fov_deg":60},)"
         R"({"time":10000000,"position":[1,1,0],"target":[1,1,-1],"fov_deg":60}]})";
}

}  // namespace hostile_scene

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

}  // namespace

TEST_CASE("hostile scene: a scatter that would place more instances than a scene draws is refused") {
  const test::TempDir tmp("renderer_hostile_scatter");
  REQUIRE(tmp.ok());
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, hostile_scene::scatter_count_past_bound()));
  SceneDesc desc;
  std::string error;
  CHECK_FALSE(read_scene_file(path, desc, error));
  CHECK(error.find(std::to_string(k_max_pairs)) != std::string::npos);
  CHECK(desc.instances.size() < 2u);
}

TEST_CASE("hostile scene: a camera path whose frame count passes the bound is refused") {
  CameraPath path;
  std::string error;
  CHECK_FALSE(parse_camera_path(hostile_scene::camera_path_frames_past_bound(), nullptr, path,
                                error));
  CHECK(error.find(std::to_string(k_max_camera_path_frames)) != std::string::npos);

  // The bound itself is a path: 100 s at 1,000 frames a second is 100,001 frames, one past it;
  // 99.999 s is 100,000.
  const std::string at_bound =
      R"({"format":"engine.camera-path.v1","fps":1000,"keys":[)"
      R"({"time":0,"position":[0,1,0],"target":[0,1,-1],"fov_deg":60},)"
      R"({"time":99.999,"position":[1,1,0],"target":[1,1,-1],"fov_deg":60}]})";
  REQUIRE_MESSAGE(parse_camera_path(at_bound, nullptr, path, error), error);
  CHECK(path.frame_count() == k_max_camera_path_frames);
}
