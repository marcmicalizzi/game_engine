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
  f64 ceiling_db = -12.0;
  bool turbulence = true;
  // The path (S114; path.h): the air's temperature and humidity, the ground's factor, and whether
  // the path's terms are taken at all (off: S112's spreading alone, for a comparison).
  f64 temperature_c = 20.0;
  f64 humidity_pct = 70.0;
  f64 ground_g = 1.0;
  bool path = true;
  // One kind of voice heard alone (`audio.world.solo`): 0 all, 1 crowns, 2 beds, 3 the ears.
  u32 solo = 0;
};
FieldConfig field_config_from_tunables() noexcept;

class WorldField {
 public:
  // Sizes every array; nothing after allocates. `mixer` outlives the field.
  void open(audio::Mixer* mixer, u32 tick_hz, const FieldConfig& config);
  bool is_open() const noexcept { return mixer_ != nullptr; }
  void set_canopy(const CanopySource& canopy) noexcept { canopy_ = canopy; }
  void set_shelter(const ShelterSource& shelter) noexcept { shelter_ = shelter; }
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
    u64 key = 0;    // a crown's identity (its place, hashed); a bed's sector; an ear's side
    WorldPos at{};  // where it sounds (a bed: a point the listener's direction gives)
    Crown crown{};  // a crown voice's crown, as the last refresh sent it
    f32 spread = 0.0f;
    f32 sent_tone_hz = 0.0f;
    audio::NoiseParams sent{};
    bool has_sent = false;
    bool needs_place = false;  // a move the ring refused: placed again on the next tick (L3)
    WorldVoiceInfo info;
    // This tick's description before the world's ceiling (`tick`'s two passes).
    f64 bands[9] = {};
    f64 tone_hz = 0.0;
    // A crown voice's path (S114): what the path leaves of each band, as the last refresh found it.
    f64 path[8] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  };
  // **A bed's share of the crowns** at a refresh, in `k_bed_rings` rings of distance so the bed
  // hears the gust where its power comes from at each (S114: a gust front crosses a wide wood, and
  // a bed that sampled one place stood almost still): each band's power at the listener per (U10 /
  // 5)^3 after the path, the needles' tone, and where the power comes from.
  struct BedRing {
    f64 band[8] = {};
    f64 tone[8] = {};           // the needles' tone power, by the band whose path it takes
    f64 needle_weight = 0.0;    // the needle class's tone power before the path
    f64 needle_weight_d = 0.0;  // ... x 1 / d, for its mean
    f64 needle_loglaw = 0.0;    // ... x its crowns' log law
    f64 px = 0.0, pz = 0.0, total = 0.0;  // power-weighted place, for the gust
    i64 count = 0;                        // crowns in it: what was taken out leaves an exact zero
  };
  static constexpr u32 k_bed_rings = 3;
  struct Bed {
    BedRing ring[k_bed_rings];
    f64 total = 0.0;
    i64 count = 0;
    f64 dx = 0.0, dz = 0.0;  // power-weighted unit directions from the listener
  };
  // **How much of a path runs under canopy** (S114): leaf area by sector and ring of distance round
  // the listener, a refresh's, each bin's cover the share of a closed canopy its leaf area makes.
  static constexpr u32 k_cover_sectors = 16;
  static constexpr u32 k_cover_rings = 15;

  void refresh(const WorldPos& listener) noexcept;
  // A crown's power at the listener per (U10 / 5)^3, band by band after the path, and its tone's
  // power before the path; the sum of `band` is what choosing ranks by. False past the reach.
  bool crown_power(const Crown& c, const WorldPos& listener, f64 law, f64 (&band)[8],
                   f64 (&path)[8], f64& tone) const noexcept;
  // The same for a point `dx`, `dy`, `dz` from the listener heard no nearer than `least`.
  void point_power(const Crown& c, f64 dx, f64 dy, f64 dz, f64 least, f64 law, f64 (&band)[8],
                   f64 (&path)[8], f64& tone) const noexcept;
  // The share of a path along the ground from the listener to `dx`, `dz` that runs under canopy,
  // in metres, from the refresh's cover.
  f64 canopy_path_m(f64 dx, f64 dz) const noexcept;
  void add_to_bed(const Crown& c, const WorldPos& listener, f64 law, const f64 (&band)[8],
                  const f64 (&path)[8], f64 tone, f64 sign) noexcept;
  // A crown sent by the canopy table: kept while there is room, then only in place of a farther
  // one (M1 of S112's review), so the table's order never loses the tree over the listener.
  void take(const Crown& c) noexcept;
  // Placed in `frame` again (a moved frame, a moved bed, or a move the ring refused).
  void place(Voice& v, const WorldPos& frame) noexcept;
  void describe(Voice& v, const f64 (&band_power)[9], f64 tone_hz, f64 tone_power, bool two_d,
                f32 pan, u64 seed, u8 priority, const WorldPos& frame) noexcept;
  void silence(Voice& v) noexcept;

  audio::Mixer* mixer_ = nullptr;
  FieldConfig config_{};
  u32 tick_hz_ = 240;
  u64 ticks_ = 0;
  u32 refresh_every_ = 30;
  CanopySource canopy_{};
  ShelterSource shelter_{};
  f64 shelter_share_ = 1.0;  // the ears' share of the open wind, from the last refresh
  WorldWind wind_{};
  Vector<Crown> crowns_;      // the last refresh's crowns
  Vector<f64> crown_static_;  // each one's power at the listener per (U10 / 5)^3, from the refresh
  Vector<u64> crown_key_;     // each one's identity, its place hashed
  Vector<f64> crown_dist_;    // each one's distance from the listener, as `take` ranks it
  Vector<u32> far_heap_;      // once full: the kept crowns as a heap, the farthest on top
  WorldPos taking_at_{};      // the listener a refresh's `take` measures from
  Vector<Voice> voices_;      // crowns, then beds, then the two ears
  Vector<Bed> beds_;
  Vector<u64> chosen_;  // the crowns chosen as voices of their own, by key
  WorldPos last_listener_{};
  bool has_listener_ = false;
  WorldStats stats_{};
  f64 shape_[3][8] = {};      // each class's band shape, powers summing to one
  f64 class_power_[3] = {};   // each class's anchor as a power
  f64 air_db_m_[8] = {};      // the air's absorption a metre at each band's centre (S114)
  f64 foliage_db_m_[8] = {};  // Hoover's term a metre under canopy at each band's centre
  f64 cover_[k_cover_sectors][k_cover_rings] = {};  // leaf area, then the cover's share
  f64 cover_run_[k_cover_sectors][k_cover_rings + 1] =
      {};  // metres under canopy to each ring's start
};

}  // namespace engine::world_sound
