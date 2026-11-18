#include <core/base/assert.h>
#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/schema/materialize.h>
#include <domain/doc/document.h>
#include <domain/events/events.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <bit>
#include <schemas/events.h>
#include <thread>

namespace engine::events {

ENGINE_LOG_CATEGORY_DEFINE(log_events, "events");

namespace {

// Run-time sizes and bounds (ADR-0011): read once, by the host, into an `EventBusConfig`.
tunables::Int t_capacity{"sim.events.capacity", 65536, 1024, 1 << 24,
                         "Event records a bank of the world event bus holds (two banks)"};
tunables::Int t_payload_kib{"sim.events.payload_kib", 4096, 64, 1 << 20,
                            "Payload KiB beside each bank of the world event bus"};
tunables::Int t_max_depth{"sim.events.max_depth", 8, 1, 255,
                          "Causal depth past which a raised event is dropped"};
tunables::Int t_max_cascade{"sim.events.max_cascade", 16384, 1, 1 << 24,
                            "Consequences (depth >= 1) delivered a tick before the rest wait"};
tunables::Int t_max_per_source{"sim.events.max_per_source", 65536, 1, 1 << 24,
                               "Events of one source delivered a tick before its rest wait"};
tunables::Int t_subscriber_budget{"sim.events.subscriber_budget", 1 << 20, 1, 1 << 30,
                                  "Deliveries a subscriber takes a tick before skipping"};
tunables::Int t_slow_us{"sim.events.slow_subscriber_us", 2000, 1, 1 << 30,
                        "A subscriber's handlers past this many microseconds a tick are reported"};
tunables::Int t_chain_slots{"sim.events.chain_slots", 1024, 16, 1 << 20,
                            "Causal chains a tick counts for the report's top chains"};

constexpr const char* k_mark_names[sim::k_phase_count] = {
    "events.input",   "events.in",          "events.lod", "events.systems",
    "events.physics", "events.postphysics", "events.out", "events.persist",
};

constexpr u32 round8(u32 size) noexcept { return (size + 7u) & ~7u; }

u32 pow2_at_least(u32 n) noexcept {
  u32 p = 16;
  while (p < n && p < (1u << 30))
    p <<= 1;
  return p;
}

}  // namespace

bool EventBus::same_key(const SortKey& x, const SortKey& y) noexcept {
  return x.when == y.when && x.source == y.source && x.key == y.key && x.seq == y.seq;
}

bool EventBus::key_less(const SortKey& x, const SortKey& y) noexcept {
  if (x.when != y.when) return x.when < y.when;
  if (x.source != y.source) return x.source < y.source;
  if (x.key != y.key) return x.key < y.key;
  if (x.seq != y.seq) return x.seq < y.seq;
  return x.index < y.index;
}

EventBusConfig config_from_tunables() {
  EventBusConfig config;
  config.capacity = static_cast<u32>(t_capacity.get());
  config.payload_bytes = static_cast<u32>(t_payload_kib.get()) * 1024u;
  config.max_depth = static_cast<u8>(t_max_depth.get());
  config.max_cascade = static_cast<u32>(t_max_cascade.get());
  config.max_per_source = static_cast<u32>(t_max_per_source.get());
  config.subscriber_budget = static_cast<u32>(t_subscriber_budget.get());
  config.slow_subscriber_us = static_cast<u32>(t_slow_us.get());
  config.chain_slots = static_cast<u32>(t_chain_slots.get());
  return config;
}

// --- the banks -------------------------------------------------------------------------------

namespace {

// `Bank::state`: records in the low half, payload words in bits 32 to 62, the selection's mark in
// bit 63.
constexpr u64 k_selecting = u64{1} << 63;
constexpr u32 state_count(u64 state) noexcept { return static_cast<u32>(state); }
constexpr u32 state_words(u64 state) noexcept {
  return static_cast<u32>(state >> 32) & 0x7FFF'FFFFu;
}

}  // namespace

u32 EventBus::Bank::size() const noexcept {
  return state_count(state.load(std::memory_order_acquire));
}

void EventBus::Bank::reset() noexcept {
  state.store(0, std::memory_order_relaxed);
  written.store(0, std::memory_order_relaxed);
}

u32 EventBus::payload_words(u16 type) const noexcept { return round8(types_[type]->size) / 8u; }

bool EventBus::append(Bank& bank, const EventRecord& head, const void* bytes, u32 align,
                      bool select) {
  ENGINE_ASSERT(align <= 8u, "events: a payload is aligned to at most 8 bytes");
  (void)align;
  // The slot and its payload words in one compare-exchange, taken only when both fit: a refused
  // raise moves neither count, so a storm of refusals cannot carry either past its bound and round
  // to a small offset over payloads already raised (B6). The payload's size is its type's, which
  // is why the record does not carry it.
  const u32 size = types_[head.type]->size;
  const u32 words = round8(size) / 8u;
  const u32 capacity = bank.records.size();
  const u32 arena = bank.words.size();
  u64 state = bank.state.load(std::memory_order_relaxed);
  for (;;) {
    // Full, or already selecting: which raises a full bank keeps must not be whichever reached it
    // first (B5), so outside a delivery point the rest go through the selection.
    if ((state & k_selecting) != 0 || state_count(state) >= capacity ||
        static_cast<u64>(state_words(state)) + words > arena) {
      return select && select_append(head, bytes);
    }
    // Relaxed: the delivery point that reads the record runs after the raiser's wave has been
    // waited for, and that wait is the synchronization; a selection waits on `written`.
    if (bank.state.compare_exchange_weak(state, state + (u64{words} << 32) + 1u,
                                         std::memory_order_relaxed, std::memory_order_relaxed)) {
      break;
    }
  }
  const u32 offset = state_words(state) * 8u;
  EventRecord& record = bank.records[state_count(state)];
  record = head;
  record.payload = offset;
  if (size != 0) std::memcpy(reinterpret_cast<u8*>(bank.words.data()) + offset, bytes, size);
  bank.written.fetch_add(1, std::memory_order_release);
  return true;
}

// --- the full bank's selection -----------------------------------------------------------------
//
// What a full live bank keeps is the longest run of the order (tick, phase, source, key, sequence;
// then type, tile and the payload's hash, so that even two raises with one whole key are told
// apart by what they carry) that fits its records and its arena, over everything raised since the
// last delivery point: a function of the raises and never of which thread reached the bank first.
// Kept as a heap of the kept slots by that order, so a lower raise evicts the highest, and a cut,
// the lowest raise turned away, at or past which everything is refused at once (a run that fits
// stops at the first raise that does not).

u64 EventBus::payload_hash(u16 type, const void* bytes) const noexcept {
  using engine::hash_detail::mix_word;
  const auto* p = static_cast<const u8*>(bytes);
  u32 left = types_[type]->size;
  u64 h = left;
  for (; left >= 8u; left -= 8u, p += 8) {
    u64 word;
    std::memcpy(&word, p, 8);
    h = mix_word(h, word);
  }
  if (left > 0) {
    u64 word = 0;
    std::memcpy(&word, p, left);
    h = mix_word(h, word ^ (static_cast<u64>(left) << 56));
  }
  return h;
}

namespace {

int rank_compare(u64 a_when, u32 a_source, u64 a_key, u32 a_seq, u32 a_type, u64 a_tile, u64 b_when,
                 u32 b_source, u64 b_key, u32 b_seq, u32 b_type, u64 b_tile) noexcept {
  if (a_when != b_when) return a_when < b_when ? -1 : 1;
  if (a_source != b_source) return a_source < b_source ? -1 : 1;
  if (a_key != b_key) return a_key < b_key ? -1 : 1;
  if (a_seq != b_seq) return a_seq < b_seq ? -1 : 1;
  if (a_type != b_type) return a_type < b_type ? -1 : 1;
  if (a_tile != b_tile) return a_tile < b_tile ? -1 : 1;
  return 0;
}

constexpr u64 when_of(const EventRecord& record) noexcept {
  return (record.tick << 8) | record.phase;
}

constexpr u32 k_no_hole = 0xFFFF'FFFFu;
constexpr u32 k_hole_classes = 14;  // `Selection::holes`
constexpr u32 floor_log2(u32 v) noexcept { return v == 0 ? 0u : 31u - std::countl_zero(v); }

}  // namespace

bool EventBus::select_less(const EventRecord& a, const void* pa, const EventRecord& b,
                           const void* pb) const noexcept {
  const int c = rank_compare(when_of(a), a.source, a.key, a.seq, a.type, a.tile, when_of(b),
                             b.source, b.key, b.seq, b.type, b.tile);
  if (c != 0) return c < 0;
  return payload_hash(a.type, pa) < payload_hash(b.type, pb);
}

bool EventBus::below_cut(const EventRecord& record, const void* bytes) const noexcept {
  if (!selection_.has_cut) return true;
  const Rank& cut = selection_.cut;
  const int c =
      rank_compare(when_of(record), record.source, record.key, record.seq, record.type, record.tile,
                   cut.when, cut.source, cut.key, cut.seq, cut.type, cut.tile);
  if (c != 0) return c < 0;
  return payload_hash(record.type, bytes) < selection_.cut_hash;
}

void EventBus::lower_cut(const EventRecord& record, const void* bytes) noexcept {
  if (!below_cut(record, bytes)) return;
  Rank& cut = selection_.cut;
  cut.when = when_of(record);
  cut.key = record.key;
  cut.tile = record.tile;
  cut.source = record.source;
  cut.seq = record.seq;
  cut.type = record.type;
  selection_.cut_hash = payload_hash(record.type, bytes);
  selection_.has_cut = true;
}

void EventBus::open_selection() {
  // Close the lock-free path, then wait for the raises it already let in to finish writing: from
  // here every raise is serialized and the bank holds still.
  const u64 state = live_.state.fetch_or(k_selecting, std::memory_order_acq_rel);
  const u32 count = state_count(state);
  while (live_.written.load(std::memory_order_acquire) < count)
    std::this_thread::yield();
  selection_.active = true;
  selection_.has_cut = false;
  selection_.end = count;
  selection_.live = count;
  selection_.bump = state_words(state);
  selection_.live_words = state_words(state);
  for (u32& hole : selection_.holes)
    hole = k_no_hole;
  order_.clear();
  for (u32 slot = 0; slot < count; ++slot)
    order_.push_back(slot);
  std::make_heap(order_.begin(), order_.end(), [this](u32 a, u32 b) {
    return select_less(live_.records[a], live_.payload(live_.records[a]), live_.records[b],
                       live_.payload(live_.records[b]));
  });
}

void EventBus::compact_selection() {
  // The kept payloads in offset order, each moved down over the gaps before it (a kept payload's
  // new offset is the sum of the kept ones below it, never past its old one); then the kept
  // records down over the evicted slots, in slot order. The heap is the kept slots, so it is the
  // list to sort, and it is rebuilt after.
  auto* base = reinterpret_cast<u8*>(live_.words.data());
  std::sort(order_.begin(), order_.end(),
            [this](u32 a, u32 b) { return live_.records[a].payload < live_.records[b].payload; });
  u32 to_words = 0;
  for (const u32 slot : order_) {
    EventRecord& record = live_.records[slot];
    const u32 words = payload_words(record.type);
    ENGINE_ASSERT(record.payload >= to_words * 8u, "events: kept payloads do not overlap");
    if (record.payload != to_words * 8u) {
      std::memmove(base + to_words * 8u, base + record.payload, words * 8u);
      record.payload = to_words * 8u;
    }
    to_words += words;
  }
  u32 to = 0;
  for (u32 from = 0; from < selection_.end; ++from) {
    if (live_.records[from].type == k_invalid_type) continue;
    if (to != from) live_.records[to] = live_.records[from];
    ++to;
  }
  ENGINE_ASSERT(to == selection_.live && to_words == selection_.live_words,
                "events: a selection's counts are its kept records");
  selection_.end = to;
  selection_.bump = to_words;
  for (u32& hole : selection_.holes)
    hole = k_no_hole;
  order_.clear();
  for (u32 slot = 0; slot < to; ++slot)
    order_.push_back(slot);
  std::make_heap(order_.begin(), order_.end(), [this](u32 a, u32 b) {
    return select_less(live_.records[a], live_.payload(live_.records[a]), live_.records[b],
                       live_.payload(live_.records[b]));
  });
}

bool EventBus::select_append(const EventRecord& head, const void* bytes) {
  std::lock_guard<std::mutex> lock(select_mutex_);
  if (!selection_.active) open_selection();
  // At or past the cut: a run that fits has already stopped before it.
  if (!below_cut(head, bytes)) return false;
  const auto heap_less = [this](u32 a, u32 b) {
    return select_less(live_.records[a], live_.payload(live_.records[a]), live_.records[b],
                       live_.payload(live_.records[b]));
  };
  const u32 capacity = live_.records.size();
  const u32 arena = live_.words.size();
  const u32 words = payload_words(head.type);
  while (selection_.live + 1u > capacity ||
         static_cast<u64>(selection_.live_words) + words > arena) {
    // Room only by evicting what is higher than this raise; if the highest kept is lower, the run
    // that fits ends here and this raise is the new cut.
    if (order_.empty()) {
      lower_cut(head, bytes);
      return false;
    }
    const EventRecord& top = live_.records[order_.front()];
    if (!select_less(head, bytes, top, live_.payload(top))) {
      lower_cut(head, bytes);
      return false;
    }
    std::pop_heap(order_.begin(), order_.end(), heap_less);
    const u32 freed = order_.back();
    order_.pop_back();
    EventRecord& gone = live_.records[freed];
    lower_cut(gone, live_.payload(gone));
    const u32 room = payload_words(gone.type);
    selection_.live -= 1u;
    selection_.live_words -= room;
    // A hole: its room in `key`, the next hole of its class in `seq`.
    gone.type = k_invalid_type;
    gone.key = room;
    const u32 hole_class = std::min(floor_log2(room), k_hole_classes - 1u);
    gone.seq = selection_.holes[hole_class];
    selection_.holes[hole_class] = freed;
    full_refused_.fetch_add(1, std::memory_order_relaxed);
  }
  // The logical room is there (the loop above made it); where it goes: a hole with room for its
  // payload, in place; else a slot (an evicted one, whose bytes wait for the next compaction, or
  // past the last) and words past the last, compacting first when the arena's end is reached.
  u32 slot = k_no_hole;
  for (u32 c = words <= 1u ? 0u : floor_log2(words - 1u) + 1u; c < k_hole_classes; ++c) {
    const u32 hole = selection_.holes[c];
    if (hole != k_no_hole && live_.records[hole].key >= words) {
      selection_.holes[c] = live_.records[hole].seq;
      slot = hole;
      break;
    }
  }
  u32 offset;
  if (slot != k_no_hole) {
    offset = live_.records[slot].payload;
  } else {
    if (static_cast<u64>(selection_.bump) + words > arena) compact_selection();
    for (u32& hole : selection_.holes) {
      if (hole == k_no_hole) continue;
      slot = hole;
      hole = live_.records[slot].seq;
      break;
    }
    // Every evicted slot is on a list, so with none the slots in use are the kept ones, fewer
    // than the capacity.
    if (slot == k_no_hole) slot = selection_.end++;
    ENGINE_ASSERT(slot < capacity, "events: a selection has a slot for what it keeps");
    offset = selection_.bump * 8u;
    selection_.bump += words;
  }
  EventRecord& record = live_.records[slot];
  record = head;
  record.payload = offset;
  const u32 size = types_[head.type]->size;
  if (size != 0) std::memcpy(reinterpret_cast<u8*>(live_.words.data()) + offset, bytes, size);
  selection_.live += 1u;
  selection_.live_words += words;
  order_.push_back(slot);
  std::push_heap(order_.begin(), order_.end(), heap_less);
  return true;
}

void EventBus::close_selection() {
  // At a delivery point, after the wave: the kept records packed to the front and the bank opened
  // again, so what a carry or a handler appends next finds the same room on every run.
  if (!selection_.active) return;
  compact_selection();
  live_.state.store((static_cast<u64>(selection_.bump) << 32) | selection_.end,
                    std::memory_order_relaxed);
  live_.written.store(selection_.end, std::memory_order_relaxed);
  selection_ = Selection{};
  order_.clear();
}

// --- construction and registration -------------------------------------------------------------

EventBus::EventBus(const EventBusConfig& config) : config_(config) {
  ENGINE_VERIFY(config_.capacity > 0 && config_.payload_bytes >= 8u,
                "events: a bus holds at least one event");
  for (Bank* bank : {&live_, &carry_}) {
    bank->records.resize(config_.capacity);
    bank->words.resize((config_.payload_bytes + 7u) / 8u);
  }
  config_.chain_slots = pow2_at_least(config_.chain_slots);
  chains_.resize(config_.chain_slots);
  touched_chains_.reserve(config_.chain_slots);
  keys_.reserve(config_.capacity);
  order_.reserve(config_.capacity);
  admitted_.reserve(config_.capacity);
  by_type_.reserve(config_.capacity);
  changed_.reserve(config_.capacity);
  touched_sources_.reserve(1024);
  current_.tick = tick_;
  // The bus's own types: what the document's feed raises.
  record_changed_ = register_type_info(schema::type_of<RecordChanged>());
  document_resynced_ = register_type_info(schema::type_of<DocumentResynced>());
}

EventBus::~EventBus() = default;

u16 EventBus::register_type_info(const schema::TypeInfo& info) {
  for (u16 i = 0; i < types_.size(); ++i) {
    if (types_[i] == &info) return i;
  }
  ENGINE_VERIFY(std::strcmp(info.tag, "event") == 0,
                "events: a type the bus carries is a schema struct tagged @kind(event)");
  ENGINE_VERIFY(info.size <= 0xFFFFu, "events: a payload is at most 64 KiB");
  ENGINE_VERIFY(types_.size() < k_invalid_type, "events: too many event types");
  types_.push_back(&info);
  type_counters_.push_back(TypeCounters{});
  type_first_.reserve(types_.size() + 1u);
  return static_cast<u16>(types_.size() - 1u);
}

const schema::TypeInfo* EventBus::type_info(u16 type) const noexcept {
  return type < types_.size() ? types_[type] : nullptr;
}

u32 EventBus::add_source(const char* name, bool external) {
  sources_.push_back(Source{name, external});
  const u32 index = sources_.size() - 1u;
  source_counts_[static_cast<u32>(Origin::Source)].resize(sources_.size(), 0u);
  return make_source(Origin::Source, index);
}

const char* EventBus::source_name(u32 source) const noexcept {
  const u32 index = source_index(source);
  switch (source_origin(source)) {
    case Origin::Source: return index < sources_.size() ? sources_[index].name : nullptr;
    case Origin::System:
      return scheduler_ != nullptr && index < scheduler_->system_count()
                 ? scheduler_->system(static_cast<u16>(index)).name
                 : nullptr;
    case Origin::Subscriber:
      return index < subscribers_.size() ? subscribers_[index].desc.name : nullptr;
    case Origin::Document: return "document";
  }
  return nullptr;
}

u16 EventBus::subscribe(const SubscriberDesc& desc) {
  ENGINE_VERIFY(subscribers_.size() < 0xFFFEu, "events: too many subscribers");
  Subscriber subscriber;
  subscriber.desc = desc;
  for (const u16 type : desc.types) {
    ENGINE_ASSERT(type < types_.size(), "events: a subscriber hears a registered type");
    if (type < types_.size()) subscriber.types.push_back(type);
  }
  std::sort(subscriber.types.begin(), subscriber.types.end());
  u32 unique = 0;
  for (u32 i = 0; i < subscriber.types.size(); ++i) {
    if (i == 0 || subscriber.types[i] != subscriber.types[unique - 1u]) {
      subscriber.types[unique++] = subscriber.types[i];
    }
  }
  subscriber.types.resize(unique);
  subscriber.desc.types = {};
  subscriber.budget = desc.budget != 0 ? desc.budget : config_.subscriber_budget;
  if (cursors_.capacity() < 2u * unique) cursors_.reserve(2u * unique);
  subscribers_.push_back(static_cast<Subscriber&&>(subscriber));
  source_counts_[static_cast<u32>(Origin::Subscriber)].resize(subscribers_.size(), 0u);
  return static_cast<u16>(subscribers_.size() - 1u);
}

void EventBus::attach_document(const doc::Document* document) {
  document_ = document;
  document_revision_ = document != nullptr ? document->revision() : 0;
  source_counts_[static_cast<u32>(Origin::Document)].resize(1, 0u);
}

void EventBus::install(sim::SimScheduler& scheduler) {
  ENGINE_VERIFY(!installed_, "events: a bus installs into one scheduler, once");
  installed_ = true;
  scheduler_ = &scheduler;
  for (u32 phase = 0; phase < sim::k_phase_count; ++phase) {
    sim::SystemDesc desc;
    desc.name = k_mark_names[phase];
    desc.phase = static_cast<sim::TickPhase>(phase);
    // Every tier: delivery is not a tier's work, and a mask that held nothing would otherwise
    // hold the events too. An empty mask still suspends it, losing nothing.
    desc.tiers = 0xFFu;
    desc.context = this;
    desc.begin_tick = &EventBus::phase_mark;
    scheduler.add_system(desc);
  }
}

void EventBus::phase_mark(sim::SystemContext& context) {
  auto* bus = static_cast<EventBus*>(context.context);
  bus->enter_phase(context.phase, context.tick.value);
  if (context.phase == sim::TickPhase::EventsIn || context.phase == sim::TickPhase::EventsOut) {
    bus->deliver(context.phase, context.tick.value);
  }
}

// --- raising ---------------------------------------------------------------------------------

Emitter EventBus::emitter(const sim::SystemContext& context, sim::Batch batch) noexcept {
  Emitter e;
  e.bus_ = this;
  e.source_ = make_source(Origin::System, context.system);
  e.key_ = static_cast<u64>(batch.begin) + 1u;
  e.phase_ = static_cast<u8>(context.phase);
  return e;
}

Emitter EventBus::hook_emitter(const sim::SystemContext& context, bool end) noexcept {
  Emitter e;
  e.bus_ = this;
  e.source_ = make_source(Origin::System, context.system);
  e.key_ = end ? ~u64{0} : 0u;
  e.phase_ = static_cast<u8>(context.phase);
  return e;
}

Emitter EventBus::emitter(u32 source, u64 key) noexcept {
  Emitter e;
  e.bus_ = this;
  e.source_ = source;
  e.key_ = key;
  e.phase_ = phase_;
  return e;
}

bool Emitter::raise_bytes(u16 type, const void* bytes, u32 size, u32 align,
                          const RaiseOptions& options) {
  if (bus_ == nullptr) return false;
  return bus_->raise_from(*this, type, bytes, size, align, options);
}

bool EventBus::subscriber_hears(const Subscriber& subscriber, u16 type) const noexcept {
  return std::binary_search(subscriber.types.begin(), subscriber.types.end(), type);
}

bool EventBus::raise_from(Emitter& from, u16 type, const void* bytes, u32 size, u32 align,
                          const RaiseOptions& options) {
  ENGINE_ASSERT(type < types_.size(), "events: raise of an unregistered type");
  if (type >= types_.size()) return false;
  ENGINE_ASSERT(size == types_[type]->size, "events: payload size is not the type's");
  if (from.depth_ > config_.max_depth) {
    // Only a handler raises past depth 0, and handlers run serially: plain counters are safe.
    ++current_.trips.depth_dropped;
    ++type_counters_[type].dropped;
    return false;
  }
  EventRecord head;
  head.tick = tick_;
  head.cause = from.cause_;
  head.root = from.root_;
  head.key = from.key_;
  head.tile = options.tile;
  head.source = from.source_;
  head.seq = from.seq_++;
  head.type = type;
  head.phase = from.phase_;
  head.depth = from.depth_;

  if (from.subscriber_ != 0xFFFFu && subscriber_hears(subscribers_[from.subscriber_], type)) {
    // No zero-delay self-loops (05 §5.7): a subscriber's reaction to its own type waits a tick.
    carry(head, bytes, &current_.trips.self_loops);
    return true;
  }
  if (source_origin(head.source) == Origin::Source && external_log_.record != nullptr) {
    const u32 index = source_index(head.source);
    if (index < sources_.size() && sources_[index].external) {
      ExternalRaise entry;
      entry.tick = head.tick;
      entry.key = head.key;
      entry.tile = head.tile;
      entry.source = head.source;
      entry.seq = head.seq;
      entry.type_id = schema::stable_type_id(types_[type]->qualified_name);
      entry.phase = head.phase;
      entry.payload = std::span<const u8>(static_cast<const u8*>(bytes), size);
      external_log_.record(external_log_.context, entry);
    }
  }
  if (!append(live_, head, bytes, align, !delivering_)) {
    full_refused_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool EventBus::replay_external(const ExternalRaise& entry) {
  if (entry.tick != tick_) return false;
  u16 type = k_invalid_type;
  for (u16 i = 0; i < types_.size(); ++i) {
    if (schema::stable_type_id(types_[i]->qualified_name) == entry.type_id) type = i;
  }
  if (type == k_invalid_type || entry.payload.size() != types_[type]->size) return false;
  EventRecord head;
  head.tick = entry.tick;
  head.key = entry.key;
  head.tile = entry.tile;
  head.source = entry.source;
  head.seq = entry.seq;
  head.type = type;
  head.phase = entry.phase;
  if (!append(live_, head, entry.payload.data(), 8u, !delivering_)) {
    full_refused_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void EventBus::carry(const EventRecord& record, const void* bytes, u32* trip) {
  if (!append(carry_, record, bytes, 8u, false)) {
    ++current_.trips.full_dropped;
    ++type_counters_[record.type].dropped;
    return;
  }
  ++*trip;
  ++type_counters_[record.type].carried;
}

// --- delivery --------------------------------------------------------------------------------

u32& EventBus::source_count(u32 source) {
  Vector<u32>& counts = source_counts_[static_cast<u32>(source_origin(source)) & 3u];
  const u32 index = source_index(source);
  // A system's first raise grows its origin's array once; every later tick finds it sized.
  if (index >= counts.size()) counts.resize(index + 1u, 0u);
  return counts[index];
}

EventId EventBus::next_id() noexcept {
  if (serial_tick_ != tick_) {
    serial_tick_ = tick_;
    serial_ = 0;
  }
  ENGINE_ASSERT(serial_ < 0x00FF'FFFFu, "events: more than 2^24 events in one tick");
  ++serial_;
  return EventId{(tick_ << 24) | serial_};
}

void EventBus::enter_phase(sim::TickPhase phase, u64 tick) noexcept {
  if (tick != tick_) begin_tick(tick);
  phase_ = static_cast<u8>(phase);
}

void EventBus::begin_tick(u64 tick) {
  finish_tick();
  tick_ = tick;
  current_ = TickReport{};
  current_.tick = tick;
}

void EventBus::finish_tick() {
  // The slow-subscriber report: derived from the clock and never read back into the simulation.
  for (u16 s = 0; s < subscribers_.size(); ++s) {
    Subscriber& subscriber = subscribers_[s];
    const u32 us = static_cast<u32>(subscriber.ns / 1000);
    if (subscriber.ns > 0 && us >= config_.slow_subscriber_us) ++current_.trips.slow_subscribers;
    if (subscriber.ns > 0 && (current_.slowest_subscriber == 0xFFFFu || us > current_.slowest_us)) {
      current_.slowest_subscriber = s;
      current_.slowest_us = us;
    }
    subscriber.ns = 0;
    subscriber.delivered = 0;
  }
  // The top chains, largest first, ties by root id so the report is a function of the tick.
  for (const u32 slot : touched_chains_) {
    const ChainSlot chain = chains_[slot];
    for (u32 k = 0; k < k_top_chains; ++k) {
      const ChainCount& top = current_.top_chains[k];
      const bool better = top.root.is_null() || chain.events > top.events ||
                          (chain.events == top.events && chain.root < top.root);
      if (!better) continue;
      for (u32 m = k_top_chains - 1u; m > k; --m) {
        current_.top_chains[m] = current_.top_chains[m - 1u];
      }
      current_.top_chains[k] = ChainCount{chain.root, chain.events, 0};
      break;
    }
    chains_[slot] = ChainSlot{};
  }
  touched_chains_.clear();
  for (const u32 source : touched_sources_)
    source_count(source) = 0;
  touched_sources_.clear();
  cascade_delivered_ = 0;
  current_.carried_out = carry_.size();

  const GuardTrips& t = current_.trips;
  total_trips_.depth_dropped += t.depth_dropped;
  total_trips_.cascade_carried += t.cascade_carried;
  total_trips_.storm_carried += t.storm_carried;
  total_trips_.self_loops += t.self_loops;
  total_trips_.budget_skipped += t.budget_skipped;
  total_trips_.full_dropped += t.full_dropped;
  total_trips_.slow_subscribers += t.slow_subscribers;
  total_trips_.key_ties += t.key_ties;
  // Logged on the tick a guard starts tripping, not on every tick it keeps tripping: the report is
  // the record, the log the alarm.
  if (t.total() != 0 && last_.trips.total() == 0) {
    ENGINE_LOG_WARN(
        log_events, "event bus guard tripped", log::field("tick", current_.tick),
        log::field("depth_dropped", t.depth_dropped),
        log::field("cascade_carried", t.cascade_carried),
        log::field("storm_carried", t.storm_carried), log::field("self_loops", t.self_loops),
        log::field("budget_skipped", t.budget_skipped), log::field("full_dropped", t.full_dropped),
        log::field("slow_subscribers", t.slow_subscribers), log::field("key_ties", t.key_ties));
  }
  last_ = current_;
}

void EventBus::read_document() {
  if (document_ == nullptr) return;
  const u64 revision = document_->revision();
  if (revision == document_revision_) return;
  Emitter e = emitter(make_source(Origin::Document, 0));
  changed_.clear();
  if (!document_->changed_since(document_revision_, changed_)) {
    // The feed no longer reaches back: one event saying so, and a subscriber rescans.
    DocumentResynced resynced;
    resynced.revision = revision;
    e.raise_bytes(document_resynced_, &resynced, sizeof(resynced), alignof(DocumentResynced), {});
  } else {
    for (const Id128 id : changed_) {
      RecordChanged changed;
      changed.record = id;
      changed.revision = document_->revision_of(id);
      changed.live = document_->exists(id);
      if (changed.live) changed.record_type = schema::stable_type_id(document_->type_of(id));
      e.raise_bytes(record_changed_, &changed, sizeof(changed), alignof(RecordChanged), {});
    }
  }
  document_revision_ = revision;
}

void EventBus::count_chain(const EventRecord& record) {
  const u32 mask = config_.chain_slots - 1u;
  u32 slot = static_cast<u32>(engine::mix64(record.root.value)) & mask;
  for (u32 probe = 0; probe < 8u; ++probe, slot = (slot + 1u) & mask) {
    ChainSlot& chain = chains_[slot];
    if (chain.root == record.root) {
      ++chain.events;
      return;
    }
    if (chain.root.is_null()) {
      chain.root = record.root;
      chain.events = 1;
      touched_chains_.push_back(slot);
      return;
    }
  }
  ++current_.chains_untracked;
}

void EventBus::hash_delivery(const EventRecord& record, const void* bytes,
                             u16 subscriber) noexcept {
  // A chain of one-multiply word mixes rather than a full finalizer per field: it is a check that
  // two runs delivered the same things in the same order, not a digest anyone keys on, and the
  // full form cost half of a delivery (docs/subsystems/events.md, "Performance"). Integer
  // arithmetic only, so every toolchain computes the same value.
  using engine::hash_detail::mix_word;
  u64 h = delivery_hash_;
  h = mix_word(h, record.id.value);
  if (bytes == nullptr) {
    // One subscriber heard it: the event is already in the chain.
    delivery_hash_ = mix_word(h, static_cast<u64>(subscriber) + 1u);
    return;
  }
  h = mix_word(h, (static_cast<u64>(record.type) << 48) | (static_cast<u64>(record.depth) << 40) |
                      record.phase);
  h = mix_word(h, (static_cast<u64>(record.source) << 32) | record.seq);
  h = mix_word(h, record.key);
  // The payload's own bytes and none past them: the arena rounds a slot to 8 and the rest of the
  // last word is whatever an earlier tick left there.
  const auto* p = static_cast<const u8*>(bytes);
  u32 left = types_[record.type]->size;
  for (; left >= 8u; left -= 8u, p += 8) {
    u64 word;
    std::memcpy(&word, p, 8);
    h = mix_word(h, word);
  }
  if (left > 0) {
    u64 word = 0;
    std::memcpy(&word, p, left);
    h = mix_word(h, word ^ (static_cast<u64>(left) << 56));
  }
  delivery_hash_ = h;
}

void EventBus::deliver(sim::TickPhase point, u64 tick) {
  if (tick != tick_) begin_tick(tick);
  phase_ = static_cast<u8>(point);
  // What a full bank kept is settled: the wave that raised it has been waited for.
  close_selection();
  delivering_ = true;

  if (point == sim::TickPhase::EventsIn) {
    // What the last tick deferred comes first in the order: it was raised earlier.
    const u32 carried = carry_.size();
    for (u32 i = 0; i < carried; ++i) {
      const EventRecord& record = carry_.records[i];
      if (append(live_, record, carry_.payload(record), 8u, false)) {
        ++current_.carried_in;
      } else {
        ++current_.trips.full_dropped;
        ++type_counters_[record.type].dropped;
      }
    }
    carry_.reset();
    read_document();
  }
  current_.trips.full_dropped += full_refused_.exchange(0, std::memory_order_relaxed);

  const u32 n = live_.size();
  if (n == 0) {
    live_.reset();
    delivering_ = false;
    return;
  }

  // The order: (tick, phase), source, key, sequence, and the bank position last, which only
  // decides between two raises with one whole key — serial ones in the order they ran, parallel
  // ones in the order they landed, which is why those are counted.
  keys_.resize(n);
  bool in_order = true;
  for (u32 i = 0; i < n; ++i) {
    const EventRecord& record = live_.records[i];
    SortKey& key = keys_[i];
    key.when = (record.tick << 8) | record.phase;
    key.key = record.key;
    key.source = record.source;
    key.seq = record.seq;
    key.index = i;
    if (i > 0 && in_order) in_order = key_less(keys_[i - 1u], key);
  }
  if (!in_order) {
    ++current_.sorted;
    std::sort(keys_.begin(), keys_.end(), &EventBus::key_less);
  }
  order_.clear();
  for (u32 k = 0; k < n; ++k) {
    if (k > 0 && same_key(keys_[k], keys_[k - 1u])) ++current_.trips.key_ties;
    order_.push_back(keys_[k].index);
  }

  u32 first = 0;
  u32 assigned = 0;  // records of `live_` given their place in the order so far
  while (first < order_.size()) {
    const u32 last = order_.size();
    assigned = live_.size();
    deliver_generation(first, last, point);
    // What the handlers raised is the next generation, in the order they raised it: delivery is
    // serial, so that order is already the same on every run.
    const u32 grown = live_.size();
    for (u32 i = assigned; i < grown; ++i)
      order_.push_back(i);
    first = last;
  }

  for (u16 s = 0; s < subscribers_.size(); ++s) {
    Subscriber& subscriber = subscribers_[s];
    if (subscriber.skipped == 0) continue;
    if (subscriber.desc.overflow != nullptr) {
      subscriber.desc.overflow(subscriber.desc.context, subscriber.skipped);
    }
    subscriber.skipped = 0;
  }
  current_.trips.full_dropped += full_refused_.exchange(0, std::memory_order_relaxed);
  live_.reset();
  delivering_ = false;
}

void EventBus::deliver_generation(u32 first, u32 last, sim::TickPhase point) {
  // The cascade and storm guards, in the generation's order, so which events wait is a function of
  // the order and never of the threads.
  admitted_.clear();
  // Consecutive events mostly share a source, so its counter is looked up once a run.
  u32 counted_source = 0xFFFF'FFFFu;
  u32* counted = nullptr;
  for (u32 k = first; k < last; ++k) {
    const u32 position = order_[k];
    EventRecord& record = live_.records[position];
    // Its place in the order is fixed now, so it gets its id here, carried or delivered.
    if (record.id.is_null()) {
      record.id = next_id();
      ++current_.raised;
      ++type_counters_[record.type].raised;
    }
    if (record.root.is_null()) record.root = record.id;
    const void* bytes = live_.payload(record);
    if (record.depth > 0 && cascade_delivered_ >= config_.max_cascade) {
      carry(record, bytes, &current_.trips.cascade_carried);
      continue;
    }
    if (record.source != counted_source) {
      counted_source = record.source;
      counted = &source_count(record.source);
    }
    u32& count = *counted;
    if (count >= config_.max_per_source) {
      carry(record, bytes, &current_.trips.storm_carried);
      continue;
    }
    if (count == 0) touched_sources_.push_back(record.source);
    ++count;
    if (record.depth > 0) {
      ++cascade_delivered_;
      count_chain(record);
    }
    ++current_.delivered;
    ++type_counters_[record.type].delivered;
    ++current_.depth[record.depth < k_depth_buckets ? record.depth : k_depth_buckets - 1u];
    if (record.depth > current_.max_depth) current_.max_depth = record.depth;
    if (config_.hash_deliveries) hash_delivery(record, bytes, 0xFFFFu);
    admitted_.push_back(position);
  }
  if (admitted_.empty() || subscribers_.empty()) return;

  // Each type's run of the generation, in order: a counting sort of ranks into `admitted_`.
  const u32 type_count = types_.size();
  type_first_.resize(type_count + 1u);
  for (u32 t = 0; t <= type_count; ++t)
    type_first_[t] = 0;
  for (const u32 position : admitted_)
    ++type_first_[live_.records[position].type + 1u];
  for (u32 t = 0; t < type_count; ++t)
    type_first_[t + 1u] += type_first_[t];
  by_type_.resize(admitted_.size());
  // `type_first_[t]` is type t's start; filling advances it to its end, and the shift below puts
  // the starts back, so no second array of fill pointers is needed.
  for (u32 rank = 0; rank < admitted_.size(); ++rank) {
    const u16 type = live_.records[admitted_[rank]].type;
    by_type_[type_first_[type]++] = rank;
  }
  // `type_first_[t]` is now the end of type t's run; the start is the previous type's end.
  for (u32 t = type_count; t > 0; --t)
    type_first_[t] = type_first_[t - 1u];
  type_first_[0] = 0;

  for (u16 s = 0; s < subscribers_.size(); ++s) {
    Subscriber& subscriber = subscribers_[s];
    const u32 heard = subscriber.types.size();
    if (heard == 0 || subscriber.desc.handle == nullptr) continue;
    cursors_.resize(2u * heard);
    bool any = false;
    for (u32 j = 0; j < heard; ++j) {
      const u16 type = subscriber.types[j];
      cursors_[2u * j] = type_first_[type];
      cursors_[2u * j + 1u] = type_first_[type + 1u];
      any = any || cursors_[2u * j] < cursors_[2u * j + 1u];
    }
    if (!any) continue;

    const i64 started = time::monotonic_ns();
    for (;;) {
      u32 best = 0xFFFF'FFFFu;
      if (heard == 1) {
        // One type: its run is already in the order.
        if (cursors_[0] >= cursors_[1]) break;
        best = by_type_[cursors_[0]++];
      } else {
        // The earliest of its types' next events: the subscriber hears its events in the order.
        u32 best_j = 0;
        for (u32 j = 0; j < heard; ++j) {
          if (cursors_[2u * j] >= cursors_[2u * j + 1u]) continue;
          const u32 rank = by_type_[cursors_[2u * j]];
          if (rank < best) {
            best = rank;
            best_j = j;
          }
        }
        if (best == 0xFFFF'FFFFu) break;
        ++cursors_[2u * best_j];
      }

      const EventRecord& record = live_.records[admitted_[best]];
      if (record.tile != k_any_tile && subscriber.desc.wants_tile != nullptr &&
          !subscriber.desc.wants_tile(subscriber.desc.context, record.tile)) {
        continue;
      }
      if (subscriber.delivered >= subscriber.budget) {
        ++subscriber.skipped;
        ++current_.trips.budget_skipped;
        continue;
      }
      ++subscriber.delivered;
      ++current_.deliveries;

      Emitter e;
      e.bus_ = this;
      e.source_ = make_source(Origin::Subscriber, s);
      e.key_ = record.id.value;
      e.cause_ = record.id;
      e.root_ = record.root;
      e.subscriber_ = s;
      e.phase_ = static_cast<u8>(point);
      e.depth_ = static_cast<u8>(record.depth < 255u ? record.depth + 1u : 255u);
      const Event event{&record, live_.payload(record)};
      subscriber.desc.handle(subscriber.desc.context, event, e);
      if (config_.hash_deliveries) hash_delivery(record, nullptr, s);
    }
    subscriber.ns += time::monotonic_ns() - started;
  }
}

u32 EventBus::pending() const noexcept {
  return (selection_.active ? selection_.live : live_.size()) + carry_.size();
}

}  // namespace engine::events
