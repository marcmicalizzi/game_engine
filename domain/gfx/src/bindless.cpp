#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/gfx/bindless.h>

#include <algorithm>

namespace engine::gfx {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_bindless, "gfx.bindless");

void set_error(std::string* error, const char* what, VkResult result) {
  if (error == nullptr) return;
  error->assign(what);
  error->append(": ");
  error->append(result_name(result));
}

}  // namespace

BindlessSet::~BindlessSet() { destroy(); }

bool BindlessSet::create(const Device& device, const BindlessConfig& config, std::string* error) {
  ENGINE_VERIFY(device_ == nullptr, "BindlessSet::create: already created");
  const Handles& h = device.handles();

  // Clamp to the device's update-after-bind limits. The clamping rule lives in
  // `gfx::clamp_bindless` (domain/gfx/requirements.h) rather than here, because `gpu.adapters`
  // has to report the clamped sizes for a machine no one has run on — and a second copy of the
  // rule in the report would be a rule that can disagree with the set that is actually created.
  // It reads `device.caps()`, so `DeviceOptions::overrides` reaches the clamping too: that is
  // how a device with 1,048,576 update-after-bind slots tests what a Surface Pro gets.
  BindlessCapacity wanted;
  wanted.sampled_images = config.sampled_images;
  wanted.storage_images = config.storage_images;
  wanted.samplers = config.samplers;
  wanted.acceleration_structures =
      device.features().acceleration_structure ? config.acceleration_structures : 0u;
  wanted.push_constant_bytes = config.push_constant_bytes;
  const BindlessCapacity granted = clamp_bindless(device.caps(), wanted, nullptr);
  capacity_ = config;
  capacity_.sampled_images = granted.sampled_images;
  capacity_.storage_images = granted.storage_images;
  capacity_.samplers = granted.samplers;
  capacity_.acceleration_structures = granted.acceleration_structures;
  capacity_.push_constant_bytes = granted.push_constant_bytes;
  const u32 binding_count = capacity_.acceleration_structures > 0 ? 4u : 3u;
  if (capacity_.sampled_images == 0 || capacity_.storage_images == 0 || capacity_.samplers == 0) {
    if (error != nullptr) *error = "device reports no update-after-bind descriptor capacity";
    return false;
  }

  const VkDescriptorBindingFlags flags =
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
  const VkDescriptorBindingFlags binding_flags[4] = {flags, flags, flags, flags};
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0].binding = k_binding_sampled_images;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[0].descriptorCount = capacity_.sampled_images;
  bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
  bindings[1].binding = k_binding_storage_images;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[1].descriptorCount = capacity_.storage_images;
  bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
  bindings[2].binding = k_binding_samplers;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[2].descriptorCount = capacity_.samplers;
  bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
  bindings[3].binding = k_binding_acceleration_structures;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[3].descriptorCount = capacity_.acceleration_structures;
  bindings[3].stageFlags = VK_SHADER_STAGE_ALL;

  VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
  flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flags_info.bindingCount = binding_count;
  flags_info.pBindingFlags = binding_flags;
  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.pNext = &flags_info;
  layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layout_info.bindingCount = binding_count;
  layout_info.pBindings = bindings;
  if (const VkResult r = vkCreateDescriptorSetLayout(h.device, &layout_info, nullptr, &layout_);
      r != VK_SUCCESS) {
    set_error(error, "vkCreateDescriptorSetLayout(bindless)", r);
    return false;
  }
  device_ = &device;

  const VkDescriptorPoolSize sizes[4] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_.sampled_images},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, capacity_.storage_images},
      {VK_DESCRIPTOR_TYPE_SAMPLER, capacity_.samplers},
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, capacity_.acceleration_structures},
  };
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = binding_count;
  pool_info.pPoolSizes = sizes;
  if (const VkResult r = vkCreateDescriptorPool(h.device, &pool_info, nullptr, &pool_);
      r != VK_SUCCESS) {
    set_error(error, "vkCreateDescriptorPool(bindless)", r);
    destroy();
    return false;
  }
  VkDescriptorSetAllocateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = pool_;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &layout_;
  if (const VkResult r = vkAllocateDescriptorSets(h.device, &set_info, &set_); r != VK_SUCCESS) {
    set_error(error, "vkAllocateDescriptorSets(bindless)", r);
    destroy();
    return false;
  }

  VkPushConstantRange range{};
  range.stageFlags = VK_SHADER_STAGE_ALL;
  range.offset = 0;
  range.size = capacity_.push_constant_bytes;
  VkPipelineLayoutCreateInfo pipeline_layout_info{};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &layout_;
  pipeline_layout_info.pushConstantRangeCount = capacity_.push_constant_bytes > 0 ? 1 : 0;
  pipeline_layout_info.pPushConstantRanges = &range;
  if (const VkResult r =
          vkCreatePipelineLayout(h.device, &pipeline_layout_info, nullptr, &pipeline_layout_);
      r != VK_SUCCESS) {
    set_error(error, "vkCreatePipelineLayout(bindless)", r);
    destroy();
    return false;
  }

  pools_[0].capacity = capacity_.sampled_images;
  pools_[1].capacity = capacity_.storage_images;
  pools_[2].capacity = capacity_.samplers;
  pools_[3].capacity = capacity_.acceleration_structures;
  ENGINE_LOG_DEBUG(log_bindless, "bindless set created",
                   log::field("sampled_images", capacity_.sampled_images),
                   log::field("storage_images", capacity_.storage_images),
                   log::field("samplers", capacity_.samplers),
                   log::field("acceleration_structures", capacity_.acceleration_structures),
                   log::field("push_constant_bytes", capacity_.push_constant_bytes));
  return true;
}

void BindlessSet::destroy() noexcept {
  if (device_ == nullptr) return;
  const Handles& h = device_->handles();
  if (pipeline_layout_ != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(h.device, pipeline_layout_, nullptr);
  if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(h.device, pool_, nullptr);  // frees the set
  if (layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(h.device, layout_, nullptr);
  pipeline_layout_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  set_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  for (Pool& p : pools_)
    p = Pool{};
  for (u32& l : live_)
    l = 0;
  pending_.clear();
  device_ = nullptr;
}

u32 BindlessSet::allocate(u32 binding) {
  Pool& pool = pools_[binding];
  u32 slot;
  if (!pool.free.empty()) {
    slot = pool.free.back();
    pool.free.pop_back();
  } else if (pool.next < pool.capacity) {
    slot = pool.next++;
  } else {
    ENGINE_LOG_ERROR(log_bindless, "bindless array full", log::field("binding", binding),
                     log::field("capacity", pool.capacity));
    return k_invalid_slot;
  }
  ++live_[binding];
  return slot;
}

void BindlessSet::write_image(u32 binding, u32 slot, VkImageView view, VkImageLayout layout,
                              VkDescriptorType type) {
  VkDescriptorImageInfo info{};
  info.imageView = view;
  info.imageLayout = layout;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set_;
  write.dstBinding = binding;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = type;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device_->handles().device, 1, &write, 0, nullptr);
}

u32 BindlessSet::add_sampled_image(VkImageView view, VkImageLayout layout) {
  ENGINE_VERIFY(device_ != nullptr, "BindlessSet: not created");
  const u32 slot = allocate(k_binding_sampled_images);
  if (slot != k_invalid_slot)
    write_image(k_binding_sampled_images, slot, view, layout, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  return slot;
}

u32 BindlessSet::add_storage_image(VkImageView view) {
  ENGINE_VERIFY(device_ != nullptr, "BindlessSet: not created");
  const u32 slot = allocate(k_binding_storage_images);
  if (slot != k_invalid_slot) {
    write_image(k_binding_storage_images, slot, view, VK_IMAGE_LAYOUT_GENERAL,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  }
  return slot;
}

u32 BindlessSet::add_sampler(VkSampler sampler) {
  ENGINE_VERIFY(device_ != nullptr, "BindlessSet: not created");
  const u32 slot = allocate(k_binding_samplers);
  if (slot == k_invalid_slot) return slot;
  VkDescriptorImageInfo info{};
  info.sampler = sampler;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set_;
  write.dstBinding = k_binding_samplers;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device_->handles().device, 1, &write, 0, nullptr);
  return slot;
}

u32 BindlessSet::add_acceleration_structure(VkAccelerationStructureKHR structure) {
  ENGINE_VERIFY(device_ != nullptr, "BindlessSet: not created");
  if (!has_acceleration_structures()) return k_invalid_slot;
  const u32 slot = allocate(k_binding_acceleration_structures);
  if (slot == k_invalid_slot) return slot;
  VkWriteDescriptorSetAccelerationStructureKHR structures{};
  structures.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
  structures.accelerationStructureCount = 1;
  structures.pAccelerationStructures = &structure;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.pNext = &structures;
  write.dstSet = set_;
  write.dstBinding = k_binding_acceleration_structures;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  vkUpdateDescriptorSets(device_->handles().device, 1, &write, 0, nullptr);
  return slot;
}

void BindlessSet::update_sampled_image(u32 slot, VkImageView view, VkImageLayout layout) {
  ENGINE_VERIFY(slot < pools_[k_binding_sampled_images].next,
                "BindlessSet: slot was never allocated");
  write_image(k_binding_sampled_images, slot, view, layout, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
}

void BindlessSet::update_storage_image(u32 slot, VkImageView view) {
  ENGINE_VERIFY(slot < pools_[k_binding_storage_images].next,
                "BindlessSet: slot was never allocated");
  write_image(k_binding_storage_images, slot, view, VK_IMAGE_LAYOUT_GENERAL,
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
}

void BindlessSet::release_sampled_image(u32 slot, u64 safe_after_value) {
  if (slot == k_invalid_slot) return;
  pending_.push_back(Pending{k_binding_sampled_images, slot, safe_after_value});
}
void BindlessSet::release_storage_image(u32 slot, u64 safe_after_value) {
  if (slot == k_invalid_slot) return;
  pending_.push_back(Pending{k_binding_storage_images, slot, safe_after_value});
}
void BindlessSet::release_sampler(u32 slot, u64 safe_after_value) {
  if (slot == k_invalid_slot) return;
  pending_.push_back(Pending{k_binding_samplers, slot, safe_after_value});
}

void BindlessSet::release_acceleration_structure(u32 slot, u64 safe_after_value) {
  ENGINE_VERIFY(slot < pools_[k_binding_acceleration_structures].next,
                "BindlessSet: slot was never allocated");
  pending_.push_back(Pending{k_binding_acceleration_structures, slot, safe_after_value});
}

void BindlessSet::recycle(u64 completed_value) {
  for (u32 i = 0; i < pending_.size();) {
    const Pending& p = pending_[i];
    if (p.safe_after_value <= completed_value) {
      pools_[p.binding].free.push_back(p.slot);
      ENGINE_ASSERT(live_[p.binding] > 0, "BindlessSet: live count underflow");
      --live_[p.binding];
      pending_.erase_unordered_at(i);
    } else {
      ++i;
    }
  }
}

void BindlessSet::bind(VkCommandBuffer commands, VkPipelineBindPoint bind_point) const noexcept {
  vkCmdBindDescriptorSets(commands, bind_point, pipeline_layout_, 0, 1, &set_, 0, nullptr);
}

// ---- views and samplers ----------------------------------------------------------------------

bool create_image_view(const Device& device, const ImageResource& image, VkImageView& out,
                       std::string* error) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image.image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = image.format;
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  const VkResult r = vkCreateImageView(device.handles().device, &info, nullptr, &out);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateImageView", r);
    out = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void destroy_image_view(const Device& device, VkImageView view) noexcept {
  if (view != VK_NULL_HANDLE) vkDestroyImageView(device.handles().device, view, nullptr);
}

bool create_sampler(const Device& device, VkFilter filter, VkSampler& out, std::string* error) {
  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = filter;
  info.minFilter = filter;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.maxLod = 0.0f;
  const VkResult r = vkCreateSampler(device.handles().device, &info, nullptr, &out);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateSampler", r);
    out = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void destroy_sampler(const Device& device, VkSampler sampler) noexcept {
  if (sampler != VK_NULL_HANDLE) vkDestroySampler(device.handles().device, sampler, nullptr);
}

}  // namespace engine::gfx
