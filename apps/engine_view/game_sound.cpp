// The session's sound for a game (game_sound.h; docs/subsystems/apps.md, "Sound a module plays").

#include "game_sound.h"

#include <domain/geometry/flora_species.h>
#include <systems/renderer/scene.h>
#if ENGINE_VIEW_WORLD
#include <systems/world/placement_tiles.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <schemas/scene.h>

namespace engine::view {

namespace {

hotload::Result result_of(world_sound::Answer a) noexcept {
  switch (a) {
    case world_sound::Answer::Ok: return hotload::Result::Ok;
    case world_sound::Answer::NotFound: return hotload::Result::NotFound;
    case world_sound::Answer::Unsupported: return hotload::Result::Unsupported;
    case world_sound::Answer::InvalidArgument: return hotload::Result::InvalidArgument;
  }
  return hotload::Result::Unsupported;
}

world_sound::SoundStage& stage_of(void* user) {
  return static_cast<GameSoundHost*>(user)->stage();
}

void on_wind(void* user, const gfx::WindLive& live, f64 sky_time_s) {
  GameSoundHost& host = *static_cast<GameSoundHost*>(user);
  world_sound::WorldWind wind;
  wind.scene = host.scene_wind();
  wind.live = live;
  wind.sky_time_s = sky_time_s;
  host.stage().set_wind(wind);
}
void on_scene(void* user, const renderer::SceneData& scene) {
  static_cast<GameSoundHost*>(user)->attach_scene(scene);
}
void on_tiles(void* user, void* placements) {
  static_cast<GameSoundHost*>(user)->attach_tiles(placements);
}

// The 128 m cell a loaded crown is merged in, as a key.
u64 cell_key(const WorldPos& p) noexcept {
  const i64 x = static_cast<i64>(std::floor(p.x / 128.0));
  const i64 z = static_cast<i64>(std::floor(p.z / 128.0));
  return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z);
}

hotload::Result on_clip(void* user, std::string_view name, u32& out) {
  return result_of(stage_of(user).clip(name, out));
}
hotload::Result on_play(void* user, u32 clip, const f64* position, f32 gain, u32& out) {
  if (position == nullptr) return result_of(stage_of(user).play(clip, nullptr, gain, out));
  const WorldPos at{position[0], position[1], position[2]};
  return result_of(stage_of(user).play(clip, &at, gain, out));
}
hotload::Result on_stop(void* user, u32 voice) { return result_of(stage_of(user).stop(voice)); }
void on_tick(void* user, const WorldPos& eye, f32 heading, f32 pitch) {
  world_sound::SoundStage& stage = stage_of(user);
  Vec3 forward;
  Vec3 up;
  listener_axes(heading, pitch, forward, up);
  stage.set_listener(eye, forward, up);
  stage.tick();
}
JsonValue on_summary(const void* user) {
  return static_cast<const GameSoundHost*>(user)->stage().summary_json();
}
bool on_finish(void* user, std::string* error) { return stage_of(user).close(error); }

}  // namespace

bool resolve_audio(std::string_view flag, bool windowed, world_sound::AudioMode& mode,
                   std::string& device, std::string* error) {
  device.clear();
  if (flag.empty()) {
    if (error != nullptr) *error = "--audio names auto, off, null or a device";
    return false;
  }
  if (flag == "auto") {
    mode = windowed ? world_sound::AudioMode::Device : world_sound::AudioMode::Off;
  } else if (flag == "off") {
    mode = world_sound::AudioMode::Off;
  } else if (flag == "null") {
    mode = world_sound::AudioMode::Null;
  } else {
    mode = world_sound::AudioMode::Device;
    device = std::string(flag);
  }
  return true;
}

void listener_axes(f32 heading, f32 pitch, Vec3& forward, Vec3& up) noexcept {
  const f32 sh = std::sin(heading);
  const f32 ch = std::cos(heading);
  const f32 sp = std::sin(pitch);
  const f32 cp = std::cos(pitch);
  forward = Vec3{-sh * cp, sp, -ch * cp};
  up = Vec3{sh * sp, cp, ch * sp};
}

bool GameSoundHost::open(world_sound::AudioMode mode, const std::string& device,
                         const std::string& wav_out, const std::string& scene, u32 tick_hz,
                         GameRunner& game, std::string* error) {
  scene::Sound block;
  bool present = false;
  if (!scene.empty() && !world_sound::read_scene_sound(scene, block, present, error)) return false;
  world_sound::StageConfig config;
  config.mode = mode;
  config.device = device;
  config.wav_out = wav_out;
  config.tick_hz = tick_hz;
  config.world = mode != world_sound::AudioMode::Off;  // the wind in the trees and at the ears
  if (!scene.empty()) config.base_dir = std::filesystem::path(scene).parent_path().string();
  if (!stage_.open(config, present ? &block : nullptr, error)) return false;
  stage_.set_canopy(world_sound::CanopySource{this, &GameSoundHost::visit});
  GameSound hooks;
  hooks.user = this;
  hooks.clip = &on_clip;
  hooks.play = &on_play;
  hooks.stop = &on_stop;
  hooks.tick = &on_tick;
  if (config.world) {
    hooks.wind = &on_wind;
    hooks.scene = &on_scene;
    hooks.tiles = &on_tiles;
  }
  hooks.summary = &on_summary;
  hooks.finish = &on_finish;
  game.set_sound(hooks);
  if (present) {
    std::fprintf(stderr, "engine-view: sound: %u clip%s, audio %s\n",
                 static_cast<u32>(block.clips.size()), block.clips.size() == 1 ? "" : "s",
                 world_sound::audio_mode_name(mode));
  }
  return true;
}

// ---- the canopy table (S112) --------------------------------------------------------------------

world_sound::CanopyClass canopy_class_of(std::string_view species, f32& needle_mm) noexcept {
  using geometry::FloraSpecies;
  // The needles' width as the material generator draws a spray (matgen.schema's
  // `Spray.needle_width_um`: 1,200 um by default, its Scots pine 1,700).
  needle_mm = 1.2f;
  const geometry::FloraSpeciesInfo* info = geometry::flora_species_info(species);
  if (info == nullptr) return world_sound::CanopyClass::Blade;
  switch (info->id) {
    case FloraSpecies::pine: needle_mm = 1.7f; return world_sound::CanopyClass::Needle;
    case FloraSpecies::conifer:
    case FloraSpecies::spruce:
    case FloraSpecies::cedar:
    case FloraSpecies::juniper:
    case FloraSpecies::fir:
    case FloraSpecies::hemlock: return world_sound::CanopyClass::Needle;
    default: return world_sound::CanopyClass::Blade;
  }
}

const gfx::WindDesc* GameSoundHost::scene_wind() const noexcept {
  return scene_ != nullptr ? &scene_->wind : nullptr;
}

world_sound::Crown GameSoundHost::crown_of(u32 mesh, const WorldPos& at, f32 scale) const noexcept {
  const renderer::MeshLeaves& l = scene_->mesh_leaves[mesh];
  world_sound::Crown c;
  c.at = at;
  c.height_m = 0.5f * (l.low_m + l.high_m) * scale;
  c.leaf_area_m2 = l.area_m2 * scale * scale;
  c.kind = mesh < mesh_class_.size() ? mesh_class_[mesh] : world_sound::CanopyClass::Blade;
  c.needle_mm = mesh < mesh_needle_mm_.size() ? mesh_needle_mm_[mesh] : 1.2f;
  return c;
}

void GameSoundHost::merge(Group& g, std::span<const world_sound::Crown> crowns) noexcept {
  f64 area[3] = {};
  f64 x = 0.0, z = 0.0, total = 0.0;
  f64 height[3] = {}, needle[3] = {}, y[3] = {};
  for (const world_sound::Crown& c : crowns) {
    const u32 k = static_cast<u32>(c.kind) < 3 ? static_cast<u32>(c.kind) : 0u;
    const f64 a = c.leaf_area_m2;
    area[k] += a;
    height[k] += a * c.height_m;
    needle[k] += a * c.needle_mm;
    y[k] += a * c.at.y;
    x += a * c.at.x;
    z += a * c.at.z;
    total += a;
  }
  g.x = total > 0.0 ? x / total : 0.0;
  g.z = total > 0.0 ? z / total : 0.0;
  g.radius = 0.0;
  for (const world_sound::Crown& c : crowns) {
    const f64 r = std::sqrt((c.at.x - g.x) * (c.at.x - g.x) + (c.at.z - g.z) * (c.at.z - g.z));
    g.radius = r > g.radius ? r : g.radius;
  }
  for (u32 k = 0; k < 3; ++k) {
    world_sound::Crown& m = g.merged[k];
    m = world_sound::Crown{};
    if (!(area[k] > 0.0)) continue;
    m.at = WorldPos{g.x, y[k] / area[k], g.z};
    m.height_m = static_cast<f32>(height[k] / area[k]);
    m.leaf_area_m2 = static_cast<f32>(area[k]);
    m.kind = static_cast<world_sound::CanopyClass>(k);
    m.needle_mm = static_cast<f32>(needle[k] / area[k]);
  }
}

void GameSoundHost::emit_group(const Group& g, std::span<const world_sound::Crown> crowns,
                               const WorldPos& centre, f64 reach_m, f64 near_m,
                               const world_sound::CrownSink& sink) noexcept {
  const f64 d = std::sqrt((g.x - centre.x) * (g.x - centre.x) + (g.z - centre.z) * (g.z - centre.z));
  if (d - g.radius > reach_m) return;
  if (d - g.radius <= near_m) {
    for (const world_sound::Crown& c : crowns)
      sink.emit(sink.user, c);
    return;
  }
  for (const world_sound::Crown& m : g.merged) {
    if (m.leaf_area_m2 > 0.0f) sink.emit(sink.user, m);
  }
}

void GameSoundHost::attach_scene(const renderer::SceneData& scene) {
  scene_ = &scene;
  const u32 meshes = static_cast<u32>(scene.mesh_leaves.size());
  mesh_class_.clear();
  mesh_needle_mm_.clear();
  mesh_class_.resize(meshes, world_sound::CanopyClass::Blade);
  mesh_needle_mm_.resize(meshes, 1.2f);
  for (u32 m = 0; m < meshes && m < scene.sources.size(); ++m) {
    // A distance form sounds as its source (`form_source`), whose species it shares.
    const u32 s = m < scene.form_source.size() ? scene.form_source[m] : m;
    const std::string& species = scene.sources[s < scene.sources.size() ? s : m].species;
    mesh_class_[m] = canopy_class_of(species, mesh_needle_mm_[m]);
  }
  // The loaded instances with leaves, by 128 m cell, each cell merged: walked at a refresh one by
  // one where the listener is near the cell, as three crowns where it is far.
  crowns_.clear();
  for (const gfx::InstanceDesc& in : scene.instances) {
    if (in.mesh >= meshes || !(scene.mesh_leaves[in.mesh].area_m2 > 0.0f)) continue;
    const WorldPos at{static_cast<f64>(in.cell.x) * k_world_cell_m + in.rows[0].w,
                      static_cast<f64>(in.cell.y) * k_world_cell_m + in.rows[1].w,
                      static_cast<f64>(in.cell.z) * k_world_cell_m + in.rows[2].w};
    crowns_.push_back(crown_of(in.mesh, at, in.scale_max));
  }
  std::sort(crowns_.begin(), crowns_.end(), [](const world_sound::Crown& a, const world_sound::Crown& b) {
    return cell_key(a.at) < cell_key(b.at);
  });
  cells_.clear();
  for (u32 i = 0; i < crowns_.size();) {
    const u64 key = cell_key(crowns_[i].at);
    u32 j = i;
    while (j < crowns_.size() && cell_key(crowns_[j].at) == key)
      ++j;
    Group g;
    g.key = key;
    g.first = i;
    g.count = j - i;
    merge(g, std::span<const world_sound::Crown>(crowns_.data() + i, g.count));
    cells_.push_back(g);
    i = j;
  }
  std::fprintf(stderr, "engine-view: sound: %u crowns of %u instances in %u cells\n",
               static_cast<u32>(crowns_.size()), static_cast<u32>(scene.instances.size()),
               static_cast<u32>(cells_.size()));
}

void GameSoundHost::visit(void* user, const WorldPos& centre, f64 reach_m,
                          const world_sound::CrownSink& sink) {
  GameSoundHost& h = *static_cast<GameSoundHost*>(user);
  if (h.scene_ == nullptr) return;
  const f64 near_m = world_sound::world_near_m();
  for (const Group& g : h.cells_)
    emit_group(g, std::span<const world_sound::Crown>(h.crowns_.data() + g.first, g.count), centre,
               reach_m, near_m, sink);
#if ENGINE_VIEW_WORLD
  // A streamed world's drawn tiles: a tile's crowns merged again only when its version moves.
  if (h.tiles_ == nullptr) return;
  const world::PlacementTiles& tiles = *static_cast<const world::PlacementTiles*>(h.tiles_);
  // The ranges' scratch, kept between refreshes so a steady walk allocates nothing.
  static thread_local Vector<world::PlacementTiles::TileRange> ranges;
  ranges.clear();
  tiles.tile_ranges(ranges);
  // Groups of tiles no longer drawn are let go once they outnumber the drawn ones.
  if (h.tile_groups_.size() > 2u * ranges.size() + 16u) {
    u32 kept = 0;
    for (u32 i = 0; i < h.tile_groups_.size(); ++i) {
      bool drawn = false;
      for (const world::PlacementTiles::TileRange& r : ranges)
        drawn = drawn || r.key == h.tile_groups_[i].key;
      if (drawn) h.tile_groups_[kept++] = h.tile_groups_[i];
    }
    h.tile_groups_.resize(kept);
  }
  const std::span<const renderer::SceneInstance> tail = tiles.tail();
  const u32 meshes = static_cast<u32>(h.scene_->mesh_leaves.size());
  for (const world::PlacementTiles::TileRange& r : ranges) {
    const renderer::SceneInstance* in =
        r.instances != nullptr ? r.instances
        : static_cast<usize>(r.first) + r.count <= tail.size() ? tail.data() + r.first
                                                              : nullptr;
    if (in == nullptr || r.count == 0) continue;
    Group* g = nullptr;
    for (Group& x : h.tile_groups_) {
      if (x.key == r.key) {
        g = &x;
        break;
      }
    }
    if (g == nullptr) {
      h.tile_groups_.push_back(Group{});
      g = &h.tile_groups_.back();
      g->key = r.key;
      g->version = ~u64{0};
    }
    const bool stale = r.version == 0 || g->version != r.version;
    h.tile_scratch_.clear();
    const f64 d = std::sqrt((g->x - centre.x) * (g->x - centre.x) +
                            (g->z - centre.z) * (g->z - centre.z));
    const bool near = stale || d - g->radius <= near_m;
    if (!near) {
      emit_group(*g, {}, centre, reach_m, near_m, sink);
      continue;
    }
    for (u32 i = 0; i < r.count; ++i) {
      const renderer::SceneInstance& s = in[i];
      if (s.mesh >= meshes || !(h.scene_->mesh_leaves[s.mesh].area_m2 > 0.0f)) continue;
      const Vec3 sc = s.transform.scale;
      const f32 scale = std::max(sc.x, std::max(sc.y, sc.z));
      const WorldPos at{s.origin.x + s.transform.position.x, s.origin.y + s.transform.position.y,
                        s.origin.z + s.transform.position.z};
      h.tile_scratch_.push_back(h.crown_of(s.mesh, at, scale));
    }
    if (stale) {
      merge(*g, std::span<const world_sound::Crown>(h.tile_scratch_.data(), h.tile_scratch_.size()));
      g->version = r.version;
    }
    emit_group(*g, std::span<const world_sound::Crown>(h.tile_scratch_.data(), h.tile_scratch_.size()),
               centre, reach_m, near_m, sink);
  }
#endif
}

}  // namespace engine::view
