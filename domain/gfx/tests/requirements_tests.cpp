// The requirements table (domain/gfx/requirements.h) and the three machine profiles the engine
// has never run on, simulated here on whatever device this machine has.
//
// Every GPU the engine has run on is one RTX 5090 with one NVIDIA driver. The three machines
// waiting for it are a 2014 Kepler laptop (Vulkan 1.2 at best, so the renderer must refuse it
// cleanly and say which version), a Surface Pro 4 (Intel Gen9, a different vendor's driver and
// much smaller update-after-bind limits, so the bindless set must clamp and keep working), and a
// GTX Titan X (Maxwell: no mesh shaders, and 64-bit buffer atomics are an open question). The
// first two cases below are `DeviceOptions::overrides` pretending to be those machines, which is
// the only way to test the refusal and the clamping before the hardware is in reach.

#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/requirements.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;

namespace {

// Everything a Required row asks for, and nothing more: the smallest device the renderer accepts.
gfx::DeviceCaps minimal_caps() {
  gfx::DeviceCaps caps;
  caps.api_version = 1003;
  caps.graphics_compute_families = 1;
  caps.dynamic_rendering = 1;
  caps.synchronization2 = 1;
  caps.maintenance4 = 1;
  caps.buffer_device_address = 1;
  caps.descriptor_indexing = 1;
  caps.runtime_descriptor_array = 1;
  caps.descriptor_binding_partially_bound = 1;
  caps.descriptor_binding_sampled_image_update_after_bind = 1;
  caps.descriptor_binding_storage_image_update_after_bind = 1;
  caps.shader_sampled_image_array_non_uniform_indexing = 1;
  caps.timeline_semaphore = 1;
  caps.scalar_block_layout = 1;
  caps.host_query_reset = 1;
  caps.draw_indirect_count = 1;
  caps.multi_draw_indirect = 1;
  caps.shader_int64 = 1;
  caps.max_push_constants_size = gfx::k_push_constant_bytes;
  caps.max_compute_workgroup_invocations = gfx::k_workgroup_invocations;
  caps.max_compute_workgroup_size_x = gfx::k_workgroup_size_x;
  caps.max_compute_workgroup_size_y = gfx::k_workgroup_size_y;
  caps.max_compute_shared_memory_size = gfx::k_shared_memory_bytes;
  caps.max_memory_allocation_count = gfx::k_memory_allocations;
  caps.max_per_stage_uab_sampled_images = gfx::k_min_sampled_images;
  caps.max_per_stage_uab_storage_images = gfx::k_min_storage_images;
  caps.max_per_stage_uab_samplers = gfx::k_min_samplers;
  caps.max_per_stage_uab_resources = gfx::k_min_sampled_images + gfx::k_min_storage_images;
  caps.max_set_uab_sampled_images = gfx::k_min_sampled_images;
  caps.max_set_uab_storage_images = gfx::k_min_storage_images;
  caps.max_set_uab_samplers = gfx::k_min_samplers;
  return caps;
}

bool any_mentions(const Vector<std::string>& lines, const char* needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Creates a device or skips with the reason, as the other gfx tests do.
bool open_device(gfx::Device& device, const gfx::DeviceOptions& options, std::string& error) {
  if (!device.create(options, &error)) {
    MESSAGE("device unavailable: " << error);
    return false;
  }
  return true;
}

// Probed once: every device creation costs the driver a client, and a machine that is short of
// them is exactly the failure mode AGENTS.md warns about.
const std::string& driver_error() {
  static const std::string error = [] {
    gfx::Device probe;
    gfx::DeviceOptions options;
    std::string message;
    if (!probe.create(options, &message))
      return message.empty() ? std::string("no device") : message;
    probe.destroy();
    return std::string{};
  }();
  return error;
}

bool driver_available() {
  if (driver_error().empty()) return true;
  MESSAGE("device unavailable: " << driver_error());
  return false;
}

}  // namespace

TEST_CASE("gfx: the requirements table answers without a device") {
  Vector<gfx::DeviceRequirement> rows;

  // A device that reports nothing fails every Required row and no Optional or Note row is
  // counted against it.
  gfx::evaluate_requirements(gfx::DeviceCaps{}, rows);
  REQUIRE_FALSE(rows.empty());
  u32 required = 0;
  u32 optional = 0;
  u32 notes = 0;
  for (const gfx::DeviceRequirement& row : rows) {
    CHECK_FALSE(row.name.empty());
    CHECK_FALSE(row.why.empty());
    CHECK_FALSE(row.needed.empty());
    CHECK_FALSE(row.found.empty());
    CHECK_FALSE(row.pass);
    if (row.level == gfx::RequirementLevel::Required) ++required;
    if (row.level == gfx::RequirementLevel::Optional) ++optional;
    if (row.level == gfx::RequirementLevel::Note) ++notes;
    // Only an Optional row names a capability, and it always names one.
    CHECK(row.unlocks.empty() == (row.level != gfx::RequirementLevel::Optional));
  }
  CHECK(required > 0);
  CHECK(optional > 0);
  CHECK(notes > 0);
  CHECK_FALSE(gfx::requirements_met({rows.data(), rows.size()}));
  gfx::DeviceVerdict verdict;
  gfx::device_verdict(gfx::DeviceCaps{}, {rows.data(), rows.size()}, verdict);
  CHECK_FALSE(verdict.usable);
  CHECK(verdict.tier == "none");
  CHECK(verdict.blocking.size() == required);
  CHECK(any_mentions(verdict.blocking, "apiVersion"));

  // The smallest accepted device passes every Required row and no Optional one.
  const gfx::DeviceCaps caps = minimal_caps();
  gfx::evaluate_requirements(caps, rows);
  CHECK(gfx::requirements_met({rows.data(), rows.size()}));
  gfx::device_verdict(caps, {rows.data(), rows.size()}, verdict);
  CHECK(verdict.usable);
  CHECK(verdict.tier == "raster");
  CHECK(verdict.blocking.empty());
  // It has no mesh shaders, no ray tracing, no swapchain and no 64-bit atomics, so the verdict
  // has to say so rather than leave the reader to infer it from a list of booleans.
  CHECK(any_mentions(verdict.degraded, "shaderBufferInt64Atomics"));
  CHECK(any_mentions(verdict.degraded, "32-bit visibility buffer"));
  CHECK(any_mentions(verdict.degraded, "VK_EXT_mesh_shader"));
  CHECK(any_mentions(verdict.degraded, "VK_KHR_acceleration_structure"));
  CHECK(any_mentions(verdict.degraded, "VK_KHR_swapchain"));

  // A row name that does not exist is an error, so a misspelled profile cannot quietly test
  // nothing.
  gfx::DeviceCaps edited = caps;
  gfx::DeviceOverrides bad;
  bad.absent.push_back("meshShader");  // the row is named after the extension
  std::string error;
  CHECK_FALSE(gfx::apply_overrides(edited, bad, &error));
  CHECK(error.find("meshShader") != std::string::npos);

  gfx::DeviceOverrides good;
  good.absent.push_back("VK_EXT_mesh_shader");
  good.limits.push_back(gfx::LimitOverride{"maxPushConstantsSize", 64});
  REQUIRE(gfx::apply_overrides(edited, good, &error));
  CHECK(edited.mesh_shader == 0);
  CHECK(edited.max_push_constants_size == 64);
  gfx::evaluate_requirements(edited, rows);
  CHECK_FALSE(gfx::requirements_met({rows.data(), rows.size()}));

  MESSAGE("requirements table: " << required << " required, " << optional << " optional, " << notes
                                 << " notes");
}

TEST_CASE("gfx: the bindless set clamps to the device and keeps its floors") {
  gfx::DeviceCaps caps = minimal_caps();
  caps.max_per_stage_uab_sampled_images = 2048;
  caps.max_set_uab_sampled_images = 2048;
  caps.max_per_stage_uab_storage_images = 64;
  caps.max_set_uab_storage_images = 64;
  caps.max_per_stage_uab_samplers = 16;
  caps.max_set_uab_samplers = 16;
  caps.max_per_stage_uab_resources = 4096;

  Vector<gfx::DeviceClamp> clamps;
  const gfx::BindlessCapacity granted =
      gfx::clamp_bindless(caps, gfx::default_bindless_capacity(), &clamps);
  CHECK(granted.sampled_images == 2048);
  CHECK(granted.storage_images == 64);
  CHECK(granted.samplers == 16);
  CHECK(granted.push_constant_bytes == gfx::k_push_constant_bytes);
  // No acceleration structures on this profile, so binding 3 does not exist.
  CHECK(granted.acceleration_structures == 0);
  CHECK(clamps.size() >= 3);
  for (const gfx::DeviceClamp& clamp : clamps) {
    CHECK(clamp.granted < clamp.requested);
    CHECK_FALSE(clamp.why.empty());
  }

  // Sampled and storage images share one per-stage budget: the storage array keeps its floor and
  // the texture array takes what is left, because nothing in the renderer writes a bindless
  // storage image and a texture is what a scene runs out of.
  caps.max_per_stage_uab_resources = 600;
  clamps.clear();
  const gfx::BindlessCapacity tight =
      gfx::clamp_bindless(caps, gfx::default_bindless_capacity(), &clamps);
  CHECK(tight.storage_images == gfx::k_min_storage_images);
  CHECK(tight.sampled_images == 600 - gfx::k_min_storage_images);
  CHECK(tight.sampled_images + tight.storage_images <= 600);
}

TEST_CASE("gfx: a Kepler-like device is refused, and the message names the version") {
  if (!driver_available()) return;
  gfx::Device device;
  gfx::DeviceOptions options;
  options.overrides.api_version = 1002;  // "Vulkan 1.2", which is Kepler's ceiling
  std::string error;
  CHECK_FALSE(device.create(options, &error));
  CHECK_FALSE(device.valid());
  MESSAGE("refusal: " << error);
  CHECK(error.find("apiVersion") != std::string::npos);
  CHECK(error.find("1.3") != std::string::npos);
  CHECK(error.find("1.2") != std::string::npos);
  // The verdict outlives the refusal, so a caller can report all of it and not just the message.
  CHECK_FALSE(device.verdict().usable);
  CHECK(device.verdict().tier == "none");
  CHECK(any_mentions(device.verdict().blocking, "apiVersion"));
  CHECK_FALSE(device.requirements().empty());
}

TEST_CASE("gfx: an Intel-Gen9-like device clamps the bindless set and still draws") {
  if (!driver_available()) return;
  gfx::Device device;
  gfx::DeviceOptions options;
  // An integrated part's shape: update-after-bind budgets in the thousands rather than the
  // millions. The numbers are plausible rather than measured — no one here has the machine yet —
  // and what the case asserts is the *behaviour*: clamp, do not fail.
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxPerStageDescriptorUpdateAfterBindSampledImages", 2048});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxDescriptorSetUpdateAfterBindSampledImages", 2048});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxPerStageDescriptorUpdateAfterBindStorageImages", 64});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxDescriptorSetUpdateAfterBindStorageImages", 64});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxPerStageDescriptorUpdateAfterBindSamplers", 16});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxDescriptorSetUpdateAfterBindSamplers", 16});
  options.overrides.limits.push_back(
      gfx::LimitOverride{"maxPerStageUpdateAfterBindResources", 4096});
  std::string error;
  if (!open_device(device, options, error)) return;
  REQUIRE(device.valid());
  CHECK(device.verdict().usable);
  CHECK(any_mentions(device.verdict().degraded, "sampled_images"));

  gfx::BindlessSet bindless;
  REQUIRE_MESSAGE(bindless.create(device, gfx::BindlessConfig{}, &error), error);
  CHECK(bindless.capacity().sampled_images == 2048);
  CHECK(bindless.capacity().storage_images == 64);
  CHECK(bindless.capacity().samplers == 16);
  CHECK(bindless.capacity().push_constant_bytes == gfx::k_push_constant_bytes);
  CHECK(bindless.pipeline_layout() != VK_NULL_HANDLE);
  bindless.destroy();

  // And the device really works: the same clear-and-readback device_tests.cpp runs, so a clamp
  // is a smaller set and not a broken one.
  constexpr u32 k_width = 32;
  constexpr u32 k_height = 24;
  gfx::ImageResource image;
  REQUIRE_MESSAGE(
      gfx::create_image_2d(device, k_width, k_height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, image,
                           &error),
      error);
  gfx::BufferResource readback;
  const u64 bytes = u64{k_width} * k_height * 4;
  REQUIRE_MESSAGE(gfx::create_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     /*host_visible=*/true, readback, &error),
                  error);
  std::memset(readback.mapped, 0, static_cast<usize>(bytes));
  REQUIRE(gfx::submit_immediate(
      device,
      [&](VkCommandBuffer commands) {
        gfx::image_barrier(commands, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue color{};
        color.float32[0] = 1.0f;
        color.float32[3] = 1.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &range);
        gfx::image_barrier(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {k_width, k_height, 1};
        vkCmdCopyImageToBuffer(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &region);
      },
      &error));
  const auto* pixels = static_cast<const u8*>(readback.mapped);
  u32 wrong = 0;
  for (u32 i = 0; i < k_width * k_height; ++i) {
    const u8* p = pixels + i * 4;
    if (p[0] != 255 || p[1] != 0 || p[2] != 0 || p[3] != 255) ++wrong;
  }
  CHECK(wrong == 0);
  gfx::destroy_buffer(device, readback);
  gfx::destroy_image(device, image);
  device.destroy();
}

TEST_CASE("gfx: a device without 64-bit buffer atomics says exactly what stops") {
  if (!driver_available()) return;
  gfx::Device device;
  gfx::DeviceOptions options;
  options.overrides.absent.push_back("shaderBufferInt64Atomics");
  std::string error;
  if (!open_device(device, options, error)) return;
  REQUIRE(device.valid());
  // The device is created — the visibility buffer is not a creation requirement — and the flag
  // every rasterizer guards on is off, so the suites that need it skip on this profile exactly
  // as they would on the hardware.
  CHECK(device.verdict().usable);
  CHECK_FALSE(device.features().buffer_int64_atomics);
  CHECK(device.verdict().tier == device.adapter().tier);
  REQUIRE(any_mentions(device.verdict().degraded, "shaderBufferInt64Atomics"));
  for (const std::string& line : device.verdict().degraded)
    MESSAGE(line);
  device.destroy();

  // Without mesh shaders as well — the Maxwell case — nothing can draw a frame at all, and the
  // verdict has to name the follow-up rather than leave it as "no picture".
  gfx::DeviceOptions maxwell;
  maxwell.overrides.absent.push_back("shaderBufferInt64Atomics");
  maxwell.overrides.absent.push_back("VK_EXT_mesh_shader");
  gfx::Device second;
  if (!open_device(second, maxwell, error)) return;
  CHECK(second.verdict().usable);
  CHECK_FALSE(second.features().mesh_shader);
  CHECK(any_mentions(second.verdict().degraded, "32-bit visibility buffer"));
  second.destroy();
}
