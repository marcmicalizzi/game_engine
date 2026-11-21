#include "wind_controls.h"

#include <systems/renderer/scene_wind.h>

#include <cmath>
#include <cstdio>

namespace engine::view {

void WindControls::start(f64 roughness_m, f64 base_mean_m_s, f64 base_to_deg) noexcept {
  *this = WindControls{};
  roughness_m_ = roughness_m > 0.0 ? roughness_m : 0.03;
  base_mean_m_s_ = base_mean_m_s;
  base_to_deg_ = base_to_deg;
}

void WindControls::set_base(f64 mean_m_s, f64 to_deg) noexcept {
  if (set_) return;
  base_mean_m_s_ = mean_m_s;
  base_to_deg_ = to_deg;
}

bool WindControls::step(bool up) noexcept {
  const f64 from = mean_m_s();
  f64 next = from;
  // The Beaufort forces' speeds ascend: up, the first above; down, the last below. A hair of
  // slack so a mean on a rung steps to the next one, not to itself.
  constexpr f64 k_slack = 1e-9;
  if (up) {
    for (u32 f = 0; f <= gfx::k_beaufort_max; ++f) {
      if (gfx::beaufort_speed(f) > from + k_slack) {
        next = gfx::beaufort_speed(f);
        break;
      }
    }
  } else {
    for (u32 f = gfx::k_beaufort_max + 1; f > 0; --f) {
      if (gfx::beaufort_speed(f - 1) < from - k_slack) {
        next = gfx::beaufort_speed(f - 1);
        break;
      }
    }
  }
  if (!set_) {
    set_ = true;
    to_deg_ = base_to_deg_;
  }
  if (next == from) {
    mean_m_s_ = from;
    return false;
  }
  mean_m_s_ = next;
  ++steps_;
  return true;
}

void WindControls::set(f64 mean_m_s, f64 to_deg) noexcept {
  const f64 m = !(mean_m_s > 0.0)                ? 0.0
                : mean_m_s > gfx::k_wind_max_m_s ? gfx::k_wind_max_m_s
                                                 : mean_m_s;
  if (!set_) {
    set_ = true;
    to_deg_ = base_to_deg_;
  }
  if (m != mean_m_s_) ++steps_;
  mean_m_s_ = m;
  if (to_deg >= 0.0 && std::isfinite(to_deg)) to_deg_ = std::fmod(to_deg, 360.0);
  ++sets_;
}

void WindControls::tick(f64 dt, bool key_held) noexcept {
  held_ = key_held || game_gust_;
  if (held_) {
    ++gust_ticks_;
    if (!set_) {
      set_ = true;
      mean_m_s_ = base_mean_m_s_;
      to_deg_ = base_to_deg_;
    }
  }
  phase_ = gfx::wind_gust_phase_step(phase_, held_, dt);
}

gfx::WindLive WindControls::live() const noexcept {
  gfx::WindLive out;
  if (!set_) return out;
  out.set = true;
  out.mean_m_s = mean_m_s_;
  out.to_deg = to_deg_;
  out.gust = gfx::wind_gust_level(phase_);
  return out;
}

WindStart wind_start(const gfx::WindDesc& scene_wind) noexcept {
  WindStart out;
  if (scene_wind.kind != gfx::WindKind::none) out.roughness_m = scene_wind.roughness_m;
  out.mean_m_s = scene_wind.kind == gfx::WindKind::fixed ? scene_wind.speed_m_s : 0.0;
  out.to_deg = scene_wind.kind != gfx::WindKind::none ? scene_wind.to_deg : 0.0;
  const renderer::WindHand hand = renderer::wind_hand_tunable();
  if (hand.set) out.mean_m_s = hand.speed_m_s;
  if (hand.set && hand.turned) out.to_deg = hand.to_deg;
  return out;
}

const char* format_wind(const WindControls& wind, char* out, usize size) noexcept {
  if (size == 0) return out;
  const f64 mean = wind.mean_m_s();
  const u32 force = gfx::beaufort_force(mean);
  const f64 speed = wind.speed_m_s();
  if (wind.gust_level() > 0.0 && speed > mean + 0.05) {
    std::snprintf(out, size, "wind %.1f m/s (Bft %u) gusting %.1f", mean, force, speed);
  } else {
    std::snprintf(out, size, "wind %.1f m/s (Bft %u)", mean, force);
  }
  return out;
}

}  // namespace engine::view
