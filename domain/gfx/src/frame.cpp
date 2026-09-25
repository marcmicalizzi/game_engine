#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
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
  VkSemaphore timeline = VK_NULL_HANDLE;
  if (const VkResult r = vkCreateSemaphore(h.device, &semaphore_info, nullptr, &timeline);
      r != VK_SUCCESS) {
    if (error != nullptr) *error = std::string("vkCreateSemaphore(timeline): ") + result_name(r);
    return false;
  }
  timeline_ = vk::wrap(timeline);
  slots_.resize(frames_in_flight);
  for (Slot& slot : slots_) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateCommandPool(h.device, &pool_info, nullptr, &pool);
        r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkCreateCommandPool: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    slot.pool = vk::wrap(pool);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (const VkResult r = vkAllocateCommandBuffers(h.device, &alloc, &commands); r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkAllocateCommandBuffers: ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    slot.commands = vk::wrap(commands);
    VkSemaphoreCreateInfo binary{};
    binary.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquire = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSemaphore(h.device, &binary, nullptr, &acquire);
        r != VK_SUCCESS) {
      if (error != nullptr) *error = std::string("vkCreateSemaphore(acquire): ") + result_name(r);
      device_ = &device;
      destroy();
      return false;
    }
    slot.acquire = vk::wrap(acquire);
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
    if (slot.pool) vkDestroyCommandPool(h.device, vk::native(slot.pool), nullptr);
    if (slot.acquire) vkDestroySemaphore(h.device, vk::native(slot.acquire), nullptr);
  }
  slots_.clear();
  if (timeline_) vkDestroySemaphore(h.device, vk::native(timeline_), nullptr);
  timeline_ = {};
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

CommandList FrameContext::begin_frame() {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::begin_frame: not created");
  ENGINE_VERIFY(!recording_, "FrameContext::begin_frame: end_frame was not called");
  slot_ = static_cast<u32>(frame_index_ % slots_.size());
  Slot& slot = slots_[slot_];
  recycle(slot);
  vkResetCommandPool(device_->handles().device, vk::native(slot.pool), 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(vk::native(slot.commands), &begin);
  recording_ = true;
  return CommandList{slot.commands};
}

u64 FrameContext::end_frame() { return end_frame(PresentSync{}); }

u64 FrameContext::end_frame(const PresentSync& sync) {
  ENGINE_VERIFY(recording_, "FrameContext::end_frame: begin_frame was not called");
  Slot& slot = slots_[slot_];
  vkEndCommandBuffer(vk::native(slot.commands));
  const u64 value = frame_index_ + 1;

  VkCommandBufferSubmitInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  command_info.commandBuffer = vk::native(slot.commands);
  VkSemaphoreSubmitInfo signals[2]{};
  signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signals[0].semaphore = vk::native(timeline_);
  signals[0].value = value;
  signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  u32 signal_count = 1;
  if (sync.signal) {
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = vk::native(sync.signal);
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal_count = 2;
  }
  VkSemaphoreSubmitInfo wait{};
  wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  wait.semaphore = vk::native(sync.wait);
  wait.stageMask = vk::native(sync.wait_stage);
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.waitSemaphoreInfoCount = sync.wait ? 1 : 0;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  submit.signalSemaphoreInfoCount = signal_count;
  submit.pSignalSemaphoreInfos = signals;
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
  vkGetSemaphoreCounterValue(device_->handles().device, vk::native(timeline_), &value);
  return value;
}

bool FrameContext::wait(u64 value, u64 timeout_ns) const noexcept {
  if (device_ == nullptr) return false;
  const VkSemaphore timeline = vk::native(timeline_);
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &timeline;
  info.pValues = &value;
  return vkWaitSemaphores(device_->handles().device, &info, timeout_ns) == VK_SUCCESS;
}

void FrameContext::wait_idle() const noexcept {
  if (device_ != nullptr) vkDeviceWaitIdle(device_->handles().device);
}

void FrameContext::defer_destroy(BufferResource buffer) {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::defer_destroy: not created");
  if (!buffer.buffer) return;
  // While recording, the current slot; otherwise the slot of the last submitted frame.
  const u32 target = recording_ || frame_index_ == 0
                         ? slot_
                         : static_cast<u32>((frame_index_ - 1) % slots_.size());
  slots_[target].buffers.push_back(buffer);
}

void FrameContext::defer_destroy(ImageResource image) {
  ENGINE_VERIFY(device_ != nullptr, "FrameContext::defer_destroy: not created");
  if (!image.image) return;
  const u32 target = recording_ || frame_index_ == 0
                         ? slot_
                         : static_cast<u32>((frame_index_ - 1) % slots_.size());
  slots_[target].images.push_back(image);
}

}  // namespace engine::gfx
