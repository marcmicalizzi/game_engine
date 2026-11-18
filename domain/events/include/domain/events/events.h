#pragma once

// The world event bus (docs/plan/05-simulation.md §5.7, roadmap S12; docs/subsystems/events.md).
//
// Persistent state changes are events and systems react to them rather than polling (05 §5.1).
// This module is the mechanism: events are schema types (`@kind(event)`), raised by systems, by
// registered sources (a flecs system, the timing wheel's sink, the host) and by the authoring
// document's change feed, and delivered to subscribers **inside the scheduler's phases**, at two
// points a tick: the start of `EventsIn` and the start of `EventsOut`.
//
// **Order.** Raises from a parallel wave land in whatever order the jobs finish; the bus never
// delivers in that order. Every event carries a key — the tick it was raised in, the phase, its
// source, a raiser-chosen 64-bit key and the raiser's own sequence — and each delivery point sorts
// what it delivers by that key, so the same world and the same inputs give the same deliveries on
// every toolchain and at every thread count. Delivery itself is serial, subscriber by subscriber
// in registration order, each seeing its events in key order; what a handler raises is the next
// generation of the same point, in the order the handlers raised it.
//
// **Runaway guards** (05 §5.7), each deterministic, each counted in the tick's `TickReport`:
//   - depth: an event raised past `max_depth` is dropped at the raise;
//   - cascade: past `max_cascade` events of depth one or more delivered in a tick, the rest are
//     carried to the next tick;
//   - storm: past `max_per_source` events of one source delivered in a tick, that source's rest are
//     carried to the next tick;
//   - self-loop: an event a subscriber raises of a type it subscribes to waits for the next tick
//     (no zero-delay self-loops);
//   - budget: past its delivery budget in a tick, a subscriber's events are skipped for it and its
//     `overflow` hook told how many;
//   - full: a raise or a carry that finds its bank full is dropped and counted, never silently.
//     What a full live bank keeps is the lowest keys of everything raised since the last delivery
//     point that fit, whatever order a parallel wave's raises arrived in (`EventBus::append`).
// A subscriber's wall time is measured and a slow one reported, and only reported: acting on a
// clock would make the simulation a function of the machine.
//
// **No allocation in a steady tick.** Two banks of records and payload bytes, sized once from the
// config (`capacity`, `payload_bytes`; the tunables `sim.events.*`), and every scratch array the
// delivery uses is reserved when a source or a subscriber is added.
//
// Determinism stance (ADR-0010): `hashed`. LOD policy: the bus has no tier of its own; an event
// carries the tile it happened in and a subscriber filters by it, and a summarized interval raises
// no events (docs/subsystems/sim.md, "The contract every LOD2/LOD3 system must meet", condition 4).

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/schema/type_info.h>
#include <core/time/time.h>
#include <domain/sim/scheduler.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <type_traits>

namespace engine::doc {
class Document;
}

namespace engine::events {

// An event that names no tile happened nowhere in particular: every subscriber's tile filter
// accepts it.
inline constexpr u64 k_any_tile = ~u64{0};
// Depth is a byte, and the histogram's last bucket holds everything at or past it.
inline constexpr u32 k_depth_buckets = 16;
// The chains the report names, largest first.
inline constexpr u32 k_top_chains = 4;
inline constexpr u16 k_invalid_type = 0xFFFFu;

// An event's identity: the tick its delivery order was fixed in, and its place in that tick's
// order. Zero is no event (a root's `cause`).
struct EventId {
  u64 value = 0;

  constexpr bool is_null() const noexcept { return value == 0; }
  constexpr auto operator<=>(const EventId&) const noexcept = default;
};

// Who raised an event, which is the first part of its order after the tick and the phase.
enum class Origin : u8 {
  Source = 0,      // a source registered by name (`EventBus::add_source`)
  System = 1,      // a scheduler table system, by its index (`EventBus::emitter(context, batch)`)
  Subscriber = 2,  // a subscriber's handler: a consequence
  Document = 3,    // the document's change feed
};

constexpr u32 make_source(Origin origin, u32 index) noexcept {
  return (static_cast<u32>(origin) << 24) | (index & 0x00FF'FFFFu);
}
constexpr Origin source_origin(u32 source) noexcept { return static_cast<Origin>(source >> 24); }
constexpr u32 source_index(u32 source) noexcept { return source & 0x00FF'FFFFu; }

// One event as the bus keeps it: 64 bytes, one per event in a bank, with its payload in the bank's
// byte arena. It has no padding, because records are compared and hashed (docs/subsystems/sim.md,
// "Invariants": no indeterminate bytes); the size table holds it at 64.
struct EventRecord {
  u64 tick = 0;   // the tick it was raised in
  EventId id;     // set when its delivery order is fixed
  EventId cause;  // the event whose handler raised it; null for a root
  EventId root;   // the root of its chain; itself for a root
  u64 key = 0;    // the raiser's stable key (a batch, an entity, a record's position)
  u64 tile = k_any_tile;
  u32 source = 0;   // `make_source(origin, index)`
  u32 seq = 0;      // the raiser's own count of its raises
  u32 payload = 0;  // byte offset in the bank's arena; its size is the type's
  u16 type = k_invalid_type;
  u8 phase = 0;  // the `sim::TickPhase` it was raised in
  u8 depth = 0;  // 0 for a root
};

// A registered event type, typed so a raise and a read cannot disagree about the payload.
template <class T>
struct EventType {
  u16 index = k_invalid_type;
  bool valid() const noexcept { return index != k_invalid_type; }
};

// What a handler is handed: the record and its payload, both valid for the call.
struct Event {
  const EventRecord* record = nullptr;
  const void* payload = nullptr;

  u16 type() const noexcept { return record->type; }
  template <class T>
  const T& as(EventType<T> as_type) const noexcept {
    ENGINE_ASSERT(as_type.index == record->type, "events::Event::as: wrong type");
    (void)as_type;
    return *static_cast<const T*>(payload);
  }
};

class EventBus;

struct RaiseOptions {
  u64 tile = k_any_tile;
};

// Raises events with one source's identity and its own sequence. **One emitter, one thread**: an
// emitter is a stack object made per invocation (a system's batch, a handler's call), and two
// threads raising through one would race on its sequence. Raising is thread-safe across emitters.
class Emitter {
 public:
  Emitter() = default;

  template <class T>
  bool raise(EventType<T> type, const T& value, const RaiseOptions& options = {}) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "events: an event payload is copied as bytes, so it is trivially copyable");
    static_assert(sizeof(T) <= 0xFFFFu, "events: a payload is at most 64 KiB");
    return raise_bytes(type.index, &value, static_cast<u32>(sizeof(T)), alignof(T), options);
  }
  // False when the raise was refused: the bank was full, the depth past the cap, no bus, or an
  // unknown type. A refusal is counted in the tick's report either way. True when the bank took
  // it; while the bank is full a later raise with a lower key can still displace it before the
  // delivery point, and that is counted as a refusal too.
  bool raise_bytes(u16 type, const void* bytes, u32 size, u32 align, const RaiseOptions& options);

  u32 source() const noexcept { return source_; }
  u32 raised() const noexcept { return seq_; }

 private:
  friend class EventBus;
  EventBus* bus_ = nullptr;
  u64 key_ = 0;
  EventId cause_;  // a handler's emitter: the event being handled, and its chain's root
  EventId root_;
  u32 source_ = 0;
  u32 seq_ = 0;
  u16 subscriber_ = 0xFFFFu;  // a handler's emitter: whose
  u8 phase_ = 0;
  u8 depth_ = 0;  // the depth of what it raises
};

// A subscriber, as data (ADR-0027: a registration point is a table, not a base class).
struct SubscriberDesc {
  const char* name = nullptr;
  void* context = nullptr;
  // The types it hears, any order; copied when it subscribes.
  std::span<const u16> types;
  // Optional: whether it hears an event of this tile. Null hears every tile. An event raised with
  // `k_any_tile` is heard whatever this says.
  bool (*wants_tile)(void* context, u64 tile) = nullptr;
  void (*handle)(void* context, const Event& event, Emitter& emit) = nullptr;
  // Optional: called once at the end of a delivery point in which its budget skipped events, with
  // how many — the place to coarsen, as a summarizer does for a fast-forward.
  void (*overflow)(void* context, u32 skipped) = nullptr;
  // Deliveries a tick before its events are skipped for it; 0 takes the bus's
  // `subscriber_budget`.
  u32 budget = 0;
};

struct EventBusConfig {
  // Records a bank holds (there are two: the tick's and the carry), and the payload bytes beside
  // them. Sized once; a raise that does not fit is dropped and counted.
  u32 capacity = 65536;
  u32 payload_bytes = 4u << 20;
  // Depth past which a raise is dropped (05 §5.7: "a hard cap on depth (drop and log beyond it)").
  u8 max_depth = 8;
  // Events of depth one or more delivered in one tick before the rest wait for the next.
  u32 max_cascade = 16384;
  // Events of one source delivered in one tick before its rest wait for the next.
  u32 max_per_source = 65536;
  // Deliveries a subscriber takes in a tick unless it names its own budget.
  u32 subscriber_budget = 1u << 20;
  // A subscriber whose handlers took longer than this in a tick is reported slow. Reported only.
  u32 slow_subscriber_us = 2000;
  // Roots whose chains a tick counts for the report's top chains; more are counted as untracked.
  u32 chain_slots = 1024;
  // A running hash of every delivery (the replay test's assertion). Off saves its cost.
  bool hash_deliveries = true;
};

// The tunables `sim.events.capacity`, `.payload_kib`, `.max_depth`, `.max_cascade`,
// `.max_per_source`, `.subscriber_budget`, `.slow_subscriber_us`, `.chain_slots`, read once.
EventBusConfig config_from_tunables();

// Each guard's trips in one tick.
struct GuardTrips {
  u32 depth_dropped = 0;     // raised past `max_depth`
  u32 cascade_carried = 0;   // past `max_cascade`, carried to the next tick
  u32 storm_carried = 0;     // past a source's `max_per_source`, carried to the next tick
  u32 self_loops = 0;        // a subscriber's raise of a type it hears, carried to the next tick
  u32 budget_skipped = 0;    // deliveries skipped for subscribers past their budget
  u32 full_dropped = 0;      // a raise or a carry that found its bank full
  u32 slow_subscribers = 0;  // subscribers past `slow_subscriber_us` (derived: never acted on)
  u32 key_ties = 0;          // two events with one whole key: their order was the raise's

  u32 total() const noexcept {
    return depth_dropped + cascade_carried + storm_carried + self_loops + budget_skipped +
           full_dropped + slow_subscribers + key_ties;
  }
};

struct ChainCount {
  EventId root;
  u32 events = 0;  // events of depth one or more delivered this tick under this root
  u32 pad = 0;
};

// One tick of the bus, as the tick's telemetry: complete once the tick's last delivery point ran.
struct TickReport {
  u64 tick = 0;
  u32 raised = 0;       // raises accepted this tick (into either bank)
  u32 delivered = 0;    // events delivered (each once, however many subscribers heard it)
  u32 deliveries = 0;   // handler calls
  u32 carried_in = 0;   // events this tick took from the last
  u32 carried_out = 0;  // events this tick leaves for the next
  u32 sorted = 0;       // delivery points whose events arrived out of order and were sorted
  u32 max_depth = 0;
  u32 chains_untracked = 0;  // chain events whose root found no slot
  u32 depth[k_depth_buckets] = {};
  ChainCount top_chains[k_top_chains] = {};
  GuardTrips trips;
  u16 slowest_subscriber = 0xFFFFu;
  u16 pad = 0;
  u32 slowest_us = 0;
};

// Counters per type since the bus was made: the rates the plan's telemetry asks for are these over
// ticks.
struct TypeCounters {
  u64 raised = 0;
  u64 delivered = 0;
  u64 dropped = 0;
  u64 carried = 0;
};

// What an external source's raise looks like to a log, and what a replay hands back: an input to
// the simulation, which is everything the bus delivers that the simulation did not compute
// (docs/subsystems/events.md, "Events and the replay").
struct ExternalRaise {
  u64 tick = 0;  // the tick the bus was at: the raise is delivered at that tick's EventsIn
  u64 key = 0;
  u64 tile = k_any_tile;
  u32 source = 0;
  u32 seq = 0;
  u32 type_id = 0;  // `schema::stable_type_id` of the type's qualified name
  u8 phase = 0;
  std::span<const u8> payload;
};

struct ExternalLog {
  void* context = nullptr;
  void (*record)(void* context, const ExternalRaise& raise) = nullptr;
};

class EventBus {
 public:
  explicit EventBus(const EventBusConfig& config = {});
  ~EventBus();
  ENGINE_NON_COPYABLE(EventBus);

  // --- registration (cold) -------------------------------------------------------------------

  // A type the bus carries: a schema struct tagged `@kind(event)`, trivially copyable. Idempotent.
  template <class T>
  EventType<T> register_type() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "events: an event payload is copied as bytes, so it is trivially copyable");
    return EventType<T>{register_type_info(schema::type_of<T>())};
  }
  u16 register_type_info(const schema::TypeInfo& info);
  u16 type_count() const noexcept { return static_cast<u16>(types_.size()); }
  const schema::TypeInfo* type_info(u16 type) const noexcept;

  // A named source (a flecs system, a timer sink, the host, a game's input). Ids are given in
  // registration order, so a host that registers its sources in a fixed order gets fixed ids.
  // `external` marks a source whose raises are inputs to the simulation: each is handed to the
  // external log (`set_external_log`) as it is raised.
  u32 add_source(const char* name, bool external = false);
  const char* source_name(u32 source) const noexcept;

  // Returns the subscriber's index: subscribers hear their events in this order.
  u16 subscribe(const SubscriberDesc& desc);
  u16 subscriber_count() const noexcept { return static_cast<u16>(subscribers_.size()); }

  // The document whose change feed raises `engine.events.RecordChanged` (and, when the feed no
  // longer reaches back, one `engine.events.DocumentResynced`) at every `EventsIn`, from the
  // document's revision now. The document must outlive the bus or be detached with null.
  void attach_document(const doc::Document* document);

  void set_external_log(const ExternalLog& log) noexcept { external_log_ = log; }

  // Registers the bus's two delivery points and its phase marks as systems in the scheduler's
  // table (every tier, no component or resource: they run serially at the start of their phases,
  // before anything else in them). Once per bus; the scheduler must outlive the bus's use of it.
  void install(sim::SimScheduler& scheduler);

  // --- raising -------------------------------------------------------------------------------

  // An emitter for one invocation of a table system: its source is the system, its key the batch.
  Emitter emitter(const sim::SystemContext& context, sim::Batch batch) noexcept;
  // The same for a system's `begin_tick` (`end` false) or `end_tick` hook (`end` true), which run
  // serially and are keyed before and after every batch.
  Emitter hook_emitter(const sim::SystemContext& context, bool end) noexcept;
  // An emitter for a registered source, keyed by `key` (an entity, a record, a worker's batch):
  // two emitters of one source raising in parallel must not share a key.
  Emitter emitter(u32 source, u64 key = 0) noexcept;

  // A recorded external raise, re-raised at the tick it was recorded at (a replay). The payload is
  // read by type id; one whose type this bus does not carry is refused.
  bool replay_external(const ExternalRaise& entry);

  // --- delivery (what `install` registers; public so a host without a scheduler can drive it) --

  // Delivers what is pending: the start of `EventsIn` (which first takes the carry from the last
  // tick and reads the document's feed) or of `EventsOut`.
  void deliver(sim::TickPhase point, u64 tick);
  // Marks the phase raises are attributed to (what `install` calls at the start of every phase).
  void enter_phase(sim::TickPhase phase, u64 tick) noexcept;

  // --- telemetry -----------------------------------------------------------------------------

  // The last complete tick's report, and the one being filled.
  const TickReport& last_report() const noexcept { return last_; }
  const TickReport& current_report() const noexcept { return current_; }
  // Every tick's trips added together since the bus was made.
  const GuardTrips& total_trips() const noexcept { return total_trips_; }
  std::span<const TypeCounters> type_counters() const noexcept {
    return {type_counters_.data(), type_counters_.size()};
  }
  // A running hash of every delivery: the point, the event's id, type, source, key, sequence and
  // depth, its payload's bytes, and which subscriber heard it. Two runs of the same world with the
  // same inputs end with the same hash.
  u64 delivery_hash() const noexcept { return delivery_hash_; }
  // Records waiting: in the tick's bank and in the carry.
  u32 pending() const noexcept;
  u32 carried() const noexcept { return carry_.size(); }
  // Inside a delivery point: a handler is running, or the point is ordering what it delivers.
  bool delivering() const noexcept { return delivering_; }

 private:
  friend class Emitter;
  friend struct BankProbe;  // the tests' view of a bank's counters

  struct Bank {
    Vector<EventRecord> records;  // sized to the capacity once
    Vector<u64> words;            // the payload arena, 8-byte aligned, sized once
    // Records in the low 32 bits, payload words in bits 32 to 62, and bit 63 set while the live
    // bank is selecting (below). One compare-exchange reserves a slot and its words together, and
    // only when both fit, so neither count can pass its bound and neither can wrap (B6).
    std::atomic<u64> state{0};
    std::atomic<u32> written{0};  // reserved slots whose record and bytes are written

    u32 size() const noexcept;
    void reset() noexcept;
    const void* payload(const EventRecord& record) const noexcept {
      return reinterpret_cast<const u8*>(words.data()) + record.payload;
    }
  };
  // A full live bank outside a delivery point keeps the lowest keys of everything raised into it
  // (docs/subsystems/events.md, "The runaway guards"): from the first raise that does not fit, the
  // bank is closed to the lock-free path and every raise goes through `select_append` under
  // `select_mutex_`, which evicts the highest keys for a lower one and refuses anything at or past
  // the lowest key it has turned away (the cut). Under the mutex only.
  struct Rank {
    u64 when = 0;  // tick << 8 | phase
    u64 key = 0;
    u64 tile = 0;
    u32 source = 0;
    u32 seq = 0;
    u32 type = 0;
    u32 pad = 0;
  };
  struct Selection {
    bool active = false;
    bool has_cut = false;
    u32 end = 0;         // slots in use, the evicted (type `k_invalid_type`) among them
    u32 live = 0;        // records kept
    u32 bump = 0;        // payload words in use, holes among them
    u32 live_words = 0;  // payload words the kept records need
    Rank cut;            // the lowest event turned away, and its payload's hash
    u64 cut_hash = 0;
    // Evicted slots by the payload room they leave, a list per power of two (a payload is at
    // most 8,192 words), threaded through the dead records themselves.
    u32 holes[14] = {};
  };
  // The delivery order: (tick, phase), source, key, sequence, and the bank position last.
  struct SortKey {
    u64 when = 0;  // tick << 8 | phase
    u64 key = 0;
    u32 source = 0;
    u32 seq = 0;
    u32 index = 0;
    u32 pad = 0;
  };
  struct Subscriber {
    SubscriberDesc desc;
    Vector<u16> types;  // sorted, unique
    u32 budget = 0;
    u32 delivered = 0;  // this tick
    u32 skipped = 0;    // this point
    i64 ns = 0;         // this tick
  };
  struct Source {
    const char* name = nullptr;
    bool external = false;
  };
  struct ChainSlot {
    EventId root;
    u32 events = 0;
    u32 pad = 0;
  };

  // `select`: the live bank outside a delivery point, where a parallel wave can raise; everywhere
  // else raises are serial and a full bank simply refuses.
  bool append(Bank& bank, const EventRecord& head, const void* bytes, u32 align, bool select);
  bool select_append(const EventRecord& head, const void* bytes);
  void open_selection();
  void compact_selection();
  void close_selection();
  u32 payload_words(u16 type) const noexcept;
  u64 payload_hash(u16 type, const void* bytes) const noexcept;
  bool select_less(const EventRecord& a, const void* pa, const EventRecord& b,
                   const void* pb) const noexcept;
  bool below_cut(const EventRecord& record, const void* bytes) const noexcept;
  void lower_cut(const EventRecord& record, const void* bytes) noexcept;
  bool raise_from(Emitter& from, u16 type, const void* bytes, u32 size, u32 align,
                  const RaiseOptions& options);
  bool subscriber_hears(const Subscriber& subscriber, u16 type) const noexcept;
  u32& source_count(u32 source);
  EventId next_id() noexcept;
  void begin_tick(u64 tick);
  void finish_tick();
  void read_document();
  void deliver_generation(u32 first, u32 last, sim::TickPhase point);
  void carry(const EventRecord& record, const void* bytes, u32* trip);
  void count_chain(const EventRecord& record);
  void hash_delivery(const EventRecord& record, const void* bytes, u16 subscriber) noexcept;
  void reserve_scratch();

  static void phase_mark(sim::SystemContext& context);
  static bool key_less(const SortKey& x, const SortKey& y) noexcept;
  static bool same_key(const SortKey& x, const SortKey& y) noexcept;

  EventBusConfig config_;
  // What waits for the next delivery point, and what waits for the next tick. A point appends the
  // carry to the live bank at `EventsIn` (it is older, so it sorts first) and moves into the carry
  // whatever a guard defers.
  Bank live_;
  Bank carry_;

  Vector<const schema::TypeInfo*> types_;
  Vector<TypeCounters> type_counters_;
  Vector<Source> sources_;
  Vector<Subscriber> subscribers_;

  // Delivery scratch, reserved to the bank's capacity.
  Vector<SortKey> keys_;
  Vector<u32> order_;       // positions in `live_`, delivery order
  Vector<u32> admitted_;    // this generation's events after the cascade and storm guards
  Vector<u32> type_first_;  // counting sort of `admitted_` by type: offsets
  Vector<u32> by_type_;     // ... and positions
  Vector<u32> cursors_;     // a subscriber's merge cursors, one per type it hears
  // Events delivered this tick per source, one array per origin, indexed by the source's index;
  // the touched list is what the tick's end resets.
  Vector<u32> source_counts_[4];
  Vector<u32> touched_sources_;
  u32 cascade_delivered_ = 0;
  Vector<ChainSlot> chains_;
  Vector<u32> touched_chains_;
  Vector<Id128> changed_;

  const doc::Document* document_ = nullptr;
  u64 document_revision_ = 0;
  u16 record_changed_ = k_invalid_type;
  u16 document_resynced_ = k_invalid_type;

  ExternalLog external_log_;
  sim::SimScheduler* scheduler_ = nullptr;
  TickReport current_;
  TickReport last_;
  GuardTrips total_trips_;
  // Raises refused for a full bank, and raises a lower key displaced from it, which a parallel wave
  // can do from many threads at once; folded into the tick's report at the next delivery point.
  std::atomic<u32> full_refused_{0};
  // The full live bank's selection; its heap of kept slots lives in `order_`, which is idle
  // between delivery points.
  std::mutex select_mutex_;
  Selection selection_;
  u64 delivery_hash_ = 0;
  u64 tick_ = 0;         // the tick the bus is in
  u64 serial_tick_ = 0;  // the tick `serial_` counts in
  u32 serial_ = 0;       // ids given in `serial_tick_`
  u8 phase_ = 0;
  bool delivering_ = false;
  bool installed_ = false;
};

}  // namespace engine::events
