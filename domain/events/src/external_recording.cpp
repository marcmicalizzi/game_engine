// A recorded run's external raises (events.h, `ExternalRecording`; events.md, "Events and the
// replay"; roadmap T53).
#include <domain/events/events.h>

#include <foundation/io/vfs.h>

#include <cstring>

namespace engine::events {

namespace {

// "ENGEXT01", u32 version, u32 0, u64 entry count, u64 payload bytes, then each entry as its ten
// fields (three u64, then six u32 and a u32 0), then the payload arena. Little-endian.
constexpr char k_magic[8] = {'E', 'N', 'G', 'E', 'X', 'T', '0', '1'};
constexpr u32 k_version = 1;
constexpr u64 k_entry_bytes = 3 * 8 + 7 * 4;

template <class T>
void put(std::string& out, T value) {
  char bytes[sizeof(T)];
  std::memcpy(bytes, &value, sizeof(T));
  out.append(bytes, sizeof(T));
}

template <class T>
bool get(std::string_view bytes, usize& at, T& value) {
  if (bytes.size() - at < sizeof(T)) return false;
  std::memcpy(&value, bytes.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

void fail(std::string* error, std::string_view why) {
  if (error != nullptr) *error = std::string(why);
}

}  // namespace

void ExternalRecording::record_hook(void* context, const ExternalRaise& raise) {
  static_cast<ExternalRecording*>(context)->record(raise);
}

void ExternalRecording::record(const ExternalRaise& raise) {
  Entry e;
  e.tick = raise.tick;
  e.key = raise.key;
  e.tile = raise.tile;
  e.source = raise.source;
  e.seq = raise.seq;
  e.type_id = raise.type_id;
  e.phase = raise.phase;
  e.offset = static_cast<u32>(payload_.size());
  e.bytes = static_cast<u32>(raise.payload.size());
  for (const u8 b : raise.payload)
    payload_.push_back(b);
  entries_.push_back(e);
}

void ExternalRecording::clear() noexcept {
  entries_.clear();
  payload_.clear();
  cursor_ = 0;
}

ExternalRaise ExternalRecording::at(u32 index) const noexcept {
  ExternalRaise r;
  if (index >= entries_.size()) return r;
  const Entry& e = entries_[index];
  r.tick = e.tick;
  r.key = e.key;
  r.tile = e.tile;
  r.source = e.source;
  r.seq = e.seq;
  r.type_id = e.type_id;
  r.phase = static_cast<u8>(e.phase);
  r.payload = std::span<const u8>(payload_.data() + e.offset, e.bytes);
  return r;
}

u32 ExternalRecording::replay_tick(EventBus& bus, u64 tick, u32* refused) {
  u32 taken = 0;
  u32 no = 0;
  while (cursor_ < entries_.size() && entries_[cursor_].tick <= tick) {
    if (entries_[cursor_].tick == tick) {
      if (bus.replay_external(at(cursor_))) {
        ++taken;
      } else {
        ++no;
      }
    } else {
      ++no;  // a raise of a tick already past: the replay was not called for it
    }
    ++cursor_;
  }
  if (refused != nullptr) *refused = no;
  return taken;
}

bool ExternalRecording::save(std::string_view native_path, std::string* error) const {
  std::string out;
  serialize(out);
  const io::Status status = io::write_file_atomic(native_path, out);
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot write the external raises: ") + io::status_name(status));
    return false;
  }
  return true;
}

bool ExternalRecording::load(std::string_view native_path, std::string* error) {
  clear();
  std::string bytes;
  const io::Status status = io::read_file(native_path, bytes);
  if (status != io::Status::Ok) {
    fail(error, std::string("cannot read the external raises: ") + io::status_name(status));
    return false;
  }
  return deserialize(bytes, error);
}

void ExternalRecording::serialize(std::string& out) const {
  out.clear();
  out.reserve(32 + entries_.size() * k_entry_bytes + payload_.size());
  out.append(k_magic, sizeof(k_magic));
  put<u32>(out, k_version);
  put<u32>(out, 0u);
  put<u64>(out, entries_.size());
  put<u64>(out, payload_.size());
  for (const Entry& e : entries_) {
    put<u64>(out, e.tick);
    put<u64>(out, e.key);
    put<u64>(out, e.tile);
    put<u32>(out, e.source);
    put<u32>(out, e.seq);
    put<u32>(out, e.type_id);
    put<u32>(out, e.phase);
    put<u32>(out, e.offset);
    put<u32>(out, e.bytes);
    put<u32>(out, 0u);
  }
  out.append(reinterpret_cast<const char*>(payload_.data()), payload_.size());
}

bool ExternalRecording::deserialize(std::string_view bytes, std::string* error) {
  clear();
  usize at = 0;
  u32 version = 0;
  u32 zero = 0;
  u64 count = 0;
  u64 payload = 0;
  if (bytes.size() < sizeof(k_magic) || std::memcmp(bytes.data(), k_magic, sizeof(k_magic)) != 0) {
    fail(error, "not a file of external raises (no ENGEXT01 header)");
    return false;
  }
  at = sizeof(k_magic);
  if (!get(bytes, at, version) || !get(bytes, at, zero) || !get(bytes, at, count) ||
      !get(bytes, at, payload) || version != k_version) {
    fail(error, "the external raises' header is cut short or of another version");
    return false;
  }
  const u64 left = bytes.size() - at;
  if (count > left / k_entry_bytes || payload != left - count * k_entry_bytes ||
      payload > 0xFFFF'FFFFull) {
    fail(error, "the external raises' counts do not match the file's size");
    return false;
  }
  entries_.reserve(static_cast<u32>(count));  // under the file's size / 52, so under 2^32
  u64 tick_before = 0;
  for (u64 i = 0; i < count; ++i) {
    Entry e;
    u32 pad = 0;
    get(bytes, at, e.tick);
    get(bytes, at, e.key);
    get(bytes, at, e.tile);
    get(bytes, at, e.source);
    get(bytes, at, e.seq);
    get(bytes, at, e.type_id);
    get(bytes, at, e.phase);
    get(bytes, at, e.offset);
    get(bytes, at, e.bytes);
    get(bytes, at, pad);
    if (static_cast<u64>(e.offset) + e.bytes > payload || e.phase >= sim::k_phase_count ||
        e.tick < tick_before) {
      fail(error, "external raise " + std::to_string(i) + " lies outside the file or out of order");
      clear();
      return false;
    }
    tick_before = e.tick;
    entries_.push_back(e);
  }
  payload_.resize(static_cast<u32>(payload));  // checked under 2^32 above
  if (payload > 0) std::memcpy(payload_.data(), bytes.data() + at, static_cast<usize>(payload));
  return true;
}

}  // namespace engine::events
