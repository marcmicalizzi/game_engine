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

namespace {

// A body as the GPU scene keeps one (W151's cases, no device): the beach's 60 km under its own
// fixed 8 m/s wind, its load's table drawn on that wind's grid, the table at offset 0 and no
// surface record.
WaterWindBody live_body() {
  WaterWindBody b;
  b.fetch_m = 60000.0;
  b.seed = 7;
  b.load_wind_m_s = 8.0;
  gfx::WindDesc own;
  own.kind = gfx::WindKind::fixed;
  own.speed_m_s = 8.0;
  const gfx::WaterWaveGrid grid = gfx::water_wind_range(own, b.fetch_m).grid;
  gfx::water_spectrum_on_grid(gfx::sea_state(own, 0.0, b.fetch_m), grid, b.seed, b.load_waves);
  b.entry_offset = 0;
  return b;
}

// The table a frame wrote for the body, or false when it wrote none.
bool written(const Vector<WaterWindUpload>& uploads, gfx::WaterWave (&waves)[gfx::k_water_waves]) {
  for (const WaterWindUpload& up : uploads) {
    if (up.surface) continue;
    std::memcpy(waves, up.data, sizeof(waves));
    return true;
  }
  return false;
}

}  // namespace

// **A live wind's step** (W151; gfx.md, "The live wind"), no device: the beach's body loaded at
// 8 m/s, then a key steps the mean to a gale (Beaufort 8, 18.9 m/s). The first live frame draws the
// sea the body drew, on the grid it drew it on — every band's wavenumber and cycles the load's, so
// no crest moves, and the sea's height the load's — and the sea then grows by the duration law over
// the world clock's seconds: higher after a minute than at the step, never past the gale's grown
// sea over the fetch, and within 2 % of it after nine hours. The bands never move on the way. A
// constant wind's growth does not depend on how the seconds were cut: a body stepped a second at a
// time and one stepped ten minutes at a time stand at the same height after an hour.
TEST_CASE("water wind: a live wind's step grows the sea by its law, on the grid it drew") {
  Vector<WaterWindUpload> uploads;
  WaterWindBody body[1] = {live_body()};
  water_wind_frame(body, 0.0, uploads);  // the first frame writes the load's table once
  water_wind_frame(body, 0.0, uploads);
  CHECK(uploads.empty());  // and then it stands

  gfx::WindLive live;
  live.set = true;
  live.mean_m_s = gfx::beaufort_speed(8);
  live.to_deg = 0.0;
  water_wind_frame(body, 0.0, uploads, live, 100.0, 0.0002);
  gfx::WaterWave waves[gfx::k_water_waves];
  REQUIRE(written(uploads, waves));
  for (u32 i = 0; i < gfx::k_water_waves; ++i) {
    CHECK(waves[i].k == body[0].load_waves[i].k);
    CHECK(waves[i].cycles == body[0].load_waves[i].cycles);
  }
  gfx::WindDesc calm;
  calm.kind = gfx::WindKind::fixed;
  calm.speed_m_s = 8.0;
  const f64 load_h = gfx::sea_state(calm, 0.0, 60000.0).height_m();
  CHECK(body[0].drawn_sea.height_m() == doctest::Approx(load_h).epsilon(1e-6));

  gfx::WindDesc gale = calm;
  gale.speed_m_s = live.mean_m_s;
  const f64 grown = gfx::sea_state(gale, 0.0, 60000.0).height_m();
  water_wind_frame(body, 0.0, uploads, live, 160.0, 0.0002);
  const f64 minute = body[0].drawn_sea.height_m();
  CHECK(minute > load_h);
  CHECK(minute < grown);
  f64 last = minute;
  for (f64 t = 160.0; t <= 9.0 * 3600.0; t += 600.0) {
    water_wind_frame(body, 0.0, uploads, live, t, 0.0002);
    if (written(uploads, waves)) {
      for (u32 i = 0; i < gfx::k_water_waves; ++i)
        CHECK(waves[i].k == body[0].load_waves[i].k);
    }
    CHECK(body[0].drawn_sea.height_m() >= last - 1e-12);
    CHECK(body[0].drawn_sea.height_m() <= grown * (1.0 + 1e-9));
    last = body[0].drawn_sea.height_m();
  }
  MESSAGE("the 8 m/s sea " << load_h << " m; after a minute of the gale " << minute
                           << " m; after nine hours " << last << " m; the gale's grown sea "
                           << grown << " m");
  CHECK(last == doctest::Approx(grown).epsilon(0.02));

  WaterWindBody fine[1] = {live_body()};
  WaterWindBody coarse[1] = {live_body()};
  water_wind_frame(fine, 0.0, uploads);
  water_wind_frame(coarse, 0.0, uploads);
  for (u32 s = 0; s <= 3600; ++s)
    water_wind_frame(fine, 0.0, uploads, live, static_cast<f64>(s), 0.0002);
  for (u32 s = 0; s <= 3600; s += 600)
    water_wind_frame(coarse, 0.0, uploads, live, static_cast<f64>(s), 0.0002);
  CHECK(fine[0].live_sea.sea_height_m ==
        doctest::Approx(coarse[0].live_sea.sea_height_m).epsilon(1e-9));
}

// **What the trees take from a live wind** (W151), no device: its mean under the scene's
// turbulence, so a gale's strength over the mean day is about the gale's over 5 m/s, and a held
// gust adds the 3 s gust factor's share of the mean on top (1.24 over the sea's roughness), on the
// same instant of the turbulence.
TEST_CASE("water wind: the trees take a live wind's mean and its forced gust") {
  gfx::WindDesc sea;
  sea.kind = gfx::WindKind::fixed;
  sea.speed_m_s = 3.0;
  sea.roughness_m = 0.0002;
  gfx::WindLive live;
  live.set = true;
  live.mean_m_s = 15.0;
  live.to_deg = 90.0;
  Vec2 to{};
  f32 calm = 0.0f;
  REQUIRE(scene_flora_wind(sea, 1000.0, 1000.0, to, calm, live));
  CHECK(to.y == doctest::Approx(1.0f).epsilon(1e-6));
  CHECK(static_cast<f64>(calm) == doctest::Approx(15.0 / k_flora_wind_mean_m_s).epsilon(0.35));
  live.gust = 1.0;
  f32 gusting = 0.0f;
  REQUIRE(scene_flora_wind(sea, 1000.0, 1000.0, to, gusting, live));
  const f64 g = gfx::wind_gust_factor(0.0002);
  CHECK(g == doctest::Approx(1.24).epsilon(0.01));
  CHECK(static_cast<f64>(gusting - calm) ==
        doctest::Approx(15.0 * (g - 1.0) / k_flora_wind_mean_m_s).epsilon(1e-5));
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
  u32 width = 320;  // the picture's size
  u32 height = 200;
  bool still = false;  // the body may stand still (no lift): a calm reference frame
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
      if (mesh >= out.data.mesh_info.size() || !out.data.mesh_info[mesh].water.has_value())
        continue;
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
  if (!as.still) REQUIRE(out.scene.water_lifted());
  SceneRenderer::Desc rd;
  rd.width = as.width;
  rd.height = as.height;
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
    MESSAGE("two instances of one body: loaded "
            << load_padding[0] << " and " << load_padding[1] << " m, under a hand gale "
            << gale_padding[0] << " and " << gale_padding[1] << " m, the second " << back_padding
            << " m cleared");
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
// raise marks the water's slot before that frame's tables are prepared; until 2026-11-17 such a
// mark reached no set, and the water kept its smaller cull spheres under the gale's higher crests.
// Each set in turn is read back and must hold the CPU's bound.
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
    REQUIRE_MESSAGE(shot.renderer.set_dynamic_instances(
                        std::span<const SceneInstance>(tail.data(), tail.size()), &error),
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
    MESSAGE("bound after the tables grew: loaded "
            << load_padding << " m, the gale's " << gale_padding << " m; " << stale << " of "
            << 2 * shot.scene.table_sets() << " frames' sets held another");
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
    load(
        device, tmp, "none",
        wind_scene(R"("wind_m_s": 2, "fetch_km": 20)", R"("wind": {"speed_m_s": 3, "to_deg": 30})"),
        none);
    load(
        device, tmp, "swell",
        wind_scene(R"("wind_m_s": 2, "fetch_km": 20)",
                   R"("wind": {"speed_m_s": 3, "to_deg": 30}, "swell": {"period_h": 24, "track": [)"
                   R"({"hour": 0, "height_m": 0.4, "period_s": 12, "to_deg": 45},)"
                   R"({"hour": 12, "height_m": 1.2, "period_s": 10, "to_deg": 45}]})"),
        swell);
    const SceneWater& water =
        *swell.data.mesh_info[swell.data.instances[swell.water_instance].mesh].water;
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

namespace {

// W157: the frame from a camera of the case's own, at a world time.
CapturedFrame draw_at(Shot& shot, f64 world_time_s, WorldPos eye, WorldPos at) {
  FrameDesc frame;
  frame.camera.position = eye;
  frame.camera.target = at;
  frame.sky_time_s = k_noon;
  frame.world_time_s = world_time_s;
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame out;
  std::string error;
  REQUIRE_MESSAGE(shot.renderer.capture(frame, channels, out, &error), error);
  return out;
}

int luma_at(const CapturedFrame& f, u32 p) {
  return int{f.color[p * 4]} + int{f.color[p * 4 + 1]} + int{f.color[p * 4 + 2]};
}

// The foam a frame draws, as a mask: pixels the sea's frame shows whiter than the calm frame's by
// more than `by` of 765, where either shows the scene (seen from above, the sea, its sheets and
// the sand under them all darken a pixel against the calm frame's dry sand or still water; only
// the foam lightens one).
struct FoamMask {
  u32 width = 0, height = 0;
  Vector<u8> on;
  bool at(i32 x, i32 y) const {
    return x >= 0 && y >= 0 && x < i32(width) && y < i32(height) && on[u32(y) * width + u32(x)];
  }
};

FoamMask foam_mask(const CapturedFrame& sea, const CapturedFrame& calm, int by) {
  FoamMask m;
  m.width = sea.width;
  m.height = sea.height;
  m.on.resize(m.width * m.height);
  for (u32 p = 0; p < m.width * m.height; ++p) {
    const bool seen = sea.ids[p * k_id_words] != k_no_id && calm.ids[p * k_id_words] != k_no_id;
    m.on[p] = seen && luma_at(sea, p) > luma_at(calm, p) + by ? 1 : 0;
  }
  return m;
}
// What a foam mask's pattern is: how many of its pixels it has, how many of them are inside a
// patch (the mask holds the pixels two away on all four sides: a blotch, not a thread), and its
// runs' mean length along the picture's rows and along its columns.
struct FoamShape {
  u64 pixels = 0, inside = 0, runs_x = 0, length_x = 0, runs_y = 0, length_y = 0;
  void add(const FoamMask& m) {
    for (i32 y = 0; y < i32(m.height); ++y) {
      for (i32 x = 0; x < i32(m.width); ++x) {
        if (!m.at(x, y)) continue;
        ++pixels;
        if (m.at(x - 2, y) && m.at(x + 2, y) && m.at(x, y - 2) && m.at(x, y + 2)) ++inside;
        if (!m.at(x - 1, y)) {
          ++runs_x;
          for (i32 k = x; m.at(k, y); ++k)
            ++length_x;
        }
        if (!m.at(x, y - 1)) {
          ++runs_y;
          for (i32 k = y; m.at(x, k); ++k)
            ++length_y;
        }
      }
    }
  }
  f64 inside_share() const { return pixels ? f64(inside) / f64(pixels) : 0.0; }
  f64 mean_x() const { return runs_x ? f64(length_x) / f64(runs_x) : 0.0; }
  f64 mean_y() const { return runs_y ? f64(length_y) / f64(runs_y) : 0.0; }
};

}  // namespace

// W157's beach: the beach test scene's bay in small (content/test-scenes/beach: the same basin
// under the same level, so a gentle shore of about 1:30 near x = -27), an open sea outline over it,
// a sea of the scene's wind (`wind`; none for the calm frame, whose body stands still).
std::string lace_beach(f32 wind) {
  char text[1024];
  std::snprintf(text, sizeof(text), R"({
  "format": "engine.scene.v1",
  "name": "lace-beach",
  "sky": {"latitude_deg": 31.1, "day_of_year": 100, "turbidity": 2.2, "ground_albedo": 0.38},
  "terrain": {"size": 257, "extent": 128, "seed": 9, "dune_height": 0,
              "basins": [{"center": [300, 0], "radius": 380, "depth": 14}]},
  "water": [{"name": "bay", "level_m": -1.0, "open": true, "wind_follow": false,
             "shape": {"polygon": [[-40, 0, -64], [-40, 0, 64], [64, 0, 64], [64, 0, -64]]},
             "seed": 11, "direction_deg": 205, "wind_m_s": %g, "fetch_km": 60, "wet_m": 0.3}]
})",
                static_cast<f64>(wind));
  return text;
}

TEST_CASE("water wind: the shore's foam is a lace drawn up the slope, and the surf's no slab") {
  // W157 (water.md, "The swash's lace" and "Where a crest breaks"): the beach under a calm and
  // under the scene's 8 m/s sea, seen from above the swash at 3 m (up the slope is the picture's
  // columns) and above the surf at 30 m, six frames half a second apart. The foam is what the sea
  // lightens against the calm. Measured on the RTX 5090 (2026-11-22; main's shaders, then
  // W157's): the swash's foam 88 % inside a patch (the mask holds the pixels two away all round)
  // with runs of 34.6 pixels across the slope and 33.3 up it — islands, the same either way — and
  // with W157 78 %, 19.1 and 22.5: threads, drawn out up the slope; the surf's 60 % inside a patch,
  // runs of 11.2 pixels along the crests, and with W157 41 % and 5.1 (the slabs a lace, a roller
  // band and stretches that thin out at their ends: the captures are the W157 log's).
  const test::TempDir tmp("water_wind_lace");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot calm;
    Shot sea;
    LoadAs big;
    big.width = 960;
    big.height = 600;
    LoadAs still = big;
    still.still = true;
    load(device, tmp, "calm", lace_beach(0.0f), calm, still);
    load(device, tmp, "sea", lace_beach(8.0f), sea, big);
    FoamShape sand, surf;
    for (u32 s = 0; s < 6; ++s) {
      const f64 t = 10.0 + 0.5 * static_cast<f64>(s);
      const WorldPos swash_eye{-28.0, 3.0, 0.0};
      const WorldPos swash_at{-27.4, -1.2, 0.01};
      const WorldPos surf_eye{-8.0, 30.0, 0.0};
      const WorldPos surf_at{-7.4, -1.0, 0.01};
      sand.add(foam_mask(draw_at(sea, t, swash_eye, swash_at),
                         draw_at(calm, t, swash_eye, swash_at), 45));
      surf.add(
          foam_mask(draw_at(sea, t, surf_eye, surf_at), draw_at(calm, t, surf_eye, surf_at), 45));
    }
    MESSAGE("the swash's foam: " << sand.pixels << " pixels, " << sand.inside_share()
                                 << " inside a patch, runs " << sand.mean_x() << " across, "
                                 << sand.mean_y() << " up the slope; the surf's: " << surf.pixels
                                 << " pixels, " << surf.inside_share() << " inside a patch, runs "
                                 << surf.mean_x() << " along the crests, " << surf.mean_y()
                                 << " across them");
    // There is foam to measure, on the sand and in the surf.
    CHECK(sand.pixels > 100000u);
    CHECK(surf.pixels > 100000u);
    // The swash's foam is threads, not islands, and drawn out up the slope.
    CHECK(sand.inside_share() < 0.83);
    CHECK(sand.mean_x() < 26.0);
    CHECK(sand.mean_y() > 1.08 * sand.mean_x());
    // The surf's foam is a lace and short stretches, not whole-white slabs along the crests.
    CHECK(surf.inside_share() < 0.5);
    CHECK(surf.mean_x() < 8.0);
  }
  device.destroy();
}
namespace {

// W157-2: the still waterline in a calm frame: for each column, the first row from the top (the
// sea, the views below looking seaward) whose pixel is not the calm body's under one that is; -1
// for a column with none.
Vector<i32> waterline_rows(const CapturedFrame& calm, u32 water) {
  Vector<i32> rows;
  rows.resize(calm.width);
  for (u32 x = 0; x < calm.width; ++x) {
    rows[x] = -1;
    for (u32 y = 1; y < calm.height; ++y) {
      const u32 above = calm.ids[((y - 1) * calm.width + x) * k_id_words];
      const u32 here = calm.ids[(y * calm.width + x) * k_id_words];
      if (above == water && here != water && here != k_no_id) {
        rows[x] = i32(y);
        break;
      }
    }
  }
  return rows;
}

// A band of rows a set distance from each column's waterline (`from` to `to`, rows, negative
// seaward): its pixels' mean luma, and its foam mask's share, runs along the rows (along the
// shore) and the holes between them.
struct Band {
  u64 pixels = 0, luma = 0, foam = 0, runs = 0, run_length = 0, holes = 0, hole_length = 0;
  u64 hole_squares = 0, luma_squares = 0, chroma = 0;
  void add(const CapturedFrame& f, const FoamMask* m, const Vector<i32>& rows, i32 from, i32 to) {
    for (u32 x = 0; x < f.width; ++x) {
      if (rows[x] < 0) continue;
      for (i32 y = rows[x] + from; y < rows[x] + to; ++y) {
        if (y < 0 || y >= i32(f.height)) continue;
        const u32 p = u32(y) * f.width + x;
        ++pixels;
        luma += u64(luma_at(f, p));
        luma_squares += u64(luma_at(f, p)) * u64(luma_at(f, p));
        const u8* c = &f.color[p * 4];
        chroma += u64(std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}));
        if (!m) continue;
        const i32 ix = i32(x);
        if (m->at(ix, y)) {
          ++foam;
          if (!m->at(ix - 1, y)) {
            ++runs;
            for (i32 k = ix; m->at(k, y); ++k)
              ++run_length;
          }
        } else if (m->at(ix - 1, y)) {  // a hole begins: its length to the next thread on the row
          i32 k = ix;
          while (k < i32(f.width) && !m->at(k, y))
            ++k;
          if (k < i32(f.width)) {
            const u64 n = u64(k - ix);
            ++holes;
            hole_length += n;
            hole_squares += n * n;
          }
        }
      }
    }
  }
  f64 mean_luma() const { return pixels ? f64(luma) / f64(pixels) : 0.0; }
  // How far its pixels are from grey: a milky plate is paler and greyer than the sand it lies on.
  f64 mean_chroma() const { return pixels ? f64(chroma) / f64(pixels) : 0.0; }
  f64 luma_spread() const {
    if (pixels < 2) return 0.0;
    const f64 m = mean_luma();
    return std::sqrt(std::max(f64(luma_squares) / f64(pixels) - m * m, 0.0));
  }
  f64 share() const { return pixels ? f64(foam) / f64(pixels) : 0.0; }
  f64 mean_run() const { return runs ? f64(run_length) / f64(runs) : 0.0; }
  f64 mean_hole() const { return holes ? f64(hole_length) / f64(holes) : 0.0; }
  // The holes' spread: their lengths' standard deviation over their mean.
  f64 hole_spread() const {
    if (holes < 2) return 0.0;
    const f64 m = mean_hole();
    return std::sqrt(std::max(f64(hole_squares) / f64(holes) - m * m, 0.0)) / m;
  }
};

// The foam a frame draws by its colour alone: near white (its least channel within a fifth of its
// most) and bright. Over the water the calm frame's sky can be brighter than the sea's foam.
FoamMask white_mask(const CapturedFrame& f) {
  FoamMask m;
  m.width = f.width;
  m.height = f.height;
  m.on.resize(m.width * m.height);
  for (u32 p = 0; p < m.width * m.height; ++p) {
    const u8* c = &f.color[p * 4];
    const int hi = std::max({int{c[0]}, int{c[1]}, int{c[2]}});
    const int lo = std::min({int{c[0]}, int{c[1]}, int{c[2]}});
    m.on[p] = f.ids[p * k_id_words] != k_no_id && hi > 170 && lo * 5 >= hi * 4 ? 1 : 0;
  }
  return m;
}

i32 median_row(const Vector<i32>& rows) {
  Vector<i32> v;
  for (i32 r : rows)
    if (r >= 0) v.push_back(r);
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

TEST_CASE("water wind: the swash's sheet is wet sand with a lace on it, and no seam at the line") {
  // W157-2 (water.md, "The swash's lace"): the lace beach under a calm and under its 8 m/s sea,
  // six frames half a second apart, from the flight's height (24 m) and from 7 m over the still
  // waterline, looking seaward. The still waterline is the calm frame's, column by column. From
  // 24 m: the sheet's band (3 to 40 rows landward of the line) against the calm frame's dry sand
  // there and the surf's band seaward, by luma and by chroma (a milky plate is paler and greyer
  // than the sand under it). From 7 m: the foam by its colour (near white and bright), its runs
  // along the rows and the holes between them, either side of the line and in 24 bands of 12 rows
  // across it. Measured on the RTX 5090 (2026-11-22, W157's shaders, then W157-2's, within the
  // hour): the sheet's chroma 49.1 and 55.4 against the dry sand's 63.1 (0.78 and 0.88 of it);
  // the holes' lengths' deviation over their mean, seaward of the line, 0.86 and 3.26 — one size
  // of hole, then holes of many; the largest step in the threads' length from one band to the
  // next, 1.23 and 1.40. (This beach's sand is pale, so its sheet never reads lighter than the dry
  // sand, and its sea's setup puts the old seam off the 7 m view: the forest's flight carries those
  // two, the W157-2 log's captures; the counts here keep them from coming back.)
  const test::TempDir tmp("water_wind_sheet");
  gfx::Device device;
  if (!open_device(device)) return;
  {
    Shot calm;
    Shot sea;
    LoadAs big;
    big.width = 960;
    big.height = 600;
    LoadAs still = big;
    still.still = true;
    load(device, tmp, "calm", lace_beach(0.0f), calm, still);
    load(device, tmp, "sea", lace_beach(8.0f), sea, big);
    Band sheet, surf, dry, near_sea, near_land;
    // The lace across the still waterline from 7 m, in bands of 12 rows from 144 seaward to 144
    // landward: a seam is a step in its threads' length from one band to the next.
    constexpr i32 k_seam_bins = 24, k_seam_bin = 12, k_seam_from = -144;
    Band seam[k_seam_bins];
    for (u32 s = 0; s < 6; ++s) {
      const f64 t = 10.0 + 0.5 * static_cast<f64>(s);
      const WorldPos air_eye{-60.0, 24.0, 0.0};
      const WorldPos air_at{-27.0, -1.0, 0.0};
      const CapturedFrame air_sea = draw_at(sea, t, air_eye, air_at);
      const CapturedFrame air_calm = draw_at(calm, t, air_eye, air_at);
      const Vector<i32> air_rows = waterline_rows(air_calm, calm.water_instance);
      sheet.add(air_sea, nullptr, air_rows, 3, 40);
      surf.add(air_sea, nullptr, air_rows, -40, -3);
      dry.add(air_calm, nullptr, air_rows, 3, 40);
      const WorldPos low_eye{-27.5, 6.0, 0.0};
      const WorldPos low_at{-25.5, -1.0, 0.01};
      const CapturedFrame low_sea = draw_at(sea, t, low_eye, low_at);
      const CapturedFrame low_calm = draw_at(calm, t, low_eye, low_at);
      const Vector<i32> low_rows = waterline_rows(low_calm, calm.water_instance);
      const FoamMask mask = white_mask(low_sea);
      if (s == 0)
        MESSAGE("the waterline's median row: " << median_row(air_rows) << " from 24 m, "
                                               << median_row(low_rows) << " from 7 m");
      near_sea.add(low_sea, &mask, low_rows, -40, -4);
      near_land.add(low_sea, &mask, low_rows, 4, 40);
      for (i32 b = 0; b < k_seam_bins; ++b)
        seam[b].add(low_sea, &mask, low_rows, k_seam_from + b * k_seam_bin,
                    k_seam_from + (b + 1) * k_seam_bin);
    }
    MESSAGE("from 24 m: the sheet's band "
            << sheet.mean_luma() << " +- " << sheet.luma_spread() << " (dry " << dry.mean_luma()
            << "), the surf's " << surf.mean_luma() << " +- " << surf.luma_spread() << "; "
            << sheet.pixels << " and " << surf.pixels << " pixels");
    MESSAGE("from 7 m, seaward of the line and landward: foam "
            << near_sea.share() << " and " << near_land.share() << ", runs " << near_sea.mean_run()
            << " and " << near_land.mean_run() << ", holes " << near_sea.mean_hole() << " and "
            << near_land.mean_hole() << " spread " << near_sea.hole_spread() << " and "
            << near_land.hole_spread() << "; " << near_sea.pixels << " and " << near_land.pixels
            << " pixels");
    f64 step = 0.0;
    for (i32 b = 1; b < k_seam_bins; ++b) {
      const f64 lo = std::min(seam[b - 1].mean_run(), seam[b].mean_run());
      const f64 hi = std::max(seam[b - 1].mean_run(), seam[b].mean_run());
      if (seam[b - 1].runs > 200 && seam[b].runs > 200 && lo > 0.0) step = std::max(step, hi / lo);
    }
    MESSAGE("the sheet's band's chroma "
            << sheet.mean_chroma() << " (dry " << dry.mean_chroma()
            << "); the largest step in the threads' length across the line " << step);
    CHECK(sheet.pixels > 10000u);
    CHECK(near_land.pixels > 10000u);
    // The sheet seen from the air is wet sand with foam on it: darker than the dry beach, and no
    // greyer than four fifths of its colour.
    CHECK(sheet.mean_luma() < dry.mean_luma());
    CHECK(sheet.mean_chroma() > 0.8 * dry.mean_chroma());
    // Its lace's holes are of many sizes (the band seaward of the line, where the surf's lace and
    // the sheet's meet; landward the sheet's foam is sparse lines whose holes the band's width
    // cuts).
    CHECK(near_sea.hole_spread() > 1.2);
    // No band of the lace across the still waterline steps to threads half again as long.
    CHECK(step < 1.5);
  }
  device.destroy();
}
#endif
