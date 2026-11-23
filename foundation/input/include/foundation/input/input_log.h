#pragma once

// An input log and its replay (docs/plan/05-simulation.md §5.10: "input log + event log =>
// replay. CI runs replays and asserts identical persistent-state hashes").
//
// The file is JSON lines. The first line is a header; every line after it is one event as an
// array with the tick first, so the file sorts and diffs by tick and stays small:
//
//     {"columns":["tick","source","code","value","device"],"map":123456789,
//      "type":"engine.input.log","version":1}
//     [0,"key",26,1.0,0]
//     [4,"key",26,0.0,0]
//     [4,"gamepad_axis",0,0.5,0]
//
// The header's `map` is the ActionMap hash the log was recorded against. Replaying into a state
// whose map hashes differently is refused rather than silently reinterpreted: the same
// scancode means a different action, so the replay would diverge from the session it came from.
//
// A header may also carry a `session`: one JSON object the *recording application* owns — what
// it was drawing, where its camera started, the tick rate, the version of whatever it computes
// from the events — which this module stores and hands back without reading. It is how
// `engine-view --record-input` makes a log self-describing (docs/subsystems/apps.md) without the
// input module learning what a camera is. A log without one writes no key, so the line is
// byte-for-byte what it was before sessions existed and the device corpus stays canonical.
//
//     InputLog log;
//     log.set_map(map);
//     for (const RawEvent& e : events) log.record(e);
//     log.save("replays/run.jsonl");
//     ...
//     InputState fresh(map);
//     log.replay(fresh, SimTick{0}, SimTick{600}, {.on_tick = &check, .user = &expected});

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/time/time.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::input {

// Bumped when the line format changes. A log from a newer version is refused. Version 2 (T216) is
// a journal's (`InputLogJournal`): version 1's lines and `{"through":<tick>}` lines between them.
// `save` still writes version 1, so a log saved whole is the bytes it always was.
inline constexpr u32 k_log_version = 2;
inline constexpr u32 k_saved_log_version = 1;
inline constexpr const char* k_log_type = "engine.input.log";

// The most ticks from a loaded log's first event to its last (T190): seven days at the 240 Hz a
// session ticks at by default (7 x 86,400 x 240). A replay walks every tick of its range, the
// empty ones too, so the span is work a file chooses; `load` refuses a longer one by name. The
// committed recordings span under 600.
inline constexpr u64 k_max_log_tick_span = 145'152'000;

// Called after every tick of a replay, with the state the tick ended in. Declared at namespace
// scope because a nested struct's default member initializers cannot serve as a default argument
// inside the enclosing class (GCC and Clang reject it).
using ReplayTickFn = void (*)(void* user, SimTick tick, const InputState& state);
struct ReplayOptions {
  ReplayTickFn on_tick = nullptr;
  void* user = nullptr;
};

// The tick loop a replay runs, for a caller that feeds a few ticks at a time — a frame's worth —
// rather than a whole log in one call: for every tick from `from` to `to` inclusive, `begin_tick`,
// every event of `events` stamped with that tick, `end_tick`, then `on_tick`. `events` must be in
// tick order; the search starts at `cursor`, skips anything stamped before `from`, and the return
// value is the index of the first event not fed, which is the next call's `cursor`. Nothing is fed
// when `to < from`.
//
// `InputLog::replay` is this function over the log's events, and a live session that stamps its
// events with the tick they will be fed at can call it on its own pending batch — which is how
// `engine-view --interactive` guarantees that a session and its replay run the same loop, not two
// loops that were meant to agree (docs/subsystems/apps.md, "The replay guarantee").
u32 feed_ticks(InputState& state, std::span<const RawEvent> events, u32 cursor, SimTick from,
               SimTick to, const ReplayOptions& options = {});

class InputLog {
 public:
  InputLog() = default;

  // The map the events are being recorded against; its hash goes in the header.
  void set_map(const ActionMap& map) noexcept { map_hash_ = map.hash(); }
  void set_map_hash(u64 value) noexcept { map_hash_ = value; }
  u64 map_hash() const noexcept { return map_hash_; }

  // The recording application's own header block (see the file comment): an object, or null for
  // none. Saved under `session` and loaded back as it was; this module never reads it.
  void set_session(JsonValue session) { session_ = std::move(session); }
  const JsonValue& session() const noexcept { return session_; }

  // The refusal `replay` makes, on its own: false, with both hashes in `error`, when `state` has
  // no map or its map hashes differently from the one this log was recorded against. A caller
  // that feeds the log a frame at a time through `feed_ticks` checks this once, before the first.
  bool check_map(const InputState& state, std::string* error = nullptr) const;

  // Appends. Ticks must not go backwards; replay walks the events with a single cursor.
  void record(const RawEvent& event);
  void clear() noexcept;
  std::span<const RawEvent> events() const noexcept { return {events_.data(), events_.size()}; }
  u32 size() const noexcept { return events_.size(); }
  bool empty() const noexcept { return events_.empty(); }
  // The range of ticks the log covers; from > to when it is empty.
  SimTick first_tick() const noexcept;
  SimTick last_tick() const noexcept;
  // A journal's last `through` line: the tick its recorder had reached when it last wrote, past
  // the last event when nothing was pressed since (T216). Zero for a log saved whole, which has
  // none; `has_through` tells the two apart.
  SimTick through() const noexcept { return through_; }
  bool has_through() const noexcept { return has_through_; }

  io::Status save(std::string_view native_path) const;
  // Replaces the log's contents. On failure the log is left empty and `error` says why.
  io::Status load(std::string_view native_path, std::string* error = nullptr);

  using TickFn = ReplayTickFn;

  // Feeds the recorded events into `state` tick by tick, from `from` to `to` inclusive,
  // including the ticks with no events (the state still ages: a press stops being new).
  // False, with a message, when the state has no map or its map hashes differently.
  bool replay(InputState& state, SimTick from, SimTick to, const ReplayOptions& options = {},
              std::string* error = nullptr) const;

 private:
  void forget() noexcept;

  Vector<RawEvent> events_;
  u64 map_hash_ = 0;
  JsonValue session_;
  SimTick through_{0};
  bool has_through_ = false;
};

// **A recording that reaches the disk as it is made** (roadmap T216; docs/subsystems/input.md,
// "A journal"). A live session holds its `InputLog` in memory and saves it whole at its end; a
// process that dies first took the whole log with it, which is what happened to the owner's walk
// of 2026-11-22. A journal is the same file written as it goes: the header when the session starts
// (version 2), then every `append` the events recorded since the last and one `{"through":<tick>}`
// line naming the tick the session had reached, so a replay of a journal runs to that tick
// (`InputLog::through`) even when nothing was pressed after the last event.
//
//     {"columns":[...],"map":123,"session":{...},"type":"engine.input.log","version":2}
//     [0,"key",26,1.0,0]
//     {"through":240}
//     [252,"key",26,0.0,0]
//     {"through":480}
//
// **What an append costs.** No allocation: each line is formatted into a stack buffer and written
// through the file the journal holds open, then flushed to the OS, so a process that dies after
// `append` returns loses none of it (the OS's cache holds it; a machine that loses power may). The
// file is opened once, by `open`, and closed by `close` or the destructor; `InputLog::save` to the
// same path replaces it whole once it is closed, which is how a session's normal end leaves the
// version-1 bytes it always left.
class InputLogJournal {
 public:
  InputLogJournal() = default;
  ~InputLogJournal();
  ENGINE_NON_COPYABLE(InputLogJournal);

  // Creates (or truncates) the file and writes the header: the map's hash and the recording
  // application's session block, as `InputLog::save` writes them, at version 2.
  io::Status open(std::string_view native_path, u64 map_hash, const JsonValue& session);
  // Writes `events` (in tick order, ticks not before the last append's) and a through line.
  io::Status append(std::span<const RawEvent> events, SimTick through) noexcept;
  void close() noexcept;
  bool is_open() const noexcept { return file_ != nullptr; }
  // What has been written: events and through lines, and the last tick a through line named.
  u64 events_written() const noexcept { return events_written_; }
  u64 appends() const noexcept { return appends_; }
  SimTick through() const noexcept { return through_; }

 private:
  void* file_ = nullptr;  // a std::FILE*
  u64 events_written_ = 0;
  u64 appends_ = 0;
  SimTick through_{0};
  bool failed_ = false;
};

}  // namespace engine::input
