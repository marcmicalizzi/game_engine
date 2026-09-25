#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/doc/partition.h>
#include <systems/world/document_tiles.h>

#include <cmath>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_doc, "world.document");

}  // namespace

void DocumentTiles::bind(const doc::Document* document) noexcept {
  document_ = document;
  grid_ok_ = grid_matches();
}

bool DocumentTiles::grid_matches(const char** why) const noexcept {
  if (document_ == nullptr) return true;
  for (u32 i = 0; i < document_->layer_count(); ++i) {
    const doc::Layer& layer = document_->layer(i);
    if (!layer.partitioned()) continue;
    if (std::abs(layer.partition().tile_size - static_cast<f64>(tile_size_)) > 1e-6) {
      if (why != nullptr) {
        *why =
            "a partitioned layer's tile size is not the ring's, so a ring tile is not a "
            "document tile";
      }
      return false;
    }
  }
  return true;
}

TileConsumer DocumentTiles::consumer(u8 rings, const TileRing* ring) noexcept {
  rings_ = rings;
  ring_ = ring;
  TileConsumer out;
  out.name = "document";
  out.context = this;
  out.rings = rings;
  out.activate = &DocumentTiles::activate;
  out.deactivate = &DocumentTiles::deactivate;
  return out;
}

u32 DocumentTiles::settle(const TileRing& ring) {
  if (document_ == nullptr || !grid_ok_) return 0;
  driver_->take_moved(moves_);
  if (moves_.empty()) return 0;
  gone_.clear();
  for (const sim::TileMove& move : moves_) {
    // Out of every tile: the record is one the document materializes whole, as the host does.
    if (!move.to_tiled) {
      driver_->refile(move.id, false, doc::TileCoord{});
      ++stats_.refiled;
      continue;
    }
    const u8 at = ring.ring_of(TileCoord{move.to.x, move.to.y});
    if (at != k_inactive && at < 8 && ((rings_ >> at) & 1u) != 0) {
      driver_->refile(move.id, true, move.to);
      ++stats_.refiled;
    } else {
      gone_.push_back(move.id);
    }
  }
  if (gone_.empty()) return 0;
  // Its state is already the document's: the write-back that moved it is what reported the move.
  const u32 left = driver_->dematerialize(std::span<const Id128>(gone_.data(), gone_.size()));
  stats_.left += left;
  stats_.dematerialized += left;
  return left;
}

void DocumentTiles::flush(const TileRing& ring) {
  if (document_ == nullptr) return;
  driver_->flush_writeback(scheduler_->tick(), scheduler_->game_time());
  settle(ring);
}

bool DocumentTiles::activate(void* context, const TileEvent& event) {
  auto* self = static_cast<DocumentTiles*>(context);
  if (self->document_ == nullptr) return false;
  if (!self->grid_ok_) {
    if (!self->warned_) {
      self->warned_ = true;
      const char* why = nullptr;
      self->grid_matches(&why);
      ENGINE_LOG_WARN(log_world_doc, "the document's tiles are not the ring's",
                      log::field("reason", why != nullptr ? why : ""),
                      log::field("ring_tile_size", self->tile_size_));
    }
    ++self->stats_.refused;
    return false;
  }
  const i64 start = time::monotonic_ns();
  const sim::MaterializeReport report = self->driver_->materialize(
      *self->document_, sim::MaterializeScope::of_tile(doc::TileCoord{event.tile.x, event.tile.z}));
  DocumentTilesStats& s = self->stats_;
  ++s.activated;
  s.created += report.created;
  s.visited += report.visited;
  s.materialize_ns += time::monotonic_ns() - start;
  return true;
}

void DocumentTiles::deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<DocumentTiles*>(context);
  if (self->document_ == nullptr || !self->grid_ok_) return;
  const i64 start = time::monotonic_ns();
  // What systems changed goes back to the document before the entities that changed it go, and a
  // record that change moved into another tile goes with that tile rather than this one.
  if (self->ring_ != nullptr) {
    self->flush(*self->ring_);
  } else {
    self->driver_->flush_writeback(self->scheduler_->tick(), self->scheduler_->game_time());
  }
  const u32 gone = self->driver_->dematerialize(
      sim::MaterializeScope::of_tile(doc::TileCoord{event.tile.x, event.tile.z}));
  DocumentTilesStats& s = self->stats_;
  ++s.deactivated;
  s.dematerialized += gone;
  s.dematerialize_ns += time::monotonic_ns() - start;
}

}  // namespace engine::world
