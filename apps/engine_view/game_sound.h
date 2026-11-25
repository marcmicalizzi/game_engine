#pragma once

// **The session's sound** (roadmap S111, S112; docs/subsystems/apps.md, "Sound a module plays"; the
// stage is systems/world_sound's): `--audio` resolved to a mode, the scene's `sound` block read, a
// `world_sound::SoundStage` opened over it, and ticked once a session tick with the camera as the
// listener. With a game module the runner's sound hooks are set to it (`hooks`), so the module's
// `sound_clip`, `sound_play` and `sound_stop` reach the stage; **without one** (S112-2) the host is
// a session's tick observer of its own (`observe`), or ticked by a flight's frames (`tick_at`), and
// the world is heard all the same. Compiled only where the capability is (ENGINE_VIEW_WORLD_SOUND);
// without it the runner has no hooks and the calls answer `Unsupported`.

#include "game_runner.h"
#include "walk.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <systems/world_sound/world_sound.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {
struct SceneInstance;
struct FlightStep;
struct FlightOptions;
}  // namespace engine::renderer

namespace engine::view {

// **What a run is, for `--audio auto`** (S112-3, N1; apps.md, "Sound a module plays"): plain
// inputs, so the rule is held by a case with no window. main.cpp fills it from its flags.
struct AudioRun {
  bool windowed = false;     // not `--offscreen`
  bool interactive = false;  // `--interactive`: a session at the window (live, injected, replayed)
  bool module = false;       // `--game`
  bool ends_by_itself = false;  // `--frames N`, a capture, a `--benchmark`, a path that ends
  bool ticked = true;           // a session, a flight or a module ticks the stage
  bool scene = true;            // there is a scene to hear
  std::string_view auto_env;    // `$ENGINE_AUDIO_AUTO` (`k_audio_auto_env`), empty when unset
};
// Set by every test main and bench main in its own process (tests/support), so every engine
// binary a case starts inherits it: while it is set `auto` never opens a device — `off` keeps the
// stage closed, any other value is the null backend (the world modelled and summarised, nothing
// played). Spelled again in tests/support/test_environment.h.
inline constexpr const char* k_audio_auto_env = "ENGINE_AUDIO_AUTO";
std::string audio_auto_env();  // the variable's value in this process, empty when unset

// `--audio auto|off|null|<device>`. `auto` is the default device **only for a windowed run that
// somebody attends**: `--interactive`, a game module, or one that runs until its window is closed;
// a run that ends by itself and every offscreen run is `off`, and so is a run with no scene or that
// nothing ticks. Under a test or a bench (`auto_env` set) it is never the device. `off` and `null`
// are themselves; any other word is a device's name, the one way a test reaches the speakers.
// False with `error` for an empty name.
bool resolve_audio(std::string_view flag, const AudioRun& run, world_sound::AudioMode& mode,
                   std::string& device, std::string* error);

// The listener's two axes from the camera's heading and pitch (fly_camera.h: heading 0 looks along
// -z and turns counter-clockwise from above; pitch up is positive).
void listener_axes(f32 heading, f32 pitch, Vec3& forward, Vec3& up) noexcept;

// **A crown's class is declared, never read from a name** (S112-2; world_sound.md, "A crown's
// class is declared"): the scene's own word on the mesh (`Mesh.sound`, `declared` the
// `scene::SoundCanopy` value or `renderer::SceneMeshInfo::k_sound_undeclared`) when it said one;
// else what the species a generated tree's container records declares of itself
// (`geometry::FloraSpeciesInfo::canopy` and `needle_um`, S112-3: this file holds no species'
// names); else nothing. False: the mesh is silent, so a fence, a grating, a decal or a species
// that declares no crown is never measured as leaf.
bool canopy_class_of(u8 declared, f32 declared_needle_mm, std::string_view species,
                     world_sound::CanopyClass& out, f32& needle_mm) noexcept;

// One drawn tile's own placements for the canopy table, by key (world::PlacementTiles::tile_own,
// or a case's own list), and the version of what it draws.
struct SoundTile {
  u64 key = 0;
  const renderer::SceneInstance* instances = nullptr;
  u32 count = 0;
  u64 version = 0;
};
using SoundTileList = void (*)(const void* user, Vector<SoundTile>& out);

// **What shelters the walker's ears** (S114; world_sound.md, "Distance"): the stage's
// `ShelterSource` over the walker's collision. `cast` is a downward ray from `origin` for `length`
// metres against what the walker stands among (`Walker::cast` in a session; a case's own world in
// its test), and `ground` the ground's own height at a cast's (x, z) (`Walker::collision_ground`:
// the collision's heightfield), false where the collision holds none there. What stands at (x, z)
// is measured **over the ground at (x, z)**, never over the feet (S114-M1): a cast that meets the
// ground itself (`WalkCast::what` 1) meets no obstacle, so a slope rising into the wind shelters
// nothing and a wall on that slope counts its own height; only where the collision holds no ground
// under a cast (a scene whose floor is a mesh) is its hit measured over `feet_y`, the walker's
// feet. The height is the **least** of three casts a pace apart across the wind
// (`k_shelter_across_m` either side): a wall or a house stands under all three and a trunk's proxy
// under one at most, so a stand of trees is not taken for a wall and the ears' wind does not drop
// 21 dB at one step and come back at the next. 0 where any cast meets nothing or the ground.
using ShelterCast = bool (*)(const void* user, WorldPos origin, f32 length, WalkCast& out);
using ShelterGround = bool (*)(const void* user, f64 x, f64 z, f64& out);
inline constexpr f32 k_shelter_across_m = 1.5f;
inline constexpr f32 k_shelter_cast_above_m = 20.0f;  // from this far over the feet, 40 m down
f32 shelter_height_at(ShelterCast cast, ShelterGround ground, const void* user, f64 feet_y, f64 x,
                      f64 z, f32 across_x, f32 across_z) noexcept;
// The shelter over a walker's own collision: its feet as they stand now (`Walker::feet`, crouched
// or not; S114-H1), its casts and its collision's ground. 0 with no walker.
f32 walker_shelter(const Walker* walker, f64 x, f64 z, f32 across_x, f32 across_z) noexcept;

class GameSoundHost {
 public:
  // Reads `scene`'s `sound` block (none: every clip call answers `Unsupported`) and opens the stage
  // in `mode`. Before a game's `start`, since the module's `on_load` names its clips. False with
  // `error` for a block that does not read or a clip that does not decode. Unless `Off`, the
  // world's sound is on (S112): the wind in the scene's crowns and at the ears.
  bool open(world_sound::AudioMode mode, const std::string& device, const std::string& wav_out,
            const std::string& scene, u32 tick_hz, std::string* error);
  bool world() const noexcept { return world_; }
  // The runner's hooks to this host (`GameRunner::set_sound`).
  GameSound hooks() noexcept;
  world_sound::SoundStage& stage() noexcept { return stage_; }
  const world_sound::SoundStage& stage() const noexcept { return stage_; }
  // The loaded scene's wind (`SceneData::wind`), or null before the scene is attached; and the
  // wind the sway uses with no live wind, the hand tunable's included (M5 of S112's review).
  const gfx::WindDesc* scene_wind() noexcept;

  // **Without a game module** (S112-2): a session's tick observer (`FlySession::
  // set_tick_observer`, user this host), whose sky stands `sky_start_s + sun_rate * t` at the
  // session's tick t seconds in, as the frame's does, and its world `world_rate * t` (the gust's,
  // S118); and a flight's frame, its camera, its sky and its world.
  void set_clock(f64 sky_start_s, f64 sun_rate, f64 world_rate, u32 tick_hz) noexcept;
  static void observe(void* user, const FlySession& session, const input::InputState& input);
  void tick_at(const WorldPos& eye, const Vec3& forward, const gfx::WindLive& live, f64 sky_time_s,
               f64 world_time_s) noexcept;
  // The wall time this host's own ticks took (`observe`, `tick_at`), nanoseconds: a frame's
  // `sound_ms` without a module (with one, the runner's `sound_ns`).
  i64 sound_ns() const noexcept { return sound_ns_; }
  // **The walker the ears stand behind** (S114): a session's (`FlySession::walker`, a module's
  // through the runner's `GameSound::walker`), whose collision the stage's shelter casts into, only
  // while the session walks; null (a session that flies, a flight's frames, a build with no
  // collision, after the runner's finish) shelters nothing.
  void set_walker(const Walker* walker) noexcept;
  const Walker* walker() const noexcept { return walker_; }

  // **The canopy table** (S112; world_sound.h `CanopySource`): the loaded scene's instances whose
  // mesh has leaves and a class (`SceneData::mesh_leaves`, `canopy_class_of`), and a streamed
  // world's drawn tiles, each tile's crowns made again only when its version moves.
  void attach_scene(const renderer::SceneData& scene);
  void attach_tiles(void* placements) noexcept;  // a world::PlacementTiles*
  void attach_tile_list(SoundTileList list, const void* user) noexcept {
    tile_list_ = list;
    tile_user_ = user;
  }
  static void visit(void* user, const WorldPos& centre, f64 reach_m,
                    const world_sound::CrownSink& sink);

  // **The flow and surf tables** (S113; world_sound.h `FlowSource`, `SurfSource`): every stretch
  // of the loaded scene's solved channels — a stretch's two stations at their solved levels, its
  // discharge (the upstream station's), the level given up, its wet width (`wet_left_m` plus
  // `wet_right_m`) and Froude number, a fall where a lip meets its foot, landing on water where the
  // foot's solved depth is 0.1 m or more — made once when the scene is attached, and every body's
  // surf records, scanned once a body mesh by the GPU scene from the field it built for its waves
  // (`renderer::SurfTable`, one a 16 m square where the field loses wave energy) and placed by each
  // instance (`attach_surf`, after the upload). The fields of the description read: `points`,
  // `discharge_m3s`, `depth_m`, `wet_left_m`, `wet_right_m`, `froude`, `flags`. **A refresh visits
  // what is in reach** (M2): both tables are bucketed by 128 m cell at attach, and a visit walks
  // the cells within the sink's reach (streams and surf `audio.world.water_reach_m`, falls
  // `reach_m`).
  void attach_surf(const renderer::SurfTable& table, const renderer::SceneData& scene);
  // Neighbouring stretches that carry the same water join into one up to this long (S113).
  static constexpr f64 k_stretch_join_m = 32.0;
  static void visit_flow(void* user, const WorldPos& centre, f64 reach_m,
                         const world_sound::StretchSink& sink);
  static void visit_surf(void* user, const WorldPos& centre, f64 reach_m,
                         const world_sound::SurfSink& sink);
  u32 stretches() const noexcept { return static_cast<u32>(stretches_.size()); }
  u32 surf_records() const noexcept { return static_cast<u32>(surf_.size()); }
  i64 surf_ns() const noexcept { return surf_ns_; }  // the scans (the upload's) and the placing
  u32 water_cells_walked() const noexcept { return water_cells_walked_; }  // the last visits'
  struct WaterCell {  // a 128 m cell's run of a table, ordered by cell (M2)
    u64 key = 0;
    u32 first = 0;
    u32 count = 0;
  };
  // The water's clock (S113): the world clock a tick of the stage stands at — a session's ticks
  // over its rate, a flight's frames over 60 — so a wave heard breaking is the wave drawn
  // breaking (at a world rate of one).
  void set_world_time(f64 world_time_s) noexcept { world_time_s_ = world_time_s; }
  f64 world_time() const noexcept { return world_time_s_; }
  void advance_world_time() noexcept { world_time_s_ += 1.0 / static_cast<f64>(tick_hz_); }

  // A group's crowns merged, one a class, at their leaves' centroid: a 128 m cell of the loaded
  // instances, or a placement tile (made again when its version changes).
  struct Group {
    u64 key = 0;
    u64 version = 0;
    u32 first = 0;  // the loaded cells': their crowns in `crowns_`
    u32 count = 0;
    f64 x = 0.0, z = 0.0;  // the leaves' centroid
    f64 radius = 0.0;      // the farthest crown's edge from it
    world_sound::Crown merged[3];
  };
  // **What a refresh did, counted** (S112-2, H1): what the cases hold a refresh's cost to.
  struct Counters {
    u64 refreshes = 0;
    u64 instances_walked = 0;  // tile placements walked to make crowns, all refreshes
    u64 tiles_walked = 0;      // tiles whose crowns were made again
    u64 tiles_carried = 0;     // changed tiles left for a later refresh by the allowance
    u64 crowns_sent = 0;       // crowns sent one by one
    u64 merged_sent = 0;       // merged groups' crowns sent
    u64 coarse_sent = 0;       // far groups summed by 512 m cell, sent
    u32 last_walked = 0;       // the last refresh's tile placements walked
    // Wall time: the tiles' walk and the whole visit, summed and the most one refresh took.
    i64 walk_ns = 0, walk_ns_max = 0, visit_ns = 0, visit_ns_max = 0;
    u32 tile_groups = 0;  // tiles held now
  };
  const Counters& counters() const noexcept { return counters_; }

 private:
  struct TileGroup {
    Group g;
    bool made = false;
    Vector<world_sound::Crown> crowns;
  };
  struct Near {
    f64 d = 0.0;
    u64 key = 0;
    const Group* g = nullptr;
    const world_sound::Crown* crowns = nullptr;
  };
  // Each mesh's leaves as its instances are placed through its fit (the loaded instances' rows
  // carry it; a tile's placement does not, L1): the foot under the leaves' centre, the height of
  // their middle over it and the fit's scale.
  struct Fitted {
    Vec3 foot{};
    f32 mid = 0.0f;
    f32 scale = 1.0f;
  };
  bool tile_crown(const renderer::SceneInstance& s, world_sound::Crown& out) const noexcept;
  void refresh_tiles(const WorldPos& listener, u32& allowance);
  static void merge(Group& g, std::span<const world_sound::Crown> crowns) noexcept;
  void tick_listener(const WorldPos& eye, const Vec3& forward, const Vec3& up) noexcept;

  world_sound::SoundStage stage_;
  bool world_ = false;
  const renderer::SceneData* scene_ = nullptr;
  gfx::WindDesc wind_held_{};  // the hand tunable's wind, when set (`scene_wind`)
  SoundTileList tile_list_ = nullptr;
  const void* tile_user_ = nullptr;
  Vector<u8> mesh_heard_;  // 1: a crown; 0: silent (no leaves, or no class)
  Vector<world_sound::CanopyClass> mesh_class_;
  Vector<f32> mesh_needle_mm_;
  Vector<Fitted> mesh_fitted_;
  Vector<world_sound::Crown> crowns_;  // the loaded instances with leaves, by cell
  Vector<Group> cells_;
  Vector<TileGroup> tile_groups_;                    // by key, ascending
  Vector<TileGroup> tile_next_;                      // the re-keying's scratch
  Vector<Vector<world_sound::Crown>> spare_crowns_;  // let-go tiles' room, for the next drawn
  Vector<SoundTile> tiles_now_;                      // the list's scratch
  // A refresh's changed tiles, nearest the listener first (S112-3, N2): what is walked first.
  struct Changed {
    f64 d = 0.0;
    u64 key = 0;
    u32 index = 0;
  };
  Vector<Changed> changed_;
  Vector<Near> near_;  // a refresh's near groups, nearest first
  // **The far field by cell** (S112-2): past four times `audio.world.near_m` a group is summed
  // into a 512 m cell of a 9 x 9 grid round the listener's (the reach's 2 km inside it), one
  // crown a class a cell, so a refresh sends a few hundred far crowns however many tiles are
  // drawn. Fixed: nothing allocates.
  static constexpr f64 k_coarse_m = 512.0;
  static constexpr u32 k_coarse_span = 9;
  struct Coarse {
    f64 area[3] = {}, x[3] = {}, y[3] = {}, z[3] = {}, height[3] = {}, needle[3] = {};
  };
  Coarse coarse_[k_coarse_span * k_coarse_span];
  Counters counters_;
  // Without a module: the session's clock and the listener's last place.
  f64 sky_start_s_ = 0.0;
  f64 sun_rate_ = 0.0;
  f64 world_rate_ = 1.0;
  u32 tick_hz_ = 240;
  i64 sound_ns_ = 0;
  // The shelter's walker (S114).
  const Walker* walker_ = nullptr;
  // Running water and the surf (S113): the loaded scene's, made at attach.
  void attach_water(const renderer::SceneData& scene);
  Vector<world_sound::Stretch> stretches_;  // the streams, then the falls, each by cell
  Vector<world_sound::Surf> surf_;          // by cell
  Vector<WaterCell> stream_cells_, fall_cells_, surf_cells_;
  f64 stream_half_max_ = 0.0;  // the longest stream's half, the visits' margin
  f64 surf_half_max_ = 0.0;
  mutable u32 water_cells_walked_ = 0;
  i64 surf_ns_ = 0;
  f64 world_time_s_ = 0.0;
  static f32 shelter(void* user, f64 x, f64 z, f32 across_x, f32 across_z) noexcept;
};

// **The session's sound as engine-view's loops reach it** (S112-2): the one host a process has,
// set by `run_view` (`set_session_sound`; `solo` when no module ticks it), so main.cpp holds no
// sound of its own and needs no guard round these: with the capability out its stubs answer
// nothing. Each does nothing without a host, and the `solo_*` ones nothing with a module's.
void set_session_sound(GameSoundHost* host, bool solo) noexcept;
// Opens `host` for a session with no module and makes it the session's: `--audio` resolved as a
// module's is, and nothing opened for `off`. False with `error` as `resolve_audio` and `open`.
bool open_solo_sound(GameSoundHost& host, std::string_view audio, const AudioRun& run,
                     const std::string& wav_out, const std::string& scene, u32 tick_hz,
                     std::string* error);
bool sound_hears_world() noexcept;  // the load measures leaves (`SceneDesc::measure_leaves`, M7)
i64 solo_sound_ns() noexcept;       // the record's `sound_ms` without a module
void attach_solo_scene(const renderer::SceneData& scene);
// The surf the GPU scene scanned at its upload (S113): to the session's host, module or none.
void attach_session_surf(const renderer::SurfTable& table, const renderer::SceneData& scene);
// The job system the session's world sound computes its refresh on (S122: its Efficiency pool,
// main.cpp's `page_jobs`); null hands it back, waiting for a refresh in flight, so a loop hands
// null back before its job system goes (`SessionSoundJobs`).
void set_session_sound_jobs(jobs::JobSystem* jobs) noexcept;
struct SessionSoundJobs {
  explicit SessionSoundJobs(jobs::JobSystem& jobs) noexcept { set_session_sound_jobs(&jobs); }
  ~SessionSoundJobs() { set_session_sound_jobs(nullptr); }
  SessionSoundJobs(const SessionSoundJobs&) = delete;
  SessionSoundJobs& operator=(const SessionSoundJobs&) = delete;
};
void attach_solo_session(FlySession& session, f64 sky_start_s, f64 sun_rate,
                         f64 world_rate) noexcept;
void attach_solo_tiles(void* placements) noexcept;  // a world::PlacementTiles*
// A flight's frames, a tick each (a sixtieth of a second, the flight's clock), heard from each
// frame's camera, the time added to its record's `world.sound_ms` and `ticks_ms`.
void set_solo_flight_clock(const renderer::FlightOptions& flight) noexcept;  // its sky and world
void solo_flight_frame(const renderer::FlightStep& step) noexcept;
// A module-less run's summary, which has no game block to sit in: two lines on stderr, once a
// run. **Before the JSON summary, never after it**: a run's last line is its summary, and every
// reader of a run takes it from there, so both loops call this ahead of that line; main's scope
// calls it again for a run that ends with no summary, and that call prints nothing twice.
void print_solo_summary();

}  // namespace engine::view
