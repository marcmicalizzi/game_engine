#pragma once

// Per-frame submission (docs/plan/04-renderer.md §4.2): N frames in flight, one command pool
// per slot, GPU progress on a timeline semaphore, and deferred destruction of resources the GPU
// may still be reading. begin_frame() blocks only when the GPU is more than N-1 frames behind.
//
//     FrameContext frames;
//     frames.create(device, 2);
//     for (;;) {
//       VkCommandBuffer commands = frames.begin_frame();   // waits for slot reuse, resets pool
//       ...record...
//       frames.defer_destroy(old_buffer);                  // freed when this slot comes around
//       const u64 value = frames.end_frame();              // submitted; timeline reaches value
//     }
//
// The render graph records into the frame's command buffer; presentation and multi-queue
// submission arrive with the swapchain and async compute.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/vulkan.h>

#include <string>

namespace engine::gfx {

class FrameContext {
 public:
  FrameContext() noexcept = default;
  ~FrameContext();
  ENGINE_NON_COPYABLE(FrameContext);

  bool create(const Device& device, u32 frames_in_flight, std::string* error = nullptr);
  // Waits for every submitted frame, runs every deferred destruction, releases the pools.
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  // Waits until the GPU has finished the frame that last used this slot, runs that frame's
  // deferred destructions, resets the pool, and returns a primary command buffer in the
  // recording state.
  VkCommandBuffer begin_frame();
  // Ends recording and submits on the graphics queue, signaling the timeline with the value
  // returned (frame_index() + 1 at the time of the call).
  u64 end_frame();

  u32 frames_in_flight() const noexcept { return slots_.size(); }
  u32 slot() const noexcept { return slot_; }
  // Frames begun so far; the frame being recorded has index frame_index().
  u64 frame_index() const noexcept { return frame_index_; }
  bool recording() const noexcept { return recording_; }

  // Timeline value the GPU has reached.
  u64 completed() const noexcept;
  // Host wait for the timeline to reach `value`; false on timeout or device loss.
  bool wait(u64 value, u64 timeout_ns = ~u64{0}) const noexcept;
  void wait_idle() const noexcept;

  // Destroyed when the current slot is next recycled, which happens only after the GPU has
  // finished the frame being recorded. Outside a frame (between end_frame and begin_frame) the
  // resource is attached to the most recently submitted frame.
  void defer_destroy(BufferResource buffer);
  void defer_destroy(ImageResource image);

  VkSemaphore timeline() const noexcept { return timeline_; }

 private:
  struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    u64 submitted_value = 0;  // 0: never submitted
    Vector<BufferResource> buffers;
    Vector<ImageResource> images;
  };
  void recycle(Slot& slot);

  const Device* device_ = nullptr;
  VkSemaphore timeline_ = VK_NULL_HANDLE;
  Vector<Slot> slots_;
  u32 slot_ = 0;
  u64 frame_index_ = 0;
  bool recording_ = false;
};

}  // namespace engine::gfx
