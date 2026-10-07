#include <core/log/log.h>
#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/world/world.h>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world, "world");

// Read by whoever builds a `RingParams` for a host, once (docs/subsystems/world.md, "The ring's
// rules and defaults"): E34's block layer lays a building in 68–85 µs and the section assembler in
// 5–7 µs, so eight activations are well under a millisecond of ruins on the frame that takes them,
// and a deactivation drops what it held and costs less, so twice as many of those.
tunables::Int ring_max_activations{"world.ring.max_activations", 8, 0, 1 << 16,
                                   "Tiles the ring activates or moves inward per update, nearest "
                                   "first; the rest wait for the next update. 0 is unlimited"};
tunables::Int ring_max_deactivations{"world.ring.max_deactivations", 16, 0, 1 << 16,
                                     "Tiles the ring deactivates or moves outward per update, "
                                     "farthest first. 0 is unlimited"};

bool in_rings(u8 mask, u8 ring) noexcept { return ring < 8 && ((mask >> ring) & 1u) != 0; }

}  // namespace

u32 max_activations_tunable() noexcept { return static_cast<u32>(ring_max_activations.get()); }
u32 max_deactivations_tunable() noexcept { return static_cast<u32>(ring_max_deactivations.get()); }

bool World::configure(const RingParams& params, const char** error) {
  if (!valid_ring_params(params, error)) return false;
  ring_.configure(params);
  return true;
}

u16 World::add_consumer(const TileConsumer& consumer) {
  consumers_.push_back(consumer);
  consumer_stats_.push_back(ConsumerStats{});
  touched_.push_back(0);
  return static_cast<u16>(consumers_.size() - 1);
}

const UpdateStats& World::update(const sim::ObserverSet& observers, u64 tick, bool unlimited) {
  last_ = UpdateStats{};
  last_.tick = tick;
  tick_ = tick;
  observers_.clear();
  for (u32 o = 0; o < observers.size(); ++o) {
    if (observers.weight(o) > 0.0f) observers_.add(observers.position(o), observers.weight(o));
  }
  events_.clear();
  const i64 start = time::monotonic_ns();
  ring_.update(observers_, events_, unlimited);
  last_.ring_ns = time::monotonic_ns() - start;
  // **Past the world's last tile** (`tile_reachable`; world.md, "Where an observer is"): the ring
  // let such an observer observe nothing. Said once when it starts, not every update it lasts.
  const u32 unreached = ring_.stats().unreached;
  if (unreached > 0 && !warned_unreached_) {
    const f64 last_tile_m = 2147483648.0 * static_cast<f64>(ring_.params().tile_size);
    ENGINE_LOG_WARN(log_world, "an observer is past the world's last tile and observes nothing",
                    log::field("observers", unreached), log::field("last_tile_m", last_tile_m));
  }
  warned_unreached_ = unreached > 0;
  dispatch(tick);
  return last_;
}

const UpdateStats& World::clear(u64 tick) {
  last_ = UpdateStats{};
  last_.tick = tick;
  tick_ = tick;
  events_.clear();
  const i64 start = time::monotonic_ns();
  ring_.clear(events_);
  last_.ring_ns = time::monotonic_ns() - start;
  dispatch(tick);
  return last_;
}

bool World::restore(std::span<const TileCoord> tiles, std::span<const u8> rings,
                    const sim::ObserverSet& observers, u64 tick, const char** error) {
  Vector<u64> keys;
  keys.reserve(static_cast<u32>(tiles.size()));
  for (const TileCoord tile : tiles)
    keys.push_back(tile_key(tile));
  observers_.clear();
  for (u32 o = 0; o < observers.size(); ++o) {
    if (observers.weight(o) > 0.0f) observers_.add(observers.position(o), observers.weight(o));
  }
  events_.clear();
  const i64 start = time::monotonic_ns();
  if (!ring_.restore(std::span<const u64>(keys.data(), keys.size()), rings, observers_, events_,
                     error)) {
    return false;
  }
  last_ = UpdateStats{};
  last_.tick = tick;
  tick_ = tick;
  last_.ring_ns = time::monotonic_ns() - start;
  dispatch(tick);
  return true;
}

void World::dispatch(u64 tick) {
  const RingStats& ring = ring_.stats();
  last_.events = events_.size();
  last_.activated = ring.activated;
  last_.changed = ring.changed;
  last_.deactivated = ring.deactivated;
  last_.active = ring.active;
  last_.tracked = ring.tracked;
  last_.deferred_promotions = ring.deferred_promotions;
  last_.deferred_demotions = ring.deferred_demotions;
  last_.unreached = ring.unreached;
  for (u32 r = 0; r < k_max_rings; ++r)
    last_.per_ring[r] = ring.per_ring[r];
  ++updates_;
  total_events_ += events_.size();

  const i64 start = time::monotonic_ns();
  for (u8& t : touched_)
    t = 0;
  const u32 n = consumers_.size();
  for (const TileEvent& event : events_) {
    // Letting go before taking: the consumers this event deactivates, in reverse registration
    // order, then the ones it activates or moves, in registration order. An event can be both —
    // a tile moving from a ring the document acts in to one only the ruins do.
    for (u32 k = n; k > 0; --k) {
      const u32 i = k - 1;
      const TileConsumer& c = consumers_[i];
      const bool was = event.from != k_inactive && in_rings(c.rings, event.from);
      const bool is = event.to != k_inactive && in_rings(c.rings, event.to);
      if (!was || is) continue;
      TileEvent mine = event;
      mine.kind = TileEventKind::Deactivate;
      mine.to = k_inactive;
      ConsumerStats& s = consumer_stats_[i];
      const i64 t0 = time::monotonic_ns();
      if (c.deactivate != nullptr) c.deactivate(c.context, mine);
      const i64 dt = time::monotonic_ns() - t0;
      s.deactivate_ns += dt;
      s.max_deactivate_ns = dt > s.max_deactivate_ns ? dt : s.max_deactivate_ns;
      ++s.deactivations;
      touched_[i] = 1;
    }
    for (u32 i = 0; i < n; ++i) {
      const TileConsumer& c = consumers_[i];
      const bool was = event.from != k_inactive && in_rings(c.rings, event.from);
      const bool is = event.to != k_inactive && in_rings(c.rings, event.to);
      if (!is) continue;
      ConsumerStats& s = consumer_stats_[i];
      TileEvent mine = event;
      bool ok = true;
      const i64 t0 = time::monotonic_ns();
      if (!was) {
        mine.kind = TileEventKind::Activate;
        mine.from = k_inactive;
        if (c.activate != nullptr) ok = c.activate(c.context, mine);
        const i64 dt = time::monotonic_ns() - t0;
        s.activate_ns += dt;
        s.max_activate_ns = dt > s.max_activate_ns ? dt : s.max_activate_ns;
        ++s.activations;
      } else if (event.from != event.to) {
        mine.kind = TileEventKind::ChangeRing;
        if (c.change_ring != nullptr) ok = c.change_ring(c.context, mine);
        const i64 dt = time::monotonic_ns() - t0;
        s.change_ns += dt;
        s.max_change_ns = dt > s.max_change_ns ? dt : s.max_change_ns;
        ++s.changes;
      } else {
        continue;
      }
      touched_[i] = 1;
      if (!ok) {
        ++s.refusals;
        ++last_.refused;
        ENGINE_LOG_WARN(log_world, "a consumer refused a tile",
                        log::field("consumer", c.name != nullptr ? c.name : "?"),
                        log::field("x", event.tile.x), log::field("z", event.tile.z),
                        log::field("ring", static_cast<u32>(event.to)), log::field("tick", tick));
      }
    }
  }
  for (u32 i = 0; i < n; ++i) {
    const TileConsumer& c = consumers_[i];
    if (touched_[i] == 0 || c.commit == nullptr) continue;
    ConsumerStats& s = consumer_stats_[i];
    const i64 t0 = time::monotonic_ns();
    c.commit(c.context);
    const i64 dt = time::monotonic_ns() - t0;
    s.commit_ns += dt;
    s.max_commit_ns = dt > s.max_commit_ns ? dt : s.max_commit_ns;
    ++s.commits;
  }
  last_.dispatch_ns = time::monotonic_ns() - start;

  // Logged after the calls, so the log costs nothing the numbers above count: one line an update
  // that changed anything, and one a tile at debug — a first fill of the default rings is 1,800
  // tiles, and a flythrough's info log is read by people.
  if (events_.empty()) return;
  ENGINE_LOG_INFO(log_world, "tiles", log::field("tick", tick),
                  log::field("activated", last_.activated), log::field("changed", last_.changed),
                  log::field("deactivated", last_.deactivated),
                  log::field("refused", last_.refused), log::field("active", last_.active),
                  log::field("ring_us", static_cast<f64>(last_.ring_ns) / 1.0e3),
                  log::field("dispatch_ms", static_cast<f64>(last_.dispatch_ns) / 1.0e6));
  for (const TileEvent& event : events_) {
    ENGINE_LOG_DEBUG(
        log_world, "tile", log::field("event", tile_event_name(event.kind)),
        log::field("x", event.tile.x), log::field("z", event.tile.z),
        log::field("from", event.from == k_inactive ? -1 : static_cast<i32>(event.from)),
        log::field("to", event.to == k_inactive ? -1 : static_cast<i32>(event.to)),
        log::field("tick", tick));
  }
}

}  // namespace engine::world
