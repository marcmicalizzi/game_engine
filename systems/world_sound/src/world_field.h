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
  // One kind of voice heard alone (`audio.world.solo`): 0 all, 1 crowns, 2 beds, 3 the ears, and
  // since S113 4 streams, 5 falls, 6 the surf.
  u32 solo = 0;
  // Running water and the surf (S113; water_field.cpp): the voices of each kind and the sources a
  // refresh keeps.
  u32 streams = 4;
  u32 falls = 4;
  u32 surfs = 3;
  u32 max_water = 4096;
  f64 water_reach_m = 800.0;  // a stream or a stretch of surf past this is not summed
  // A surf record whose bound is under the floor less 20 dB is skipped (`audio.world.surf_cull`).
  bool surf_cull = true;
};
FieldConfig field_config_from_tunables() noexcept;
// The water's tunables (S113, `audio.world.streams` and its neighbours; water_field.cpp).
void water_config_from_tunables(FieldConfig& config) noexcept;
// The surf's envelope's mean over a period, which the envelope is divided by (water_field.cpp).
f64 surf_envelope_mean() noexcept;

class WorldField {
 public:
  // Sizes every array; nothing after allocates. `mixer` outlives the field.
  void open(audio::Mixer* mixer, u32 tick_hz, const FieldConfig& config);
  bool is_open() const noexcept { return mixer_ != nullptr; }
  void set_canopy(const CanopySource& canopy) noexcept { canopy_ = canopy; }
  void set_shelter(const ShelterSource& shelter) noexcept { shelter_ = shelter; }
  void set_wind(const WorldWind& wind) noexcept { wind_ = wind; }
  void set_flow(const FlowSource& flow) noexcept { flow_ = flow; }
  void set_surf(const SurfSource& surf) noexcept { surf_ = surf; }
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
    // Running water and the surf (S113): the band powers at the listener after the path of every
    // stretch, fall and stretch of surf not heard as a voice of its own (the surf at its mean over
    // a period). The wind does not move them.
    f64 water[8] = {};
    f64 water_total = 0.0;
    i64 water_count = 0;
  };
  // **A water source at a refresh** (S113; water_field.cpp): its kind, identity and place, its
  // band powers at the listener after the path (the surf's at its mean), and what its voice needs
  // between refreshes.
  struct WaterSource {
    WorldVoiceKind kind = WorldVoiceKind::Stream;
    u64 key = 0;
    WorldPos at{};  // where its voice sounds: a stretch's nearest point, a fall's foot, the break
    f32 spread = 0.0f;
    f64 band[8] = {};
    f64 total = 0.0;       // the sum of `band`: what choosing ranks by
    f64 dist = 0.0;        // from the listener, as `take_water` ranks it when the table is full
    f64 trickle = 0.0;     // the share of a stream's power heard as separate bubbles
    f64 bubbles_hz = 0.0;  // their rate
    f64 path[8] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};  // a bubble takes its own band's
    f64 period_s = 0.0;                                      // the surf's
    f64 arrival_s = 0.0;
  };
  // A water voice's own between ticks: the bubbles' clock and the next resonator.
  struct WaterVoice {
    WaterSource source{};
    f64 next_bubble_s = 0.0;
    u32 resonator = 0;
    u32 rng = 1;
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
  void describe(Voice& v, const f64 (&band_power)[9], f64 tone_hz, f64 extra, bool two_d, f32 pan,
                u64 seed, u8 priority, const WorldPos& frame) noexcept;
  void silence(Voice& v) noexcept;
  // ---- running water and the surf (S113; water_field.cpp) -----------------------------------
  // At a refresh, after the crowns: the flow and surf tables visited, each source's power at the
  // listener through its path, everything into its sector's bed, then the loudest of each kind
  // chosen (3 dB of hysteresis) and taken out of its bed again by the same arithmetic.
  void refresh_water(const WorldPos& listener) noexcept;
  void take_water(const WaterSource& s) noexcept;
  void add_water_to_bed(const WaterSource& s, const WorldPos& listener, f64 sign) noexcept;
  // **The path's eight bands** (S114) from a source `dx`, `dy`, `dz` from the listener at `height`
  // over its ground: the air over the whole path, the forest over its share under canopy, the
  // ground's term; 1 a band with `audio.world.path` off. A crown's and the water's alike.
  void path_terms(f64 dx, f64 dy, f64 dz, f64 height, f64 (&path)[8]) const noexcept;
  // Every tick: each water voice's bands (the surf's envelope at the water's clock), and the
  // bubbles a trickle's voice strikes.
  void tick_water(const WorldPos& frame, bool frame_moved) noexcept;
  void strike_bubbles(Voice& v, WaterVoice& w, f64 now_s, f64 dt) noexcept;
  // Each kind's power at the listener summed over the ticks (after the hold), for the summary's
  // levels by kind: what a listening run reads without soloing each kind in turn.
  void account_levels() noexcept;

 public:
  // The mean level of each kind over the ticks so far, dB SPL (-1000: never heard): crowns,
  // beds (all), beds' water, the ears, streams, falls, surf; and the ticks on which the streams
  // and falls stood over the wind's own (the crowns, the ears and the beds' wind).
  static constexpr u32 k_level_kinds = 7;
  f64 mean_level_db(u32 kind) const noexcept;
  u64 water_over_wind_ticks() const noexcept { return water_over_wind_; }
  u64 level_ticks() const noexcept { return level_ticks_; }

 private:
  bool water_voice(const Voice& v) const noexcept {
    return v.kind == WorldVoiceKind::Stream || v.kind == WorldVoiceKind::Fall ||
           v.kind == WorldVoiceKind::Surf;
  }

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
  // Running water and the surf (S113): the tables, a refresh's sources, the farthest-first heap
  // once full, and each water voice's own (parallel to the voices from `water_first_`).
  FlowSource flow_{};
  SurfSource surf_{};
  Vector<WaterSource> water_;
  Vector<u32> water_heap_;
  Vector<WaterVoice> water_voices_;
  u32 water_first_ = 0;
  f64 surf_shape_mean_ = 1.0;  // the surf envelope's mean over a period, for its normalisation
  f64 cull_power_ = 0.0;       // a stretch under this before its path is not taken (floor - 20 dB)
  f64 level_sum_[k_level_kinds] = {};
  u64 level_ticks_ = 0;
  u64 water_over_wind_ = 0;
};

}  // namespace engine::world_sound
