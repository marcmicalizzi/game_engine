#pragma once

// The global bindless set (ADR-0023): three unbounded, partially bound, update-after-bind arrays
// (sampled images, storage images, samplers) in one descriptor set that every pipeline binds
// at set 0, plus the pipeline layout that goes with it (set 0 and a push-constant block).
// Buffers are reached through device addresses and never appear here.
//
//     BindlessSet bindless;
//     bindless.create(device, {});
//     const u32 albedo = bindless.add_sampled_image(view,
//     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
//     ...push {albedo, sampler_index, address}...
//     bindless.release_sampled_image(albedo, frames.frame_index() + 1);   // free after the GPU is
//     done bindless.recycle(frames.completed());                               // once per frame
//
// Slot indices are stable for the resource's lifetime; the shader side declares
//   [[vk::binding(0, 0)]] Texture2D g_textures[];
//   [[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];
//   [[vk::binding(2, 0)]] SamplerState g_samplers[];

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/vulkan.h>

#include <string>

namespace engine::gfx {

struct BindlessConfig {
  // Capacities; clamped to the device's update-after-bind limits and reported by capacity().
  u32 sampled_images = 16384;
  u32 storage_images = 4096;
  u32 samplers = 256;
  // Push-constant block shared by every pipeline using the layout, all stages.
  u32 push_constant_bytes = 128;
};

class BindlessSet {
 public:
  static constexpr u32 k_binding_sampled_images = 0;
  static constexpr u32 k_binding_storage_images = 1;
  static constexpr u32 k_binding_samplers = 2;
  static constexpr u32 k_invalid_slot = ~u32{0};

  BindlessSet() noexcept = default;
  ~BindlessSet();
  ENGINE_NON_COPYABLE(BindlessSet);

  bool create(const Device& device, const BindlessConfig& config, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  // Registration writes the descriptor immediately (update after bind) and returns a slot, or
  // k_invalid_slot when the array is full.
  u32 add_sampled_image(VkImageView view, VkImageLayout layout);
  u32 add_storage_image(VkImageView view);
  u32 add_sampler(VkSampler sampler);
  // Overwrites a slot in place (a streamed texture replacing its placeholder).
  void update_sampled_image(u32 slot, VkImageView view, VkImageLayout layout);
  void update_storage_image(u32 slot, VkImageView view);

  // Returns a slot to its free list once recycle() sees the timeline at or past
  // `safe_after_value`: the value of the last frame that could still read the slot.
  void release_sampled_image(u32 slot, u64 safe_after_value);
  void release_storage_image(u32 slot, u64 safe_after_value);
  void release_sampler(u32 slot, u64 safe_after_value);
  // Frees every release whose value has completed. Call once per frame.
  void recycle(u64 completed_value);

  const BindlessConfig& capacity() const noexcept { return capacity_; }
  u32 live_sampled_images() const noexcept { return live_[k_binding_sampled_images]; }
  u32 live_storage_images() const noexcept { return live_[k_binding_storage_images]; }
  u32 live_samplers() const noexcept { return live_[k_binding_samplers]; }
  u32 pending_releases() const noexcept { return pending_.size(); }

  VkDescriptorSetLayout layout() const noexcept { return layout_; }
  VkDescriptorSet set() const noexcept { return set_; }
  // Set 0 = this set; one push-constant range of config.push_constant_bytes for all stages.
  VkPipelineLayout pipeline_layout() const noexcept { return pipeline_layout_; }
  void bind(VkCommandBuffer commands, VkPipelineBindPoint bind_point) const noexcept;

 private:
  struct Pool {
    Vector<u32> free;
    u32 next = 0;
    u32 capacity = 0;
  };
  struct Pending {
    u32 binding;
    u32 slot;
    u64 safe_after_value;
  };
  u32 allocate(u32 binding);
  void write_image(u32 binding, u32 slot, VkImageView view, VkImageLayout layout,
                   VkDescriptorType type);

  const Device* device_ = nullptr;
  BindlessConfig capacity_;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  Pool pools_[3];
  u32 live_[3] = {0, 0, 0};
  Vector<Pending> pending_;
};

// Image views and samplers the set holds.
bool create_image_view(const Device& device, const ImageResource& image, VkImageView& out,
                       std::string* error = nullptr);
void destroy_image_view(const Device& device, VkImageView view) noexcept;
// Nearest or linear filtering, clamp to edge, no anisotropy, no mips: the tooling default.
bool create_sampler(const Device& device, VkFilter filter, VkSampler& out,
                    std::string* error = nullptr);
void destroy_sampler(const Device& device, VkSampler sampler) noexcept;

}  // namespace engine::gfx
