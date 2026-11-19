// The door's record in the store's event log (door_events.h; world.md, "The door in the store").
#include <systems/world/door_events.h>

namespace engine::world {

// A row's payload is the entry's delivery (one byte) and then the entry's own payload; everything
// else the entry carries is a column of the row (tick, game time, kind as the type, origin).
store::Status append_door_events(store::EventLog& log, const sim::Door& door, u32& cursor) {
  const std::span<const sim::DoorEntry> entries = door.entries();
  if (cursor >= entries.size()) return store::Status::Ok;
  Vector<u8> bytes;
  usize total = 0;
  for (u32 i = cursor; i < entries.size(); ++i)
    total += 1u + entries[i].payload_size;
  bytes.reserve(static_cast<u32>(total));
  for (u32 i = cursor; i < entries.size(); ++i) {
    bytes.push_back(static_cast<u8>(entries[i].delivery));
    const std::span<const u8> p = door.payload(entries[i]);
    bytes.append(p);
  }
  Vector<store::EventRecord> rows;
  rows.reserve(static_cast<u32>(entries.size() - cursor));
  usize at = 0;
  for (u32 i = cursor; i < entries.size(); ++i) {
    const sim::DoorEntry& e = entries[i];
    store::EventRecord row;
    row.tile = k_door_tile;
    row.sim_tick = e.tick;
    row.game_time_us = e.game_time_us;
    row.type = e.kind;
    row.origin = static_cast<store::EventOrigin>(e.origin);
    row.payload = std::span<const u8>(bytes.data() + at, 1u + e.payload_size);
    at += 1u + e.payload_size;
    rows.push_back(row);
  }
  const store::Status status = log.append(std::span<store::EventRecord>(rows.data(), rows.size()));
  if (status == store::Status::Ok) cursor = static_cast<u32>(entries.size());
  return status;
}

namespace {

struct Reader {
  sim::Door* door = nullptr;
  u32 read = 0;
  bool bad = false;
};

void read_row(const store::EventRecord& row, void* user) {
  auto* r = static_cast<Reader*>(user);
  if (r->bad) return;
  if (row.origin == store::EventOrigin::Deterministic || row.payload.empty()) {
    r->bad = true;
    return;
  }
  sim::DoorEntry entry;
  entry.tick = row.sim_tick;
  entry.game_time_us = row.game_time_us;
  entry.kind = row.type;
  entry.origin = static_cast<sim::DoorOrigin>(row.origin);
  entry.delivery = static_cast<sim::DoorDelivery>(row.payload[0]);
  if (!r->door->add_recorded(entry, row.payload.subspan(1))) {
    r->bad = true;
    return;
  }
  ++r->read;
}

}  // namespace

store::Status read_door_events(store::EventLog& log, sim::Door& door, u32* read) {
  if (!door.entries().empty()) return store::Status::InvalidArgument;
  Reader reader;
  reader.door = &door;
  const store::Status status = log.replay(k_door_tile, 0, &read_row, &reader);
  if (read != nullptr) *read = reader.read;
  if (status != store::Status::Ok) return status;
  if (reader.bad) {
    door.clear();
    return store::Status::Corrupt;
  }
  return store::Status::Ok;
}

}  // namespace engine::world
