#pragma once

// The one recorded door: every input to the simulation that the simulation did not compute — a
// player's input, a client's action between ticks, an external source's raise — reaches the world
// through it, stamped with the tick it reaches the world at, recorded, and in a replay handed back
// at that tick from the record and from nothing else (docs/plan/05-simulation.md §5.10, plan 03
// §3.10; docs/subsystems/sim.md, "The door"; roadmap S18).
//
// **Why one door.** A replay is the recorded inputs played into the same world, so a replay is only
// as good as the set of inputs it knows about. An input that reaches the world any other way — a
// value a host samples when a frame begins, an edit applied between two calls, a flush at the end
// of a call — is one the replay cannot know, and the run it reproduces is then a function of how
// the host's frames and calls happened to fall. The door makes the tick the only clock an input
// carries: what a host submits between ticks is stamped with the next tick to run, whatever frame,
// call or stall it came in, and the world reads it at that tick.
//
// **Two deliveries.**
//   - `Tick`: the door holds the entry and hands it over at the start of its tick's `Input` phase,
//     live and replayed alike. A player's input is this kind: the door feeds an `input::InputState`
//     (`set_input_state`) at every tick — begin, the tick's events in submission order, end — so a
//     game's systems read the input in the `Input` phase and never at frame time.
//   - `Between`: the host has already applied it, between two ticks (a client's call ending with a
//     write-back flush, an agent's transaction, an external source's raise on the event bus). Live,
//     the door only records it; in a replay the door applies it through its kind's handler at the
//     same place — after the tick before it, before anything of its tick — which is the start of
//     that tick's `Input` phase for a replay stepping the scheduler, and `replay_pending` for the
//     ones after the replay's last tick.
//
// **Replay.** `begin_replay` turns the door around: the entries it holds are the record, a live
// submission is refused (and counted), and a tick's entries are delivered whatever the replay's own
// frames, calls or step pattern were. A host that ends a call in a replay does not flush: the
// record says where the recorded run flushed, and the door does it there (`replaying()`).
//
// **Cost.** Nothing unless a host makes one. When on: 32 bytes an entry and its payload (13 bytes
// for a player's input event), nothing per tick but the input state's begin and end, and nothing
// per entity. The record grows for the session's length in memory; `serialize` writes it, and
// `systems/world` writes it into the store's event log (`append_door_events`).
//
// Single-threaded: the host submits from the thread that steps the scheduler.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/sim/scheduler.h>
#include <foundation/input/input.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::sim {

// Where an input came from. The numbers are the store's `EventOrigin` (foundation/store), so an
// entry is a store event with a cast; `Deterministic` (0) is the simulation's own and never comes
// through the door.
enum class DoorOrigin : u8 {
  Player = 1,
  Agent = 2,  // a protocol client: an agent, an editor, a script calling the session
  Llm = 3,
  Debug = 4,
};
const char* door_origin_name(DoorOrigin origin) noexcept;

enum class DoorDelivery : u8 {
  Tick = 0,     // held and handed over at the start of its tick's Input phase
  Between = 1,  // applied by the host when it happened; replayed at the start of its tick
};

// One entry. Laid over bytes on disk (the door's file and the store's payload), so it has no
// indeterminate byte: `reserved` is written zero.
struct DoorEntry {
  // The tick the entry reaches the world at: a `Tick` entry is delivered in it, a `Between` entry
  // happened after `tick - 1` ran and before `tick` began.
  u64 tick = 0;
  // The game time when it was submitted: the time after `tick - 1`, which is what a handler that
  // stands for a host's call (a flush) stamps its work with.
  i64 game_time_us = 0;
  u32 kind = 0;  // `schema::stable_type_id` of what it carries
  u32 payload_offset = 0;
  u32 payload_size = 0;
  DoorOrigin origin = DoorOrigin::Player;
  DoorDelivery delivery = DoorDelivery::Tick;
  u16 reserved = 0;
};

// A player's input event: `stable_type_id("engine.input.RawEvent")`.
u32 door_input_kind() noexcept;
// A client's write-back flush between ticks: `stable_type_id("engine.sim.Flush")`. Empty payload.
u32 door_flush_kind() noexcept;

// What a kind's entries do when the door hands them over (a `Tick` entry, live and replayed, or a
// `Between` entry in a replay).
struct DoorHandler {
  void* context = nullptr;
  void (*apply)(void* context, const DoorEntry& entry, std::span<const u8> payload) = nullptr;
};

struct DoorStats {
  u64 submitted = 0;     // live entries taken
  u64 delivered = 0;     // Tick entries handed over (input fed, or a handler called)
  u64 replayed = 0;      // Between entries applied in a replay
  u64 refused_live = 0;  // live submissions refused because the door was replaying
  u64 unhandled = 0;     // entries handed over with no handler for their kind
  u64 input_ticks = 0;   // ticks the input state was fed
};

class Door {
 public:
  Door() = default;
  ENGINE_NON_COPYABLE(Door);

  // Adds the door's system to `scheduler`: an `Input` phase `begin_tick`, so it runs before every
  // other system of the phase that was registered after it — install the door first. Once; the
  // scheduler must outlive the door's use and the door the scheduler's steps.
  void install(SimScheduler& scheduler);
  bool installed() const noexcept { return scheduler_ != nullptr; }

  // The state the door feeds every tick. The state (and its map) must outlive the door's use.
  void set_input_state(input::InputState* state) noexcept { input_ = state; }
  const input::InputState* input_state() const noexcept { return input_; }
  // The handler for a kind; a second call for one kind replaces it.
  void on(u32 kind, const DoorHandler& handler);

  // ---- live ------------------------------------------------------------------------------------

  // A player's input event, delivered at the next tick to run (`scheduler.tick() + 1`); the event's
  // own tick is ignored and set at delivery. False when the door is replaying.
  bool submit_input(const input::RawEvent& event, DoorOrigin origin = DoorOrigin::Player);
  // Any entry, `Tick` or `Between`, at the next tick to run. False when the door is replaying, or
  // when the payload would take the door past 4 GiB.
  bool submit(DoorOrigin origin, DoorDelivery delivery, u32 kind, std::span<const u8> payload);
  // A `Between` entry the host has applied (`submit` with `Between`).
  bool record_between(DoorOrigin origin, u32 kind, std::span<const u8> payload = {}) {
    return submit(origin, DoorDelivery::Between, kind, payload);
  }

  // ---- replay ----------------------------------------------------------------------------------

  // The entries held become the record: live submissions are refused from now on, and every entry
  // is delivered from the record at its tick. Entries of ticks the scheduler has already run are
  // skipped (a replay starts where its record starts).
  void begin_replay();
  bool replaying() const noexcept { return replaying_; }
  // Applies, in a replay, every `Between` entry of a tick up to `scheduler.tick() + 1` not yet
  // applied: what the recorded run did after the replay's last tick (a call's final flush). A host
  // calls it before it hashes, saves or hands the world back. Returns how many.
  u32 replay_pending();

  // ---- the record ------------------------------------------------------------------------------

  std::span<const DoorEntry> entries() const noexcept { return {entries_.data(), entries_.size()}; }
  std::span<const u8> payload(const DoorEntry& entry) const noexcept;
  // Appends an entry read from a record (a file, the store's log), before `begin_replay`. False
  // when its tick is before the last entry's, its origin or delivery is not one this build knows.
  bool add_recorded(const DoorEntry& entry, std::span<const u8> payload);
  void clear() noexcept;
  // Bytes held: the entries and their payloads.
  usize memory_bytes() const noexcept;
  // A hash of every entry handed over so far (`Tick` delivered, `Between` recorded live or applied
  // in a replay), in order: a `TickHashSource` for the hash log, so an input that differs between
  // two runs shows as the table "door" at its tick.
  u64 delivered_hash() const noexcept { return delivered_hash_; }
  const DoorStats& stats() const noexcept { return stats_; }

  // The file form: `ENGDOOR1`, little-endian, the entries then the payload bytes, every entry
  // checked against the file's size and the order on read. `load` replaces what the door holds and
  // leaves it live (call `begin_replay`); on failure it is left empty and `error` says why.
  void serialize(std::string& out) const;
  bool deserialize(std::string_view bytes, std::string* error = nullptr);
  bool save(std::string_view native_path, std::string* error = nullptr) const;
  bool load(std::string_view native_path, std::string* error = nullptr);

  // A player's input event as the door's payload, and back.
  static void encode_input(const input::RawEvent& event, u8 (&out)[13]) noexcept;
  static bool decode_input(std::span<const u8> payload, input::RawEvent& out) noexcept;

 private:
  static void input_phase(SystemContext& context);
  void deliver(u64 tick);
  void apply(const DoorEntry& entry);
  void mix(const DoorEntry& entry);
  const DoorHandler* handler(u32 kind) const noexcept;

  SimScheduler* scheduler_ = nullptr;
  input::InputState* input_ = nullptr;
  Vector<DoorEntry> entries_;
  Vector<u8> payload_;
  struct KindHandler {
    u32 kind = 0;
    DoorHandler handler;
  };
  Vector<KindHandler> handlers_;
  u32 tick_cursor_ = 0;     // the next entry the Input phase looks at
  u32 between_cursor_ = 0;  // the next Between entry a replay has not applied
  bool replaying_ = false;
  u64 delivered_hash_ = 0;
  DoorStats stats_;
};

}  // namespace engine::sim
