#include <core/profiling/frame_capture.h>
#include <core/profiling/profile.h>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::profiling;

namespace {

std::vector<CaptureEvent> snapshot(CaptureInfo& info) {
  std::vector<CaptureEvent> events(capture_count() + 16);
  events.resize(capture_snapshot(events, info));
  return events;
}

int zoned_work(int n) {
  ENGINE_PROFILE_ZONE();
  int sum = 0;
  for (int i = 0; i < n; ++i) {
    ENGINE_PROFILE_ZONE_NAMED("capture tests: iteration");
    sum += i;
  }
  return sum;
}

}  // namespace

TEST_CASE("frame capture: off records nothing, on records zones, marks and values") {
  capture_reset();
  CHECK_FALSE(capture_enabled());
  CHECK(zoned_work(3) == 3);
  capture_frame(1);
  capture_value(CaptureTrack::gpu, "pass", 1, 2.0);
  CaptureInfo info;
  CHECK(snapshot(info).empty());

  REQUIRE(capture_start());
  CHECK_FALSE(capture_start());  // already on
  capture_name_thread("capture main");
  capture_frame(7);
  CHECK(zoned_work(2) == 1);
  capture_value(CaptureTrack::gpu, "pass", 7, 1.5);
  capture_value(CaptureTrack::cpu, "submit", 7, 0.25);
  capture_frame_end(7);
  capture_stop();
  CHECK(zoned_work(2) == 1);  // after the stop: nothing
  const std::vector<CaptureEvent> events = snapshot(info);
  CHECK_FALSE(info.running);
  CHECK(info.dropped == 0);
  CHECK(info.threads == 1);
  char name[k_capture_thread_name_bytes];
  capture_thread_name(0, name);
  CHECK(std::strcmp(name, "capture main") == 0);
  // mark, two iterations and the function's zone, two values, the end
  REQUIRE(events.size() == 7);
  CHECK(events[0].kind == CaptureKind::frame);
  CHECK(events[0].frame == 7);
  u32 iterations = 0, functions = 0;
  for (const CaptureEvent& e : events) {
    if (e.kind != CaptureKind::zone) continue;
    CHECK(e.end_ns >= e.begin_ns);
    CHECK(e.begin_ns >= events[0].begin_ns);
    if (std::strcmp(e.name, "capture tests: iteration") == 0) ++iterations;
    if (std::strstr(e.name, "zoned_work") != nullptr) ++functions;  // __func__
  }
  CHECK(iterations == 2);
  CHECK(functions == 1);
  CHECK(events[4].kind == CaptureKind::value);
  CHECK(events[4].track == CaptureTrack::gpu);
  CHECK(events[4].ms == 1.5);
  CHECK(events[5].track == CaptureTrack::cpu);
  CHECK(events[6].kind == CaptureKind::frame_end);
  capture_reset();
}

TEST_CASE("frame capture: a full buffer counts what it dropped, and a restart clears it") {
  capture_reset();
  CaptureOptions options;
  options.capacity = 4;
  REQUIRE(capture_start(options));
  for (u64 f = 0; f < 10; ++f)
    capture_frame(f);
  capture_stop();
  CaptureInfo info;
  std::vector<CaptureEvent> events = snapshot(info);
  CHECK(events.size() == 4);
  CHECK(info.dropped == 6);
  CHECK(events[3].frame == 3);

  REQUIRE(capture_start(options));  // the same buffer, cleared
  capture_frame(42);
  capture_stop();
  events = snapshot(info);
  REQUIRE(events.size() == 1);
  CHECK(events[0].frame == 42);
  CHECK(info.dropped == 0);
  capture_reset();
}

// Found by batch 68's review of the capture: a start kept a larger buffer from an earlier capture,
// so it dropped later than the caller was told, and the largest buffer anyone asked for was never
// given back.
TEST_CASE("frame capture: a start holds exactly the capacity asked, and says it") {
  capture_reset();
  CHECK(capture_capacity() == 0);
  CaptureOptions large;
  large.capacity = 1024;
  REQUIRE(capture_start(large));
  CHECK(capture_capacity() == 1024);
  capture_stop();
  CaptureOptions small;
  small.capacity = 64;
  REQUIRE(capture_start(small));
  CHECK(capture_capacity() == 64);
  for (u64 f = 0; f < 100; ++f)
    capture_frame(f);
  capture_stop();
  CaptureInfo info;
  const std::vector<CaptureEvent> events = snapshot(info);
  CHECK(events.size() == 64);  // the 65th was refused, where the caller was told it would be
  CHECK(info.dropped == 36);
  CHECK(info.full_ns >= 0);
  // The stopped capture stays readable until the next start or a reset, which gives it back.
  CHECK(capture_capacity() == 64);
  capture_reset();
  CHECK(capture_capacity() == 0);
}

TEST_CASE("frame capture: zones on several threads, and a stop that waits for its writers") {
  capture_reset();
  CaptureOptions options;
  options.capacity = 1u << 16;
  REQUIRE(capture_start(options));
  std::atomic<bool> go{true};
  std::atomic<u32> recorded{0};  // workers whose first zone has ended
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&go, &recorded, t] {
      char name[16] = "worker-0";
      name[7] = static_cast<char>('0' + t);
      ENGINE_PROFILE_THREAD(name);
      // One zone each before any spins, so a buffer the others fill cannot drop a late worker's
      // first event; then zones until told to stop.
      {
        ENGINE_PROFILE_ZONE_NAMED("capture tests: worker zone");
      }
      recorded.fetch_add(1);
      while (recorded.load() < 3)
        std::this_thread::yield();
      while (go.load(std::memory_order_relaxed)) {
        ENGINE_PROFILE_ZONE_NAMED("capture tests: worker zone");
      }
    });
  }
  // On a loaded machine a worker may not have run yet: the stop comes after every one has.
  while (recorded.load() < 3)
    std::this_thread::yield();
  for (u64 f = 0; f < 50; ++f) {
    capture_frame(f);
    std::this_thread::yield();
  }
  capture_stop();  // the workers are still running zones
  CaptureInfo info;
  const std::vector<CaptureEvent> first = snapshot(info);
  const std::vector<CaptureEvent> second = snapshot(info);
  CHECK(first.size() == second.size());  // nothing written after the stop returned
  go.store(false);
  for (std::thread& t : threads)
    t.join();
  CHECK(info.threads >= 2);
  u32 named = 0;
  for (u32 t = 0; t < info.threads; ++t) {
    char name[k_capture_thread_name_bytes];
    capture_thread_name(t, name);
    if (std::strncmp(name, "worker-", 7) == 0) ++named;
  }
  CHECK(named >= 1);
  u32 marks = 0;
  for (const CaptureEvent& e : first) {
    if (e.kind == CaptureKind::frame) ++marks;
    if (e.kind == CaptureKind::zone) CHECK(e.end_ns >= e.begin_ns);
  }
  CHECK(marks + info.dropped >= 50);
  capture_reset();
}

// A report of a running capture copies the thread names while threads make their first events and
// rename themselves (profiling.md, "Thread names"). Before the table was a sequence of atomic
// words, the copy read plain chars a thread was writing — a data race the thread sanitizer reports
// on this case — and could keep a torn name. Every name read here is whole: one of the names the
// threads gave, or empty.
TEST_CASE("frame capture: a name read while threads name themselves is whole or empty") {
  capture_reset();
  CaptureOptions options;
  options.capacity = 1u << 16;
  REQUIRE(capture_start(options));
  constexpr u32 k_workers = 8;
  std::atomic<bool> go{true};
  std::atomic<u32> named{0};  // workers that have named themselves and made their first event
  std::vector<std::thread> threads;
  for (u32 w = 0; w < k_workers; ++w) {
    threads.emplace_back([w, &go, &named] {
      const std::string a = "worker-" + std::to_string(w) + "-" + std::string(40, 'a');
      const std::string b = "worker-" + std::to_string(w) + "-" + std::string(40, 'b');
      capture_name_thread(a.c_str());
      u32 turn = 0;
      bool first = true;
      while (go.load(std::memory_order_relaxed)) {
        {
          ENGINE_PROFILE_ZONE_NAMED("capture tests: naming zone");
        }
        if (first) named.fetch_add(1);
        first = false;
        capture_name_thread((++turn & 1u) != 0 ? b.c_str() : a.c_str());
      }
    });
  }
  // The looks start at once, while the workers make their first events, and run 2000 times; on a
  // loaded machine the workers may not have run by then (T199: 2000 looks once ended before any
  // worker had a name, and `whole` was 0), so the looks go on until every worker has named itself
  // and one look has been taken after that, which must read names. A worker not scheduled in a
  // minute fails the case by name rather than passing it on no names at all.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  u32 whole = 0;
  bool all_named = false;
  for (u32 look = 0; look < 2000 || !all_named; ++look) {
    if (look >= 2000 && std::chrono::steady_clock::now() > until) break;
    all_named = named.load() == k_workers;  // read before the look, so the look comes after
    CaptureInfo info;
    (void)snapshot(info);
    for (u32 t = 0; t < info.threads; ++t) {
      char name[k_capture_thread_name_bytes];
      capture_thread_name(t, name);
      const std::string n(name);
      if (n.empty()) continue;
      // `worker-<w>-` and then one letter to the end (the names are cut to the table's width): a
      // torn name mixes the two.
      REQUIRE_MESSAGE(n.size() == k_capture_thread_name_bytes - 1, n);
      REQUIRE_MESSAGE(n.substr(0, 7) == "worker-", n);
      const usize dash = n.find('-', 7);
      REQUIRE_MESSAGE(dash != std::string::npos, n);
      const char letter = n[dash + 1];
      REQUIRE_MESSAGE(n.find_first_not_of(letter, dash + 1) == std::string::npos, n);
      ++whole;
    }
  }
  go.store(false);
  for (std::thread& th : threads)
    th.join();
  capture_stop();
  REQUIRE_MESSAGE(all_named, "a worker had not made its first event after a minute");
  CHECK(whole > 0);
  capture_reset();
}
