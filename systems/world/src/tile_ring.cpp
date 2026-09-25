#include <systems/world/tile_ring.h>

#include <algorithm>
#include <cmath>

namespace engine::world {

TileCoord tile_at(Vec3 position, f32 tile_size) noexcept {
  const f64 size = static_cast<f64>(tile_size);
  return TileCoord{static_cast<i32>(std::floor(static_cast<f64>(position.x) / size)),
                   static_cast<i32>(std::floor(static_cast<f64>(position.z) / size))};
}

Vec3 tile_center(TileCoord tile, f32 tile_size) noexcept {
  return Vec3{(static_cast<f32>(tile.x) + 0.5f) * tile_size, 0.0f,
              (static_cast<f32>(tile.z) + 0.5f) * tile_size};
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

void TileRing::add_candidates(Vec3 observer, f32 weight) {
  // An observer of weight w reaches w times as far (the score divides the distance by it).
  const f32 reach = reach_tiles_ * (weight > 0.0f ? weight : 0.0f);
  if (!(reach > 0.0f)) return;
  const f32 size = params_.tile_size;
  const f32 ox = observer.x / size;
  const f32 oz = observer.z / size;
  const i32 x0 = static_cast<i32>(std::floor(ox - reach));
  const i32 x1 = static_cast<i32>(std::floor(ox + reach));
  const i32 z0 = static_cast<i32>(std::floor(oz - reach));
  const i32 z1 = static_cast<i32>(std::floor(oz + reach));
  const f32 reach2 = reach * reach;
  // x, then z: the window comes out in tile order, so each observer's run is already sorted.
  for (i32 x = x0; x <= x1; ++x) {
    const f32 dx = static_cast<f32>(x) + 0.5f - ox;
    for (i32 z = z0; z <= z1; ++z) {
      const f32 dz = static_cast<f32>(z) + 0.5f - oz;
      if (dx * dx + dz * dz <= reach2) candidates_.push_back(tile_key(TileCoord{x, z}));
    }
  }
}

u32 TileRing::update(const sim::ObserverSet& observers, Vector<TileEvent>& events, bool unlimited) {
  stats_ = RingStats{};
  const u8 inactive_tier = static_cast<u8>(params_.ring_count);

  // The observers on the ground plane: a tile is a column, so height says nothing about which
  // tiles are near (03 §3.7).
  ground_.clear();
  candidates_.clear();
  for (u32 o = 0; o < observers.size(); ++o) {
    const Vec3 p = observers.position(o);
    ground_.add(Vec3{p.x, 0.0f, p.z}, observers.weight(o));
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
  input.positions = std::span<const Vec3>(centers_.data(), n);
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
