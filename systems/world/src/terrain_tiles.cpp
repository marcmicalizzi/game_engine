#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/schema/materialize.h>
#include <core/time/time.h>
#include <domain/terrain/fixed.h>
#include <systems/world/terrain_tiles.h>

#include <algorithm>
#include <string>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_terrain, "world.terrain");

terrain::TileCoord to_terrain(TileCoord t) noexcept { return terrain::TileCoord{t.x, t.z}; }

}  // namespace

struct TerrainTiles::Held {
  TileCoord tile;
  u8 ring = 0;
  terrain::Overlay overlay;
  terrain::TileOutput output;
};

TerrainTiles::TerrainTiles(const TerrainTilesConfig& config)
    : config_(config), lag_(config.tile_mm, config.rules.feedback) {}

TerrainTiles::~TerrainTiles() = default;

TileConsumer TerrainTiles::consumer(u8 rings) noexcept {
  TileConsumer c;
  c.name = "terrain";
  c.context = this;
  c.rings = rings;
  c.activate = &TerrainTiles::on_activate;
  c.change_ring = &TerrainTiles::on_change;
  c.deactivate = &TerrainTiles::on_deactivate;
  c.commit = &TerrainTiles::on_commit;
  return c;
}

void TerrainTiles::set_sink(TerrainSink sink, void* context) noexcept {
  sink_ = sink;
  sink_context_ = context;
}

void TerrainTiles::set_time(i64 time_us) noexcept { time_us_ = std::max(time_us_, time_us); }

Id128 TerrainTiles::overlay_entity(TileCoord tile) noexcept {
  // A fixed high half naming the overlay, and the tile: one row per tile, never a record's id.
  static const u64 hi = hash_bytes("engine.terrain.OverlayTile", 26);
  return Id128{hi, store_tile(tile)};
}

u32 TerrainTiles::overlay_kind() noexcept {
  static const u32 kind = schema::stable_type_id("engine.terrain.OverlayTile");
  return kind;
}

TerrainTiles::Held* TerrainTiles::find(TileCoord tile) const noexcept {
  const u32* slot = index_.find_value(store_tile(tile));
  return slot != nullptr ? slots_[*slot].get() : nullptr;
}

bool TerrainTiles::holds(TileCoord tile) const noexcept { return find(tile) != nullptr; }

const terrain::TileOutput* TerrainTiles::tile(TileCoord tile) const noexcept {
  const Held* held = find(tile);
  return held != nullptr ? &held->output : nullptr;
}

const terrain::Overlay* TerrainTiles::overlay(TileCoord tile) const noexcept {
  const Held* held = find(tile);
  return held != nullptr ? &held->overlay : nullptr;
}

terrain::TileLag TerrainTiles::stored_lag(TileCoord tile) {
  if (const Held* held = find(tile); held != nullptr) return held->overlay.lag();
  if (config_.store == nullptr || !config_.store->is_open()) return terrain::TileLag{};
  store::ProjectionRecord record;
  if (config_.store->log().load_projection(overlay_entity(tile), overlay_kind(), record) !=
      store::Status::Ok) {
    return terrain::TileLag{};
  }
  terrain::Overlay overlay;
  if (!overlay.read(record.blob, to_terrain(tile), config_.tile_mm, config_.rules)) {
    return terrain::TileLag{};
  }
  return overlay.lag();
}

terrain::TileLag TerrainTiles::total_lag(TileCoord tile) {
  terrain::TileLag lag = stored_lag(tile);
  if (config_.drifts != nullptr) {
    drifts_.clear();
    config_.drifts(config_.drifts_context, tile, drifts_);
    const u32 derived = terrain::derived_lag_units(
        std::span<const terrain::DriftDecl>(drifts_.data(), drifts_.size()),
        terrain::day_of(time_us_), config_.rules.feedback);
    for (u8& u : lag.units) {
      u = static_cast<u8>(
          std::min<u32>(u + derived, static_cast<u32>(config_.rules.feedback.lag_max_units)));
    }
  }
  return lag;
}

void TerrainTiles::refresh_lags(TileCoord centre) {
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dx = -1; dx <= 1; ++dx) {
      const TileCoord t{centre.x + dx, centre.z + dz};
      lag_.set(to_terrain(t), total_lag(t));
    }
  }
}

void TerrainTiles::load(Held& held) {
  held.overlay.reset(to_terrain(held.tile), config_.tile_mm, time_us_);
  if (config_.store != nullptr && config_.store->is_open()) {
    const i64 start = time::monotonic_ns();
    store::ProjectionRecord record;
    if (config_.store->log().load_projection(overlay_entity(held.tile), overlay_kind(), record) ==
        store::Status::Ok) {
      std::string error;
      if (held.overlay.read(record.blob, to_terrain(held.tile), config_.tile_mm, config_.rules,
                            &error)) {
        ++stats_.loaded;
      } else {
        // A record another build's rules wrote, or a damaged one: the tile starts clean rather than
        // reading it as something it is not, and says so.
        ++stats_.refused;
        ENGINE_LOG_WARN(log_world_terrain, "a tile's overlay could not be read",
                        log::field("x", held.tile.x), log::field("z", held.tile.z),
                        log::field("why", error));
        held.overlay.reset(to_terrain(held.tile), config_.tile_mm, time_us_);
      }
    }
    stats_.store_ns += time::monotonic_ns() - start;
  }
  held.overlay.set_fill_rates(*config_.field);
  // Caught up in one step: decay is a closed form, so a month away is one advance.
  held.overlay.advance(time_us_, config_.field->wind(), config_.rules);
}

void TerrainTiles::build(Held& held, u8 ring) {
  const i64 start = time::monotonic_ns();
  held.ring = ring;
  terrain::TileOptions options;
  options.tile_mm = config_.tile_mm;
  options.cells = config_.cells[std::min<u32>(ring, k_max_rings - 1)];
  // The deformed surface: the overlays of the 3 x 3 tiles this consumer holds, and the drifts that
  // can reach the tile.
  terrain::Deformation deformation;
  for (i32 dz = -1; dz <= 1; ++dz) {
    for (i32 dx = -1; dx <= 1; ++dx) {
      const Held* n = find(TileCoord{held.tile.x + dx, held.tile.z + dz});
      deformation.overlays[dz + 1][dx + 1] = n != nullptr ? &n->overlay : nullptr;
    }
  }
  drifts_.clear();
  if (config_.drifts != nullptr) {
    for (i32 dz = -1; dz <= 1; ++dz) {
      for (i32 dx = -1; dx <= 1; ++dx)
        config_.drifts(config_.drifts_context, TileCoord{held.tile.x + dx, held.tile.z + dz},
                       drifts_);
    }
  }
  deformation.drifts = std::span<const terrain::DriftDecl>(drifts_.data(), drifts_.size());
  terrain::evaluate_tile(*config_.field, to_terrain(held.tile), time_us_, options, &lag_,
                         &deformation, held.output);
  ++stats_.built;
  const i64 ns = time::monotonic_ns() - start;
  stats_.build_ns += ns;
  stats_.max_build_ns = std::max(stats_.max_build_ns, ns);
  built_.push_back(held.tile);
}

void TerrainTiles::write(Held& held) {
  held.overlay.advance(time_us_, config_.field->wind(), config_.rules);
  if (config_.store == nullptr || !config_.store->is_open()) return;
  const i64 start = time::monotonic_ns();
  held.overlay.write(scratch_, config_.rules);
  if (scratch_.empty()) {
    // Buried: the tile stores nothing. Erasing a row that is not there is not an error.
    (void)config_.store->log().erase_projection(overlay_entity(held.tile), overlay_kind());
    ++stats_.erased;
  } else {
    store::ProjectionRecord record;
    record.entity = overlay_entity(held.tile);
    record.tile = store_tile(held.tile);
    record.kind = overlay_kind();
    record.blob = std::span<const u8>(scratch_.data(), scratch_.size());
    const store::Status status = config_.store->log().upsert_projection(record);
    if (status == store::Status::Ok) {
      ++stats_.written;
      stats_.largest_record = std::max<u64>(stats_.largest_record, scratch_.size());
    } else {
      ENGINE_LOG_WARN(log_world_terrain, "a tile's overlay was not written",
                      log::field("x", held.tile.x), log::field("z", held.tile.z),
                      log::field("status", store::status_name(status)));
    }
  }
  stats_.store_ns += time::monotonic_ns() - start;
}

bool TerrainTiles::on_activate(void* context, const TileEvent& event) {
  auto* self = static_cast<TerrainTiles*>(context);
  if (self->config_.field == nullptr) return false;
  if (self->find(event.tile) != nullptr) return true;
  auto held = std::make_unique<Held>();
  held->tile = event.tile;
  Held& h = *held;
  self->index_[store_tile(event.tile)] = self->slots_.size();
  self->slots_.push_back(std::move(held));
  self->load(h);
  self->refresh_lags(event.tile);
  self->build(h, event.to);
  self->stats_.held = self->slots_.size();
  return true;
}

bool TerrainTiles::on_change(void* context, const TileEvent& event) {
  auto* self = static_cast<TerrainTiles*>(context);
  Held* held = self->find(event.tile);
  if (held == nullptr) return on_activate(context, event);
  const u32 before = self->config_.cells[std::min<u32>(held->ring, k_max_rings - 1)];
  const u32 after = self->config_.cells[std::min<u32>(event.to, k_max_rings - 1)];
  held->ring = event.to;
  if (before != after) self->build(*held, event.to);
  return true;
}

void TerrainTiles::on_deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<TerrainTiles*>(context);
  const u64 key = store_tile(event.tile);
  const u32* slot = self->index_.find_value(key);
  if (slot == nullptr) return;
  const u32 at = *slot;
  self->write(*self->slots_[at]);
  // Swap-remove: the last slot moves into the freed one and its index follows it.
  const u32 last = self->slots_.size() - 1;
  if (at != last) {
    self->slots_[at] = std::move(self->slots_[last]);
    self->index_[store_tile(self->slots_[at]->tile)] = at;
  }
  self->slots_.pop_back();
  self->index_.erase(key);
  ++self->stats_.dropped;
  self->stats_.held = self->slots_.size();
}

void TerrainTiles::on_commit(void* context) {
  auto* self = static_cast<TerrainTiles*>(context);
  if (self->built_.empty()) return;
  if (self->sink_ != nullptr) {
    self->sink_(self->sink_context_,
                std::span<const TileCoord>(self->built_.data(), self->built_.size()));
  }
  self->built_.clear();
}

bool TerrainTiles::deform(const terrain::Stamp& stamp) {
  if (stamp.time_us < time_us_) return false;
  const i64 reach = (std::max<i64>(stamp.radius_mm, stamp.radius2_mm) * 8) / 5 + 1;
  const i64 x0 = terrain::fx::floor_div(stamp.x_mm - reach, config_.tile_mm);
  const i64 x1 = terrain::fx::floor_div(stamp.x_mm + reach, config_.tile_mm);
  const i64 z0 = terrain::fx::floor_div(stamp.z_mm - reach, config_.tile_mm);
  const i64 z1 = terrain::fx::floor_div(stamp.z_mm + reach, config_.tile_mm);
  bool taken = false;
  for (i64 z = z0; z <= z1; ++z) {
    for (i64 x = x0; x <= x1; ++x) {
      Held* held = find(TileCoord{static_cast<i32>(x), static_cast<i32>(z)});
      if (held == nullptr) continue;
      if (held->overlay.push(stamp, config_.field->wind(), config_.rules)) {
        taken = true;
        ++stats_.stamps;
      }
    }
  }
  return taken;
}

void TerrainTiles::advance(i64 time_us) {
  set_time(time_us);
  Vector<TileCoord> changed;
  for (const std::unique_ptr<Held>& held : slots_) {
    held->overlay.advance(time_us_, config_.field->wind(), config_.rules);
    const terrain::TileLag now = total_lag(held->tile);
    if (!(lag_.get(to_terrain(held->tile)) == now)) changed.push_back(held->tile);
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

}  // namespace engine::world
