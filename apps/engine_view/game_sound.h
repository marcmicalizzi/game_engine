#pragma once

// **The session's sound for a game** (roadmap S111; docs/subsystems/apps.md, "Sound a module
// plays"; the stage is systems/world_sound's): `--audio` resolved to a mode, the scene's `sound`
// block read, a `world_sound::SoundStage` opened over it, and the game runner's sound hooks set to
// it, so the module's `sound_clip`, `sound_play` and `sound_stop` reach the stage and the stage is
// ticked once a session tick with the camera as the listener. Compiled only where the capability is
// (ENGINE_VIEW_WORLD_SOUND); without it the runner has no hooks and the calls answer `Unsupported`.

#include "game_runner.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <systems/world_sound/world_sound.h>

#include <span>
#include <string>
#include <string_view>

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

// **A crown's class from its mesh** (S112): a generated tree's species (`SourceMesh::species`,
// geometry's `FloraSpecies`) — the conifers' needles, with the width the material generator
// draws them at — and a mesh that says nothing is a broadleaf (`Blade`). Nothing keys on which
// generator made a mesh beyond the species name a container records.
world_sound::CanopyClass canopy_class_of(std::string_view species, f32& needle_mm) noexcept;

class GameSoundHost {
 public:
  // Reads `scene`'s `sound` block (none: every sound call answers `Unsupported`), opens the stage
  // in `mode` and sets `game`'s hooks. Before `game.start`, since the module's `on_load` names its
  // clips. False with `error` for a block that does not read or a clip that does not decode.
  // Unless `Off`, the world's sound is on (S112): the wind in the scene's crowns and at the ears.
  bool open(world_sound::AudioMode mode, const std::string& device, const std::string& wav_out,
            const std::string& scene, u32 tick_hz, GameRunner& game, std::string* error);
  world_sound::SoundStage& stage() noexcept { return stage_; }
  const world_sound::SoundStage& stage() const noexcept { return stage_; }
  // The loaded scene's wind (`SceneData::wind`), or null before the scene is attached.
  const gfx::WindDesc* scene_wind() const noexcept;

  // **The canopy table** (S112; world_sound.h `CanopySource`): the loaded scene's instances whose
  // mesh has leaves (`SceneData::mesh_leaves`), and a streamed world's drawn placements, a tile
  // past `audio.world.near_m` as one crown a class at its leaves' centroid, made when its version
  // changes. Through the runner's hooks; public for the case that drives one by hand.
  void attach_scene(const renderer::SceneData& scene);
  void attach_tiles(void* placements) noexcept { tiles_ = placements; }
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
    f64 radius = 0.0;      // the farthest crown from it
    world_sound::Crown merged[3];
  };

 private:
  world_sound::Crown crown_of(u32 mesh, const WorldPos& at, f32 scale) const noexcept;
  static void merge(Group& g, std::span<const world_sound::Crown> crowns) noexcept;
  static void emit_group(const Group& g, std::span<const world_sound::Crown> crowns,
                         const WorldPos& centre, f64 reach_m, f64 near_m,
                         const world_sound::CrownSink& sink) noexcept;

  world_sound::SoundStage stage_;
  const renderer::SceneData* scene_ = nullptr;
  void* tiles_ = nullptr;
  Vector<world_sound::CanopyClass> mesh_class_;
  Vector<f32> mesh_needle_mm_;
  Vector<world_sound::Crown> crowns_;  // the loaded instances with leaves, by cell
  Vector<Group> cells_;
  Vector<Group> tile_groups_;
  Vector<world_sound::Crown> tile_scratch_;
};

}  // namespace engine::view
