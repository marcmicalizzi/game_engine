#pragma once

// Buffers and images, and the one-shot submission that uploads and reads them
// back (docs/subsystems/gfx.md). Memory comes from VMA inside the backend; what a caller holds is
// a `BufferResource` or an `ImageResource` of engine handles, the same 40 bytes the Vulkan-typed
// structs they replaced were, so nothing that embeds one changed size.
//
//     gfx::BufferResource vis;
//     gfx::create_buffer(device, bytes, gfx::BufferUsage::Storage |
//     gfx::BufferUsage::ShaderDeviceAddress,
//                        /*host_visible=*/false, vis, &error);
//     ...vis.address goes into a parameter block; vis.buffer into a copy or a fill...
//     gfx::destroy_buffer(device, vis);

#include <core/base/types.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>

namespace engine::gfx {

struct BufferResource {
  BufferHandle buffer;
  AllocationHandle allocation;
  void* mapped = nullptr;  // persistent mapping when host visible
  u64 size = 0;
  DeviceAddress address = 0;  // when created with BufferUsage::ShaderDeviceAddress
};

struct ImageResource {
  ImageHandle image;
  AllocationHandle allocation;
  Format format = Format::Undefined;
  u32 width = 0;
  u32 height = 0;
  u32 levels = 1;  // mip levels; a view made by create_image_view covers all of them
  u64 bytes = 0;   // what the allocation takes on the device, as VMA reports it
};

// One mip level's bytes for `upload_image_2d_levels`, tightly packed: rows of texels for an
// uncompressed format, rows of 4x4 blocks for a block-compressed one (a level whose sides are not
// multiples of four still takes whole blocks).
struct ImageLevelData {
  const void* data = nullptr;
  u64 bytes = 0;
};

// host_visible buffers are persistently mapped and host-coherent (upload and readback);
// otherwise device-local.
bool create_buffer(const Device& device, u64 size, BufferUsage usage, bool host_visible,
                   BufferResource& out, std::string* error = nullptr);
void destroy_buffer(const Device& device, BufferResource& buffer) noexcept;
// The buffers the calling thread has made with `create_buffer` (and the uploads that call it)
// since it started. A buffer is an allocation the engine's memory tags never see — the driver's
// and the device allocator's — so a frame loop's test counts these to show the loop makes none in
// steady state (docs/subsystems/renderer.md, "What a frame waits for").
u64 buffers_created_on_thread() noexcept;
// The times the calling thread has stopped for the device since it started: a `submit_immediate`
// (a submission and a wait for its fence) or a `FrameContext::wait_idle` (the device drained). A
// frame loop's test counts these to show that what a host does between frames — moving instances,
// say — never makes the CPU wait for the GPU (docs/subsystems/renderer.md, "Moving an instance").
u64 device_waits_on_thread() noexcept;
// Creates a device-local buffer with `usage` plus TransferDst and ShaderDeviceAddress, fills it
// from `data` through a staging buffer, and waits for the copy. For setup-time uploads.
bool upload_buffer(const Device& device, const void* data, u64 bytes, BufferUsage usage,
                   BufferResource& out, std::string* error = nullptr);
// Creates a sampled 2D image, fills it from tightly packed `pixels` through a staging buffer, and
// leaves it in ImageLayout::ShaderReadOnly. For setup-time textures.
bool upload_image_2d(const Device& device, u32 width, u32 height, Format format, const void* pixels,
                     u64 bytes, ImageResource& out, std::string* error = nullptr);
// The same with a mip chain: `levels.size()` levels of `width` x `height` halved (rounded down,
// never below one) at each step, level 0 first, all copied from one staging buffer in one
// submission and left in ImageLayout::ShaderReadOnly. The format may be block-compressed (the BC
// formats need DeviceFeatures::texture_compression_bc); each level's bytes are then its blocks.
// This is how the content build's textures reach the GPU without a decode (texture.md).
bool upload_image_2d_levels(const Device& device, u32 width, u32 height, Format format,
                            std::span<const ImageLevelData> levels, ImageResource& out,
                            std::string* error = nullptr);

// **The same upload in two halves, so that nothing waits for the device** (texture.md, "Textures
// that arrive after the load"; R145). `create_sampled_image_levels` makes the sampled image with
// its chain and nothing in it (ImageLayout::Undefined); `record_image_levels_upload` records, into
// a command list the caller owns — a frame's, inside a transfer pass — the transition to a copy
// target, one copy per level out of `staging` at `level_offsets` (each a multiple of 16, the
// level's bytes as `upload_image_2d_levels` takes them), and the transition to
// ImageLayout::ShaderReadOnly, where the image stays. The image is one the render graph has never
// seen and no pass declares, which is why its two barriers are recorded here and not derived:
// nothing reads it before this, and every later reader samples it through the bindless set.
// `upload_image_2d_levels` is these two around a submission it waits for.
inline constexpr u32 k_max_image_levels = 16;
bool create_sampled_image_levels(const Device& device, u32 width, u32 height, Format format,
                                 u32 levels, ImageResource& out, std::string* error = nullptr);
void record_image_levels_upload(CommandList commands, const ImageResource& image,
                                BufferHandle staging, std::span<const u64> level_offsets) noexcept;
bool create_image_2d(const Device& device, u32 width, u32 height, Format format, ImageUsage usage,
                     ImageResource& out, std::string* error = nullptr);
void destroy_image(const Device& device, ImageResource& image) noexcept;

// ---- one-shot submission ------------------------------------------------------------------------

using RecordFn = void (*)(CommandList commands, void* context);

// Records with `record`, submits on the graphics queue, and waits for completion. For uploads,
// readbacks, tests, and tools; the render graph owns per-frame submission.
bool submit_immediate(const Device& device, RecordFn record, void* context,
                      std::string* error = nullptr);

template <class F>
bool submit_immediate(const Device& device, F&& record, std::string* error = nullptr) {
  return submit_immediate(
      device, [](CommandList commands, void* context) { (*static_cast<F*>(context))(commands); },
      &record, error);
}

static_assert(sizeof(BufferResource) == 40 && sizeof(ImageResource) == 40);

}  // namespace engine::gfx
