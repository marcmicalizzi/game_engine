#include <core/log/log.h>
#include <core/time/time.h>
#include <systems/world/placement_tiles.h>

#include <algorithm>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_placements, "world.placements");

}  // namespace

const char* ring_ruins_name(RingRuins ruins) noexcept {
  switch (ruins) {
    case RingRuins::Blocks: return "blocks";
    case RingRuins::Sections: return "sections";
    case RingRuins::Walls: return "walls";
  }
  return "?";
}

// One streamed entry: its generator, the state it opened (its kits, its plan, and the scratch a
// tile at a time reuses — so each entry is its own allocation and never moves), and which scene
// mesh each of the generator's resident meshes is.
struct PlacementTiles::Entry {
  const scene_gen::PlacementGeneratorDesc* generator = nullptr;
  void* state = nullptr;
  const renderer::StreamedPlacements* source = nullptr;
  ~Entry() {
    if (generator != nullptr) generator->close(state);
  }
};

PlacementTiles::PlacementTiles() = default;
PlacementTiles::~PlacementTiles() = default;

scene_gen::Context PlacementTiles::context_for(const Entry& entry, u8 ring) const noexcept {
  scene_gen::Context context;
  context.world_seed = scene_->terrain.enabled ? scene_->terrain.seed : 0;
  context.tile_size = config_.tile_size;
  context.dir = entry.source->dir;
  context.where = entry.source->where;
  if (ground_ != nullptr) context.ground = ground_->provider().view();
  context.world = &world_rings_;
  context.ring = ring;
  return context;
}

bool PlacementTiles::create(const renderer::SceneData& scene, const PlacementTilesConfig& config,
                            std::string* error) {
  scene_ = &scene;
  config_ = config;
  entries_.clear();
  tiles_.clear();
  tail_.clear();
  tail_tags_.clear();
  tail_pairs_ = 0;
  stats_ = PlacementTilesStats{};
  // The world block the generators read a ring's representation from: the config's words, with the
  // scene's radii (or the ring's defaults).
  world_rings_ = scene::WorldRings{};
  world_rings_.tile_size = config.tile_size;
  const RingParams defaults;
  for (u32 r = 0; r < config.ring_count && r < k_max_rings; ++r) {
    scene::WorldRing ring;
    ring.radius = scene.world.ring_count > r ? scene.world.radius[r] : defaults.radius[r];
    ring.ruins = static_cast<scene::RingRuins>(config.ruins[r]);
    world_rings_.rings.push_back(ring);
  }
  ground_.reset();
  if (scene.terrain.enabled) ground_ = std::make_unique<renderer::TerrainSampler>(scene.terrain);
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  for (const renderer::StreamedPlacements& source : scene.streamed) {
    auto entry = std::make_unique<Entry>();
    entry->source = &source;
    entry->generator = registry.find_placement(source.generator);
    if (entry->generator == nullptr) {
      if (error != nullptr)
        *error = source.where + " " + registry.unknown_placement(source.generator);
      return false;
    }
    if (entry->generator->tile == nullptr) {
      if (error != nullptr) {
        *error = source.where + ": the placement generator \"" + source.generator +
                 "\" has no tiles, so a streamed world cannot hold it";
      }
      entry->generator = nullptr;
      return false;
    }
    const scene_gen::Context context = context_for(*entry, scene_gen::k_no_ring);
    std::string why;
    void* state = nullptr;
    if (!entry->generator->open(source.params, context, &state, &why)) {
      if (error != nullptr) *error = why;
      entry->generator = nullptr;
      return false;
    }
    entry->state = state;
    ENGINE_LOG_INFO(log_world_placements, "placements streamed", log::field("entry", source.where),
                    log::field("generator", source.generator),
                    log::field("meshes", source.meshes.size()));
    entries_.push_back(std::move(entry));
  }
  return true;
}

TileConsumer PlacementTiles::consumer() noexcept {
  TileConsumer out;
  out.name = "placements";
  out.context = this;
  out.rings = 0x7Fu;
  out.activate = &PlacementTiles::activate;
  out.change_ring = &PlacementTiles::change_ring;
  out.deactivate = &PlacementTiles::deactivate;
  out.commit = &PlacementTiles::commit;
  return out;
}

bool PlacementTiles::occupied(TileCoord tile) const noexcept {
  const scene_gen::TileCoord at{tile.x, tile.z};
  for (const std::unique_ptr<Entry>& entry : entries_) {
    if (entry->generator->occupies == nullptr || entry->generator->occupies(entry->state, at))
      return true;
  }
  return false;
}

std::span<const renderer::SceneInstance> PlacementTiles::tile_instances(
    TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  if (block == nullptr) return {};
  return {block->instances.data(), block->instances.size()};
}

bool PlacementTiles::withheld(TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  return block != nullptr && block->withheld;
}

void PlacementTiles::tile_ranges(Vector<TileRange>& out) const {
  out.clear();
  u32 first = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (tiles_.value_at(i).withheld) continue;
    const u32 count = tiles_.value_at(i).instances.size();
    out.push_back(TileRange{tiles_.key_at(i), first, count});
    first += count;
  }
}

// The tile's placements in `ring`'s representation: every entry's, in the scene's order, each
// exactly what the whole read put on the tile. False when a generator refused the tile (a kit whose
// footprints do not fit it); what did build is kept.
bool PlacementTiles::build(TileCoord tile, u8 ring, TileBlock& out) {
  out.ring = ring;
  out.instances.clear();
  out.tags.clear();
  out.buildings = 0;
  const scene_gen::TileCoord at{tile.x, tile.z};
  bool ok = true;
  scene_gen::Placements placed;
  for (const std::unique_ptr<Entry>& entry_ptr : entries_) {
    const Entry& entry = *entry_ptr;
    if (entry.generator->occupies != nullptr && !entry.generator->occupies(entry.state, at))
      continue;
    placed.clear();
    std::string why;
    const i64 start = time::monotonic_ns();
    if (!entry.generator->tile(entry.state, at, context_for(entry, ring), placed, &why)) {
      ok = false;
      ENGINE_LOG_WARN(log_world_placements, "a tile's placements were not made",
                      log::field("entry", entry.source->where), log::field("x", tile.x),
                      log::field("z", tile.z), log::field("error", why));
      continue;
    }
    if (placed.things == 0 && placed.instances.empty()) continue;  // nothing on this tile
    const Vector<u32>& meshes = entry.source->meshes;
    for (const scene_gen::Placement& p : placed.instances) {
      if (p.mesh >= meshes.size()) {
        ok = false;
        ENGINE_LOG_WARN(log_world_placements, "a tile's placement names a mesh it did not hold",
                        log::field("entry", entry.source->where), log::field("mesh", p.mesh));
        continue;
      }
      // At the placement's `WorldPos`, with its rotation and scale (ADR-0053): the renderer adds
      // the mesh's fit to it in f64 (`renderer::instance_translation`).
      renderer::SceneInstance instance;
      instance.mesh = meshes[p.mesh];
      instance.transform = p.turn();
      instance.origin = p.position;
      out.instances.push_back(instance);
      out.tags.push_back(p.tag);
    }
    const u32 kind = placed.kind < k_placement_kinds ? placed.kind : k_placement_kinds - 1;
    stats_.build_ns[kind] += time::monotonic_ns() - start;
    ++stats_.built[kind];
    out.buildings += placed.things;
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

// A tile activated or moved: its placements in the ring's representation. Whether it is drawn is
// the budget's, decided at the commit over every tile held (`admit`), not here: deciding per event
// would give the budget to whichever tiles come first in tile order, and a first fill would then
// refuse a building beside the camera for one at the horizon.
bool PlacementTiles::take(const TileEvent& event) {
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

bool PlacementTiles::activate(void* context, const TileEvent& event) {
  return static_cast<PlacementTiles*>(context)->take(event);
}

bool PlacementTiles::change_ring(void* context, const TileEvent& event) {
  auto* self = static_cast<PlacementTiles*>(context);
  // Only when some entry on the tile draws the two rings differently (the ruins: sections with
  // debris to walls only is a change, and so is anything to or from blocks); two rings that draw
  // the same need nothing built again.
  const scene_gen::TileCoord at{event.tile.x, event.tile.z};
  bool same = true;
  for (const std::unique_ptr<Entry>& entry : self->entries_) {
    const scene_gen::PlacementGeneratorDesc& g = *entry->generator;
    if (g.occupies != nullptr && !g.occupies(entry->state, at)) continue;
    if (g.representation == nullptr ||
        g.representation(entry->state, self->context_for(*entry, event.from)) !=
            g.representation(entry->state, self->context_for(*entry, event.to))) {
      same = false;
      break;
    }
  }
  if (same) {
    if (TileBlock* held = self->tiles_.find_value(tile_key(event.tile))) held->ring = event.to;
    return true;
  }
  return self->take(event);
}

void PlacementTiles::deactivate(void* context, const TileEvent& event) {
  auto* self = static_cast<PlacementTiles*>(context);
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
void PlacementTiles::admit() {
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
    ENGINE_LOG_WARN(log_world_placements, "the pair budget withholds tiles' placements",
                    log::field("withheld", withheld), log::field("nearest_ring", nearest),
                    log::field("pairs", pairs), log::field("budget", config_.max_pairs));
  }
  stats_.refused += newly;
  stats_.withheld = withheld;
}

void PlacementTiles::commit(void* context) {
  auto* self = static_cast<PlacementTiles*>(context);
  // Every active tile's instances, in tile order: the map is sorted by key, so a scene streamed to
  // a set of tiles has one tail — one numbering of its pairs — whatever order the tiles came in.
  self->admit();
  self->tail_.clear();
  self->tail_tags_.clear();
  u32 buildings = 0;
  for (u32 i = 0; i < self->tiles_.size(); ++i) {
    const TileBlock& block = self->tiles_.value_at(i);
    if (block.withheld) continue;
    buildings += block.buildings;
    for (const renderer::SceneInstance& instance : block.instances)
      self->tail_.push_back(instance);
    for (const u8 tag : block.tags)
      self->tail_tags_.push_back(tag);
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
    ENGINE_LOG_ERROR(log_world_placements, "the renderer refused the placements' instances",
                     log::field("instances", self->tail_.size()),
                     log::field("pairs", self->tail_pairs_), log::field("error", error));
  }
}

}  // namespace engine::world
