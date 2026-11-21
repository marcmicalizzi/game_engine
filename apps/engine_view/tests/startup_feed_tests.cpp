// The startup trace's feed (startup.h, `StartupTrace::feed`; apps.md, "--startup-trace: from
// launch to ready"), with no device and no window (T211). A window under `--game` is ready at
// `player_controllable`, on the frame its ground has collision, and its loop stopped feeding the
// trace there, so the thirty stable frames `nearby_stable` waits for were never counted and every
// windowed report carried that milestone null. The window loop cannot be run by a test (no
// engine-view starts without `--offscreen` here), so the decision it makes every frame is
// `feed`'s, and this case drives `feed` the way that loop does: a ready rung reached at frame k,
// the hand-over there, and the feed going on to `nearby_stable` thirty frames later.

#include "../startup.h"

#include <core/json/json_value.h>
#include <core/profiling/startup_trace.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>
#include <string_view>

using namespace engine;

namespace {

// The milestone `name`'s entry of a summary block's list, or null.
const JsonValue* milestone_of(const JsonValue& block, std::string_view name) {
  const JsonValue* list = block.find("milestones");
  if (list == nullptr || !list->is_array()) return nullptr;
  for (const JsonValue& m : list->as_array()) {
    const JsonValue* n = m.find("name");
    std::string_view s;
    if (n != nullptr && n->get_string(s) && s == name) return &m;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("engine-view: a loop feeds the startup trace past its ready rung to nearby_stable") {
  const test::TempDir tmp("engine_view_startup_feed");
  const std::string path = tmp.file("trace.json");
  view::StartupTrace trace;  // its own recorder, not the process's (`startup_trace()`)
  std::string error;
  REQUIRE_MESSAGE(trace.start(path, profiling::trace_now_ns(), &error), error);
  CHECK(trace.wants_frames());

  // The window loop's shape: frame 0 has drawn nothing; from frame 1 a frame is done; the walker
  // ticks and its ground has collision from frame k, and the neighbourhood is settled from then.
  constexpr u32 k = 7;
  constexpr u32 frames = k + view::k_stable_frames + 40;
  const char* const rung = "player_controllable";  // a `--game` window's ready rung
  bool load_ready = false;
  u32 ready_frame = 0;
  u32 reads = 0;
  u32 stable_frame = 0;
  for (u32 f = 0; f < frames; ++f) {
    trace.feed([&]() {
      ++reads;
      view::Readiness r;
      r.frame_done = f > 0;
      r.controllable = f >= k;
      r.collision = f >= k;
      r.world_settled = f >= k;
      r.pages_settled = f >= k;
      r.ground_settled = f >= k;
      return r;
    });
    if (!load_ready && trace.reached(rung)) {
      load_ready = true;  // the hand-over; the loop's feed goes on
      ready_frame = f;
      // The defect: here the old loop stopped feeding, with the last milestone still ahead.
      CHECK_FALSE(trace.reached("nearby_stable"));
      CHECK(trace.reached("ground_collision_ready"));
    }
    if (stable_frame == 0 && trace.reached("nearby_stable")) stable_frame = f;
  }
  CHECK(load_ready);
  CHECK(ready_frame == k);
  // Held thirty frames, the first of them frame k: reached on frame k + 29.
  CHECK(stable_frame == k + view::k_stable_frames - 1);
  // Read every frame up to that one and on no frame after it: the cost stops with the milestone.
  CHECK(reads == stable_frame + 1);
  CHECK_FALSE(trace.wants_frames());

  const JsonValue block = trace.finish(&error);
  CHECK_MESSAGE(error.empty(), error);
  REQUIRE(block.is_object());
  const JsonValue* stable = milestone_of(block, "nearby_stable");
  REQUIRE(stable != nullptr);
  const JsonValue* ms = stable->find("ms");
  REQUIRE(ms != nullptr);
  CHECK_FALSE(ms->is_null());
  const JsonValue* ready = milestone_of(block, rung);
  REQUIRE(ready != nullptr);
  CHECK_FALSE(ready->find("ms")->is_null());
  bool reached = false;
  REQUIRE(block.find("until_reached")->get_bool(reached));
  CHECK(reached);
  CHECK_FALSE(block.find("dropped_before_ready")->is_null());
}

TEST_CASE("engine-view: with neither the trace nor the progress on, a loop reads no readiness") {
  if (profiling::progress_enabled()) {
    MESSAGE("the live progress is on in this process");
    return;
  }
  view::StartupTrace trace;
  CHECK_FALSE(trace.wants_frames());
  u32 reads = 0;
  for (u32 f = 0; f < 10; ++f)
    CHECK_FALSE(trace.feed([&]() {
      ++reads;
      return view::Readiness{};
    }));
  CHECK(reads == 0);
}
