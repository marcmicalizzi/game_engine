// The session's sound (game_sound.h; docs/subsystems/apps.md, "Sound a module plays").

#include "game_sound.h"

#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/geometry/flora_species.h>
#include <systems/renderer/flythrough.h>
#include <systems/renderer/request.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_wind.h>
#include <systems/renderer/view_set.h>
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

world_sound::SoundStage& stage_of(void* user) { return static_cast<GameSoundHost*>(user)->stage(); }

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

f64 distance_xz(f64 x, f64 z, const WorldPos& p) noexcept {
  return std::sqrt((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z));
}

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
                         std::string* error) {
  scene::Sound block;
  bool present = false;
  if (!scene.empty() && !world_sound::read_scene_sound(scene, block, present, error)) return false;
  world_sound::StageConfig config;
  config.mode = mode;
  config.device = device;
  config.wav_out = wav_out;
  config.tick_hz = tick_hz;
  config.world = mode != world_sound::AudioMode::Off;  // the wind in the trees and at the ears
  world_ = config.world;
  tick_hz_ = tick_hz != 0 ? tick_hz : 240;
  if (!scene.empty()) config.base_dir = std::filesystem::path(scene).parent_path().string();
  if (!stage_.open(config, present ? &block : nullptr, error)) return false;
  stage_.set_canopy(world_sound::CanopySource{this, &GameSoundHost::visit});
  if (present) {
    std::fprintf(stderr, "engine-view: sound: %u clip%s, audio %s\n",
                 static_cast<u32>(block.clips.size()), block.clips.size() == 1 ? "" : "s",
                 world_sound::audio_mode_name(mode));
  }
  return true;
}

GameSound GameSoundHost::hooks() noexcept {
  GameSound hooks;
  hooks.user = this;
  hooks.clip = &on_clip;
  hooks.play = &on_play;
  hooks.stop = &on_stop;
  hooks.tick = &on_tick;
  if (world_) {
    hooks.wind = &on_wind;
    hooks.scene = &on_scene;
    hooks.tiles = &on_tiles;
  }
  hooks.summary = &on_summary;
  hooks.finish = &on_finish;
  return hooks;
}

void GameSoundHost::set_clock(f64 sky_start_s, f64 sun_rate, u32 tick_hz) noexcept {
  sky_start_s_ = sky_start_s;
  sun_rate_ = sun_rate;
  if (tick_hz != 0) tick_hz_ = tick_hz;
}

void GameSoundHost::tick_listener(const WorldPos& eye, const Vec3& forward,
                                  const Vec3& up) noexcept {
  stage_.set_listener(eye, forward, up);
  stage_.tick();
}

void GameSoundHost::observe(void* user, const FlySession& session, const input::InputState&) {
  GameSoundHost& h = *static_cast<GameSoundHost*>(user);
  const i64 started = time::monotonic_ns();
  // The session's sky as the frame's: the start plus the sun's rate times the ticks' seconds.
  const f64 t = static_cast<f64>(session.tick().value) / static_cast<f64>(h.tick_hz_);
  if (h.world_) on_wind(&h, session.wind().live(), h.sky_start_s_ + h.sun_rate_ * t);
  on_tick(&h, session.state().position, session.state().yaw, session.state().pitch);
  h.sound_ns_ += time::monotonic_ns() - started;
}

void GameSoundHost::tick_at(const WorldPos& eye, const Vec3& forward, const gfx::WindLive& live,
                            f64 sky_time_s) noexcept {
  const i64 started = time::monotonic_ns();
  if (world_) on_wind(this, live, sky_time_s);
  // Up as the world's, made square to where the camera looks.
  const Vec3 world_up{0.0f, 1.0f, 0.0f};
  Vec3 up = world_up - forward * dot(world_up, forward);
  const f32 l = length(up);
  up = l > 1e-6f ? up * (1.0f / l) : Vec3{0.0f, 0.0f, 1.0f};
  tick_listener(eye, forward, up);
  sound_ns_ += time::monotonic_ns() - started;
}

// ---- the canopy table (S112) --------------------------------------------------------------------

bool canopy_class_of(u8 declared, f32 declared_needle_mm, std::string_view species,
                     world_sound::CanopyClass& out, f32& needle_mm) noexcept {
  using geometry::FloraSpecies;
  // The needles' width as the material generator draws a spray (matgen.schema's
  // `Spray.needle_width_um`: 1,200 um by default, its Scots pine 1,700).
  needle_mm = 1.2f;
  if (declared != renderer::SceneMeshInfo::k_sound_undeclared) {
    if (declared_needle_mm > 0.0f) needle_mm = declared_needle_mm;
    switch (static_cast<scene::SoundCanopy>(declared)) {
      case scene::SoundCanopy::Blade: out = world_sound::CanopyClass::Blade; return true;
      case scene::SoundCanopy::Needle: out = world_sound::CanopyClass::Needle; return true;
      case scene::SoundCanopy::Bare: out = world_sound::CanopyClass::Bare; return true;
      case scene::SoundCanopy::None: return false;
    }
    return false;
  }
  const geometry::FloraSpeciesInfo* info = geometry::flora_species_info(species);
  if (info == nullptr) return false;  // a mesh that says nothing is silent
  switch (info->id) {
    case FloraSpecies::pine:
      needle_mm = 1.7f;
      out = world_sound::CanopyClass::Needle;
      return true;
    case FloraSpecies::conifer:
    case FloraSpecies::spruce:
    case FloraSpecies::cedar:
    case FloraSpecies::juniper:
    case FloraSpecies::fir:
    case FloraSpecies::hemlock: out = world_sound::CanopyClass::Needle; return true;
    // The ground's cover is not a crown: its rustle is not this stage's law.
    case FloraSpecies::sward:
    case FloraSpecies::cover:
    case FloraSpecies::tuft:
    case FloraSpecies::herb:
    case FloraSpecies::fern:
    case FloraSpecies::ivy:
    case FloraSpecies::roots:
    case FloraSpecies::litter: return false;
    default: out = world_sound::CanopyClass::Blade; return true;
  }
}

const gfx::WindDesc* GameSoundHost::scene_wind() noexcept {
  if (scene_ == nullptr) return nullptr;
  return &renderer::scene_wind_in_use(scene_->wind, wind_held_);
}

#if ENGINE_VIEW_WORLD
namespace {
void list_placements(const void* user, Vector<SoundTile>& out) {
  const auto& tiles = *static_cast<const world::PlacementTiles*>(user);
  // The tiles' own scratch, kept between refreshes so a steady walk allocates nothing.
  static thread_local Vector<world::PlacementTiles::TileOwn> own;
  tiles.tile_own(own);
  out.clear();
  for (const world::PlacementTiles::TileOwn& t : own)
    out.push_back(SoundTile{t.key, t.instances, t.count, t.version});
}
}  // namespace
void GameSoundHost::attach_tiles(void* placements) noexcept {
  if (placements != nullptr) {
    attach_tile_list(&list_placements, placements);
  } else {
    attach_tile_list(nullptr, nullptr);
  }
}
#else
void GameSoundHost::attach_tiles(void*) noexcept { attach_tile_list(nullptr, nullptr); }
#endif

void GameSoundHost::merge(Group& g, std::span<const world_sound::Crown> crowns) noexcept {
  f64 area[3] = {};
  f64 x = 0.0, z = 0.0, total = 0.0;
  f64 height[3] = {}, needle[3] = {}, y[3] = {};
  for (const world_sound::Crown& c : crowns) {
    const u32 k = static_cast<u32>(c.kind) < 3 ? static_cast<u32>(c.kind) : 0u;
    const f64 a = static_cast<f64>(c.leaf_area_m2);
    area[k] += a;
    height[k] += a * static_cast<f64>(c.height_m);
    needle[k] += a * static_cast<f64>(c.needle_mm);
    y[k] += a * c.at.y;
    x += a * c.at.x;
    z += a * c.at.z;
    total += a;
  }
  g.x = total > 0.0 ? x / total : 0.0;
  g.z = total > 0.0 ? z / total : 0.0;
  g.radius = 0.0;
  for (const world_sound::Crown& c : crowns) {
    const f64 r = distance_xz(g.x, g.z, c.at) + static_cast<f64>(c.extent_m);
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
    m.merged = true;  // the beds' only (L6)
    m.extent_m = static_cast<f32>(g.radius);
  }
}

void GameSoundHost::attach_scene(const renderer::SceneData& scene) {
  scene_ = &scene;
  const u32 meshes = static_cast<u32>(scene.mesh_leaves.size());
  mesh_heard_.clear();
  mesh_class_.clear();
  mesh_needle_mm_.clear();
  mesh_fitted_.clear();
  mesh_heard_.resize(meshes, 0);
  mesh_class_.resize(meshes, world_sound::CanopyClass::Blade);
  mesh_needle_mm_.resize(meshes, 1.2f);
  mesh_fitted_.resize(meshes);
  for (u32 m = 0; m < meshes && m < scene.sources.size(); ++m) {
    const renderer::MeshLeaves& l = scene.mesh_leaves[m];
    if (!(l.area_m2 > 0.0f)) continue;
    // A distance form sounds as its source (`form_source`), whose word and species it shares.
    const u32 f = m < scene.form_source.size() ? scene.form_source[m] : m;
    const u32 s = f < scene.sources.size() ? f : m;
    const renderer::SceneMeshInfo* info = s < scene.mesh_info.size()   ? &scene.mesh_info[s]
                                          : m < scene.mesh_info.size() ? &scene.mesh_info[m]
                                                                       : nullptr;
    const u8 declared =
        info != nullptr ? info->sound_canopy : renderer::SceneMeshInfo::k_sound_undeclared;
    const f32 declared_needle = info != nullptr ? info->sound_needle_mm : 0.0f;
    mesh_heard_[m] = canopy_class_of(declared, declared_needle, scene.sources[s].species,
                                     mesh_class_[m], mesh_needle_mm_[m])
                         ? 1u
                         : 0u;
    // Through the mesh's fit, as a tile's placement is drawn (`place_instance`; L1).
    const Mat4 fit = m < scene.mesh_fit.size() ? scene.mesh_fit[m] : Mat4::identity();
    Fitted& fd = mesh_fitted_[m];
    fd.foot = transform_point(fit, Vec3{l.centre_x, 0.0f, l.centre_z});
    const Vec3 mid =
        transform_point(fit, Vec3{l.centre_x, 0.5f * (l.low_m + l.high_m), l.centre_z});
    const Vec3 unit = transform_point(fit, Vec3{l.centre_x + 1.0f, 0.0f, l.centre_z}) - fd.foot;
    fd.scale = length(unit);
    fd.mid = mid.y - fd.foot.y;
  }
  // The loaded instances with leaves, by 128 m cell, each cell merged: walked at a refresh one by
  // one where the listener is near the cell, as three crowns where it is far. A loaded instance's
  // rows already carry its mesh's fit.
  crowns_.clear();
  for (const gfx::InstanceDesc& in : scene.instances) {
    if (in.mesh >= meshes || mesh_heard_[in.mesh] == 0) continue;
    const renderer::MeshLeaves& l = scene.mesh_leaves[in.mesh];
    const f32 cx = l.centre_x;
    const f32 cz = l.centre_z;
    const auto row = [&](u32 r) {
      return static_cast<f64>(in.rows[r].x * cx + in.rows[r].z * cz + in.rows[r].w);
    };
    world_sound::Crown c;
    c.at = WorldPos{static_cast<f64>(in.cell.x) * k_world_cell_m + row(0),
                    static_cast<f64>(in.cell.y) * k_world_cell_m + row(1),
                    static_cast<f64>(in.cell.z) * k_world_cell_m + row(2)};
    c.height_m = 0.5f * (l.low_m + l.high_m) * in.scale_max;
    c.leaf_area_m2 = l.area_m2 * in.scale_max * in.scale_max;
    c.kind = mesh_class_[in.mesh];
    c.needle_mm = mesh_needle_mm_[in.mesh];
    c.extent_m = l.extent_m * in.scale_max;
    crowns_.push_back(c);
  }
  std::sort(crowns_.begin(), crowns_.end(),
            [](const world_sound::Crown& a, const world_sound::Crown& b) {
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

bool GameSoundHost::tile_crown(const renderer::SceneInstance& s,
                               world_sound::Crown& out) const noexcept {
  if (s.mesh >= mesh_heard_.size() || mesh_heard_[s.mesh] == 0) return false;
  const renderer::MeshLeaves& l = scene_->mesh_leaves[s.mesh];
  const Fitted& fd = mesh_fitted_[s.mesh];
  const Vec3 sc = s.transform.scale;
  const f32 scale = std::max(sc.x, std::max(sc.y, sc.z)) * fd.scale;
  const Vec3 foot = transform_point(s.transform, fd.foot);
  out = world_sound::Crown{};
  out.at = WorldPos{s.origin.x + static_cast<f64>(foot.x), s.origin.y + static_cast<f64>(foot.y),
                    s.origin.z + static_cast<f64>(foot.z)};
  out.height_m = fd.mid * std::max(sc.x, std::max(sc.y, sc.z));
  out.leaf_area_m2 = l.area_m2 * scale * scale;
  out.kind = mesh_class_[s.mesh];
  out.needle_mm = mesh_needle_mm_[s.mesh];
  out.extent_m = l.extent_m * scale;
  return true;
}

// **A refresh costs what changed** (S112-2, H1): the drawn tiles are listed by key (no tail,
// T131), matched to the groups held in one pass of two sorted runs, and only a tile whose version
// moved has its placements walked, within `allowance` placements a refresh (the rest carried, the
// first changed tile always taken so a tile larger than the allowance is still made).
void GameSoundHost::refresh_tiles(u32& allowance) {
  if (tile_list_ == nullptr) {
    tile_groups_.clear();
    counters_.tile_groups = 0;
    return;
  }
  tile_list_(tile_user_, tiles_now_);
  bool sorted = true;
  for (u32 i = 1; i < tiles_now_.size() && sorted; ++i)
    sorted = tiles_now_[i - 1].key < tiles_now_[i].key;
  if (!sorted) {
    std::sort(tiles_now_.begin(), tiles_now_.end(),
              [](const SoundTile& a, const SoundTile& b) { return a.key < b.key; });
  }
  bool same = tile_groups_.size() == tiles_now_.size();
  for (u32 i = 0; i < tiles_now_.size() && same; ++i)
    same = tile_groups_[i].g.key == tiles_now_[i].key;
  if (!same) {
    tile_next_.clear();
    u32 r = 0;
    for (const SoundTile& t : tiles_now_) {
      while (r < tile_groups_.size() && tile_groups_[r].g.key < t.key)
        ++r;  // no longer drawn: let go with `tile_next_`'s clear below
      if (r < tile_groups_.size() && tile_groups_[r].g.key == t.key) {
        tile_next_.push_back(std::move(tile_groups_[r++]));
      } else {
        tile_next_.push_back(TileGroup{});
        tile_next_.back().g.key = t.key;
      }
    }
    tile_groups_.swap(tile_next_);
    tile_next_.clear();
  }
  counters_.tile_groups = static_cast<u32>(tile_groups_.size());
  bool took = false;
  for (u32 i = 0; i < tiles_now_.size(); ++i) {
    const SoundTile& t = tiles_now_[i];
    TileGroup& tg = tile_groups_[i];
    if (tg.made && tg.g.version == t.version) continue;
    if (t.count > allowance && took) {
      ++counters_.tiles_carried;
      continue;
    }
    took = true;
    allowance -= t.count < allowance ? t.count : allowance;
    tg.crowns.clear();
    for (u32 k = 0; k < t.count; ++k) {
      world_sound::Crown c;
      if (tile_crown(t.instances[k], c)) tg.crowns.push_back(c);
    }
    merge(tg.g, std::span<const world_sound::Crown>(tg.crowns.data(), tg.crowns.size()));
    tg.g.version = t.version;
    tg.made = true;
    counters_.instances_walked += t.count;
    counters_.last_walked += t.count;
    ++counters_.tiles_walked;
  }
}

void GameSoundHost::visit(void* user, const WorldPos& centre, f64 reach_m,
                          const world_sound::CrownSink& sink) {
  GameSoundHost& h = *static_cast<GameSoundHost*>(user);
  if (h.scene_ == nullptr) return;
  ++h.counters_.refreshes;
  h.counters_.last_walked = 0;
  const f64 near_m = world_sound::world_near_m();
  u32 allowance = world_sound::world_walk_instances();
  h.refresh_tiles(allowance);
  // Every group: past the reach nothing, far its merged crowns, near kept for the pass below.
  h.near_.clear();
  const auto consider = [&](const Group& g, const world_sound::Crown* crowns) {
    if (g.count == 0 && !(g.merged[0].leaf_area_m2 > 0.0f || g.merged[1].leaf_area_m2 > 0.0f ||
                          g.merged[2].leaf_area_m2 > 0.0f))
      return;
    const f64 d = distance_xz(g.x, g.z, centre);
    if (d - g.radius > reach_m) return;
    if (d - g.radius <= near_m) {
      h.near_.push_back(Near{d, g.key, &g, crowns});
      return;
    }
    for (const world_sound::Crown& m : g.merged) {
      if (!(m.leaf_area_m2 > 0.0f)) continue;
      sink.emit(sink.user, m);
      ++h.counters_.merged_sent;
    }
  };
  for (const Group& g : h.cells_)
    consider(g, h.crowns_.data() + g.first);
  for (TileGroup& tg : h.tile_groups_) {
    tg.g.count = static_cast<u32>(tg.crowns.size());
    consider(tg.g, tg.crowns.data());
  }
  // The near groups nearest first, one by one while the allowance lasts and merged after it (M1:
  // the tree over the listener is never the one an order left out).
  std::sort(h.near_.begin(), h.near_.end(),
            [](const Near& a, const Near& b) { return a.d != b.d ? a.d < b.d : a.key < b.key; });
  u32 room = world_sound::world_near_crowns();
  for (const Near& n : h.near_) {
    if (n.g->count <= room) {
      for (u32 i = 0; i < n.g->count; ++i)
        sink.emit(sink.user, n.crowns[i]);
      room -= n.g->count;
      h.counters_.crowns_sent += n.g->count;
      continue;
    }
    for (const world_sound::Crown& m : n.g->merged) {
      if (!(m.leaf_area_m2 > 0.0f)) continue;
      sink.emit(sink.user, m);
      ++h.counters_.merged_sent;
    }
  }
}

// ---- the session's sound as engine-view's loops reach it (S112-2) ------------------------------

namespace {

GameSoundHost* g_host = nullptr;
bool g_solo = false;
f64 g_flight_sky_start_s = 0.0;
f64 g_flight_sun_rate = 0.0;

GameSoundHost* solo_world() noexcept {
  return g_solo && g_host != nullptr && g_host->world() ? g_host : nullptr;
}

}  // namespace

void set_session_sound(GameSoundHost* host, bool solo) noexcept {
  g_host = host;
  g_solo = host != nullptr && solo;
}
bool open_solo_sound(GameSoundHost& host, std::string_view audio, bool windowed,
                     const std::string& wav_out, const std::string& scene, u32 tick_hz,
                     std::string* error) {
  world_sound::AudioMode mode = world_sound::AudioMode::Off;
  std::string device;
  if (!resolve_audio(audio, windowed, mode, device, error)) return false;
  if (mode == world_sound::AudioMode::Off) return true;  // nothing opened, no leaves measured
  if (!host.open(mode, device, wav_out, scene, tick_hz, error)) return false;
  set_session_sound(&host, true);
  return true;
}
bool sound_hears_world() noexcept { return g_host != nullptr && g_host->world(); }
i64 solo_sound_ns() noexcept { return g_solo && g_host != nullptr ? g_host->sound_ns() : 0; }
void attach_solo_scene(const renderer::SceneData& scene) {
  if (GameSoundHost* h = solo_world()) h->attach_scene(scene);
}
void attach_solo_session(FlySession& session, f64 sky_start_s, f64 sun_rate) noexcept {
  if (!g_solo || g_host == nullptr) return;
  session.set_tick_observer(&GameSoundHost::observe, g_host);
  g_host->set_clock(sky_start_s, sun_rate, 0);
}
void attach_solo_tiles(void* placements) noexcept {
  if (GameSoundHost* h = solo_world()) h->attach_tiles(placements);
}
void set_solo_flight_clock(f64 sky_start_s, f64 sun_rate) noexcept {
  g_flight_sky_start_s = sky_start_s;
  g_flight_sun_rate = sun_rate;
}
void solo_flight_frame(const renderer::FlightStep& step) noexcept {
  GameSoundHost* h = solo_world();
  if (h == nullptr) return;
  const i64 before = h->sound_ns();
  const f64 sky = g_flight_sky_start_s + g_flight_sun_rate * static_cast<f64>(step.frame) /
                                             static_cast<f64>(renderer::k_frame_hz);
  h->tick_at(step.camera.position, renderer::camera_forward(step.camera), gfx::WindLive{}, sky);
  if (step.world == nullptr) return;
  // The sound's tick is a flight frame's only one: in `ticks_ms` too, so `other_ms` leaves it out.
  const f64 ms = static_cast<f64>(h->sound_ns() - before) / 1.0e6;
  step.world->sound_ms += ms;
  step.world->ticks_ms += ms;
}
void print_solo_summary() {
  if (!g_solo || g_host == nullptr) return;
  std::string error;
  (void)g_host->stage().close(&error);
  std::fprintf(
      stderr, "engine-view: sound summary %s\n",
      write_json(g_host->stage().summary_json(), JsonWriteOptions{.pretty = false}).c_str());
  const GameSoundHost::Counters& c = g_host->counters();
  std::fprintf(stderr,
               "engine-view: sound table: %llu refreshes, %llu tile placements walked in %llu "
               "tiles (%llu carried), %llu crowns and %llu merged sent, %u tiles held\n",
               static_cast<unsigned long long>(c.refreshes),
               static_cast<unsigned long long>(c.instances_walked),
               static_cast<unsigned long long>(c.tiles_walked),
               static_cast<unsigned long long>(c.tiles_carried),
               static_cast<unsigned long long>(c.crowns_sent),
               static_cast<unsigned long long>(c.merged_sent), c.tile_groups);
}

}  // namespace engine::view
