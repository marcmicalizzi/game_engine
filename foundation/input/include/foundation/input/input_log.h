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
#include <core/time/time.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::input {

// Bumped when the line format changes. A log from a newer version is refused.
inline constexpr u32 k_log_version = 1;
inline constexpr const char* k_log_type = "engine.input.log";

class InputLog {
 public:
  InputLog() = default;

  // The map the events are being recorded against; its hash goes in the header.
  void set_map(const ActionMap& map) noexcept { map_hash_ = map.hash(); }
  void set_map_hash(u64 value) noexcept { map_hash_ = value; }
  u64 map_hash() const noexcept { return map_hash_; }

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

  // Called after every tick of a replay, with the state the tick ended in.
  using TickFn = void (*)(void* user, SimTick tick, const InputState& state);
  struct ReplayOptions {
    TickFn on_tick = nullptr;
    void* user = nullptr;
  };

  // Feeds the recorded events into `state` tick by tick, from `from` to `to` inclusive,
  // including the ticks with no events (the state still ages: a press stops being new).
  // False, with a message, when the state has no map or its map hashes differently.
  bool replay(InputState& state, SimTick from, SimTick to, const ReplayOptions& options = {},
              std::string* error = nullptr) const;

 private:
  Vector<RawEvent> events_;
  u64 map_hash_ = 0;
};

}  // namespace engine::input
