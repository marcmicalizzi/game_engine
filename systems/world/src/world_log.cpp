#include <systems/world/world_log.h>

#include <algorithm>
#include <cmath>

namespace engine::world {

namespace {

f64 ms(i64 ns) { return static_cast<f64>(ns) / 1.0e6; }

f64 rank(const Vector<f64>& sorted, f64 p) {
  if (sorted.empty()) return 0.0;
  const f64 position = p * static_cast<f64>(sorted.size() - 1);
  const u32 lo = static_cast<u32>(std::floor(position));
  const u32 hi = std::min<u32>(lo + 1, sorted.size() - 1);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (position - static_cast<f64>(lo));
}

}  // namespace

void WorldLog::before(const World& world) {
  before_.clear();
  for (u16 i = 0; i < world.consumer_count(); ++i)
    before_.push_back(world.consumer_stats(i));
}

TileFrame WorldLog::after(const World& world, u32 repeat, u32 frame, bool recorded) {
  const UpdateStats& s = world.last();
  TileFrame out;
  out.repeat = repeat;
  out.frame = frame;
  out.recorded = recorded;
  out.tick = s.tick;
  out.tracked = s.tracked;
  out.active = s.active;
  for (u32 r = 0; r < world.params().ring_count; ++r)
    out.per_ring.push_back(s.per_ring[r]);
  out.activated = s.activated;
  out.changed = s.changed;
  out.deactivated = s.deactivated;
  out.refused = s.refused;
  out.deferred_promotions = s.deferred_promotions;
  out.deferred_demotions = s.deferred_demotions;
  out.ring_us = static_cast<f64>(s.ring_ns) / 1.0e3;
  out.dispatch_ms = ms(s.dispatch_ns);
  ring_us_.push_back(out.ring_us);
  for (u16 i = 0; i < world.consumer_count(); ++i) {
    const ConsumerStats& now = world.consumer_stats(i);
    const ConsumerStats then = i < before_.size() ? before_[i] : ConsumerStats{};
    TileConsumerFrame c;
    c.name = world.consumer(i).name != nullptr ? world.consumer(i).name : "?";
    c.calls = static_cast<u32>((now.activations - then.activations) + (now.changes - then.changes) +
                               (now.deactivations - then.deactivations));
    c.refused = static_cast<u32>(now.refusals - then.refusals);
    c.ms = ms((now.activate_ns - then.activate_ns) + (now.change_ns - then.change_ns) +
              (now.deactivate_ns - then.deactivate_ns));
    c.commit_ms = ms(now.commit_ns - then.commit_ns);
    out.consumers.push_back(std::move(c));
  }
  return out;
}

TileWorldSummary WorldLog::summary(const World& world) const {
  TileWorldSummary out;
  const RingParams& p = world.params();
  out.tile_size = p.tile_size;
  for (u32 r = 0; r < p.ring_count; ++r)
    out.radius.push_back(p.radius[r]);
  out.hysteresis = p.hysteresis;
  out.max_activations = p.max_activations;
  out.max_deactivations = p.max_deactivations;
  out.updates = world.updates();
  out.events = world.total_events();
  Vector<f64> sorted = ring_us_;
  std::sort(sorted.begin(), sorted.end());
  out.ring_us_median = rank(sorted, 0.5);
  out.ring_us_p99 = rank(sorted, 0.99);
  out.ring_us_max = sorted.empty() ? 0.0 : sorted[sorted.size() - 1];
  for (u16 i = 0; i < world.consumer_count(); ++i) {
    const ConsumerStats& s = world.consumer_stats(i);
    TileConsumerFrame c;
    c.name = world.consumer(i).name != nullptr ? world.consumer(i).name : "?";
    c.calls = static_cast<u32>(s.activations + s.changes + s.deactivations);
    c.refused = static_cast<u32>(s.refusals);
    c.ms = ms(s.activate_ns + s.change_ns + s.deactivate_ns);
    c.commit_ms = ms(s.commit_ns);
    c.max_ms = ms(std::max(std::max(s.max_activate_ns, s.max_change_ns), s.max_deactivate_ns));
    out.consumers.push_back(std::move(c));
  }
  return out;
}

}  // namespace engine::world
