#pragma once

// **The session's sound for a game** (roadmap S111; docs/subsystems/apps.md, "Sound a module
// plays"; the stage is systems/world_sound's): `--audio` resolved to a mode, the scene's `sound`
// block read, a `world_sound::SoundStage` opened over it, and the game runner's sound hooks set to
// it, so the module's `sound_clip`, `sound_play` and `sound_stop` reach the stage and the stage is
// ticked once a session tick with the camera as the listener. Compiled only where the capability is
// (ENGINE_VIEW_WORLD_SOUND); without it the runner has no hooks and the calls answer `Unsupported`.

#include "game_runner.h"

#include <core/base/types.h>
#include <systems/world_sound/world_sound.h>

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

class GameSoundHost {
 public:
  // Reads `scene`'s `sound` block (none: every sound call answers `Unsupported`), opens the stage
  // in `mode` and sets `game`'s hooks. Before `game.start`, since the module's `on_load` names its
  // clips. False with `error` for a block that does not read or a clip that does not decode.
  bool open(world_sound::AudioMode mode, const std::string& device, const std::string& wav_out,
            const std::string& scene, u32 tick_hz, GameRunner& game, std::string* error);
  world_sound::SoundStage& stage() noexcept { return stage_; }

 private:
  world_sound::SoundStage stage_;
};

}  // namespace engine::view
