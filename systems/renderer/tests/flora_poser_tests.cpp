// Posing a scene's trees on a pool (docs/subsystems/renderer.md, "Who poses the trees";
// flora_poser.h): every worker count and every cut of the instances into chunks gives the serial
// loop's poses, push states, instance records and frame answers byte for byte, in a wind alone and
// with a walker pushing through the grove. No device.
#include "flora_grove.h"

#include <systems/renderer/flora_poser.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::test_grove;

namespace {

// The frame `f` of a run: the wind turning and gusting, the frame index skipping now and then (so
// the push's ease advances by more than a tick), and with `walk`, a walker crossing the grove's
// first rows diagonally with a capsule tall enough to reach the crowns.
FloraPoseFrame frame_of(u32 f, bool walk, FrameFlora& flora) {
  FloraPoseFrame frame;
  const f32 a = 0.3f + 0.05f * static_cast<f32>(f);
  frame.wind_to = Vec2{std::cos(a), std::sin(a)};
  frame.wind_strength = f % 7 == 3 ? 0.0f : 0.4f + 0.06f * static_cast<f32>(f % 11);
  frame.time_s = static_cast<f64>(f) / 60.0;
  frame.frame_index = 100 + f + f / 4;
  flora = FrameFlora{};
  if (walk) {
    flora.walker = true;
    flora.walker_feet = WorldPos{0.5 * f, 0.0, 0.45 * f};
    flora.walker_radius = 0.4f;
    flora.walker_height = 9.0f;
  }
  frame.flora = &flora;
  return frame;
}

bool same_instances(const Grove& a, const Grove& b) {
  if (a.instances.size() != b.instances.size()) return false;
  for (u32 i = 0; i < a.instances.size(); ++i) {
    if (a.instances[i].last_tick != b.instances[i].last_tick ||
        a.instances[i].posed != b.instances[i].posed)
      return false;
  }
  return true;
}

// Runs `frames` frames serially on one grove and through a poser of `workers` threads cut at
// `chunk_parts` on another, and compares everything after every frame.
void compare(u32 trees, u32 workers, u32 chunk_parts, u32 frames, bool walk, u32* pushed_most,
             u32* chunks_out) {
  Grove serial;
  Grove pooled;
  REQUIRE(make_grove(trees, 4.0f, serial));
  REQUIRE(make_grove(trees, 4.0f, pooled));
  FloraPoser poser;
  poser.prepare(std::span<const FloraInstance>(pooled.instances.data(), pooled.instances.size()),
                std::span<const FloraMesh>(pooled.meshes.data(), pooled.meshes.size()), workers,
                chunk_parts);
  *chunks_out = poser.chunk_count();
  u32 differing_frames = 0;
  for (u32 f = 0; f < frames; ++f) {
    FrameFlora flora;
    const FloraPoseFrame frame = frame_of(f, walk, flora);
    const FloraPoseResult a =
        flora_pose_instances(serial.inputs(), frame, 0, serial.instances.size());
    const FloraPoseResult b = poser.pose(pooled.inputs(), frame);
    const bool poses = std::memcmp(serial.poses.data(), pooled.poses.data(),
                                   serial.poses.size() * sizeof(gfx::FloraPartPose)) == 0;
    const bool state = std::memcmp(serial.state.data(), pooled.state.data(),
                                   serial.state.size() * sizeof(FloraPartState)) == 0;
    const bool same = poses && state && a.pushed_parts == b.pushed_parts && a.moving == b.moving &&
                      same_instances(serial, pooled);
    differing_frames += same ? 0u : 1u;
    if (a.pushed_parts > *pushed_most) *pushed_most = a.pushed_parts;
  }
  CHECK_MESSAGE(differing_frames == 0, "workers " << workers << ", chunk parts " << chunk_parts
                                                  << ": " << differing_frames << " of " << frames
                                                  << " frames differ from the serial loop");
}

}  // namespace

static_assert(sizeof(gfx::FloraPartPose) == 80, "compared as bytes: no padding");
static_assert(sizeof(FloraPartState) == 16, "compared as bytes: no padding");

TEST_CASE("flora poser: the chunks cut the instances by their parts, at most 64") {
  Grove g;
  REQUIRE(make_grove(400, 4.0f, g));
  const auto instances = std::span<const FloraInstance>(g.instances.data(), g.instances.size());
  const auto meshes = std::span<const FloraMesh>(g.meshes.data(), g.meshes.size());
  FloraPoser poser;
  // One worker besides the frame's thread: four chunks a thread, so eight.
  poser.prepare(instances, meshes, 1, 64);
  CHECK(poser.chunk_count() == 8);
  CHECK(poser.worker_count() == 1);
  // Many workers and small chunks: never more than parallel_for keeps in place.
  poser.prepare(instances, meshes, 200, 1);
  CHECK(poser.chunk_count() == k_flora_max_chunks);
  CHECK(poser.worker_count() <= poser.chunk_count() - 1);
  // A grove too small to pay for a wake-up is posed inline, on no pool.
  Grove few;
  REQUIRE(make_grove(4, 4.0f, few));
  poser.prepare(std::span<const FloraInstance>(few.instances.data(), few.instances.size()),
                std::span<const FloraMesh>(few.meshes.data(), few.meshes.size()), 0);
  CHECK(poser.chunk_count() == 1);
  CHECK(poser.worker_count() == 0);
  // Three workers: sixteen chunks.
  poser.prepare(instances, meshes, 3, 64);
  CHECK(poser.chunk_count() == 16);
  CHECK(poser.worker_count() == 3);
  MESSAGE("400 trees, " << g.parts << " parts, " << poser.chunk_count() << " chunks");
}

TEST_CASE("flora poser: any worker count poses a grove in a wind to the serial loop's bytes") {
  u32 pushed = 0;
  u32 chunks = 0;
  for (u32 workers : {1u, 2u, 3u, 7u, 0u}) {
    compare(160, workers, 64, 24, false, &pushed, &chunks);
    if (workers != 1) CHECK(chunks >= 2);
  }
  // The scene's own cut, at the default chunk size, where it splits at all.
  compare(160, 0, k_flora_chunk_parts, 8, false, &pushed, &chunks);
  CHECK(pushed == 0);
}

TEST_CASE("flora poser: a walker pushing through the grove gives the serial loop's bytes") {
  u32 pushed = 0;
  u32 chunks = 0;
  for (u32 workers : {2u, 5u, 0u}) {
    compare(160, workers, 64, 40, true, &pushed, &chunks);
    CHECK(chunks >= 2);
  }
  MESSAGE("the walker bent at most " << pushed << " parts in a frame");
  CHECK(pushed > 0);
}
