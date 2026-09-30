#include <core/time/time.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/device.h>
#include <foundation/gpu_lock/device_hold.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>

using namespace engine;

namespace {

// Creates a device or skips the test with the reason (machines without a usable driver). The GPU
// as it is, whatever ENGINE_GFX_TEST_DEVICE says (raster_path.h): these cases compare the enabled
// features with the adapter's own report, which a profile would make disagree on purpose.
bool open_device(gfx::Device& device) {
  std::string error;
  gfx::DeviceOptions options;
  options.validation = false;
  if (!device.create(options, &error)) {
    MESSAGE("skipped: device unavailable: " << error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(device.valid());
    return false;
  }
  return true;
}

}  // namespace

TEST_CASE("gfx: device creation enables the feature chain the renderer needs") {
  gfx::Device device;
  if (!open_device(device)) return;
  CHECK(device.valid());
  const gfx::AdapterInfo& adapter = device.adapter();
  CHECK_FALSE(adapter.name.empty());
  const gfx::DeviceFeatures& f = device.features();
  MESSAGE(adapter.name << ": mesh_shader=" << f.mesh_shader
                       << " acceleration_structure=" << f.acceleration_structure
                       << " ray_tracing_pipeline=" << f.ray_tracing_pipeline << " ray_query="
                       << f.ray_query << " cluster_as=" << f.cluster_acceleration_structure
                       << " descriptor_buffer=" << f.descriptor_buffer
                       << " memory_decompression=" << f.memory_decompression);
  // Optional features only appear when the adapter advertised the extension.
  auto advertised = [&](const char* name) {
    const bool* v = adapter.extensions.find_value(std::string_view(name));
    return v != nullptr && *v;
  };
  if (f.mesh_shader) CHECK(advertised("VK_EXT_mesh_shader"));
  if (f.ray_tracing_pipeline) CHECK(advertised("VK_KHR_ray_tracing_pipeline"));
  if (f.cluster_acceleration_structure) CHECK(advertised("VK_NV_cluster_acceleration_structure"));
  if (adapter.tier == "rt-cluster") CHECK(f.cluster_acceleration_structure);
  if (adapter.tier != "raster") CHECK(f.acceleration_structure);
  // The tier is keyed on ray queries, because every ray this engine traces is one: a device
  // called "rt" that cannot trace one is the mistake the first run on a Pascal card found.
  if (adapter.tier != "raster") CHECK(f.ray_query);
  if (f.ray_query) CHECK(advertised("VK_KHR_ray_query"));
  const gfx::Handles& h = device.handles();
  CHECK(h.instance != VK_NULL_HANDLE);
  CHECK(h.device != VK_NULL_HANDLE);
  CHECK(h.graphics_queue != VK_NULL_HANDLE);
  CHECK(h.allocator != nullptr);
  device.wait_idle();
  device.destroy();
  CHECK_FALSE(device.valid());
}

// `enumerate_adapters` makes an instance of its own and loads it into volk's process-wide table;
// beside an open device it has to put the device's instance back, or the device's teardown calls
// through entry points of a destroyed instance. Until 2026-09-30 it did not, and engine-host
// crashed on every exit after `gpu.adapters` had run beside a device (src/volk_instance.h). On a
// machine where that happens this case crashes the executable in `destroy()`.
TEST_CASE("gfx: enumerating adapters beside an open device leaves the device's instance loaded") {
  gfx::Device device;
  if (!open_device(device)) return;
  const VkInstance instance = device.handles().instance;
  CHECK(volkGetLoadedInstance() == instance);
  Vector<gfx::AdapterInfo> adapters;
  std::string error;
  REQUIRE_MESSAGE(gfx::enumerate_adapters(adapters, &error), error);
  CHECK_FALSE(adapters.empty());
  CHECK(volkGetLoadedInstance() == instance);
  device.wait_idle();
  device.destroy();
  CHECK_FALSE(device.valid());
}

// The one place a process's GPU use takes the machine-wide lock (ADR-0050): device creation, when
// the switch is on. Pointed at a lock in this case's scratch directory, so the machine's real lock
// is neither taken nor waited for here.
TEST_CASE("gfx: a device holds the GPU lock while it lives, when the switch is on") {
  const test::TempDir dir("gfx_device_gpu_lock");
  const std::string path = dir.file("gpu.lock");
  const test::ScopedEnv lock("ENGINE_GPU_LOCK", path);
  const test::ScopedEnv holder(gpu_lock::k_env_holder, "");
  const test::ScopedEnv hold_log(gpu_lock::k_env_log, "");
  auto present = [&] {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
  };
  {
    const test::ScopedEnv on(gpu_lock::k_env_on_device, "0");
    gfx::Device device;
    if (!open_device(device)) return;
    CHECK_FALSE(present());  // the owner's own sessions: no switch, no lock
  }
  const test::ScopedEnv on(gpu_lock::k_env_on_device, "1");
  gfx::Device first;
  if (!open_device(first)) return;
  REQUIRE(present());
  const gpu_lock::Identity self = gpu_lock::current_identity();
  const gpu_lock::State held = gpu_lock::read(path, self, time::wall_unix_ms() / 1000);
  CHECK(held.pid == self.pid);
  CHECK(held.purpose.find("engine_gfx_tests") != std::string::npos);
  CHECK(gpu_lock::hold_status().devices == 1);
  {
    // Anything else in the process that holds for a device shares the hold (not a second live
    // gfx::Device: one per process, gfx.md "One live device per process").
    gpu_lock::DeviceHold other;
    (void)other.acquire();
    CHECK(gpu_lock::hold_status().devices == 2);
  }
  CHECK(present());  // the device still has it
  first.destroy();
  CHECK_FALSE(present());
}

TEST_CASE("gfx: offscreen clear and readback") {
  gfx::Device device;
  if (!open_device(device)) return;
  constexpr u32 k_width = 64;
  constexpr u32 k_height = 48;
  gfx::ImageResource image;
  std::string error;
  REQUIRE_MESSAGE(gfx::create_image_2d(device, k_width, k_height, gfx::Format::R8G8B8A8Unorm,
                                       gfx::ImageUsage::TransferDst | gfx::ImageUsage::TransferSrc,
                                       image, &error),
                  error);
  gfx::BufferResource readback;
  const u64 bytes = u64{k_width} * k_height * 4;
  REQUIRE_MESSAGE(gfx::create_buffer(device, bytes, gfx::BufferUsage::TransferDst,
                                     /*host_visible=*/true, readback, &error),
                  error);
  REQUIRE(readback.mapped != nullptr);
  std::memset(readback.mapped, 0, static_cast<usize>(bytes));

  const bool submitted = gfx::submit_immediate(
      device,
      [&](gfx::CommandList commands) {
        gfx::image_barrier(gfx::vk::native(commands), gfx::vk::native(image.image),
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue color{};
        color.float32[0] = 0.25f;
        color.float32[1] = 0.5f;
        color.float32[2] = 0.75f;
        color.float32[3] = 1.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(gfx::vk::native(commands), gfx::vk::native(image.image),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        gfx::image_barrier(gfx::vk::native(commands), gfx::vk::native(image.image),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_width, k_height, 1};
        vkCmdCopyImageToBuffer(gfx::vk::native(commands), gfx::vk::native(image.image),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               gfx::vk::native(readback.buffer), 1, &region);
      },
      &error);
  REQUIRE_MESSAGE(submitted, error);

  // UNORM conversion may round 0.5 * 255 either way, so allow one unit per channel.
  const auto* pixels = static_cast<const u8*>(readback.mapped);
  const int expected[4] = {64, 128, 191, 255};
  u32 wrong = 0;
  for (u32 i = 0; i < k_width * k_height; ++i) {
    const u8* p = pixels + i * 4;
    for (int c = 0; c < 4; ++c) {
      const int d = static_cast<int>(p[c]) - expected[c];
      if (d < -1 || d > 1) ++wrong;
    }
  }
  CHECK(wrong == 0);
  CHECK(pixels[3] == 255);

  // A device-address buffer round trip through an upload.
  gfx::BufferResource device_local;
  REQUIRE(gfx::create_buffer(device, 256,
                             gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc |
                                 gfx::BufferUsage::ShaderDeviceAddress,
                             /*host_visible=*/false, device_local, &error));
  CHECK(device_local.address != 0);
  CHECK(device_local.mapped == nullptr);
  gfx::BufferResource staging_up;
  gfx::BufferResource staging_down;
  REQUIRE(gfx::create_buffer(device, 256, gfx::BufferUsage::TransferSrc, /*host_visible=*/true,
                             staging_up, &error));
  REQUIRE(gfx::create_buffer(device, 256, gfx::BufferUsage::TransferDst, /*host_visible=*/true,
                             staging_down, &error));
  for (u32 i = 0; i < 256; ++i)
    static_cast<u8*>(staging_up.mapped)[i] = static_cast<u8>(i);
  std::memset(staging_down.mapped, 0, 256);
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList commands) {
        gfx::BufferCopy up{0, 0, 256};
        commands.copy_buffer(staging_up.buffer, device_local.buffer, up);
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(gfx::vk::native(commands), &dependency);
        gfx::BufferCopy down{0, 0, 256};
        commands.copy_buffer(device_local.buffer, staging_down.buffer, down);
      },
      &error));
  u32 mismatches = 0;
  for (u32 i = 0; i < 256; ++i) {
    if (static_cast<const u8*>(staging_down.mapped)[i] != static_cast<u8>(i)) ++mismatches;
  }
  CHECK(mismatches == 0);

  gfx::destroy_buffer(device, staging_up);
  gfx::destroy_buffer(device, staging_down);
  gfx::destroy_buffer(device, device_local);
  gfx::destroy_buffer(device, readback);
  gfx::destroy_image(device, image);
  device.destroy();
}
