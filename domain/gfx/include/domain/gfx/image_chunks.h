#pragma once

// **Sampled images in chunks of their own, so that a freed image's bytes can go back to the
// device** (W167, W47's stage 1; docs/subsystems/gfx.md, "Images in chunks";
// docs/experiments/residency-manager-design.md, "Pools are made of chunks"). The device allocator
// suballocates every image from large blocks it shares with everything else and keeps a block while
// anything lives in it, so an image the texture residency trims leaves a hole the engine's next
// allocation takes and the driver never gets back. A pool of `ImageChunks` holds its images in
// device-memory chunks of a size the caller names (one chunk a dedicated allocation of the device
// allocator), each suballocated by a chunk-local allocator, and destroys a chunk when its last
// image goes, keeping at most one empty chunk of the standard size as a spare so that a promotion
// after a trim does not make one again. An image larger than a chunk takes a chunk of its own size,
// which is never kept as the spare.
//
// **Placement packs**: an image goes into the fullest chunk it fits in, never into a chunk the
// caller has marked draining, and a new chunk is made only when none fits. A caller that wants a
// sparse chunk's bytes back marks it draining and re-places what is in it (the texture residency
// does this through its promotion path); the chunk is destroyed when the last of its images is.
//
//     gfx::ImageChunks chunks;
//     chunks.create(device, 64ull << 20, &error);
//     chunks.create_sampled_image_levels(w, h, format, levels, image, &error);  // like gfx's own
//     ...record_image_levels_upload(list, image, staging, offsets) as for any sampled image...
//     chunks.destroy_image(image);      // never gfx::destroy_image: the pool frees its range
//     chunks.destroy();                 // every image gone first
//
// Making and destroying images is the caller's thread's; the pool is not thread-safe. Nothing in a
// placement or a free allocates engine memory once `create` has reserved its tables (a frame may
// make an image); a new chunk is a device allocation and is counted (`ImageChunkUse::made`).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <memory>
#include <string>

namespace engine::gfx {

// What the pool holds, for an account and a test.
struct ImageChunkUse {
  u32 chunks = 0;       // live chunks, the spare included
  u32 empty = 0;        // of them, empty (the spare: at most one)
  u32 draining = 0;     // of them, marked draining
  u32 images = 0;       // live images placed in chunks
  u64 chunk_bytes = 0;  // the device bytes the live chunks take: what the driver is charged
  u64 used_bytes = 0;   // of them, what live images take (their memory requirements' sizes)
  u64 made = 0;         // chunks made since `create`
  u64 freed = 0;        // chunks destroyed since `create`
  u32 most_chunks = 0;  // the most live at once
  u64 most_chunk_bytes = 0;
};

class ImageChunks {
 public:
  ImageChunks() noexcept;
  ~ImageChunks();
  ENGINE_NON_COPYABLE(ImageChunks);

  // `chunk_bytes`: the standard chunk's size, rounded up to 64 KiB. `expected_images` sizes the
  // pool's tables so that placements and frees allocate nothing up to that count.
  bool create(const Device& device, u64 chunk_bytes, u32 expected_images,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }

  // `gfx::create_sampled_image_levels`, placed in a chunk: the same image (sampled, a copy
  // target, optimal tiling, Undefined layout), `out.allocation` empty and `out.bytes` the range it
  // takes. False, with `out` empty, when the device cannot make it.
  bool create_sampled_image_levels(u32 width, u32 height, Format format, u32 levels,
                                   ImageResource& out, std::string* error = nullptr);
  // Destroys an image this pool made and frees its range; the chunk goes when it is empty (or
  // stays as the spare). An image the pool did not make is destroyed as `gfx::destroy_image` would.
  void destroy_image(ImageResource& image) noexcept;

  // The chunk an image of this pool is in, or ~0u.
  u32 chunk_of(const ImageResource& image) const noexcept;
  // The live chunk, not draining, whose images take the fewest bytes, or ~0u; and its used bytes.
  u32 sparsest_chunk(u64* used = nullptr) const noexcept;
  // A draining chunk takes no new image; the spare is never draining.
  void set_draining(u32 chunk, bool draining) noexcept;
  bool draining(u32 chunk) const noexcept;
  u64 chunk_size() const noexcept;

  ImageChunkUse use() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::gfx
