#include <core/base/assert.h>
#include <systems/world/tile_ring.h>

#include <algorithm>
#include <cmath>

namespace engine::world {

namespace {

// The tiles an i32 index names, as the f64 a floored quotient is compared with: every tile index
// is exact in f64.
constexpr f64 k_first_tile = -2147483648.0;
constexpr f64 k_last_tile = 2147483647.0;

// Whether [floor(t - reach), floor(t + reach)] is all i32s: the window of tiles round a place `t`
// tiles from the origin. False for anything not finite.
bool window_reachable(f64 t, f64 reach) noexcept {
  return std::floor(t - reach) >= k_first_tile && std::floor(t + reach) <= k_last_tile;
}

}  // namespace

bool tile_reachable(WorldPos position, f32 tile_size, f64 reach_tiles) noexcept {
  const f64 size = static_cast<f64>(tile_size);
  if (!(size > 0.0) || !(reach_tiles >= 0.0)) return false;
  return window_reachable(position.x / size, reach_tiles) &&
         window_reachable(position.z / size, reach_tiles);
}

TileCoord tile_at(WorldPos position, f32 tile_size) noexcept {
  ENGINE_ASSERT(tile_reachable(position, tile_size), "a position past the world's last tile");
  const f64 size = static_cast<f64>(tile_size);
  return TileCoord{static_cast<i32>(std::floor(position.x / size)),
                   static_cast<i32>(std::floor(position.z / size))};
}

WorldPos tile_center(TileCoord tile, f32 tile_size) noexcept {
  // Exact in f64 for any tile an i32 names: an integer and a half, times the size.
  const f64 size = static_cast<f64>(tile_size);
  return WorldPos{(static_cast<f64>(tile.x) + 0.5) * size, 0.0,
                  (static_cast<f64>(tile.z) + 0.5) * size};
}

bool valid_ring_params(const RingParams& params, const char** error) noexcept {
  const char* why = nullptr;
  if (!(params.tile_size > 0.0f) || !std::isfinite(params.tile_size)) {
    why = "the tile size must be positive";
  } else if (params.ring_count < 1 || params.ring_count > k_max_rings) {
    why = "a ring has 1 to 7 rings";
  } else if (!(params.hysteresis >= 0.0f) || !std::isfinite(params.hysteresis)) {
    why = "the hysteresis must be zero or more";
  } else {
    for (u32 r = 0; r < params.ring_count && why == nullptr; ++r) {
      if (!(params.radius[r] > 0.0f) || !std::isfinite(params.radius[r])) {
        why = "every ring's radius must be positive";
      } else if (r > 0 && !(params.radius[r] > params.radius[r - 1])) {
        why = "the rings' radii must increase outward";
      }
    }
  }
  if (error != nullptr) *error = why;
  return why == nullptr;
}

const char* tile_event_name(TileEventKind kind) noexcept {
  switch (kind) {
    case TileEventKind::Activate: return "activate";
    case TileEventKind::ChangeRing: return "change_ring";
    case TileEventKind::Deactivate: return "deactivate";
  }
  return "?";
}

void TileRing::configure(const RingParams& params) {
  params_ = params;
  // One tier per ring and one more for "inactive": `TierAssignment` bands a score by the first
  // `tier_count - 1` boundaries, and a score past the last is the last tier.
  tiers_params_ = sim::TierParams{};
  tiers_params_.tier_count = params.ring_count + 1;
  for (u32 r = 0; r < params.ring_count; ++r)
    tiers_params_.boundaries[r] = params.radius[r] * params.tile_size;
  tiers_params_.hysteresis = params.hysteresis;
  tiers_params_.min_importance = 1.0f;
  tiers_params_.max_promotions = params.max_activations;
  tiers_params_.max_demotions = params.max_deactivations;
  // A tile still active past the outermost radius stays tracked (it is in `active_keys_`), so the
  // window only has to reach the tiles that could be *promoted*: those inside the outermost radius.
  // One tile of slack covers a centre just inside it from an observer in the next tile over.
  reach_tiles_ = params.ring_count > 0 ? params.radius[params.ring_count - 1] + 1.0f : 0.0f;
}

u8 TileRing::ring_of(TileCoord tile) const noexcept {
  const u64 key = tile_key(tile);
  const u64* at = std::lower_bound(active_keys_.begin(), active_keys_.end(), key);
  if (at == active_keys_.end() || *at != key) return k_inactive;
  return active_rings_[static_cast<u32>(at - active_keys_.begin())];
}

f32 TileRing::score_of(TileCoord tile) const noexcept {
  return sim::TierAssignment::score(tile_center(tile, params_.tile_size), 1.0f, ground_,
                                    tiers_params_);
}

void TileRing::add_candidates(WorldPos observer, f32 weight) {
  // An observer of weight w reaches w times as far (the score divides the distance by it).
  const f32 reach = reach_tiles_ * (weight > 0.0f ? weight : 0.0f);
  if (!(reach > 0.0f)) return;
  // The observer's place in the grid in f64, in tiles (ADR-0053): never a float32 metre, which
  // 10,000 km out steps by a metre and moved the window's edge by it. Each centre's offset from
  // the observer is a few tiles, which a float holds to a part in 2^24.
  const f64 size = static_cast<f64>(params_.tile_size);
  const f64 ox = observer.x / size;
  const f64 oz = observer.z / size;
  const f64 r = static_cast<f64>(reach);
  // The window is all i32s: `update` let no observer through whose window is not (`reaches`). The
  // counters are i64 so a window ending at the last i32 tile does not wrap.
  const i64 x0 = static_cast<i64>(std::floor(ox - r));
  const i64 x1 = static_cast<i64>(std::floor(ox + r));
  const i64 z0 = static_cast<i64>(std::floor(oz - r));
  const i64 z1 = static_cast<i64>(std::floor(oz + r));
  const f32 reach2 = reach * reach;
  // x, then z: the window comes out in tile order, so each observer's run is already sorted.
  for (i64 x = x0; x <= x1; ++x) {
    const f32 dx = static_cast<f32>(static_cast<f64>(x) + 0.5 - ox);
    for (i64 z = z0; z <= z1; ++z) {
      const f32 dz = static_cast<f32>(static_cast<f64>(z) + 0.5 - oz);
      if (dx * dx + dz * dz <= reach2)
        candidates_.push_back(tile_key(TileCoord{static_cast<i32>(x), static_cast<i32>(z)}));
    }
  }
}

bool TileRing::reaches(WorldPos observer, f32 weight) const noexcept {
  // The reach exactly as `add_candidates` forms it, so the window checked is the window walked.
  const f32 reach = reach_tiles_ * (weight > 0.0f ? weight : 0.0f);
  return tile_reachable(observer, params_.tile_size, static_cast<f64>(reach));
}

u32 TileRing::update(const sim::ObserverSet& observers, Vector<TileEvent>& events, bool unlimited) {
  stats_ = RingStats{};
  const u8 inactive_tier = static_cast<u8>(params_.ring_count);

  // The observers on the ground plane: a tile is a column, so height says nothing about which
  // tiles are near (03 §3.7).
  ground_.clear();
  candidates_.clear();
  for (u32 o = 0; o < observers.size(); ++o) {
    const WorldPos p = observers.position(o);
    // Past the world's last tile there is no tile to name (`tile_reachable`): the observer
    // observes nothing — no candidates and no score — and is counted.
    if (!reaches(p, observers.weight(o))) {
      ++stats_.unreached;
      continue;
    }
    ground_.add(WorldPos{p.x, 0.0, p.z}, observers.weight(o));
    add_candidates(p, observers.weight(o));
  }
  std::sort(candidates_.begin(), candidates_.end());
  candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());

  // The tracked set: what is within reach, and what is still active wherever it is. Both sorted,
  // so the merge is tile order and every index below is a tile's place in it.
  tracked_.clear();
  tier_.clear();
  tracked_.reserve(candidates_.size() + active_keys_.size());
  u32 a = 0;
  u32 c = 0;
  while (a < active_keys_.size() || c < candidates_.size()) {
    const bool take_active =
        c == candidates_.size() || (a < active_keys_.size() && active_keys_[a] <= candidates_[c]);
    if (take_active) {
      if (c < candidates_.size() && candidates_[c] == active_keys_[a]) ++c;
      tracked_.push_back(active_keys_[a]);
      tier_.push_back(active_rings_[a]);
      ++a;
    } else {
      tracked_.push_back(candidates_[c]);
      tier_.push_back(inactive_tier);
      ++c;
    }
  }
  const u32 n = tracked_.size();
  centers_.resize(n);
  importance_.assign(n, 1.0f);
  for (u32 i = 0; i < n; ++i)
    centers_[i] = tile_center(tile_of_key(tracked_[i]), params_.tile_size);
  stats_.tracked = n;

  sim::TierParams params = tiers_params_;
  if (unlimited) {
    params.max_promotions = n;
    params.max_demotions = n;
  } else {
    if (params.max_promotions == 0) params.max_promotions = n;
    if (params.max_demotions == 0) params.max_demotions = n;
  }
  changes_.clear();
  sim::TierInput input;
  input.positions = std::span<const WorldPos>(centers_.data(), n);
  input.importance = std::span<const f32>(importance_.data(), n);
  input.tiers = std::span<u8>(tier_.data(), n);
  const sim::TierStats tier_stats = tiers_.assign_tiers(input, ground_, params, changes_);
  stats_.deferred_promotions = tier_stats.deferred_promotions;
  stats_.deferred_demotions = tier_stats.deferred_demotions;

  // The changes are in index order, which is tile order.
  u32 emitted = 0;
  for (const sim::TierChange& change : changes_) {
    TileEvent event;
    event.tile = tile_of_key(tracked_[change.index]);
    if (change.from == inactive_tier) {
      event.kind = TileEventKind::Activate;
      event.to = change.to;
      ++stats_.activated;
    } else if (change.to == inactive_tier) {
      event.kind = TileEventKind::Deactivate;
      event.from = change.from;
      ++stats_.deactivated;
    } else {
      event.kind = TileEventKind::ChangeRing;
      event.from = change.from;
      event.to = change.to;
      ++stats_.changed;
    }
    events.push_back(event);
    ++emitted;
  }

  // Keep what is active; an inactive tile is re-enumerated when it comes within reach again.
  active_keys_.clear();
  active_rings_.clear();
  for (u32 i = 0; i < n; ++i) {
    if (tier_[i] == inactive_tier) continue;
    active_keys_.push_back(tracked_[i]);
    active_rings_.push_back(tier_[i]);
    ++stats_.per_ring[tier_[i]];
  }
  stats_.active = active_keys_.size();
  return emitted;
}

bool TileRing::restore(std::span<const u64> keys, std::span<const u8> rings,
                       const sim::ObserverSet& observers, Vector<TileEvent>& events,
                       const char** error) {
  const char* why = nullptr;
  if (!active_keys_.empty()) {
    why = "the ring already holds tiles";
  } else if (keys.size() != rings.size()) {
    why = "every tile needs its ring";
  } else {
    for (usize i = 0; i < keys.size() && why == nullptr; ++i) {
      if (i > 0 && !(keys[i - 1] < keys[i])) why = "the tiles are not in tile order, once each";
      if (rings[i] >= params_.ring_count) why = "a tile is in a ring this ring does not have";
    }
  }
  if (error != nullptr) *error = why;
  if (why != nullptr) return false;
  stats_ = RingStats{};
  ground_.clear();
  for (u32 o = 0; o < observers.size(); ++o) {
    const WorldPos p = observers.position(o);
    ground_.add(WorldPos{p.x, 0.0, p.z}, observers.weight(o));
  }
  for (usize i = 0; i < keys.size(); ++i) {
    active_keys_.push_back(keys[i]);
    active_rings_.push_back(rings[i]);
    TileEvent event;
    event.tile = tile_of_key(keys[i]);
    event.kind = TileEventKind::Activate;
    event.to = rings[i];
    events.push_back(event);
    ++stats_.per_ring[rings[i]];
  }
  stats_.activated = active_keys_.size();
  stats_.active = active_keys_.size();
  stats_.tracked = active_keys_.size();
  return true;
}

u32 TileRing::clear(Vector<TileEvent>& events) {
  stats_ = RingStats{};
  for (u32 i = 0; i < active_keys_.size(); ++i) {
    TileEvent event;
    event.tile = tile_of_key(active_keys_[i]);
    event.kind = TileEventKind::Deactivate;
    event.from = active_rings_[i];
    events.push_back(event);
  }
  stats_.deactivated = active_keys_.size();
  active_keys_.clear();
  active_rings_.clear();
  return stats_.deactivated;
}

}  // namespace engine::world
