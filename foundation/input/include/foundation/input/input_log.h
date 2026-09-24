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

// Bumped when the line format changes. A log from a newer version is refused.
inline constexpr u32 k_log_version = 1;
inline constexpr const char* k_log_type = "engine.input.log";

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
};

}  // namespace engine::input
