#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

namespace {

u32 live_allocations(const gfx::Device& device) {
  VmaTotalStatistics stats{};
  vmaCalculateStatistics(device.handles().allocator, &stats);
  return stats.total.statistics.allocationCount;
}

}  // namespace

TEST_CASE("gfx: frames in flight advance a timeline and recycle deferred resources") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("device unavailable: " << error);
    return;
  }
  gfx::FrameContext frames;
  REQUIRE_MESSAGE(frames.create(device, 2, &error), error);
  CHECK(frames.frames_in_flight() == 2);
  CHECK(frames.completed() == 0);
  CHECK(frames.frame_index() == 0);

  const u32 baseline = live_allocations(device);
  gfx::BufferResource target;
  REQUIRE(
      gfx::create_buffer(device, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, target, &error));

  constexpr u32 k_frames = 6;
  u64 last_value = 0;
  for (u32 f = 0; f < k_frames; ++f) {
    VkCommandBuffer commands = frames.begin_frame();
    CHECK(frames.recording());
    CHECK(frames.slot() == f % 2);
    CHECK(frames.frame_index() == f);
    // Per-frame scratch that the GPU reads: destroyed only once this slot is recycled.
    gfx::BufferResource scratch;
    REQUIRE(
        gfx::create_buffer(device, 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, scratch, &error));
    vkCmdFillBuffer(commands, target.buffer, 0, 4096, f);
    VkBufferCopy copy{0, 0, 1024};
    vkCmdCopyBuffer(commands, scratch.buffer, target.buffer, 1, &copy);
    frames.defer_destroy(scratch);
    last_value = frames.end_frame();
    CHECK(last_value == f + 1);
    CHECK_FALSE(frames.recording());
    // At most frames_in_flight scratch buffers are alive at any time (plus target).
    CHECK(live_allocations(device) <= baseline + 1 + 2);
  }
  CHECK(frames.wait(last_value));
  CHECK(frames.completed() == k_frames);
  CHECK(frames.frame_index() == k_frames);

  // Deferred destruction outside a frame attaches to the last submitted frame and runs when
  // its slot is recycled.
  gfx::BufferResource late;
  REQUIRE(gfx::create_buffer(device, 512, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, late, &error));
  frames.defer_destroy(late);
  const u32 before = live_allocations(device);
  (void)frames.begin_frame();  // recycles slot 0 (frame 4)
  frames.end_frame();
  (void)frames.begin_frame();  // recycles slot 1 (frame 5, which owns `late`)
  frames.end_frame();
  CHECK(live_allocations(device) < before);

  frames.destroy();
  CHECK_FALSE(frames.valid());
  CHECK(live_allocations(device) == baseline + 1);  // only `target` remains
  gfx::destroy_buffer(device, target);
  CHECK(live_allocations(device) == baseline);
  device.destroy();
}
