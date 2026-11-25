// The refresh off the frame's thread and the surf joined into lines (roadmap S122;
// docs/subsystems/world_sound.md, "The refresh off the frame's thread", "The surf joined into
// lines"): a steady world starts no refresh and sends no command; a refresh's result is applied at
// its tick whether a worker or the tick's own thread computed it, so the null render is the same
// bytes; a worker the tick finds late is counted and the result is still the one it would have
// been; the join keeps the surf's power, and its level heard where the line's law says it must be.
// Every case judges by counts, the stage's descriptions or the null backend's bytes; none opens a
// device.

#include <core/jobs/job_system.h>
#include <core/memory/memory.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

namespace {

constexpr f64 k_far = 1.0e7;
constexpr f64 k_line_pi = 3.14159265358979323846;

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

// The case's world: a grove (a crown overhead, a ring near, a wood far) and a channel, every one
// sent whatever its distance, and a shore of surf when asked.
struct World {
  std::vector<Crown> crowns;
  std::vector<Stretch> stretches;
  std::vector<Surf> surf;
  static void canopy(void* user, const WorldPos&, f64, const CrownSink& sink) {
    for (const Crown& c : static_cast<World*>(user)->crowns)
      sink.emit(sink.user, c);
  }
  static void flow(void* user, const WorldPos&, f64, const StretchSink& sink) {
    for (const Stretch& s : static_cast<World*>(user)->stretches)
      sink.emit(sink.user, s);
  }
  static void shore(void* user, const WorldPos&, f64, const SurfSink& sink) {
    for (const Surf& s : static_cast<World*>(user)->surf)
      sink.emit(sink.user, s);
  }
};

World world() {
  World w;
  w.crowns.push_back(
      Crown{WorldPos{k_far + 3.0, 0.0, k_far - 2.0}, 8.0f, 200.0f, CanopyClass::Blade, 1.0f});
  for (u32 i = 0; i < 40; ++i) {
    const f64 a = 0.157 * i;
    const f64 r = 12.0 + 4.0 * i;
    w.crowns.push_back(Crown{WorldPos{k_far + r * std::cos(a), 0.0, k_far + r * std::sin(a)}, 10.0f,
                             150.0f, i % 3 == 0 ? CanopyClass::Needle : CanopyClass::Blade, 1.2f});
  }
  for (u32 i = 0; i < 10; ++i) {
    const f64 x0 = -50.0 + 10.0 * i;
    Stretch s;
    s.a = WorldPos{k_far + x0, -0.02 * x0, k_far + 30.0};
    s.b = WorldPos{k_far + x0 + 10.0, -0.02 * (x0 + 10.0), k_far + 30.0};
    s.discharge_m3s = 0.3f;
    s.drop_m = 0.2f;
    s.wet_width_m = 3.0f;
    s.froude = 0.5f;
    w.stretches.push_back(s);
  }
  return w;
}

// A straight shore along +x at z = `z`, `n` records of 16 m, each giving up `w_m` watts a metre,
// their crests arriving together.
std::vector<Surf> shore(u32 n, f64 z, f64 w_m) {
  std::vector<Surf> out;
  for (u32 i = 0; i < n; ++i) {
    Surf s;
    s.at = WorldPos{k_far - 8.0 * n + 16.0 * i + 8.0, 0.0, k_far + z};
    s.along = Vec2{1.0f, 0.0f};
    s.length_m = 16.0f;
    s.power_w_m = static_cast<f32>(w_m);
    s.height_m = 1.0f;
    s.surf_similarity = 0.6f;
    s.period_s = 8.0f;
    s.arrival_s = 3.0f;
    out.push_back(s);
  }
  return out;
}

std::unique_ptr<jobs::JobSystem> make_jobs() {
  jobs::JobSystemConfig c;
  c.performance_workers = 1;
  c.efficiency_workers = 1;
  c.pin_threads = false;
  return std::make_unique<jobs::JobSystem>(c);
}

struct Session {
  SoundStage stage;
  gfx::WindDesc wind;
  WorldWind frame;
  World w;
  WorldPos ears{k_far, 1.7, k_far};
  bool walk = true;
  bool clock = true;  // the sky's and the world's clocks run
  Session(World world_in, jobs::JobSystem* jobs, bool turbulence = true) : w(std::move(world_in)) {
    StageConfig config;
    config.mode = AudioMode::Null;
    config.world = true;
    config.turbulence = turbulence;
    config.tick_hz = 60;
    REQUIRE(stage.open(config, nullptr, nullptr));
    wind.kind = gfx::WindKind::fixed;
    wind.speed_m_s = 6.0;
    wind.to_deg = 90.0;
    wind.roughness_m = 0.5;
    frame.scene = &wind;
    frame.sky_time_s = 1000.0;
    frame.world_time_s = 1000.0;
    stage.set_canopy(CanopySource{&w, &World::canopy});
    stage.set_flow(FlowSource{&w, &World::flow});
    stage.set_surf(SurfSource{&w, &World::shore});
    stage.set_jobs(jobs);
  }
  ~Session() { stage.set_jobs(nullptr); }
  void tick(u32 n = 1) {
    for (u32 i = 0; i < n; ++i) {
      stage.set_listener(ears, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
      stage.set_wind(frame);
      stage.tick();
      if (clock) {
        frame.sky_time_s += 1.0 / 60.0;
        frame.world_time_s += 1.0 / 60.0;
      }
      frame.water_time_s += 1.0 / 60.0;
      if (walk) ears.x += 1.4 / 60.0;  // a walker's pace along x
    }
  }
  // The beds' and the water voices' power summed (the surf's join is held by the level heard).
  f64 water_db() const {
    f64 p = 0.0;
    for (u32 i = 0; i < stage.world_voice_count(); ++i) {
      const WorldVoiceInfo v = stage.world_voice(i);
      if (v.kind != WorldVoiceKind::Bed && v.kind != WorldVoiceKind::Surf) continue;
      if (v.level_db > -999.0f) p += std::pow(10.0, static_cast<f64>(v.level_db) / 10.0);
    }
    return p > 0.0 ? 10.0 * std::log10(p) : -1000.0;
  }
};

}  // namespace

TEST_CASE("world refresh: a steady world starts no refresh and sends no command") {
  // A fixed wind with no turbulence and a listener standing still: the snapshot a refresh takes is
  // the last one's from the third refresh on (the first chose from nothing, the second kept what
  // the first chose), so nothing is started, and nothing is sent once every voice has its first.
  auto jobs = make_jobs();
  Session s(world(), jobs.get(), false);
  s.walk = false;
  s.clock = false;
  s.tick(7 * 3);  // three refreshes and their results
  const WorldStats warm = s.stage.world_stats();
  CHECK(warm.refresh_started == 2u);
  CHECK(warm.refresh_skipped == 1u);
  s.tick(7 * 40);
  const WorldStats& after = s.stage.world_stats();
  CHECK(after.refreshes == warm.refreshes + 40u);
  CHECK(after.refresh_started == 2u);
  CHECK(after.refresh_skipped == warm.refresh_skipped + 40u);
  CHECK(after.commands == warm.commands);
  CHECK(after.crown_voices > 0u);
  CHECK(after.stream_voices > 0u);
  MESSAGE("snapshot " << after.snapshot_bytes << " bytes; " << after.crown_voices << " crowns, "
                      << after.stream_voices << " streams");
}

TEST_CASE("world refresh: a result is applied at its tick, on a worker or not, the same bytes") {
  // At 60 ticks a second a refresh is gathered every 7 ticks (8 a second) and applied 3 after.
  const auto run = [](jobs::JobSystem* jobs, u32 ticks, WorldStats* stats) {
    Session s(world(), jobs);
    s.tick(3);  // ticks 0 to 2: the first refresh gathered at 0, not yet applied
    CHECK(s.stage.world_stats().crown_voices == 0u);
    CHECK(s.stage.world_stats().stream_voices == 0u);
    s.tick(1);  // tick 3: applied
    CHECK(s.stage.world_stats().crown_voices > 0u);
    CHECK(s.stage.world_stats().stream_voices > 0u);
    s.tick(ticks - 4);
    if (stats != nullptr) *stats = s.stage.world_stats();
    return s.stage.render_hash();
  };
  WorldStats here{};
  const u64 inline_hash = run(nullptr, 600, &here);
  CHECK(here.refresh_on_worker == 0u);
  CHECK(here.refresh_late == 0u);
  CHECK(here.refresh_started > 60u);  // a walker's world changes every refresh
  auto jobs = make_jobs();
  for (u32 n = 0; n < 3; ++n) {
    WorldStats worker{};
    CHECK(run(jobs.get(), 600, &worker) == inline_hash);
    CHECK(worker.refresh_started == here.refresh_started);
    CHECK(worker.refresh_on_worker + worker.refresh_late >= worker.refresh_started);
    CHECK(worker.commands == here.commands);
    MESSAGE("run " << n << ": " << worker.refresh_on_worker << " of " << worker.refresh_started
                   << " on the worker, " << worker.refresh_late << " late");
  }
}

TEST_CASE("world refresh: a late worker is counted and its result is the one it would have been") {
  // The Efficiency pool's one worker held by a job of the case's own: the refresh queues behind it,
  // its tick comes, and the tick's thread takes it (counted late); the bytes are the inline run's.
  const auto render = [](jobs::JobSystem* jobs, bool hold, WorldStats& stats) {
    std::atomic<bool> release{false};
    jobs::Counter held;
    Session s(world(), jobs);
    if (hold) {
      held.add(1);
      jobs->schedule(jobs::Pool::Efficiency,
                     jobs::Job{[](void* flag) {
                                 while (!static_cast<std::atomic<bool>*>(flag)->load())
                                   platform::pause_cpu();
                               },
                               &release, &held});
    }
    s.tick(4);  // gathered at tick 0 behind the held worker, applied at tick 3
    release.store(true);
    if (hold) held.wait_blocking();
    s.tick(200);
    stats = s.stage.world_stats();
    return s.stage.render_hash();
  };
  WorldStats here{};
  const u64 expected = render(nullptr, false, here);
  auto jobs = make_jobs();
  WorldStats late{};
  CHECK(render(jobs.get(), true, late) == expected);
  CHECK(late.refresh_late >= 1u);
  CHECK(late.refresh_started == here.refresh_started);
  CHECK(late.refresh_on_worker >= 1u);  // the later ones went to the worker again
  MESSAGE("late " << late.refresh_late << ", on the worker " << late.refresh_on_worker << " of "
                  << late.refresh_started);
}

TEST_CASE("world refresh: 2,400 ticks with a worker allocate nothing on the tick's thread") {
  auto jobs = make_jobs();
  Session s(world(), jobs.get());
  s.walk = false;
  s.tick(60);
  const u64 before = mem::allocations_on_thread();
  s.tick(2400);
  CHECK(mem::allocations_on_thread() - before == 0u);
}

TEST_CASE("surf join: neighbouring records become lines and keep their power") {
  // A straight shore of 40 records of 16 m: lines of the 64 m squares they fall in, the power W' L
  // summed the same to a part in 10^6, and a record alone in its square kept as it is.
  const std::vector<Surf> records = shore(40, -20.0, 900.0);
  Vector<Surf> lines;
  const u32 n = join_surf(records.data(), static_cast<u32>(records.size()), lines);
  CHECK(n == lines.size());
  CHECK(n < records.size() / 3u);
  f64 before = 0.0, after = 0.0, length = 0.0;
  for (const Surf& s : records)
    before += static_cast<f64>(s.power_w_m) * static_cast<f64>(s.length_m);
  for (const Surf& s : lines) {
    after += static_cast<f64>(s.power_w_m) * static_cast<f64>(s.length_m);
    length += static_cast<f64>(s.length_m);
    CHECK(s.period_s == 8.0f);
    CHECK(s.arrival_s == doctest::Approx(3.0).epsilon(1e-5));
  }
  CHECK(after == doctest::Approx(before).epsilon(1e-6));
  CHECK(length == doctest::Approx(640.0).epsilon(1e-6));
  MESSAGE(records.size() << " records of surf joined into " << n << " lines");
  // Crests half a period apart do not share a line, nor do plunging and surging breakers.
  std::vector<Surf> mixed = shore(4, -20.0, 900.0);
  for (u32 i = 0; i < 4; ++i)  // all four in one 64 m square
    mixed[i].at.x = k_far + 8.0 + 16.0 * i;
  mixed[1].arrival_s = 7.0f;
  mixed[2].surf_similarity = 2.5f;
  Vector<Surf> apart;
  CHECK(join_surf(mixed.data(), static_cast<u32>(mixed.size()), apart) == 3u);
  // One alone is kept, bit for bit.
  Vector<Surf> one;
  CHECK(join_surf(records.data(), 1u, one) == 1u);
  CHECK(std::memcmp(&one[0], &records[0], sizeof(Surf)) == 0);
}

TEST_CASE("surf join: the joined shore is heard at the records' level, by the line's law") {
  // Spreading alone (the path off) and the surf in the beds at its mean (no voice of its own): a
  // straight shore's lines are its records end to end, and a line's W' angle / (2 pi r) is additive
  // over collinear pieces, so the level heard is the same near and far (to the f32 rounding of W').
  // With the path, a line's terms are taken at its nearest point, so a joined line is heard a
  // little louder than its pieces (their far ends cross more air and ground); within 0.5 dB at
  // the near and the far places here.
  const Tunable voices("audio.world.surf", "0");
  for (const bool path : {false, true}) {
    const Tunable p("audio.world.path", path ? "true" : "false");
    for (const f64 off : {5.0, 60.0, 400.0}) {
      World a;
      a.surf = shore(40, -off, 900.0);
      World b;
      Vector<Surf> lines;
      join_surf(a.surf.data(), static_cast<u32>(a.surf.size()), lines);
      for (const Surf& s : lines)
        b.surf.push_back(s);
      Session records(std::move(a), nullptr);
      Session joined(std::move(b), nullptr);
      records.walk = false;
      joined.walk = false;
      records.tick(10);
      joined.tick(10);
      const f64 r = records.water_db();
      const f64 j = joined.water_db();
      REQUIRE(r > -999.0);
      if (!path) {
        CHECK(std::fabs(j - r) < 0.01);
      } else {
        CHECK(j - r >= -0.01);
        CHECK(j - r < 0.5);
      }
      CHECK(joined.stage.world_stats().surf_seen < records.stage.world_stats().surf_seen);
      MESSAGE(std::string(path ? "path on" : "path off")
              << ", the shore " << off << " m off: records " << r << " dB SPL, joined " << j << " ("
              << records.stage.world_stats().surf_seen << " to "
              << joined.stage.world_stats().surf_seen << ")");
    }
  }
}

TEST_CASE("surf join: a finite line falls 3 dB a doubling near it and 6 far, turning at L / pi") {
  // The law the stage takes for a line (the finite incoherent line source, W' angle / (2 pi r)),
  // broadside of a 128 m line from its middle: near (r << L) the angle is pi and the level falls as
  // 10 log r; far (r >> L) the angle is L / r and it falls as 20 log r; the two asymptotes cross at
  // r = L / pi, where the line is 1.95 dB under the near one.
  const f64 len = 128.0;
  const auto level = [len](f64 r) {
    const f64 angle = 2.0 * std::atan(0.5 * len / r);
    return 10.0 * std::log10(angle / (2.0 * k_line_pi * r));
  };
  CHECK(level(2.0) - level(4.0) == doctest::Approx(3.0).epsilon(0.03));
  CHECK(level(4000.0) - level(8000.0) == doctest::Approx(6.02).epsilon(0.001));
  const f64 turn = len / k_line_pi;
  const f64 near = 10.0 * std::log10(k_line_pi / (2.0 * k_line_pi * turn));
  CHECK(near - level(turn) == doctest::Approx(1.95).epsilon(0.01));
}
