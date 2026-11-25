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
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

// Waits, bounded, until the stage has no refresh queued or being computed, so that the apply that
// follows finds the result in hand: the worker's path held by construction, never by who won a
// race. False past the bound (a worker that never ran), which the case reports after its window.
bool settle(const SoundStage& stage) {
  const i64 until = time::monotonic_ns() + 60'000'000'000ll;
  while (!stage.world_refresh_settled()) {
    if (time::monotonic_ns() > until) return false;
    std::this_thread::yield();
  }
  return true;
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
  bool turbulence = true;
  Session(World world_in, jobs::JobSystem* jobs, bool turbulence_in = true)
      : w(std::move(world_in)), turbulence(turbulence_in) {
    open_stage();
    wind.kind = gfx::WindKind::fixed;
    wind.speed_m_s = 6.0;
    wind.to_deg = 90.0;
    wind.roughness_m = 0.5;
    frame.scene = &wind;
    frame.sky_time_s = 1000.0;
    frame.world_time_s = 1000.0;
    stage.set_jobs(jobs);
  }
  ~Session() { stage.set_jobs(nullptr); }
  void open_stage() {
    StageConfig config;
    config.mode = AudioMode::Null;
    config.world = true;
    config.turbulence = turbulence;
    config.tick_hz = 60;
    REQUIRE(stage.open(config, nullptr, nullptr));
    stage.set_canopy(CanopySource{&w, &World::canopy});
    stage.set_flow(FlowSource{&w, &World::flow});
    stage.set_surf(SurfSource{&w, &World::shore});
  }
  // The stage closed and opened again over `next`, its job system kept (L8 of S122's review).
  void reopen(World next) {
    REQUIRE(stage.close(nullptr));
    w = std::move(next);
    open_stage();
  }
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
  // Late by construction, not by a slow worker: the hold has begun on the pool's one worker before
  // the first refresh is queued, Efficiency jobs run on Efficiency workers alone (core/jobs), and a
  // pool never steals from the other, so nothing can take the two queued refreshes but this
  // thread, whatever order the pool keeps its queue in. After the release, the next refresh's
  // result is waited for (bounded) before its tick, so the worker's path is held by construction
  // too, never by the worker winning a race against three back-to-back null ticks.
  struct Hold {
    std::atomic<bool> begun{false};
    std::atomic<bool> release{false};
  };
  const auto render = [](jobs::JobSystem* jobs, bool hold, WorldStats& stats, WorldStats& early,
                         WorldStats& next, bool& settled) {
    Hold h;
    jobs::Counter held;
    Session s(world(), jobs);
    if (hold) {
      held.add(1);
      jobs->schedule(jobs::Pool::Efficiency, jobs::Job{[](void* p) {
                                                         Hold& hh = *static_cast<Hold*>(p);
                                                         hh.begun.store(true);
                                                         while (!hh.release.load())
                                                           platform::pause_cpu();
                                                       },
                                                       &h, &held});
      const i64 until = time::monotonic_ns() + 60'000'000'000ll;
      while (!h.begun.load() && time::monotonic_ns() < until)
        std::this_thread::yield();
      REQUIRE(h.begun.load());
    }
    // Gathered at ticks 0 and 7 behind the held worker, applied at 3 and 10, both by this thread:
    // the second gather does not wait for the first's job, still queued behind the held one.
    s.tick(11);
    early = s.stage.world_stats();
    h.release.store(true);
    if (hold) held.wait_blocking();
    // The refresh gathered at tick 14 is computed before its apply at 17: on the worker, since the
    // stale jobs of 0 and 7 and the job of 14 are the pool's alone and the first to run claims it.
    s.tick(4);
    settled = settle(s.stage);
    s.tick(3);
    next = s.stage.world_stats();
    s.tick(193);
    stats = s.stage.world_stats();
    return s.stage.render_hash();
  };
  WorldStats here{}, here_early{}, here_next{};
  bool here_settled = false;
  const u64 expected = render(nullptr, false, here, here_early, here_next, here_settled);
  CHECK(here_settled);  // with no job system nothing is ever in flight
  auto jobs = make_jobs();
  WorldStats late{}, early{}, next{};
  bool settled = false;
  CHECK(render(jobs.get(), true, late, early, next, settled) == expected);
  REQUIRE(settled);
  CHECK(early.refresh_late == 2u);  // both refreshes before the release, taken by this thread
  CHECK(early.refresh_on_worker == 0u);
  CHECK(next.refresh_late == 2u);       // the one after it was in hand at its tick ...
  CHECK(next.refresh_on_worker == 1u);  // ... and the worker's
  CHECK(late.refresh_late >= 2u);
  CHECK(late.refresh_started == here.refresh_started);
  MESSAGE("late " << late.refresh_late << ", on the worker " << late.refresh_on_worker << " of "
                  << late.refresh_started);
}

TEST_CASE("world refresh: 2,400 ticks with a worker allocate nothing on the tick's thread") {
  // A walker, so every refresh in the window is gathered, scheduled, computed and applied (a still
  // listener's would all be skipped, which holds the skip path only). The first half waits for
  // each result before its tick (the worker's path, by construction); the second ticks back to
  // back (whatever the race gives: the worker's result, or one this thread takes or waits for).
  auto jobs = make_jobs();
  Session s(world(), jobs.get());
  s.tick(60);
  const WorldStats before_stats = s.stage.world_stats();
  const u64 before = mem::allocations_on_thread();
  bool settled = true;
  for (u32 t = 0; t < 1200; ++t) {
    s.tick(1);
    settled = settle(s.stage) && settled;
  }
  s.tick(1200);
  const u64 allocations = mem::allocations_on_thread() - before;
  const WorldStats& after = s.stage.world_stats();
  CHECK(settled);
  CHECK(allocations == 0u);
  // Ticks 60 to 2,459 hold the gathers at the multiples of 7 from 63 to 2,457: 343, each started
  // (the listener moves every tick), and the 342 gathered from 63 to 2,450 applied in the window.
  const u64 started = after.refresh_started - before_stats.refresh_started;
  CHECK(started == 343u);
  CHECK(after.refresh_skipped == before_stats.refresh_skipped);
  const u64 applied = (after.refresh_on_worker - before_stats.refresh_on_worker) +
                      (after.refresh_late - before_stats.refresh_late);
  CHECK(applied >= 342u);
  // The first half's 171 (gathered at 63 to 1,253, applied by tick 1,259) were the worker's.
  CHECK(after.refresh_on_worker - before_stats.refresh_on_worker >= 171u);
  MESSAGE(started << " refreshes started in the window, "
                  << after.refresh_on_worker - before_stats.refresh_on_worker << " on the worker, "
                  << after.refresh_late - before_stats.refresh_late << " late");
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
  // The join's time at attach (L9 of S122's review): 4,000 records, about the staging's sea's
  // 3,920, timed and reported, never checked (a count holds a promise; a clock is the machine's).
  const std::vector<Surf> sea = shore(4000, -20.0, 900.0);
  Vector<Surf> sea_lines;
  const i64 t0 = time::monotonic_ns();
  const u32 sea_n = join_surf(sea.data(), static_cast<u32>(sea.size()), sea_lines);
  const f64 ms = static_cast<f64>(time::monotonic_ns() - t0) * 1e-6;
  CHECK(sea_n == sea_lines.size());
  MESSAGE(sea.size() << " records joined into " << sea_n << " lines in " << ms << " ms");
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
  // The law the stage must take for a line (the finite incoherent line source, W' angle / (2 pi
  // r)), broadside of a 128 m line from its middle: near (r << L) the angle is pi and the level
  // falls as 10 log r; far (r >> L) the angle is L / r and it falls as 20 log r; the two asymptotes
  // cross at r = L / pi, where the line is 1.95 dB under the near one.
  const f64 len = 128.0;
  const auto level = [len](f64 r) {
    const f64 angle = 2.0 * std::atan(0.5 * len / r);
    return 10.0 * std::log10(angle / (2.0 * k_line_pi * r));
  };
  const f64 turn = len / k_line_pi;
  const auto near_asymptote = [](f64 r) {
    return 10.0 * std::log10(k_line_pi / (2.0 * k_line_pi * r));
  };
  CHECK(near_asymptote(turn) - level(turn) == doctest::Approx(1.95).epsilon(0.01));
  // The stage held to it (L4 of S122's review: the law alone held nothing the stage does): one
  // line of 128 m at the ears' height (so its perpendicular distance is the horizontal one), heard
  // through the beds at its mean (no surf voice of its own), the path off (spreading alone), the
  // reaches past 8 km; the level the beds carry, at each distance, less the law's, is one constant.
  const Tunable voices("audio.world.surf", "0");
  const Tunable path("audio.world.path", "false");
  const Tunable reach("audio.world.reach_m", "20000");
  const Tunable water_reach("audio.world.water_reach_m", "20000");
  const auto heard = [len](f64 r) {
    World w;
    Surf s;
    s.at = WorldPos{k_far, 1.7, k_far - r};
    s.along = Vec2{1.0f, 0.0f};
    s.length_m = static_cast<f32>(len);
    s.power_w_m = 9000.0f;
    s.height_m = 1.0f;
    s.surf_similarity = 0.6f;
    s.period_s = 8.0f;
    s.arrival_s = 3.0f;
    w.surf.push_back(s);
    Session session(std::move(w), nullptr);
    session.walk = false;
    session.tick(10);
    CHECK(session.stage.world_stats().surf_seen == 1u);
    CHECK(session.stage.world_stats().held_db == 0.0f);  // under the world's ceiling
    return session.water_db();
  };
  const f64 at2 = heard(2.0), at4 = heard(4.0), at_turn = heard(turn);
  const f64 at4k = heard(4000.0), at8k = heard(8000.0);
  REQUIRE(at8k > -999.0);
  // Within 0.01 dB: the levels are f32s of about 80 dB, and the beds' arithmetic is the stage's.
  const f64 offset = at2 - level(2.0);
  CHECK(std::fabs(at4 - level(4.0) - offset) < 0.01);
  CHECK(std::fabs(at_turn - level(turn) - offset) < 0.01);
  CHECK(std::fabs(at4k - level(4000.0) - offset) < 0.01);
  CHECK(std::fabs(at8k - level(8000.0) - offset) < 0.01);
  // The three claims, in the stage's own levels: 3 dB a doubling near (3.10 for 128 m at 2 and
  // 4 m, the angle already short of pi), 6.02 far, and 1.95 dB under the near asymptote (in the
  // stage's units, through the offset found at 2 m) at the turn.
  CHECK(std::fabs((at2 - at4) - (level(2.0) - level(4.0))) < 0.01);
  CHECK(std::fabs((at4k - at8k) - 6.02) < 0.01);
  const f64 stage_near_at_turn = near_asymptote(turn) + offset;
  CHECK(std::fabs((stage_near_at_turn - at_turn) - 1.95) < 0.02);
  MESSAGE("a 128 m line heard at 2, 4, " << turn << ", 4000 and 8000 m: " << at2 << ", " << at4
                                         << ", " << at_turn << ", " << at4k << ", " << at8k
                                         << " dB SPL");
}

TEST_CASE("world refresh: a reopened stage starts clean") {
  // L8 of S122's review: open, walk, close, open again. Over a different world (a shore and no
  // trees), the first refresh is started and its result is the new world's at its tick, never the
  // last session's result applied; over the same world at the same place, the first refresh is
  // started rather than skipped on the last session's hash.
  auto jobs = make_jobs();
  Session s(world(), jobs.get());
  s.tick(30);
  REQUIRE(s.stage.world_stats().crown_voices > 0u);
  World shore_only;
  shore_only.surf = shore(8, -40.0, 900.0);
  s.reopen(std::move(shore_only));
  s.tick(3);  // ticks 0 to 2: gathered at 0, nothing applied yet
  CHECK(s.stage.world_stats().refresh_started == 1u);
  CHECK(s.stage.world_stats().refresh_skipped == 0u);
  CHECK(s.stage.world_stats().crown_voices == 0u);
  CHECK(s.stage.world_stats().stream_voices == 0u);
  CHECK(s.stage.world_stats().surf_voices == 0u);
  REQUIRE(settle(s.stage));
  s.tick(1);  // tick 3: the new world's result
  CHECK(s.stage.world_stats().crown_voices == 0u);
  CHECK(s.stage.world_stats().stream_voices == 0u);
  CHECK(s.stage.world_stats().surf_voices > 0u);
  // The same world, the listener standing where it stood: not skipped.
  s.walk = false;
  s.tick(30);
  World again;
  again.surf = shore(8, -40.0, 900.0);
  s.reopen(std::move(again));
  s.tick(1);
  CHECK(s.stage.world_stats().refresh_started == 1u);
  CHECK(s.stage.world_stats().refresh_skipped == 0u);
}
