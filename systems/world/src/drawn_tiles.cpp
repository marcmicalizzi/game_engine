// The drawn ground (drawn_tiles.h).
#include <core/time/time.h>
#include <systems/world/drawn_tiles.h>

#include <algorithm>

namespace engine::world {

void DrawnTiles::create(renderer::TerrainTileSet& tiles) noexcept {
  tiles_ = &tiles;
  held_.clear();
  handed_.clear();
  stats_ = DrawnTilesStats{};
}

TileConsumer DrawnTiles::consumer() noexcept {
  TileConsumer out;
  out.name = "drawn ground";
  out.context = this;
  out.rings = 0x7Fu;
  out.activate = &DrawnTiles::on_activate;
  out.change_ring = &DrawnTiles::on_change;
  out.deactivate = &DrawnTiles::on_deactivate;
  out.commit = &DrawnTiles::on_commit;
  return out;
}

RingParams DrawnTiles::ring_params(const renderer::TerrainTilesDesc& tiles) noexcept {
  RingParams p;
  p.tile_size = tiles.tile_size;
  p.ring_count = std::min<u32>(tiles.ring_count, k_max_rings);
  for (u32 r = 0; r < k_max_rings; ++r)
    p.radius[r] = r < p.ring_count ? tiles.radius[r] : 0.0f;
  p.hysteresis = tiles.hysteresis;
  p.max_activations = max_activations_tunable();
  p.max_deactivations = max_deactivations_tunable();
  return p;
}

bool DrawnTiles::on_activate(void* context, const TileEvent& event) {
  auto& self = *static_cast<DrawnTiles*>(context);
  self.held_.insert_or_assign(tile_key(event.tile), event.to);
  self.handed_.push_back(renderer::TerrainTile{event.tile.x, event.tile.z, event.to});
  ++self.stats_.activated;
  return true;
}

bool DrawnTiles::on_change(void* context, const TileEvent& event) {
  auto& self = *static_cast<DrawnTiles*>(context);
  self.held_.insert_or_assign(tile_key(event.tile), event.to);
  self.handed_.push_back(renderer::TerrainTile{event.tile.x, event.tile.z, event.to});
  ++self.stats_.moved;
  return true;
}

void DrawnTiles::on_deactivate(void* context, const TileEvent& event) {
  auto& self = *static_cast<DrawnTiles*>(context);
  self.held_.erase(tile_key(event.tile));
  self.handed_.push_back(renderer::TerrainTile{event.tile.x, event.tile.z, renderer::k_tile_gone});
  ++self.stats_.deactivated;
}

void DrawnTiles::on_commit(void* context) {
  auto& self = *static_cast<DrawnTiles*>(context);
  if (self.tiles_ == nullptr) return;
  const i64 started = time::monotonic_ns();
  ++self.stats_.commits;
  // What changed, not the whole set: the renderer's rebuild then costs what changed too.
  if (self.tiles_->change_tiles(
          std::span<const renderer::TerrainTile>(self.handed_.data(), self.handed_.size())))
    ++self.stats_.changed;
  self.handed_.clear();
  self.stats_.held = self.held_.size();
  const i64 took = time::monotonic_ns() - started;
  self.stats_.commit_ns += took;
  self.stats_.max_commit_ns = std::max(self.stats_.max_commit_ns, took);
}

}  // namespace engine::world
