#include <core/profiling/frame_capture.h>
#include <core/profiling/profile.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
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
  CHECK(std::strcmp(capture_thread_name(0), "capture main") == 0);
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
      { ENGINE_PROFILE_ZONE_NAMED("capture tests: worker zone"); }
      recorded.fetch_add(1);
      while (recorded.load() < 3) std::this_thread::yield();
      while (go.load(std::memory_order_relaxed)) {
        ENGINE_PROFILE_ZONE_NAMED("capture tests: worker zone");
      }
    });
  }
  // On a loaded machine a worker may not have run yet: the stop comes after every one has.
  while (recorded.load() < 3) std::this_thread::yield();
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
  for (u32 t = 0; t < info.threads; ++t)
    if (std::strncmp(capture_thread_name(t), "worker-", 7) == 0) ++named;
  CHECK(named >= 1);
  u32 marks = 0;
  for (const CaptureEvent& e : first) {
    if (e.kind == CaptureKind::frame) ++marks;
    if (e.kind == CaptureKind::zone) CHECK(e.end_ns >= e.begin_ns);
  }
  CHECK(marks + info.dropped >= 50);
  capture_reset();
}
