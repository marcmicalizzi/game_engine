#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <systems/world/ruins_tiles.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_ruins, "world.ruins");

f32 terrain_ground(const void* context, f32 x, f32 z) noexcept {
  return static_cast<const renderer::TerrainSampler*>(context)->height(x, z);
}

renderer::SceneInstance yawed(u32 mesh, Vec3 at, u32 yaw_step) {
  renderer::SceneInstance instance;
  instance.mesh = mesh;
  instance.transform.position = at;
  instance.transform.rotation =
      quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(22.5f * static_cast<f32>(yaw_step)));
  return instance;
}

}  // namespace

const char* ring_ruins_name(RingRuins ruins) noexcept {
  switch (ruins) {
    case RingRuins::Blocks: return "blocks";
    case RingRuins::Sections: return "sections";
    case RingRuins::Walls: return "walls";
  }
  return "?";
}

// One streamed entry: its kits (which the assemblers hold by reference, so each entry is its own
// allocation and never moves), its placement, and the tiles that hold its buildings, sorted by key.
struct RuinsTiles::Entry {
  renderer::StreamedRuins source;
  ruins::Kit kit;
  ruins::BlockKit block_kit;
  bool blocks = false;
  ruins::Placement placement;
  Vector<u64> tiles;
  std::unique_ptr<ruins::Assembler> assembler;
  std::unique_ptr<ruins::BlockAssembler> block_assembler;
  ruins::Output out;
  ruins::BlockOutput block_out;
};

RuinsTiles::RuinsTiles() = default;
RuinsTiles::~RuinsTiles() = default;

bool RuinsTiles::create(const renderer::SceneData& scene, const RuinsTilesConfig& config,
                        std::string* error) {
  scene_ = &scene;
  config_ = config;
  entries_.clear();
  tiles_.clear();
  tail_.clear();
  tail_pairs_ = 0;
  stats_ = RuinsTilesStats{};
  if (scene.terrain.enabled) ground_ = std::make_unique<renderer::TerrainSampler>(scene.terrain);
  for (u32 e = 0; e < scene.streamed_ruins.size(); ++e) {
    const renderer::StreamedRuins& source = scene.streamed_ruins[e];
    auto entry = std::make_unique<Entry>();
    entry->source = source;
    std::string why;
    if (std::abs(source.tile_size - config.tile_size) > 1e-4f) {
      if (error != nullptr) {
        *error = "ruins entry " + std::to_string(e) + " is on " + std::to_string(source.tile_size) +
                 " m tiles and the ring on " + std::to_string(config.tile_size);
      }
      return false;
    }
    if (!ruins::read_kit_file(source.kit, entry->kit, why)) {
      if (error != nullptr) *error = "ruins entry " + std::to_string(e) + ": " + why;
      return false;
    }
    entry->blocks = source.blocks && !source.block_kit.empty();
    if (entry->blocks && !ruins::read_block_kit_file(source.block_kit, entry->block_kit, why)) {
      if (error != nullptr) *error = "ruins entry " + std::to_string(e) + ": " + why;
      return false;
    }
    entry->placement.world_seed = source.seed;
    entry->placement.tile_cm = ruins::to_cm(source.tile_size);
    entry->placement.wind_step = ruins::yaw_step_from_degrees(source.wind_deg);
    if (ground_ != nullptr) entry->placement.ground = ruins::Ground{&terrain_ground, ground_.get()};
    // The same tiles the whole read would have assembled: `choose_tiles` over the entry's square.
    Vector<ruins::TileCoord> chosen;
    ruins::choose_tiles(source.seed, ruins::TileCoord{source.tile_min[0], source.tile_min[1]},
                        ruins::TileCoord{source.tile_max[0], source.tile_max[1]}, source.count,
                        source.density, chosen);
    for (const ruins::TileCoord& t : chosen)
      entry->tiles.push_back(tile_key(TileCoord{t.x, t.z}));
    std::sort(entry->tiles.begin(), entry->tiles.end());
    entry->assembler = std::make_unique<ruins::Assembler>(entry->kit);
    if (entry->blocks) {
      entry->block_assembler =
          std::make_unique<ruins::BlockAssembler>(entry->kit, entry->block_kit);
    }
    ENGINE_LOG_INFO(log_world_ruins, "ruins streamed", log::field("entry", e),
                    log::field("kit", entry->kit.name),
                    log::field("block_kit", entry->blocks ? entry->block_kit.name.c_str() : ""),
                    log::field("buildings", entry->tiles.size()));
    entries_.push_back(std::move(entry));
  }
  return true;
}

TileConsumer RuinsTiles::consumer() noexcept {
  TileConsumer out;
  out.name = "ruins";
  out.context = this;
  out.rings = 0x7Fu;
  out.activate = &RuinsTiles::activate;
  out.change_ring = &RuinsTiles::change_ring;
  out.deactivate = &RuinsTiles::deactivate;
  out.commit = &RuinsTiles::commit;
  return out;
}

bool RuinsTiles::has_building(TileCoord tile) const noexcept {
  const u64 key = tile_key(tile);
  for (const std::unique_ptr<Entry>& entry : entries_) {
    if (std::binary_search(entry->tiles.begin(), entry->tiles.end(), key)) return true;
  }
  return false;
}

std::span<const renderer::SceneInstance> RuinsTiles::tile_instances(TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  if (block == nullptr) return {};
  return {block->instances.data(), block->instances.size()};
}

bool RuinsTiles::withheld(TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  return block != nullptr && block->withheld;
}

void RuinsTiles::tile_ranges(Vector<TileRange>& out) const {
  out.clear();
  u32 first = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (tiles_.value_at(i).withheld) continue;
    const u32 count = tiles_.value_at(i).instances.size();
    out.push_back(TileRange{tiles_.key_at(i), first, count});
    first += count;
  }
}

// The tile's buildings in `ring`'s representation. False when an assembler refused the tile (a kit
// whose footprints do not fit it); what did assemble is kept.
bool RuinsTiles::build(TileCoord tile, u8 ring, TileBlock& out) {
  out.ring = ring;
  out.instances.clear();
  out.buildings = 0;
  const RingRuins want = ring < config_.ring_count ? config_.ruins[ring] : RingRuins::Walls;
  const u64 key = tile_key(tile);
  const ruins::TileCoord at{tile.x, tile.z};
  bool ok = true;
  for (const std::unique_ptr<Entry>& entry_ptr : entries_) {
    Entry& entry = *entry_ptr;
    if (!std::binary_search(entry.tiles.begin(), entry.tiles.end(), key)) continue;
    std::string why;
    const i64 start = time::monotonic_ns();
    if (want == RingRuins::Blocks && entry.blocks) {
      entry.block_out.clear();
      ruins::Placement placement = entry.placement;
      placement.detail = ruins::Detail::full;
      if (!entry.block_assembler->assemble(placement, at, entry.block_out, &why)) {
        ok = false;
        ENGINE_LOG_WARN(log_world_ruins, "a tile's building was not laid", log::field("x", tile.x),
                        log::field("z", tile.z), log::field("error", why));
        continue;
      }
      for (const ruins::Block& block : entry.block_out.blocks) {
        out.instances.push_back(
            yawed(entry.source.block_first_mesh + entry.block_kit.blocks[block.block].mesh_index,
                  ruins::block_translation(entry.block_kit, block),
                  ruins::block_yaw_step(entry.block_kit, block)));
      }
      stats_.lay_ns += time::monotonic_ns() - start;
      ++stats_.laid;
    } else {
      entry.out.clear();
      ruins::Placement placement = entry.placement;
      placement.detail = want == RingRuins::Walls ? ruins::Detail::walls : ruins::Detail::full;
      if (!entry.assembler->assemble(placement, at, entry.out, &why)) {
        ok = false;
        ENGINE_LOG_WARN(log_world_ruins, "a tile's building was not assembled",
                        log::field("x", tile.x), log::field("z", tile.z), log::field("error", why));
        continue;
      }
      for (const ruins::Instance& piece : entry.out.instances) {
        out.instances.push_back(
            yawed(entry.source.kit_first_mesh + entry.kit.members[piece.member].mesh_index,
                  ruins::instance_translation(entry.kit, piece),
                  ruins::instance_yaw_step(entry.kit, piece)));
      }
      stats_.assemble_ns += time::monotonic_ns() - start;
      ++stats_.assembled;
    }
    ++out.buildings;
  }
  u32 end = 0;
  out.pairs = renderer::pairs_after(*scene_, 0,
                                    std::span<const renderer::SceneInstance>(out.instances.data(),
                                                                             out.instances.size()),
                                    renderer::k_max_pairs, end)
                  ? end
                  : renderer::k_max_pairs + 1u;
  return ok;
}

// A tile activated or moved: its building in the ring's representation. Whether it is drawn is
// the budget's, decided at the commit over every tile held (`admit`), not here: deciding per event
// would give the budget to whichever tiles come first in tile order, and a first fill would then
// refuse a building beside the camera for one at the horizon.
bool RuinsTiles::take(const TileEvent& event) {
  TileBlock block;
  const bool built = build(event.tile, event.to, block);
  if (!built) ++stats_.failures;
  const u64 key = tile_key(event.tile);
  TileBlock* held = tiles_.find_value(key);
  if (block.instances.empty()) {
    if (held != nullptr) tiles_.erase(key);
    return built;
  }
  block.withheld = held != nullptr && held->withheld;
  tiles_.insert_or_assign(key, std::move(block));
  return built;
}
bool RuinsTiles::activate(void* context, const TileEvent& event) {
  return static_cast<RuinsTiles*>(context)->take(event);
}

bool RuinsTiles::change_ring(void* context, const TileEvent& event) {
  auto* self = static_cast<RuinsTiles*>(context);
  // Only when the representation changes: sections with debris to walls only is a change, and so
  // is anything to or from blocks; two rings that draw the same need no new building.
  const auto drawn = [&](u8 ring) {
    const RingRuins r =
        ring < self->config_.ring_count ? self->config_.ruins[ring] : RingRuins::Walls;
    return r;
  };
  if (drawn(event.from) == drawn(event.to)) {
    if (TileBlock* held = self->tiles_.find_value(tile_key(event.tile))) held->ring = event.to;
    return true;
  }
  return self->take(event);
}

void RuinsTiles::deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<RuinsTiles*>(context);
  const u64 key = tile_key(event.tile);
  TileBlock* held = self->tiles_.find_value(key);
  if (held == nullptr) return;
  self->tiles_.erase(key);
  ++self->stats_.dropped;
}

// Which held tiles the tail draws: by ring, innermost first, then by the ring's score — the
// nearest first — then in tile order, admitted while the scene stays within its pair budget and
// none after the first that does not fit, so every tile drawn outranks every tile withheld. A
// function of the tiles held, their rings and the observers, like the ring itself.
void RuinsTiles::admit() {
  ranks_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const u64 key = tiles_.key_at(i);
    Rank rank;
    rank.ring = tiles_.value_at(i).ring;
    rank.score = ring_ != nullptr ? ring_->score_of(tile_of_key(key)) : 0.0f;
    rank.key = key;
    rank.index = i;
    ranks_.push_back(rank);
  }
  std::sort(ranks_.begin(), ranks_.end(), [](const Rank& a, const Rank& b) {
    if (a.ring != b.ring) return a.ring < b.ring;
    if (a.score != b.score) return a.score < b.score;
    return a.key < b.key;
  });
  u64 pairs = scene_->pair_count;
  bool full = false;
  u32 withheld = 0;
  u32 newly = 0;
  u8 nearest = k_inactive;
  for (const Rank& rank : ranks_) {
    TileBlock& block = tiles_.value_at(rank.index);
    if (!full && pairs + block.pairs <= config_.max_pairs) {
      pairs += block.pairs;
      block.withheld = false;
      continue;
    }
    full = true;
    ++withheld;
    newly += block.withheld ? 0u : 1u;
    nearest = rank.ring < nearest ? rank.ring : nearest;
    block.withheld = true;
  }
  tail_pairs_ = static_cast<u32>(pairs - scene_->pair_count);
  if (withheld != 0 && withheld != stats_.withheld) {
    ENGINE_LOG_WARN(log_world_ruins, "the pair budget withholds tiles' ruins",
                    log::field("withheld", withheld), log::field("nearest_ring", nearest),
                    log::field("pairs", pairs), log::field("budget", config_.max_pairs));
  }
  stats_.refused += newly;
  stats_.withheld = withheld;
}

void RuinsTiles::commit(void* context) {
  auto* self = static_cast<RuinsTiles*>(context);
  // Every active tile's instances, in tile order: the map is sorted by key, so a scene streamed to
  // a set of tiles has one tail — one numbering of its pairs — whatever order the tiles came in.
  self->admit();
  self->tail_.clear();
  u32 buildings = 0;
  for (u32 i = 0; i < self->tiles_.size(); ++i) {
    const TileBlock& block = self->tiles_.value_at(i);
    if (block.withheld) continue;
    buildings += block.buildings;
    for (const renderer::SceneInstance& instance : block.instances)
      self->tail_.push_back(instance);
  }
  self->stats_.buildings = buildings;
  self->stats_.instances = self->tail_.size();
  self->stats_.pairs = self->tail_pairs_;
  if (self->sink_ == nullptr) return;
  std::string error;
  const i64 start = time::monotonic_ns();
  const bool ok = self->sink_(
      self->sink_context_,
      std::span<const renderer::SceneInstance>(self->tail_.data(), self->tail_.size()), &error);
  const i64 dt = time::monotonic_ns() - start;
  self->stats_.sink_ns += dt;
  self->stats_.max_sink_ns = dt > self->stats_.max_sink_ns ? dt : self->stats_.max_sink_ns;
  ++self->stats_.handed;
  if (!ok) {
    ++self->stats_.failures;
    ENGINE_LOG_ERROR(log_world_ruins, "the renderer refused the ruins' instances",
                     log::field("instances", self->tail_.size()),
                     log::field("pairs", self->tail_pairs_), log::field("error", error));
  }
}

}  // namespace engine::world
