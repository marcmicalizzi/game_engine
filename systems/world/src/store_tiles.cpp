#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/doc/partition.h>
#include <systems/world/store_tiles.h>

#include <algorithm>
#include <cmath>
#include <schemas/world_settings.h>
#include <string>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_tiles, "world.store_tiles");

// A JSON number is a double, and the document's position is one (`worldpos`, ADR-0053): read as
// it is, never through a float32, which put a record 420 km out on a 3.1 cm grid in its tile's
// projection — and so wherever the tile's next activation promoted it from.
bool number(const JsonValue* value, f64& out) {
  return value != nullptr && value->get_f64(out) && std::isfinite(out);
}

// [x, y] (on the ground, y = 0 here) or [x, y, z]; an object with x, y, z; or a transform with a
// `position` or `translation` member — the shapes `doc::read_position` accepts, all three axes.
bool read_point(const JsonValue& value, WorldPos& out) {
  if (value.is_array()) {
    if (value.size() == 3) {
      return number(&value[0], out.x) && number(&value[1], out.y) && number(&value[2], out.z);
    }
    if (value.size() == 2) {
      out.y = 0.0;
      return number(&value[0], out.x) && number(&value[1], out.z);
    }
    return false;
  }
  if (!value.is_object()) return false;
  for (const char* member : {"position", "translation"}) {
    if (const JsonValue* inner = value.find(member)) return read_point(*inner, out);
  }
  out.y = 0.0;
  if (!number(value.find("x"), out.x)) return false;
  number(value.find("y"), out.y);
  return number(value.find("z"), out.z);
}

}  // namespace

// ---- where a record is --------------------------------------------------------------------------

bool record_position(const doc::Document& document, const Id128& id, WorldPos& out) {
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
  // A position no cell can name is not a place a projection can keep (`worldpos` refuses it).
  WorldPos p;
  if (value == nullptr || !read_point(*value, p) || !world_cell_valid(p)) return false;
  out = p;
  return true;
}

// ---- the world seed -----------------------------------------------------------------------------

bool document_world_seed(const doc::Document& document, u64& out) {
  // One lookup at a fixed id, never a walk of the document for a type (AGENTS.md: ask the
  // document); a deleted record names no seed.
  if (!document.exists(k_world_settings) ||
      document.type_of(k_world_settings) != std::string_view(k_world_settings_type))
    return false;
  u64 seed = WorldSettings{}.seed;
  const JsonValue* value = document.property(k_world_settings, "seed");
  if (value != nullptr && !value->get_u64(seed)) return false;
  out = seed;
  return true;
}

// ---- the projections a flush wrote --------------------------------------------------------------

store::Status project_writeback(WorldStore& world_store, const doc::Document& document,
                                const sim::WriteBackBatch& batch, ProjectWritebackStats* stats) {
  ProjectWritebackStats s;
  struct Row {
    u64 tile = 0;
    TileRow row;
  };
  // The records to project: the batch's, in (record, row) order, one projection a record — and,
  // after a call whose write failed, its records again (D45), as the document has them now.
  Vector<Id128> ids;
  ids.reserve(batch.changes.size());
  for (u32 i = 0; i < batch.changes.size(); ++i) {
    const Id128& id = batch.changes[i].record;
    if (i != 0 && batch.changes[i - 1].record == id) continue;
    ids.push_back(id);
  }
  Vector<Id128>& unprojected = world_store.unprojected();
  if (!unprojected.empty()) {
    s.retried = unprojected.size();
    for (const Id128& id : unprojected)
      ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }
  Vector<Row> rows;
  Vector<Id128> gone;
  for (const Id128& id : ids) {
    doc::TileCoord tile;
    Row r;
    if (!document.exists(id) || !record_position(document, id, r.row.position)) {
      // Its projection goes with it: a later load would read a record the document cannot place.
      ++s.gone;
      gone.push_back(id);
      continue;
    }
    if (!document.object_tile(id, tile)) {
      ++s.untiled;
      continue;
    }
    r.tile = store_tile(TileCoord{tile.x, tile.y});
    r.row.record = id;
    r.row.type = std::string(document.type_of(id));
    rows.push_back(std::move(r));
  }
  // Clustered by tile (store.md: a flush sorts by tile before it writes, 4.5x), then by record, so
  // the rows go in in an order that is a function of the batch.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.tile != b.tile) return a.tile < b.tile;
    return a.row.record < b.row.record;
  });
  Vector<TileRow> out;
  Vector<u64> tiles;
  out.reserve(rows.size());
  tiles.reserve(rows.size());
  for (Row& r : rows) {
    tiles.push_back(r.tile);
    out.push_back(std::move(r.row));
  }
  const store::Status status =
      world_store.write_projections(std::span<const TileRow>(out.data(), out.size()),
                                    std::span<const u64>(tiles.data(), tiles.size()),
                                    std::span<const Id128>(gone.data(), gone.size()));
  if (status == store::Status::Ok) {
    s.written = out.size();
    s.erased = gone.size();
    unprojected.clear();
  } else {
    // Nothing was written (one transaction): the next call projects these again.
    unprojected = std::move(ids);
  }
  if (stats != nullptr) *stats = s;
  return status;
}

// ---- the store consumer -------------------------------------------------------------------------

StoreTiles::StoreTiles(const StoreTilesBinding& binding) noexcept : binding_(binding) {
  u64 seed = 0;
  if (binding_.store != nullptr && binding_.document != nullptr &&
      document_world_seed(*binding_.document, seed))
    binding_.store->set_world_seed(seed);
}

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
  // With a document bound, the tile's projections are read through it: one whose record the
  // document no longer has is left out (D45) — a store written before the flush and a tile's going
  // erased them may still hold one, and the tile's next going erases it.
  sim::TileStore seam = b.store->tile_store();
  if (b.document != nullptr) {
    self->inner_ = seam;
    seam.context = self;
    seam.tile_state = &StoreTiles::tile_state;
    seam.load_records = &StoreTiles::load_records;
  }
  self->last_ = b.scheduler->reconcile_tile(params, seam, observers, b.tiers);
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
  // What the tile's projections name that the document no longer has goes with the tile's rows, in
  // their transaction (D45): a record deleted while its tile was live, or before this mend, would
  // otherwise keep its projection here for good, since an upsert never removes a row.
  self->gone_.clear();
  if (b.document != nullptr) {
    if (b.store->tile_records(event.tile, self->stored_) == store::Status::Ok) {
      WorldPos unused;
      for (const Id128& id : self->stored_) {
        if (!b.document->exists(id) || !record_position(*b.document, id, unused))
          self->gone_.push_back(id);
      }
    }
  }
  const store::Status status = b.store->write_tile(
      event.tile, std::span<const TileRow>(self->rows_.data(), self->rows_.size()),
      b.scheduler->tick().value, b.scheduler->game_time().us,
      std::span<const Id128>(self->gone_.data(), self->gone_.size()));
  StoreTilesStats& s = self->stats_;
  if (status != store::Status::Ok) {
    ++s.failures;
    ENGINE_LOG_WARN(log_world_tiles, "a tile was not written", log::field("x", event.tile.x),
                    log::field("z", event.tile.z),
                    log::field("status", store::status_name(status)));
  } else {
    ++s.written;
    s.rows += self->rows_.size();
    s.erased += self->gone_.size();
  }
  s.write_ns += time::monotonic_ns() - start;
}

bool StoreTiles::tile_state(void* context, u64 tile, sim::TileState& out) {
  const sim::TileStore& inner = static_cast<StoreTiles*>(context)->inner_;
  return inner.tile_state != nullptr && inner.tile_state(inner.context, tile, out);
}

void StoreTiles::load_records(void* context, u64 tile, Vector<sim::EntityRecord>& out) {
  auto* self = static_cast<StoreTiles*>(context);
  const u32 first = out.size();
  if (self->inner_.load_records != nullptr)
    self->inner_.load_records(self->inner_.context, tile, out);
  const doc::Document& document = *self->binding_.document;
  // Kept in the store's order; a record the document cannot place is left out.
  u32 kept = first;
  WorldPos unused;
  for (u32 i = first; i < out.size(); ++i) {
    const Id128& id = out[i].entity;
    if (!document.exists(id) || !record_position(document, id, unused)) {
      ++self->stats_.skipped;
      continue;
    }
    if (kept != i) out[kept] = out[i];
    ++kept;
  }
  out.resize(kept);
}

}  // namespace engine::world
