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

// The TITAN Xp in the project's Linux server, as its first `engine-cli gpu.adapters --report`
// recorded it on 2026-09-23 (Gentoo, driver 580.178.04, Vulkan 1.4.312; the report's rows are in
// `k_titan_xp_rows` below and the run is in docs/ci/self-hosted-runners.md). Every Required row at
// the value it reported and every Optional flag as its extension list had it. The shape that
// matters: acceleration structures and ray-tracing pipelines present — the driver's compute
// fallback on Pascal — and no ray query, no mesh shaders, no cluster acceleration structures.
gfx::DeviceCaps titan_xp_caps() {
  gfx::DeviceCaps caps = minimal_caps();
  caps.api_version = 1004;
  caps.max_push_constants_size = 256;
  caps.max_compute_workgroup_invocations = 1536;
  caps.max_compute_workgroup_size_x = 1536;
  caps.max_compute_workgroup_size_y = 1024;
  caps.max_compute_shared_memory_size = 49152;
  caps.max_memory_allocation_count = 4294967295u;
  caps.max_per_stage_uab_sampled_images = 1048576;
  caps.max_per_stage_uab_storage_images = 1048576;
  caps.max_per_stage_uab_samplers = 1048576;
  caps.max_per_stage_uab_resources = 4294967295u;
  caps.max_set_uab_sampled_images = 1048576;
  caps.max_set_uab_storage_images = 1048576;
  caps.max_set_uab_samplers = 1048576;
  caps.max_set_uab_acceleration_structures = 1048576;
  caps.max_storage_buffer_range = 4294967295u;
  caps.subgroup_size = 32;
  caps.swapchain = 1;
  caps.shader_buffer_int64_atomics = 1;
  caps.fragment_stores_and_atomics = 1;
  caps.deferred_host_operations = 1;
  caps.acceleration_structure = 1;
  caps.ray_tracing_pipeline = 1;
  caps.memory_budget = 1;
  caps.descriptor_buffer = 1;
  caps.sampler_anisotropy = 1;
  caps.index_type_uint8 = 1;
  caps.storage_buffer_16bit_access = 1;
  caps.storage_buffer_8bit_access = 1;
  // Absent: VK_EXT_mesh_shader, VK_KHR_ray_query, VK_NV_cluster_acceleration_structure,
  // VK_EXT_memory_decompression, VK_KHR_fragment_shading_rate.
  return caps;
}

// The report's rows, name, `found` and `pass`, exactly as the card wrote them. The fixture above
// has to evaluate to these, which is what makes it this card and not a guess at one.
struct ReportedRow {
  const char* name;
  const char* found;
  bool pass;
};
constexpr ReportedRow k_titan_xp_rows[] = {
    {"apiVersion", "1.4", true},
    {"graphics+compute queue family", "1", true},
    {"dynamicRendering", "true", true},
    {"synchronization2", "true", true},
    {"maintenance4", "true", true},
    {"bufferDeviceAddress", "true", true},
    {"descriptorIndexing", "true", true},
    {"runtimeDescriptorArray", "true", true},
    {"descriptorBindingPartiallyBound", "true", true},
    {"descriptorBindingSampledImageUpdateAfterBind", "true", true},
    {"descriptorBindingStorageImageUpdateAfterBind", "true", true},
    {"shaderSampledImageArrayNonUniformIndexing", "true", true},
    {"timelineSemaphore", "true", true},
    {"scalarBlockLayout", "true", true},
    {"hostQueryReset", "true", true},
    {"drawIndirectCount", "true", true},
    {"multiDrawIndirect", "true", true},
    {"shaderInt64", "true", true},
    {"maxPushConstantsSize", "256", true},
    {"maxComputeWorkGroupInvocations", "1536", true},
    {"maxComputeWorkGroupSize[0]", "1536", true},
    {"maxComputeWorkGroupSize[1]", "1024", true},
    {"maxComputeSharedMemorySize", "49152", true},
    {"maxMemoryAllocationCount", "4294967295", true},
    {"maxPerStageDescriptorUpdateAfterBindSampledImages", "1048576", true},
    {"maxPerStageDescriptorUpdateAfterBindStorageImages", "1048576", true},
    {"maxPerStageDescriptorUpdateAfterBindSamplers", "1048576", true},
    {"maxPerStageUpdateAfterBindResources", "4294967295", true},
    {"maxDescriptorSetUpdateAfterBindSampledImages", "1048576", true},
    {"maxDescriptorSetUpdateAfterBindStorageImages", "1048576", true},
    {"maxDescriptorSetUpdateAfterBindSamplers", "1048576", true},
    {"VK_KHR_swapchain", "true", true},
    {"shaderBufferInt64Atomics", "true", true},
    {"fragmentStoresAndAtomics", "true", true},
    {"VK_EXT_mesh_shader", "false", false},
    {"VK_KHR_deferred_host_operations", "true", true},
    {"VK_KHR_acceleration_structure", "true", true},
    {"VK_KHR_ray_query", "false", false},
    {"VK_KHR_ray_tracing_pipeline", "true", true},
    {"VK_NV_cluster_acceleration_structure", "false", false},
    {"maxDescriptorSetUpdateAfterBindAccelerationStructures", "1048576", true},
    {"VK_EXT_memory_budget", "true", true},
    {"VK_EXT_memory_decompression", "false", false},
    {"VK_EXT_descriptor_buffer", "true", true},
    {"samplerAnisotropy", "true", true},
    {"VK_KHR_fragment_shading_rate", "false", false},
    {"maxStorageBufferRange", "4095 MiB", true},
    {"VK_EXT_index_type_uint8", "true", true},
    {"subgroupSize", "32", true},
    {"storageBuffer16BitAccess", "true", true},
    {"storageBuffer8BitAccess", "true", true},
};

bool any_mentions(const Vector<std::string>& lines, const char* needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Creates a device or skips with the reason, as the other gfx tests do.
bool open_device(gfx::Device& device, const gfx::DeviceOptions& options, std::string& error) {
  if (!device.create(options, &error)) {
    MESSAGE("skipped: device unavailable: " << error);
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
  MESSAGE("skipped: device unavailable: " << driver_error());
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

TEST_CASE("gfx: the TITAN Xp's report is the baseline tier, and says why it cannot trace") {
  const gfx::DeviceCaps caps = titan_xp_caps();
  Vector<gfx::DeviceRequirement> rows;
  gfx::evaluate_requirements(caps, rows);
  for (const ReportedRow& reported : k_titan_xp_rows) {
    const gfx::DeviceRequirement* row = nullptr;
    for (const gfx::DeviceRequirement& candidate : rows) {
      if (candidate.name == reported.name) row = &candidate;
    }
    INFO("row ", reported.name);
    REQUIRE(row != nullptr);
    CHECK(row->found == reported.found);
    CHECK(row->pass == reported.pass);
  }
  CHECK(gfx::requirements_met({rows.data(), rows.size()}));

  // Acceleration structures and ray-tracing pipelines, and no ray query: "raster", not "rt". The
  // rule that called this card "rt" keyed on the pipeline, which nothing here builds.
  CHECK(std::string(gfx::hardware_tier(caps)) == "raster");
  gfx::DeviceVerdict verdict;
  gfx::device_verdict(caps, {rows.data(), rows.size()}, verdict);
  CHECK(verdict.usable);
  CHECK(verdict.tier == "raster");
  CHECK(verdict.blocking.empty());
  CHECK(verdict.clamps.empty());
  for (const std::string& line : verdict.degraded)
    MESSAGE(line);
  // Exactly two things are missing that a reader needs told about: mesh shaders, and every ray.
  REQUIRE(verdict.degraded.size() == 2);
  CHECK(verdict.degraded[0].find("no VK_EXT_mesh_shader") == 0);
  const std::string rays = gfx::ray_tracing_degradation(caps);
  CHECK(verdict.degraded[1] == rays);
  CHECK(rays.find("no VK_KHR_ray_query and no VK_NV_cluster_acceleration_structure") == 0);
  CHECK(rays.find("--shadows rt") != std::string::npos);
  CHECK(rays.find("--raster rt") != std::string::npos);
  CHECK(rays.find("\"raster\"") != std::string::npos);
  CHECK(rays.find("VK_KHR_ray_tracing_pipeline are present") != std::string::npos);
}

TEST_CASE("gfx: the rt tier is ray queries, not ray-tracing pipelines") {
  // Every ray-tracing consumer in the engine is a ray query, so the tier keys on that row; and the
  // sentence the verdict carries names exactly what is missing for the renderer's ray paths, which
  // today also need the cluster structures (the KHR fallback is gfx's, not the renderer's yet).
  struct Case {
    const char* what;
    u32 acceleration_structure;
    u32 ray_query;
    u32 ray_tracing_pipeline;
    u32 cluster;
    const char* tier;
    const char* sentence_starts;  // empty: every ray path of the renderer runs
  };
  const Case cases[] = {
      {"no ray tracing at all", 0, 0, 0, 0, "raster", "no VK_KHR_acceleration_structure:"},
      {"Pascal's compute fallback", 1, 0, 1, 0, "raster",
       "no VK_KHR_ray_query and no VK_NV_cluster_acceleration_structure:"},
      {"cluster structures without ray queries", 1, 0, 1, 1, "raster", "no VK_KHR_ray_query:"},
      {"ray queries and no pipeline", 1, 1, 0, 0, "rt", "no VK_NV_cluster_acceleration_structure:"},
      {"KHR ray tracing", 1, 1, 1, 0, "rt", "no VK_NV_cluster_acceleration_structure:"},
      {"RTX with cluster structures", 1, 1, 1, 1, "rt-cluster", ""},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    gfx::DeviceCaps caps = minimal_caps();
    caps.shader_buffer_int64_atomics = 1;
    caps.fragment_stores_and_atomics = 1;
    caps.deferred_host_operations = c.acceleration_structure;
    caps.acceleration_structure = c.acceleration_structure;
    caps.ray_query = c.ray_query;
    caps.ray_tracing_pipeline = c.ray_tracing_pipeline;
    caps.cluster_acceleration_structure = c.cluster;
    caps.max_set_uab_acceleration_structures = c.acceleration_structure != 0 ? 1024u : 0u;
    CHECK(std::string(gfx::hardware_tier(caps)) == c.tier);
    Vector<gfx::DeviceRequirement> rows;
    gfx::evaluate_requirements(caps, rows);
    gfx::DeviceVerdict verdict;
    gfx::device_verdict(caps, {rows.data(), rows.size()}, verdict);
    CHECK(verdict.tier == c.tier);
    const std::string sentence = gfx::ray_tracing_degradation(caps);
    if (c.sentence_starts[0] == '\0') {
      CHECK(sentence.empty());
      CHECK_FALSE(any_mentions(verdict.degraded, "--raster rt"));
    } else {
      CHECK(sentence.find(c.sentence_starts) == 0);
      bool carried = false;
      for (const std::string& line : verdict.degraded)
        carried = carried || line == sentence;
      CHECK(carried);
    }
  }
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
