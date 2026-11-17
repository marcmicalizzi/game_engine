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

tunables::Float swell_height{
    "renderer.swell.height_m", -1.0, -1.0, 15.0,
    "A hand swell arriving from a distant storm, significant height in deep water, metres, on every "
    "open body that draws a wave table, at once (2 a fair surf, 4 a big one, 6 a storm swell); "
    "below 0 the scene's"};
tunables::Float swell_period{"renderer.swell.period_s", 12.0, 4.0, 25.0,
                             "The hand swell's peak period, seconds (10 to 20 for a storm's swell)"};
tunables::Float swell_to{
    "renderer.swell.to_deg", -1.0, -1.0, 360.0,
    "Where the hand swell travels to: degrees in the ground plane from +x towards +z; below 0 as "
    "each body's shore train does"};

std::string field(const std::string& path, const char* what) { return path + ": sky.wind" + what; }
std::string swell_field(const std::string& path, const char* what) {
  return path + ": sky.swell" + what;
}

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

bool read_scene_swell(const scene::Swell& swell, const std::string& path, gfx::SwellDesc& out,
                      std::string& error) {
  out = gfx::SwellDesc{};
  const auto height_ok = [](f32 v) { return v >= 0.0f && v <= 15.0f; };
  const auto period_ok = [](f32 v) { return v >= 4.0f && v <= 25.0f; };
  if (!(swell.distance_km >= 0.0f && swell.distance_km <= 20000.0f)) {
    error = swell_field(path, ".distance_km") + " is outside [0, 20000] km";
    return false;
  }
  out.distance_km =
      swell.distance_km > 0.0f ? static_cast<f64>(swell.distance_km) : gfx::k_swell_default_km;
  if (!swell.track.empty()) {
    if (!(swell.period_h > 0.0f && swell.period_h <= 8760.0f)) {
      error = swell_field(path, ".period_h") + " is outside (0, 8760] h";
      return false;
    }
    f32 last = -1.0f;
    for (u32 i = 0; i < swell.track.size(); ++i) {
      const scene::SwellKey& key = swell.track[i];
      const std::string at = swell_field(path, ".track[") + std::to_string(i) + "]";
      if (!(key.hour >= 0.0f && key.hour < swell.period_h) || !(key.hour > last)) {
        error = at + ".hour is out of the period or out of order";
        return false;
      }
      if (!height_ok(key.height_m) || !period_ok(key.period_s)) {
        error = at + " has a height outside [0, 15] m or a period outside [4, 25] s";
        return false;
      }
      last = key.hour;
      out.track.push_back(gfx::SwellKey{static_cast<f64>(key.hour), static_cast<f64>(key.height_m),
                                        static_cast<f64>(key.period_s),
                                        static_cast<f64>(key.to_deg)});
    }
    out.period_h = static_cast<f64>(swell.period_h);
    out.kind = gfx::SwellKind::track;
    return true;
  }
  if (!height_ok(swell.height_m) || !period_ok(swell.period_s)) {
    error = swell_field(path, "") + " has a height outside [0, 15] m or a period outside [4, 25] s";
    return false;
  }
  out.height_m = static_cast<f64>(swell.height_m);
  out.period_s = static_cast<f64>(swell.period_s);
  out.to_deg = static_cast<f64>(swell.to_deg);
  out.kind = swell.height_m > 0.0f ? gfx::SwellKind::fixed : gfx::SwellKind::none;
  return true;
}

SwellHand swell_hand_tunable() noexcept {
  SwellHand hand;
  const f64 height = swell_height.get();
  if (height >= 0.0) {
    hand.set = true;
    hand.height_m = height;
    hand.period_s = swell_period.get();
  }
  const f64 to = swell_to.get();
  if (to >= 0.0) {
    hand.turned = true;
    hand.to_deg = to;
  }
  return hand;
}

gfx::WaterCross water_cross_of_span(f64 height_m, f64 period_s, Vec2 direction) noexcept {
  if (!(height_m > 0.0) || !(period_s > 0.0)) return gfx::WaterCross{};
  // The mean shape's density per log frequency an octave above the peak, against the peak's.
  const f64 p = 2.0 * gfx::k_wind_pi / period_s;
  const f64 share = 2.0 * gfx::jonswap(2.0 * p, 1.0, p) / gfx::jonswap(p, 1.0, p);
  const f64 h1 = height_m / std::sqrt(1.0 + share);
  const f64 h2 = height_m * std::sqrt(share / (1.0 + share));
  return gfx::water_cross_of_swell(h1, period_s, h2, 0.5 * period_s, direction);
}

bool water_train_heights(const gfx::SeaState& state, f64 period_1, f64 period_2, f64& h1, f64& h2,
                         f64& period) noexcept {
  if (!(period_1 > 0.0) || !(period_2 > 0.0)) return false;
  const f64 o1 = 2.0 * gfx::k_wind_pi / period_1;
  const f64 o2 = 2.0 * gfx::k_wind_pi / period_2;
  const f64 w1 = gfx::sea_density(state, o1) * o1;
  const f64 w2 = gfx::sea_density(state, o2) * o2;
  if (!(w1 + w2 > 0.0)) return false;
  const f64 h = state.height_m();
  h1 = h * std::sqrt(w1 / (w1 + w2));
  h2 = h * std::sqrt(w2 / (w1 + w2));
  period = (w1 * period_1 + w2 * period_2) / (w1 + w2);
  return true;
}

void water_wind_frame(std::span<WaterWindBody> bodies, f64 sky_time_s,
                      Vector<WaterWindUpload>& uploads) {
  uploads.clear();
  if (bodies.empty()) return;
  const WindHand hand = wind_hand_tunable();
  const SwellHand hand_swell = swell_hand_tunable();
  const f64 quantum = std::floor(sky_time_s / k_water_wind_quantum_s) * k_water_wind_quantum_s;
  for (WaterWindBody& b : bodies) {
    // What the body draws now, and the key that says so: 0 the load's table, 1 a hand wind or
    // swell (W136-3: the swell on open bodies only), 2 the body's own sea (`sea`) at the quantum.
    const bool swell_here = hand_swell.set && b.open;
    const bool follows = b.sea.kind != gfx::WindKind::none;
    u32 kind = 0;
    f64 key[5]{0.0, 0.0, 0.0, 0.0, 0.0};
    gfx::WindDesc held;
    if (hand.set || swell_here) {
      kind = 1;
      // The wind: the hand's, fully grown at once; else the body's sea's; else its own, fixed.
      if (follows) held = b.sea;
      if (hand.set || !follows) {
        held.kind = gfx::WindKind::fixed;
        held.speed_m_s = hand.set ? hand.speed_m_s : b.load_wind_m_s;
        held.to_deg = hand.set && hand.turned ? hand.to_deg
                                              : std::atan2(static_cast<f64>(b.direction.y),
                                                           static_cast<f64>(b.direction.x)) *
                                                    (180.0 / gfx::k_wind_pi);
        held.track.clear();
      }
      if (swell_here) {
        held.swell = gfx::SwellDesc{};
        held.swell.kind = hand_swell.height_m > 0.0 ? gfx::SwellKind::fixed : gfx::SwellKind::none;
        held.swell.height_m = hand_swell.height_m;
        held.swell.period_s = hand_swell.period_s;
        held.swell.to_deg = hand_swell.turned ? hand_swell.to_deg
                                              : std::atan2(static_cast<f64>(b.train_to.y),
                                                           static_cast<f64>(b.train_to.x)) *
                                                    (180.0 / gfx::k_wind_pi);
        held.swell.distance_km = follows ? b.sea.swell.distance_km : gfx::k_swell_default_km;
      }
      key[0] = hand.set ? hand.speed_m_s : -1.0;
      key[1] = hand.set && hand.turned ? hand.to_deg : -1.0;
      key[2] = swell_here ? hand_swell.height_m : -1.0;
      key[3] = swell_here ? hand_swell.period_s : -1.0;
      key[4] = swell_here && hand_swell.turned ? hand_swell.to_deg : -1.0;
    } else if (follows && (b.scene_wind || b.sky_swell) && b.sea.sea_varies()) {
      kind = 2;
    }
    // A sea that varies is taken at the quantum; a held one at once.
    const f64 at = kind == 2 || (kind == 1 && held.sea_varies()) ? quantum : -1.0;
    if (kind == b.key_kind && at == b.key_a && key[0] == b.key_b && key[1] == b.key_c &&
        key[2] == b.key_d && key[3] == b.key_e && key[4] == b.hand_key[4])
      continue;
    b.key_kind = kind;
    b.key_a = at;
    b.key_b = key[0];
    b.key_c = key[1];
    b.key_d = key[2];
    b.key_e = key[3];
    b.hand_key[4] = key[4];
    gfx::WaterWave waves[gfx::k_water_waves];
    gfx::WaterSurface surface = b.load_surface;
    if (kind == 0) {
      std::memcpy(waves, b.load_waves, sizeof(waves));
    } else {
      const gfx::SeaState state = kind == 1 ? gfx::sea_state(held, quantum, b.fetch_m)
                                            : gfx::sea_state(b.sea, quantum, b.fetch_m);
      gfx::WaterWaveGrid grid = b.grid;
      if (kind == 1) {
        // The hand's grid, drawn again only when the hand moves (a scene sea under it keeps its
        // bands from quantum to quantum, so no wave jumps while the hand holds).
        if (key[0] != b.hand_key[0] || key[1] != b.hand_key[1] || key[2] != b.hand_key[2] ||
            key[3] != b.hand_key[3] || !b.hand_grid.valid()) {
          gfx::WindDesc range_of = held;
          if (range_of.varies()) {
            // A scene wind that varies under a hand swell: its own grid, widened to the swell.
            range_of.kind = gfx::WindKind::none;
          }
          const gfx::WaterWindRange range = gfx::water_wind_range(range_of, b.fetch_m);
          b.hand_grid = range.grid;
          if (held.varies() && b.grid.valid()) {
            b.hand_grid = b.grid;
            if (range.grid.valid()) {
              b.hand_grid.lo = std::min(b.grid.lo, range.grid.lo);
              b.hand_grid.hi = std::min(b.grid.hi, gfx::k_water_grid_max_ratio * b.hand_grid.lo);
            }
          }
          for (u32 i = 0; i < 4; ++i)
            b.hand_key[i] = key[i];
        }
        if (b.hand_grid.valid()) grid = b.hand_grid;
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
      // **The shore's trains follow the sea** (W136-2; gfx.md, "The shore's train follows the
      // sea"), unless the body's own swell is the train. Each train keeps the period and the
      // arrival field it was built with, so no crest jumps: a varying wind's two span its seas and
      // take the state's energy by its density at their frequencies, so the surf's period moves
      // from the second's to the first's as the sea grows; any other body's two keep the load's
      // split, scaled to the sea now. The run-up and the reach take the period the weights give.
      // **A swell's train** (W136-3; gfx.md, "A swell a scene names"): where the first train is a
      // swell (the sky's, or the body's own under a hand swell), it carries the arriving swell now
      // and the second the wind's sea and its leftover swell, each at the period it was built
      // with; the run-up and the reach take the two together at the period their energies give.
      const bool swell_train = b.sky_swell || (b.swell && swell_here);
      if (swell_train && b.surface_offset != ~u64{0}) {
        const gfx::WaterSurface& load = b.load_surface;
        const f64 hs = state.groundswell_height_m;
        const f64 hw = std::sqrt(state.sea_height_m * state.sea_height_m +
                                 state.swell_height_m * state.swell_height_m);
        const f64 h = std::sqrt(hs * hs + hw * hw);
        const f64 p1 = static_cast<f64>(load.period_s);
        const f64 p2 = load.cross_period_s > 0.0f ? static_cast<f64>(load.cross_period_s) : p1;
        const f64 period = h > 0.0 ? (hs * hs * p1 + hw * hw * p2) / (h * h) : p1;
        surface.height_m = static_cast<f32>(h);
        if (load.train_m > 0.0f) surface.train_m = static_cast<f32>(hs);
        if (load.cross_period_s > 0.0f) surface.cross_m = static_cast<f32>(hw);
        surface.run_up_m = static_cast<f32>(
            std::min(gfx::stockdon_r2(h, period, b.slope), gfx::k_swash_steep_cap * h));
        surface.reach_scale = static_cast<f32>(gfx::swash_reach_scale(h, period));
        surface.reach_cap = static_cast<f32>(gfx::swash_reach_cap(h, b.bank_max));
      } else if (!b.swell && b.surface_offset != ~u64{0}) {
        const gfx::WaterSurface& load = b.load_surface;
        const f64 h = state.height_m();
        f64 h1 = h;
        f64 h2 = 0.0;
        f64 period = b.period_s;
        const bool two = load.cross_m > 0.0f && load.cross_period_s > 0.0f;
        if (!(two && b.scene_wind && kind == 2 &&
              water_train_heights(state, static_cast<f64>(load.period_s),
                                  static_cast<f64>(load.cross_period_s), h1, h2, period))) {
          const f64 load_h = static_cast<f64>(load.height_m);
          const f64 scale = load_h > 0.0 ? h / load_h : 0.0;
          h1 = static_cast<f64>(load.train_m > 0.0f ? load.train_m : load.height_m) * scale;
          h2 = static_cast<f64>(load.cross_m) * scale;
          period = b.period_s;
        }
        surface.height_m = static_cast<f32>(h);
        if (load.train_m > 0.0f) surface.train_m = static_cast<f32>(h1);
        if (two) surface.cross_m = static_cast<f32>(h2);
        surface.run_up_m = static_cast<f32>(
            std::min(gfx::stockdon_r2(h, period, b.slope), gfx::k_swash_steep_cap * h));
        surface.reach_scale = static_cast<f32>(gfx::swash_reach_scale(h, period));
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
      // W136-3: the sea the body draws, whether its train is the sky's swell, and where it goes.
      b.open = water.body.open;
      b.sea = water.sea;
      b.sky_swell = water.scene_swell && water.swell_train_m > 0.0f;
      b.load_wind_m_s = static_cast<f64>(water.wind_m_s);
      if (b.sky_swell) {
        b.train_to = water.swell_train_to;
      } else if (b.swell) {
        const f32 a = radians(water.body.swell_direction_deg);
        b.train_to = Vec2{std::cos(a), std::sin(a)};
      } else {
        b.train_to = water.shading.direction;
      }
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
                             sky_time_s, water_wind_uploads_);
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
