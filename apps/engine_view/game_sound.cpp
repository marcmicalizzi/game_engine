// The session's sound (game_sound.h; docs/subsystems/apps.md, "Sound a module plays").

#include "game_sound.h"

#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/geometry/flora_species.h>
#include <domain/scene_gen/water_channel.h>
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
#include <cstdlib>
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
  wind.world_time_s = host.world_time();  // the water's clock (S113)
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
void on_walker(void* user, const Walker* walker) {
  static_cast<GameSoundHost*>(user)->set_walker(walker);
}
void on_tick(void* user, const WorldPos& eye, f32 heading, f32 pitch) {
  world_sound::SoundStage& stage = stage_of(user);
  Vec3 forward;
  Vec3 up;
  listener_axes(heading, pitch, forward, up);
  stage.set_listener(eye, forward, up);
  stage.tick();
  static_cast<GameSoundHost*>(user)->advance_world_time();  // a module's ticks (S113)
}
JsonValue on_summary(const void* user) {
  return static_cast<const GameSoundHost*>(user)->stage().summary_json();
}
bool on_finish(void* user, std::string* error) {
  // The session's walker is a local of the loop that ran it: nothing casts through it after the
  // runner's finish (S114-L6).
  static_cast<GameSoundHost*>(user)->set_walker(nullptr);
  return stage_of(user).close(error);
}

f64 distance_xz(f64 x, f64 z, const WorldPos& p) noexcept {
  return std::sqrt((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z));
}

}  // namespace

std::string audio_auto_env() {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, k_audio_auto_env) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(k_audio_auto_env);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

bool resolve_audio(std::string_view flag, const AudioRun& run, world_sound::AudioMode& mode,
                   std::string& device, std::string* error) {
  device.clear();
  if (flag.empty()) {
    if (error != nullptr) *error = "--audio names auto, off, null or a device";
    return false;
  }
  if (flag == "auto") {
    // The speakers only for somebody at the window (N1 of S112's second review): nothing that
    // ends by itself — a test's `--frames`, an agent's capture — and nothing nothing ticks.
    const bool attended = run.windowed && (run.interactive || run.module || !run.ends_by_itself);
    if (!attended || !run.ticked || !run.scene) {
      mode = world_sound::AudioMode::Off;
    } else if (!run.auto_env.empty()) {
      mode = run.auto_env == "off" ? world_sound::AudioMode::Off : world_sound::AudioMode::Null;
    } else {
      mode = world_sound::AudioMode::Device;
    }
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
  stage_.set_flow(world_sound::FlowSource{this, &GameSoundHost::visit_flow});  // S113
  stage_.set_surf(world_sound::SurfSource{this, &GameSoundHost::visit_surf});
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
    hooks.walker = &on_walker;
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
  h.world_time_s_ = t;  // the session's world clock at a rate of one: the water's (S113)
  if (h.world_) {
    // The walker only while the session walks: a session that flies keeps its walker attached
    // (`FlySession::walker`), and its eye is no walker's (S114-M2).
    h.set_walker(session.walking() ? session.walker() : nullptr);
    on_wind(&h, session.wind().live(), h.sky_start_s_ + h.sun_rate_ * t);
  }
  on_tick(&h, session.state().position, session.state().yaw, session.state().pitch);
  h.sound_ns_ += time::monotonic_ns() - started;
}

void GameSoundHost::tick_at(const WorldPos& eye, const Vec3& forward, const gfx::WindLive& live,
                            f64 sky_time_s) noexcept {
  const i64 started = time::monotonic_ns();
  // A flight's frame has no walker: a session's, if one ran before, is gone (S114-L6).
  set_walker(nullptr);
  if (world_) on_wind(this, live, sky_time_s);
  // Up as the world's, made square to where the camera looks.
  const Vec3 world_up{0.0f, 1.0f, 0.0f};
  Vec3 up = world_up - forward * dot(world_up, forward);
  const f32 l = length(up);
  up = l > 1e-6f ? up * (1.0f / l) : Vec3{0.0f, 0.0f, 1.0f};
  tick_listener(eye, forward, up);
  sound_ns_ += time::monotonic_ns() - started;
}

// ---- the ears' shelter (S114) -------------------------------------------------------------------

f32 shelter_height_at(ShelterCast cast, ShelterGround ground, const void* user, f64 feet_y, f64 x,
                      f64 z, f32 across_x, f32 across_z) noexcept {
  if (cast == nullptr) return 0.0f;
  f64 least = 1e30;
  for (const f32 side : {-k_shelter_across_m, 0.0f, k_shelter_across_m}) {
    const WorldPos origin{x + static_cast<f64>(side * across_x),
                          feet_y + static_cast<f64>(k_shelter_cast_above_m),
                          z + static_cast<f64>(side * across_z)};
    WalkCast hit;
    if (!cast(user, origin, 2.0f * k_shelter_cast_above_m, hit) || !hit.hit) return 0.0f;
    // The ground itself is no obstacle: a slope rising into the wind shelters nothing (S114-M1).
    if (hit.what == 1) return 0.0f;
    // What stands there, over the ground under it; over the feet only where no ground is held.
    f64 base = feet_y;
    f64 under = 0.0;
    if (ground != nullptr && ground(user, origin.x, origin.z, under)) base = under;
    const f64 h = hit.position.y - base;
    least = h < least ? h : least;
  }
  return least > 0.0 ? static_cast<f32>(least) : 0.0f;
}

namespace {

bool walker_cast(const void* user, WorldPos origin, f32 length, WalkCast& out) {
  return static_cast<const Walker*>(user)->cast(origin, Vec3{0.0f, -1.0f, 0.0f}, length, 0.0f, out);
}

bool walker_ground(const void* user, f64 x, f64 z, f64& out) {
  return static_cast<const Walker*>(user)->collision_ground(x, z, out);
}

}  // namespace

f32 walker_shelter(const Walker* walker, f64 x, f64 z, f32 across_x, f32 across_z) noexcept {
  if (walker == nullptr) return 0.0f;
  // The walker's own feet: crouched, the drawn eye is the feet plus `eye_height()`, not the
  // standing `params().eye_height` (S114-H1).
  return shelter_height_at(&walker_cast, &walker_ground, walker, walker->feet().y, x, z, across_x,
                           across_z);
}

f32 GameSoundHost::shelter(void* user, f64 x, f64 z, f32 across_x, f32 across_z) noexcept {
  return walker_shelter(static_cast<const GameSoundHost*>(user)->walker_, x, z, across_x, across_z);
}

void GameSoundHost::set_walker(const Walker* walker) noexcept {
  if (walker == walker_) return;
  walker_ = walker;
  stage_.set_shelter(walker != nullptr ? world_sound::ShelterSource{this, &GameSoundHost::shelter}
                                       : world_sound::ShelterSource{});
}

// ---- the canopy table (S112) --------------------------------------------------------------------

bool canopy_class_of(u8 declared, f32 declared_needle_mm, std::string_view species,
                     world_sound::CanopyClass& out, f32& needle_mm) noexcept {
  // The needles' width as the material generator draws a spray (matgen.schema's
  // `Spray.needle_width_um`: 1,200 um by default), where neither the mesh nor its species says.
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
  // The species' own declaration (S112-3, N3: flora_species.h `FloraSpeciesInfo::canopy`), never
  // its name: a species that declares no crown is silent, and so is a new one until it says.
  const geometry::FloraSpeciesInfo* info = geometry::flora_species_info(species);
  if (info == nullptr) return false;  // a mesh that says nothing is silent
  switch (info->canopy) {
    case geometry::FloraCanopy::blade: out = world_sound::CanopyClass::Blade; return true;
    case geometry::FloraCanopy::needle:
      if (info->needle_um != 0) needle_mm = static_cast<f32>(info->needle_um) * 0.001f;
      out = world_sound::CanopyClass::Needle;
      return true;
    case geometry::FloraCanopy::none: return false;
  }
  return false;
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

// ---- running water and the surf (S113) ----------------------------------------------------------

void GameSoundHost::attach_water(const renderer::SceneData& scene) {
  stretches_.clear();
  surf_.clear();
  // The solved channels: a stretch between each two neighbouring stations, as the solve wrote
  // them (renderer::SceneWater::channel_source, the scene's description).
  for (const renderer::SceneMeshInfo& info : scene.mesh_info) {
    if (!info.water.has_value() || !info.water->channel) continue;
    const scene::WaterChannel& c = info.water->channel_source;
    const u32 n = static_cast<u32>(c.points.size());
    if (n < 2 || c.discharge_m3s.size() != n || c.flags.size() != n) continue;
    // **A run of stations is one stretch up to `k_stretch_join_m`** (the forest staging's solve
    // writes 39,142 stretches of a few metres; S113's first flight): neighbouring stretches that
    // are not falls, carry the same water within a tenth and give up some level are joined, their
    // drops summed and the wider wet width kept, so a line of 32 m is one source — what its sound
    // is from more than a few metres off.
    bool run = false;
    for (u32 i = 0; i + 1 < n; ++i) {
      world_sound::Stretch s;
      s.a = c.points[i];
      s.b = c.points[i + 1];
      s.discharge_m3s = c.discharge_m3s[i];
      const f64 drop = s.a.y - s.b.y;
      s.drop_m = drop > 0.0 ? static_cast<f32>(drop) : 0.0f;
      if (c.wet_left_m.size() == n && c.wet_right_m.size() == n)
        s.wet_width_m = c.wet_left_m[i] + c.wet_right_m[i];
      if (c.froude.size() == n) s.froude = c.froude[i];
      s.fall = (c.flags[i] & scene_gen::k_channel_fall_lip) != 0 &&
               (c.flags[i + 1] & scene_gen::k_channel_fall_foot) != 0;
      s.lands_on_water = s.fall && c.depth_m.size() == n && c.depth_m[i + 1] >= 0.1f;
      if (!(s.drop_m > 0.0f) || !(s.discharge_m3s > 0.0f)) {
        run = false;
        continue;
      }
      if (run && !s.fall) {
        world_sound::Stretch& p = stretches_.back();
        const f64 dx = s.b.x - p.a.x, dy = s.b.y - p.a.y, dz = s.b.z - p.a.z;
        const f32 dq = s.discharge_m3s - p.discharge_m3s;
        if (dx * dx + dy * dy + dz * dz <= k_stretch_join_m * k_stretch_join_m &&
            (dq < 0.0f ? -dq : dq) <= 0.1f * p.discharge_m3s) {
          p.b = s.b;
          p.drop_m += s.drop_m;
          p.wet_width_m = p.wet_width_m > s.wet_width_m ? p.wet_width_m : s.wet_width_m;
          p.froude = p.froude > s.froude ? p.froude : s.froude;
          continue;
        }
      }
      stretches_.push_back(s);
      run = !s.fall;
    }
  }
  // The bodies' surf: each body instance's records, from the field the GPU scene draws its waves
  // from, placed by the instance's rows (a body's mesh is body-local at its level).
  const i64 started = time::monotonic_ns();
  Vector<renderer::SurfRecord> records;
  for (const gfx::InstanceDesc& in : scene.instances) {
    if (in.mesh >= scene.mesh_info.size()) continue;
    const renderer::SceneMeshInfo& info = scene.mesh_info[in.mesh];
    if (!info.water.has_value() || info.water->channel) continue;
    renderer::water_surf_records(*info.water, 16.0, records);
    for (const renderer::SurfRecord& r : records) {
      const auto row = [&](u32 k) {
        return static_cast<f64>(in.rows[k].x * r.at.x + in.rows[k].z * r.at.y + in.rows[k].w);
      };
      world_sound::Surf s;
      s.at = WorldPos{static_cast<f64>(in.cell.x) * k_world_cell_m + row(0),
                      static_cast<f64>(in.cell.y) * k_world_cell_m + row(1),
                      static_cast<f64>(in.cell.z) * k_world_cell_m + row(2)};
      s.along = Vec2{in.rows[0].x * r.along.x + in.rows[0].z * r.along.y,
                     in.rows[2].x * r.along.x + in.rows[2].z * r.along.y};
      s.length_m = r.length_m;
      s.power_w_m = r.power_w_m;
      s.height_m = r.height_m;
      s.surf_similarity = r.surf_similarity;
      s.period_s = r.period_s;
      s.arrival_s = r.arrival_s;
      surf_.push_back(s);
    }
  }
  surf_ns_ = time::monotonic_ns() - started;
  if (!stretches_.empty() || !surf_.empty()) {
    std::fprintf(stderr,
                 "engine-view: sound: %u stretches of running water, %u of surf (%.0f ms)\n",
                 static_cast<u32>(stretches_.size()), static_cast<u32>(surf_.size()),
                 static_cast<f64>(surf_ns_) / 1.0e6);
  }
}

void GameSoundHost::visit_flow(void* user, const WorldPos& centre, f64 reach_m,
                               const world_sound::StretchSink& sink) {
  const GameSoundHost& h = *static_cast<const GameSoundHost*>(user);
  for (const world_sound::Stretch& s : h.stretches_) {
    const f64 x = 0.5 * (s.a.x + s.b.x);
    const f64 z = 0.5 * (s.a.z + s.b.z);
    if (distance_xz(x, z, centre) <= reach_m) sink.emit(sink.user, s);
  }
}

void GameSoundHost::visit_surf(void* user, const WorldPos& centre, f64 reach_m,
                               const world_sound::SurfSink& sink) {
  const GameSoundHost& h = *static_cast<const GameSoundHost*>(user);
  for (const world_sound::Surf& s : h.surf_)
    if (distance_xz(s.at.x, s.at.z, centre) <= reach_m) sink.emit(sink.user, s);
}

void GameSoundHost::attach_scene(const renderer::SceneData& scene) {
  scene_ = &scene;
  if (world_) attach_water(scene);  // running water and the surf (S113)
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
// first changed tile always taken so a tile larger than the allowance is still made). **Nearest
// the listener first** (S112-3, N2): after a load or a cut the ring fills from the listener's own
// trees outward, not in key order.
void GameSoundHost::refresh_tiles(const WorldPos& listener, u32& allowance) {
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
      // A tile no longer drawn leaves its crowns' room for the next one drawn, so a walk that
      // trades tiles allocates nothing once the room has grown.
      while (r < tile_groups_.size() && tile_groups_[r].g.key < t.key)
        spare_crowns_.push_back(std::move(tile_groups_[r++].crowns));
      if (r < tile_groups_.size() && tile_groups_[r].g.key == t.key) {
        tile_next_.push_back(std::move(tile_groups_[r++]));
      } else {
        tile_next_.push_back(TileGroup{});
        tile_next_.back().g.key = t.key;
        if (!spare_crowns_.empty()) {
          tile_next_.back().crowns = std::move(spare_crowns_.back());
          tile_next_.back().crowns.clear();
          spare_crowns_.pop_back();
        }
      }
    }
    while (r < tile_groups_.size())
      spare_crowns_.push_back(std::move(tile_groups_[r++].crowns));
    tile_groups_.swap(tile_next_);
    tile_next_.clear();
  }
  counters_.tile_groups = static_cast<u32>(tile_groups_.size());
  // The changed tiles alone, by their distance: a made tile's leaves' centroid, a new one's first
  // placement (within a tile of its leaves). Sorted are only these, at most the tiles held, in a
  // scratch that keeps its room; a steady world collects none and sorts nothing.
  changed_.clear();
  for (u32 i = 0; i < tiles_now_.size(); ++i) {
    const SoundTile& t = tiles_now_[i];
    const TileGroup& tg = tile_groups_[i];
    if (tg.made && tg.g.version == t.version) continue;
    f64 d = 0.0;
    if (tg.made) {
      d = distance_xz(tg.g.x, tg.g.z, listener);
    } else if (t.count > 0) {
      d = distance_xz(t.instances[0].origin.x, t.instances[0].origin.z, listener);
    }
    changed_.push_back(Changed{d, t.key, i});
  }
  if (changed_.size() > 1) {
    std::sort(changed_.begin(), changed_.end(), [](const Changed& a, const Changed& b) {
      return a.d != b.d ? a.d < b.d : a.key < b.key;
    });
  }
  bool took = false;
  for (const Changed& next : changed_) {
    const SoundTile& t = tiles_now_[next.index];
    TileGroup& tg = tile_groups_[next.index];
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
  const i64 started = time::monotonic_ns();
  h.refresh_tiles(centre, allowance);
  const i64 walked = time::monotonic_ns() - started;
  h.counters_.walk_ns += walked;
  h.counters_.walk_ns_max = walked > h.counters_.walk_ns_max ? walked : h.counters_.walk_ns_max;
  struct Timed {
    Counters& c;
    i64 started;
    ~Timed() {
      const i64 ns = time::monotonic_ns() - started;
      c.visit_ns += ns;
      c.visit_ns_max = ns > c.visit_ns_max ? ns : c.visit_ns_max;
    }
  } timed{h.counters_, started};
  // Every group: past the reach nothing, far its merged crowns, near kept for the pass below.
  h.near_.clear();
  const f64 coarse_m = 4.0 * near_m;
  const i64 lx = static_cast<i64>(std::floor(centre.x / k_coarse_m));
  const i64 lz = static_cast<i64>(std::floor(centre.z / k_coarse_m));
  for (Coarse& bin : h.coarse_)
    bin = Coarse{};
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
    if (d - g.radius > coarse_m) {
      // Past four times the near distance a group is summed into its 512 m cell round the
      // listener's (`k_coarse_m`): a few hundred crowns a refresh, not a crown or three a tile.
      const i64 cx = static_cast<i64>(std::floor(g.x / k_coarse_m)) - lx;
      const i64 cz = static_cast<i64>(std::floor(g.z / k_coarse_m)) - lz;
      const i64 r = static_cast<i64>(k_coarse_span / 2);
      const u32 at = static_cast<u32>((std::clamp(cx, -r, r) + r) * k_coarse_span +
                                      (std::clamp(cz, -r, r) + r));
      Coarse& bin = h.coarse_[at];
      for (u32 k = 0; k < 3; ++k) {
        const world_sound::Crown& m = g.merged[k];
        const f64 a = static_cast<f64>(m.leaf_area_m2);
        if (!(a > 0.0)) continue;
        bin.area[k] += a;
        bin.x[k] += a * m.at.x;
        bin.y[k] += a * m.at.y;
        bin.z[k] += a * m.at.z;
        bin.height[k] += a * static_cast<f64>(m.height_m);
        bin.needle[k] += a * static_cast<f64>(m.needle_mm);
      }
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
  for (const Coarse& bin : h.coarse_) {
    for (u32 k = 0; k < 3; ++k) {
      if (!(bin.area[k] > 0.0)) continue;
      world_sound::Crown m;
      m.at = WorldPos{bin.x[k] / bin.area[k], bin.y[k] / bin.area[k], bin.z[k] / bin.area[k]};
      m.height_m = static_cast<f32>(bin.height[k] / bin.area[k]);
      m.leaf_area_m2 = static_cast<f32>(bin.area[k]);
      m.kind = static_cast<world_sound::CanopyClass>(k);
      m.needle_mm = static_cast<f32>(bin.needle[k] / bin.area[k]);
      m.extent_m = static_cast<f32>(0.5 * k_coarse_m);
      m.merged = true;
      sink.emit(sink.user, m);
      ++h.counters_.coarse_sent;
    }
  }
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
bool g_solo_summarised = false;  // print_solo_summary's lines are a run's once
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
bool open_solo_sound(GameSoundHost& host, std::string_view audio, const AudioRun& run,
                     const std::string& wav_out, const std::string& scene, u32 tick_hz,
                     std::string* error) {
  world_sound::AudioMode mode = world_sound::AudioMode::Off;
  std::string device;
  if (!resolve_audio(audio, run, mode, device, error)) return false;
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
  // The flight's world clock, its frames over their rate (at a world rate of one): the water's.
  h->set_world_time(static_cast<f64>(step.frame) / static_cast<f64>(renderer::k_frame_hz));
  h->tick_at(step.camera.position, renderer::camera_forward(step.camera), gfx::WindLive{}, sky);
  if (step.world == nullptr) return;
  // The sound's tick is a flight frame's only one: in `ticks_ms` too, so `other_ms` leaves it out.
  const f64 ms = static_cast<f64>(h->sound_ns() - before) / 1.0e6;
  step.world->sound_ms += ms;
  step.world->ticks_ms += ms;
}
void print_solo_summary() {
  if (!g_solo || g_host == nullptr || g_solo_summarised) return;
  g_solo_summarised = true;
  std::string error;
  (void)g_host->stage().close(&error);
  std::fprintf(
      stderr, "engine-view: sound summary %s\n",
      write_json(g_host->stage().summary_json(), JsonWriteOptions{.pretty = false}).c_str());
  const GameSoundHost::Counters& c = g_host->counters();
  std::fprintf(
      stderr,
      "engine-view: sound table: %llu refreshes, %llu tile placements walked in %llu "
      "tiles (%llu carried), %llu crowns, %llu merged and %llu by cell sent, %u tiles "
      "held; walk %.3f ms a refresh (most %.3f), visit %.3f (most %.3f)\n",
      static_cast<unsigned long long>(c.refreshes),
      static_cast<unsigned long long>(c.instances_walked),
      static_cast<unsigned long long>(c.tiles_walked),
      static_cast<unsigned long long>(c.tiles_carried),
      static_cast<unsigned long long>(c.crowns_sent),
      static_cast<unsigned long long>(c.merged_sent),
      static_cast<unsigned long long>(c.coarse_sent), c.tile_groups,
      c.refreshes != 0 ? static_cast<f64>(c.walk_ns) / 1e6 / static_cast<f64>(c.refreshes) : 0.0,
      static_cast<f64>(c.walk_ns_max) / 1e6,
      c.refreshes != 0 ? static_cast<f64>(c.visit_ns) / 1e6 / static_cast<f64>(c.refreshes) : 0.0,
      static_cast<f64>(c.visit_ns_max) / 1e6);
}

}  // namespace engine::view
