#include <core/base/assert.h>
#include <core/schema/materialize.h>
#include <domain/sim/door.h>
#include <foundation/io/vfs.h>

#include <cstring>

namespace engine::sim {

namespace {

constexpr char k_magic[8] = {'E', 'N', 'G', 'D', 'O', 'O', 'R', '1'};
constexpr usize k_entry_bytes = 32;
constexpr usize k_header_bytes = 16;  // magic, entry count, payload bytes
constexpr u64 k_max_payload = 0xFFFF'FFFFull;

template <class T>
void put(std::string& out, T value) {
  for (usize i = 0; i < sizeof(T); ++i)
    out.push_back(static_cast<char>((static_cast<u64>(value) >> (8 * i)) & 0xFFu));
}

template <class T>
T get(const u8* p) {
  u64 v = 0;
  for (usize i = 0; i < sizeof(T); ++i)
    v |= static_cast<u64>(p[i]) << (8 * i);
  return static_cast<T>(v);
}

void fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

// FNV-1a over bytes, then a word mix: the same value on every toolchain.
u64 mix_bytes(u64 h, const u8* p, usize n) {
  for (usize i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

u64 mix_word(u64 h, u64 v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}

bool known_origin(DoorOrigin origin) {
  const u8 o = static_cast<u8>(origin);
  return o >= 1 && o <= 4;
}

bool known_delivery(DoorDelivery delivery) { return static_cast<u8>(delivery) <= 1; }

}  // namespace

const char* door_origin_name(DoorOrigin origin) noexcept {
  switch (origin) {
    case DoorOrigin::Player: return "player";
    case DoorOrigin::Agent: return "agent";
    case DoorOrigin::Llm: return "llm";
    case DoorOrigin::Debug: return "debug";
  }
  return "unknown";
}

u32 door_input_kind() noexcept {
  static const u32 kind = schema::stable_type_id("engine.input.RawEvent");
  return kind;
}

u32 door_flush_kind() noexcept {
  static const u32 kind = schema::stable_type_id("engine.sim.Flush");
  return kind;
}

void Door::install(SimScheduler& scheduler) {
  ENGINE_ASSERT(scheduler_ == nullptr, "sim: a door is installed once");
  scheduler_ = &scheduler;
  SystemDesc desc;
  desc.name = "sim.door";
  desc.phase = TickPhase::Input;
  desc.context = this;
  desc.begin_tick = &Door::input_phase;
  scheduler.add_system(desc);
}

void Door::on(u32 kind, const DoorHandler& handler) {
  for (KindHandler& h : handlers_) {
    if (h.kind == kind) {
      h.handler = handler;
      return;
    }
  }
  handlers_.push_back(KindHandler{kind, handler});
}

const DoorHandler* Door::handler(u32 kind) const noexcept {
  for (const KindHandler& h : handlers_) {
    if (h.kind == kind && h.handler.apply != nullptr) return &h.handler;
  }
  return nullptr;
}

void Door::encode_input(const input::RawEvent& event, u8 (&out)[13]) noexcept {
  u32 value_bits = 0;
  std::memcpy(&value_bits, &event.value, sizeof(value_bits));
  out[0] = static_cast<u8>(event.source);
  for (u32 i = 0; i < 4; ++i) {
    out[1 + i] = static_cast<u8>(event.code >> (8 * i));
    out[5 + i] = static_cast<u8>(value_bits >> (8 * i));
    out[9 + i] = static_cast<u8>(event.device >> (8 * i));
  }
}

bool Door::decode_input(std::span<const u8> payload, input::RawEvent& out) noexcept {
  if (payload.size() != 13 || payload[0] >= input::k_source_count) return false;
  out.source = static_cast<input::Source>(payload[0]);
  out.code = get<u32>(payload.data() + 1);
  const u32 value_bits = get<u32>(payload.data() + 5);
  std::memcpy(&out.value, &value_bits, sizeof(value_bits));
  out.device = get<u32>(payload.data() + 9);
  return true;
}

bool Door::submit_input(const input::RawEvent& event, DoorOrigin origin) {
  u8 bytes[13];
  encode_input(event, bytes);
  return submit(origin, DoorDelivery::Tick, door_input_kind(), std::span<const u8>(bytes, 13));
}

bool Door::submit(DoorOrigin origin, DoorDelivery delivery, u32 kind, std::span<const u8> payload) {
  if (replaying_) {
    ++stats_.refused_live;
    return false;
  }
  if (!known_origin(origin) || !known_delivery(delivery) ||
      payload_.size() + payload.size() > k_max_payload)
    return false;
  DoorEntry entry;
  entry.tick = scheduler_ != nullptr ? scheduler_->tick().value + 1 : 1;
  entry.game_time_us = scheduler_ != nullptr ? scheduler_->game_time().us : 0;
  entry.kind = kind;
  entry.payload_offset = static_cast<u32>(payload_.size());
  entry.payload_size = static_cast<u32>(payload.size());
  entry.origin = origin;
  entry.delivery = delivery;
  payload_.append(payload);
  entries_.push_back(entry);
  ++stats_.submitted;
  // A Between entry is in the world as it is recorded; a Tick entry when it is delivered.
  if (delivery == DoorDelivery::Between) mix(entry);
  return true;
}

std::span<const u8> Door::payload(const DoorEntry& entry) const noexcept {
  return std::span<const u8>(payload_.data() + entry.payload_offset, entry.payload_size);
}

bool Door::add_recorded(const DoorEntry& entry, std::span<const u8> payload) {
  if (!known_origin(entry.origin) || !known_delivery(entry.delivery) || entry.reserved != 0)
    return false;
  if (!entries_.empty() && entry.tick < entries_.back().tick) return false;
  if (payload_.size() + payload.size() > k_max_payload) return false;
  DoorEntry copy = entry;
  copy.payload_offset = static_cast<u32>(payload_.size());
  copy.payload_size = static_cast<u32>(payload.size());
  payload_.append(payload);
  entries_.push_back(copy);
  return true;
}

void Door::clear() noexcept {
  entries_.clear();
  payload_.clear();
  tick_cursor_ = 0;
  between_cursor_ = 0;
  replaying_ = false;
  delivered_hash_ = 0;
  stats_ = DoorStats{};
}

usize Door::memory_bytes() const noexcept {
  return entries_.capacity() * sizeof(DoorEntry) + payload_.capacity();
}

void Door::begin_replay() {
  replaying_ = true;
  const u64 now = scheduler_ != nullptr ? scheduler_->tick().value : 0;
  tick_cursor_ = 0;
  while (tick_cursor_ < entries_.size() && entries_[tick_cursor_].tick <= now)
    ++tick_cursor_;
  between_cursor_ = tick_cursor_;
  delivered_hash_ = 0;
}

void Door::mix(const DoorEntry& entry) {
  u64 h = mix_word(delivered_hash_, entry.tick);
  h = mix_word(h, (static_cast<u64>(entry.kind) << 16) | (static_cast<u64>(entry.origin) << 8) |
                      static_cast<u64>(entry.delivery));
  const std::span<const u8> bytes = payload(entry);
  h = mix_word(h, mix_bytes(0xCBF29CE484222325ull, bytes.data(), bytes.size()));
  delivered_hash_ = h;
}

void Door::apply(const DoorEntry& entry) {
  if (const DoorHandler* h = handler(entry.kind)) {
    h->apply(h->context, entry, payload(entry));
  } else {
    ++stats_.unhandled;
  }
}

u32 Door::replay_pending() {
  if (!replaying_) return 0;
  const u64 upto = scheduler_ != nullptr ? scheduler_->tick().value + 1 : ~0ull;
  u32 applied = 0;
  while (between_cursor_ < entries_.size() && entries_[between_cursor_].tick <= upto) {
    const DoorEntry& entry = entries_[between_cursor_++];
    if (entry.delivery != DoorDelivery::Between) continue;
    mix(entry);
    apply(entry);
    ++stats_.replayed;
    ++applied;
  }
  return applied;
}

void Door::input_phase(SystemContext& context) {
  static_cast<Door*>(context.context)->deliver(context.tick.value);
}

void Door::deliver(u64 tick) {
  // In a replay, first what the recorded host did between the last tick and this one: it happened
  // before anything of this tick, so it is applied before this tick's input is.
  if (replaying_) {
    while (between_cursor_ < entries_.size() && entries_[between_cursor_].tick <= tick) {
      const DoorEntry& entry = entries_[between_cursor_++];
      if (entry.delivery != DoorDelivery::Between) continue;
      mix(entry);
      apply(entry);
      ++stats_.replayed;
    }
  }
  if (input_ != nullptr) {
    input_->begin_tick(SimTick{tick});
    ++stats_.input_ticks;
  }
  const u32 input_kind = door_input_kind();
  while (tick_cursor_ < entries_.size() && entries_[tick_cursor_].tick <= tick) {
    const DoorEntry& entry = entries_[tick_cursor_++];
    // An entry of a tick already past was submitted after its tick began: it is late, which a
    // live door never makes (it stamps the next tick) and a replay skips at its start.
    if (entry.delivery != DoorDelivery::Tick || entry.tick != tick) continue;
    mix(entry);
    ++stats_.delivered;
    if (entry.kind == input_kind && input_ != nullptr) {
      input::RawEvent event;
      if (decode_input(payload(entry), event)) {
        event.tick = SimTick{tick};
        input_->feed(event);
      }
      continue;
    }
    apply(entry);
  }
  if (input_ != nullptr) input_->end_tick();
}

void Door::serialize(std::string& out) const {
  out.clear();
  out.reserve(k_header_bytes + entries_.size() * k_entry_bytes + payload_.size());
  out.append(k_magic, sizeof(k_magic));
  put<u32>(out, static_cast<u32>(entries_.size()));
  put<u32>(out, static_cast<u32>(payload_.size()));
  for (const DoorEntry& e : entries_) {
    put<u64>(out, e.tick);
    put<u64>(out, static_cast<u64>(e.game_time_us));
    put<u32>(out, e.kind);
    put<u32>(out, e.payload_offset);
    put<u32>(out, e.payload_size);
    put<u8>(out, static_cast<u8>(e.origin));
    put<u8>(out, static_cast<u8>(e.delivery));
    put<u16>(out, 0);
  }
  out.append(reinterpret_cast<const char*>(payload_.data()), payload_.size());
}

bool Door::deserialize(std::string_view bytes, std::string* error) {
  clear();
  const auto* p = reinterpret_cast<const u8*>(bytes.data());
  if (bytes.size() < k_header_bytes || std::memcmp(p, k_magic, sizeof(k_magic)) != 0) {
    fail(error, "not a door record (ENGDOOR1)");
    return false;
  }
  const u64 count = get<u32>(p + 8);
  const u64 payload_bytes = get<u32>(p + 12);
  if (bytes.size() != k_header_bytes + count * k_entry_bytes + payload_bytes) {
    fail(error, "the door record's size is not what its header says");
    return false;
  }
  const u8* payload = p + k_header_bytes + count * k_entry_bytes;
  entries_.reserve(static_cast<u32>(count));
  payload_.reserve(static_cast<u32>(payload_bytes));
  // **The entries' payloads in order and apart** (T194, from the review of batch 69's tree): each
  // entry's bytes are copied, so entries that all named the same bytes made the copy the payload
  // times the count — 4,096 entries over 1 MiB of a 1.1 MiB file reached the 4 GiB cap before the
  // cap refused it. An entry begins at or after the end of the one before it, so the copies sum to
  // no more than the payload the file holds; the writer lays them end to end, so it always does.
  u64 previous_end = 0;
  for (u64 i = 0; i < count; ++i) {
    const u8* e = p + k_header_bytes + i * k_entry_bytes;
    DoorEntry entry;
    entry.tick = get<u64>(e);
    entry.game_time_us = static_cast<i64>(get<u64>(e + 8));
    entry.kind = get<u32>(e + 16);
    const u64 offset = get<u32>(e + 20);
    const u64 size = get<u32>(e + 24);
    entry.origin = static_cast<DoorOrigin>(e[28]);
    entry.delivery = static_cast<DoorDelivery>(e[29]);
    entry.reserved = get<u16>(e + 30);
    if (offset < previous_end) {
      clear();
      fail(error, "door record entry " + std::to_string(i) + " names payload bytes " +
                      std::to_string(offset) + " to " + std::to_string(offset + size) +
                      ", which overlap or come before the entry before it's (to " +
                      std::to_string(previous_end) + ")");
      return false;
    }
    previous_end = offset + size;
    if (offset + size > payload_bytes ||
        !add_recorded(entry, std::span<const u8>(payload + offset, static_cast<usize>(size)))) {
      clear();
      fail(error, "door record entry " + std::to_string(i) +
                      " is out of order, outside its payload, or of an unknown origin");
      return false;
    }
  }
  return true;
}

bool Door::save(std::string_view native_path, std::string* error) const {
  std::string out;
  serialize(out);
  const io::Status status = io::write_file_atomic(native_path, out);
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot write the door record: ") + io::status_name(status));
    return false;
  }
  return true;
}

bool Door::load(std::string_view native_path, std::string* error) {
  std::string bytes;
  const io::Status status = io::read_file(native_path, bytes);
  if (status != io::Status::Ok) {
    clear();
    fail(error, std::string("cannot read the door record: ") + io::status_name(status));
    return false;
  }
  return deserialize(bytes, error);
}

}  // namespace engine::sim
