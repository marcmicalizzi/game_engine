#pragma once

// The global bindless set (ADR-0023): unbounded, partially bound, update-after-bind arrays
// (sampled images, storage images, samplers, and on devices with ray tracing acceleration
// structures) in one descriptor set that every pipeline binds at set 0, plus the pipeline layout
// that goes with it (set 0 and a push-constant block). Buffers are reached through device
// addresses and never appear here; acceleration structures have no address form a shader can
// trace against, so they do.
//
//     BindlessSet bindless;
//     bindless.create(device, {});
//     const u32 albedo = bindless.add_sampled_image(view, ImageLayout::ShaderReadOnly);
//     ...push {albedo, sampler_index, address}...
//     bindless.release_sampled_image(albedo, frames.frame_index() + 1);   // free after the GPU is
//     done bindless.recycle(frames.completed());                               // once per frame
//
// Slot indices are stable for the resource's lifetime; the shader side declares
//   [[vk::binding(0, 0)]] Texture2D g_textures[];
//   [[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];
//   [[vk::binding(2, 0)]] SamplerState g_samplers[];
//   [[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];   // ray tracing devices

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <string>

namespace engine::gfx {

struct BindlessConfig {
  // Capacities; clamped to the device's update-after-bind limits and reported by capacity().
  u32 sampled_images = 16384;
  u32 storage_images = 4096;
  u32 samplers = 256;
  u32 acceleration_structures =
      256;  // binding 3 exists only with DeviceFeatures::acceleration_structure
  // Push-constant block shared by every pipeline using the layout, all stages.
  u32 push_constant_bytes = 128;
};

class BindlessSet {
 public:
  static constexpr u32 k_binding_sampled_images = 0;
  static constexpr u32 k_binding_storage_images = 1;
  static constexpr u32 k_binding_samplers = 2;
  static constexpr u32 k_binding_acceleration_structures = 3;
  static constexpr u32 k_invalid_slot = ~u32{0};

  BindlessSet() noexcept = default;
  ~BindlessSet();
  ENGINE_NON_COPYABLE(BindlessSet);

  bool create(const Device& device, const BindlessConfig& config, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  // Registration writes the descriptor immediately (update after bind) and returns a slot, or
  // k_invalid_slot when the array is full.
  u32 add_sampled_image(ImageViewHandle view, ImageLayout layout);
  u32 add_storage_image(ImageViewHandle view);
  u32 add_sampler(SamplerHandle sampler);
  // k_invalid_slot on a device without acceleration structures (has_acceleration_structures()).
  u32 add_acceleration_structure(AccelerationStructureHandle structure);
  // Overwrites a slot in place (a streamed texture replacing its placeholder).
  void update_sampled_image(u32 slot, ImageViewHandle view, ImageLayout layout);
  void update_storage_image(u32 slot, ImageViewHandle view);

  // Returns a slot to its free list once recycle() sees the timeline at or past
  // `safe_after_value`: the value of the last frame that could still read the slot.
  void release_sampled_image(u32 slot, u64 safe_after_value);
  void release_storage_image(u32 slot, u64 safe_after_value);
  void release_sampler(u32 slot, u64 safe_after_value);
  void release_acceleration_structure(u32 slot, u64 safe_after_value);
  // Frees every release whose value has completed. Call once per frame.
  void recycle(u64 completed_value);

  const BindlessConfig& capacity() const noexcept { return capacity_; }
  bool has_acceleration_structures() const noexcept {
    return pools_[k_binding_acceleration_structures].capacity > 0;
  }
  u32 live_sampled_images() const noexcept { return live_[k_binding_sampled_images]; }
  u32 live_storage_images() const noexcept { return live_[k_binding_storage_images]; }
  u32 live_samplers() const noexcept { return live_[k_binding_samplers]; }
  u32 live_acceleration_structures() const noexcept {
    return live_[k_binding_acceleration_structures];
  }
  u32 pending_releases() const noexcept { return pending_.size(); }

  DescriptorSetLayoutHandle layout() const noexcept { return layout_; }
  DescriptorSetHandle set() const noexcept { return set_; }
  // Set 0 = this set; one push-constant range of config.push_constant_bytes for all stages.
  PipelineLayoutHandle pipeline_layout() const noexcept { return pipeline_layout_; }
  void bind(CommandList commands, BindPoint bind_point) const noexcept;

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
  void write_image(u32 binding, u32 slot, ImageViewHandle view, ImageLayout layout,
                   DescriptorType type);

  const Device* device_ = nullptr;
  BindlessConfig capacity_;
  DescriptorSetLayoutHandle layout_;
  DescriptorPoolHandle pool_;
  DescriptorSetHandle set_;
  PipelineLayoutHandle pipeline_layout_;
  Pool pools_[4];
  u32 live_[4] = {0, 0, 0, 0};
  Vector<Pending> pending_;
};

// Image views and samplers the set holds. A view of a depth format covers the depth aspect.
bool create_image_view(const Device& device, const ImageResource& image, ImageViewHandle& out,
                       std::string* error = nullptr);
void destroy_image_view(const Device& device, ImageViewHandle view) noexcept;
// Nearest or linear filtering, clamp to edge, no anisotropy, no mips: the tooling default.
bool create_sampler(const Device& device, Filter filter, SamplerHandle& out,
                    std::string* error = nullptr);
// The material sampler of built, mipmapped textures (docs/subsystems/texture.md): linear within
// and between levels, clamp to edge like `create_sampler`, the whole chain, and `max_anisotropy`
// (clamped to 16, the minimum every device with the feature supports) where
// DeviceFeatures::sampler_anisotropy is on — trilinear where it is not.
bool create_mip_sampler(const Device& device, f32 max_anisotropy, SamplerHandle& out,
                        std::string* error = nullptr);

// A material sampler as a glTF sampler describes one (docs/subsystems/gfx.md, "Samplers"): the
// wrap in u and v, the magnification and minification filters and the blend between levels.
// `mipmapped` samples the whole chain and turns anisotropy on — `max_anisotropy`, clamped to 16,
// where DeviceFeatures::sampler_anisotropy is on and the minification filter is linear, since
// anisotropy is a way of minifying and a nearest one asks for none. Without it the sampler reads
// level 0 alone (maxLod 0, no anisotropy), which is what a texture decoded at one level and
// sampled with an explicit level of 0 has always been read through.
struct SamplerDesc {
  SamplerAddressMode address_u = SamplerAddressMode::Repeat;
  SamplerAddressMode address_v = SamplerAddressMode::Repeat;
  Filter mag = Filter::Linear;
  Filter min = Filter::Linear;
  SamplerMipmapMode mip = SamplerMipmapMode::Linear;
  bool mipmapped = true;
  f32 max_anisotropy = 16.0f;
};
bool create_sampler(const Device& device, const SamplerDesc& desc, SamplerHandle& out,
                    std::string* error = nullptr);
void destroy_sampler(const Device& device, SamplerHandle sampler) noexcept;

}  // namespace engine::gfx
