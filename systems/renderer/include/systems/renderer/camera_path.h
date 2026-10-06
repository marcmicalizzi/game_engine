#pragma once

// A scripted camera (docs/subsystems/renderer.md, "Scenes, camera paths and flythroughs"; schema
// `engine.scene.CameraPath`). Keyframes of time, position, a look-at point or an orientation, and
// a field of view, sampled linearly or through a cubic, so that a flythrough is the same camera
// on every run and every machine and a frame index names one view of the scene.
//
// **A path is resolved once, against the scene it flies over.** A key may hold its height above
// the terrain rather than an absolute one, and an orientation rather than a point to look at; both
// are turned into plain positions and look-at points when the file is read, so sampling is pure
// arithmetic on resolved keys and costs nothing a frame.
//
// **Positions are the world's, in f64** (ADR-0053; `CameraKey` version 2): a key is the file's
// numbers as written, and a sample is the segment's first key plus a displacement computed from
// the keys' differences. So a path moved by a whole number of cells samples to the same
// displacements, and its cameras move by exactly as much wherever f64 holds the sum (a path whose
// numbers and times are whole 1024ths and powers of two, as the translation suite's are); in
// general a far sample rounds at f64's own step there, 2 nm at 10,000 km.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/view_set.h>

#include <string>
#include <string_view>

namespace engine::renderer {

struct CameraPathKey {
  f64 time = 0.0;  // seconds
  WorldPos position{};
  WorldPos target{};          // where the camera looks; always resolved
  f32 fov_y = 0.9599310886f;  // radians
};

struct CameraPathMarker {
  u32 frame = 0;  // at the path's own fps
  std::string name;
  std::string note;
};

struct CameraPath {
  std::string name;
  bool smooth = true;  // cubic through the keys; false is straight segments
  f32 fps = 60.0f;
  f32 znear = 0.1f;
  Vector<CameraPathKey> keys;
  Vector<CameraPathMarker> markers;
  u64 hash = 0;  // of the file's bytes: what a run's summary names the path by

  f64 start() const noexcept { return keys.empty() ? 0.0 : keys[0].time; }
  f64 duration() const noexcept {
    return keys.size() < 2 ? 0.0 : keys[keys.size() - 1].time - keys[0].time;
  }
  // Frames at the path's own rate, both ends included.
  u32 frame_count() const noexcept;
};

// Reads and resolves a camera path file. `terrain` answers the keys that hold a height above the
// ground; a path with such a key and no terrain to hold it over is refused. Keys must be in
// strictly increasing time, and there must be at least one.
bool read_camera_path(const std::string& path, const TerrainDesc* terrain, CameraPath& out,
                      std::string& error);
// The same from text already in memory, for a caller with no file (the tests, the protocol).
bool parse_camera_path(std::string_view text, const TerrainDesc* terrain, CameraPath& out,
                       std::string& error);

// The camera at path time `time` (clamped to the keys).
Camera sample_camera_path(const CameraPath& path, f64 time) noexcept;
// Frame `frame` of `frames` spread evenly over the whole path, both ends included: at the path's
// own frame count this is time `start + frame / fps`, and any other count resamples it.
Camera camera_path_frame(const CameraPath& path, u32 frame, u32 frames) noexcept;
f64 camera_path_frame_time(const CameraPath& path, u32 frame, u32 frames) noexcept;

}  // namespace engine::renderer
