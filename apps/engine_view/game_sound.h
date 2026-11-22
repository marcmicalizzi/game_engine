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

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <systems/world_sound/world_sound.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::renderer {
struct SceneInstance;
struct FlightStep;
}  // namespace engine::renderer

namespace engine::view {

// `--audio auto|off|null|<device>`: `auto` is the default device for a session with a window,
// whatever else it does (a `--benchmark` or a `--capture` in a window is a person's session: the
// owner's launcher always records frames), and `off` offscreen; any other word is a device's name.
// False with `error` for an empty name.
bool resolve_audio(std::string_view flag, bool windowed, world_sound::AudioMode& mode,
                   std::string& device, std::string* error);

// The listener's two axes from the camera's heading and pitch (fly_camera.h: heading 0 looks along
// -z and turns counter-clockwise from above; pitch up is positive).
void listener_axes(f32 heading, f32 pitch, Vec3& forward, Vec3& up) noexcept;

// **A crown's class is declared, never read from a name** (S112-2; world_sound.md, "A crown's
// class is declared"): the scene's own word on the mesh (`Mesh.sound`, `declared` the
// `scene::SoundCanopy` value or `renderer::SceneMeshInfo::k_sound_undeclared`) when it said one;
// else the species a generated tree's container records (the conifers' needles at the width the
// material generator draws them, a broadleaf's blades; the ground's cover — sward, cover, tuft,
// herb, fern, ivy, roots, litter — is not a crown); else nothing. False: the mesh is silent, so a
// fence, a grating or a decal is never measured as leaf.
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
  // session's tick t seconds in, as the frame's does; and a flight's frame, its camera and sky.
  void set_clock(f64 sky_start_s, f64 sun_rate, u32 tick_hz) noexcept;
  static void observe(void* user, const FlySession& session, const input::InputState& input);
  void tick_at(const WorldPos& eye, const Vec3& forward, const gfx::WindLive& live,
               f64 sky_time_s) noexcept;
  // The wall time this host's own ticks took (`observe`, `tick_at`), nanoseconds: a frame's
  // `sound_ms` without a module (with one, the runner's `sound_ns`).
  i64 sound_ns() const noexcept { return sound_ns_; }

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
    u32 last_walked = 0;       // the last refresh's tile placements walked
    u32 tile_groups = 0;       // tiles held now
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
  void refresh_tiles(u32& allowance);
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
  Vector<TileGroup> tile_groups_;  // by key, ascending
  Vector<TileGroup> tile_next_;    // the re-keying's scratch
  Vector<SoundTile> tiles_now_;    // the list's scratch
  Vector<Near> near_;              // a refresh's near groups, nearest first
  Counters counters_;
  // Without a module: the session's clock and the listener's last place.
  f64 sky_start_s_ = 0.0;
  f64 sun_rate_ = 0.0;
  u32 tick_hz_ = 240;
  i64 sound_ns_ = 0;
};

// **The session's sound as engine-view's loops reach it** (S112-2): the one host a process has,
// set by `run_view` (`set_session_sound`; `solo` when no module ticks it), so main.cpp holds no
// sound of its own and needs no guard round these: with the capability out its stubs answer
// nothing. Each does nothing without a host, and the `solo_*` ones nothing with a module's.
void set_session_sound(GameSoundHost* host, bool solo) noexcept;
// Opens `host` for a session with no module and makes it the session's: `--audio` resolved as a
// module's is, and nothing opened for `off`. False with `error` as `resolve_audio` and `open`.
bool open_solo_sound(GameSoundHost& host, std::string_view audio, bool windowed,
                     const std::string& wav_out, const std::string& scene, u32 tick_hz,
                     std::string* error);
bool sound_hears_world() noexcept;  // the load measures leaves (`SceneDesc::measure_leaves`, M7)
i64 solo_sound_ns() noexcept;       // the record's `sound_ms` without a module
void attach_solo_scene(const renderer::SceneData& scene);
void attach_solo_session(FlySession& session, f64 sky_start_s, f64 sun_rate) noexcept;
void attach_solo_tiles(void* placements) noexcept;  // a world::PlacementTiles*
// A flight's frames, a tick each (a sixtieth of a second, the flight's clock), heard from each
// frame's camera, the time added to its record's `world.sound_ms` and `ticks_ms`.
void set_solo_flight_clock(f64 sky_start_s, f64 sun_rate) noexcept;
void solo_flight_frame(const renderer::FlightStep& step) noexcept;
// A module-less run's summary, which has no game block to sit in: two lines on stderr.
void print_solo_summary();

}  // namespace engine::view
