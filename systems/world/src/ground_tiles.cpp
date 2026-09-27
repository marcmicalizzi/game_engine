// The ground consumer (ground_tiles.h). The store's side of the world's `TerrainTiles` as it was
// under `ENGINE_WORLD_TERRAIN` — a tile's record one projection under the tile, keyed by an entity
// that names the record type and the tile — with the ground's own half behind the provider's tiles.
#include <core/hash/hash.h>
#include <core/log/log.h>
#include <core/schema/materialize.h>
#include <systems/world/ground_tiles.h>

#include <string>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_ground, "world.ground");

scene_gen::TileCoord to_gen(TileCoord t) noexcept { return scene_gen::TileCoord{t.x, t.z}; }

}  // namespace

GroundTiles::~GroundTiles() = default;

Id128 GroundTiles::record_entity(std::string_view record, TileCoord tile) noexcept {
  // A fixed high half naming the record type, and the tile: one row per tile, never a record's id.
  return Id128{hash_bytes(record.data(), record.size()), store_tile(tile)};
}

u32 GroundTiles::record_kind(std::string_view record) noexcept {
  return schema::stable_type_id(record);
}

bool GroundTiles::create(const scene_gen::GroundProvider& ground, const GroundTilesConfig& config,
                         std::string* error) {
  tiles_.reset();
  store_ = config.store;
  record_ = ground.record() != nullptr ? ground.record() : "";
  record_hi_ = record_entity(record_, TileCoord{});
  kind_ = record_kind(record_);
  const i64 tile_mm = static_cast<i64>(static_cast<f64>(config.tile_size) * 1000.0 + 0.5);
  // Records only where there is a store to keep them in and a type to file them under.
  scene_gen::TileRecords records;
  if (store_ != nullptr && !record_.empty()) {
    records.context = this;
    records.load = &GroundTiles::load;
    records.save = &GroundTiles::save;
    records.erase = &GroundTiles::erase;
  }
  return ground.open_tiles(records, tile_mm, tiles_, error);
}

TileConsumer GroundTiles::consumer(u8 rings) noexcept {
  TileConsumer c;
  c.name = "ground";
  c.context = this;
  c.rings = rings;
  c.activate = &GroundTiles::on_activate;
  c.change_ring = &GroundTiles::on_change;
  c.deactivate = &GroundTiles::on_deactivate;
  c.commit = &GroundTiles::on_commit;
  return c;
}

void GroundTiles::set_time(i64 time_us) {
  if (tiles_.valid()) tiles_.set_time(time_us);
}

bool GroundTiles::on_activate(void* context, const TileEvent& event) {
  auto* self = static_cast<GroundTiles*>(context);
  return self->tiles_.valid() && self->tiles_.activate(to_gen(event.tile), event.to);
}

bool GroundTiles::on_change(void* context, const TileEvent& event) {
  auto* self = static_cast<GroundTiles*>(context);
  return self->tiles_.valid() && self->tiles_.change_ring(to_gen(event.tile), event.to);
}

void GroundTiles::on_deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<GroundTiles*>(context);
  if (self->tiles_.valid()) self->tiles_.deactivate(to_gen(event.tile));
}

void GroundTiles::on_commit(void* context) {
  auto* self = static_cast<GroundTiles*>(context);
  if (!self->tiles_.valid()) return;
  self->tiles_.take_built(self->built_);
  if (self->built_.empty() || self->sink_ == nullptr) return;
  self->handed_.clear();
  for (const scene_gen::TileCoord& t : self->built_)
    self->handed_.push_back(TileCoord{t.x, t.z});
  self->sink_(self->sink_context_,
              std::span<const TileCoord>(self->handed_.data(), self->handed_.size()));
}

bool GroundTiles::load(void* context, scene_gen::TileCoord tile, Vector<u8>& out) {
  auto* self = static_cast<GroundTiles*>(context);
  if (self->store_ == nullptr || !self->store_->is_open()) return false;
  const TileCoord t{tile.x, tile.z};
  store::ProjectionRecord record;
  if (self->store_->log().load_projection(Id128{self->record_hi_.hi, store_tile(t)}, self->kind_,
                                          record) != store::Status::Ok) {
    return false;
  }
  out.assign(record.blob.begin(), record.blob.end());
  return true;
}

bool GroundTiles::save(void* context, scene_gen::TileCoord tile, std::span<const u8> bytes) {
  auto* self = static_cast<GroundTiles*>(context);
  if (self->store_ == nullptr || !self->store_->is_open()) return false;
  const TileCoord t{tile.x, tile.z};
  store::ProjectionRecord record;
  record.entity = Id128{self->record_hi_.hi, store_tile(t)};
  record.tile = store_tile(t);
  record.kind = self->kind_;
  record.blob = bytes;
  const store::Status status = self->store_->log().upsert_projection(record);
  if (status != store::Status::Ok) {
    ENGINE_LOG_WARN(log_world_ground, "a tile's ground record was not written",
                    log::field("record", self->record_), log::field("x", t.x), log::field("z", t.z),
                    log::field("status", store::status_name(status)));
    return false;
  }
  return true;
}

void GroundTiles::erase(void* context, scene_gen::TileCoord tile) {
  auto* self = static_cast<GroundTiles*>(context);
  if (self->store_ == nullptr || !self->store_->is_open()) return;
  // Erasing a row that is not there is not an error.
  (void)self->store_->log().erase_projection(
      Id128{self->record_hi_.hi, store_tile(TileCoord{tile.x, tile.z})}, self->kind_);
}

}  // namespace engine::world
