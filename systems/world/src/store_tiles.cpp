#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/doc/partition.h>
#include <systems/world/store_tiles.h>

#include <string>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_tiles, "world.store_tiles");

bool number(const JsonValue* value, f32& out) {
  f64 v = 0.0;
  if (value == nullptr || !value->get_f64(v)) return false;
  out = static_cast<f32>(v);
  return true;
}

// [x, y] (on the ground, y = 0 here) or [x, y, z]; an object with x, y, z; or a transform with a
// `position` or `translation` member — the shapes `doc::read_position` accepts, all three axes.
bool read_vec3(const JsonValue& value, Vec3& out) {
  if (value.is_array()) {
    if (value.size() == 3) {
      return number(&value[0], out.x) && number(&value[1], out.y) && number(&value[2], out.z);
    }
    if (value.size() == 2) {
      out.y = 0.0f;
      return number(&value[0], out.x) && number(&value[1], out.z);
    }
    return false;
  }
  if (!value.is_object()) return false;
  for (const char* member : {"position", "translation"}) {
    if (const JsonValue* inner = value.find(member)) return read_vec3(*inner, out);
  }
  out.y = 0.0f;
  if (!number(value.find("x"), out.x)) return false;
  number(value.find("y"), out.y);
  return number(value.find("z"), out.z);
}

}  // namespace

// ---- where a record is --------------------------------------------------------------------------

bool record_position(const doc::Document& document, const Id128& id, Vec3& out) {
  // The layer that defines the record decides which property is its position, exactly as the
  // partition's file layout does (`doc::position_property`).
  const doc::ObjectRecord* defining = nullptr;
  u32 layer = 0;
  document.visit_records(id, [&](u32 index, const doc::ObjectRecord& record) {
    if (record.type.empty()) return;
    defining = &record;
    layer = index;
  });
  if (defining == nullptr) return false;
  std::string_view property = doc::position_property(*defining, document.layer(layer).partition());
  if (property.empty()) {
    for (const char* name : {"position", "transform"}) {
      if (document.property(id, name) != nullptr) {
        property = name;
        break;
      }
    }
  }
  if (property.empty()) return false;
  const JsonValue* value = document.property(id, property);
  return value != nullptr && read_vec3(*value, out);
}

// ---- the store consumer -------------------------------------------------------------------------

TileConsumer StoreTiles::consumer(u8 rings) noexcept {
  TileConsumer out;
  out.name = "store";
  out.context = this;
  out.rings = rings;
  out.activate = &StoreTiles::activate;
  out.deactivate = &StoreTiles::deactivate;
  return out;
}

bool StoreTiles::activate(void* context, const TileEvent& event) {
  auto* self = static_cast<StoreTiles*>(context);
  const StoreTilesBinding& b = self->binding_;
  if (b.store == nullptr || !b.store->is_open() || b.scheduler == nullptr) return false;
  const i64 start = time::monotonic_ns();
  sim::ReconcileParams params;
  params.tile = store_tile(event.tile);
  params.now = b.scheduler->game_time();
  params.materialize_tier = b.materialize_tier;
  static const sim::ObserverSet k_nobody;
  const sim::ObserverSet& observers = b.world != nullptr ? b.world->observers() : k_nobody;
  self->last_ = b.scheduler->reconcile_tile(params, b.store->tile_store(), observers, b.tiers);
  StoreTilesStats& s = self->stats_;
  ++s.reconciled;
  s.known += self->last_.known ? 1u : 0u;
  s.summarized += self->last_.summarized;
  s.records += self->last_.records;
  s.promoted += self->last_.promoted;
  s.reconcile_ns += time::monotonic_ns() - start;
  return true;
}

void StoreTiles::deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<StoreTiles*>(context);
  const StoreTilesBinding& b = self->binding_;
  if (b.store == nullptr || !b.store->is_open() || b.scheduler == nullptr) return;
  const i64 start = time::monotonic_ns();
  // What the tile holds, where the document says it is — after the write-back has told the
  // document where the world left it (03 §3.4, "Dematerialize: flush persistent deltas").
  self->rows_.clear();
  if (b.driver != nullptr && b.document != nullptr) {
    if (b.document_tiles != nullptr && b.world != nullptr) {
      b.document_tiles->flush(b.world->ring());
    } else {
      b.driver->flush_writeback(b.scheduler->tick(), b.scheduler->game_time());
    }
    b.driver->held(sim::MaterializeScope::of_tile(doc::TileCoord{event.tile.x, event.tile.z}),
                   self->held_);
    for (const Id128& id : self->held_) {
      if (!b.document->exists(id)) continue;
      TileRow row;
      row.record = id;
      row.type = std::string(b.document->type_of(id));
      if (!record_position(*b.document, id, row.position)) continue;
      self->rows_.push_back(std::move(row));
    }
  }
  const store::Status status = b.store->write_tile(
      event.tile, std::span<const TileRow>(self->rows_.data(), self->rows_.size()),
      b.scheduler->tick().value, b.scheduler->game_time().us);
  StoreTilesStats& s = self->stats_;
  if (status != store::Status::Ok) {
    ++s.failures;
    ENGINE_LOG_WARN(log_world_tiles, "a tile was not written", log::field("x", event.tile.x),
                    log::field("z", event.tile.z),
                    log::field("status", store::status_name(status)));
  } else {
    ++s.written;
    s.rows += self->rows_.size();
  }
  s.write_ns += time::monotonic_ns() - start;
}

}  // namespace engine::world
