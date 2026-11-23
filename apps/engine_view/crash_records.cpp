// T216's pieces both frame loops and the crash hook share (session_records.h; apps.md, "When the
// process faults"): the loop's place, the test seam, the recording's journal and the `crashed`
// block. A file of its own, apart from the handlers' writer, so the no-device cases compile it
// with few neighbours (tests/frame_records_tests.cpp).
#include "session_records.h"

#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/profiling/startup_trace.h>
#include <core/time/time.h>
#include <domain/gfx/test_profile.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine::view {

namespace {

std::string hex_of(u64 value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

}  // namespace
// ---- T216
// ----------------------------------------------------------------------------------------

LoopState& loop_state() {
  static LoopState state;
  return state;
}

TestFaultAt parse_test_fault(std::string_view text) noexcept {
  TestFaultAt out;
  u64 frame = 0;
  usize i = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
    frame = frame * 10 + static_cast<u64>(text[i] - '0');
  if (i == 0) return out;
  const std::string_view kind = i < text.size() && text[i] == ':' ? text.substr(i + 1) : "";
  if (i < text.size() && text[i] != ':') return out;
  if (kind.empty() || kind == "access") {
    out.kind = TestFault::access;
  } else if (kind == "verify") {
    out.kind = TestFault::verify;
  } else {
    return out;
  }
  out.frame = frame;
  return out;
}

namespace {

std::string environment(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

// The seam, read once under the test-main rule (test_profile.h's `judge_test_profile`).
TestFaultAt test_fault() {
  static const TestFaultAt at = [] {
    const std::string text = environment(k_test_fault_env);
    if (text.empty()) return TestFaultAt{};
    const std::string run = environment(gfx::k_test_run_env);
    std::error_code temp_error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(temp_error);
    const std::string root = temp_error ? std::string() : (temp / "engine-tests").string();
    std::error_code run_error;
    const bool exists = !run.empty() && std::filesystem::is_directory(run, run_error);
    if (gfx::judge_test_profile(text, run, root, exists) != gfx::TestProfile::honoured) {
      std::fprintf(stderr,
                   "engine-view: %s is set and ignored: it is honoured only under a live test's "
                   "main\n",
                   k_test_fault_env);
      return TestFaultAt{};
    }
    const TestFaultAt parsed = parse_test_fault(text);
    if (parsed.kind == TestFault::none)
      std::fprintf(stderr, "engine-view: %s=%s is not <frame>[:access|verify]\n", k_test_fault_env,
                   text.c_str());
    return parsed;
  }();
  return at;
}

volatile uintptr_t g_bad_address = 0x10;
volatile int g_never = 0;

}  // namespace

void loop_at(u64 frame, u64 tick) noexcept {
  LoopState& s = loop_state();
  s.frame.store(frame, std::memory_order_relaxed);
  s.tick.store(tick, std::memory_order_relaxed);
  s.doing.store("frame", std::memory_order_relaxed);
  const TestFaultAt fault = test_fault();
  if (fault.kind == TestFault::none || fault.frame != frame) return;
  std::fprintf(stderr, "engine-view: the test seam faults at frame %llu\n",
               static_cast<unsigned long long>(frame));
  std::fflush(stderr);
  if (fault.kind == TestFault::verify) ENGINE_VERIFY(g_never != 0, "the test seam's verify");
  *reinterpret_cast<volatile int*>(g_bad_address) = 1;  // a real access violation, on this thread
}

bool journal_step(RecordingJournal& j, input::InputLog& recording, const LostRecords& held,
                  u64 tick, u64 marks) {
  if (held.record_input.empty() || j.failed) return false;
  const i64 start = time::monotonic_ns();
  if (!j.file.is_open()) {
    // The header as the normal end would stamp it now (the map, the session block); its `ticks` is
    // the tick reached at the open, and the through lines carry it on from there.
    if (held.finish_recording != nullptr) held.finish_recording(held.finish_context, recording);
    if (j.file.open(held.record_input, recording.map_hash(), recording.session()) !=
        io::Status::Ok) {
      j.failed = true;
      std::fprintf(stderr,
                   "engine-view: cannot write the input journal %s; the recording is saved only "
                   "at the session's end\n",
                   held.record_input.c_str());
      return false;
    }
    j.last_tick = tick;
  }
  const bool mark = marks != j.marks_seen;
  if (!mark && tick < j.last_tick + j.every_ticks) return false;
  const std::span<const input::RawEvent> events = recording.events();
  const u32 from = j.written <= events.size() ? j.written : static_cast<u32>(events.size());
  if (j.file.append(events.subspan(from), SimTick{tick}) != io::Status::Ok) {
    j.failed = true;
    std::fprintf(stderr, "engine-view: the input journal %s stopped: a write failed\n",
                 held.record_input.c_str());
    return false;
  }
  if (mark) {
    std::fprintf(stderr, "engine-view: mark %llu at tick %llu, in the input journal\n",
                 static_cast<unsigned long long>(marks), static_cast<unsigned long long>(tick));
  }
  j.written = static_cast<u32>(events.size());
  j.last_tick = tick;
  j.marks_seen = marks;
  const i64 took = time::monotonic_ns() - start;
  ++j.appends;
  j.append_ns += took;
  if (took > j.worst_append_ns) j.worst_append_ns = took;
  return true;
}

std::string journal_summary(const RecordingJournal& j) {
  char line[256];
  std::snprintf(
      line, sizeof(line),
      "engine-view: the input journal: %llu appends, %llu events, mean %.1f us, worst "
      "%.1f us%s",
      static_cast<unsigned long long>(j.appends),
      static_cast<unsigned long long>(j.file.events_written()),
      j.appends > 0 ? static_cast<f64>(j.append_ns) / 1e3 / static_cast<f64>(j.appends) : 0.0,
      static_cast<f64>(j.worst_append_ns) / 1e3, j.failed ? " (stopped)" : "");
  return line;
}

JsonValue crashed_json(const platform::CrashInfo& info, std::span<const scene::FrameRecord> records,
                       const Vector<FlyMarker>* marks) {
  const LoopState& s = loop_state();
  JsonValue out = JsonValue::object();
  const u64 frame = s.frame.load(std::memory_order_relaxed);
  out.set("frame", frame == ~u64{0} ? JsonValue() : JsonValue(frame));
  out.set("tick", s.tick.load(std::memory_order_relaxed));
  out.set("doing", std::string(s.doing.load(std::memory_order_relaxed)));
  // The load's stage running then, or the last one begun (startup_trace.h's progress record): what
  // a fault before the first frame was in the middle of.
  profiling::LoadProgress progress;
  profiling::progress_read(progress);
  out.set("load_stage", std::string(progress.stage));
  out.set("load_detail", std::string(progress.detail));
  out.set("what", std::string(info.what));
  out.set("code", hex_of(info.code));
  out.set("address", hex_of(info.address));
  out.set("access", std::string(info.access));
  out.set("at", std::string(info.at));
  JsonValue stack = JsonValue::array();
  for (u32 i = 0; i < info.stack_count; ++i)
    stack.push_back(JsonValue(std::string(info.stack[i])));
  out.set("stack", std::move(stack));
  out.set("thread", info.thread_id);
  out.set("thread_name", std::string(info.thread_name));
  out.set("main_thread", info.main_thread);
  if (info.expression[0] != '\0' || info.file[0] != '\0') {
    JsonValue a = JsonValue::object();
    a.set("expression", std::string(info.expression));
    a.set("message", std::string(info.message));
    a.set("file", std::string(info.file));
    a.set("line", static_cast<i64>(info.line));
    out.set("assertion", std::move(a));
  } else {
    out.set("assertion", JsonValue());
  }
  out.set("crash_file", std::string(info.crash_file));
  out.set("line", std::string(info.line_text));
  out.set("frames_recorded", static_cast<u64>(records.size()));
  out.set("last_recorded_frame",
          records.empty() ? JsonValue() : JsonValue(records[records.size() - 1].frame));
  JsonValue marked = JsonValue::array();
  if (marks != nullptr) {
    for (const FlyMarker& m : *marks)
      marked.push_back(JsonValue(m.tick));
  }
  out.set("marks", std::move(marked));
  return out;
}

}  // namespace engine::view
