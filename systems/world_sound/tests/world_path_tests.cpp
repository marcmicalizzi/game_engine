// Distance, and the far wood's level (roadmap S114; docs/subsystems/world_sound.md, "Distance";
// the sources: docs/experiments/world-sound.md, "Read again by S114"): the path's terms against the
// standards' own figures, and the world's voices by kind on a wood of the case's own, through the
// stage's descriptions. No case opens a device.

#include <domain/audio/mixer.h>
#include <domain/audio/noise_voice.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/path.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <schemas/scene.h>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

TEST_CASE("world path: the air's absorption is ISO 9613-1's table at its mid-band frequencies") {
  // ISO 9613-1's exact mid-band frequencies, 1000 x 10^(0.3 k), and its table at 20 degC, dB a km.
  const f64 f[8] = {63.0957, 125.893, 251.189, 501.187, 1000.0, 1995.26, 3981.07, 7943.28};
  const f64 at70[8] = {0.090, 0.339, 1.13, 2.80, 4.98, 9.02, 22.9, 76.6};
  const f64 at10[8] = {0.370, 0.775, 1.58, 4.25, 14.1, 45.3, 109.0, 175.0};
  const f64 at90[8] = {0.071, 0.272, 0.966, 2.71, 5.30, 9.06, 20.2, 62.6};
  for (u32 b = 0; b < 8; ++b) {
    CHECK(air_absorption_db_per_m(f[b], 20.0, 70.0) * 1000.0 ==
          doctest::Approx(at70[b]).epsilon(0.01));
    CHECK(air_absorption_db_per_m(f[b], 20.0, 10.0) * 1000.0 ==
          doctest::Approx(at10[b]).epsilon(0.01));
    CHECK(air_absorption_db_per_m(f[b], 20.0, 90.0) * 1000.0 ==
          doctest::Approx(at90[b]).epsilon(0.01));
  }
  // The row's figures: at 200 m and 70 %, of the order of 15 dB at 8 kHz and one at 1 kHz.
  CHECK(air_absorption_db_per_m(8000.0, 20.0, 70.0) * 200.0 == doctest::Approx(15.5).epsilon(0.02));
  CHECK(air_absorption_db_per_m(1000.0, 20.0, 70.0) * 200.0 == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("world path: Hoover's forest term, the ground's and a barrier's, at the row's figures") {
  // A = 0.01 r f^(1/3): 100 m of forest is 10 dB at 1 kHz and 20 at 8 kHz.
  CHECK(foliage_db(1000.0, 100.0) == doctest::Approx(10.0).epsilon(1e-9));
  CHECK(foliage_db(8000.0, 100.0) == doctest::Approx(20.0).epsilon(1e-9));
  CHECK(foliage_db(1000.0, 0.0) == 0.0);
  // The ground at 200 m: a source on soft ground loses 13.7 dB at 500 Hz in its own region (the
  // row's "of the order of 14"), and over rock gains the region's 1.5 dB. The region alone is the
  // difference against a source high enough that its region and the middle's take nothing.
  const f64 soft = ground_db(3, 0.0, 1.7, 200.0, 1.0) - ground_db(3, 10.0, 1.7, 200.0, 1.0);
  CHECK(soft ==
        doctest::Approx(-1.5 + 1.5 + 14.0 * (1.0 - std::exp(-4.0)) + 1.5 - 1.5).epsilon(1e-9));
  CHECK(soft == doctest::Approx(13.74).epsilon(0.001));
  // Over rock every region is -1.5: the source's region a gain of 1.5 dB at every band.
  for (u32 b = 1; b < 8; ++b) {
    const f64 hard_middle = -3.0 * (1.0 - 30.0 * (0.0 + 1.7) / 200.0);
    CHECK(ground_db(b, 0.0, 1.7, 200.0, 0.0) ==
          doctest::Approx(-1.5 - 1.5 + hard_middle).epsilon(1e-9));
  }
  // Porous ground takes nothing above 1 kHz and the middle's 63 Hz band still gains.
  CHECK(ground_db(6, 0.0, 1.7, 200.0, 1.0) == doctest::Approx(0.0).epsilon(1e-12));
  // A ridge takes the high bands first: a detour of 0.1 m, 5.7 dB at 125 Hz (S110 rounded it to
  // 6), 9.5 at 1 kHz, 17 at 8.
  CHECK(barrier_db(125.0, 0.1) == doctest::Approx(5.72).epsilon(0.01));
  CHECK(barrier_db(1000.0, 0.1) == doctest::Approx(9.5).epsilon(0.02));
  CHECK(barrier_db(8000.0, 0.1) == doctest::Approx(17.0).epsilon(0.02));
  CHECK(barrier_db(8000.0, 1.0) == 20.0);  // one edge holds at 20
  CHECK(barrier_db(1000.0, 0.0) == 0.0);   // in sight: no barrier
}

namespace {

constexpr f64 k_far = 1.0e7;

// **A wood of the case's own**: crowns on an 8 m grid (a 6 m crown at the closed canopy's leaf area
// index, 224 m2 of leaf on 64 m2 of ground) out to `near` from the listener, alternately blade and
// needle, and past it the wood merged by 128 m cell as a host's table sends it, out to 2 km: far
// groups in every sector; nothing past `cut`.
struct Wood {
  std::vector<Crown> crowns;
  static void visit(void* user, const WorldPos& centre, f64 reach_m, const CrownSink& sink) {
    for (const Crown& c : static_cast<Wood*>(user)->crowns) {
      const f64 dx = c.at.x - centre.x;
      const f64 dz = c.at.z - centre.z;
      if (dx * dx + dz * dz <= reach_m * reach_m) sink.emit(sink.user, c);
    }
  }
  CanopySource source() { return CanopySource{this, &Wood::visit}; }
};

Wood wood(f64 near = 384.0, f64 cut = 1.0e9) {
  Wood w;
  const i32 n = static_cast<i32>(near / 8.0);
  for (i32 i = -n; i <= n; ++i) {
    for (i32 j = -n; j <= n; ++j) {
      const f64 x = 8.0 * i + 4.0;
      const f64 z = 8.0 * j + 4.0;
      if (x * x + z * z > near * near || x * x + z * z > cut * cut) continue;
      Crown c{WorldPos{k_far + x, 0.0, k_far + z}, 10.0f, 224.0f,
              ((i + j) & 1) != 0 ? CanopyClass::Needle : CanopyClass::Blade, 1.2f};
      c.extent_m = 3.0f;
      w.crowns.push_back(c);
    }
  }
  for (i32 i = -16; i < 16; ++i) {
    for (i32 j = -16; j < 16; ++j) {
      const f64 x = 128.0 * i + 64.0;
      const f64 z = 128.0 * j + 64.0;
      const f64 d = std::sqrt(x * x + z * z);
      if (d <= near + 64.0 || d > 2000.0 || d > cut) continue;
      Crown c{WorldPos{k_far + x, 0.0, k_far + z}, 10.0f, 128.0f * 128.0f * 3.5f,
              CanopyClass::Blade, 1.2f};
      c.extent_m = 90.5f;
      c.merged = true;
      w.crowns.push_back(c);
    }
  }
  return w;
}

struct Listening {
  SoundStage stage;
  gfx::WindDesc wind;
  WorldWind frame;
  Wood trees;
  WorldPos ears{k_far, 1.7, k_far};
  Vec3 forward{0.0f, 0.0f, -1.0f};

  Listening(Wood w, f64 speed, bool turbulence) : trees(std::move(w)) {
    StageConfig config;
    config.mode = AudioMode::Null;
    config.world = true;
    config.turbulence = turbulence;
    config.tick_hz = 60;
    REQUIRE(stage.open(config, nullptr, nullptr));
    wind.kind = gfx::WindKind::fixed;
    wind.speed_m_s = speed;
    wind.to_deg = 90.0;
    wind.roughness_m = 0.5;  // a forest's
    frame.scene = &wind;
    frame.sky_time_s = 1000.0;
    stage.set_canopy(trees.source());
  }
  // `n` ticks of audio, the sky's clock stepped `sky_dt` a tick (a long wind in few ticks).
  void tick(u32 n, f64 sky_dt = 1.0 / 60.0) {
    for (u32 i = 0; i < n; ++i) {
      stage.set_listener(ears, forward, Vec3{0.0f, 1.0f, 0.0f});
      stage.set_wind(frame);
      stage.tick();
      frame.sky_time_s += sky_dt;
    }
  }
  f64 power(WorldVoiceKind kind, u32 band = 99) const {
    f64 p = 0.0;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      // A voice never described holds no level (its bands' zero is not 0 dB SPL).
      if (v.kind != kind || !(v.level_db > -999.0f)) continue;
      if (band == 99) {
        p += std::pow(10.0, static_cast<f64>(v.level_db) / 10.0);
      } else if (v.band_db[band] > -999.0f) {
        p += std::pow(10.0, static_cast<f64>(v.band_db[band]) / 10.0);
      }
    }
    return p;
  }
  f64 db(WorldVoiceKind kind, u32 band = 99) const {
    const f64 p = power(kind, band);
    return p > 0.0 ? 10.0 * std::log10(p) : -1000.0;
  }
};

struct Tunable {
  tunables::Tunable* t;
  Tunable(const char* name, const char* value) : t(tunables::find(name)) {
    REQUIRE(t != nullptr);
    REQUIRE(t->set_from_text(value));
  }
  ~Tunable() { t->reset(); }
};

f64 percentile(std::vector<f64> v, f64 p) {
  std::sort(v.begin(), v.end());
  const f64 at = p * static_cast<f64>(v.size() - 1u);
  const usize i = static_cast<usize>(at);
  const f64 t = at - static_cast<f64>(i);
  return i + 1u < v.size() ? v[i] * (1.0 - t) + v[i + 1u] * t : v[i];
}

}  // namespace

TEST_CASE("world path: the far wood is not a waterfall: its power and its top taken by the path") {
  // The forest's need 171 in small: inside a uniform wood at 3 m/s, S112's beds (spreading alone)
  // summed the whole wood out to 2 km and stood about 10 dB over the six nearest crowns with their
  // top octave kept. The path's terms take the far wood's power and its top first; the stand round
  // the listener, which is many crowns, still sounds over the six nearest.
  f64 over[2] = {};
  f64 tilt_bed[2] = {};
  f64 far_adds[2] = {};
  for (u32 on = 0; on < 2; ++on) {
    const Tunable path("audio.world.path", on == 0 ? "false" : "true");
    const Tunable ears("audio.world.ears_db", "-80");
    Listening s(wood(), 3.0, false);
    Listening near(wood(384.0, 128.0), 3.0, false);
    s.tick(10);
    near.tick(10);
    over[on] = s.db(WorldVoiceKind::Bed) - s.db(WorldVoiceKind::Crown);
    tilt_bed[on] = s.db(WorldVoiceKind::Bed, 7) - s.db(WorldVoiceKind::Bed, 4);
    far_adds[on] = s.db(WorldVoiceKind::Bed) - near.db(WorldVoiceKind::Bed);
    MESSAGE(std::string(on != 0 ? "path: " : "spreading alone: ")
            << "beds " << s.db(WorldVoiceKind::Bed) << " dB, crowns " << s.db(WorldVoiceKind::Crown)
            << " dB SPL; the wood past 128 m adds " << far_adds[on]
            << " dB; 8 kHz over 1 kHz: beds " << tilt_bed[on] << ", crowns "
            << s.db(WorldVoiceKind::Crown, 7) - s.db(WorldVoiceKind::Crown, 4));
    CHECK(s.stage.world_stats().crown_voices == 6u);
  }
  // Spreading alone, the fault as the owner heard it: the beds about 10 dB over the crowns, and the
  // wood past 128 m a good share of them.
  CHECK(over[0] > 8.0);
  CHECK(far_adds[0] > 2.0);
  // With the path, the far wood is all but gone from the beds and the beds are 6 dB nearer the
  // crowns ...
  CHECK(far_adds[1] < 0.5);
  CHECK(over[1] < over[0] - 6.0);
  // ... and darker: their top octave falls against their 1 kHz.
  CHECK(tilt_bed[1] < tilt_bed[0] - 2.5);
}

TEST_CASE("world path: a crown's bands at 200 m are down by the path's terms and nothing else") {
  // One crown 200 m off over a bare plain (no cover but its own bin's): each band's level with the
  // path against without it differs by the air over the 3D path, the forest over its share of the
  // path under canopy and the ground's term, as path.h gives them.
  Wood w;
  Crown c{WorldPos{k_far + 200.0, 0.0, k_far}, 10.0f, 300.0f, CanopyClass::Blade, 1.2f};
  w.crowns.push_back(c);
  f64 level[2][8] = {};
  for (u32 on = 0; on < 2; ++on) {
    const Tunable path("audio.world.path", on == 0 ? "false" : "true");
    const Tunable ears("audio.world.ears_db", "-80");
    Listening s(w, 5.0, false);
    s.tick(10);
    for (u32 b = 0; b < 8; ++b)
      level[on][b] = s.db(WorldVoiceKind::Crown, b);
  }
  const f64 r3 = std::sqrt(200.0 * 200.0 + (10.0 - 1.7) * (10.0 - 1.7));
  // The crown's own bin (sector 4 of 16, the ring 192 to 256 m) is 300 m2 of leaf on 2,749 m2 of
  // ground, 3.1 % of a closed canopy: 8 m of the path's 64 in that ring is under it.
  const f64 bin = 3.14159265358979323846 * (256.0 * 256.0 - 192.0 * 192.0) / 16.0;
  const f64 under = (200.0 - 192.0) * (300.0 / bin) / 3.5;
  for (u32 b = 0; b < 8; ++b) {
    const f64 f = 62.5 * std::pow(2.0, static_cast<f64>(b));
    const f64 expect = air_absorption_db_per_m(f, 20.0, 70.0) * r3 + foliage_db(f, under) +
                       ground_db(b, 10.0, 1.7, 200.0, 1.0);
    CHECK(level[0][b] - level[1][b] == doctest::Approx(expect).epsilon(1e-3));
  }
  // The row's figures at 200 m: of the order of 15 dB at 8 kHz, about one at 1 kHz.
  CHECK(level[0][7] - level[1][7] > 15.0);
  CHECK(level[0][4] - level[1][4] < 2.0);
}

TEST_CASE("world path: a group's power is kept across its promotion from the beds to its crowns") {
  // A host's table sends a group far away as one merged crown at its leaves' centroid with its
  // radius as its extent, and near as its crowns one by one (engine-view's 128 m cells, promoted
  // when the group's near edge comes within `audio.world.near_m`). At that distance the world's
  // power must not step: the beds' merged crown against the same crowns sent one by one.
  const Tunable crowns("audio.world.crowns", "0");  // everything in the beds, both ways
  const Tunable ears("audio.world.ears_db", "-80");
  Wood one;
  Wood merged;
  f64 x = 0.0, z = 0.0, area = 0.0;
  f64 radius = 0.0;
  const f64 cx = 96.0 + 64.0;  // the group's centre: its near edge at near_m
  for (i32 i = -8; i < 8; ++i) {
    for (i32 j = -8; j < 8; ++j) {
      Crown c{WorldPos{k_far + cx + 8.0 * i + 4.0, 0.0, k_far + 8.0 * j + 4.0}, 10.0f, 224.0f,
              CanopyClass::Blade, 1.2f};
      c.extent_m = 3.0f;
      one.crowns.push_back(c);
      x += 224.0 * c.at.x;
      z += 224.0 * c.at.z;
      area += 224.0;
    }
  }
  for (const Crown& c : one.crowns) {
    const f64 r = std::hypot(c.at.x - x / area, c.at.z - z / area) + 3.0;
    radius = r > radius ? r : radius;
  }
  Crown m{WorldPos{x / area, 0.0, z / area}, 10.0f, static_cast<f32>(area), CanopyClass::Blade,
          1.2f};
  m.extent_m = static_cast<f32>(radius);
  m.merged = true;
  merged.crowns.push_back(m);
  f64 level[2] = {};
  for (u32 k = 0; k < 2; ++k) {
    Listening s(k == 0 ? one : merged, 5.0, false);
    s.tick(10);
    level[k] = s.db(WorldVoiceKind::Bed);
  }
  MESSAGE("one by one " << level[0] << " dB, merged " << level[1] << " dB SPL");
  CHECK(std::fabs(level[1] - level[0]) < 1.0);
}

TEST_CASE("world path: a far wood swells as the wind crosses it, less than one crown does") {
  // Ten minutes of a 3 m/s wind over the forest's roughness in 600 ticks: each crown of its own and
  // the beds summed, by their levels' 5th to 95th percentile spread.
  const Tunable ears("audio.world.ears_db", "-80");
  Listening s(wood(), 3.0, true);
  s.tick(30);
  std::vector<f64> bed;
  std::vector<f64> crown;
  for (u32 i = 0; i < 600; ++i) {
    s.tick(1, 1.0);
    bed.push_back(s.db(WorldVoiceKind::Bed));
    crown.push_back(s.db(WorldVoiceKind::Crown));
  }
  const f64 bed_spread = percentile(bed, 0.95) - percentile(bed, 0.05);
  const f64 crown_spread = percentile(crown, 0.95) - percentile(crown, 0.05);
  MESSAGE("5-95 % spread: beds " << bed_spread << " dB, crowns " << crown_spread << " dB");
  // The grounded figure (world-sound.md, "Read again by S114"): the natural ambient level's
  // standard deviation of 2.0 to 2.4 dB(A) Boersma and Etienne report, a 5th to 95th percentile
  // spread of 6.6 to 7.9 dB were it Gaussian; held here with a decibel either side, since their
  // quantity is the ambient's over intervals and not a bed's over a run. And a wide sector averages
  // its gusts, so the beds move less than the crowns of their own do.
  CHECK(bed_spread > 5.6);
  CHECK(bed_spread < 8.9);
  CHECK(bed_spread < crown_spread);
}

namespace {

// The mono sum's power in one of the shaped-noise voice's octave bands, dB full scale, after the
// first quarter second (the same band-pass the voice uses, audio::k_noise_band_table).
f64 band_power_db(const std::vector<f32>& stereo, u32 band) {
  const audio::NoiseBand& b = audio::k_noise_band_table[band];
  f64 s1 = 0.0, s2 = 0.0, sum = 0.0;
  u64 n = 0;
  for (usize f = 0; f + 1 < stereo.size(); f += 2) {
    const f64 x = 0.5 * (static_cast<f64>(stereo[f]) + static_cast<f64>(stereo[f + 1]));
    const f64 cx = static_cast<f64>(b.c) * x;
    const f64 y = cx + s1;
    s1 = s2 - static_cast<f64>(b.a1) * y;
    s2 = -cx - static_cast<f64>(b.a2) * y;
    if (f >= 24000u) {
      sum += y * y;
      ++n;
    }
  }
  return n != 0 && sum > 0.0 ? 10.0 * std::log10(sum / static_cast<f64>(n)) : -1000.0;
}

// A game's noise clip played once `distance_m` ahead of the listener, its null render's 8 kHz band
// over its 1 kHz band.
f64 clip_tilt(f64 distance_m, const std::string& wav) {
  scene::Sound block;
  scene::SoundClip c;
  c.name = "hiss";
  scene::SoundRecipe r;
  r.kind = scene::SoundRecipeKind::Noise;
  r.duration_s = 2.0f;
  r.seed = 7;
  c.recipe = r;
  c.min_m = 1.0f;
  c.max_m = 1000.0f;
  block.clips.push_back(c);
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  config.tick_hz = 60;
  config.wav_out = wav;
  REQUIRE(stage.open(config, &block, nullptr));
  const WorldPos ears{k_far, 1.7, k_far};
  stage.set_listener(ears, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
  const WorldPos at{k_far, 1.7, k_far - distance_m};
  u32 handle = 0;
  REQUIRE(stage.play(0, &at, 1.0f, handle) == Answer::Ok);
  for (u32 i = 0; i < 90; ++i)
    stage.tick();
  REQUIRE(stage.close(nullptr));
  std::string bytes;
  REQUIRE(io::read_file(wav, bytes) == io::Status::Ok);
  std::vector<f32> samples((bytes.size() - 44u) / 4u);
  std::memcpy(samples.data(), bytes.data() + 44, samples.size() * sizeof(f32));
  return band_power_db(samples, 7) - band_power_db(samples, 4);
}

}  // namespace

TEST_CASE("world path: a game's clip loses its top with distance, as the air takes it") {
  // The one-pole's corner is set so its loss at 8 kHz is the air's over the distance (0.0776 dB a
  // metre at 20 degC and 70 %): within half a decibel of it to 300 m (the corner is set for the
  // continuous one-pole; the sampled one takes 0.4 dB less), taking more than the air in the middle
  // bands beyond about 200 m (1.9 dB at 1 kHz at 200 m against the air's 1.0).
  for (const f32 d : {50.0f, 100.0f, 200.0f, 300.0f}) {
    const f32 air = static_cast<f32>(air_absorption_db_per_m(8000.0, 20.0, 70.0));
    const f64 a = static_cast<f64>(audio::air_lowpass_coefficient(d, air));
    const f64 om = 2.0 * 3.14159265358979323846 * 8000.0 / 48000.0;
    const f64 h2 = a * a / (1.0 - 2.0 * (1.0 - a) * std::cos(om) + (1.0 - a) * (1.0 - a));
    CHECK(std::fabs(-10.0 * std::log10(h2) - static_cast<f64>(d * air)) < 0.5);
  }
  CHECK(audio::air_lowpass_coefficient(0.0f, 0.0776f) == 1.0f);
  CHECK(audio::air_lowpass_coefficient(100.0f, 0.0f) == 1.0f);
  // Through the stage: the same noise at 5 m and at 200 m, the 8 kHz octave against the 1 kHz one
  // (the distance law takes both alike): 15.1 - 1.9 = 13.2 dB darker at the bands' centres, less
  // over the 8 kHz octave's lower half (11.3 measured); held between 10.5 and 14.
  const test::TempDir tmp("world_path_clip");
  const f64 near = clip_tilt(5.0, tmp.file("near.wav"));
  const f64 far = clip_tilt(200.0, tmp.file("far.wav"));
  MESSAGE("8 kHz over 1 kHz: at 5 m " << near << " dB, at 200 m " << far << " dB");
  CHECK(near - far > 10.5);
  CHECK(near - far < 14.0);
  // With the path off nothing is filtered: the same tilt at both distances.
  const Tunable path("audio.world.path", "false");
  CHECK(std::fabs(clip_tilt(200.0, tmp.file("far-off.wav")) - near) < 0.3);
}

namespace {

// A wall 2 m high and 40 m long across the wind, `at_m` along the wind from the listener (negative
// is upwind), and the open air everywhere else.
struct Wall {
  f64 at_m = 0.0;
  static f32 height_at(void* user, f64 x, f64 z, f32 across_x, f32 across_z) noexcept {
    const Wall& w = *static_cast<Wall*>(user);
    // Across the wind (toward +z) is along x.
    CHECK(std::fabs(std::fabs(across_x) - 1.0f) < 1e-5f);
    CHECK(std::fabs(across_z) < 1e-5f);
    // The case's wind blows toward +z: along the wind is z.
    const f64 along = z - k_far;
    return std::fabs(along - w.at_m) < 0.5 && std::fabs(x - k_far) < 20.0 ? 2.0f : 0.0f;
  }
};

f64 ears_db(const Wall* wall) {
  Wood none;
  Listening s(none, 8.0, false);
  s.wind.roughness_m = 0.03;
  if (wall != nullptr)
    s.stage.set_shelter(ShelterSource{const_cast<Wall*>(wall), &Wall::height_at});
  s.tick(10);
  return 10.0 * std::log10(s.power(WorldVoiceKind::EarLeft) + s.power(WorldVoiceKind::EarRight));
}

}  // namespace

TEST_CASE("world path: behind a wall the ears' wind drops by the lee's share") {
  // Open air, then a wall 2 m high 3 m upwind (1.5 heights: the near lee, 0.3 of the open wind),
  // then the same wall 3 m downwind (no lee at the listener) and 50 m upwind (past 24 heights:
  // the probes at 48 m find it, 0.88 of the wind).
  const f64 open = ears_db(nullptr);
  const Wall near{-3.0};
  const Wall behind{3.0};
  const Wall far{-48.0};
  const f64 lee = ears_db(&near);
  MESSAGE("ears: open " << open << " dB SPL, in the lee " << lee << ", 48 m upwind "
                        << ears_db(&far));
  // The ears' level goes as 40 log of the wind at the head: 40 log 0.3 = -20.9 dB.
  CHECK(lee - open == doctest::Approx(40.0 * std::log10(0.3)).epsilon(1e-3));
  CHECK(ears_db(&behind) == doctest::Approx(open).epsilon(1e-9));
  CHECK(ears_db(&far) - open ==
        doctest::Approx(40.0 * std::log10(0.8 + 0.2 * 4.0 / 10.0)).epsilon(1e-3));
}
