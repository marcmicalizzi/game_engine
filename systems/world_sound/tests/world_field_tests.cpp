// The wind in the trees and at the ears (roadmap S112; docs/subsystems/world_sound.md, "The wind in
// the trees and at the ears"): the laws the world's voices follow, held through the stage's own
// descriptions and through the null backend's samples, read by band. No case opens a device.

#include <core/memory/memory.h>
#include <domain/audio/audio.h>
#include <domain/audio/noise_voice.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

namespace {

// A grove of the case's own: a crown overhead, five near, and a ring of forty far ones that merge
// into the beds, round a listener 10,000 km out (the mixer's frame is the cell's corner).
constexpr f64 k_far = 1.0e7;

struct Grove {
  std::vector<Crown> crowns;
  static void visit(void* user, const WorldPos& centre, f64 reach_m, const CrownSink& sink) {
    for (const Crown& c : static_cast<Grove*>(user)->crowns) {
      const f64 dx = c.at.x - centre.x;
      const f64 dz = c.at.z - centre.z;
      if (dx * dx + dz * dz <= reach_m * reach_m) sink.emit(sink.user, c);
    }
  }
  CanopySource source() { return CanopySource{this, &Grove::visit}; }
};

Grove grove(CanopyClass near_kind = CanopyClass::Blade) {
  Grove g;
  g.crowns.push_back(Crown{WorldPos{k_far + 3.0, 0.0, k_far - 2.0}, 8.0f, 200.0f, near_kind, 1.0f});
  for (u32 i = 0; i < 5; ++i) {
    const f64 a = 1.3 * i;
    g.crowns.push_back(Crown{WorldPos{k_far + 15.0 * std::cos(a), 0.0, k_far + 15.0 * std::sin(a)},
                             10.0f, 150.0f, i % 2 == 0 ? CanopyClass::Needle : CanopyClass::Blade,
                             1.2f});
  }
  for (u32 i = 0; i < 40; ++i) {
    const f64 a = 0.157 * i;
    const f64 r = 80.0 + 3.0 * i;
    g.crowns.push_back(Crown{WorldPos{k_far + r * std::cos(a), 0.0, k_far + r * std::sin(a)}, 12.0f,
                             300.0f, i % 3 == 0 ? CanopyClass::Bare : CanopyClass::Needle, 1.0f});
  }
  return g;
}

struct Session {
  SoundStage stage;
  gfx::WindDesc wind;
  WorldWind frame;
  Grove trees;
  WorldPos ears{k_far, 1.7, k_far};
  Vec3 forward{0.0f, 0.0f, -1.0f};

  Session(f64 speed, bool turbulence, const std::string& wav = {},
          CanopyClass kind = CanopyClass::Blade)
      : trees(grove(kind)) {
    StageConfig config;
    config.mode = AudioMode::Null;
    config.world = true;
    config.turbulence = turbulence;
    config.tick_hz = 60;
    config.wav_out = wav;
    REQUIRE(stage.open(config, nullptr, nullptr));
    wind.kind = speed > 0.0 ? gfx::WindKind::fixed : gfx::WindKind::none;
    wind.speed_m_s = speed;
    wind.to_deg = 90.0;  // toward +z: from the listener's back, ahead along -z faces into it
    wind.roughness_m = 0.03;
    frame.scene = &wind;
    frame.sky_time_s = 1000.0;
    stage.set_canopy(trees.source());
  }
  void tick(u32 n = 1, f64 dt = 1.0 / 60.0) {
    for (u32 i = 0; i < n; ++i) {
      stage.set_listener(ears, forward, Vec3{0.0f, 1.0f, 0.0f});
      stage.set_wind(frame);
      stage.tick();
      frame.sky_time_s += dt;
    }
  }
  WorldVoiceInfo first(WorldVoiceKind kind) const {
    WorldVoiceInfo loudest;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      if (v.kind == kind && v.level_db > loudest.level_db) loudest = v;
    }
    return loudest;
  }
  f64 total_db(WorldVoiceKind kind) const {
    f64 p = 0.0;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      if (v.kind == kind && v.level_db > -999.0f)
        p += std::pow(10.0, static_cast<f64>(v.level_db) / 10.0);
    }
    return p > 0.0 ? 10.0 * std::log10(p) : -1000.0;
  }
};

}  // namespace

TEST_CASE("world sound: a bed rises 9 dB and the ears 12 dB a doubling of a fixed wind") {
  Session slow(4.0, false);
  Session fast(8.0, false);
  slow.tick(10);
  fast.tick(10);
  // The crowns, the beds and a crown of their own: 30 log 2 = 9.03 dB (Fegeant, through Bolin).
  CHECK(fast.total_db(WorldVoiceKind::Bed) - slow.total_db(WorldVoiceKind::Bed) ==
        doctest::Approx(9.031).epsilon(0.001));
  CHECK(fast.total_db(WorldVoiceKind::Crown) - slow.total_db(WorldVoiceKind::Crown) ==
        doctest::Approx(9.031).epsilon(0.001));
  // The ears, the pressure as the square of the speed: 40 log 2 = 12.04 dB (Korhonen).
  CHECK(fast.first(WorldVoiceKind::EarLeft).level_db -
            slow.first(WorldVoiceKind::EarLeft).level_db ==
        doctest::Approx(12.041).epsilon(0.001));
  // The beds are sounding, the counts inside their tunables.
  const WorldStats& w = fast.stage.world_stats();
  CHECK(w.crowns_seen == 46u);
  CHECK(w.crown_voices == 6u);
  CHECK(w.bed_voices >= 1u);
  CHECK(w.bed_voices <= 8u);
  CHECK(w.ear_voices == 2u);
}

TEST_CASE("world sound: the downwind ear is the louder, and turning the head turns it") {
  Session s(6.0, false);
  s.forward = Vec3{1.0f, 0.0f, 0.0f};  // facing +x: the wind, toward +z, blows to the right
  s.tick(3);
  const f32 left = s.first(WorldVoiceKind::EarLeft).level_db;
  const f32 right = s.first(WorldVoiceKind::EarRight).level_db;
  CHECK(right - left == doctest::Approx(8.0).epsilon(0.01));  // 2 x audio.world.ears_side_db
  s.forward = Vec3{-1.0f, 0.0f, 0.0f};                        // a half turn
  s.tick(3);
  CHECK(s.first(WorldVoiceKind::EarLeft).level_db - s.first(WorldVoiceKind::EarRight).level_db ==
        doctest::Approx(8.0).epsilon(0.01));
}

TEST_CASE("world sound: a live wind set between two ticks moves the level on the next") {
  Session s(4.0, true);
  s.tick(5);
  const f32 before = s.first(WorldVoiceKind::Crown).level_db;
  s.frame.live.set = true;
  s.frame.live.mean_m_s = 12.0;
  s.frame.live.to_deg = 90.0;
  s.tick(1);
  const f32 after = s.first(WorldVoiceKind::Crown).level_db;
  CHECK(after - before > 10.0f);  // 30 log 3 = 14.3 dB, less the turbulence's few dB either way
}

TEST_CASE(
    "world sound: a forced gust raises a bed by 30 log G along its envelope and lets it fall") {
  Session s(0.0, false);
  s.frame.live.set = true;
  s.frame.live.mean_m_s = 6.0;
  s.frame.live.to_deg = 90.0;
  s.tick(2);
  const f64 calm_gust = s.total_db(WorldVoiceKind::Bed);
  const f64 g = gfx::wind_gust_factor(0.03);  // 1.46 over grassland
  // Held: the phase rises over 10 s; the level follows mean (1 + level (G - 1)) cubed.
  f64 phase = 0.0;
  f64 last = calm_gust;
  for (u32 t = 0; t < 660; ++t) {
    phase = gfx::wind_gust_phase_step(phase, true, 1.0 / 60.0);
    s.frame.live.gust = gfx::wind_gust_level(phase);
    s.tick(1);
    const f64 now = s.total_db(WorldVoiceKind::Bed);
    CHECK(now >= last - 1e-9);
    last = now;
    if (t == 299) {  // halfway: the envelope's level at its phase
      const f64 expect = 30.0 * std::log10(1.0 + s.frame.live.gust * (g - 1.0));
      CHECK(now - calm_gust == doctest::Approx(expect).epsilon(0.002));
    }
  }
  CHECK(last - calm_gust == doctest::Approx(30.0 * std::log10(g)).epsilon(0.002));  // 4.9 dB
  CHECK(30.0 * std::log10(g) == doctest::Approx(4.9).epsilon(0.01));
  // Released: it dies the same way, back to the mean's level.
  for (u32 t = 0; t < 660; ++t) {
    phase = gfx::wind_gust_phase_step(phase, false, 1.0 / 60.0);
    s.frame.live.gust = gfx::wind_gust_level(phase);
    s.tick(1);
    const f64 now = s.total_db(WorldVoiceKind::Bed);
    CHECK(now <= last + 1e-9);
    last = now;
  }
  CHECK(last == doctest::Approx(calm_gust).epsilon(1e-6));
}

TEST_CASE("world sound: a gust's front raises a crown on the tick it raises the sway's strength") {
  // The sway's strength under a live wind is (gust + forced) / 5 m/s at the place it asks
  // (renderer::scene_flora_wind); the stage asks the same arguments at each crown's place, so the
  // tick the forced gust's level first moves is the tick the crown's level moves.
  Session s(0.0, true);
  s.frame.live.set = true;
  s.frame.live.mean_m_s = 6.0;
  s.frame.live.to_deg = 90.0;
  s.tick(3, 0.0);  // the sky's clock held: only the gust moves
  const f32 before = s.first(WorldVoiceKind::Crown).level_db;
  const f32 wind_before = s.first(WorldVoiceKind::Crown).wind_m_s;
  s.frame.live.gust = gfx::wind_gust_level(gfx::wind_gust_phase_step(0.0, true, 0.5));
  s.tick(1, 0.0);
  const WorldVoiceInfo after = s.first(WorldVoiceKind::Crown);
  CHECK(after.level_db > before);
  CHECK(after.wind_m_s > wind_before);
}

TEST_CASE("world sound: a needle crown's movable band sits at 0.2 U / d, a blade crown has none") {
  Session needle(6.0, false, {}, CanopyClass::Needle);
  needle.tick(3);
  bool seen_needle = false;
  bool seen_blade = false;
  for (u32 i = 0; i < needle.stage.world_voice_count(); ++i) {
    const WorldVoiceInfo v = needle.stage.world_voice(i);
    if (v.kind != WorldVoiceKind::Crown || !v.sounding) continue;
    if (v.tone_hz > 0.0f) {
      // Needles of 1 mm (the overhead crown) or 1.2 mm (the near ring): f = 0.2 U / d.
      const f64 wind = static_cast<f64>(v.wind_m_s);
      const f64 tone = static_cast<f64>(v.tone_hz);
      const f64 f1 = 0.2 * wind / 0.001;
      const f64 f12 = 0.2 * wind / 0.0012;
      CHECK((std::fabs(tone - f1) < 1e-3 * f1 || std::fabs(tone - f12) < 1e-3 * f12));
      CHECK(v.band_db[audio::k_noise_tone] > -100.0f);
      seen_needle = true;
    } else {
      CHECK(v.band_db[audio::k_noise_tone] < -999.0f);
      seen_blade = true;
    }
  }
  CHECK(seen_needle);
  CHECK(seen_blade);
}

TEST_CASE("world sound: an unchanged world sends nothing, calm holds no voice, counts are capped") {
  Session s(6.0, true);
  s.tick(2, 0.0);  // the sky's clock held: the same wind every tick
  const u64 after_start = s.stage.world_stats().commands;
  CHECK(after_start > 0u);
  s.tick(120, 0.0);  // four refreshes of the same crowns
  CHECK(s.stage.world_stats().commands == after_start);

  Session calm(0.0, true);
  calm.tick(120);
  CHECK(calm.stage.world_stats().commands == 0u);
  CHECK(calm.stage.world_stats().crown_voices == 0u);
  CHECK(calm.stage.world_stats().ear_voices == 0u);
  CHECK(calm.stage.world_stats().crowns_seen == 46u);

  // The wind dropping to calm stops every voice, and they stay stopped.
  s.frame.live.set = true;
  s.frame.live.mean_m_s = 0.0;
  s.tick(2);
  const WorldStats& w = s.stage.world_stats();
  CHECK(w.crown_voices == 0u);
  CHECK(w.bed_voices == 0u);
  CHECK(w.ear_voices == 0u);
  CHECK(w.stops == w.plays);
}

TEST_CASE("world sound: the counts never pass their tunables") {
  tunables::Tunable* crowns = tunables::find("audio.world.crowns");
  tunables::Tunable* beds = tunables::find("audio.world.beds");
  REQUIRE(crowns != nullptr);
  REQUIRE(beds != nullptr);
  REQUIRE(crowns->set_from_text("2"));
  REQUIRE(beds->set_from_text("3"));
  {
    Session s(8.0, true);
    s.tick(60);
    const WorldStats& w = s.stage.world_stats();
    CHECK(w.crown_voices == 2u);
    CHECK(w.bed_voices <= 3u);
    CHECK(w.ear_voices == 2u);
    CHECK(s.stage.world_voice_count() == 2u + 3u + 2u);
  }
  crowns->reset();
  beds->reset();
}

TEST_CASE("world sound: 2,400 ticks of wind in a grove allocate nothing") {
  Session s(7.0, true);
  s.tick(30);  // the voices started and the first refreshes done
  const u64 before = mem::allocations_on_thread();
  s.tick(2400);
  CHECK(mem::allocations_on_thread() - before == 0u);
}

namespace {

// The WAV's samples, stereo float after its 44-byte header.
std::vector<f32> read_wav(const std::string& path) {
  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  REQUIRE(bytes.size() >= 44u);
  std::vector<f32> out((bytes.size() - 44u) / 4u);
  std::memcpy(out.data(), bytes.data() + 44, out.size() * sizeof(f32));
  return out;
}

// The mono sum's power in one of the voice's own octave bands, in dB full scale, after the first
// half second.
f64 band_db(const std::vector<f32>& stereo, u32 band) {
  const audio::NoiseBand& b = audio::k_noise_band_table[band];
  f64 s1 = 0.0;
  f64 s2 = 0.0;
  f64 sum = 0.0;
  u64 n = 0;
  for (usize f = 0; f + 1 < stereo.size(); f += 2) {
    const f64 x = 0.5 * (static_cast<f64>(stereo[f]) + static_cast<f64>(stereo[f + 1]));
    const f64 cx = static_cast<f64>(b.c) * x;
    const f64 y = cx + s1;
    s1 = s2 - static_cast<f64>(b.a1) * y;
    s2 = -cx - static_cast<f64>(b.a2) * y;
    if (f >= 48000u) {
      sum += y * y;
      ++n;
    }
  }
  return n != 0 && sum > 0.0 ? 10.0 * std::log10(sum / static_cast<f64>(n)) : -1000.0;
}

f64 wav_db(const std::vector<f32>& stereo) {
  f64 sum = 0.0;
  for (usize i = 96000u; i < stereo.size(); ++i)
    sum += static_cast<f64>(stereo[i]) * static_cast<f64>(stereo[i]);
  return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<f64>(stereo.size() - 96000u)) : -1000.0;
}

u64 render(f64 speed, const std::string& wav) {
  Session s(speed, true, wav);
  s.tick(180);  // three seconds
  std::string error;
  CHECK(s.stage.close(&error));
  return s.stage.render_hash();
}

}  // namespace

TEST_CASE("world sound: the null render, read by band, is louder and brighter with the wind") {
  // The trees alone: the ears' own wind grows 12 dB a doubling in its low bands and is held to its
  // law by the cases above, so here it is turned down to the tunable's floor.
  tunables::Tunable* ears = tunables::find("audio.world.ears_db");
  REQUIRE(ears != nullptr);
  REQUIRE(ears->set_from_text("-80"));
  const test::TempDir tmp("world_sound_field");
  const std::string calm_wav = tmp.file("calm.wav");
  const std::string light_wav = tmp.file("light.wav");
  const std::string strong_wav = tmp.file("strong.wav");
  const u64 light = render(3.0, light_wav);
  CHECK(render(3.0, tmp.file("light2.wav")) == light);  // the same bytes twice on one compiler
  render(9.0, strong_wav);
  render(0.0, calm_wav);
  const std::vector<f32> a = read_wav(light_wav);
  const std::vector<f32> b = read_wav(strong_wav);
  const std::vector<f32> c = read_wav(calm_wav);
  REQUIRE(a.size() == 3u * 48000u * 2u);
  REQUIRE(b.size() == a.size());
  // Silent in a calm: not a sample above zero.
  bool silent = true;
  for (const f32 x : c)
    silent = silent && x == 0.0f;
  CHECK(silent);
  // Louder by the trees' 30 log 3 (14.3 dB), give or take the turbulence over three seconds.
  const f64 rise = wav_db(b) - wav_db(a);
  MESSAGE("light " << wav_db(a) << " dBFS, strong " << wav_db(b) << " dBFS");
  CHECK(rise > 12.0);
  CHECK(rise < 17.0);
  // Brighter: the needles' tone climbs from near 500 Hz to near 1.4 kHz (0.2 U / d), so the 2 kHz
  // band gains on the 500 Hz band.
  const f64 tilt_light = band_db(a, 5) - band_db(a, 3);
  const f64 tilt_strong = band_db(b, 5) - band_db(b, 3);
  MESSAGE("2 kHz over 500 Hz: light " << tilt_light << " dB, strong " << tilt_strong << " dB");
  CHECK(tilt_strong > tilt_light + 2.0);
  ears->reset();
}

namespace {

// The grove sent far first: the order a canopy table may well have (S112's review, M1).
struct Reversed {
  std::vector<Crown> crowns;
  static void visit(void* user, const WorldPos&, f64, const CrownSink& sink) {
    const std::vector<Crown>& c = static_cast<Reversed*>(user)->crowns;
    for (usize i = c.size(); i-- > 0;)
      sink.emit(sink.user, c[i]);
  }
};

}  // namespace

TEST_CASE(
    "world sound: a full canopy table keeps the nearest crowns, whatever order they come in") {
  tunables::Tunable* max_crowns = tunables::find("audio.world.max_crowns");
  REQUIRE(max_crowns != nullptr);
  REQUIRE(max_crowns->set_from_text("8"));
  f64 loudest[2] = {};
  for (u32 order = 0; order < 2; ++order) {
    Session s(8.0, false);
    Reversed reversed{s.trees.crowns};
    if (order == 1) s.stage.set_canopy(CanopySource{&reversed, &Reversed::visit});
    s.tick(30);
    const WorldStats& w = s.stage.world_stats();
    CHECK(w.crowns_seen == 8u);
    CHECK(w.crowns_dropped == s.trees.crowns.size() - 8u);
    loudest[order] = static_cast<f64>(s.first(WorldVoiceKind::Crown).level_db);
  }
  // Sent nearest last, the crown overhead is still kept and still the loudest voice.
  CHECK(loudest[1] == doctest::Approx(loudest[0]).epsilon(1e-9));
  max_crowns->reset();
}

TEST_CASE(
    "world sound: a merged group is the beds', and a crown the listener stands in is heard "
    "at its edge") {
  for (const bool merged : {false, true}) {
    Session s(8.0, false);
    s.trees.crowns.clear();
    Crown c{WorldPos{k_far + 1.0, 0.0, k_far}, 1.7f, 400.0f, CanopyClass::Blade, 1.2f};
    c.merged = merged;
    s.trees.crowns.push_back(c);
    s.tick(10);
    CHECK(s.stage.world_stats().crown_voices == (merged ? 0u : 1u));
    CHECK(s.stage.world_stats().bed_voices == (merged ? 1u : 0u));
  }
  // The same crown all round the listener with an extent of 50 m against none: heard as at 50 m
  // rather than at the 2 m least, 20 log 25 = 27.96 dB under.
  f64 level[2] = {};
  for (u32 i = 0; i < 2; ++i) {
    Session s(8.0, false);
    s.trees.crowns.clear();
    Crown c{WorldPos{k_far, 0.0, k_far}, 1.7f, 400.0f, CanopyClass::Blade, 1.2f};
    c.extent_m = i == 0 ? 0.0f : 50.0f;
    s.trees.crowns.push_back(c);
    s.tick(10);
    level[i] = static_cast<f64>(s.first(WorldVoiceKind::Crown).level_db);
  }
  CHECK(level[0] - level[1] == doctest::Approx(20.0 * std::log10(25.0)).epsilon(1e-4));
}

TEST_CASE("world sound: the world is held at its ceiling, counted, and under it is not touched") {
  // The case's grove is sparse: at 45 m/s it sums to about -13 dBFS, just under the default -12,
  // so the ceiling is brought down to -20 for the gale and left there for the breeze.
  tunables::Tunable* ceiling = tunables::find("audio.world.ceiling_db");
  REQUIRE(ceiling != nullptr);
  REQUIRE(ceiling->set_from_text("-20"));
  Session gale(45.0, false);
  gale.tick(60);
  const WorldStats& g = gale.stage.world_stats();
  CHECK(g.held_ticks > 0u);
  CHECK(g.held_db < 0.0f);
  f64 total = 0.0;
  for (u32 i = 0; i < gale.stage.world_voice_count(); ++i) {
    const WorldVoiceInfo v = gale.stage.world_voice(i);
    if (v.level_db > -999.0f) total += std::pow(10.0, static_cast<f64>(v.level_db) / 10.0);
  }
  MESSAGE("gale: held " << g.held_db << " dB, the world at " << 10.0 * std::log10(total)
                        << " dB SPL on the 105 dB scale");
  CHECK(10.0 * std::log10(total) <= 105.0 - 20.0 + 0.01);
  Session breeze(8.0, false);
  breeze.tick(60);
  CHECK(breeze.stage.world_stats().held_ticks == 0u);
  CHECK(breeze.stage.world_stats().held_db == 0.0f);
  ceiling->reset();
}
