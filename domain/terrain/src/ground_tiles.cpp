// The dunes' tiles in a world (ground_tiles.h). The world's `TerrainTiles` as it was under
// `ENGINE_WORLD_TERRAIN`, with the store's calls through the world's `scene_gen::TileRecords`
// instead: the same overlays, lags, fields and records, byte for byte.
#include <core/time/time.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/ground_tiles.h>
#include <domain/terrain/terrain.h>

#include <algorithm>
#include <string>

namespace engine::terrain {

struct DuneTiles::Held {
  TileCoord tile;
  u8 ring = 0;
  Overlay overlay;
  TileOutput output;
};

DuneTiles::DuneTiles(const DuneField& field, const scene_gen::TileRecords& records, i64 tile_mm,
                     const OverlayRules& rules)
    : field_(&field),
      records_(records),
      tile_mm_(tile_mm),
      rules_(rules),
      lag_(tile_mm, rules.feedback) {}

DuneTiles::~DuneTiles() = default;

void DuneTiles::set_cells(u32 ring, u32 cells) noexcept {
  if (ring < k_tile_rings) cells_[ring] = cells;
}

u32 DuneTiles::cells(u32 ring) const noexcept { return cells_[std::min(ring, k_tile_rings - 1)]; }

void DuneTiles::set_drifts(DriftsFn fn, void* context) noexcept {
  drifts_fn_ = fn;
  drifts_context_ = context;
}

void DuneTiles::set_time(i64 time_us) noexcept { time_us_ = std::max(time_us_, time_us); }

DuneTiles::Held* DuneTiles::find(TileCoord tile) const noexcept {
  const u32* slot = index_.find_value(tile_key(tile));
  return slot != nullptr ? slots_[*slot].get() : nullptr;
}

bool DuneTiles::holds(TileCoord tile) const noexcept { return find(tile) != nullptr; }

const TileOutput* DuneTiles::tile(TileCoord tile) const noexcept {
  const Held* held = find(tile);
  return held != nullptr ? &held->output : nullptr;
}

const Overlay* DuneTiles::overlay(TileCoord tile) const noexcept {
  const Held* held = find(tile);
  return held != nullptr ? &held->overlay : nullptr;
}

TileLag DuneTiles::stored_lag(TileCoord tile) {
  if (const Held* held = find(tile); held != nullptr) return held->overlay.lag();
  if (records_.load == nullptr) return TileLag{};
  Vector<u8> blob;
  if (!records_.load(records_.context, scene_gen::TileCoord{tile.x, tile.z}, blob))
    return TileLag{};
  Overlay overlay;
  if (!overlay.read(std::span<const u8>(blob.data(), blob.size()), tile, tile_mm_, rules_))
    return TileLag{};
  return overlay.lag();
}

TileLag DuneTiles::total_lag(TileCoord tile) {
  TileLag lag = stored_lag(tile);
  if (drifts_fn_ != nullptr) {
    drifts_.clear();
    drifts_fn_(drifts_context_, tile, drifts_);
    const u32 derived =
        derived_lag_units(std::span<const DriftDecl>(drifts_.data(), drifts_.size()),
                          day_of(time_us_), rules_.feedback);
    for (u8& u : lag.units) {
      u = static_cast<u8>(
          std::min<u32>(u + derived, static_cast<u32>(rules_.feedback.lag_max_units)));
    }
  }
  return lag;
}

void DuneTiles::refresh_lags(TileCoord centre) {
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dx = -1; dx <= 1; ++dx) {
      const TileCoord t{centre.x + dx, centre.z + dz};
      lag_.set(t, total_lag(t));
    }
  }
}

void DuneTiles::load(Held& held) {
  held.overlay.reset(held.tile, tile_mm_, time_us_);
  if (records_.load != nullptr) {
    const i64 start = time::monotonic_ns();
    Vector<u8> blob;
    if (records_.load(records_.context, scene_gen::TileCoord{held.tile.x, held.tile.z}, blob)) {
      std::string error;
      if (held.overlay.read(std::span<const u8>(blob.data(), blob.size()), held.tile, tile_mm_,
                            rules_, &error)) {
        ++stats_.loaded;
      } else {
        // A record another build's rules wrote, or a damaged one: the tile starts clean rather than
        // reading it as something it is not; the world says so, since it logs.
        ++stats_.refused;
        held.overlay.reset(held.tile, tile_mm_, time_us_);
      }
    }
    stats_.store_ns += time::monotonic_ns() - start;
  }
  held.overlay.set_fill_rates(*field_);
  // Caught up in one step: decay is a closed form, so a month away is one advance.
  held.overlay.advance(time_us_, field_->wind(), rules_);
}

void DuneTiles::build(Held& held, u8 ring) {
  const i64 start = time::monotonic_ns();
  held.ring = ring;
  TileOptions options;
  options.tile_mm = tile_mm_;
  options.cells = cells(ring);
  // The deformed surface: the overlays of the 3 x 3 tiles held, and the drifts that can reach the
  // tile.
  Deformation deformation;
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dx = -1; dx <= 1; ++dx) {
      const Held* n = find(TileCoord{held.tile.x + dx, held.tile.z + dz});
      deformation.overlays[dz + 1][dx + 1] = n != nullptr ? &n->overlay : nullptr;
    }
  }
  drifts_.clear();
  if (drifts_fn_ != nullptr) {
    for (i32 dz = -1; dz <= 1; ++dz) {
      for (i32 dx = -1; dx <= 1; ++dx)
        drifts_fn_(drifts_context_, TileCoord{held.tile.x + dx, held.tile.z + dz}, drifts_);
    }
  }
  deformation.drifts = std::span<const DriftDecl>(drifts_.data(), drifts_.size());
  evaluate_tile(*field_, held.tile, time_us_, options, &lag_, &deformation, held.output);
  ++stats_.built;
  const i64 ns = time::monotonic_ns() - start;
  stats_.build_ns += ns;
  stats_.max_build_ns = std::max(stats_.max_build_ns, ns);
  built_.push_back(held.tile);
}

void DuneTiles::write(Held& held) {
  held.overlay.advance(time_us_, field_->wind(), rules_);
  if (records_.save == nullptr) return;
  const i64 start = time::monotonic_ns();
  held.overlay.write(scratch_, rules_);
  const scene_gen::TileCoord at{held.tile.x, held.tile.z};
  if (scratch_.empty()) {
    // Buried: the tile stores nothing. Erasing a record that is not there is not an error.
    if (records_.erase != nullptr) records_.erase(records_.context, at);
    ++stats_.erased;
  } else if (records_.save(records_.context, at,
                           std::span<const u8>(scratch_.data(), scratch_.size()))) {
    ++stats_.written;
    stats_.largest_record = std::max<u64>(stats_.largest_record, scratch_.size());
  }
  stats_.store_ns += time::monotonic_ns() - start;
}

bool DuneTiles::activate(TileCoord tile, u8 ring) {
  if (find(tile) != nullptr) return true;
  auto held = std::make_unique<Held>();
  held->tile = tile;
  Held& h = *held;
  index_[tile_key(tile)] = slots_.size();
  slots_.push_back(std::move(held));
  load(h);
  refresh_lags(tile);
  build(h, ring);
  stats_.held = slots_.size();
  return true;
}

bool DuneTiles::change_ring(TileCoord tile, u8 ring) {
  Held* held = find(tile);
  if (held == nullptr) return activate(tile, ring);
  const u32 before = cells(held->ring);
  const u32 after = cells(ring);
  held->ring = ring;
  if (before != after) build(*held, ring);
  return true;
}

void DuneTiles::deactivate(TileCoord tile) {
  const u64 key = tile_key(tile);
  const u32* slot = index_.find_value(key);
  if (slot == nullptr) return;
  const u32 at = *slot;
  write(*slots_[at]);
  // Swap-remove: the last slot moves into the freed one and its index follows it.
  const u32 last = slots_.size() - 1;
  if (at != last) {
    slots_[at] = std::move(slots_[last]);
    index_[tile_key(slots_[at]->tile)] = at;
  }
  slots_.pop_back();
  index_.erase(key);
  ++stats_.dropped;
  stats_.held = slots_.size();
}

void DuneTiles::take_built(Vector<TileCoord>& out) {
  out.clear();
  for (const TileCoord& t : built_)
    out.push_back(t);
  built_.clear();
}

bool DuneTiles::deform(const Stamp& stamp) {
  if (stamp.time_us < time_us_) return false;
  const i64 reach = (std::max<i64>(stamp.radius_mm, stamp.radius2_mm) * 8) / 5 + 1;
  const i64 x0 = fx::floor_div(stamp.x_mm - reach, tile_mm_);
  const i64 x1 = fx::floor_div(stamp.x_mm + reach, tile_mm_);
  const i64 z0 = fx::floor_div(stamp.z_mm - reach, tile_mm_);
  const i64 z1 = fx::floor_div(stamp.z_mm + reach, tile_mm_);
  bool taken = false;
  for (i64 z = z0; z <= z1; ++z) {
    for (i64 x = x0; x <= x1; ++x) {
      Held* held = find(TileCoord{static_cast<i32>(x), static_cast<i32>(z)});
      if (held == nullptr) continue;
      if (held->overlay.push(stamp, field_->wind(), rules_)) {
        taken = true;
        ++stats_.stamps;
      }
    }
  }
  return taken;
}

void DuneTiles::advance(i64 time_us) {
  set_time(time_us);
  Vector<TileCoord> changed;
  for (const std::unique_ptr<Held>& held : slots_) {
    held->overlay.advance(time_us_, field_->wind(), rules_);
    const TileLag now = total_lag(held->tile);
    if (!(lag_.get(held->tile) == now)) changed.push_back(held->tile);
  }
  // A lag that moved changes the field under its own tile and its eight neighbours: rebuild the
  // held ones among them, so every shared edge still agrees.
  if (changed.empty()) return;
  for (const TileCoord& t : changed)
    refresh_lags(t);
  for (const std::unique_ptr<Held>& held : slots_) {
    for (const TileCoord& t : changed) {
      if (std::abs(held->tile.x - t.x) <= 1 && std::abs(held->tile.z - t.z) <= 1) {
        build(*held, held->ring);
        break;
      }
    }
  }
}

// ---- the world's handle -------------------------------------------------------------------------

namespace {

TileCoord from(scene_gen::TileCoord t) noexcept { return TileCoord{t.x, t.z}; }

void tiles_destroy(void* tiles) noexcept { delete static_cast<DuneTiles*>(tiles); }
bool tiles_activate(void* tiles, scene_gen::TileCoord tile, u8 ring) {
  return static_cast<DuneTiles*>(tiles)->activate(from(tile), ring);
}
bool tiles_change_ring(void* tiles, scene_gen::TileCoord tile, u8 ring) {
  return static_cast<DuneTiles*>(tiles)->change_ring(from(tile), ring);
}
void tiles_deactivate(void* tiles, scene_gen::TileCoord tile) {
  static_cast<DuneTiles*>(tiles)->deactivate(from(tile));
}
void tiles_take_built(void* tiles, Vector<scene_gen::TileCoord>& out) {
  Vector<TileCoord> built;
  static_cast<DuneTiles*>(tiles)->take_built(built);
  out.clear();
  for (const TileCoord& t : built)
    out.push_back(scene_gen::TileCoord{t.x, t.z});
}
void tiles_set_time(void* tiles, i64 time_us) { static_cast<DuneTiles*>(tiles)->set_time(time_us); }

constexpr scene_gen::GroundTilesOps k_dune_tiles_ops{
    .destroy = &tiles_destroy,
    .activate = &tiles_activate,
    .change_ring = &tiles_change_ring,
    .deactivate = &tiles_deactivate,
    .take_built = &tiles_take_built,
    .set_time = &tiles_set_time,
};

}  // namespace

scene_gen::GroundTiles make_dune_tiles(const DuneField& field,
                                       const scene_gen::TileRecords& records, i64 tile_mm) {
  return scene_gen::GroundTiles(
      &k_dune_tiles_ops, new DuneTiles(field, records, tile_mm, overlay_rules_from_tunables()));
}

DuneTiles* dune_tiles(const scene_gen::GroundTiles& tiles) noexcept {
  if (tiles.ops() != &k_dune_tiles_ops) return nullptr;
  return static_cast<DuneTiles*>(tiles.state());
}

}  // namespace engine::terrain
