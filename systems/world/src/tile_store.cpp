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

// A projection written before its record's type was renamed names the type by the name it had then
// (`@alias`): read as the current name, so an entity's kind and a row's type are the ones the rest
// of the build compares against (schemas/README.md, "Renaming a field or a type").
std::string_view current_type_name(std::string_view stored) {
  const schema::TypeInfo* info = schema::Registry::global().resolve(stored);
  return info != nullptr ? std::string_view(info->qualified_name) : stored;
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
        entity.kind = schema::stable_type_id(current_type_name(projection.type));
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
        row.type = std::string(current_type_name(projection.type));
        row.position = projection.position;
        row.importance = projection.importance;
        ctx->rows->push_back(std::move(row));
      },
      &read);
}

bool read_event_payload(const store::EventRecord& record, EventPayload& out, std::string& error) {
  out = EventPayload{};
  const schema::TypeInfo* type = schema::find_by_stable_id(record.type);
  if (type == nullptr || type->kind != schema::Kind::Struct) {
    error = "event type " + std::to_string(record.type) + " is no struct this build has";
    return false;
  }
  const std::string_view text(reinterpret_cast<const char*>(record.payload.data()),
                              record.payload.size());
  JsonValue value;
  if (!parse_json(text, value).ok || !value.is_object()) {
    error = std::string("the payload of a ") + type->qualified_name + " event is not a JSON object";
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::upcast(*type, value, record.payload_version, ctx)) {
    error = std::string("a ") + type->qualified_name + " event written at version " +
            std::to_string(record.payload_version) + " does not read as version " +
            std::to_string(type->version) +
            (ctx.diagnostics.empty() ? std::string() : ": " + ctx.diagnostics[0].message);
    return false;
  }
  // Members under a field's former name (`@alias`) take its name, after the version steps, which
  // see the payload as it was written: the value handed on is in this build's names, as the type
  // found by a former name is this build's type (schemas/README.md, "Renaming a field or a type").
  u32 renamed = 0;
  if (!schema::rename_former_fields(*type, value.as_object(), ctx, renamed)) {
    error = std::string("a ") + type->qualified_name + " event " +
            (ctx.diagnostics.empty() ? std::string("names a field twice")
                                     : ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message);
    return false;
  }
  out.type = type;
  out.written_version = record.payload_version;
  out.value = std::move(value);
  return true;
}

}  // namespace engine::world
