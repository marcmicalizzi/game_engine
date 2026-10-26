// engine-view's streamed world (world_view.h).
#include "world_view.h"

#include "anim_lod.h"
#include "game_runner.h"  // k_max_placement_query_m: what the query cache is bounded by

#if ENGINE_VIEW_WORLD

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/flythrough.h>
#include <systems/world/scene_consumers.h>
#include <systems/world/tile_ring.h>

#include <algorithm>
#include <cmath>
#include <schemas/world_tiles.h>

namespace engine::view {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view_world, "view.world");

// Tiles of placements stood on the moving sand again an update: a full pass over the held tiles
// takes their count over this many updates, nearest first (scatter.md, "A moving ground").
constexpr u32 k_restand_tiles = 4;

}  // namespace

ViewWorld::~ViewWorld() { close_log(); }

bool ViewWorld::create(const renderer::SceneData& data, renderer::SceneRenderer& renderer,
                       std::string* error, renderer::TerrainTileSet* tiles) {
  data_ = &data;
  renderer_ = &renderer;
  world::RingParams params;
  params.tile_size = data.world.tile_size;
  if (data.world.ring_count > 0) {
    params.ring_count = data.world.ring_count;
    for (u32 r = 0; r < world::k_max_rings; ++r)
      params.radius[r] = r < data.world.ring_count ? data.world.radius[r] : 0.0f;
  }
  params.hysteresis = data.world.hysteresis;
  params.max_activations = world::max_activations_tunable();
  params.max_deactivations = world::max_deactivations_tunable();
  // The world's tiles drawn: the ring the tile set was built for — the scene's block, or the
  // world's defaults for `--terrain-tiles` on a scene with none. **With no streamed placements the
  // ring takes no budget**: the drawn ground's events are a hash map's writes, and what they cost
  // is the renderer's rebuild, which runs on its own worker and takes whatever changed since the
  // last one in one go. The default budget of eight activations an update fell behind the flow of
  // desert-endless's rings at 100 m/s (about ten a frame, counting the tiles moving inwards), and
  // its horizon thinned: 8,971 of 12,900 tiles held after 12 km (renderer.md, "Which tiles").
  // **Nor with streamed placements beside them** (2026-10-12): the placements' cost is the tiles
  // their kinds reach — a scatter layer's `far` leaves every outer ring's tile empty at the cost
  // of a mask (scatter.md, "The budget is the LOD policy") — so the ring that draws to the horizon
  // is not held to a budget a few near tiles would be.
  if (tiles != nullptr) {
    params = world::DrawnTiles::ring_params(tiles->tiles_desc());
    params.max_activations = 0;
    params.max_deactivations = 0;
  }
  const char* why = nullptr;
  if (!world_.configure(params, &why)) {
    if (error != nullptr) *error = std::string("the scene's world rings: ") + why;
    return false;
  }
  placements_config_.tile_size = params.tile_size;
  placements_config_.ring_count = params.ring_count;
  if (data.world.ring_count > 0) {
    for (u32 r = 0; r < data.world.ring_count; ++r)
      placements_config_.ruins[r] = static_cast<world::RingRuins>(data.world.ruins[r]);
  }
  // The drawn ground first: a tile is drawn before anything is placed on it (world.md, "The
  // order"), then what stands on it.
  if (tiles != nullptr) {
    drawn_.create(*tiles);
    world_.add_consumer(drawn_.consumer());
  }
  // The scene's streamed placement entries, whatever generators they name: the reader has already
  // refused a name this executable does not carry, and the consumer finds each by it again.
  if (!data.streamed.empty()) {
    // In the declared order, through the one function that states it; the drawn ground, when the
    // world draws one, is registered above. The pair budget is the renderer's own: with the ray
    // tracing chain, what the device's geometry indices name over the views.
    placements_config_.max_pairs = renderer.scene().pair_limit();
    placements_config_.fixed_pairs = renderer.scene().static_pair_count();
    if (!placements_.create(data, placements_config_, error)) return false;
    placements_.set_sink(&ViewWorld::hand_over, this);
    world::add_scene_consumers(world_, nullptr, &placements_);
    // Each scene mesh's kind for `nearest_placement`: the generator of the streamed entry whose
    // resident meshes it is (`StreamedPlacements::meshes`), by the generator's registered name.
    mesh_kind_.clear();
    kinds_.clear();
    for (const renderer::StreamedPlacements& entry : data.streamed) {
      u32 k = 0;
      while (k < kinds_.size() && kinds_[k] != entry.generator)
        ++k;
      if (k == kinds_.size()) kinds_.push_back(entry.generator);
      for (const u32 mesh : entry.meshes) {
        while (mesh_kind_.size() <= mesh)
          mesh_kind_.push_back(~u32{0});
        mesh_kind_[mesh] = k;
      }
    }
  }
  ENGINE_LOG_INFO(log_view_world, "world", log::field("tile_size", params.tile_size),
                  log::field("rings", params.ring_count), log::field("inner", params.radius[0]),
                  log::field("outer", params.radius[params.ring_count - 1]),
                  log::field("consumers", world_.consumer_count()),
                  log::field("max_activations", params.max_activations));
  return true;
}

// The tail goes over tile by tile: each tile's run of it is a block of its own in the renderer,
// named by the tile's key, so a change costs the renderer the tiles that changed and not the whole
// tail (docs/subsystems/renderer.md, "Instances that come and go"). The ranges are the ruins
// consumer's own, for the tail it is handing over.
bool ViewWorld::hand_over(void* context, std::span<const renderer::SceneInstance> tail,
                          std::string* error) {
  auto* self = static_cast<ViewWorld*>(context);
  self->blocks_.clear();
  if (!self->data_->streamed.empty()) {
    self->placements_.tile_ranges(self->ranges_);
    for (const world::PlacementTiles::TileRange& range : self->ranges_)
      self->blocks_.push_back(
          renderer::DynamicBlock{range.key, range.first, range.count, range.ring});
  }
  // The appended block (game_props.h): after the tiles, under its own key. Both runs are copied
  // into one tail because the renderer takes one span; the copy is the tail's size, kept between
  // hand-overs, and happens only on a frame something changed. With no extra block the tail goes
  // over as it came.
  std::span<const renderer::SceneInstance> whole = tail;
  if (self->extra_ != nullptr && !self->extra_->empty()) {
    self->combined_.clear();
    for (const renderer::SceneInstance& instance : tail)
      self->combined_.push_back(instance);
    for (const renderer::SceneInstance& instance : *self->extra_)
      self->combined_.push_back(instance);
    self->blocks_.push_back(renderer::DynamicBlock{self->extra_key_, static_cast<u32>(tail.size()),
                                                   static_cast<u32>(self->extra_->size())});
    whole =
        std::span<const renderer::SceneInstance>(self->combined_.data(), self->combined_.size());
  }
  // A world started over (a flythrough's repeat) is laid out again from scratch, as its first fill
  // was: without it each repeat would inherit where the last one's tiles ended up, and the repeats'
  // occlusion histories — and so their visible pair counts — would differ where their pictures
  // do not.
  const bool compact = self->compact_next_;
  self->compact_next_ = false;
  return self->renderer_->set_dynamic_instances(
      whole, std::span<const renderer::DynamicBlock>(self->blocks_.data(), self->blocks_.size()),
      error, compact);
}

bool ViewWorld::hand_over_now(std::string* error) {
  if (!valid()) return true;
  const std::span<const renderer::SceneInstance> tail =
      data_->streamed.empty() ? std::span<const renderer::SceneInstance>() : placements_.tail();
  return hand_over(this, tail, error);
}

bool ViewWorld::nearest_placement(std::string_view kind, const WorldPos& from, f64 radius,
                                  WorldPos& out, f64& distance) {
  if (!valid() || !(radius > 0.0)) return false;
  u32 wanted = ~u32{0};
  for (u32 k = 0; k < kinds_.size(); ++k) {
    if (kinds_[k] == kind) wanted = k;
  }
  if (wanted == ~u32{0}) return false;
  const u32 entries = placements_.entries_for(kind);
  // The tiles the radius reaches, one square of them at a time out from the walker's, and in each
  // the instances the scene puts there (`query_tile`): a ruin's pieces, a scatter's rocks, each at
  // `origin + transform.position` (ADR-0053), measured over the ground. Square k's tiles are at
  // least `(k - 1/2) · tile - off` away — the square's inner edge less how far the walker stands
  // from its own tile's centre — so the search stops at the first square that cannot hold anything
  // nearer than the best so far, or anything within the radius at all. Ties go to the first met,
  // and the order is fixed (square by square, then z, then x), so the answer is the scene's.
  const f32 tile_size = placements_config_.tile_size;
  const f64 size = static_cast<f64>(tile_size);
  const i32 reach = static_cast<i32>(std::ceil(radius / size));
  const world::TileCoord centre = world::tile_at(from, tile_size);
  const WorldPos centre_at = world::tile_center(centre, tile_size);
  const f64 off = std::max(std::fabs(from.x - centre_at.x), std::fabs(from.z - centre_at.z));
  f64 best = radius * radius;
  bool found = false;
  const auto visit = [&](i32 dx, i32 dz) {
    const world::TileCoord tile{centre.x + dx, centre.z + dz};
    for (const renderer::SceneInstance& instance : placements_.query_tile(tile, entries)) {
      if (instance.mesh >= mesh_kind_.size() || mesh_kind_[instance.mesh] != wanted) continue;
      const f64 x = instance.origin.x + static_cast<f64>(instance.transform.position.x);
      const f64 z = instance.origin.z + static_cast<f64>(instance.transform.position.z);
      const f64 d2 = (x - from.x) * (x - from.x) + (z - from.z) * (z - from.z);
      if (d2 > best || (found && d2 == best)) continue;
      best = d2;
      found = true;
      out = WorldPos{x, instance.origin.y + static_cast<f64>(instance.transform.position.y), z};
    }
  };
  for (i32 k = 0; k <= reach; ++k) {
    const f64 nearest_possible = k == 0 ? 0.0 : (static_cast<f64>(k) - 0.5) * size - off;
    if (nearest_possible > radius) break;
    if (found && best <= nearest_possible * nearest_possible) break;
    if (k == 0) {
      visit(0, 0);
      continue;
    }
    for (i32 dz = -k; dz <= k; ++dz) {
      if (dz == -k || dz == k) {
        for (i32 dx = -k; dx <= k; ++dx)
          visit(dx, dz);
      } else {
        visit(-k, dz);
        visit(k, dz);
      }
    }
  }
  // The cache keeps the tiles within the runner's cap of the walker and drops the rest: bounded by
  // the cap, whatever radii the module alternates between (the fixture asks 6 m every tick and
  // 400 m once a second; trimming to each query's own reach would regenerate the far tiles every
  // second).
  placements_.trim_query_tiles(centre,
                               static_cast<i32>(std::ceil(k_max_placement_query_m / size)) + 1);
  if (found) distance = std::sqrt(best);
  return found;
}

void ViewWorld::query_stats(u64& generated_tiles, f64& generate_ms) const noexcept {
  generated_tiles = placements_.stats().queried;
  generate_ms = static_cast<f64>(placements_.stats().query_ns) / 1.0e6;
}

// The log is kept in memory and written whole at the end — a line an update is a few hundred bytes,
// a flythrough a few thousand updates — so writing it costs the timed frames nothing. Opening it
// writes the file empty at once, so a path that cannot be written is refused before the run.

bool ViewWorld::open_log(const std::string& path, std::string* error) {
  close_log();
  if (io::write_file(path, std::string_view()) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  log_path_ = path;
  log_text_.clear();
  return true;
}

void ViewWorld::close_log() {
  if (log_path_.empty()) return;
  log_text_ += write_json(schema::to_json(summary()), JsonWriteOptions{.pretty = false});
  log_text_ += '\n';
  if (io::write_file(log_path_, log_text_) != io::Status::Ok) {
    ENGINE_LOG_WARN(log_view_world, "the world log was not written", log::field("path", log_path_));
  }
  log_path_.clear();
  log_text_.clear();
}

bool ViewWorld::update(const renderer::Camera& camera, u64 tick, Mode mode, u32 repeat, u32 frame,
                       bool recorded, std::string* error) {
  if (renderer_ == nullptr) return true;
  if (mode == Mode::Restart) {
    world_.clear(tick);
    compact_next_ = true;
  }
  observers_.clear();  // kept, so a frame's update allocates nothing for it
  observers_.add(camera.position, 1.0f);
  {
    // The world log is the measurement's and not the frame loop's: what it allocates (a line a
    // frame, the ring's times for the summary) a flight leaves out of the frame-loop count.
    const renderer::FlightBookkeeping bookkeeping;
    log_.before(world_);
  }
  world_.update(observers_, tick, mode != Mode::Budgeted);
  // **A moving ground** (scatter.md, "A moving ground"): the sand the renderer draws stands at its
  // surface time, and the placements standing on it follow, a few tiles an update, nearest first
  // (`world::PlacementTiles::restand`). Nothing when the ground does not move.
  if (!data_->streamed.empty() && data_->terrain.enabled) {
    (void)placements_.restand(renderer_->scene().ground_time_s(), k_restand_tiles);
  }
  // A consumer that could not take a tile says so in its own log line and counts it; the frame
  // draws what the scene has, so nothing here fails the run. `error` is for the hook's contract.
  (void)error;
  const renderer::FlightBookkeeping bookkeeping;
  world::TileFrame line = log_.after(world_, repeat, frame, recorded);
  if (log_path_.empty()) return true;
  // What the renderer holds after the update: the load's prefix and the placements' tail.
  line.instances = data_->instances.size();
  line.pairs = data_->pair_count;
  line.buildings = placements_.stats().buildings;
  line.ruin_instances = placements_.stats().instances;
  line.instances += placements_.stats().instances;
  line.pairs += placements_.stats().pairs;
  log_text_ += write_json(schema::to_json(line), JsonWriteOptions{.pretty = false});
  log_text_ += '\n';
  return true;
}

world::TileWorldSummary ViewWorld::summary() const {
  world::TileWorldSummary summary = log_.summary(world_);
  for (u32 r = 0; r < world_.params().ring_count; ++r)
    summary.ruins.push_back(world::ring_ruins_name(placements_config_.ruins[r]));
  // The consumer counts by the representation a generator drew, the generator's own numbering; the
  // summary's columns are the ruins' — laid in blocks (0), assembled in sections or walls (1, 2),
  // which is the scene's `RingRuins` — and a city's tiles, which draw one kind, count as assembled.
  const world::PlacementTilesStats& s = placements_.stats();
  const auto blocks = static_cast<u32>(world::RingRuins::Blocks);
  for (u32 k = 0; k < world::k_placement_kinds; ++k) {
    if (k == blocks) {
      summary.laid += s.built[k];
      summary.lay_ms += static_cast<f64>(s.build_ns[k]) / 1.0e6;
    } else {
      summary.assembled += s.built[k];
      summary.assemble_ms += static_cast<f64>(s.build_ns[k]) / 1.0e6;
    }
  }
  summary.dropped = s.dropped;
  summary.refused = s.refused;
  summary.insert_ms = static_cast<f64>(s.sink_ns) / 1.0e6;
  summary.insert_ms_max = static_cast<f64>(s.max_sink_ns) / 1.0e6;
  summary.handed = s.handed;
  const renderer::GpuScene& scene = renderer_->scene();
  summary.prefix_pairs = scene.static_pair_count();
  summary.slot_pairs = scene.ring_slot_pairs();
  summary.pair_limit = scene.pair_limit();
  summary.tail_pairs = s.pairs;
  for (u32 r = 0; r < world_.params().ring_count && r < world::k_max_rings; ++r) {
    summary.ring_tiles.push_back(s.ring_tiles[r]);
    summary.ring_instances.push_back(s.ring_instances[r]);
    summary.ring_pairs.push_back(s.ring_pairs[r]);
    summary.ring_withheld.push_back(s.ring_withheld[r]);
  }
  const renderer::AuthoredStats& a = data_->authored;
  summary.authored_streamed = a.streamed;
  summary.authored_kept = a.kept_actors + a.kept_deformed + a.kept_reach + a.kept_paged;
  summary.authored_forms = a.forms;
  summary.authored_form_clusters = a.form_clusters;
  summary.authored_tile_floor = data_->tail_floor_pairs;
  return summary;
}

bool ViewWorld::fly_handovers(const renderer::CameraPath& camera_path, u32 frames,
                              const std::string& path, u32& handovers, std::string* error) {
  handovers = 0;
  std::string out;
  renderer::CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  u64 tick = 0;
  bool ok = true;
  // The world as the timed pass starts it, and a few frames there so the history is a moving one.
  ok = update(renderer::camera_path_frame(camera_path, 0, frames), tick++, Mode::Restart, 0, 0,
              false, error);
  for (u32 w = 0; w < 4 && ok; ++w) {
    renderer::FrameDesc frame;
    frame.camera = renderer::camera_path_frame(camera_path, 0, frames);
    ok = renderer_->render_offscreen(frame, error);
  }
  // A handover is measured one tile at a time: the frame as the update left it, against the same
  // frame with only that tile's instances put back as they were before the update. Everything else
  // the update did — tiles coming in at the outer ring, going at the back, other tiles changing —
  // is in both pictures, so what differs is the one tile's pop and nothing else.
  Vector<renderer::SceneInstance> before_tail;
  Vector<renderer::SceneInstance> after_tail;
  Vector<u8> before_rubble;
  Vector<u8> after_rubble;
  Vector<renderer::SceneInstance> swapped;
  Vector<world::PlacementTiles::TileRange> before_ranges;
  Vector<world::PlacementTiles::TileRange> after_ranges;
  // A tile the pair budget left out of the tail has no range; it would sit where the next one
  // starts, so that is where an empty range of it is.
  auto range_of = [](const Vector<world::PlacementTiles::TileRange>& ranges, u64 key, u32 total) {
    for (const world::PlacementTiles::TileRange& range : ranges) {
      if (range.key == key) return range;
      if (range.key > key) return world::PlacementTiles::TileRange{key, range.first, 0};
    }
    return world::PlacementTiles::TileRange{key, total, 0};
  };
  auto pairs_of = [&](const renderer::SceneInstance* first, u32 count) {
    u32 pairs = 0;
    for (u32 i = 0; i < count; ++i)
      pairs += data_->parts[first[i].mesh].cluster_count;
    return pairs;
  };
  // The renderer keeps each tile's instances in a block of their own, wherever the block is, so a
  // capture's instance slot is turned back into its place in the tail before a tile's pixels are
  // counted, while the scene still holds the tail that drew it. The load's own instances and an
  // empty pixel become `k_no_id`.
  Vector<u32> tail_of_slot;
  auto to_tail = [&](renderer::CapturedFrame& shot) {
    renderer_->scene().tail_indices(tail_of_slot);
    const u32 pixels = shot.width * shot.height;
    for (u32 p = 0; p < pixels; ++p) {
      u32& id = shot.ids[p * renderer::k_id_words];
      if (id == renderer::k_no_id) continue;
      id = id < tail_of_slot.size() && tail_of_slot[id] != ~0u ? tail_of_slot[id]
                                                               : renderer::k_no_id;
    }
  };
  // The blocks of a tail laid out as `ranges` say, with tile `key`'s run `count` long.
  Vector<renderer::DynamicBlock> blocks;
  auto blocks_of = [&](const Vector<world::PlacementTiles::TileRange>& ranges, u64 key, u32 count) {
    blocks.clear();
    u32 first = 0;
    bool placed = false;
    for (const world::PlacementTiles::TileRange& range : ranges) {
      if (!placed && range.key >= key) {
        if (count > 0) blocks.push_back(renderer::DynamicBlock{key, first, count});
        first += count;
        placed = true;
        if (range.key == key) continue;
      }
      blocks.push_back(renderer::DynamicBlock{range.key, first, range.count});
      first += range.count;
    }
    if (!placed && count > 0) blocks.push_back(renderer::DynamicBlock{key, first, count});
    return std::span<const renderer::DynamicBlock>(blocks.data(), blocks.size());
  };
  for (u32 f = 0; f < frames && ok; ++f) {
    renderer::FrameDesc frame;
    frame.camera = renderer::camera_path_frame(camera_path, f, frames);
    frame.frame_index = f;
    before_tail.assign(placements_.tail().begin(), placements_.tail().end());
    before_rubble.assign(placements_.tail_tags().begin(), placements_.tail_tags().end());
    placements_.tile_ranges(before_ranges);
    ok = update(frame.camera, tick++, Mode::Budgeted, 0, f, false, error);
    if (!ok) break;
    bool changed = false;
    for (const world::TileEvent& event : world_.last_events())
      changed = changed || is_handover(event);
    if (!changed) {
      ok = renderer_->render_offscreen(frame, error);
      continue;
    }
    // Each picture is drawn once before it is captured, so the capture's occlusion history is its
    // own tail's at this camera and its visible pairs are comparable with the other's.
    renderer::CapturedFrame after;
    ok = renderer_->render_offscreen(frame, error) &&
         renderer_->capture(frame, channels, after, error);
    const u32 visible_after = renderer_->stats().visible_pairs();
    after_tail.assign(placements_.tail().begin(), placements_.tail().end());
    after_rubble.assign(placements_.tail_tags().begin(), placements_.tail_tags().end());
    placements_.tile_ranges(after_ranges);
    if (ok) to_tail(after);
    // A distance for the log line, from the eye in f64 (ADR-0053).
    const WorldPos eye = frame.camera.position;
    for (const world::TileEvent& event : world_.last_events()) {
      if (!ok) break;
      if (!is_handover(event)) continue;
      const u64 key = world::tile_key(event.tile);
      const world::PlacementTiles::TileRange was = range_of(before_ranges, key, before_tail.size());
      const world::PlacementTiles::TileRange now = range_of(after_ranges, key, after_tail.size());
      // The update's tail with this tile's block as it was: the blocks before it are the same
      // length in both, so the tile starts at `now.first` either way.
      swapped.clear();
      for (u32 i = 0; i < now.first; ++i)
        swapped.push_back(after_tail[i]);
      for (u32 i = 0; i < was.count; ++i)
        swapped.push_back(before_tail[was.first + i]);
      for (u32 i = now.first + now.count; i < after_tail.size(); ++i)
        swapped.push_back(after_tail[i]);
      renderer::CapturedFrame before;
      ok = renderer_->set_dynamic_instances(
               std::span<const renderer::SceneInstance>(swapped.data(), swapped.size()),
               blocks_of(after_ranges, key, was.count), error) &&
           renderer_->render_offscreen(frame, error) &&
           renderer_->capture(frame, channels, before, error);
      if (!ok) break;
      to_tail(before);
      const u32 visible_before = renderer_->stats().visible_pairs();
      // What popped: coverage that came or went, and depths that moved by more than a thousandth
      // of the nearer (reversed-Z: larger is nearer) — binned by how far, the last three bins
      // being geometry that moved rather than relief on a surface that stayed; colours that
      // changed by more than 8 of 255; the pixels the tile's own instances drew; and which of the
      // changed pixels show the tile's rubble in either picture.
      constexpr f32 k_bin_edges[4] = {3.0e-3f, 1.0e-2f, 3.0e-2f, 1.0e-1f};
      u32 coverage = 0;
      u32 depth = 0;
      u32 moved = 0;
      u32 bins[5] = {0, 0, 0, 0, 0};
      u32 rubble = 0;
      u32 color = 0;
      u32 tile_before = 0;
      u32 tile_after = 0;
      const u32 pixels = after.width * after.height;
      for (u32 p = 0; p < pixels; ++p) {
        const f32 a = before.depth[p];
        const f32 b = after.depth[p];
        bool popped = false;
        if ((a == 0.0f) != (b == 0.0f)) {
          ++coverage;
          popped = true;
        } else if (a != 0.0f && std::fabs(a - b) > 1.0e-3f * std::max(a, b)) {
          ++depth;
          popped = true;
          const f32 diff = std::fabs(a - b);
          const f32 nearer = std::max(a, b);
          u32 bin = 0;
          while (bin < 4 && diff > k_bin_edges[bin] * nearer)
            ++bin;
          ++bins[bin];
          moved += bin >= 2 ? 1u : 0u;
        }
        const u8* ca = before.color.data() + u64{p} * 4u;
        const u8* cb = after.color.data() + u64{p} * 4u;
        bool shade = false;
        for (u32 k = 0; k < 3; ++k)
          shade = shade || (ca[k] > cb[k] ? ca[k] - cb[k] : cb[k] - ca[k]) > 8;
        color += shade ? 1u : 0u;
        const u32 ia = before.ids[u64{p} * renderer::k_id_words];
        const u32 ib = after.ids[u64{p} * renderer::k_id_words];
        // Places in the tail each picture drew (`to_tail`): in the swapped tail the tile's run
        // starts at `now.first` as it does in the update's.
        const bool in_before =
            ia != renderer::k_no_id && ia >= now.first && ia < now.first + was.count;
        const bool in_after =
            ib != renderer::k_no_id && ib >= now.first && ib < now.first + now.count;
        tile_before += in_before ? 1u : 0u;
        tile_after += in_after ? 1u : 0u;
        if (popped && ((in_before && before_rubble[was.first + (ia - now.first)] != 0) ||
                       (in_after && after_rubble[ib] != 0))) {
          ++rubble;
        }
      }
      world::TileHandover line;
      line.frame = f;
      line.x = event.tile.x;
      line.z = event.tile.z;
      line.from_ring = event.from;
      line.to_ring = event.to;
      line.from = drawn_as(event.from);
      line.to = drawn_as(event.to);
      const WorldPos c = world::tile_center(event.tile, world_.params().tile_size);
      line.distance = static_cast<f32>(
          std::sqrt((c.x - eye.x) * (c.x - eye.x) + (c.z - eye.z) * (c.z - eye.z)));
      line.tile_before = tile_before;
      line.tile_after = tile_after;
      line.coverage_changed = coverage;
      line.depth_changed = depth;
      line.depth_moved = moved;
      line.color_changed = color;
      line.rubble_changed = rubble;
      for (const u32 n : bins)
        line.depth_bins.push_back(n);
      line.visible_before = visible_before;
      line.visible_after = visible_after;
      line.instances_before = was.count;
      line.instances_after = now.count;
      line.pairs_before = pairs_of(before_tail.data() + was.first, was.count);
      line.pairs_after = pairs_of(after_tail.data() + now.first, now.count);
      out += write_json(schema::to_json(line), JsonWriteOptions{.pretty = false});
      out += '\n';
      ++handovers;
    }
    // The update's own tail again, tile by tile: every block but the swapped tile's is where it
    // was.
    blocks.clear();
    for (const world::PlacementTiles::TileRange& range : after_ranges)
      blocks.push_back(renderer::DynamicBlock{range.key, range.first, range.count});
    ok = ok && renderer_->set_dynamic_instances(
                   std::span<const renderer::SceneInstance>(after_tail.data(), after_tail.size()),
                   std::span<const renderer::DynamicBlock>(blocks.data(), blocks.size()), error);
  }
  if (ok && io::write_file(path, out) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return ok;
}

bool ViewWorld::is_handover(const world::TileEvent& event) const noexcept {
  if (event.kind != world::TileEventKind::ChangeRing) return false;
  if (!placements_.occupied(event.tile)) return false;
  // Whatever the entries on the tile draw the two rings as: the ruins' blocks, sections and walls,
  // and a scene's own trees in the distance form each ring allows (renderer.md, "Authored
  // instances per tile").
  return placements_.redraws(event.tile, event.from, event.to);
}

const char* ViewWorld::drawn_as(u8 ring) const noexcept {
  return world::ring_ruins_name(ring < placements_config_.ring_count
                                    ? placements_config_.ruins[ring]
                                    : world::RingRuins::Walls);
}

JsonValue ViewWorld::summary_json() const { return schema::to_json(summary()); }

}  // namespace engine::view

#endif  // ENGINE_VIEW_WORLD
