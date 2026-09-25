#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/schema/materialize.h>
#include <systems/world/tile_store.h>

#include <schemas/world_tiles.h>
#include <string>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_store, "world.store");

u32 projection_kind() noexcept {
  static const u32 kind = schema::stable_type_id(schema::type_of<TileProjection>().qualified_name);
  return kind;
}

struct LoadContext {
  const WorldStore* store = nullptr;
  u64 tile = 0;
  Vector<sim::EntityRecord>* out = nullptr;
  u32 undecodable = 0;
};

struct ReadContext {
  Vector<TileRow>* rows = nullptr;
  u32 undecodable = 0;
};

bool decode(std::span<const u8> blob, TileProjection& out) {
  JsonValue json;
  const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
  if (!parse_json(text, json).ok) return false;
  schema::ReadContext ctx;
  return schema::from_json(out, json, ctx) && ctx.ok();
}

}  // namespace

// ---- WorldStore ---------------------------------------------------------------------------------

store::Status WorldStore::open(std::string_view native_path, bool create) {
  store::OpenOptions options;
  options.create = create;
  store::Status status = db_.open(native_path, options);
  if (status == store::Status::Ok) status = log_.open();
  if (status != store::Status::Ok) db_.close();
  return status;
}

store::Status WorldStore::open_memory() {
  store::Status status = db_.open_memory();
  if (status == store::Status::Ok) status = log_.open();
  if (status != store::Status::Ok) db_.close();
  return status;
}

void WorldStore::close() noexcept { db_.close(); }

u64 WorldStore::tile_seed(u64 tile) const noexcept { return hash_combine(world_seed_, tile); }

sim::TileStore WorldStore::tile_store() noexcept {
  sim::TileStore out;
  out.context = this;
  out.tile_state = &WorldStore::tile_state;
  out.load_records = &WorldStore::load_records;
  return out;
}

bool WorldStore::tile_state(void* context, u64 tile, sim::TileState& out) {
  auto* self = static_cast<WorldStore*>(context);
  out.seed = self->tile_seed(tile);
  store::SnapshotInfo info;
  if (self->log_.load_snapshot(tile, info, nullptr, nullptr) != store::Status::Ok) return false;
  out.last_active = GameTime{info.game_time_us};
  return true;
}

void WorldStore::load_records(void* context, u64 tile, Vector<sim::EntityRecord>& out) {
  auto* self = static_cast<WorldStore*>(context);
  LoadContext load{self, tile, &out, 0};
  const store::Status status = self->log_.projections_by_tile(
      tile,
      [](const store::ProjectionRecord& record, void* user) {
        auto* ctx = static_cast<LoadContext*>(user);
        if (record.kind != projection_kind()) return;
        TileProjection projection;
        if (!decode(record.blob, projection)) {
          ++ctx->undecodable;
          return;
        }
        sim::EntityRecord entity;
        entity.entity = projection.record;
        entity.seed = hash_combine(ctx->store->tile_seed(ctx->tile),
                                   hash_combine(projection.record.hi, projection.record.lo));
        entity.position = projection.position;
        entity.importance = projection.importance;
        entity.kind = schema::stable_type_id(projection.type);
        ctx->out->push_back(entity);
      },
      &load);
  if (status != store::Status::Ok || load.undecodable != 0) {
    ENGINE_LOG_WARN(log_world_store, "a tile's projections were not all read",
                    log::field("tile", tile), log::field("status", store::status_name(status)),
                    log::field("undecodable", load.undecodable));
  }
}

store::Status WorldStore::write_tile(TileCoord tile, std::span<const TileRow> rows, u64 sim_tick,
                                     i64 game_time_us) {
  const u64 id = store_tile(tile);
  // The blobs first, into one buffer that has stopped growing before any span points into it.
  Vector<u32> offsets;
  offsets.reserve(static_cast<u32>(rows.size()) + 1);
  scratch_.clear();
  for (const TileRow& row : rows) {
    TileProjection projection;
    projection.record = row.record;
    projection.type = row.type;
    projection.position = row.position;
    projection.importance = row.importance;
    const std::string text = write_json(schema::to_json(projection));
    offsets.push_back(scratch_.size());
    for (const char c : text)
      scratch_.push_back(static_cast<u8>(c));
  }
  offsets.push_back(scratch_.size());
  Vector<store::ProjectionRecord> records;
  records.reserve(static_cast<u32>(rows.size()));
  for (u32 i = 0; i < rows.size(); ++i) {
    store::ProjectionRecord record;
    record.entity = rows[i].record;
    record.tile = id;
    record.kind = projection_kind();
    record.blob = std::span<const u8>(scratch_.data() + offsets[i], offsets[i + 1] - offsets[i]);
    records.push_back(record);
  }
  if (!records.empty()) {
    const store::Status status = log_.upsert_projections(
        std::span<const store::ProjectionRecord>(records.data(), records.size()));
    if (status != store::Status::Ok) return status;
  }
  store::SnapshotInfo info;
  return log_.snapshot(id, sim_tick, game_time_us, info);
}

store::Status WorldStore::read_tile(TileCoord tile, Vector<TileRow>& rows, bool& has_snapshot,
                                    store::SnapshotInfo& info) {
  rows.clear();
  const u64 id = store_tile(tile);
  const store::Status snapshot = log_.load_snapshot(id, info, nullptr, nullptr);
  has_snapshot = snapshot == store::Status::Ok;
  if (snapshot != store::Status::Ok && snapshot != store::Status::NotFound) return snapshot;
  ReadContext read{&rows, 0};
  return log_.projections_by_tile(
      id,
      [](const store::ProjectionRecord& record, void* user) {
        auto* ctx = static_cast<ReadContext*>(user);
        if (record.kind != projection_kind()) return;
        TileProjection projection;
        if (!decode(record.blob, projection)) {
          ++ctx->undecodable;
          return;
        }
        TileRow row;
        row.record = projection.record;
        row.type = std::move(projection.type);
        row.position = projection.position;
        row.importance = projection.importance;
        ctx->rows->push_back(std::move(row));
      },
      &read);
}

}  // namespace engine::world
