// engine-view's streamed world (world_view.h).
#include "world_view.h"

#if ENGINE_VIEW_WORLD

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/world/tile_ring.h>

#include <algorithm>
#include <cmath>
#include <schemas/world_tiles.h>

namespace engine::view {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_view_world, "view.world");

}  // namespace

ViewWorld::~ViewWorld() { close_log(); }

bool ViewWorld::create(const renderer::SceneData& data, renderer::SceneRenderer& renderer,
                       std::string* error) {
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
  const char* why = nullptr;
  if (!world_.configure(params, &why)) {
    if (error != nullptr) *error = std::string("the scene's world rings: ") + why;
    return false;
  }
#if ENGINE_WORLD_RUINS
  ruins_config_.tile_size = params.tile_size;
  ruins_config_.ring_count = params.ring_count;
  if (data.world.ring_count > 0) {
    for (u32 r = 0; r < data.world.ring_count; ++r)
      ruins_config_.ruins[r] = static_cast<world::RingRuins>(data.world.ruins[r]);
  }
  if (!data.streamed_ruins.empty()) {
    if (!ruins_.create(data, ruins_config_, error)) return false;
    ruins_.set_sink(&ViewWorld::hand_over, this);
    ruins_.set_ring(&world_.ring());
    world_.add_consumer(ruins_.consumer());
  }
#else
  if (!data.streamed_ruins.empty()) {
    if (error != nullptr) {
      *error = "the scene streams ruins, and this build has no ruins capability";
    }
    return false;
  }
#endif
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
#if ENGINE_WORLD_RUINS
  self->ruins_.tile_ranges(self->ranges_);
  self->blocks_.clear();
  for (const world::RuinsTiles::TileRange& range : self->ranges_)
    self->blocks_.push_back(renderer::DynamicBlock{range.key, range.first, range.count});
  // A world started over (a flythrough's repeat) is laid out again from scratch, as its first fill
  // was: without it each repeat would inherit where the last one's tiles ended up, and the repeats'
  // occlusion histories — and so their visible pair counts — would differ where their pictures
  // do not.
  const bool compact = self->compact_next_;
  self->compact_next_ = false;
  return self->renderer_->set_dynamic_instances(
      tail, std::span<const renderer::DynamicBlock>(self->blocks_.data(), self->blocks_.size()),
      error, compact);
#else
  return self->renderer_->set_dynamic_instances(tail, error);
#endif
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
  sim::ObserverSet observers;
  observers.add(camera.position, 1.0f);
  log_.before(world_);
  world_.update(observers, tick, mode != Mode::Budgeted);
  // A consumer that could not take a tile says so in its own log line and counts it; the frame
  // draws what the scene has, so nothing here fails the run. `error` is for the hook's contract.
  (void)error;
  world::TileFrame line = log_.after(world_, repeat, frame, recorded);
  if (log_path_.empty()) return true;
  // What the renderer holds after the update: the load's prefix and the ruins' tail.
  line.instances = data_->instances.size();
  line.pairs = data_->pair_count;
#if ENGINE_WORLD_RUINS
  line.buildings = ruins_.stats().buildings;
  line.ruin_instances = ruins_.stats().instances;
  line.instances += ruins_.stats().instances;
  line.pairs += ruins_.stats().pairs;
#endif
  log_text_ += write_json(schema::to_json(line), JsonWriteOptions{.pretty = false});
  log_text_ += '\n';
  return true;
}

world::TileWorldSummary ViewWorld::summary() const {
  world::TileWorldSummary summary = log_.summary(world_);
#if ENGINE_WORLD_RUINS
  for (u32 r = 0; r < world_.params().ring_count; ++r)
    summary.ruins.push_back(world::ring_ruins_name(ruins_config_.ruins[r]));
  const world::RuinsTilesStats& s = ruins_.stats();
  summary.assembled = s.assembled;
  summary.laid = s.laid;
  summary.dropped = s.dropped;
  summary.refused = s.refused;
  summary.assemble_ms = static_cast<f64>(s.assemble_ns) / 1.0e6;
  summary.lay_ms = static_cast<f64>(s.lay_ns) / 1.0e6;
  summary.insert_ms = static_cast<f64>(s.sink_ns) / 1.0e6;
  summary.insert_ms_max = static_cast<f64>(s.max_sink_ns) / 1.0e6;
  summary.handed = s.handed;
#endif
  return summary;
}

bool ViewWorld::fly_handovers(const renderer::CameraPath& camera_path, u32 frames,
                              const std::string& path, u32& handovers, std::string* error) {
  handovers = 0;
#if ENGINE_WORLD_RUINS
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
  Vector<world::RuinsTiles::TileRange> before_ranges;
  Vector<world::RuinsTiles::TileRange> after_ranges;
  // A tile the pair budget left out of the tail has no range; it would sit where the next one
  // starts, so that is where an empty range of it is.
  auto range_of = [](const Vector<world::RuinsTiles::TileRange>& ranges, u64 key, u32 total) {
    for (const world::RuinsTiles::TileRange& range : ranges) {
      if (range.key == key) return range;
      if (range.key > key) return world::RuinsTiles::TileRange{key, range.first, 0};
    }
    return world::RuinsTiles::TileRange{key, total, 0};
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
  auto blocks_of = [&](const Vector<world::RuinsTiles::TileRange>& ranges, u64 key, u32 count) {
    blocks.clear();
    u32 first = 0;
    bool placed = false;
    for (const world::RuinsTiles::TileRange& range : ranges) {
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
    before_tail.assign(ruins_.tail().begin(), ruins_.tail().end());
    before_rubble.assign(ruins_.tail_rubble().begin(), ruins_.tail_rubble().end());
    ruins_.tile_ranges(before_ranges);
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
    after_tail.assign(ruins_.tail().begin(), ruins_.tail().end());
    after_rubble.assign(ruins_.tail_rubble().begin(), ruins_.tail_rubble().end());
    ruins_.tile_ranges(after_ranges);
    if (ok) to_tail(after);
    const Vec3 eye = frame.camera.position;
    for (const world::TileEvent& event : world_.last_events()) {
      if (!ok) break;
      if (!is_handover(event)) continue;
      const u64 key = world::tile_key(event.tile);
      const world::RuinsTiles::TileRange was = range_of(before_ranges, key, before_tail.size());
      const world::RuinsTiles::TileRange now = range_of(after_ranges, key, after_tail.size());
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
      const Vec3 c = world::tile_center(event.tile, world_.params().tile_size);
      line.distance = std::sqrt((c.x - eye.x) * (c.x - eye.x) + (c.z - eye.z) * (c.z - eye.z));
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
    for (const world::RuinsTiles::TileRange& range : after_ranges)
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
#else
  (void)camera_path;
  (void)frames;
  (void)path;
  if (error != nullptr) *error = "a handover is a ruins tile's, and this build has no ruins";
  return false;
#endif
}

bool ViewWorld::is_handover(const world::TileEvent& event) const noexcept {
#if ENGINE_WORLD_RUINS
  if (event.kind != world::TileEventKind::ChangeRing) return false;
  if (!ruins_.has_building(event.tile)) return false;
  const u32 n = ruins_config_.ring_count;
  const world::RingRuins from =
      event.from < n ? ruins_config_.ruins[event.from] : world::RingRuins::Walls;
  const world::RingRuins to =
      event.to < n ? ruins_config_.ruins[event.to] : world::RingRuins::Walls;
  return from != to;
#else
  (void)event;
  return false;
#endif
}

const char* ViewWorld::drawn_as(u8 ring) const noexcept {
#if ENGINE_WORLD_RUINS
  return world::ring_ruins_name(ring < ruins_config_.ring_count ? ruins_config_.ruins[ring]
                                                                : world::RingRuins::Walls);
#else
  (void)ring;
  return "";
#endif
}

JsonValue ViewWorld::summary_json() const { return schema::to_json(summary()); }

}  // namespace engine::view

#endif  // ENGINE_VIEW_WORLD
