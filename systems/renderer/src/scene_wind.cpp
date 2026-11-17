// The scene's wind as the renderer reads and draws it (scene_wind.h; docs/subsystems/gfx.md, "The
// wind a scene names"; roadmap W136).
#include <foundation/tunables/tunables.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene_wind.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::renderer {

namespace {

tunables::Float wind_speed{
    "renderer.wind.speed_m_s", -1.0, -1.0, 60.0,
    "A hand wind 10 m up, m/s, for every water body that draws a wave table and every tree, its "
    "sea at once (0 glass, 5 a gentle breeze, 10 a fresh one, 20 a gale); below 0 the scene's"};
tunables::Float wind_to{
    "renderer.wind.to_deg", -1.0, -1.0, 360.0,
    "Where the hand wind blows to: degrees in the ground plane from +x towards +z; below 0 each "
    "body's own direction"};

std::string field(const std::string& path, const char* what) { return path + ": sky.wind" + what; }

}  // namespace

bool read_scene_wind(const scene::Wind& wind, const std::string& path, gfx::WindDesc& out,
                     std::string& error) {
  out = gfx::WindDesc{};
  const auto speed_ok = [](f32 v) {
    return v >= 0.0f && v <= static_cast<f32>(gfx::k_wind_max_m_s);
  };
  if (!speed_ok(wind.speed_m_s)) {
    error = field(path, ".speed_m_s") + " is outside [0, 60] m/s";
    return false;
  }
  if (!(wind.roughness_m > 0.0f && wind.roughness_m <= 2.0f)) {
    error = field(path, ".roughness_m") + " is outside (0, 2] m";
    return false;
  }
  if (!wind.track.empty() && wind.climate.has_value()) {
    error = field(path, "") + " has a track and a climate: it may have one";
    return false;
  }
  out.roughness_m = static_cast<f64>(wind.roughness_m);
  out.speed_m_s = static_cast<f64>(wind.speed_m_s);
  out.to_deg = static_cast<f64>(wind.to_deg);
  if (!wind.track.empty()) {
    if (!(wind.period_h > 0.0f && wind.period_h <= 8760.0f)) {
      error = field(path, ".period_h") + " is outside (0, 8760] h";
      return false;
    }
    f32 last = -1.0f;
    for (u32 i = 0; i < wind.track.size(); ++i) {
      const scene::WindKey& key = wind.track[i];
      if (!(key.hour >= 0.0f && key.hour < wind.period_h) || !(key.hour > last)) {
        error = field(path, ".track[") + std::to_string(i) +
                "].hour is out of the period or out of order";
        return false;
      }
      if (!speed_ok(key.speed_m_s)) {
        error = field(path, ".track[") + std::to_string(i) + "].speed_m_s is outside [0, 60] m/s";
        return false;
      }
      last = key.hour;
      out.track.push_back(gfx::WindKey{static_cast<f64>(key.hour), static_cast<f64>(key.speed_m_s),
                                       static_cast<f64>(key.to_deg)});
    }
    out.period_h = static_cast<f64>(wind.period_h);
    out.kind = gfx::WindKind::track;
    return true;
  }
  if (wind.climate.has_value()) {
    const scene::WindClimate& c = *wind.climate;
    if (!(c.mean_m_s > 0.0f && c.mean_m_s <= 30.0f) ||
        !(c.weibull_k >= 1.0f && c.weibull_k <= 4.0f) ||
        !(c.veer_deg >= 0.0f && c.veer_deg <= 180.0f) ||
        !(c.synoptic_h >= 6.0f && c.synoptic_h <= 480.0f) ||
        !(c.diurnal >= 0.0f && c.diurnal <= 1.0f) ||
        !(c.peak_hour >= 0.0f && c.peak_hour < 24.0f)) {
      error = field(path, ".climate") +
              " has a number out of its range (mean (0, 30] m/s, weibull_k [1, 4], veer_deg "
              "[0, 180], synoptic_h [6, 480], diurnal [0, 1], peak_hour [0, 24))";
      return false;
    }
    out.climate = gfx::WindClimate{static_cast<f64>(c.mean_m_s),   static_cast<f64>(c.weibull_k),
                                   static_cast<f64>(c.to_deg),     static_cast<f64>(c.veer_deg),
                                   static_cast<f64>(c.synoptic_h), static_cast<f64>(c.diurnal),
                                   static_cast<f64>(c.peak_hour),  c.seed};
    out.kind = gfx::WindKind::climate;
    return true;
  }
  out.kind = gfx::WindKind::fixed;
  return true;
}

WindHand wind_hand_tunable() noexcept {
  WindHand hand;
  const f64 speed = wind_speed.get();
  if (speed >= 0.0) {
    hand.set = true;
    hand.speed_m_s = speed;
  }
  const f64 to = wind_to.get();
  if (to >= 0.0) {
    hand.turned = true;
    hand.to_deg = to;
  }
  return hand;
}

void water_wind_frame(std::span<WaterWindBody> bodies, const gfx::WindDesc& wind, f64 sky_time_s,
                      Vector<WaterWindUpload>& uploads) {
  uploads.clear();
  if (bodies.empty()) return;
  const WindHand hand = wind_hand_tunable();
  const f64 quantum = std::floor(sky_time_s / k_water_wind_quantum_s) * k_water_wind_quantum_s;
  for (WaterWindBody& b : bodies) {
    // What the body draws now, and the key that says so.
    u32 kind = 0;
    f64 key_a = 0.0;
    f64 key_b = 0.0;
    gfx::WindDesc held;
    if (hand.set) {
      kind = 1;
      key_a = hand.speed_m_s;
      key_b = hand.turned ? hand.to_deg : -1.0;
      held.kind = gfx::WindKind::fixed;
      held.speed_m_s = hand.speed_m_s;
      held.to_deg = hand.turned ? hand.to_deg
                                : std::atan2(static_cast<f64>(b.direction.y),
                                             static_cast<f64>(b.direction.x)) *
                                      (180.0 / gfx::k_wind_pi);
    } else if (b.scene_wind && wind.varies()) {
      kind = 2;
      key_a = quantum;
    }
    if (kind == b.key_kind && key_a == b.key_a && key_b == b.key_b) continue;
    b.key_kind = kind;
    b.key_a = key_a;
    b.key_b = key_b;
    gfx::WaterWave waves[gfx::k_water_waves];
    gfx::WaterSurface surface = b.load_surface;
    if (kind == 0) {
      std::memcpy(waves, b.load_waves, sizeof(waves));
    } else {
      const gfx::SeaState state = kind == 1 ? gfx::sea_state(held, quantum, b.fetch_m)
                                            : gfx::sea_state(wind, quantum, b.fetch_m);
      gfx::WaterWaveGrid grid = b.grid;
      if (kind == 1) {
        const gfx::WaterWindRange range = gfx::water_wind_range(held, b.fetch_m);
        if (range.grid.valid()) grid = range.grid;
      }
      if (grid.valid()) {
        gfx::water_spectrum_on_grid(state, grid, b.seed, waves);
      } else {
        // No grid to draw on (a calm hand wind on a body whose table is its load's): the load's
        // bands, flat.
        std::memcpy(waves, b.load_waves, sizeof(waves));
        for (gfx::WaterWave& w : waves) {
          w.amplitude = 0.0f;
          w.steepness = 0.0f;
        }
      }
      // The shore's train: the sea's height now, on the load's period (the arrival field is built
      // for it), unless the body's own swell is the train.
      if (!b.swell && b.surface_offset != ~u64{0}) {
        const f64 h = state.height_m();
        surface.height_m = static_cast<f32>(h);
        surface.run_up_m = static_cast<f32>(
            std::min(gfx::stockdon_r2(h, b.period_s, b.slope), gfx::k_swash_steep_cap * h));
        surface.reach_scale = static_cast<f32>(gfx::swash_reach_scale(h, b.period_s));
        surface.reach_cap = static_cast<f32>(gfx::swash_reach_cap(h, b.bank_max));
      }
    }
    for (u32 i = 0; i < gfx::k_water_waves; ++i)
      surface.waves[i] = waves[i];
    if (b.surface_offset != ~u64{0})
      b.padding = water_lift_padding(surface, waves, static_cast<f32>(b.bank_max));
    if (b.entry_offset != ~u64{0}) {
      WaterWindUpload up;
      up.surface = false;
      up.offset = b.entry_offset;
      up.bytes = static_cast<u32>(sizeof(waves));
      std::memcpy(up.data, waves, sizeof(waves));
      uploads.push_back(up);
    }
    if (b.surface_offset != ~u64{0}) {
      WaterWindUpload up;
      up.surface = true;
      up.offset = b.surface_offset;
      up.bytes = static_cast<u32>(sizeof(gfx::WaterSurface));
      std::memcpy(up.data, &surface, sizeof(surface));
      uploads.push_back(up);
    }
  }
}

bool scene_flora_wind(const gfx::WindDesc& wind, f64 sky_time_s, Vec2& to, f32& strength) noexcept {
  const WindHand hand = wind_hand_tunable();
  gfx::WindDesc used = wind;
  if (hand.set) {
    used.kind = gfx::WindKind::fixed;
    used.speed_m_s = hand.speed_m_s;
    if (hand.turned) used.to_deg = hand.to_deg;
  }
  if (used.kind == gfx::WindKind::none) return false;
  const gfx::WindSample mean = gfx::wind_at(used, sky_time_s);
  to = mean.to;
  strength = static_cast<f32>(gfx::wind_gust(used, mean, Vec2{0.0f, 0.0f}, sky_time_s) /
                              k_flora_wind_mean_m_s);
  return true;
}

}  // namespace engine::renderer

// ---- the GPU scene's half (gpu_scene.h, `water_wind_frame`) -------------------------------------

namespace engine::renderer {

std::span<const WaterWindUpload> GpuScene::water_wind_frame(f64 sky_time_s) {
  water_wind_uploads_.clear();
  if (!water_wind_ready_) {
    // Once, after the load: every body that draws a wave table, where its table and its surface
    // are, and what the load wrote into them.
    water_wind_ready_ = true;
    water_wind_.clear();
    for (const WaterWaveEntry& e : water_wave_entries_) {
      if (data_ == nullptr || e.mesh >= data_->mesh_info.size() ||
          !data_->mesh_info[e.mesh].water.has_value())
        continue;
      const SceneWater& water = *data_->mesh_info[e.mesh].water;
      if (water.waves.size() != gfx::k_water_waves || water.channel) continue;
      WaterWindBody b;
      b.mesh = e.mesh;
      b.entry_offset = u64{e.entry} * sizeof(gfx::ResolveMaterial);
      b.scene_wind = water.scene_wind && water.grid.valid();
      b.grid = water.grid;
      b.fetch_m = static_cast<f64>(water.fetch_m);
      b.seed = water.shading.seed;
      b.direction = water.shading.direction;
      b.swell =
          water.body.open && water.body.swell_height_m > 0.0f && water.body.swell_period_s > 0.0f;
      b.bank_max = static_cast<f64>(water.depth.bank_max);
      for (u32 i = 0; i < gfx::k_water_waves; ++i)
        b.load_waves[i] = water.waves[i];
      for (u32 i = 0; i < water_surface_of_.size() && i < data_->instances.size(); ++i) {
        const u32 at = water_surface_of_[i];
        if (data_->instances[i].mesh != e.mesh || at == ~0u || at >= water_surface_load_.size())
          continue;
        b.surface_offset = u64{at} * sizeof(gfx::WaterSurface);
        b.load_surface = water_surface_load_[at];
        if (i < instance_table_.size() && (!dynamic_ || i < static_instances_)) {
          b.instance = i;
          b.load_padding = instance_table_[i].bounds_padding;
          b.padding = b.load_padding;
        }
        b.slope = static_cast<f64>(b.load_surface.slope);
        b.period_s = static_cast<f64>(b.load_surface.period_s);
        break;
      }
      water_wind_.push_back(b);
    }
  }
  if (water_wind_.empty() || data_ == nullptr) return {};
  renderer::water_wind_frame(std::span<WaterWindBody>(water_wind_.data(), water_wind_.size()),
                             data_->wind, sky_time_s, water_wind_uploads_);
  // The lift's bound: never under the load's (the largest sea a varying wind raises), raised for a
  // higher sea than it — a hand gale on a body loaded calm — and back when that sea goes. The
  // record rides this frame's record upload, which runs after this (scene_renderer.cpp), so the
  // frame that draws the higher crests culls by the larger spheres.
  for (const WaterWindBody& b : water_wind_) {
    if (b.instance == ~0u || b.instance >= instance_table_.size()) continue;
    const f32 want = std::max(b.load_padding, b.padding);
    if (instance_table_[b.instance].bounds_padding == want) continue;
    instance_table_[b.instance].bounds_padding = want;
    mark_instance_written(b.instance);
  }
  return {water_wind_uploads_.data(), water_wind_uploads_.size()};
}

}  // namespace engine::renderer
