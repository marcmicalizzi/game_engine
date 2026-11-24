// Sampled images in chunks of their own (W167; image_chunks.h has the contract, gfx.md "Images in
// chunks" the why). One chunk is one dedicated allocation of the device allocator; inside it a
// VMA virtual block hands out ranges, and each image is bound to its chunk's memory at its range.

#include <core/base/assert.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/image_chunks.h>

#include <algorithm>
#include <cstdint>

namespace engine::gfx {

namespace {

constexpr u64 k_chunk_alignment = u64{64} << 10;

u64 align_up(u64 v, u64 a) noexcept { return (v + a - 1) / a * a; }

u64 key_of(VkImage image) noexcept {
  return static_cast<u64>(reinterpret_cast<std::uintptr_t>(image));
}

void set_error(std::string* error, const char* what, VkResult result) {
  if (error == nullptr) return;
  error->assign(what);
  error->append(": ");
  error->append(result_name(result));
}

}  // namespace

struct ImageChunks::Impl {
  struct Chunk {
    VmaAllocation memory = nullptr;
    VmaVirtualBlock block = nullptr;
    u64 size = 0;
    u64 used = 0;
    u32 images = 0;
    bool live = false;
    bool draining = false;
    bool single = false;  // made for one image larger than a chunk: it takes no other
  };
  struct Placed {
    u32 chunk = ~0u;
    VmaVirtualAllocation range = nullptr;
    u64 size = 0;
  };

  const Device* device = nullptr;
  u64 chunk_bytes = 0;
  u32 memory_type = ~0u;
  Vector<Chunk> chunks;   // an index is stable; a destroyed chunk's entry is made again
  Vector<u32> order;      // the live chunks, for placement (fullest first), sized with `chunks`
  Vector<Placed> placed;  // a record an image
  Vector<u32> free_placed;
  HashMap<u64, u32> by_image;
  ImageChunkUse use;

  void count() noexcept {
    use.chunks = use.empty = use.draining = 0;
    use.chunk_bytes = 0;
    for (const Chunk& c : chunks) {
      if (!c.live) continue;
      ++use.chunks;
      use.chunk_bytes += c.size;
      if (c.images == 0) ++use.empty;
      if (c.draining) ++use.draining;
    }
    use.most_chunks = std::max(use.most_chunks, use.chunks);
    use.most_chunk_bytes = std::max(use.most_chunk_bytes, use.chunk_bytes);
  }

  void free_chunk(u32 index) noexcept {
    Chunk& c = chunks[index];
    if (!c.live) return;
    vmaClearVirtualBlock(c.block);
    vmaDestroyVirtualBlock(c.block);
    vmaFreeMemory(device->handles().allocator, c.memory);
    c = Chunk{};
    ++use.freed;
  }

  // A chunk whose last image went: destroyed, unless it is the one spare of the standard size.
  void emptied(u32 index) noexcept {
    Chunk& c = chunks[index];
    bool spare_held = false;
    for (u32 i = 0; i < chunks.size(); ++i)
      spare_held = spare_held || (i != index && chunks[i].live && chunks[i].images == 0);
    if (c.draining || c.size != chunk_bytes || spare_held) free_chunk(index);
  }

  bool make_chunk(u64 size, bool single, u32& index, std::string* error) {
    VkMemoryRequirements requirements{};
    requirements.size = size;
    requirements.alignment = k_chunk_alignment;
    requirements.memoryTypeBits = 1u << memory_type;
    VmaAllocationCreateInfo info{};
    info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    info.memoryTypeBits = 1u << memory_type;
    VmaAllocation memory = nullptr;
    if (const VkResult r =
            vmaAllocateMemory(device->handles().allocator, &requirements, &info, &memory, nullptr);
        r != VK_SUCCESS) {
      set_error(error, "vmaAllocateMemory (an image chunk)", r);
      return false;
    }
    VmaVirtualBlockCreateInfo block_info{};
    block_info.size = size;
    VmaVirtualBlock block = nullptr;
    if (const VkResult r = vmaCreateVirtualBlock(&block_info, &block); r != VK_SUCCESS) {
      vmaFreeMemory(device->handles().allocator, memory);
      set_error(error, "vmaCreateVirtualBlock", r);
      return false;
    }
    index = ~0u;
    for (u32 i = 0; i < chunks.size() && index == ~0u; ++i)
      if (!chunks[i].live) index = i;
    if (index == ~0u) {
      index = chunks.size();
      chunks.push_back(Chunk{});
      order.reserve(chunks.size());
    }
    Chunk& c = chunks[index];
    c.memory = memory;
    c.block = block;
    c.size = size;
    c.live = true;
    c.single = single;
    ++use.made;
    return true;
  }
};

ImageChunks::ImageChunks() noexcept = default;
ImageChunks::~ImageChunks() { destroy(); }

bool ImageChunks::create(const Device& device, u64 chunk_bytes, u32 expected_images,
                         std::string* error) {
  destroy();
  if (!device.valid() || chunk_bytes == 0) {
    if (error != nullptr) *error = "ImageChunks::create: no device, or a chunk of no size";
    return false;
  }
  auto impl = std::make_unique<Impl>();
  impl->device = &device;
  impl->chunk_bytes = align_up(chunk_bytes, k_chunk_alignment);
  // The memory type a sampled, block-compressed or plain colour image of optimal tiling takes; an
  // image whose requirements exclude it is made by the device allocator instead (`chunk_of` ~0u).
  VkImageCreateInfo image{};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent = {256, 256, 1};
  image.mipLevels = 9;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if (const VkResult r = vmaFindMemoryTypeIndexForImageInfo(device.handles().allocator, &image,
                                                            &alloc, &impl->memory_type);
      r != VK_SUCCESS) {
    set_error(error, "vmaFindMemoryTypeIndexForImageInfo", r);
    return false;
  }
  impl->chunks.reserve(64);
  impl->order.reserve(64);
  impl->placed.reserve(expected_images);
  impl->free_placed.reserve(expected_images);
  impl->by_image.reserve(expected_images);
  impl_ = std::move(impl);
  return true;
}

void ImageChunks::destroy() noexcept {
  if (impl_ == nullptr) return;
  ENGINE_ASSERT(impl_->use.images == 0, "every image of a chunk pool is destroyed before the pool");
  for (u32 i = 0; i < impl_->chunks.size(); ++i)
    impl_->free_chunk(i);
  impl_.reset();
}

bool ImageChunks::create_sampled_image_levels(u32 width, u32 height, Format format, u32 levels,
                                              ImageResource& out, std::string* error) {
  out = ImageResource{};
  if (impl_ == nullptr) {
    if (error != nullptr) *error = "ImageChunks: no pool";
    return false;
  }
  Impl& m = *impl_;
  const Device& device = *m.device;
  if (levels == 0 || levels > k_max_image_levels || width == 0 || height == 0) {
    if (error != nullptr) *error = "ImageChunks: no extent, or a chain past 16";
    return false;
  }
  if (!image_extent_fits(width, height, device.caps().max_image_dimension_2d, error)) return false;
  const Handles& h = device.handles();
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = vk::native(format);
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  if (const VkResult r = vkCreateImage(h.device, &info, nullptr, &image); r != VK_SUCCESS) {
    set_error(error, "vkCreateImage", r);
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(h.device, image, &requirements);
  if ((requirements.memoryTypeBits & (1u << m.memory_type)) == 0) {
    // Not the pool's memory type: the device allocator's, as gfx makes any other image.
    vkDestroyImage(h.device, image, nullptr);
    return gfx::create_sampled_image_levels(device, width, height, format, levels, out, error);
  }
  VmaVirtualAllocationCreateInfo range_info{};
  range_info.size = requirements.size;
  range_info.alignment = requirements.alignment;
  // The fullest chunk it fits in first, so that the emptier ones drain; never a draining one.
  m.order.clear();
  for (u32 i = 0; i < m.chunks.size(); ++i)
    if (m.chunks[i].live && !m.chunks[i].draining && !m.chunks[i].single) m.order.push_back(i);
  std::sort(m.order.begin(), m.order.end(), [&m](u32 a, u32 b) {
    if (m.chunks[a].used != m.chunks[b].used) return m.chunks[a].used > m.chunks[b].used;
    return a < b;
  });
  u32 chunk = ~0u;
  VmaVirtualAllocation range = nullptr;
  VkDeviceSize offset = 0;
  for (const u32 i : m.order) {
    if (m.chunks[i].size - m.chunks[i].used < requirements.size) continue;
    if (vmaVirtualAllocate(m.chunks[i].block, &range_info, &range, &offset) == VK_SUCCESS) {
      chunk = i;
      break;
    }
  }
  if (chunk == ~0u) {
    // One larger than a chunk takes a chunk of its own size, which takes no other image and goes
    // with it: packing small images round it would keep its bytes after it left.
    const bool single = requirements.size > m.chunk_bytes;
    const u64 size = single ? align_up(requirements.size, k_chunk_alignment) : m.chunk_bytes;
    if (!m.make_chunk(size, single, chunk, error) ||
        vmaVirtualAllocate(m.chunks[chunk].block, &range_info, &range, &offset) != VK_SUCCESS) {
      if (chunk != ~0u && m.chunks[chunk].images == 0) m.free_chunk(chunk);
      vkDestroyImage(h.device, image, nullptr);
      if (error != nullptr && error->empty()) *error = "ImageChunks: a new chunk took no image";
      m.count();  // the chunk made and freed again: `use()` says so now, not at the next image
      return false;
    }
  }
  Impl::Chunk& c = m.chunks[chunk];
  if (const VkResult r = vmaBindImageMemory2(h.allocator, c.memory, offset, image, nullptr);
      r != VK_SUCCESS) {
    vmaVirtualFree(c.block, range);
    if (c.images == 0) m.emptied(chunk);
    vkDestroyImage(h.device, image, nullptr);
    set_error(error, "vmaBindImageMemory2", r);
    m.count();
    return false;
  }
  u32 record = 0;
  if (!m.free_placed.empty()) {
    record = m.free_placed.back();
    m.free_placed.pop_back();
  } else {
    record = m.placed.size();
    m.placed.push_back(Impl::Placed{});
  }
  m.placed[record] = Impl::Placed{chunk, range, requirements.size};
  m.by_image.insert(key_of(image), record);
  c.used += requirements.size;
  ++c.images;
  ++m.use.images;
  m.use.used_bytes += requirements.size;
  m.count();
  out.image = vk::wrap(image);
  out.allocation = {};
  out.format = format;
  out.width = width;
  out.height = height;
  out.levels = levels;
  out.bytes = requirements.size;
  return true;
}

void ImageChunks::destroy_image(ImageResource& image) noexcept {
  if (!image.image.valid()) return;
  if (impl_ == nullptr || image.allocation.valid()) {
    if (impl_ != nullptr) gfx::destroy_image(*impl_->device, image);
    image = ImageResource{};
    return;
  }
  Impl& m = *impl_;
  const VkImage native = vk::native(image.image);
  const u32* at = m.by_image.find_value(key_of(native));
  vkDestroyImage(m.device->handles().device, native, nullptr);
  image = ImageResource{};
  if (at == nullptr) return;
  const u32 record = *at;
  m.by_image.erase(key_of(native));
  const Impl::Placed placed = m.placed[record];
  m.free_placed.push_back(record);
  Impl::Chunk& c = m.chunks[placed.chunk];
  vmaVirtualFree(c.block, placed.range);
  c.used -= placed.size;
  --c.images;
  --m.use.images;
  m.use.used_bytes -= placed.size;
  if (c.images == 0) m.emptied(placed.chunk);
  m.count();
}

u32 ImageChunks::chunk_of(const ImageResource& image) const noexcept {
  if (impl_ == nullptr || !image.image.valid() || image.allocation.valid()) return ~0u;
  const u32* at = impl_->by_image.find_value(key_of(vk::native(image.image)));
  return at != nullptr ? impl_->placed[*at].chunk : ~0u;
}

u32 ImageChunks::sparsest_chunk(u64* used) const noexcept {
  if (impl_ == nullptr) return ~0u;
  u32 best = ~0u;
  for (u32 i = 0; i < impl_->chunks.size(); ++i) {
    const Impl::Chunk& c = impl_->chunks[i];
    if (!c.live || c.draining || c.images == 0) continue;
    if (best == ~0u || c.used < impl_->chunks[best].used) best = i;
  }
  if (used != nullptr) *used = best != ~0u ? impl_->chunks[best].used : 0u;
  return best;
}

void ImageChunks::set_draining(u32 chunk, bool draining) noexcept {
  if (impl_ == nullptr || chunk >= impl_->chunks.size() || !impl_->chunks[chunk].live) return;
  impl_->chunks[chunk].draining = draining;
  if (draining && impl_->chunks[chunk].images == 0) impl_->free_chunk(chunk);
  impl_->count();
}

bool ImageChunks::draining(u32 chunk) const noexcept {
  return impl_ != nullptr && chunk < impl_->chunks.size() && impl_->chunks[chunk].live &&
         impl_->chunks[chunk].draining;
}

u64 ImageChunks::chunk_size() const noexcept { return impl_ != nullptr ? impl_->chunk_bytes : 0u; }

ImageChunkUse ImageChunks::use() const noexcept {
  return impl_ != nullptr ? impl_->use : ImageChunkUse{};
}

}  // namespace engine::gfx
