// Channels through the renderer's scene reader, and drawn (docs/subsystems/water.md, "Channels";
// roadmap S96): a scene's `water_channels` read as one mesh and one instance a channel after the
// bodies and before the terrain, carrying the channel bit and the optics; an empty list the scene
// it was; and on a GPU, a channel beside a still pool — the same world time twice the same bytes,
// and two world times apart only the channel's pixels change, the pool's and the ground's not at
// all.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/device.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/water/channel.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_channel_scene(const std::string& path, const std::string& channels) {
  std::ofstream out(path, std::ios::binary);
  out << R"({
  "format": "engine.scene.v1",
  "name": "channel-test",
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0.6, "dune_wavelength": 12},
  "water": [{"name": "pool", "level_m": 0.2,
    "shape": {"circle": {"center": [3, 0, -6], "radius_m": 4}}, "wind_follow": false,
    "wave_amplitude_m": 0}])"
      << (channels.empty() ? std::string() : ",\n  \"water_channels\": " + channels) << "\n}\n";
  return static_cast<bool>(out);
}

// A brook across the dunes along +x, a gentle reach and then a steep one (Froude above 1).
constexpr const char* k_brook = R"([{"name": "brook",
  "points": [[-12, 2.0, 4], [0, 1.8, 4], [12, 1.0, 4]],
  "width_m": [3, 3, 2.5], "depth_m": [0.5, 0.5, 0.35], "slope": [0.017, 0.017, 0.067],
  "seed": 3}])";

bool gpu_here() {
  gfx::Device probe;
  std::string why;
  if (!probe.create(gfx::DeviceOptions{}, &why)) {
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  const bool atomics = probe.features().buffer_int64_atomics;
  probe.destroy();
  if (!atomics) MESSAGE("no 64-bit buffer atomics here");
  return atomics;
}

struct ChannelShot {
  CapturedFrame frame;
  u32 pool = ~0u;
  u32 channel = ~0u;
};

void shoot_channel(const gfx::Device& device, const std::string& path, const std::string& ddc,
                   const f64* world_times, u32 shots, ChannelShot* out) {
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = ddc;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 320;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  u32 pool = ~0u;
  u32 channel = ~0u;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh >= data.mesh_info.size() || !data.mesh_info[mesh].water.has_value()) continue;
    (data.mesh_info[mesh].water->channel ? channel : pool) = i;
  }
  CaptureChannels channels;
  channels.ids = true;
  for (u32 s = 0; s < shots; ++s) {
    FrameDesc frame;
    frame.camera.position = WorldPos{-14.0, 8.0, -12.0};
    frame.camera.target = WorldPos{2.0, 0.5, 0.0};
    frame.sky_time_s = 0.0;
    frame.world_time_s = world_times[s];
    REQUIRE_MESSAGE(renderer.capture(frame, channels, out[s].frame, &error), error);
    out[s].pool = pool;
    out[s].channel = channel;
  }
}

}  // namespace

TEST_CASE(
    "water channel: a scene's channels are one mesh and one instance each, after the bodies") {
  const test::TempDir tmp("water_channel_scene");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_channel_scene(path, k_brook));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  // The pool, the brook, and the terrain last.
  REQUIRE(desc.meshes.size() == 3);
  REQUIRE(desc.mesh_info.size() == 3);
  CHECK(desc.mesh_info[0].name == "water/pool");
  CHECK(desc.mesh_info[1].name == "water/brook");
  CHECK(std::string_view(desc.mesh_info[1].origin) == "water");
  CHECK(std::string_view(desc.mesh_info[2].origin) == "terrain");
  REQUIRE(desc.mesh_info[1].water.has_value());
  const SceneWater& brook = *desc.mesh_info[1].water;
  CHECK(brook.channel);
  CHECK_FALSE(desc.mesh_info[0].water->channel);
  CHECK(brook.body.name == "brook");
  CHECK(brook.body.shore_m == 2.0f);  // a body's default, under a body's name
  CHECK(brook.body.transmission == 1.0f);
  CHECK_FALSE(brook.body.open);
  CHECK(brook.depth.size >= water::k_depth_map_min);  // the description's bed, beside the mesh
  // The load carries it; the GPU scene sets the channel bit on its material (the picture case).
  {
    SceneDesc loaded;
    REQUIRE_MESSAGE(read_scene_file(path, loaded, error), error);
    loaded.ddc = tmp.file("ddc");
    SceneData data;
    REQUIRE_MESSAGE(load_scene(loaded, data, error), error);
    REQUIRE(data.mesh_info.size() == 3);
    REQUIRE(data.mesh_info[1].water.has_value());
    CHECK(data.mesh_info[1].water->channel);
  }
  REQUIRE(desc.instances.size() == 3);
  CHECK(desc.instances[1].mesh == 1u);
  CHECK(desc.instances[1].origin == WorldPos{0.0, 2.0, 4.0});

  // An empty list is the scene without one, mesh for mesh and instance for instance.
  const std::string bare_path = tmp.file("bare.json");
  const std::string empty_path = tmp.file("empty.json");
  REQUIRE(write_channel_scene(bare_path, ""));
  REQUIRE(write_channel_scene(empty_path, "[]"));
  SceneDesc bare;
  SceneDesc empty;
  REQUIRE_MESSAGE(read_scene_file(bare_path, bare, error), error);
  REQUIRE_MESSAGE(read_scene_file(empty_path, empty, error), error);
  REQUIRE(bare.meshes.size() == 2);
  REQUIRE(empty.meshes.size() == bare.meshes.size());
  for (u32 m = 0; m < bare.meshes.size(); ++m)
    CHECK(empty.meshes[m] == bare.meshes[m]);
  CHECK(empty.instances.size() == bare.instances.size());

  // A channel the capability refuses refuses the file with its sentence.
  const std::string bad_path = tmp.file("bad.json");
  REQUIRE(write_channel_scene(bad_path, R"([{"name": "trickle", "points": [[0, 1, 0]],
    "width_m": [1], "depth_m": [0.2], "slope": [0.01]}])"));
  SceneDesc bad;
  CHECK_FALSE(read_scene_file(bad_path, bad, error));
  CHECK_MESSAGE(
      error.find("water channel \"trickle\": it has fewer than two points") != std::string::npos,
      error);
}

TEST_CASE("water channel: the channel moves with the world's clock, and nothing else does") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_channel_picture");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_channel_scene(path, k_brook));
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  const f64 times[3] = {10.0, 10.0, 11.5};
  ChannelShot shots[3];
  shoot_channel(device, path, tmp.file("ddc"), times, 3, shots);
  device.destroy();
  const CapturedFrame& a = shots[0].frame;
  REQUIRE(shots[0].channel != ~0u);
  REQUIRE(shots[0].pool != ~0u);
  REQUIRE(a.color.size() == shots[1].frame.color.size());
  REQUIRE(a.color.size() == shots[2].frame.color.size());
  // The same world time twice: the same bytes.
  CHECK(std::equal(a.color.begin(), a.color.end(), shots[1].frame.color.begin()));
  // 1.5 world seconds on: the channel's pixels move, and no other pixel does — not the still pool
  // beside it (amplitude 0, so its surface has no clock), not the ground.
  const u32 pixels = a.width * a.height;
  u32 channel = 0;
  u32 pool = 0;
  u32 moved = 0;
  u32 moved_elsewhere = 0;
  for (u32 p = 0; p < pixels; ++p) {
    const u32 id = a.ids[p * k_id_words];
    channel += id == shots[0].channel ? 1u : 0u;
    pool += id == shots[0].pool ? 1u : 0u;
    bool same = true;
    for (u32 c = 0; c < 4; ++c)
      same = same && a.color[p * 4 + c] == shots[2].frame.color[p * 4 + c];
    if (same) continue;
    if (id == shots[0].channel) {
      ++moved;
    } else {
      ++moved_elsewhere;
    }
  }
  MESSAGE(channel << " channel pixels, " << moved << " of them moved 1.5 s on; " << pool
                  << " pool pixels; " << moved_elsewhere << " other pixels moved");
  CHECK(channel > 500u);
  CHECK(pool > 500u);
  CHECK(moved > channel / 4);
  CHECK(moved_elsewhere == 0u);
  if (moved_elsewhere != 0u || moved <= channel / 4) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    (void)write_capture(tmp.path(), "before", a, files, why);
    (void)write_capture(tmp.path(), "after", shots[2].frame, files, why);
  }
}
