// Running water and the surf (roadmap S113; docs/subsystems/world_sound.md, "Running water and the
// surf"): a stretch's level against rho g Q h, a fall's height and its cap, the surf's envelope at
// the arrival's crest, a lake's lap, the far ones in the beds with their power kept across a
// promotion, the counts, an unchanged world and a still one, and a trickle's bubbles. Every case
// judges the stage's own descriptions or the null backend's samples; none opens a device.

#include <core/memory/memory.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>

#include <cmath>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

namespace {

constexpr f64 k_far = 1.0e7;  // the listener 10,000 km out, as the wind's cases stand it
constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_rho = 998.2;
constexpr f64 k_g = 9.81;

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

// The case's own tables: stretches and surf, every one sent whatever its distance.
struct Water {
  std::vector<Stretch> stretches;
  std::vector<Surf> surf;
  static void flow(void* user, const WorldPos&, f64, const StretchSink& sink) {
    for (const Stretch& s : static_cast<Water*>(user)->stretches)
      sink.emit(sink.user, s);
  }
  static void shore(void* user, const WorldPos&, f64, const SurfSink& sink) {
    for (const Surf& s : static_cast<Water*>(user)->surf)
      sink.emit(sink.user, s);
  }
};

// A fixture channel along +x: `n` stations 10 m apart from x = -50 m, its level falling `slope`
// a metre, carrying `q` over a wet width of 3 m (a uniform reach as a solve writes it).
std::vector<Stretch> channel(f64 q, f64 slope, u32 n = 11) {
  std::vector<Stretch> out;
  for (u32 i = 0; i + 1 < n; ++i) {
    const f64 x0 = -50.0 + 10.0 * i;
    Stretch s;
    s.a = WorldPos{k_far + x0, -slope * x0, k_far};
    s.b = WorldPos{k_far + x0 + 10.0, -slope * (x0 + 10.0), k_far};
    s.discharge_m3s = static_cast<f32>(q);
    s.drop_m = static_cast<f32>(slope * 10.0);
    s.wet_width_m = 3.0f;
    s.froude = 0.5f;
    out.push_back(s);
  }
  return out;
}

struct Session {
  SoundStage stage;
  Water water;
  WorldWind clock;
  WorldPos ears{k_far, 1.7, k_far + 5.0};
  explicit Session(Water w) : water(std::move(w)) {
    StageConfig config;
    config.mode = AudioMode::Null;
    config.world = true;
    config.tick_hz = 60;
    REQUIRE(stage.open(config, nullptr, nullptr));
    stage.set_flow(FlowSource{&water, &Water::flow});
    stage.set_surf(SurfSource{&water, &Water::shore});
  }
  void tick(u32 n = 1) {
    for (u32 i = 0; i < n; ++i) {
      stage.set_listener(ears, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
      stage.set_wind(clock);  // no wind: the crowns and the ears are silent, the water is not
      stage.tick();
      clock.world_time_s += 1.0 / 60.0;
    }
  }
  WorldVoiceInfo loudest(WorldVoiceKind kind) const {
    WorldVoiceInfo out;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      if (v.kind == kind && v.level_db > out.level_db) out = v;
    }
    return out;
  }
  // Every voice of the kinds the water reaches, summed by power: the beds and the water's own.
  f64 water_power() const {
    f64 p = 0.0;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      if (v.kind == WorldVoiceKind::Crown || v.kind == WorldVoiceKind::EarLeft ||
          v.kind == WorldVoiceKind::EarRight || v.level_db < -999.0f)
        continue;
      p += std::pow(10.0, static_cast<f64>(v.level_db) / 10.0);
    }
    return p;
  }
};

// The law with nothing else: a line's intensity W' angle / (2 pi r) over dB SPL's reference.
f64 line_db(f64 watts_m, f64 r, f64 angle) {
  return 10.0 * std::log10(watts_m * angle / (2.0 * k_pi * r) / 1e-12);
}

}  // namespace

TEST_CASE("world water: a stretch's level is the power it gives up, rho g Q h, along the channel") {
  Tunable path("audio.world.path", "false");  // the law alone: spreading, no air or ground
  Session s(Water{channel(0.5, 0.02), {}});
  s.ears = WorldPos{k_far + 5.0, 1.7, k_far + 5.0};
  s.tick(2);
  // The stretch beside the listener (x 0 to 10 m, its level 0 to -0.2 m; the ears 5 m off its
  // middle, 1.7 m up) is the loudest: W' = eta rho g Q h / L, and its angle from the listener.
  const WorldVoiceInfo v = s.loudest(WorldVoiceKind::Stream);
  REQUIRE(v.level_db > -999.0f);
  const f64 len = std::sqrt(100.04);
  const f64 watts_m = k_water_airborne_efficiency * k_rho * k_g * 0.5 * 0.2 / len;
  // The line from (0, 0, 0) to (10, -0.2, 0) seen from (5, 1.7, 5): the foot of the
  // perpendicular, its length and the angle the line subtends.
  const f64 ux = 10.0 / len, uy = -0.2 / len;
  const f64 along = 5.0 * ux + 1.7 * uy;
  const f64 fx = 5.0 - along * ux, fy = 1.7 - along * uy, fz = 5.0;
  const f64 r = std::sqrt(fx * fx + fy * fy + fz * fz);
  const f64 angle = std::atan((len - along) / r) - std::atan(-along / r);
  CHECK(static_cast<f64>(v.level_db) == doctest::Approx(line_db(watts_m, r, angle)).epsilon(1e-4));
  MESSAGE("a 0.5 m3/s stream on 2 %, 5 m off: " << v.level_db << " dB SPL");
  // Twice the flow on the same reach: twice the power, 3.01 dB.
  Session twice(Water{channel(1.0, 0.02), {}});
  twice.ears = s.ears;
  twice.tick(2);
  CHECK(static_cast<f64>(twice.loudest(WorldVoiceKind::Stream).level_db - v.level_db) ==
        doctest::Approx(10.0 * std::log10(2.0)).epsilon(1e-4));
  // A still reach gives up nothing and holds no voice.
  Session still(Water{channel(0.5, 0.0), {}});
  still.tick(10);
  CHECK(still.stage.world_stats().stream_voices == 0u);
  CHECK(still.stage.world_stats().commands == 0u);
}

TEST_CASE("world water: a fall of twice the height is 3.01 dB louder, and its pool caps its flow") {
  Tunable path("audio.world.path", "false");
  const auto fall = [](f64 height, f64 q, bool on_water) {
    Stretch f;
    f.a = WorldPos{k_far + 30.0, height, k_far - 30.0};
    f.b = WorldPos{k_far + 30.0, 0.0, k_far - 29.0};
    f.discharge_m3s = static_cast<f32>(q);
    f.drop_m = static_cast<f32>(height);
    f.wet_width_m = 2.0f;
    f.fall = true;
    f.lands_on_water = on_water;
    Session s(Water{{f}, {}});
    s.tick(2);
    return s.loudest(WorldVoiceKind::Fall);
  };
  const WorldVoiceInfo ten = fall(10.0, 0.2, true);
  const WorldVoiceInfo twenty = fall(20.0, 0.2, true);
  REQUIRE(ten.level_db > -999.0f);
  CHECK(static_cast<f64>(twenty.level_db - ten.level_db) ==
        doctest::Approx(10.0 * std::log10(2.0)).epsilon(1e-4));
  MESSAGE("a 10 m fall of 0.2 m3/s, 45 m off: " << ten.level_db << " dB SPL");
  // Under the cap (0.5 m2/s over its 2 m lip) twice the flow is 3 dB; past it nothing more.
  CHECK(static_cast<f64>(fall(10.0, 0.4, true).level_db - ten.level_db) ==
        doctest::Approx(10.0 * std::log10(2.0)).epsilon(1e-4));
  CHECK(fall(10.0, 2.0, true).level_db == doctest::Approx(fall(10.0, 4.0, true).level_db));
  // On water its middle octaves are the stronger; on the bed its top.
  const WorldVoiceInfo rock = fall(10.0, 0.2, false);
  CHECK(ten.band_db[4] > rock.band_db[4]);
  CHECK(rock.band_db[7] > ten.band_db[7]);
  CHECK(rock.level_db == doctest::Approx(ten.level_db));
}

TEST_CASE("world water: the surf peaks within a tick of the arrival's crest at its place") {
  Surf su;
  su.at = WorldPos{k_far, 0.0, k_far - 20.0};
  su.along = Vec2{1.0f, 0.0f};
  su.length_m = 40.0f;
  su.power_w_m = static_cast<f32>(surf_power_w_m(1.0, 8.0));
  su.height_m = 1.0f;
  su.surf_similarity = 0.6f;
  su.period_s = 8.0f;
  su.arrival_s = 3.0f;
  Session s(Water{{}, {su}});
  // Two periods at 60 ticks a second: the loudest tick of each is the crest's, within a tick.
  for (u32 period = 0; period < 2; ++period) {
    f64 best = -1e9;
    f64 best_t = 0.0;
    for (u32 i = 0; i < 480; ++i) {
      const f64 t = s.clock.world_time_s;
      s.tick();
      const f64 level = static_cast<f64>(s.loudest(WorldVoiceKind::Surf).level_db);
      if (level > best) {
        best = level;
        best_t = t;
      }
    }
    const f64 crest = 3.0 + 8.0 * std::round((best_t - 3.0) / 8.0);
    CHECK(std::fabs(best_t - crest) <= 1.0 / 60.0 + 1e-9);
  }
  CHECK(s.stage.world_stats().surf_voices == 1u);
  // A sea with no height — no wind and no swell — gives up nothing: no voice, nothing sent.
  Surf calm = su;
  calm.power_w_m = static_cast<f32>(surf_power_w_m(0.0, 8.0));
  Session still(Water{{}, {calm}});
  still.tick(120);
  CHECK(still.stage.world_stats().surf_voices == 0u);
  CHECK(still.stage.world_stats().commands == 0u);
}

TEST_CASE("world water: a lake's lap at 0.1 m is heard on its shore") {
  Tunable path("audio.world.path", "false");
  Surf lap;
  lap.at = WorldPos{k_far, 0.0, k_far};
  lap.along = Vec2{1.0f, 0.0f};
  lap.length_m = 40.0f;
  lap.power_w_m = static_cast<f32>(surf_power_w_m(0.1, 1.2));
  lap.height_m = 0.1f;
  lap.surf_similarity = 0.5f;
  lap.period_s = 1.2f;
  Session s(Water{{}, {lap}});
  // Averaged over whole periods the envelope is the law's mean: 72 ticks, one second and a fifth.
  s.tick(1);
  f64 mean = 0.0;
  for (u32 i = 0; i < 72; ++i) {
    s.tick();
    mean += std::pow(10.0, static_cast<f64>(s.loudest(WorldVoiceKind::Surf).level_db) / 10.0);
  }
  const f64 level = 10.0 * std::log10(mean / 72.0);
  const f64 r = std::sqrt(25.0 + 1.7 * 1.7);
  const f64 watts_m = k_water_airborne_efficiency * surf_power_w_m(0.1, 1.2);
  const f64 want = line_db(watts_m, r, 2.0 * std::atan(20.0 / r));
  MESSAGE("a lake's lap at 0.1 m, 5 m from the shore: " << level << " dB SPL (the law " << want
                                                        << ")");
  CHECK(std::fabs(level - want) < 0.25);  // 72 samples of the envelope against its mean
  CHECK(level > 40.0);
  CHECK(level < 55.0);
}

TEST_CASE("world water: the surf's bound is over what its path delivers, at every place tried") {
  // The cull's bound (`surf_bound_db`) against the level a record is heard at after its whole path
  // (air, ground), the mean over whole periods of its voice, each record alone, nothing culled.
  Tunable cull("audio.world.surf_cull", "false");
  u32 tried = 0;
  f64 least_margin = 1e9;
  for (const f64 length : {4.0, 16.0, 40.0}) {
    for (const f64 off : {3.0, 25.0, 140.0, 600.0}) {
      for (const f64 turn : {0.0, 1.0}) {
        for (const f64 xi : {0.5, 3.0}) {
          Surf su;
          su.at = WorldPos{k_far + 7.0, 0.0, k_far};
          su.along = Vec2{static_cast<f32>(std::cos(turn)), static_cast<f32>(std::sin(turn))};
          su.length_m = static_cast<f32>(length);
          su.power_w_m = static_cast<f32>(surf_power_w_m(0.6, 1.2));
          su.height_m = 0.6f;
          su.surf_similarity = static_cast<f32>(xi);
          su.period_s = 1.2f;
          Session s(Water{{}, {su}});
          s.ears = WorldPos{k_far + 7.0 + 0.6 * off, 1.7, k_far + 0.8 * off};
          s.tick(1);
          f64 mean = 0.0;
          for (u32 i = 0; i < 72; ++i) {  // whole periods: the envelope's mean
            s.tick();
            const f32 level = s.loudest(WorldVoiceKind::Surf).level_db;
            if (level > -999.0f) mean += std::pow(10.0, static_cast<f64>(level) / 10.0);
          }
          const f64 bound = surf_bound_db(su, s.ears);
          if (mean > 0.0) {
            const f64 level = 10.0 * std::log10(mean / 72.0);
            least_margin = std::min(least_margin, bound - level);
            CHECK_MESSAGE(bound >= level, "length " << length << " off " << off << " turn " << turn
                                                    << " xi " << xi << ": bound " << bound
                                                    << " against " << level);
            ++tried;
          }
        }
      }
    }
  }
  MESSAGE(tried << " records heard; the bound over the heard level by at least " << least_margin
                << " dB");
  CHECK(tried >= 40u);
}

TEST_CASE("world water: the surf's cull skips only what could not be heard, and counts it") {
  // A shore of small laps from 5 m to 790 m: the near ones costed, the far ones' bound under the
  // floor less 20 dB. The sum heard with the cull against the sum with none differs by at most
  // what the skipped records' bounds add, a small fraction of a decibel.
  Water w;
  for (i32 i = -79; i <= 79; ++i) {
    Surf su;
    su.at = WorldPos{k_far + 10.0 * i, 0.0, k_far + 5.0};
    su.along = Vec2{1.0f, 0.0f};
    su.length_m = 4.0f;
    su.power_w_m = static_cast<f32>(surf_power_w_m(0.01, 1.0));
    su.height_m = 0.01f;
    su.surf_similarity = 0.5f;
    su.period_s = 1.0f;
    w.surf.push_back(su);
  }
  const WorldPos ears{k_far, 1.7, k_far};
  u64 skipped = 0;
  f64 skipped_bound = 0.0;
  for (const Surf& su : w.surf) {
    const f64 b = surf_bound_db(su, ears);
    if (b <= 10.0 - 20.0) {  // `audio.world.floor_db`'s default less 20 dB
      ++skipped;
      skipped_bound += std::pow(10.0, b / 10.0);
    }
  }
  const auto heard = [&](const char* cull) {
    Tunable t("audio.world.surf_cull", cull);
    Session s(w);
    s.ears = ears;
    s.tick(2);
    return std::pair<f64, WorldStats>{s.water_power(), s.stage.world_stats()};
  };
  const auto on = heard("true");
  const auto off = heard("false");
  CHECK(on.second.surf_culled == skipped);
  CHECK(off.second.surf_culled == 0u);
  CHECK(on.second.surf_seen + on.second.surf_culled == w.surf.size());
  CHECK(skipped > w.surf.size() / 3);  // the shore past about 400 m
  const f64 moved = 10.0 * std::log10(off.first / on.first);
  const f64 allowed = 10.0 * std::log10(1.0 + skipped_bound / on.first);
  MESSAGE(skipped << " of " << w.surf.size() << " skipped; heard " << moved
                  << " dB less than with none, the bound " << allowed << " dB");
  CHECK(moved >= 0.0);
  CHECK(moved <= allowed);
  CHECK(allowed < 0.1);
}
TEST_CASE("world water: the far ones are the beds', their power kept across a promotion") {
  Water w;
  // Three channels round the listener and a fall behind it: many stretches, near and far.
  for (u32 c = 0; c < 3; ++c) {
    std::vector<Stretch> ch = channel(0.3 + 0.2 * c, 0.01 + 0.01 * c);
    for (Stretch& s : ch) {
      s.a.z += 40.0 * c - 30.0;
      s.b.z += 40.0 * c - 30.0;
      w.stretches.push_back(s);
    }
  }
  const auto heard = [&w](const char* streams) {
    Tunable t("audio.world.streams", streams);
    Session s(w);
    s.tick(2);
    return std::pair<f64, u32>{s.water_power(), s.stage.world_stats().stream_voices};
  };
  const auto none = heard("0");
  const auto four = heard("4");
  CHECK(none.second == 0u);
  CHECK(four.second == 4u);
  CHECK(10.0 * std::log10(four.first / none.first) == doctest::Approx(0.0).epsilon(1e-3));
}

TEST_CASE("world water: the counts never pass their tunables, and solo hears one kind") {
  Tunable streams("audio.world.streams", "2");
  Tunable falls("audio.world.falls", "1");
  Tunable surf("audio.world.surf", "1");
  Water w{channel(0.5, 0.02), {}};
  for (u32 i = 0; i < 3; ++i) {
    Stretch f;
    f.a = WorldPos{k_far - 20.0 * i, 5.0, k_far - 40.0};
    f.b = WorldPos{k_far - 20.0 * i, 0.0, k_far - 39.0};
    f.discharge_m3s = 0.3f;
    f.drop_m = 5.0f;
    f.wet_width_m = 2.0f;
    f.fall = true;
    w.stretches.push_back(f);
    Surf su;
    su.at = WorldPos{k_far + 30.0 * i, 0.0, k_far + 60.0};
    su.length_m = 30.0f;
    su.power_w_m = static_cast<f32>(surf_power_w_m(0.5, 5.0));
    su.height_m = 0.5f;
    su.period_s = 5.0f;
    w.surf.push_back(su);
  }
  {
    Session s(w);
    s.tick(30);
    CHECK(s.stage.world_stats().stream_voices == 2u);
    CHECK(s.stage.world_stats().fall_voices == 1u);
    CHECK(s.stage.world_stats().surf_voices == 1u);
    CHECK(s.stage.world_stats().stretches_seen == 13u);
    CHECK(s.stage.world_stats().surf_seen == 3u);
  }
  Tunable solo("audio.world.solo", "5");  // the falls alone
  Session s(w);
  s.tick(30);
  CHECK(s.stage.world_stats().fall_voices == 1u);
  CHECK(s.stage.world_stats().stream_voices == 0u);
  CHECK(s.stage.world_stats().surf_voices == 0u);
  CHECK(s.stage.world_stats().bed_voices == 0u);
}

TEST_CASE("world water: an unchanged world sends nothing, and a tick allocates nothing") {
  Session s(Water{channel(0.5, 0.02), {}});
  s.tick(60);
  const u64 after_start = s.stage.world_stats().commands;
  CHECK(after_start > 0u);
  const u64 before = mem::allocations_on_thread();
  s.tick(2400);
  CHECK(mem::allocations_on_thread() - before == 0u);
  CHECK(s.stage.world_stats().commands == after_start);
}

TEST_CASE("world water: a trickle is heard as separate bubbles, the same bytes run after run") {
  const auto run = []() {
    Session s(Water{channel(0.001, 0.05), {}});  // a litre a second down a 5 % run
    s.tick(120);
    return std::pair<u64, u64>{s.stage.world_stats().bubbles, s.stage.render_hash()};
  };
  const auto a = run();
  const auto b = run();
  // 15 a second for each litre a second on each of the four loudest stretches: about 120 in two
  // seconds, the draw's own spread either way.
  MESSAGE("bubbles in two seconds: " << a.first);
  CHECK(a.first > 60u);
  CHECK(a.first < 200u);
  CHECK(a.second != 0u);
  CHECK(a == b);
}
