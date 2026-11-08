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

#include <cmath>
#include <fstream>
#include <iterator>
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

// ---- S98: the plunge, and nothing repeating ---------------------------------------------------

namespace {

// A spout: a reach above the dunes falling 5 m over a metre of run into a pool whose level is its
// foot's, so the foot lands in the pool (water.md, "The plunge").
bool write_plunge_scene(const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  out << R"({
  "format": "engine.scene.v1",
  "name": "plunge-test",
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0.6, "dune_wavelength": 12},
  "water": [{"name": "pool", "level_m": 1.0,
    "shape": {"circle": {"center": [3, 0, -6], "radius_m": 5}}, "wind_follow": false,
    "wave_amplitude_m": 0}],
  "water_channels": [{"name": "spout",
    "points": [[-12, 6.4, -6], [0, 6.0, -6], [1, 1.0, -6]],
    "width_m": [2, 2, 2], "depth_m": [0.4, 0.4, 0.4], "slope": [0.033, 0.033, 0.033],
    "seed": 5}]
}
)";
  return static_cast<bool>(out);
}

f32 luminance(const CapturedFrame& f, u32 p) {
  return 0.2126f * static_cast<f32>(f.color[p * 4 + 0]) +
         0.7152f * static_cast<f32>(f.color[p * 4 + 1]) +
         0.0722f * static_cast<f32>(f.color[p * 4 + 2]);
}

// The plunge scene from beside the pool, with the pool's plunges as read or cleared.
void shoot_plunge(const gfx::Device& device, const std::string& path, const std::string& ddc,
                  bool plunges, ChannelShot& out) {
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = ddc;
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh >= data.mesh_info.size() || !data.mesh_info[mesh].water.has_value()) continue;
    SceneWater& water = *data.mesh_info[mesh].water;
    (water.channel ? out.channel : out.pool) = i;
    if (!plunges) water.plunges.clear();
  }
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
  CaptureChannels channels;
  channels.ids = true;
  FrameDesc frame;
  frame.camera.position = WorldPos{9.0, 7.0, 3.0};
  frame.camera.target = WorldPos{1.0, 1.0, -6.0};
  frame.world_time_s = 10.0;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
}

}  // namespace

TEST_CASE("water channel: a fall's foot in a body is the body's plunge, and the ribbon stops") {
  const test::TempDir tmp("water_channel_plunge_read");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_plunge_scene(path));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  REQUIRE(desc.mesh_info.size() == 3);
  REQUIRE(desc.mesh_info[0].water.has_value());
  const SceneWater& pool = *desc.mesh_info[0].water;
  REQUIRE(pool.plunges.size() == 1);
  // The foot (1, 1, -6) less the pool's place (3, 1, -6); the radius the 5 m fall over four, the
  // speed the free fall's from rest.
  CHECK(pool.plunges[0].at.x == doctest::Approx(-2.0f));
  CHECK(pool.plunges[0].at.y == doctest::Approx(0.0f));
  CHECK(pool.plunges[0].radius_m == doctest::Approx(1.25f));
  CHECK(pool.plunges[0].speed_m_s == doctest::Approx(std::sqrt(2.0 * 9.81 * 5.0)).epsilon(1e-5));
  CHECK(desc.mesh_info[1].water->plunges.empty());
  // A pool a metre and more below the foot takes none.
  const std::string low_path = tmp.file("low.json");
  {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const usize at = text.find("\"level_m\": 1.0");
    REQUIRE(at != std::string::npos);
    text.replace(at, 14, "\"level_m\": -0.1");
    std::ofstream out(low_path, std::ios::binary);
    out << text;
  }
  SceneDesc low;
  REQUIRE_MESSAGE(read_scene_file(low_path, low, error), error);
  CHECK(low.mesh_info[0].water->plunges.empty());
}

TEST_CASE("water channel: a fall's foot in the pool is whiter than the pool beside it") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_channel_plunge_picture");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_plunge_scene(path));
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  ChannelShot with;
  ChannelShot without;
  shoot_plunge(device, path, tmp.file("ddc"), true, with);
  shoot_plunge(device, path, tmp.file("ddc"), false, without);
  device.destroy();
  REQUIRE(with.pool != ~0u);
  const CapturedFrame& a = with.frame;
  const u32 pixels = a.width * a.height;
  // The pool's pixels the plunge changed are brighter, and many; no pixel of anything else moved.
  u32 pool = 0, changed = 0, brighter = 0, elsewhere = 0;
  f64 lift = 0.0, white = 0.0, beside = 0.0;
  for (u32 p = 0; p < pixels; ++p) {
    const u32 id = a.ids[p * k_id_words];
    bool same = true;
    for (u32 c = 0; c < 4; ++c)
      same = same && a.color[p * 4 + c] == without.frame.color[p * 4 + c];
    if (id == with.pool) ++pool;
    if (same) {
      if (id == with.pool) beside += static_cast<f64>(luminance(a, p));
      continue;
    }
    if (id != with.pool) {
      ++elsewhere;
      continue;
    }
    ++changed;
    const f32 d = luminance(a, p) - luminance(without.frame, p);
    lift += static_cast<f64>(d);
    white += static_cast<f64>(luminance(a, p));
    if (d > 0.0f) ++brighter;
  }
  MESSAGE(pool << " pool pixels, " << changed << " changed by the plunge (" << brighter
               << " brighter, mean lift " << (changed > 0 ? lift / changed : 0.0) << "); "
               << elsewhere << " elsewhere; the foot's luminance "
               << white / (changed > 0 ? changed : 1) << " against the pool's beside it "
               << beside / (pool > changed ? pool - changed : 1));
  CHECK(pool > 1000u);
  CHECK(changed > 200u);
  CHECK(brighter > changed * 9 / 10);
  // Whiter than the pool beside it: the foot's mean luminance over the rest of the pool's.
  CHECK(white / static_cast<f64>(changed > 0 ? changed : 1) >
        1.25 * beside / static_cast<f64>(pool > changed ? pool - changed : 1));
  CHECK(elsewhere == 0u);
  if (changed <= 200u || elsewhere != 0u) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    (void)write_capture(tmp.path(), "with", a, files, why);
    (void)write_capture(tmp.path(), "without", without.frame, files, why);
  }
}

TEST_CASE("water channel: nothing on the channel repeats a period later") {
  if (!gpu_here()) return;
  const test::TempDir tmp("water_channel_period");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_channel_scene(path, k_brook));
  gfx::Device device;
  std::string error;
  REQUIRE_MESSAGE(device.create(gfx::DeviceOptions{}, &error), error);
  // One flow-phase period apart (8 s): S96's ripples drew the same bytes; the flow map's copies
  // restart at a new place every cycle, so the channel's picture is a different one. What moves
  // is compared, not the picture, whose still part (the banks' shallows, the sky's gradient) is
  // the same at any time: the change over 0.37 s at 10 s against the change over 0.37 s at 18 s.
  const f64 period = static_cast<f64>(water::k_channel_phase_period_s);
  const f64 times[4] = {10.0, 10.37, 10.0 + period, 10.37 + period};
  ChannelShot shots[4];
  shoot_channel(device, path, tmp.file("ddc"), times, 4, shots);
  device.destroy();
  const CapturedFrame& a = shots[0].frame;
  const CapturedFrame& b = shots[2].frame;
  const auto change = [&](u32 first, u32 p) {
    return static_cast<f64>(luminance(shots[first + 1].frame, p)) -
           static_cast<f64>(luminance(shots[first].frame, p));
  };
  // The correlation of the channel's pixels' changes about their means, at the period's shift.
  f64 sa = 0.0, sb = 0.0, n = 0.0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    if (a.ids[p * k_id_words] != shots[0].channel) continue;
    sa += change(0, p);
    sb += change(2, p);
    n += 1.0;
  }
  REQUIRE(n > 500.0);
  const f64 ma = sa / n, mb = sb / n;
  f64 cab = 0.0, caa = 0.0, cbb = 0.0;
  for (u32 p = 0; p < pixels; ++p) {
    if (a.ids[p * k_id_words] != shots[0].channel) continue;
    const f64 x = change(0, p) - ma;
    const f64 y = change(2, p) - mb;
    cab += x * y;
    caa += x * x;
    cbb += y * y;
  }
  const f64 r = caa > 0.0 && cbb > 0.0 ? cab / std::sqrt(caa * cbb) : 1.0;
  MESSAGE(n << " channel pixels; their correlation a period apart " << r);
  CHECK(r < 0.5);
  if (!(r < 0.5)) {
    tmp.keep();
    CaptureFiles files;
    std::string why;
    (void)write_capture(tmp.path(), "t10", a, files, why);
    (void)write_capture(tmp.path(), "t18", b, files, why);
  }
}
