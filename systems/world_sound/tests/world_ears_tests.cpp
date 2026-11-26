// The wind at the ears as it is heard (roadmap S121; docs/subsystems/world_sound.md, "The wind at
// the ears (S121)"; the grounding, docs/experiments/world-sound.md, "Read again by S121"): a
// stander, a walker at 1.5 m/s and a runner at 5 m/s, in still air and in a 3 m/s wind, each
// rendered by the null backend at engine-view's 240 ticks a second and read from the WAV: the
// level against the law, where the power lies, how it comes and goes over half seconds, and what
// the two ears share. No case opens a device.

#include <domain/audio/noise_voice.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

namespace {

constexpr f64 k_far = 1.0e7;
constexpr u32 k_tick_hz = 240;
constexpr f64 k_pi = 3.14159265358979323846;

struct Tunable {
  tunables::Tunable* t;
  Tunable(const char* name, const char* value) : t(tunables::find(name)) {
    REQUIRE(t != nullptr);
    REQUIRE(t->set_from_text(value));
  }
  ~Tunable() { t->reset(); }
  Tunable(const Tunable&) = delete;
  Tunable& operator=(const Tunable&) = delete;
};

// Crowns either side of the runner's path, every 8 m for 64 m along -z: a light wood to run under.
struct Avenue {
  std::vector<Crown> crowns;
  Avenue() {
    for (u32 i = 0; i < 9; ++i) {
      for (const f64 side : {-6.0, 6.0}) {
        crowns.push_back(Crown{WorldPos{k_far + side, 0.0, k_far + 4.0 - 8.0 * i}, 8.0f, 150.0f,
                               CanopyClass::Blade, 1.0f});
      }
    }
  }
  static void visit(void* user, const WorldPos& centre, f64 reach_m, const CrownSink& sink) {
    for (const Crown& c : static_cast<Avenue*>(user)->crowns) {
      const f64 dx = c.at.x - centre.x;
      const f64 dz = c.at.z - centre.z;
      if (dx * dx + dz * dz <= reach_m * reach_m) sink.emit(sink.user, c);
    }
  }
};

// One listening run: a listener moving along `heading` (and facing it) at `pace_m_s` for
// `seconds` from `start`, in a fixed wind of `wind_m_s` 10 m up blowing toward +z over grass.
struct Run {
  f64 wind_m_s = 0.0;
  f64 pace_m_s = 0.0;
  f64 seconds = 0.0;
  Vec3 heading{0.0f, 0.0f, -1.0f};
  WorldPos start{k_far, 1.7, k_far};
  f64 world_time_s = 1000.0;
  bool turbulence = true;
  u32 tick_hz = k_tick_hz;
  Avenue* trees = nullptr;
  WorldStats* stats = nullptr;       // the world's counts at the end
  std::vector<f64>* held = nullptr;  // the world's hold each tick, dB
};

u64 listen(const Run& r, const std::string& wav) {
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  config.world = true;
  config.turbulence = r.turbulence;
  config.tick_hz = r.tick_hz;
  config.wav_out = wav;
  REQUIRE(stage.open(config, nullptr, nullptr));
  gfx::WindDesc wind;
  wind.kind = r.wind_m_s > 0.0 ? gfx::WindKind::fixed : gfx::WindKind::none;
  wind.speed_m_s = r.wind_m_s;
  wind.to_deg = 90.0;
  wind.roughness_m = 0.03;
  WorldWind frame;
  frame.scene = &wind;
  frame.sky_time_s = r.world_time_s;
  frame.world_time_s = r.world_time_s;
  if (r.trees != nullptr) stage.set_canopy(CanopySource{r.trees, &Avenue::visit});
  WorldPos at = r.start;
  const u32 ticks = static_cast<u32>(r.seconds * r.tick_hz);
  for (u32 i = 0; i < ticks; ++i) {
    at.x += static_cast<f64>(r.heading.x) * r.pace_m_s / r.tick_hz;
    at.z += static_cast<f64>(r.heading.z) * r.pace_m_s / r.tick_hz;
    stage.set_listener(at, r.heading, Vec3{0.0f, 1.0f, 0.0f});
    stage.set_wind(frame);
    stage.tick();
    if (r.held != nullptr) r.held->push_back(static_cast<f64>(stage.world_stats().held_db));
    frame.sky_time_s += 1.0 / r.tick_hz;
    frame.world_time_s += 1.0 / r.tick_hz;
  }
  if (r.stats != nullptr) *r.stats = stage.world_stats();
  std::string error;
  CHECK(stage.close(&error));
  return stage.render_hash();
}

// One listener moving along -z (facing it) at `pace_m_s` for `seconds`, in a fixed wind of
// `wind_m_s` 10 m up blowing toward +z (into the listener's face) over grass, the WAV kept.
u64 listen(f64 wind_m_s, f64 pace_m_s, f64 seconds, const std::string& wav,
           Avenue* trees = nullptr) {
  Run r;
  r.wind_m_s = wind_m_s;
  r.pace_m_s = pace_m_s;
  r.seconds = seconds;
  r.trees = trees;
  return listen(r, wav);
}

std::vector<f32> read_wav(const std::string& path) {
  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  REQUIRE(bytes.size() >= 44u);
  std::vector<f32> out((bytes.size() - 44u) / 4u);
  std::memcpy(out.data(), bytes.data() + 44, out.size() * sizeof(f32));
  return out;
}

void fft(std::vector<std::complex<f64>>& a) {
  const usize n = a.size();
  for (usize i = 1, j = 0; i < n; ++i) {
    usize bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (usize len = 2; len <= n; len <<= 1) {
    const f64 ang = -2.0 * k_pi / static_cast<f64>(len);
    const std::complex<f64> wl(std::cos(ang), std::sin(ang));
    for (usize i = 0; i < n; i += len) {
      std::complex<f64> w(1.0, 0.0);
      for (usize j = 0; j < len / 2; ++j) {
        const std::complex<f64> u = a[i + j];
        const std::complex<f64> v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// What a listening run reads of one WAV, after its first half second (the voices' ramp up).
struct Heard {
  f64 level_db = -1000.0;  // the two ears' mean power, dB SPL on the stage's 105 dB scale
  f64 left_db = -1000.0;   // each ear's
  f64 right_db = -1000.0;
  f64 octave_db[8] = {};    // the mean power by octave, 62.5 Hz to 8 kHz (an FFT's bins)
  f64 under_300 = 0.0;      // the share of the power under 300 Hz
  f64 spread_db = 0.0;      // the 5th to 95th percentile of the half-second levels
  f64 correlation = 0.0;    // the two ears' samples
  f64 envelope_corr = 0.0;  // the two ears' quarter-second levels, in dB
};

f64 percentile(std::vector<f64> v, f64 p) {
  std::sort(v.begin(), v.end());
  const f64 x = p * static_cast<f64>(v.size() - 1);
  const usize i = static_cast<usize>(x);
  const f64 t = x - static_cast<f64>(i);
  return i + 1 < v.size() ? v[i] * (1.0 - t) + v[i + 1] * t : v[i];
}

f64 pearson(const std::vector<f64>& a, const std::vector<f64>& b) {
  f64 ma = 0.0, mb = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<f64>(a.size());
  mb /= static_cast<f64>(b.size());
  f64 ab = 0.0, aa = 0.0, bb = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    ab += (a[i] - ma) * (b[i] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i] - mb) * (b[i] - mb);
  }
  return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
}

Heard hear(const std::vector<f32>& stereo) {
  Heard h;
  const usize skip = 24000u;  // frames
  const usize frames = stereo.size() / 2u;
  REQUIRE(frames > skip + 48000u);
  std::vector<f64> left(frames - skip), right(frames - skip);
  for (usize f = skip; f < frames; ++f) {
    left[f - skip] = static_cast<f64>(stereo[2 * f]);
    right[f - skip] = static_cast<f64>(stereo[2 * f + 1]);
  }
  const usize n = left.size();
  f64 pl = 0.0, pr = 0.0, lr = 0.0;
  for (usize i = 0; i < n; ++i) {
    pl += left[i] * left[i];
    pr += right[i] * right[i];
    lr += left[i] * right[i];
  }
  const f64 power = 0.5 * (pl + pr) / static_cast<f64>(n);
  if (!(power > 0.0)) return h;
  h.level_db = 105.0 + 10.0 * std::log10(power);
  h.left_db = 105.0 + 10.0 * std::log10(pl / static_cast<f64>(n) + 1e-30);
  h.right_db = 105.0 + 10.0 * std::log10(pr / static_cast<f64>(n) + 1e-30);
  h.correlation = lr / std::sqrt(pl * pr);
  // Welch: 8,192-point Hann frames, half overlapped, each ear's power spectrum averaged.
  constexpr usize k_n = 8192;
  std::vector<f64> spectrum(k_n / 2 + 1, 0.0);
  std::vector<std::complex<f64>> buf(k_n);
  for (const std::vector<f64>* ch : {&left, &right}) {
    for (usize at = 0; at + k_n <= n; at += k_n / 2) {
      for (usize i = 0; i < k_n; ++i) {
        const f64 w = 0.5 - 0.5 * std::cos(2.0 * k_pi * static_cast<f64>(i) / k_n);
        buf[i] = std::complex<f64>((*ch)[at + i] * w, 0.0);
      }
      fft(buf);
      for (usize k = 0; k <= k_n / 2; ++k)
        spectrum[k] += std::norm(buf[k]);
    }
  }
  f64 total = 0.0, low = 0.0;
  for (usize k = 1; k <= k_n / 2; ++k) {
    const f64 f = 48000.0 * static_cast<f64>(k) / k_n;
    total += spectrum[k];
    if (f < 300.0) low += spectrum[k];
    for (u32 b = 0; b < 8; ++b) {
      const f64 c = static_cast<f64>(audio::k_noise_band_hz[b]);
      if (f >= c / std::sqrt(2.0) && f < c * std::sqrt(2.0)) h.octave_db[b] += spectrum[k];
    }
  }
  h.under_300 = low / total;
  for (f64& o : h.octave_db)
    o = h.level_db + (o > 0.0 ? 10.0 * std::log10(o / total) : -1000.0);
  // Half seconds of both ears' power; quarter seconds of each ear's.
  std::vector<f64> halves;
  for (usize at = 0; at + 24000u <= n; at += 24000u) {
    f64 p = 0.0;
    for (usize i = at; i < at + 24000u; ++i)
      p += left[i] * left[i] + right[i] * right[i];
    halves.push_back(10.0 * std::log10(p / 48000.0 + 1e-30));
  }
  h.spread_db = percentile(halves, 0.95) - percentile(halves, 0.05);
  std::vector<f64> ql, qr;
  for (usize at = 0; at + 12000u <= n; at += 12000u) {
    f64 a = 0.0, b = 0.0;
    for (usize i = at; i < at + 12000u; ++i) {
      a += left[i] * left[i];
      b += right[i] * right[i];
    }
    ql.push_back(10.0 * std::log10(a + 1e-30));
    qr.push_back(10.0 * std::log10(b + 1e-30));
  }
  h.envelope_corr = pearson(ql, qr);
  return h;
}

// The law at a speed past the head of `rel`, one ear, the head facing it: Seidman's anchor, 40 log
// of the speed (Korhonen) and `audio.world.ears_db`.
f64 law_db(f64 rel) {
  tunables::Tunable* t = tunables::find("audio.world.ears_db");
  REQUIRE(t != nullptr);
  std::string text;
  t->append_value(text);
  return 84.9 + 40.0 * std::log10(rel / 4.5) + std::stod(text);
}

// The wind 1.7 m up over grass (z0 0.03) for a wind of `u10` 10 m up: the neutral log law.
f64 head_wind(f64 u10) { return u10 * std::log(1.7 / 0.03) / std::log(10.0 / 0.03); }

void print(const char* what, const Heard& h) {
  MESSAGE(what << ": " << h.level_db << " dB SPL; octaves 62.5 Hz-8 kHz " << h.octave_db[0] << " "
               << h.octave_db[1] << " " << h.octave_db[2] << " " << h.octave_db[3] << " "
               << h.octave_db[4] << " " << h.octave_db[5] << " " << h.octave_db[6] << " "
               << h.octave_db[7] << "; under 300 Hz " << h.under_300 << "; half-second spread "
               << h.spread_db << " dB; ears' correlation " << h.correlation
               << ", their quarter-second levels' " << h.envelope_corr);
}

}  // namespace

TEST_CASE("world ears: in still air a stander hears nothing, a walker and a runner their own") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_still");
  listen(0.0, 0.0, 3.0, tmp.file("stand.wav"));
  const std::vector<f32> stand = read_wav(tmp.file("stand.wav"));
  CHECK(std::all_of(stand.begin(), stand.end(), [](f32 x) { return x == 0.0f; }));

  const u64 walk_hash = listen(0.0, 1.5, 12.0, tmp.file("walk.wav"));
  CHECK(listen(0.0, 1.5, 12.0, tmp.file("walk2.wav")) == walk_hash);  // the same bytes twice
  listen(0.0, 5.0, 12.0, tmp.file("run.wav"));
  const Heard walk = hear(read_wav(tmp.file("walk.wav")));
  const Heard run = hear(read_wav(tmp.file("run.wav")));
  print("walking 1.5 m/s, still air", walk);
  print("running 5 m/s, still air", run);
  // The law at the pace within a decibel: the buffeting's power is one on average (its mean over
  // these 11.5 s varies by a few tenths of a decibel, a Weibull draw a 20 ms frame), and the voice
  // is its described power within 0.75 dB (the low-pass's account, computed on the grounding page).
  CHECK(std::fabs(walk.level_db - law_db(1.5)) < 1.0);
  CHECK(std::fabs(run.level_db - law_db(5.0)) < 1.0);
  // A sprint stands 40 log (5 / 1.5) = 20.9 dB over a walk.
  CHECK(std::fabs((run.level_db - walk.level_db) - 40.0 * std::log10(5.0 / 1.5)) < 1.0);
  // A rumble: most of a runner's own wind under 300 Hz, and nothing of a waterfall's top. S121's
  // refit to Nelke's published coefficients (Table 3.3) moved both pins: that spectrum is brighter
  // than the one first read (the most at 125 to 500 Hz, 8 dB an octave above, 0.77 of its power
  // under 300 Hz at 4.5 m/s), so the run reads 0.66 under 300 Hz where it read 0.87, and its 4 kHz
  // octave 38 dB under the loudest where it read over 40. The share under 300 Hz no longer tells
  // this voice from S112's roar (0.57): the 4 kHz pin does (S112's roar was 11 dB down). That pin
  // holds the low-pass's overshoot as well as the shape: the shape itself is about 27 dB down at
  // 4 kHz at 5 m/s, and the corner was fitted from 62.5 Hz to 1 kHz only, which leaves the voice 5
  // to 26 dB under the shape from 2 to 8 kHz; a voice exactly on Table 3.3 would read about 27.
  CHECK(run.under_300 > 0.6);
  CHECK(walk.under_300 > 0.9);
  const f64 loudest = *std::max_element(run.octave_db, run.octave_db + 8);
  CHECK(loudest - run.octave_db[6] > 30.0);
  // It comes and goes with the head's own turbulence (each ear's own: Nelke's short-term gain, a
  // Weibull draw a 20 ms frame), and the two ears share neither their samples nor their buffeting:
  // two washes, one at each ear. The spread is held against the same run with the buffeting off
  // (still air, so nothing else moves): what the buffeting adds, not a figure that sits on an edge.
  Run steady;
  steady.pace_m_s = 5.0;
  steady.seconds = 12.0;
  steady.turbulence = false;
  listen(steady, tmp.file("steady.wav"));
  const Heard calm = hear(read_wav(tmp.file("steady.wav")));
  MESSAGE("half-second spread with the buffeting " << run.spread_db << " dB, without "
                                                   << calm.spread_db << " dB");
  // Read: 1.16 against 0.48 dB (a Weibull of shape 2.71 moves a gain less than the Gaussian's
  // square did).
  CHECK(run.spread_db > calm.spread_db + 0.3);
  CHECK(run.spread_db < 8.0);
  CHECK(std::fabs(run.correlation) < 0.05);
  CHECK(std::fabs(run.envelope_corr) < 0.5);
}

TEST_CASE("world ears: in a 3 m/s wind the stander hears the gust come and go in both ears") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_wind");
  listen(3.0, 0.0, 60.0, tmp.file("stand.wav"));
  listen(3.0, 1.5, 12.0, tmp.file("walk.wav"));
  listen(3.0, 5.0, 12.0, tmp.file("run.wav"));
  const Heard stand = hear(read_wav(tmp.file("stand.wav")));
  const Heard walk = hear(read_wav(tmp.file("walk.wav")));
  const Heard run = hear(read_wav(tmp.file("run.wav")));
  print("standing, 3 m/s", stand);
  print("walking 1.5 m/s into 3 m/s", walk);
  print("running 5 m/s into 3 m/s", run);
  const f64 u = head_wind(3.0);
  // The law at the mean wind past the head, give or take the turbulence: the mean of the fourth
  // power of a turbulent speed stands over the fourth power of its mean. Since S121's Weibull
  // buffeting the walker reads 2.01 dB over the law (the Gaussian's read 1.9), so its bound is 2.5.
  CHECK(std::fabs(stand.level_db - law_db(u)) < 2.0);
  CHECK(std::fabs(walk.level_db - law_db(u + 1.5)) < 2.5);
  CHECK(std::fabs(run.level_db - law_db(u + 5.0)) < 2.0);
  CHECK(run.level_db > walk.level_db);
  CHECK(walk.level_db > stand.level_db);
  // The stander's wind is the gust's: it comes and goes over seconds, more than a runner's own
  // buffeting does, and the two ears share it (the gust and the turbulence at the head are one
  // flow) under each ear's own buffeting, which they do not.
  CHECK(stand.spread_db > 4.0);
  CHECK(stand.spread_db > run.spread_db + 1.0);
  CHECK(stand.envelope_corr > 0.2);
  CHECK(stand.envelope_corr > run.envelope_corr + 0.2);
  CHECK(std::fabs(stand.correlation) < 0.05);
  CHECK(stand.under_300 > 0.9);
}

TEST_CASE(
    "world ears: a runner in a light wind under crowns: the low end the ears', the top "
    "the leaves'") {
  const test::TempDir tmp("world_ears_crowns");
  Avenue trees;
  {
    const Tunable solo("audio.world.solo", "3");
    listen(3.0, 5.0, 6.0, tmp.file("ears.wav"), &trees);
  }
  {
    const Tunable solo("audio.world.solo", "1");
    listen(3.0, 5.0, 6.0, tmp.file("crowns.wav"), &trees);
  }
  listen(3.0, 5.0, 6.0, tmp.file("all.wav"), &trees);
  const Heard ears = hear(read_wav(tmp.file("ears.wav")));
  const Heard crowns = hear(read_wav(tmp.file("crowns.wav")));
  const Heard all = hear(read_wav(tmp.file("all.wav")));
  print("the runner's ears", ears);
  print("the crowns round the runner", crowns);
  print("the whole world", all);
  // The ears lead the mix, as a runner's own wind does, and lead it where it is low...
  CHECK(ears.level_db > crowns.level_db);
  CHECK(ears.octave_db[1] > crowns.octave_db[1] + 10.0);
  // ... while the leaves are heard over them in the top octaves, where a waterfall's hiss would
  // have buried them. S121's refit spectrum (8 dB an octave above 500 Hz where the first read 11)
  // raised the ears' 4 kHz octave by about 9 dB, and the default gain that keeps a run's A-weighted
  // level at S112's (-26 dB) took 9.5 back: the crowns lead by 12.8 dB at 4 kHz and 27 at 8.
  CHECK(crowns.octave_db[6] > ears.octave_db[6] + 10.0);
  CHECK(crowns.octave_db[7] > ears.octave_db[7] + 10.0);
  CHECK(all.level_db > ears.level_db);
}

// The floor on the mean (S121): the floor is decided on an ear's mean, not on its buffeting, so an
// ear that is heard stays heard through its dips: each ear started once and never stopped, for the
// whole walk and the whole run (before the mend a walker's ear was stopped and replayed from its
// seed about twice a second).
TEST_CASE("world ears: a heard ear stays heard through its buffeting: two plays and no stop") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_floor");
  for (const f64 pace : {1.5, 5.0}) {
    Run r;
    r.pace_m_s = pace;
    r.seconds = 12.0;
    WorldStats stats;
    r.stats = &stats;
    listen(r, tmp.file("walk.wav"));
    MESSAGE("still air at " << pace << " m/s: " << stats.plays << " plays, " << stats.stops
                            << " stops");
    CHECK(stats.plays == 2u);
    CHECK(stats.stops == 0u);
  }
}

// The same anywhere (S121): the head's turbulence is carried past the listener by the head's mean
// speed and its clock holds the distance travelled through the air, never a world coordinate. The
// same minute's stander at the world's origin and 5 km down the wind (the gust's own clock moved by
// the gust's travel time, 5,000 / 3 s, so the two stand in the same gust): the same spread of the
// half-second levels and the same share between the ears, within what the two runs' own buffeting
// and turbulence, which run on the world's clock and so differ, leave.
TEST_CASE("world ears: the head's turbulence is the same wherever in the world the head is") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_origin");
  Run near;
  near.wind_m_s = 3.0;
  near.seconds = 60.0;
  near.start = WorldPos{0.0, 1.7, 0.0};
  Run far = near;
  far.start = WorldPos{0.0, 1.7, 5000.0};
  far.world_time_s = near.world_time_s + 5000.0 / 3.0;
  listen(near, tmp.file("near.wav"));
  listen(far, tmp.file("far.wav"));
  const Heard a = hear(read_wav(tmp.file("near.wav")));
  const Heard b = hear(read_wav(tmp.file("far.wav")));
  print("standing at the origin, 3 m/s", a);
  print("standing 5 km down the wind, 3 m/s", b);
  CHECK(std::fabs(a.level_db - b.level_db) < 0.5);
  CHECK(std::fabs(a.spread_db - b.spread_db) < 1.0);
  CHECK(std::fabs(a.envelope_corr - b.envelope_corr) < 0.15);
  CHECK(a.envelope_corr > 0.2);  // the gust's share, as the minute in a wind above holds it
  CHECK(b.envelope_corr > 0.2);
}

// The hold on the mean (S121): the world's ceiling holds on the ears' mean, so in a gale the hold
// follows the wind and never an ear's 40 ms buffeting: tick to tick it moves as the gust and the
// head's turbulence do (a quarter of a decibel a tick at most, 0.23 at the 95th percentile when
// this was written), not as the buffeting's factor (several decibels between its frames). The
// refreshes, 8 a second, step the crowns. The ears are set 9.5 dB over the default (-16.5, batch
// 75's), so that a 25 m/s gale takes them to the world's ceiling: at the default since S121's refit
// (-26) they stay under it and the hold never acts, which would leave the case nothing to hold.
TEST_CASE("world ears: in a gale the ears' buffeting never moves the world's hold") {
  const Tunable loud("audio.world.ears_db", "-16.5");
  const test::TempDir tmp("world_ears_gale");
  Avenue trees;
  Run r;
  r.wind_m_s = 25.0;
  r.seconds = 6.0;
  r.trees = &trees;
  WorldStats stats;
  std::vector<f64> held;
  r.stats = &stats;
  r.held = &held;
  listen(r, tmp.file("gale.wav"));
  std::vector<f64> step;
  for (usize i = k_tick_hz / 2; i + 1 < held.size(); ++i)
    step.push_back(std::fabs(held[i + 1] - held[i]));
  const f64 p95 = percentile(step, 0.95);
  MESSAGE("gale: held on " << stats.held_ticks << " ticks, last " << stats.held_db
                           << " dB; tick-to-tick move p95 " << p95 << " dB, most "
                           << *std::max_element(step.begin(), step.end()) << " dB");
  CHECK(stats.held_ticks > 0u);
  CHECK(p95 < 0.3);
}

// The ramp's loss given back (S121): the buffeting's mean is one on the ears' gains as heard, the
// mixer's ramp's loss given back by a factor derived from the ramp and the tick rate: a minute's
// runner in still air with the buffeting against without it (`config.turbulence`: no wind, so no
// gust and no head turbulence either way).
TEST_CASE("world ears: the buffeting leaves the heard mean where the law puts it") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_ramp");
  Run r;
  r.pace_m_s = 5.0;
  r.seconds = 60.0;
  listen(r, tmp.file("on.wav"));
  r.turbulence = false;
  listen(r, tmp.file("off.wav"));
  const Heard on = hear(read_wav(tmp.file("on.wav")));
  const Heard off = hear(read_wav(tmp.file("off.wav")));
  print("running 5 m/s, buffeting", on);
  print("running 5 m/s, no buffeting", off);
  CHECK(std::fabs(on.level_db - off.level_db) < 0.3);
}

// The vector difference (S121): the speed past the head is the vector difference of the wind and
// the head's velocity. A runner going down the wind at its speed at the head hears no wind of their
// own; a runner across it hears the two speeds' hypotenuse, the downwind ear the louder by the
// side-on share of `audio.world.ears_side_db` either way. No turbulence, so the law is met exactly.
TEST_CASE("world ears: down the wind at its speed nothing, across it the vector difference") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_vector");
  const f64 u = head_wind(3.0);
  Run tail;
  tail.wind_m_s = 3.0;
  tail.pace_m_s = u;
  tail.seconds = 3.0;
  tail.heading = Vec3{0.0f, 0.0f, 1.0f};
  tail.turbulence = false;
  listen(tail, tmp.file("tail.wav"));
  const Heard t = hear(read_wav(tmp.file("tail.wav")));
  CHECK(t.level_db < -900.0);  // nothing at all after the first half second

  Run cross = tail;
  cross.pace_m_s = 5.0;
  cross.seconds = 6.0;
  cross.heading = Vec3{1.0f, 0.0f, 0.0f};
  listen(cross, tmp.file("cross.wav"));
  const Heard c = hear(read_wav(tmp.file("cross.wav")));
  print("running 5 m/s across 3 m/s", c);
  const f64 rel = std::sqrt(25.0 + u * u);
  const f64 side = u / rel;  // the air blows to the runner's right: the right ear is downwind
  const f64 both =
      10.0 * std::log10(0.5 * (std::pow(10.0, 0.4 * side) + std::pow(10.0, -0.4 * side)));
  MESSAGE("across: " << c.level_db << " against " << law_db(rel) + both << "; right over left "
                     << c.right_db - c.left_db << " against " << 8.0 * side);
  CHECK(std::fabs(c.level_db - (law_db(rel) + both)) < 1.0);
  CHECK(std::fabs((c.right_db - c.left_db) - 8.0 * side) < 0.5);
  CHECK(c.level_db < law_db(5.0 + u) - 3.0);  // not the sum of the two speeds
}

// The law at engine-view's own rate (S121): the cases above run at 240 ticks a second; at 60 the
// buffeting's frames are a tick and a bit and the mixer's ramp spans three ticks, and the ramp's
// loss the field derives for that rate gives the mean back as it does at 240.
TEST_CASE("world ears: at 60 ticks a second a runner still hears the law") {
  const Tunable solo("audio.world.solo", "3");
  const test::TempDir tmp("world_ears_60");
  Run r;
  r.pace_m_s = 5.0;
  r.seconds = 12.0;
  r.tick_hz = 60;
  listen(r, tmp.file("run60.wav"));
  const Heard run = hear(read_wav(tmp.file("run60.wav")));
  print("running 5 m/s, still air, 60 ticks a second", run);
  CHECK(std::fabs(run.level_db - law_db(5.0)) < 1.0);
  CHECK(run.under_300 > 0.6);
}
