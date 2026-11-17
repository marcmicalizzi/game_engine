// The scene's wind on a GPU (docs/subsystems/gfx.md, "Testing the wind"; roadmap W136): the
// "water wind" pass and the lift's bound that follows it, through the renderer's reader, the GPU
// scene and the resolve's water modules, on the S97 basin made open. A fixed wind under the sky
// draws the bytes the body's own `wind_m_s` at that direction drew; a varying wind's picture at a
// sky time is the same bytes whatever frames came before it; and a hand gale on a body loaded at
// a light wind holds the instance's bound at the gale's sea, and gives it back when it is cleared.
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/wind.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_device.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

using namespace engine;
using namespace engine::renderer;

// **The shore's trains follow the sea** (gfx.md, "The shore's train follows the sea"; W136-2), no
// device: a wind over 60 km that falls to calm at midnight from the day before's gale, rises to a
// gale by 06:00 and holds. The two trains span its seas (the largest's peak and an octave above, on
// the group's lattice). At every quantum their heights hold the sea's energy, H1^2 + H2^2 = H^2.
// While the new sea is young its energy is the short train's (the weighted period within a tenth of
// the short train's) and by noon the long one's (past nine tenths of it). And nothing jumps: in no
// quantum does a train's height move by more than 1 % of the largest sea's, a centimetre and a
// half on a 2.5 m sea, which is under the height a 4K pixel spans on a crest a hundred metres off.
TEST_CASE("water wind: the shore's two trains take the sea's energy as it grows") {
  gfx::WindDesc w;
  w.kind = gfx::WindKind::track;
  w.period_h = 48.0;
  w.track.push_back(gfx::WindKey{0.0, 0.0, 30.0});
  w.track.push_back(gfx::WindKey{6.0, 18.0, 30.0});
  w.track.push_back(gfx::WindKey{20.0, 18.0, 30.0});
  w.track.push_back(gfx::WindKey{26.0, 2.0, 30.0});
  const f64 fetch = 60000.0;
  const gfx::WaterWindRange range = gfx::water_wind_range(w, fetch);
  const f64 t1 = gfx::swash_period(range.max_state.sea_period_s);
  const gfx::WaterCross cross = water_cross_of_span(range.max_height_m, t1, Vec2{1.0f, 0.0f});
  REQUIRE(cross.period_s > 0.0);
  CHECK(cross.period_s == doctest::Approx(0.5 * t1));
  CHECK(cross.train_m * cross.train_m + cross.cross_m * cross.cross_m ==
        doctest::Approx(range.max_height_m * range.max_height_m));
  f64 least_period = 1e9;
  f64 last_period = 0.0;
  f64 worst_step = 0.0;  // metres: the largest move of either train's height in one quantum
  f64 worst_at = 0.0;
  f64 worst_sea = 0.0;  // the sea's own move then
  std::string periods;
  for (f64 t = 3600.0; t <= 12.0 * 3600.0; t += k_water_wind_quantum_s) {
    const gfx::SeaState s = gfx::sea_state(w, t, fetch);
    f64 h1 = 0.0, h2 = 0.0, period = 0.0;
    REQUIRE(water_train_heights(s, t1, cross.period_s, h1, h2, period));
    CHECK(h1 * h1 + h2 * h2 == doctest::Approx(s.height_m() * s.height_m()));
    least_period = std::min(least_period, period);
    last_period = period;
    if (std::fmod(t, 3600.0) == 0.0) periods += " " + std::to_string(period);
    const gfx::SeaState n = gfx::sea_state(w, t + k_water_wind_quantum_s, fetch);
    f64 n1 = 0.0, n2 = 0.0, np = 0.0;
    REQUIRE(water_train_heights(n, t1, cross.period_s, n1, n2, np));
    const f64 step = std::max(std::abs(n1 - h1), std::abs(n2 - h2));
    if (step > worst_step) {
      worst_step = step;
      worst_at = t;
      worst_sea = std::abs(n.height_m() - s.height_m());
    }
  }
  MESSAGE("trains at " << t1 << " s and " << cross.period_s << " s; the weighted period on the hour"
                       << periods << "; the largest move in a quantum " << worst_step << " m at "
                       << worst_at << " s (the sea's own " << worst_sea << " m), the largest sea "
                       << range.max_height_m << " m");
  CHECK(least_period < 1.1 * cross.period_s);
  CHECK(last_period > 0.9 * t1);
  CHECK(worst_step < 0.01 * range.max_height_m);
}

#if ENGINE_WATER_TESTS_DUNES

namespace {

// The basin of shore_scene_tests.cpp, open, under a sky; `body` ends the water block's fields and
// `sky` is the sky's (both JSON fragments).
std::string wind_scene(const char* body, const char* sky) {
  char text[2048];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "wind-test",
  "terrain": {"size": 129, "extent": 64, "seed": 2, "dune_height": 0,
              "basins": [{"center": [0, 0], "radius": 14, "depth": 4}]},
  "sky": {%s},
  "water": [{"name": "basin", "level_m": -1.5, "open": true,
             "shape": {"fill": {"seed": [0, 0, 0], "max_extent_m": 30}},
             "wet_m": 0.3, %s}]
})",
                sky, body);
  return text;
}

// A device as the case's profile gives it (test_device.h), or none where the renderer cannot run:
// the visibility buffer's 64-bit atomics are all a frame of this scene needs.
bool open_device(gfx::Device& device) {
  std::string why;
  if (!device.create(renderer_test::device_options(), &why)) {
    if (!renderer_test::device_profile().empty())
      FAIL("the forced device profile yields no device: " << why);
    MESSAGE("renderer unavailable here: " << why);
    return false;
  }
  if (!device.features().buffer_int64_atomics) {
    MESSAGE("no 64-bit buffer atomics here");
    device.destroy();
    return false;
  }
  return true;
}

// One loaded scene and its renderer, held so a case can draw it at several times.
struct Shot {
  SceneData data;
  GpuScene scene;
  SceneRenderer renderer;
  u32 water_instance = ~0u;
};

// How a case changes the loaded scene before the GPU takes it: a second instance of the water mesh
// where the first stands (each gets a surface of its own), and a scene that takes a tail.
struct LoadAs {
  bool second_water = false;
  bool dynamic = false;
};

void load(const gfx::Device& device, const test::TempDir& tmp, const std::string& name,
          const std::string& text, Shot& out, LoadAs as = {}) {
  const std::string path = tmp.file(name + ".json");
  {
    std::ofstream file(path, std::ios::binary);
    file << text;
  }
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  REQUIRE_MESSAGE(load_scene(desc, out.data, error), error);
  if (as.second_water) {
    for (u32 i = 0; i < out.data.instances.size(); ++i) {
      const u32 mesh = out.data.instances[i].mesh;
      if (mesh >= out.data.mesh_info.size() || !out.data.mesh_info[mesh].water.has_value()) continue;
      gfx::InstanceDesc copy = out.data.instances[i];
      copy.first_pair = out.data.pair_count;
      out.data.pair_count += out.data.parts[mesh].cluster_count;
      out.data.instances.push_back(copy);
      if (!out.data.instance_joints.empty()) out.data.instance_joints.push_back(0);
      break;
    }
  }
  out.data.dynamic = out.data.dynamic || as.dynamic;
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &out.data, resolved);
  REQUIRE(check_availability(resolved, device.features()) == RenderAvailability::Ok);
  REQUIRE_MESSAGE(out.scene.create(device, out.data, resolved, &error), error);
  REQUIRE(out.scene.water_lifted());
  SceneRenderer::Desc rd;
  rd.width = 320;
  rd.height = 200;
  REQUIRE_MESSAGE(out.renderer.create(device, out.scene, resolved, rd, &error), error);
  for (u32 i = 0; i < out.data.instances.size(); ++i) {
    const u32 mesh = out.data.instances[i].mesh;
    if (mesh < out.data.mesh_info.size() && out.data.mesh_info[mesh].water.has_value())
      out.water_instance = i;
  }
  REQUIRE(out.water_instance != ~0u);
}

CapturedFrame draw(Shot& shot, f64 sky_time_s) {
  FrameDesc frame;
  // Low over the basin's south-west rim, looking across at the north-east bank.
  frame.camera.position = WorldPos{-6.0, 1.0, -6.0};
  frame.camera.target = WorldPos{8.0, -1.5, 8.0};
  frame.sky_time_s = sky_time_s;
  frame.world_time_s = 12.5;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame out;
  std::string error;
  REQUIRE_MESSAGE(shot.renderer.capture(frame, channels, out, &error), error);
  return out;
}

bool same(const CapturedFrame& a, const CapturedFrame& b) {
  return a.color.size() == b.color.size() && a.ids.size() == b.ids.size() &&
         std::memcmp(a.color.data(), b.color.data(), a.color.size()) == 0 &&
         std::memcmp(a.ids.data(), b.ids.data(), a.ids.size() * sizeof(u32)) == 0;
}

u32 water_pixels_changed(const CapturedFrame& a, const CapturedFrame& b, u32 water) {
  u32 changed = 0;
  const u32 pixels = a.width * a.height;
  for (u32 p = 0; p < pixels; ++p) {
    if (a.ids[p * k_id_words] != water && b.ids[p * k_id_words] != water) continue;
    if (std::memcmp(&a.color[p * 4], &b.color[p * 4], 3) != 0) ++changed;
  }
  return changed;
}

constexpr f64 k_noon = 12.0 * 3600.0;

// Every water instance of the scene, in order.
Vector<u32> water_instances(const SceneData& data) {
  Vector<u32> out;
  for (u32 i = 0; i < data.instances.size(); ++i) {
    const u32 mesh = data.instances[i].mesh;
    if (mesh < data.mesh_info.size() && data.mesh_info[mesh].water.has_value()) out.push_back(i);
  }
  return out;
}

// Instance `index`'s record in the table the last frame read, as the GPU holds it.
gfx::InstanceDesc gpu_record(gfx::Device& device, const GpuScene& scene, u32 index) {
  device.wait_idle();
  gfx::InstanceDesc out{};
  gfx::BufferResource host;
  std::string error;
  REQUIRE_MESSAGE(gfx::create_buffer(device, sizeof(gfx::InstanceDesc),
                                     gfx::BufferUsage::TransferDst, true, host, &error),
                  error);
  const gfx::BufferResource from = scene.instances;
  const bool ok = gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        const gfx::BufferCopy copy{u64{index} * sizeof(gfx::InstanceDesc), 0,
                                   sizeof(gfx::InstanceDesc)};
        cb.copy_buffer(from.buffer, host.buffer, std::span<const gfx::BufferCopy>(&copy, 1));
      },
      &error);
  if (ok) std::memcpy(&out, host.mapped, sizeof(out));
  gfx::destroy_buffer(device, host);
  REQUIRE_MESSAGE(ok, error);
  return out;
}

}  // namespace

TEST_CASE("water wind: a fixed wind under the sky draws the bytes the body's own wind drew") {
  const test::TempDir tmp("water_wind_fixed");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot own;
    Shot fixed;
    load(device, tmp, "own",
         wind_scene(R"("wind_follow": false, "wind_m_s": 8, "direction_deg": 30, "fetch_km": 2)",
                    ""),
         own);
    load(device, tmp, "fixed",
         wind_scene(R"("wind_m_s": 2, "fetch_km": 2)", R"("wind": {"speed_m_s": 8, "to_deg": 30})"),
         fixed);
    CHECK(fixed.data.wind.kind == gfx::WindKind::fixed);
    CHECK(fixed.scene.instance_record(fixed.water_instance)->bounds_padding ==
          own.scene.instance_record(own.water_instance)->bounds_padding);
    u32 frames = 0;
    for (const f64 t : {k_noon, k_noon + 3600.0, k_noon + 7.0}) {
      const CapturedFrame a = draw(own, t);
      const CapturedFrame b = draw(fixed, t);
      CHECK(same(a, b));
      ++frames;
    }
    MESSAGE(frames << " frames of a fixed sky wind against the body's own: the same bytes");
  }
  device.destroy();
}

TEST_CASE(
    "water wind: a varying wind's picture at a time is the same bytes however it was reached") {
  const test::TempDir tmp("water_wind_track");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    // Calm to a gale over six hours, held, and down again: over 60 km the sea is still growing at
    // 08:00 (it takes 5.4 h to grow at 18 m/s), so the picture then is its history's.
    Shot shot;
    load(device, tmp, "track",
         wind_scene(R"("wind_m_s": 2, "fetch_km": 60)",
                    R"("wind": {"track": [{"hour": 0, "speed_m_s": 0, "to_deg": 30},
                                          {"hour": 6, "speed_m_s": 18, "to_deg": 30},
                                          {"hour": 14, "speed_m_s": 18, "to_deg": 30},
                                          {"hour": 20, "speed_m_s": 3, "to_deg": 30}]})"),
         shot);
    CHECK(shot.data.wind.kind == gfx::WindKind::track);
    const f64 t = 8.0 * 3600.0;
    const f32 padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
    const CapturedFrame first = draw(shot, t);  // the scene's first frame
    const CapturedFrame earlier = draw(shot, t - 3.0 * 3600.0);
    (void)draw(shot, t + 2.0 * 3600.0);
    (void)draw(shot, t - 600.0);
    const CapturedFrame again = draw(shot, t);  // after an earlier, a later and a nearer time
    CHECK(same(first, again));
    // The redraw is not idle: the sea five hours in is not the sea two hours in.
    const u32 changed = water_pixels_changed(first, earlier, shot.water_instance);
    MESSAGE("08:00 drawn first and again after 05:00, 10:00 and 07:50: same bytes; "
            << changed << " water pixels differ between 08:00 and 05:00");
    CHECK(changed > 100u);
    // A varying wind's bound is its largest sea's from the load, and no frame moves it.
    CHECK(shot.scene.instance_record(shot.water_instance)->bounds_padding == padding);
  }
  device.destroy();
}

TEST_CASE("water wind: a hand gale on a body loaded light holds its bound at the gale's sea") {
  const test::TempDir tmp("water_wind_hand");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot shot;
    load(
        device, tmp, "light",
        wind_scene(R"("wind_m_s": 2, "fetch_km": 20)", R"("wind": {"speed_m_s": 3, "to_deg": 30})"),
        shot);
    const f32 load_padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
    const CapturedFrame calm = draw(shot, k_noon);
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=20"));
    const CapturedFrame gale = draw(shot, k_noon);
    const f32 gale_padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=-1"));
    const CapturedFrame back = draw(shot, k_noon);
    const f32 back_padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
    // The padding holds twice the train's height among its terms, and every other term (the
    // run-up, the table's amplitudes) grows with the sea, so the gale's is at least the load's
    // plus twice the two seas' difference in height.
    const f64 fetch = 20000.0;
    const f64 rise = 2.0 * (gfx::sea_height(20.0, fetch) - gfx::sea_height(3.0, fetch));
    MESSAGE("bounds padding: loaded at 3 m/s "
            << load_padding << " m, under a hand gale " << gale_padding << " m (at least "
            << static_cast<f64>(load_padding) + rise << "), cleared " << back_padding << " m");
    CHECK(static_cast<f64>(gale_padding) >= static_cast<f64>(load_padding) + rise - 1e-3);
    CHECK(back_padding == load_padding);
    CHECK(water_pixels_changed(calm, gale, shot.water_instance) > 100u);
    CHECK(same(calm, back));
  }
  device.destroy();
}

// **Every instance of a water mesh follows the wind** (review A3 of batch 67): each has a surface
// record and a cull padding of its own, so under a hand gale the second instance's bound rises with
// the first's. Until 2026-11-17 the redraw found the first instance alone, and the second kept the
// load's surface and bound under the redrawn wave table.
TEST_CASE("water wind: a hand gale raises the bound of every instance of a water mesh") {
  const test::TempDir tmp("water_wind_two");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot shot;
    LoadAs as;
    as.second_water = true;
    load(
        device, tmp, "two",
        wind_scene(R"("wind_m_s": 2, "fetch_km": 20)", R"("wind": {"speed_m_s": 3, "to_deg": 30})"),
        shot, as);
    const Vector<u32> water = water_instances(shot.data);
    REQUIRE(water.size() == 2);
    const f32 load_padding[2] = {shot.scene.instance_record(water[0])->bounds_padding,
                                 shot.scene.instance_record(water[1])->bounds_padding};
    (void)draw(shot, k_noon);
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=20"));
    (void)draw(shot, k_noon);
    const f32 gale_padding[2] = {shot.scene.instance_record(water[0])->bounds_padding,
                                 shot.scene.instance_record(water[1])->bounds_padding};
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=-1"));
    (void)draw(shot, k_noon);
    const f32 back_padding = shot.scene.instance_record(water[1])->bounds_padding;
    const f64 fetch = 20000.0;
    const f64 rise = 2.0 * (gfx::sea_height(20.0, fetch) - gfx::sea_height(3.0, fetch));
    MESSAGE("two instances of one body: loaded " << load_padding[0] << " and " << load_padding[1]
                                                 << " m, under a hand gale " << gale_padding[0]
                                                 << " and " << gale_padding[1] << " m, the second "
                                                 << back_padding << " m cleared");
    CHECK(load_padding[1] == load_padding[0]);
    CHECK(static_cast<f64>(gale_padding[1]) >= static_cast<f64>(load_padding[1]) + rise - 1e-3);
    CHECK(gale_padding[1] == gale_padding[0]);
    CHECK(back_padding == load_padding[1]);
  }
  device.destroy();
}

// **A bound the wind raises on the frame after the tables grow reaches the GPU's table** (review
// A2 of batch 67): in a scene that takes a tail, a tail that grows the per-pair buffers makes every
// table set again, and the next frame carries its set over from the old one (T167). The wind's
// raise marks the water's slot before that frame's tables are prepared; until 2026-11-17 such a mark
// reached no set, and the water kept its smaller cull spheres under the gale's higher crests. Each
// set in turn is read back and must hold the CPU's bound.
TEST_CASE("water wind: a hand gale on the frame after the tables grow reaches every table set") {
  const test::TempDir tmp("water_wind_grown");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot shot;
    LoadAs as;
    as.dynamic = true;
    load(
        device, tmp, "grown",
        wind_scene(R"("wind_m_s": 2, "fetch_km": 20)", R"("wind": {"speed_m_s": 3, "to_deg": 30})"),
        shot, as);
    REQUIRE(shot.scene.dynamic());
    for (u32 f = 0; f < 3; ++f)
      (void)draw(shot, k_noon);
    // A tail of the ground's mesh, far under the basin, enough of it to outgrow the pair stride.
    REQUIRE(shot.data.terrain_mesh < shot.data.parts.size());
    const u32 clusters = shot.data.parts[shot.data.terrain_mesh].cluster_count;
    REQUIRE(clusters > 0);
    const u32 stride = shot.scene.pair_stride();
    Vector<SceneInstance> tail;
    for (u32 i = 0; i < stride / clusters + 2; ++i) {
      SceneInstance ground;
      ground.mesh = shot.data.terrain_mesh;
      ground.transform.position = Vec3{static_cast<f32>(i) * 70.0f, -400.0f, 0.0f};
      tail.push_back(ground);
    }
    std::string error;
    REQUIRE_MESSAGE(
        shot.renderer.set_dynamic_instances(std::span<const SceneInstance>(tail.data(), tail.size()),
                                            &error),
        error);
    REQUIRE(shot.scene.pair_stride() != stride);
    const f32 load_padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=20"));
    u32 stale = 0;
    f32 gale_padding = 0.0f;
    for (u32 f = 0; f < 2 * shot.scene.table_sets(); ++f) {
      (void)draw(shot, k_noon);
      gale_padding = shot.scene.instance_record(shot.water_instance)->bounds_padding;
      const gfx::InstanceDesc held = gpu_record(device, shot.scene, shot.water_instance);
      stale += held.bounds_padding != gale_padding ? 1u : 0u;
    }
    REQUIRE(tunables::apply_overrides("renderer.wind.speed_m_s=-1"));
    MESSAGE("bound after the tables grew: loaded " << load_padding << " m, the gale's "
                                                   << gale_padding << " m; " << stale << " of "
                                                   << 2 * shot.scene.table_sets()
                                                   << " frames' sets held another");
    CHECK(gale_padding > load_padding);
    CHECK(stale == 0);
  }
  device.destroy();
}

TEST_CASE("water wind: a sky's swell comes ashore and is drawn, a function of the time") {
  // W136-3 (gfx.md, "A swell a scene names"): an open basin under a light air, with and without
  // the sky's swell; then the swell's track drawn at a time reached two ways; then a hand swell on
  // the swell-less body, and cleared.
  const test::TempDir tmp("water_wind_swell");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot none;
    Shot swell;
    load(device, tmp, "none",
         wind_scene(R"("wind_m_s": 2, "fetch_km": 20)", R"("wind": {"speed_m_s": 3, "to_deg": 30})"),
         none);
    load(device, tmp, "swell",
         wind_scene(R"("wind_m_s": 2, "fetch_km": 20)",
                    R"("wind": {"speed_m_s": 3, "to_deg": 30}, "swell": {"period_h": 24, "track": [)"
                    R"({"hour": 0, "height_m": 0.4, "period_s": 12, "to_deg": 45},)"
                    R"({"hour": 12, "height_m": 1.2, "period_s": 10, "to_deg": 45}]})"),
         swell);
    const SceneWater& water = *swell.data.mesh_info[swell.data.instances[swell.water_instance].mesh].water;
    // The body's first train is the swell at its largest, and the body draws the sky's swell.
    CHECK(water.scene_swell);
    CHECK(water.swell_train_m == doctest::Approx(1.2f).epsilon(0.02));
    CHECK(water.swell_train_period_s == doctest::Approx(10.0f).epsilon(0.05));
    const CapturedFrame plain = draw(none, k_noon);
    const CapturedFrame noon = draw(swell, k_noon);
    const u32 changed = water_pixels_changed(plain, noon, swell.water_instance);
    // A time reached two ways: noon first, then 03:00, then noon again.
    const CapturedFrame early = draw(swell, 3.0 * 3600.0);
    const CapturedFrame again = draw(swell, k_noon);
    MESSAGE("the swell changes " << changed << " water pixels; 03:00 against noon "
                                 << water_pixels_changed(early, noon, swell.water_instance));
    CHECK(changed > 100u);
    CHECK(water_pixels_changed(early, noon, swell.water_instance) > 100u);
    CHECK(same(noon, again));
    // A hand swell on the body that has none, and back.
    REQUIRE(tunables::apply_overrides("renderer.swell.height_m=1.5"));
    const CapturedFrame hand = draw(none, k_noon);
    REQUIRE(tunables::apply_overrides("renderer.swell.height_m=-1"));
    const CapturedFrame back = draw(none, k_noon);
    CHECK(water_pixels_changed(plain, hand, none.water_instance) > 100u);
    CHECK(same(plain, back));
  }
  device.destroy();
}

#endif
