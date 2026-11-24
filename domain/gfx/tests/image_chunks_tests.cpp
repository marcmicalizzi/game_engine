#include "raster_path.h"

#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <domain/gfx/image_chunks.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

// **Images in chunks give their bytes back to the device** (W167, W47's stage 1; gfx.md, "Images
// in chunks"). Twelve 1 MiB images in 4 MiB chunks: they pack into as few chunks as fit, and the
// pool's live chunks are what the driver is charged. Emptying one chunk keeps it as the one spare;
// emptying a second gives it back, counted by the pool (a chunk freed, one fewer live) and on the
// driver's own usage for the process where the device reports one. A draining chunk takes no new
// image. Device-only: no shader, no frame.
TEST_CASE("gfx: image chunks pack, keep one spare and give an emptied chunk back") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  constexpr u64 k_mib = 1024ull * 1024ull;
  constexpr u64 k_chunk = 4 * k_mib;
  gfx::ImageChunks chunks;
  REQUIRE_MESSAGE(chunks.create(device, k_chunk, 64, &error), error);
  gfx::MemoryBudget before;
  const bool budget = device.memory_budget(before) && before.valid;

  Vector<gfx::ImageResource> images;
  for (u32 i = 0; i < 12; ++i) {
    gfx::ImageResource image;
    REQUIRE_MESSAGE(
        chunks.create_sampled_image_levels(512, 512, gfx::Format::R8G8B8A8Unorm, 1, image, &error),
        error);
    CHECK(image.bytes >= k_mib);
    CHECK(!image.allocation.valid());
    REQUIRE(chunks.chunk_of(image) != ~0u);
    images.push_back(image);
  }
  const gfx::ImageChunkUse full = chunks.use();
  MESSAGE("12 images of " << images[0].bytes << " bytes in " << full.chunks << " chunks of "
                          << k_chunk << " bytes, " << full.used_bytes << " bytes used");
  CHECK(full.images == 12);
  CHECK(full.chunk_bytes == u64{full.chunks} * k_chunk);
  // As few chunks as the images fit in: a chunk holds k_chunk / bytes of them.
  const u64 per_chunk = k_chunk / images[0].bytes;
  REQUIRE(per_chunk >= 1);
  CHECK(full.chunks == (12 + per_chunk - 1) / per_chunk);
  REQUIRE(full.chunks >= 3);
  gfx::MemoryBudget filled;
  if (budget) REQUIRE(device.memory_budget(filled));

  // The images of the first two chunks go; the first emptied stays as the spare, the second goes.
  const u32 first = chunks.chunk_of(images[0]);
  u32 second = ~0u;
  for (const gfx::ImageResource& image : images)
    if (chunks.chunk_of(image) != first && second == ~0u) second = chunks.chunk_of(image);
  REQUIRE(second != ~0u);
  for (gfx::ImageResource& image : images) {
    const u32 c = chunks.chunk_of(image);
    if (c == first || c == second) chunks.destroy_image(image);
  }
  const gfx::ImageChunkUse emptied = chunks.use();
  CHECK(emptied.freed == 1);
  CHECK(emptied.empty == 1);
  CHECK(emptied.chunks == full.chunks - 1);
  CHECK(emptied.chunk_bytes == full.chunk_bytes - k_chunk);
  if (budget) {
    gfx::MemoryBudget after;
    REQUIRE(device.memory_budget(after));
    MESSAGE("the driver's usage for the process: " << before.used_bytes << " before, "
                                                   << filled.used_bytes << " filled, "
                                                   << after.used_bytes << " with a chunk back");
    CHECK(filled.used_bytes >= before.used_bytes + full.chunk_bytes);
    CHECK(after.used_bytes + k_chunk <= filled.used_bytes);
  }

  // A draining chunk takes no new image: with every live chunk but the spare draining, a new image
  // goes to the spare.
  for (const gfx::ImageResource& image : images)
    if (image.image.valid()) chunks.set_draining(chunks.chunk_of(image), true);
  gfx::ImageResource late;
  REQUIRE_MESSAGE(
      chunks.create_sampled_image_levels(512, 512, gfx::Format::R8G8B8A8Unorm, 1, late, &error),
      error);
  CHECK(!chunks.draining(chunks.chunk_of(late)));
  CHECK(chunks.use().made == full.made);  // the spare took it: no chunk made
  chunks.destroy_image(late);
  for (gfx::ImageResource& image : images)
    chunks.destroy_image(image);
  const gfx::ImageChunkUse end = chunks.use();
  CHECK(end.images == 0);
  CHECK(end.chunks == 1);  // the spare alone; the draining ones went with their last image
  CHECK(end.used_bytes == 0);
  chunks.destroy();
}
