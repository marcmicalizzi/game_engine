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
  scene_gen::Placements placed;  // one entry's tile output, kept across arrivals and rebuilds
  ~Entry() {
    if (generator != nullptr) generator->close(state);
  }
};

PlacementTiles::PlacementTiles() = default;
PlacementTiles::~PlacementTiles() = default;

scene_gen::Context PlacementTiles::context_for(const Entry& entry, u8 ring,
                                               bool scene_ground) const noexcept {
  scene_gen::Context context;
  context.world_seed = scene_->terrain.enabled ? scene_->terrain.seed : 0;
  context.tile_size = config_.tile_size;
  context.dir = entry.source->dir;
  context.where = entry.source->where;
  if (ground_ != nullptr) context.ground = ground_->provider().view();
  if (!scene_->terrain.exclusion.empty()) context.exclusion = &scene_->terrain.exclusion;
  context.site = &scene_->terrain.site;
  if (scene_->terrain.formation.has_value()) context.formation = &*scene_->terrain.formation;
  context.uncut_ground = uncut_hook_;
  if (stand_ != nullptr && !scene_ground) context.stand = stand_->provider().view();
  context.earlier = std::span<const scene_gen::Footprint>(earlier_.data(), earlier_.size());
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
  spare_tiles_.clear();
  free_blocks_.clear();
  tail_.clear();
  tail_tags_.clear();
  tail_stale_ = false;
  runs_.clear();
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
  stand_.reset();
  stand_time_ = scene.terrain.enabled ? scene.terrain.time_s : 0.0;
  earlier_.clear();
  if (scene.terrain.enabled) ground_ = std::make_unique<renderer::TerrainSampler>(scene.terrain);
  uncut_ = std::make_unique<renderer::UncutGround>(scene.terrain);
  uncut_hook_ = uncut_->hook();
  // No room for a tile's meshes until a host says where they go (`set_meshes`).
  meshes_.create(scene.mesh_options, TileMeshConfig{}, TileMeshSink{}, nullptr);
  mesh_refusals_logged_ = 0;
  // Each mesh's reach on the ground, measured as the reader measures it for a whole read.
  radius_of_.clear();
  for (const geometry::ClusterMeshPart& part : scene.parts)
    radius_of_.push_back(
        renderer::ground_radius(scene.lod, part.first_cluster, part.cluster_count));
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  FlatMap<u64, u32> tile_counts;
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
    u32 largest = 0;
    for (const renderer::StreamedPlacements::TileInstances& bound : source.tile_instances) {
      tile_counts[tile_key(TileCoord{bound.x, bound.z})] += bound.count;
      largest = std::max(largest, bound.count);
    }
    entry->placed.instances.reserve(largest);
    ENGINE_LOG_INFO(log_world_placements, "placements streamed", log::field("entry", source.where),
                    log::field("generator", source.generator),
                    log::field("meshes", source.meshes.size()));
    entries_.push_back(std::move(entry));
  }
  u32 total = 0;
  u32 largest = 0;
  spare_tiles_.reserve(tile_counts.size());
  for (u32 i = 0; i < tile_counts.size(); ++i) {
    TileBlock& spare = spare_tiles_[tile_counts.key_at(i)];
    const u32 count = tile_counts.value_at(i);
    spare.instances.reserve(count);
    spare.tags.reserve(count);
    total += count;
    largest = std::max(largest, count);
  }
  tiles_.reserve(spare_tiles_.size());
  ranks_.reserve(spare_tiles_.size());
  runs_.reserve(spare_tiles_.size());
  tail_.reserve(total);
  tail_tags_.reserve(total);
  earlier_.reserve(largest);
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

bool PlacementTiles::redraws(TileCoord tile, u8 from, u8 to) const noexcept {
  const scene_gen::TileCoord at{tile.x, tile.z};
  for (const std::unique_ptr<Entry>& entry : entries_) {
    const scene_gen::PlacementGeneratorDesc& g = *entry->generator;
    if (g.occupies != nullptr && !g.occupies(entry->state, at)) continue;
    if (g.representation == nullptr ||
        g.representation(entry->state, context_for(*entry, from)) !=
            g.representation(entry->state, context_for(*entry, to))) {
      return true;
    }
  }
  return false;
}

std::span<const renderer::SceneInstance> PlacementTiles::tile_instances(
    TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  if (block == nullptr) return {};
  return block->drawn();
}

void PlacementTiles::set_meshes(const TileMeshConfig& config, TileMeshSink sink,
                                jobs::JobSystem* jobs) {
  if (scene_ == nullptr) return;
  meshes_.create(scene_->mesh_options, config, sink, jobs);
}

void PlacementTiles::release_tickets(Vector<u32>& tickets) noexcept {
  for (const u32 slot : tickets)
    meshes_.release(slot);
  tickets.clear();
}

// Before a build: what the tile draws now becomes what it draws while the build waits for its
// meshes. A tile already waiting keeps what it holds and lets the build it never drew go.
void PlacementTiles::stage(TileBlock& block) {
  if (block.waiting) {
    release_tickets(block.tickets);
    return;
  }
  block.held_instances.swap(block.instances);
  block.held_tags.swap(block.tags);
  block.held_tickets.swap(block.tickets);
  block.held_buildings = block.buildings;
}

u32 PlacementTiles::count_pairs(const TileBlock& block) const noexcept {
  u64 pairs = 0;
  for (const renderer::SceneInstance& instance : block.instances) {
    if (instance.mesh < scene_->parts.size()) {
      pairs += scene_->parts[instance.mesh].cluster_count;
    } else {
      pairs += meshes_.clusters(meshes_.slot_of(instance.mesh));
    }
  }
  return pairs > renderer::k_max_pairs ? renderer::k_max_pairs + 1u : static_cast<u32>(pairs);
}

// The tile's last build is what it draws from here: every mesh it names is resident (or it names
// none). What it drew before goes, with its meshes' references; a new version, never one a tile
// held before (W81), tells the renderer the tile changed without it reading an instance.
void PlacementTiles::finalize(TileBlock& block) {
  release_tickets(block.held_tickets);
  block.held_instances.clear();
  block.held_tags.clear();
  block.held_buildings = 0;
  block.waiting = false;
  block.starved = false;
  block.pairs = count_pairs(block);
  block.version = ++next_version_;
}

u64 PlacementTiles::room_bytes() const noexcept {
  const auto room = [](const TileBlock& block) {
    return u64{block.instances.capacity() + block.held_instances.capacity()} *
               sizeof(renderer::SceneInstance) +
           u64{block.tags.capacity() + block.held_tags.capacity()} * sizeof(u8);
  };
  u64 bytes = 0;
  for (u32 i = 0; i < spare_tiles_.size(); ++i)
    bytes += room(spare_tiles_.value_at(i));
  for (u32 i = 0; i < tiles_.size(); ++i)
    bytes += room(tiles_.value_at(i));
  for (const TileBlock& block : free_blocks_)
    bytes += room(block);
  return bytes;
}

bool PlacementTiles::withheld(TileCoord tile) const noexcept {
  const TileBlock* block = tiles_.find_value(tile_key(tile));
  return block != nullptr && block->withheld;
}

void PlacementTiles::tile_ranges(Vector<TileRange>& out) const {
  out.clear();
  u32 first = 0;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const TileBlock& block = tiles_.value_at(i);
    if (block.withheld) continue;
    // What it draws: its last build, or what it held while that build waits for its meshes.
    const std::span<const renderer::SceneInstance> drawn = block.drawn();
    if (drawn.empty()) continue;  // a new tile whose first build still waits draws nothing
    const u32 count = static_cast<u32>(drawn.size());
    out.push_back(
        TileRange{tiles_.key_at(i), first, count, block.ring, drawn.data(), block.version});
    first += count;
  }
}

// The drawn tiles' instances and tags in tile order: what a commit hands an instance sink, and
// what `tail()` answers after a commit that handed a run sink the tiles instead (W81).
void PlacementTiles::write_tail() const {
  tail_.clear();
  tail_tags_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const TileBlock& block = tiles_.value_at(i);
    if (block.withheld) continue;
    for (const renderer::SceneInstance& instance : block.drawn())
      tail_.push_back(instance);
    for (const u8 tag : block.waiting ? block.held_tags : block.tags)
      tail_tags_.push_back(tag);
  }
  tail_stale_ = false;
}

std::span<const renderer::SceneInstance> PlacementTiles::tail() const {
  if (tail_stale_) write_tail();
  return {tail_.data(), tail_.size()};
}

std::span<const u8> PlacementTiles::tail_tags() const {
  if (tail_stale_) write_tail();
  return {tail_tags_.data(), tail_tags_.size()};
}

// The tile's placements in `ring`'s representation: every entry's, in the scene's order, each
// exactly what the whole read put on the tile. False when a generator refused the tile (a kit whose
// footprints do not fit it); what did build is kept.
bool PlacementTiles::build(TileCoord tile, u8 ring, TileBlock& out, u32 entries, bool for_query) {
  const i64 build_started = time::monotonic_ns();
  out.ring = ring;
  if (!for_query) out.due = out.fresh = false;  // its instances are its ring's from here
  // With room for a tile's meshes, what the tile draws now is kept while the build waits for its
  // own (`stage`); without, the build replaces it in place, as it always did.
  const bool staged = !for_query && meshes_.enabled();
  if (staged) stage(out);
  out.instances.clear();
  out.tags.clear();
  out.buildings = 0;
  out.stood = for_query ? (scene_->terrain.enabled ? scene_->terrain.time_s : 0.0) : stand_time_;
  const scene_gen::TileCoord at{tile.x, tile.z};
  bool ok = true;
  // Each entry is handed the footprints of the entries before it on this tile (ADR-0046 decision
  // 4, as built): the tile is a function of the entries, the seed, the tile, the ground and those.
  earlier_.clear();
  const u32 run = entries < entries_.size() ? entries : entries_.size();
  for (u32 e = 0; e < run; ++e) {
    Entry& entry = *entries_[e];
    if (entry.generator->occupies != nullptr && !entry.generator->occupies(entry.state, at))
      continue;
    scene_gen::Placements& placed = entry.placed;
    placed.clear();
    std::string why;
    const i64 start = time::monotonic_ns();
    if (!entry.generator->tile(entry.state, at, context_for(entry, ring, for_query), placed,
                               &why)) {
      ok = false;
      ENGINE_LOG_WARN(log_world_placements, "a tile's placements were not made",
                      log::field("entry", entry.source->where), log::field("x", tile.x),
                      log::field("z", tile.z), log::field("error", why));
      continue;
    }
    if (placed.things == 0 && placed.instances.empty()) continue;  // nothing on this tile
    const Vector<u32>& meshes = entry.source->meshes;
    // **The meshes the tile handed** (scene_gen.md, "A tile's meshes"): each named in the table
    // of them (`TileMeshes`), which builds one the first time a tile names it and holds it while
    // any tile does. A query's tile names none: it is a function of the scene alone, and a tile's
    // mesh is resident only while a held tile draws it.
    handed_.clear();
    for (const scene_gen::PlacementMesh& mesh : placed.meshes) {
      u32 slot = TileMeshes::k_none;
      if (!for_query) {
        std::string refused;
        slot = meshes_.acquire(mesh, entry.source->dir, &refused);
        if (slot == TileMeshes::k_none) {
          ok = false;
          // Once a streamed entry and a few times a run, not once a tile: a world whose host made
          // no room would otherwise say it for every tile of every ring.
          if (mesh_refusals_logged_ < 8) {
            ++mesh_refusals_logged_;
            ENGINE_LOG_WARN(log_world_placements, "a tile's mesh has no room",
                            log::field("entry", entry.source->where), log::field("x", tile.x),
                            log::field("z", tile.z), log::field("error", refused));
          }
        } else {
          out.tickets.push_back(slot);
        }
      }
      handed_.push_back(slot);
    }
    for (const scene_gen::Placement& p : placed.instances) {
      // One list: the entry's resident meshes, then this tile's own.
      u32 scene_mesh = ~0u;
      f32 reach = 0.0f;
      if (p.mesh < meshes.size()) {
        scene_mesh = meshes[p.mesh];
        reach = scene_mesh < radius_of_.size() ? radius_of_[scene_mesh] : 0.0f;
      } else if (p.mesh - meshes.size() < placed.meshes.size()) {
        const u32 k = p.mesh - meshes.size();
        reach = placed.meshes[k].reach;  // the generator's word: nothing is built yet
        if (!for_query) {
          if (handed_[k] == TileMeshes::k_none) continue;  // refused above: not drawn
          scene_mesh = meshes_.scene_mesh(handed_[k]);
        }
      } else {
        ok = false;
        ENGINE_LOG_WARN(log_world_placements, "a tile's placement names a mesh it did not hold",
                        log::field("entry", entry.source->where), log::field("mesh", p.mesh));
        continue;
      }
      // At the placement's `WorldPos`, with its rotation and scale (ADR-0053): the renderer adds
      // the mesh's fit to it in f64 (`renderer::instance_translation`).
      renderer::SceneInstance instance;
      instance.mesh = scene_mesh;
      instance.transform = p.turn();
      instance.origin = p.position;
      instance.handoff = p.handoff;  // the near and far forms' hand-off (G136)
      // The whole read's place, for an entry whose ids name it (the `authored` entry's): what a
      // swaying tree's phase is hashed from, so it sways as it would loaded, whenever its tile came
      // (renderer.md, "Streamed trees sway").
      if (entry.source->motion_key_base != renderer::k_motion_key_load && p.id != 0)
        instance.motion_key = entry.source->motion_key_base + (p.id - 1);
      out.instances.push_back(instance);
      out.tags.push_back(p.tag);
      if (e + 1 < entries_.size()) {
        const f32 scale = std::max(std::fabs(p.scale.x), std::fabs(p.scale.z));
        earlier_.push_back(scene_gen::Footprint{scene_gen::nearest_mm(p.position.x),
                                                scene_gen::nearest_mm(p.position.z),
                                                static_cast<f64>(reach * scale)});
      }
    }
    if (for_query) {
      stats_.query_ns += time::monotonic_ns() - start;
    } else {
      const u32 kind = placed.kind < k_placement_kinds ? placed.kind : k_placement_kinds - 1;
      stats_.build_ns[kind] += time::monotonic_ns() - start;
      ++stats_.built[kind];
    }
    out.buildings += placed.things;
  }
  if (for_query) {
    out.pairs = 0;  // never drawn: nothing to budget
    return ok;
  }
  // **A build that names a mesh not yet resident waits** (world.md, "A tile's meshes: residency by
  // reference"): the tile goes on drawing what it held, with the pairs and the version of that,
  // until a commit has every mesh in (`admit_meshes`). Any other build is what the tile draws at
  // once — a new version for its new instances, never one a tile held before (W81), which is what
  // tells the renderer a tile no update rebuilt is the block it holds without reading it — and
  // its pairs are counted from the meshes it names, the scene's and the table's.
  bool wait = false;
  for (const u32 slot : out.tickets)
    wait = wait || !meshes_.resident(slot);
  if (wait) {
    out.waiting = true;
    if (out.held_instances.empty()) out.pairs = 0;  // it draws nothing meanwhile
  } else {
    finalize(out);
  }
  ++stats_.tiles_built;
  stats_.tiles_ns += time::monotonic_ns() - build_started;
  return ok;
}

// ---- queries
// --------------------------------------------------------------------------------------

u32 PlacementTiles::entries_for(std::string_view generator) const noexcept {
  u32 through = 0;
  for (u32 e = 0; e < entries_.size(); ++e) {
    if (entries_[e]->source->generator == generator) through = e + 1;
  }
  return through;
}

std::span<const renderer::SceneInstance> PlacementTiles::query_tile(TileCoord tile, u32 entries) {
  const u32 want = entries < entries_.size() ? entries : entries_.size();
  const u64 key = tile_key(tile);
  QueryTile* cached = query_tiles_.find_value(key);
  if (cached != nullptr && cached->entries >= want)
    return {cached->instances.data(), cached->instances.size()};
  // The innermost ring's representation (ring 0), the scene's own ground: a function of the scene
  // and the tile. What did not build is logged by `build` and left out, as a streamed tile's is.
  ++stats_.queried;
  (void)build(tile, 0, query_scratch_, want, true);
  QueryTile& slot = query_tiles_[key];
  slot.entries = want;
  slot.instances.assign(query_scratch_.instances.begin(), query_scratch_.instances.end());
  return {slot.instances.data(), slot.instances.size()};
}

void PlacementTiles::trim_query_tiles(TileCoord centre, i32 reach) {
  for (u32 i = 0; i < query_tiles_.size();) {
    const TileCoord t = tile_of_key(query_tiles_.key_at(i));
    const i32 dx = t.x > centre.x ? t.x - centre.x : centre.x - t.x;
    const i32 dz = t.z > centre.z ? t.z - centre.z : centre.z - t.z;
    if (dx > reach || dz > reach) {
      const u64 key = query_tiles_.key_at(i);  // a copy: erase moves the element it names
      query_tiles_.erase(key);
    } else {
      ++i;
    }
  }
}

// A tile activated or moved: its placements in the ring's representation. Whether it is drawn is
// the budget's, decided at the commit over every tile held (`admit`), not here: deciding per event
// would give the budget to whichever tiles come first in tile order, and a first fill would then
// refuse a building beside the camera for one at the horizon.
u32 PlacementTiles::restand(f64 surface_time_s, u32 budget) {
  if (scene_ == nullptr || !scene_->terrain.enabled || entries_.empty() || budget == 0) return 0;
  if (surface_time_s != stand_time_) {
    // The surface where it stands now: the terrain's own function at that time, a provider made
    // once a time the host hands over (the dunes' is a closed form of time; terrain.md).
    renderer::TerrainDesc at = scene_->terrain;
    at.time_s = surface_time_s;
    stand_ = std::make_unique<renderer::TerrainSampler>(at);
    stand_time_ = surface_time_s;
  }
  // The tiles still standing on another time, innermost ring first, then nearest, then in tile
  // order — the order the pair budget draws them in — so a frame's budget goes where it is seen.
  ranks_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (tiles_.value_at(i).stood == stand_time_) continue;
    if (tiles_.value_at(i).due) continue;  // built at the surface's time when its turn comes
    Rank rank;
    rank.ring = tiles_.value_at(i).ring;
    rank.score = ring_ != nullptr ? ring_->score_of(tile_of_key(tiles_.key_at(i))) : 0.0f;
    rank.key = tiles_.key_at(i);
    rank.index = i;
    ranks_.push_back(rank);
  }
  if (ranks_.empty()) return 0;
  std::sort(ranks_.begin(), ranks_.end(), [](const Rank& a, const Rank& b) {
    if (a.ring != b.ring) return a.ring < b.ring;
    if (a.score != b.score) return a.score < b.score;
    return a.key < b.key;
  });
  const i64 start = time::monotonic_ns();
  u32 built = 0;
  for (const Rank& rank : ranks_) {
    if (built >= budget) break;
    TileBlock& held = tiles_.value_at(rank.index);
    if (!build(tile_of_key(rank.key), held.ring, held)) ++stats_.failures;
    ++built;
  }
  stats_.restood += built;
  stats_.restand_ns += time::monotonic_ns() - start;
  commit_tail();  // the update's own commit built its budget's due tiles already
  return built;
}

bool PlacementTiles::take(const TileEvent& event) {
  const u64 key = tile_key(event.tile);
  TileBlock* held = tiles_.find_value(key);
  // A tile no entry occupies is never built: its build would come out empty and be let go.
  if (held == nullptr && !occupied(event.tile)) return true;
  if (build_budget_ != 0) {
    // **Built at a commit, within its budget** (W82, `build_due`): a held tile keeps drawing what
    // it holds until then, and a new one is held empty and drawn as nothing.
    if (held != nullptr) {
      held->ring = event.to;
      held->due = true;
      ++stats_.deferred;
      return true;
    }
    ++stats_.deferred;
    TileBlock block;
    if (TileBlock* spare = spare_tiles_.find_value(key)) {
      block = std::move(*spare);
    } else if (!free_blocks_.empty()) {
      block = std::move(free_blocks_.back());
      free_blocks_.pop_back();
    }
    block.instances.clear();
    block.tags.clear();
    block.buildings = 0;
    block.pairs = 0;
    block.ring = event.to;
    block.withheld = false;
    block.due = true;
    block.fresh = true;
    tiles_.insert_or_assign(key, std::move(block));
    return true;
  }
  if (held != nullptr) {
    const bool built = build(event.tile, event.to, *held);
    if (!built) ++stats_.failures;
    if (held->instances.empty()) {
      recycle(key, std::move(*held));
      tiles_.erase(key);
    }
    return built;
  }
  TileBlock block;
  if (TileBlock* spare = spare_tiles_.find_value(key)) {
    block = std::move(*spare);
  } else if (!free_blocks_.empty()) {
    block = std::move(free_blocks_.back());
    free_blocks_.pop_back();
  }
  const bool built = build(event.tile, event.to, block);
  if (!built) ++stats_.failures;
  if (block.instances.empty()) {
    recycle(key, std::move(block));
    return built;
  }
  block.withheld = false;
  tiles_.insert_or_assign(key, std::move(block));
  return built;
}

void PlacementTiles::recycle(u64 key, TileBlock&& block) {
  // Its meshes' references go with it, the build's and what it drew: a mesh no held tile names is
  // let go at a later commit, after the tail no longer names it (`TileMeshes::end_commit`).
  release_tickets(block.tickets);
  release_tickets(block.held_tickets);
  block.held_instances.clear();
  block.held_tags.clear();
  block.held_buildings = 0;
  block.waiting = false;
  block.starved = false;
  block.instances.clear();
  block.tags.clear();
  if (TileBlock* spare = spare_tiles_.find_value(key)) {
    *spare = std::move(block);
  } else if (block.instances.capacity() != 0 || block.tags.capacity() != 0) {
    free_blocks_.push_back(std::move(block));
  }
}

bool PlacementTiles::activate(void* context, const TileEvent& event) {
  return static_cast<PlacementTiles*>(context)->take(event);
}

bool PlacementTiles::change_ring(void* context, const TileEvent& event) {
  auto* self = static_cast<PlacementTiles*>(context);
  // Only when some entry on the tile draws the two rings differently (the ruins: sections with
  // debris to walls only is a change, and so is anything to or from blocks); two rings that draw
  // the same need nothing built again.
  if (!self->redraws(event.tile, event.from, event.to)) {
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
  self->recycle(key, std::move(*held));
  self->tiles_.erase(key);
  ++self->stats_.dropped;
}

// Which held tiles the tail draws: by ring, innermost first, then by the ring's score — the
// nearest first — then in tile order, admitted while the scene stays within its pair budget and
// none after the first that does not fit, so every tile drawn outranks every tile withheld. A
// function of the tiles held, their rings and the observers, like the ring itself.
// The due tiles a commit builds (W82): the first `build_budget_` of them in the pair budget's order
// — innermost ring, nearest, tile order — or all of them with no budget. A build that puts nothing
// on its tile lets the tile go, as an event's build does.
void PlacementTiles::build_due() {
  ranks_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (!tiles_.value_at(i).due) continue;
    const u64 key = tiles_.key_at(i);
    Rank rank;
    rank.ring = tiles_.value_at(i).ring;
    rank.score = ring_ != nullptr ? ring_->score_of(tile_of_key(key)) : 0.0f;
    rank.key = key;
    rank.index = i;
    ranks_.push_back(rank);
  }
  if (ranks_.empty()) return;
  std::sort(ranks_.begin(), ranks_.end(), [](const Rank& a, const Rank& b) {
    if (a.ring != b.ring) return a.ring < b.ring;
    if (a.score != b.score) return a.score < b.score;
    return a.key < b.key;
  });
  const u32 count =
      build_budget_ == 0 || build_budget_ > ranks_.size() ? ranks_.size() : build_budget_;
  emptied_.clear();
  for (u32 r = 0; r < count; ++r) {
    TileBlock& block = tiles_.value_at(ranks_[r].index);
    if (!build(tile_of_key(ranks_[r].key), block.ring, block)) ++stats_.failures;
    if (block.instances.empty()) emptied_.push_back(ranks_[r].key);
  }
  for (const u64 key : emptied_) {
    TileBlock* block = tiles_.find_value(key);
    if (block == nullptr) continue;
    recycle(key, std::move(*block));
    tiles_.erase(key);
  }
}

void PlacementTiles::admit() {
  ranks_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const u64 key = tiles_.key_at(i);
    // A tile held and never built is not drawn and takes nothing of the budget (W82).
    if (tiles_.value_at(i).fresh) {
      tiles_.value_at(i).withheld = true;
      continue;
    }
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
  const u32 fixed = config_.fixed_pairs != 0 ? config_.fixed_pairs : scene_->pair_count;
  u64 pairs = fixed;
  u64 live_pairs = 0;
  bool full = false;
  u32 withheld = 0;
  u32 newly = 0;
  u8 nearest = k_inactive;
  for (const Rank& rank : ranks_) {
    TileBlock& block = tiles_.value_at(rank.index);
    const u32 reserved = renderer::reserved_tile_pairs(block.pairs);
    if (!full && pairs + reserved <= config_.max_pairs) {
      pairs += reserved;
      live_pairs += block.pairs;
      block.withheld = false;
      continue;
    }
    full = true;
    ++withheld;
    newly += block.withheld ? 0u : 1u;
    nearest = rank.ring < nearest ? rank.ring : nearest;
    block.withheld = true;
  }
  tail_pairs_ = static_cast<u32>(live_pairs);
  if (withheld != 0 && withheld != stats_.withheld) {
    ENGINE_LOG_WARN(log_world_placements, "the pair budget withholds tiles' placements",
                    log::field("withheld", withheld), log::field("nearest_ring", nearest),
                    log::field("pairs", pairs), log::field("budget", config_.max_pairs));
  }
  stats_.refused += newly;
  stats_.withheld = withheld;
}

// The farthest drawn tile that names a mesh, when it is farther than `wanting`: its meshes'
// references go (so what only it named can be let go for room) and it is due again — built at a
// later commit, when it will wait in its turn. The order is the pair budget's, so a tile is only
// ever displaced by one that outranks it, and nothing swings back.
bool PlacementTiles::yield_farthest(const Rank& wanting) {
  u32 farthest = ~0u;
  Rank far_rank;
  for (u32 i = 0; i < tiles_.size(); ++i) {
    const TileBlock& block = tiles_.value_at(i);
    if (block.waiting || block.fresh || block.tickets.empty()) continue;
    Rank rank;
    rank.ring = block.ring;
    rank.key = tiles_.key_at(i);
    rank.score = ring_ != nullptr ? ring_->score_of(tile_of_key(rank.key)) : 0.0f;
    rank.index = i;
    const auto after = [](const Rank& a, const Rank& b) {
      if (a.ring != b.ring) return a.ring > b.ring;
      if (a.score != b.score) return a.score > b.score;
      return a.key > b.key;
    };
    if (!after(rank, wanting)) continue;
    if (farthest == ~0u || after(rank, far_rank)) {
      farthest = i;
      far_rank = rank;
    }
  }
  if (farthest == ~0u) return false;
  TileBlock& block = tiles_.value_at(farthest);
  release_tickets(block.tickets);
  block.instances.clear();
  block.tags.clear();
  block.buildings = 0;
  block.pairs = 0;
  block.version = ++next_version_;
  block.due = true;
  block.fresh = true;  // it draws nothing until it is built again and its meshes are in
  ++stats_.mesh_yielded;
  return true;
}

// **A commit's admissions** (world.md, "A tile's meshes: residency by reference"): the tiles whose
// last build waits, in the pair budget's order — innermost ring, nearest, tile order — each asked
// for its meshes in turn. A mesh the room has no run for is made room for by what nobody names
// (the lingering meshes, longest unnamed first) and then by the farthest drawn tile past this one;
// when neither is left the tile is the room's first withheld, and every tile after it in the order
// is withheld with it, so the room goes to the nearest tiles whatever order they arrived in. A
// tile whose meshes are all in draws its build from this commit on.
void PlacementTiles::admit_meshes() {
  stats_.mesh_waiting = 0;
  stats_.mesh_starved = 0;
  if (!meshes_.enabled()) return;
  waiting_.clear();
  for (u32 i = 0; i < tiles_.size(); ++i) {
    if (!tiles_.value_at(i).waiting) continue;
    Rank rank;
    rank.ring = tiles_.value_at(i).ring;
    rank.key = tiles_.key_at(i);
    rank.score = ring_ != nullptr ? ring_->score_of(tile_of_key(rank.key)) : 0.0f;
    rank.index = i;
    waiting_.push_back(rank);
  }
  if (waiting_.empty()) return;
  std::sort(waiting_.begin(), waiting_.end(), [](const Rank& a, const Rank& b) {
    if (a.ring != b.ring) return a.ring < b.ring;
    if (a.score != b.score) return a.score < b.score;
    return a.key < b.key;
  });
  bool full = false;
  u32 starved = 0;
  u32 newly = 0;
  emptied_.clear();
  for (const Rank& rank : waiting_) {
    // By key: a yield above this point changed no index (nothing is erased until the end).
    TileBlock& block = tiles_.value_at(rank.index);
    bool ready = true;
    bool refused = false;
    for (u32 t = 0; t < block.tickets.size();) {
      const u32 slot = block.tickets[t];
      TileMeshes::Admit result =
          full && !meshes_.resident(slot) ? TileMeshes::Admit::no_room : meshes_.admit(slot);
      while (result == TileMeshes::Admit::no_room && !full) {
        if (!meshes_.evict_lingering() && !yield_farthest(rank)) break;
        result = meshes_.admit(slot);
      }
      if (result == TileMeshes::Admit::failed) {
        // Its mesh could not be built (said once, by the table): the tile draws the rest.
        const u32 scene_mesh = meshes_.scene_mesh(slot);
        u32 kept = 0;
        for (u32 k = 0; k < block.instances.size(); ++k) {
          if (block.instances[k].mesh == scene_mesh) continue;
          block.instances[kept] = block.instances[k];
          block.tags[kept] = block.tags[k];
          ++kept;
        }
        block.instances.resize(kept);
        block.tags.resize(kept);
        meshes_.release(slot);
        block.tickets.erase(block.tickets.begin() + t);
        ++stats_.failures;
        continue;
      }
      if (result == TileMeshes::Admit::no_room) {
        refused = true;
        full = true;
      }
      ready = ready && result == TileMeshes::Admit::done;
      ++t;
    }
    if (ready) {
      finalize(block);
      if (block.instances.empty()) emptied_.push_back(rank.key);
      continue;
    }
    ++stats_.mesh_waiting;
    if (refused) {
      ++starved;
      newly += block.starved ? 0u : 1u;
      block.starved = true;
    } else {
      block.starved = false;
    }
  }
  for (const u64 key : emptied_) {
    TileBlock* block = tiles_.find_value(key);
    if (block == nullptr) continue;
    recycle(key, std::move(*block));
    tiles_.erase(key);
  }
  stats_.mesh_starved = starved;
  if (newly != 0) {
    // Which bound bound: the room's clusters (the budget), said with what it holds.
    const TileMeshStats& held = meshes_.stats();
    ENGINE_LOG_WARN(log_world_placements, "the mesh room withholds tiles' meshes",
                    log::field("withheld", starved), log::field("newly", newly),
                    log::field("bound", "renderer.meshes.budget_mib"),
                    log::field("resident_meshes", held.resident),
                    log::field("resident_clusters", held.clusters),
                    log::field("room_clusters", meshes_.config().clusters),
                    log::field("resident_mib", static_cast<f64>(held.bytes) / (1024.0 * 1024.0)));
  }
}

void PlacementTiles::commit(void* context) {
  auto* self = static_cast<PlacementTiles*>(context);
  self->committed_ = true;
  self->build_due();
  // The tiles' meshes, between the builds that name them and the tail that draws them: a whole
  // update waits for every build and admits everything the room has; a budgeted one takes what
  // has finished, within its allowance.
  self->meshes_.begin_commit(self->build_budget_ == 0);
  self->admit_meshes();
  self->commit_tail();
  // After the tail is handed on, so nothing the renderer draws names a mesh this lets go.
  self->meshes_.end_commit();
}

u32 PlacementTiles::end_update() {
  // Nothing due and nothing of the tiles' meshes outstanding: a tile that waits for a mesh, a
  // build not collected, or a mesh nobody names whose linger a commit counts (W82's reason, for
  // the meshes: a camera that stopped touches no consumer, and its tiles' meshes still arrive).
  const TileMeshStats& meshes = meshes_.stats();
  const bool meshes_due = stats_.mesh_waiting != 0 || meshes.building != 0 || meshes.waiting != 0 ||
                          meshes.lingering != 0;
  if (committed_ || (stats_.pending == 0 && !meshes_due)) return 0;
  const u64 before = stats_.tiles_built;
  commit(this);
  return static_cast<u32>(stats_.tiles_built - before);
}

void PlacementTiles::commit_tail() {
  PlacementTiles* self = this;
  // Every active tile's instances, in tile order: the map is sorted by key, so a scene streamed to
  // a set of tiles has one tail — one numbering of its pairs — whatever order the tiles came in.
  const i64 admit_start = time::monotonic_ns();
  self->admit();
  const i64 tail_start = time::monotonic_ns();
  self->stats_.admit_ns += tail_start - admit_start;
  u32 buildings = 0;
  u32 instances = 0;
  PlacementTilesStats& stats = self->stats_;
  for (u32 r = 0; r < k_max_rings; ++r) {
    stats.ring_tiles[r] = stats.ring_instances[r] = stats.ring_pairs[r] = 0;
    stats.ring_withheld[r] = 0;
  }
  u32 pending = 0;
  for (u32 i = 0; i < self->tiles_.size(); ++i) {
    const TileBlock& block = self->tiles_.value_at(i);
    const u32 ring = block.ring < k_max_rings ? block.ring : k_max_rings - 1;
    pending += block.due ? 1u : 0u;
    if (block.fresh) continue;  // not built yet: neither drawn nor withheld (W82)
    if (block.withheld) {
      ++stats.ring_withheld[ring];
      continue;
    }
    // What it draws: its last build, or what it held while that build waits for its meshes.
    const u32 drawn = static_cast<u32>(block.drawn().size());
    ++stats.ring_tiles[ring];
    stats.ring_instances[ring] += drawn;
    stats.ring_pairs[ring] += block.pairs;
    buildings += block.drawn_buildings();
    instances += drawn;
  }
  // **The tiles, not a tail** (W81): a run sink is handed each drawn tile's own instances and
  // version, and nothing is copied — a commit costs its tiles' bookkeeping, and the renderer reads
  // only the tiles whose version it does not hold. Until 2026-10-31 every commit wrote the whole
  // tail and the renderer compared all of it: the v23 forest's flight spent 6.3 s of its 40 s
  // writing it and 2.7 s comparing it (docs/experiments/forest-load-in-2026-10-31.md).
  if (self->run_sink_ != nullptr) {
    self->tile_ranges(self->runs_);
    self->tail_stale_ = true;
  } else {
    self->write_tail();
  }
  self->stats_.buildings = buildings;
  self->stats_.pending = pending;
  self->stats_.instances = instances;
  self->stats_.pairs = self->tail_pairs_;
  self->stats_.tail_ns += time::monotonic_ns() - tail_start;
  if (self->sink_ == nullptr && self->run_sink_ == nullptr) return;
  std::string error;
  const i64 start = time::monotonic_ns();
  const bool ok =
      self->run_sink_ != nullptr
          ? self->run_sink_(self->run_sink_context_,
                            std::span<const TileRange>(self->runs_.data(), self->runs_.size()),
                            &error)
          : self->sink_(
                self->sink_context_,
                std::span<const renderer::SceneInstance>(self->tail_.data(), self->tail_.size()),
                &error);
  const i64 dt = time::monotonic_ns() - start;
  self->stats_.sink_ns += dt;
  self->stats_.max_sink_ns = dt > self->stats_.max_sink_ns ? dt : self->stats_.max_sink_ns;
  ++self->stats_.handed;
  if (!ok) {
    ++self->stats_.failures;
    ENGINE_LOG_ERROR(log_world_placements, "the renderer refused the placements' instances",
                     log::field("instances", self->stats_.instances),
                     log::field("pairs", self->tail_pairs_), log::field("error", error));
  }
}

}  // namespace engine::world
