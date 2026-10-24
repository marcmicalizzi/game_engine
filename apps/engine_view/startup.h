#pragma once

// `--startup-trace <out.json>` (apps.md, "--startup-trace: from launch to ready"): the process's
// startup recorded by the core trace (core/profiling/startup_trace.h) from the OS's launch time
// to the moment the player can move with the ground and collision under them and the
// neighbourhood has stopped changing, written as Chrome trace JSON and summarized as the JSON
// summary's `startup` block. Off — no flag — none of it runs and the summary's block is null.

#include <core/base/types.h>
#include <core/json/json_value.h>

#include <string>

namespace engine::view {

// The milestones, in the order the summary holds them monotone (apps.md has each definition).
inline constexpr const char* k_startup_milestones[] = {"main",
                                                       "device",
                                                       "scene_loaded",
                                                       "gpu_scene",
                                                       "first_frame",
                                                       "player_controllable",
                                                       "ground_collision_ready",
                                                       "nearby_stable"};
inline constexpr u32 k_startup_milestone_count =
    sizeof(k_startup_milestones) / sizeof(k_startup_milestones[0]);
// "Nearby quality stable" holds for this many consecutive frames before it is said to be reached,
// and is dated from the first of them.
inline constexpr u32 k_stable_frames = 30;

// What a frame says about readiness, read from what the frame loop already has.
struct Readiness {
  bool frame_done = false;      // a frame's GPU work has been read back
  bool controllable = false;    // the input has moved the player (or there is no player)
  bool collision = false;       // the ground under the feet has collision (or there is no walker)
  bool world_settled = false;   // the world's ring changed nothing and deferred nothing
  bool pages_settled = false;   // no geometry page pending or in flight
  bool ground_settled = false;  // the drawn ground's layout built nothing and lags nothing
};

class StartupTrace {
 public:
  // Starts the trace dated from the process's launch; `main_ns` is when `main` began.
  bool start(const std::string& path, i64 main_ns, std::string* error);
  bool on() const noexcept { return on_; }
  // A milestone the first time it is named; later calls do nothing.
  void milestone(const char* name) noexcept;
  void frame(const Readiness& readiness) noexcept;
  bool reached(const char* name) const noexcept;
  // Stops the trace, writes the file, and returns the summary block (null when off).
  JsonValue finish(std::string* error);

 private:
  bool on_ = false;
  bool finished_ = false;
  std::string path_;
  bool reached_[k_startup_milestone_count] = {};
  u32 stable_ = 0;
  i64 stable_since_ = 0;
  JsonValue block_;
};

// The one recorder engine-view has: started by the flag, read by the summary.
StartupTrace& startup_trace();

}  // namespace engine::view
