#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <systems/world/state_hash.h>

#include <string>

namespace engine::world {

namespace {

// Distinct seeds per part, so an empty table hashes differently from an empty document.
constexpr u64 k_seed_clock = 0x636c6f636b000001ull;     // "clock"
constexpr u64 k_seed_document = 0x646f63756d000001ull;  // "docum"
constexpr u64 k_seed_events = 0x6576656e74000001ull;    // "event"
constexpr u64 k_seed_projections = 0x70726f6a65000001ull;
constexpr u64 k_seed_snapshots = 0x736e617073000001ull;
constexpr u64 k_seed_state = 0x7374617465000001ull;

// One row's bytes, little-endian and length-prefixed, into a buffer reused across rows.
struct Row {
  std::string bytes;

  void clear() { bytes.clear(); }
  void u64le(u64 value) {
    for (u32 i = 0; i < 8; ++i)
      bytes.push_back(static_cast<char>((value >> (i * 8)) & 0xFFu));
  }
  void i64le(i64 value) { u64le(static_cast<u64>(value)); }
  void blob(std::span<const u8> data) {
    u64le(data.size());
    bytes.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  void text(std::string_view data) {
    u64le(data.size());
    bytes.append(data.data(), data.size());
  }
  u64 hash() const { return hash_bytes(bytes.data(), bytes.size()); }
};

struct Walk {
  Row row;
  u64 hash = 0;
  u64 count = 0;
};

void hash_event(const store::EventRecord& r, void* user) {
  auto* walk = static_cast<Walk*>(user);
  Row& row = walk->row;
  row.clear();
  row.u64le(r.tile);
  row.u64le(r.sequence);
  row.u64le(r.sim_tick);
  row.i64le(r.game_time_us);
  row.u64le(r.type);
  row.u64le(r.depth);
  row.u64le(static_cast<u64>(r.origin));
  row.u64le(r.subject.hi);
  row.u64le(r.subject.lo);
  row.u64le(r.cause);
  row.u64le(r.payload_version);
  row.blob(r.payload);
  walk->hash = hash_combine(walk->hash, row.hash());
  ++walk->count;
}

void hash_projection(const store::ProjectionRecord& r, void* user) {
  auto* walk = static_cast<Walk*>(user);
  Row& row = walk->row;
  row.clear();
  row.u64le(r.tile);
  row.u64le(r.entity.hi);
  row.u64le(r.entity.lo);
  row.u64le(r.kind);
  row.u64le(r.version);
  row.blob(r.blob);
  walk->hash = hash_combine(walk->hash, row.hash());
  ++walk->count;
}

void hash_snapshot(const store::SnapshotInfo& info, std::span<const u8> blob, void* user) {
  auto* walk = static_cast<Walk*>(user);
  Row& row = walk->row;
  row.clear();
  row.u64le(info.tile);
  row.u64le(info.sequence);
  row.u64le(info.sim_tick);
  row.i64le(info.game_time_us);
  row.u64le(info.record_count);
  row.blob(blob);
  walk->hash = hash_combine(walk->hash, row.hash());
  ++walk->count;
}

}  // namespace

store::Status state_hash(u64 tick, i64 game_time_us, const doc::Document& document,
                         store::EventLog* log, StateHash& out) {
  StateHash h;
  h.clock = hash_combine(hash_combine(k_seed_clock, tick), static_cast<u64>(game_time_us));

  // The document: live records in id order (`objects()` is sorted), each one's composition.
  {
    Row row;
    u64 hash = k_seed_document;
    char hex[Id128::k_hex_length + 1];
    doc::ResolvedObject resolved;
    std::string value;
    for (const Id128& id : document.objects()) {
      if (!document.resolve(id, resolved) || resolved.deleted) continue;
      row.clear();
      id.to_hex(hex);
      row.text(std::string_view(hex, Id128::k_hex_length));
      row.text(resolved.type);
      resolved.parent.to_hex(hex);
      row.text(std::string_view(hex, Id128::k_hex_length));
      row.u64le(resolved.properties.size());
      for (auto [name, json] : resolved.properties) {
        row.text(name);
        value.clear();
        if (json != nullptr) write_json(*json, value, JsonWriteOptions{false});
        row.text(value);
      }
      hash = hash_combine(hash, row.hash());
      ++h.records;
    }
    h.document = hash;
  }

  Walk events{Row{}, k_seed_events, 0};
  Walk projections{Row{}, k_seed_projections, 0};
  Walk snapshots{Row{}, k_seed_snapshots, 0};
  if (log != nullptr) {
    store::Status status = log->visit_events(&hash_event, &events);
    if (status == store::Status::Ok)
      status = log->visit_projections(&hash_projection, &projections);
    if (status == store::Status::Ok) status = log->visit_snapshots(&hash_snapshot, &snapshots);
    if (status != store::Status::Ok) return status;
    h.store = true;
  }
  h.events = hash_combine(events.hash, events.count);
  h.projections = hash_combine(projections.hash, projections.count);
  h.snapshots = hash_combine(snapshots.hash, snapshots.count);
  h.event_count = events.count;
  h.projection_count = projections.count;
  h.snapshot_count = snapshots.count;

  u64 value = hash_combine(k_seed_state, h.clock);
  value = hash_combine(value, h.document);
  value = hash_combine(value, h.events);
  value = hash_combine(value, h.projections);
  value = hash_combine(value, h.snapshots);
  h.value = value;
  out = h;
  return store::Status::Ok;
}

std::string hash_hex(u64 value) {
  static constexpr char k_digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (u32 i = 0; i < 16; ++i)
    out[15 - i] = k_digits[(value >> (i * 4)) & 0xFu];
  return out;
}

bool parse_hash_hex(std::string_view text, u64& out) noexcept {
  if (text.size() != 16) return false;
  u64 value = 0;
  for (const char c : text) {
    u64 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u64>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<u64>(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

}  // namespace engine::world
