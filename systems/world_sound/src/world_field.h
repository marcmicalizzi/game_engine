#pragma once

// The world's sound inside the stage (S112; world_sound.h, docs/subsystems/world_sound.md, "The
// wind in the trees and at the ears"): the crowns the host's canopy table names, the beds the rest
// merge into by power, and the ears' two voices, each a shaped-noise voice of the mixer
// (domain/audio/noise_voice.h) re-described every tick from the frame's wind. Internal to the
// module: the stage owns one and hands it the listener, the mixer's frame and the tick.

#include <core/containers/vector.h>
#include <domain/audio/mixer.h>
#include <systems/world_sound/world_sound.h>

namespace engine::world_sound {

// The tunables' values, read once when the stage opens.
struct FieldConfig {
  u32 crowns = 6;
  u32 beds = 8;
  u32 max_crowns = 8192;
  f64 reach_m = 2000.0;
  u32 refresh_hz = 8;
  f64 reference_db = 105.0;
  f64 ears_db = -30.0;
  f64 ears_side_db = 4.0;
  f64 ears_slope_db = 26.0;
  f64 calm_m_s = 0.5;
  f64 floor_db = 10.0;
  bool turbulence = true;
};
FieldConfig field_config_from_tunables() noexcept;

class WorldField {
 public:
  // Sizes every array; nothing after allocates. `mixer` outlives the field.
  void open(audio::Mixer* mixer, u32 tick_hz, const FieldConfig& config);
  bool is_open() const noexcept { return mixer_ != nullptr; }
  void set_canopy(const CanopySource& canopy) noexcept { canopy_ = canopy; }
  void set_wind(const WorldWind& wind) noexcept { wind_ = wind; }
  // One tick: a refresh when it is due, then every voice described. `frame` is the mixer's frame
  // (the corner of the listener's cell); `frame_moved` places every world voice again in it.
  void tick(const WorldPos& listener, const Vec3& forward, const WorldPos& frame,
            bool frame_moved) noexcept;
  // Every world voice stopped (the stage goes silent).
  void stop_all() noexcept;

  const WorldStats& stats() const noexcept { return stats_; }
  u32 voice_count() const noexcept { return static_cast<u32>(voices_.size()); }
  WorldVoiceInfo voice(u32 i) const noexcept {
    return i < voices_.size() ? voices_[i].info : WorldVoiceInfo{};
  }

  // The wind 10 m up at a place of the world, by the sway's arguments (S112): the live mean and
  // direction under the scene's turbulence with the forced gust over it, or the scene's own wind.
  f64 wind10_at(f64 x, f64 z, Vec2& to, f64& z0) const noexcept;

 private:
  struct Voice {
    WorldVoiceKind kind = WorldVoiceKind::None;
    audio::VoiceHandle voice;
    u64 key = 0;          // a crown's identity (its place, hashed); a bed's sector; an ear's side
    WorldPos at{};        // where it sounds (a bed: a point the listener's direction gives)
    Crown crown{};        // a crown voice's crown, as the last refresh sent it
    f32 spread = 0.0f;
    f32 sent_tone_hz = 0.0f;
    audio::NoiseParams sent{};
    bool has_sent = false;
    WorldVoiceInfo info;
  };
  // A bed's share of the crowns, gathered at a refresh: the power each class gives at the listener
  // per (U10 / 5)^3, the needles' tone, and where its power comes from.
  struct Bed {
    f64 weight[3] = {};
    f64 needle_weight_d = 0.0;  // the needle class's weight x 1 / d, for its mean
    f64 needle_loglaw = 0.0;    // the needle class's weight x its crowns' log law
    f64 px = 0.0, pz = 0.0, total = 0.0;  // power-weighted place, for the gust
    f64 dx = 0.0, dz = 0.0;               // power-weighted unit directions from the listener
  };

  void refresh(const WorldPos& listener) noexcept;
  void describe(Voice& v, const f64 (&band_power)[9], f64 tone_hz, f64 tone_power,
                bool two_d, f32 pan, u64 seed, u8 priority, const WorldPos& frame) noexcept;
  void silence(Voice& v) noexcept;

  audio::Mixer* mixer_ = nullptr;
  FieldConfig config_{};
  u32 tick_hz_ = 240;
  u64 ticks_ = 0;
  u32 refresh_every_ = 30;
  CanopySource canopy_{};
  WorldWind wind_{};
  Vector<Crown> crowns_;      // the last refresh's crowns
  Vector<f64> crown_static_;  // each one's power at the listener per (U10 / 5)^3, from the refresh
  Vector<u64> crown_key_;     // each one's identity, its place hashed
  Vector<Voice> voices_;      // crowns, then beds, then the two ears
  Vector<Bed> beds_;
  Vector<u64> chosen_;        // the crowns chosen as voices of their own, by key
  WorldPos last_listener_{};
  bool has_listener_ = false;
  WorldStats stats_{};
  f64 shape_[3][8] = {};      // each class's band shape, powers summing to one
};

}  // namespace engine::world_sound
