// External raises through the one recorded door (events.h, `DoorBinding`; events.md, "Through the
// door"; roadmap S18).
#include <domain/events/events.h>

#include <core/schema/materialize.h>

namespace engine::events {

namespace {

// The raise's key, then its payload: u64 tick, key, tile; u32 source, seq, type id; u8 phase.
// Little-endian, so the door's record and the store's row are the same bytes on every machine.
constexpr usize k_head = 3 * 8 + 3 * 4 + 1;

void put(Vector<u8>& out, u64 value, u32 bytes) {
  for (u32 i = 0; i < bytes; ++i)
    out.push_back(static_cast<u8>(value >> (8 * i)));
}

u64 get(const u8* p, u32 bytes) {
  u64 v = 0;
  for (u32 i = 0; i < bytes; ++i)
    v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

}  // namespace

DoorBinding::DoorBinding(EventBus& bus, sim::Door& door, sim::DoorOrigin origin)
    : bus_(&bus), door_(&door), origin_(origin) {
  bus.set_external_log(ExternalLog{this, &DoorBinding::record_hook});
  door.on(kind(), sim::DoorHandler{this, &DoorBinding::replay});
}

void DoorBinding::set_origin(u32 source, sim::DoorOrigin origin) {
  for (auto& [s, o] : origins_) {
    if (s == source) {
      o = origin;
      return;
    }
  }
  origins_.push_back({source, origin});
}

u32 DoorBinding::kind() noexcept {
  static const u32 id = schema::stable_type_id("engine.events.ExternalRaise");
  return id;
}

void DoorBinding::record_hook(void* context, const ExternalRaise& raise) {
  auto* self = static_cast<DoorBinding*>(context);
  sim::DoorOrigin origin = self->origin_;
  for (const auto& [s, o] : self->origins_) {
    if (s == raise.source) origin = o;
  }
  Vector<u8>& b = self->scratch_;
  b.clear();
  put(b, raise.tick, 8);
  put(b, raise.key, 8);
  put(b, raise.tile, 8);
  put(b, raise.source, 4);
  put(b, raise.seq, 4);
  put(b, raise.type_id, 4);
  put(b, raise.phase, 1);
  b.append(raise.payload);
  // Live, the bus has the raise already; the door records it. In a replay the bus raises nothing
  // external of its own accord, so this is not reached.
  self->door_->record_between(origin, kind(), std::span<const u8>(b.data(), b.size()));
}

void DoorBinding::replay(void* context, const sim::DoorEntry&, std::span<const u8> payload) {
  auto* self = static_cast<DoorBinding*>(context);
  if (payload.size() < k_head) {
    ++self->refused_;
    return;
  }
  const u8* p = payload.data();
  ExternalRaise raise;
  raise.tick = get(p, 8);
  raise.key = get(p + 8, 8);
  raise.tile = get(p + 16, 8);
  raise.source = static_cast<u32>(get(p + 24, 4));
  raise.seq = static_cast<u32>(get(p + 28, 4));
  raise.type_id = static_cast<u32>(get(p + 32, 4));
  raise.phase = static_cast<u8>(p[36]);
  raise.payload = payload.subspan(k_head);
  if (!self->bus_->replay_external(raise)) ++self->refused_;
}

}  // namespace engine::events
