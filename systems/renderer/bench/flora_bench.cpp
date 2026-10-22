// What posing a scene's trees costs on the frame's thread and on the poser's pool (flora_poser.h;
// docs/subsystems/renderer.md, "Who poses the trees"), for groves from four trees to a forest.
//
// **Every row sleeps between frames, untimed**, because a frame does: the pool's workers spin a
// little after a frame's chunks and then sleep, so the frame after pays their wake-up, and a bench
// that posed back to back would find them spinning and measure a pool no frame ever has. The sleep
// is 4 ms, past the workers' spin.
//
// Rows: `serial` is the loop on this thread (what every scene paid before the pool); `split` is the
// pool with every grove cut as finely as it will go (64 parts a chunk), which shows where a pool
// starts to pay; `poser` is what the renderer does, cut at `k_flora_chunk_parts`, so a small grove
// is `serial` again. Items are parts.
#include "../tests/flora_grove.h"

#include <core/base/types.h>
#include <foundation/bench/bench.h>
#include <systems/renderer/flora_poser.h>

#include <chrono>
#include <thread>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::test_grove;

namespace {

enum class Mode { Serial, Split, Poser };

void run(bench::State& state, u32 trees, Mode mode) {
  if (bench::smoke_mode() && trees > 64) trees = 64;
  Grove g;
  if (!make_grove(trees, 4.0f, g)) return;
  FloraPoser poser;
  if (mode != Mode::Serial) {
    poser.prepare(std::span<const FloraInstance>(g.instances.data(), g.instances.size()),
                  std::span<const FloraMesh>(g.meshes.data(), g.meshes.size()), 0,
                  mode == Mode::Split ? 64u : k_flora_chunk_parts);
  }
  FrameFlora flora;
  FloraPoseFrame frame;
  frame.wind_to = Vec2{0.8f, 0.6f};
  frame.wind_strength = 1.0f;
  frame.flora = &flora;
  u64 f = 0;
  while (state.keep_running()) {
    state.pause_timing();
    std::this_thread::sleep_for(std::chrono::milliseconds(bench::smoke_mode() ? 0 : 4));
    ++f;
    frame.frame_index = f;
    frame.time_s = static_cast<f64>(f) / 60.0;
    state.resume_timing();
    const FloraPoseResult r = mode == Mode::Serial
                                  ? flora_pose_instances(g.inputs(), frame, 0, g.instances.size())
                                  : poser.pose(g.inputs(), frame);
    bench::keep(r.moving);
    bench::keep(g.poses[g.parts - 1]);
  }
  state.set_items(g.parts);
}

}  // namespace

#define FLORA_ROWS(trees)                                                    \
  ENGINE_BENCH(flora_serial_##trees, "renderer.flora.pose.serial." #trees) { \
    run(state, trees, Mode::Serial);                                         \
  }                                                                          \
  ENGINE_BENCH(flora_split_##trees, "renderer.flora.pose.split." #trees) {   \
    run(state, trees, Mode::Split);                                          \
  }                                                                          \
  ENGINE_BENCH(flora_poser_##trees, "renderer.flora.pose.poser." #trees) {   \
    run(state, trees, Mode::Poser);                                          \
  }

FLORA_ROWS(4)
FLORA_ROWS(16)
FLORA_ROWS(64)
FLORA_ROWS(256)
FLORA_ROWS(4096)
