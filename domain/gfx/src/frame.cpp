#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/gfx/frame.h>

namespace engine::gfx {

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_frame, "gfx.frame");
}

FrameContext::~FrameContext() { destroy(); }

bool FrameContext::create(const Device& device, u32 frames_in_flight, std::string* error) {
  ENGINE_VERIFY(device_ == nullptr, "FrameContext::create: already created");
  if (frames_in_flight == 0 || !device.valid()) {
    if (error != nullptr)
      *error = "FrameContext needs a valid device and at least one frame in flight";
    return false;
  }
  const Handles& h = device.handles();
  VkSemaphoreTypeCreateInfo type_info{};
  type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type_info.initialValue = 0;
  VkSemaphoreCreateInfo semaphore_info{};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = &type_info;
  if (const VkResult r = vkCreateSemaphore(h.device, &semaphore_info, nullptr, &timeline_);
      r != VK_SUCCESS) {
    if (error != nullptr) *error = std::string("vkCreateSemaphore(timeline): ") + result_name(r);
    return false;
  }
  slots_.resize(frames_in_flight);
  for (Slot& slot : slots_) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.graphics_family();
    if (const VkResult r = vkCreateCommandPool(h.device, &pool_info, nullptr, &slot.pool);
        r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkCreateCommandPool: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = slot.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (const VkResult r = vkAllocateCommandBuffers(h.device, &alloc, &slot.commands);
        r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkAllocateCommandBuffers: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
  }
  device_ = &device;
  ENGINE_LOG_DEBUG(log_frame, "frame context created",
                   log::field("frames_in_flight", frames_in_flight));
  return true;
}

void FrameContext::destroy() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  if (h.device != VK_NULL_HANDLE) vkDeviceWaitIdle(h.device);
  for (Slot& slot : slots_) {
    for (BufferResource& b : slot.buffers)
      destroy_buffer(*device_, b);
    for (ImageResource& i : slot.images)
      destroy_image(*device_, i);
    slot.buffers.clear();
    slot.images.clear();
    if (slot.pool != VK_NULL_HANDLE) vkDestroyCommandPool(h.device, slot.pool, nullptr);
  }
  slots_.clear();
  if (timeline_ != VK_NULL_HANDLE) vkDestroySemaphore(h.device, timeline_, nullptr);
  timeline_ = VK_NULL_HANDLE;
  device_ = nullptr;
  recording_ = false;
}

void FrameContext::recycle(Slot& slot) {
  if (slot.submitted_value != 0) {
    const bool done = wait(slot.submitted_value);
    ENGINE_VERIFY(done, "FrameContext: waiting for a submitted frame failed (device lost?)");
  }
  for (BufferResource& b : slot.buffers)
    destroy_buffer(*device_, b);
  for (ImageResource& i : slot.images)
    destroy_image(*device_, i);
  slot.buffers.clear();
  slot.images.clear();
}

VkCommandBuffer FrameContext::begin_frame() {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::begin_frame: not created");
  ENGINE_VERIFY(!recording_, "FrameContext::begin_frame: end_frame was not called");
  slot_ = static_cast<u32>(frame_index_ % slots_.size());
  Slot& slot = slots_[slot_];
  recycle(slot);
  vkResetCommandPool(device_->handles().device, slot.pool, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(slot.commands, &begin);
  recording_ = true;
  return slot.commands;
}

u64 FrameContext::end_frame() {
  ENGINE_VERIFY(recording_, "FrameContext::end_frame: begin_frame was not called");
  Slot& slot = slots_[slot_];
  vkEndCommandBuffer(slot.commands);
  const u64 value = frame_index_ + 1;

  VkCommandBufferSubmitInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  command_info.commandBuffer = slot.commands;
  VkSemaphoreSubmitInfo signal{};
  signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signal.semaphore = timeline_;
  signal.value = value;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signal;
  const VkResult r = vkQueueSubmit2(device_->handles().graphics_queue, 1, &submit, VK_NULL_HANDLE);
  ENGINE_VERIFY(r == VK_SUCCESS, "FrameContext::end_frame: vkQueueSubmit2 failed");
  slot.submitted_value = value;
  ++frame_index_;
  recording_ = false;
  return value;
}

u64 FrameContext::completed() const noexcept {
  if (device_ == nullptr) return 0;
  u64 value = 0;
  vkGetSemaphoreCounterValue(device_->handles().device, timeline_, &value);
  return value;
}

bool FrameContext::wait(u64 value, u64 timeout_ns) const noexcept {
  if (device_ == nullptr) return false;
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &timeline_;
  info.pValues = &value;
  return vkWaitSemaphores(device_->handles().device, &info, timeout_ns) == VK_SUCCESS;
}

void FrameContext::wait_idle() const noexcept {
  if (device_ != nullptr) vkDeviceWaitIdle(device_->handles().device);
}

void FrameContext::defer_destroy(BufferResource buffer) {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::defer_destroy: not created");
  if (buffer.buffer == VK_NULL_HANDLE) return;
  // While recording, the current slot; otherwise the slot of the last submitted frame.
  const u32 target = recording_ || frame_index_ == 0
                         ? slot_
                         : static_cast<u32>((frame_index_ - 1) % slots_.size());
  slots_[target].buffers.push_back(buffer);
}

void FrameContext::defer_destroy(ImageResource image) {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::defer_destroy: not created");
  if (image.image == VK_NULL_HANDLE) return;
  const u32 target = recording_ || frame_index_ == 0
                         ? slot_
                         : static_cast<u32>((frame_index_ - 1) % slots_.size());
  slots_[target].images.push_back(image);
}

}  // namespace engine::gfx
