#include <domain/gfx/requirements.h>

#include <algorithm>
#include <cstring>

namespace engine::gfx {

namespace {

// How a row's `needed` and `found` are turned into text.
enum class Unit : u8 {
  Flag,        // a Vulkan VkBool32: "true" / "false"
  Count,       // a plain limit: ">= 128" / "256"
  Bytes,       // a limit in bytes, printed in MiB
  ApiVersion,  // major * 1000 + minor: "1.3"
};

// One row of the table. Everything it reads is a `u32` member of `DeviceCaps` — a feature bit is
// a 0 or a 1 — so one member pointer and one minimum cover a feature, an extension, a limit and
// a queue count alike, and the rendering is decided by `unit` rather than by a second kind of
// row.
struct Requirement {
  const char* name;
  const char* kind;  // "api", "feature", "extension", "limit", "queue"
  RequirementLevel level;
  Unit unit;
  u32 DeviceCaps::* field;
  u32 needed;
  const char* unlocks;  // Optional rows only
  const char* why;
};

constexpr const char* k_none = "";

// ---- the table ---------------------------------------------------------------------------
//
// Order is the order a reader wants it: what makes the device usable at all, then what each
// capability tier needs, then what is reported because somebody will come looking for it.
constexpr Requirement k_requirements[] = {
    // ---- the device is usable at all ----
    {"apiVersion", "api", RequirementLevel::Required, Unit::ApiVersion, &DeviceCaps::api_version,
     1003, k_none,
     "the engine creates a Vulkan 1.3 instance and enables the 1.3 core feature chain; a 1.2 "
     "device cannot be handed it, and a 1.2 loader cannot even enumerate for one."},
    {"graphics+compute queue family", "queue", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::graphics_compute_families, 1, k_none,
     "one queue family carries both the render graph's raster passes and its compute passes; a "
     "compute-only and a transfer-only family are used when the device has them and alias this "
     "one when it does not."},

    // ---- Vulkan 1.3 core ----
    {"dynamicRendering", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::dynamic_rendering, 1, k_none,
     "every raster pass begins rendering with its attachments directly; the render graph has no "
     "VkRenderPass and no framebuffer objects at all."},
    {"synchronization2", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::synchronization2, 1, k_none,
     "every barrier the render graph derives from a pass's declared accesses is a "
     "VkDependencyInfo with stage and access masks 2."},
    {"maintenance4", "feature", RequirementLevel::Required, Unit::Flag, &DeviceCaps::maintenance4,
     1, k_none,
     "relaxed interface matching between shader stages, and the memory-requirement queries that "
     "need no object; part of the 1.3 chain Device::create() enables."},

    // ---- Vulkan 1.2 core ----
    {"bufferDeviceAddress", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::buffer_device_address, 1, k_none,
     "every buffer a shader reads is reached by its address rather than by a descriptor "
     "(ADR-0023): the scene, the visible list, the visibility buffer, the parameter blocks. "
     "Nothing in the renderer binds a storage buffer."},
    {"descriptorIndexing", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::descriptor_indexing, 1, k_none,
     "the one global descriptor set of ADR-0023 is descriptor indexing; there is no per-draw "
     "set to fall back to."},
    {"runtimeDescriptorArray", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::runtime_descriptor_array, 1, k_none,
     "g_textures[], g_storage_images[] and g_samplers[] are declared unbounded."},
    {"descriptorBindingPartiallyBound", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::descriptor_binding_partially_bound, 1, k_none,
     "slots are written as resources are created, so a pipeline binds the set with most of it "
     "never written."},
    {"descriptorBindingSampledImageUpdateAfterBind", "feature", RequirementLevel::Required,
     Unit::Flag, &DeviceCaps::descriptor_binding_sampled_image_update_after_bind, 1, k_none,
     "a texture registers into the set while earlier command buffers that bound it are still "
     "recorded; that is what update-after-bind means and what the set's flags ask for."},
    {"descriptorBindingStorageImageUpdateAfterBind", "feature", RequirementLevel::Required,
     Unit::Flag, &DeviceCaps::descriptor_binding_storage_image_update_after_bind, 1, k_none,
     "the storage-image array carries the same update-after-bind flag as the sampled one, so "
     "the set layout is invalid without it."},
    {"shaderSampledImageArrayNonUniformIndexing", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::shader_sampled_image_array_non_uniform_indexing, 1, k_none,
     "the material resolve samples g_textures[NonUniformResourceIndex(material.albedo_texture)]: "
     "the index varies per pixel and therefore across a wave, and every shaded picture goes "
     "through that pass."},
    {"timelineSemaphore", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::timeline_semaphore, 1, k_none,
     "FrameContext tracks GPU progress on one timeline (frame k signals k+1); deferred resource "
     "destruction and bindless slot release are keyed on its value."},
    {"scalarBlockLayout", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::scalar_block_layout, 1, k_none,
     "every GPU-mirrored struct (ClusterDesc, MeshDesc, InstanceDesc, the parameter blocks) is "
     "laid out the way C++ lays it out; the size table pins those layouts."},
    {"hostQueryReset", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::host_query_reset, 1, k_none,
     "GpuTimer resets a frame slot's timestamp pool from the host after reading it, rather than "
     "recording a reset into the command buffer."},
    {"drawIndirectCount", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::draw_indirect_count, 1, k_none,
     "required by Device::create() since the first indirect draw. Today's call sites are "
     "vkCmdDrawIndirect, vkCmdDrawIndexedIndirect and vkCmdDrawMeshTasksIndirectEXT with a draw "
     "count of one, which needs neither this nor multiDrawIndirect, so this row is the first to "
     "relax if a device is ever refused for it. A draw per cluster through vkCmdDrawIndirectCount "
     "was measured for the vertex path and cost the TITAN Xp 0.45 ms a frame whatever the cut."},
    {"multiDrawIndirect", "feature", RequirementLevel::Required, Unit::Flag,
     &DeviceCaps::multi_draw_indirect, 1, k_none,
     "enabled beside drawIndirectCount and required with it; see that row."},
    {"shaderInt64", "feature", RequirementLevel::Required, Unit::Flag, &DeviceCaps::shader_int64, 1,
     k_none,
     "every push-constant block carries buffer device addresses as uint64_t, and a SPIR-V module "
     "that declares a 64-bit integer needs the Int64 capability. Without it not one shader of "
     "the renderer can be turned into a pipeline."},

    // ---- limits the renderer depends on ----
    {"maxPushConstantsSize", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_push_constants_size, k_push_constant_bytes, k_none,
     "gfx::ClusterDrawParams is exactly 128 bytes and is the largest block the renderer allows; "
     "the bindless pipeline layout declares that range for all stages (ADR-0023). It is also "
     "Vulkan's guaranteed minimum, so a conforming device cannot fail it."},
    {"maxComputeWorkGroupInvocations", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_compute_workgroup_invocations, k_workgroup_invocations, k_none,
     "deform_alloc.slang is 256 x 1 x 1 and hiz_build.slang is 16 x 16 x 1. Vulkan guarantees "
     "only 128, so this is a real question on a device nobody here has run."},
    {"maxComputeWorkGroupSize[0]", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_compute_workgroup_size_x, k_workgroup_size_x, k_none,
     "the deformed-vertex pool's allocator is one workgroup of 256 threads on x."},
    {"maxComputeWorkGroupSize[1]", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_compute_workgroup_size_y, k_workgroup_size_y, k_none,
     "hiz_build.slang folds a 32 x 32 tile with 16 x 16 threads."},
    {"maxComputeSharedMemorySize", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_compute_shared_memory_size, k_shared_memory_bytes, k_none,
     "cluster_sw_raster.slang keeps 255 screen positions and 255 front-facing flags in shared "
     "memory, about 4 KiB. Vulkan guarantees 16 KiB."},
    {"maxMemoryAllocationCount", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_memory_allocation_count, k_memory_allocations, k_none,
     "VMA suballocates, so the engine's allocations are counted in dozens: the GPU scene's "
     "buffers, the frame slots' pools, the acceleration structures. Vulkan guarantees 4096."},
    {"maxPerStageDescriptorUpdateAfterBindSampledImages", "limit", RequirementLevel::Required,
     Unit::Count, &DeviceCaps::max_per_stage_uab_sampled_images, k_min_sampled_images, k_none,
     "the bindless texture array is visible to every stage. Above this floor the array is "
     "clamped to the device and the renderer runs with a smaller scene; below it there is "
     "nothing to draw."},
    {"maxPerStageDescriptorUpdateAfterBindStorageImages", "limit", RequirementLevel::Required,
     Unit::Count, &DeviceCaps::max_per_stage_uab_storage_images, k_min_storage_images, k_none,
     "the bindless storage-image array; nothing in the renderer writes one today, so the floor "
     "is the RHI's own compute tests and room for one tool pass."},
    {"maxPerStageDescriptorUpdateAfterBindSamplers", "limit", RequirementLevel::Required,
     Unit::Count, &DeviceCaps::max_per_stage_uab_samplers, k_min_samplers, k_none,
     "the renderer creates a nearest and a linear sampler."},
    {"maxPerStageUpdateAfterBindResources", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_per_stage_uab_resources, k_min_sampled_images + k_min_storage_images, k_none,
     "sampled images and storage images share one per-stage budget, and the set is bound to "
     "every stage, so the two arrays have to fit in it together (samplers do not count against "
     "it)."},
    {"maxDescriptorSetUpdateAfterBindSampledImages", "limit", RequirementLevel::Required,
     Unit::Count, &DeviceCaps::max_set_uab_sampled_images, k_min_sampled_images, k_none,
     "the per-set budget for the same array as the per-stage row above; the smaller of the two "
     "is what the set is clamped to."},
    {"maxDescriptorSetUpdateAfterBindStorageImages", "limit", RequirementLevel::Required,
     Unit::Count, &DeviceCaps::max_set_uab_storage_images, k_min_storage_images, k_none,
     "the per-set budget for the storage-image array."},
    {"maxDescriptorSetUpdateAfterBindSamplers", "limit", RequirementLevel::Required, Unit::Count,
     &DeviceCaps::max_set_uab_samplers, k_min_samplers, k_none,
     "the per-set budget for the sampler array."},

    // ---- optional: what each capability unlocks ----
    {"VK_KHR_swapchain", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::swapchain, 1, "presentation",
     "Swapchain and FrameContext::end_frame(PresentSync). Every offscreen path — render.capture, "
     "engine-view --reference, every GPU test — works without it."},
    {"shaderBufferInt64Atomics", "feature", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::shader_buffer_int64_atomics, 1, "visibility-buffer",
     "the visibility buffer is one 64-bit word per pixel taken with an atomic max on a storage "
     "buffer, written by the mesh, vertex, software and ray-traced paths alike. Without it only "
     "--raster direct, which needs mesh shaders, can put a picture on the screen."},
    {"fragmentStoresAndAtomics", "feature", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::fragment_stores_and_atomics, 1, "visibility-buffer",
     "fs_visibility is a fragment shader that writes the visibility buffer; it is the fragment "
     "stage of both hardware rasterizers."},
    {"VK_EXT_mesh_shader", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::mesh_shader, 1, "mesh-shaders",
     "one workgroup per cluster (cluster_mesh.slang). Without it --raster hw, direct and auto "
     "fall back to the vertex-shader baseline tier, which draws the same visibility buffer "
     "(ADR-0024)."},
    {"geometryShader", "feature", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::geometry_shader, 1, "indexed-vertex-path",
     "not for a geometry shader — none exists here — but for SV_PrimitiveID in the fragment "
     "stage of a vertex pipeline, which is the same SPIR-V capability. The vertex path draws a "
     "culled cut indexed, so the vertex cache shares a cluster's vertices between its triangles, "
     "and names each triangle by its primitive id (cluster_vertex_indexed.slang); without it the "
     "path draws every cluster's whole triangle capacity, one vertex invocation per corner."},
    {"fullDrawIndexUint32", "feature", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::full_draw_index_uint32, 1, "indexed-vertex-path",
     "an index of the vertex path's indexed draw is `slot << 8 | local vertex`, so a run of more "
     "than 65,536 visible clusters has index values past 2^24 - 1, the most a device without "
     "this feature has to honour. Wanted beside geometryShader; without either the path draws "
     "every cluster's capacity."},
    {"VK_KHR_deferred_host_operations", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::deferred_host_operations, 1, "ray-tracing",
     "VK_KHR_acceleration_structure requires it; the engine enables neither without the other."},
    {"VK_KHR_acceleration_structure", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::acceleration_structure, 1, "ray-tracing",
     "bottom- and top-level structures (acceleration.h), binding 3 of the bindless set, and "
     "everything the ray paths build."},
    {"VK_KHR_ray_query", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::ray_query, 1, "ray-tracing",
     "ray-traced shadows in the resolve, ray_visibility.slang, and the reference path tracer, "
     "all of which trace from a RayQuery rather than from a ray-tracing pipeline. This row and "
     "VK_KHR_acceleration_structure are what the \"rt\" tier is decided by."},
    {"VK_KHR_ray_tracing_pipeline", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::ray_tracing_pipeline, 1, "ray-tracing",
     "no pass builds a ray-tracing pipeline, so this row decides nothing and is reported for the "
     "reader. It used to decide the \"rt\" tier, until a Pascal TITAN Xp advertised it through "
     "the driver's compute fallback with no ray query and was called \"rt\" while unable to "
     "trace one ray the renderer asks for."},
    {"VK_NV_cluster_acceleration_structure", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::cluster_acceleration_structure, 1, "cluster-acceleration-structures",
     "the per-frame cluster structures of ADR-0025, built from the GPU's own cut. They are the "
     "only structures the renderer builds today, so --raster rt, ray-traced shadows and the "
     "reference path tracer all need this; ADR-0025's KHR fallback draws the same picture in "
     "gfx's tests and is not wired into the renderer yet."},
    {"maxDescriptorSetUpdateAfterBindAccelerationStructures", "limit", RequirementLevel::Optional,
     Unit::Count, &DeviceCaps::max_set_uab_acceleration_structures, k_min_acceleration_structures,
     "ray-tracing",
     "binding 3 of the bindless set holds the frame's top-level structures; a ViewSet shares one "
     "across its views."},
    {"VK_EXT_memory_budget", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::memory_budget, 1, "memory-budget",
     "Device::memory_budget() reports this process's share of the device-local heaps instead of "
     "their size alone, which is what says whether a measurement was taken on a busy card."},
    {"VK_EXT_memory_decompression", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::memory_decompression, 1, "memory-decompression",
     "reported and enabled; the streaming path does not use it yet."},
    {"VK_EXT_descriptor_buffer", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::descriptor_buffer, 1, "descriptor-buffer",
     "ADR-0023 left this as a fast path to adopt only if descriptor update cost ever appears in "
     "a profile; it is enabled and unused."},
    {"samplerAnisotropy", "feature", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::sampler_anisotropy, 1, "anisotropic-filtering",
     "create_sampler asks for no anisotropy today; the material pipeline will."},
    {"VK_KHR_fragment_shading_rate", "extension", RequirementLevel::Optional, Unit::Flag,
     &DeviceCaps::fragment_shading_rate, 1, "fragment-shading-rate",
     "reported for the peripheral-quality work of plan 04 §4.6; nothing reads it today."},

    // ---- notes: reported because a reader comes looking for them ----
    {"maxStorageBufferRange", "limit", RequirementLevel::Note, Unit::Bytes,
     &DeviceCaps::max_storage_buffer_range, 128u * 1024u * 1024u, k_none,
     "nothing enforces this. Every scene buffer is read through a device address (ADR-0023), "
     "which this limit does not bound; it is here because the largest buffer in the frame — the "
     "visibility buffer, 8 bytes a pixel, 190 MiB at 11520x2160 — is the number a reader comes "
     "to check, and 128 MiB, Vulkan's guaranteed minimum, is what it would have to clear if it "
     "were ever bound as a descriptor."},
    {"VK_EXT_index_type_uint8", "extension", RequirementLevel::Note, Unit::Flag,
     &DeviceCaps::index_type_uint8, 1, k_none,
     "not needed. The cluster format's 8-bit local indices are widened on the host for the KHR "
     "builders (expand_packed_triangles) and consumed natively by "
     "VK_NV_cluster_acceleration_structure. The one index buffer bound, the vertex path's indexed "
     "draw, is 32-bit because an index carries the visible slot beside the local vertex."},
    {"subgroupSize", "limit", RequirementLevel::Note, Unit::Count, &DeviceCaps::subgroup_size, 1,
     k_none,
     "not needed. No shader in the tree uses a subgroup operation; subgroupSizeControl and "
     "computeFullSubgroups are enabled when offered and nothing requires them. Reported because "
     "the pool allocator's serial prefix sum is the first thing a wave-level scan would replace."},
    {"storageBuffer16BitAccess", "feature", RequirementLevel::Note, Unit::Flag,
     &DeviceCaps::storage_buffer_16bit_access, 1, k_none,
     "not needed. The 16-bit position grid is read out of the 32-bit words containing it "
     "(load_position in scene.slang) for exactly this reason."},
    {"storageBuffer8BitAccess", "feature", RequirementLevel::Note, Unit::Flag,
     &DeviceCaps::storage_buffer_8bit_access, 1, k_none,
     "not needed. Packed triangle indices and skin bindings are read as words and unpacked with "
     "shifts, which is also fewer loads."},
};

std::string count_text(u32 value) { return std::to_string(value); }

std::string bytes_text(u32 value) { return std::to_string(value >> 20) + " MiB"; }

std::string api_text(u32 value) {
  return std::to_string(value / 1000u) + "." + std::to_string(value % 1000u);
}

std::string needed_text(const Requirement& row) {
  switch (row.unit) {
    case Unit::Flag: return "true";
    case Unit::Count: return ">= " + count_text(row.needed);
    case Unit::Bytes: return ">= " + bytes_text(row.needed);
    case Unit::ApiVersion: return ">= " + api_text(row.needed);
  }
  return {};
}

std::string found_text(const Requirement& row, u32 value) {
  switch (row.unit) {
    case Unit::Flag: return value != 0 ? "true" : "false";
    case Unit::Count: return count_text(value);
    case Unit::Bytes: return bytes_text(value);
    case Unit::ApiVersion: return api_text(value);
  }
  return {};
}

const Requirement* find_row(std::string_view name) noexcept {
  for (const Requirement& row : k_requirements) {
    if (name == row.name) return &row;
  }
  return nullptr;
}

void append_reason(Vector<std::string>& out, std::string text) { out.push_back(std::move(text)); }

}  // namespace

// ---- overrides -----------------------------------------------------------------------------

bool apply_overrides(DeviceCaps& caps, const DeviceOverrides& overrides, std::string* error) {
  for (const std::string& name : overrides.absent) {
    const Requirement* row = find_row(name);
    if (row == nullptr) {
      if (error != nullptr) *error = "no requirement named '" + name + "'";
      return false;
    }
    caps.*(row->field) = 0;
  }
  for (const LimitOverride& limit : overrides.limits) {
    const Requirement* row = find_row(limit.name);
    if (row == nullptr) {
      if (error != nullptr) *error = "no requirement named '" + limit.name + "'";
      return false;
    }
    caps.*(row->field) = limit.value;
  }
  if (overrides.api_version != 0) caps.api_version = overrides.api_version;
  return true;
}

// ---- evaluation ----------------------------------------------------------------------------

void evaluate_requirements(const DeviceCaps& caps, Vector<DeviceRequirement>& out) {
  out.clear();
  out.reserve(static_cast<u32>(sizeof(k_requirements) / sizeof(k_requirements[0])));
  for (const Requirement& row : k_requirements) {
    const u32 value = caps.*(row.field);
    DeviceRequirement entry;
    entry.name = row.name;
    entry.kind = row.kind;
    entry.level = row.level;
    entry.needed = needed_text(row);
    entry.found = found_text(row, value);
    entry.pass = value >= row.needed;
    entry.unlocks = row.unlocks;
    entry.why = row.why;
    out.push_back(std::move(entry));
  }
}

bool requirements_met(std::span<const DeviceRequirement> rows) noexcept {
  for (const DeviceRequirement& row : rows) {
    if (row.level == RequirementLevel::Required && !row.pass) return false;
  }
  return true;
}

// ---- bindless clamping ---------------------------------------------------------------------

BindlessCapacity default_bindless_capacity() noexcept {
  BindlessCapacity wanted;
  wanted.sampled_images = 16384;
  wanted.storage_images = 4096;
  wanted.samplers = 256;
  wanted.acceleration_structures = 256;
  wanted.push_constant_bytes = k_push_constant_bytes;
  return wanted;
}

namespace {

void note_clamp(Vector<DeviceClamp>* clamps, const char* name, u32 requested, u32 granted,
                u32 floor_value, const char* why) {
  if (clamps == nullptr || granted >= requested) return;
  DeviceClamp clamp;
  clamp.name = name;
  clamp.requested = requested;
  clamp.granted = granted;
  clamp.floor = floor_value;
  clamp.why = why;
  clamps->push_back(std::move(clamp));
}

}  // namespace

BindlessCapacity clamp_bindless(const DeviceCaps& caps, const BindlessCapacity& wanted,
                                Vector<DeviceClamp>* clamps) {
  BindlessCapacity granted = wanted;
  granted.sampled_images =
      std::min(wanted.sampled_images,
               std::min(caps.max_per_stage_uab_sampled_images, caps.max_set_uab_sampled_images));
  granted.storage_images =
      std::min(wanted.storage_images,
               std::min(caps.max_per_stage_uab_storage_images, caps.max_set_uab_storage_images));
  granted.samplers = std::min(wanted.samplers,
                              std::min(caps.max_per_stage_uab_samplers, caps.max_set_uab_samplers));
  granted.acceleration_structures =
      caps.acceleration_structure != 0
          ? std::min(wanted.acceleration_structures, caps.max_set_uab_acceleration_structures)
          : 0u;
  granted.push_constant_bytes = std::min(wanted.push_constant_bytes, caps.max_push_constants_size);

  // Sampled and storage images share one per-stage budget. Give the storage array its floor
  // first, because nothing in the renderer writes a bindless storage image and a texture is the
  // resource a scene actually runs out of.
  const u32 total = granted.sampled_images + granted.storage_images;
  if (caps.max_per_stage_uab_resources != 0 && total > caps.max_per_stage_uab_resources) {
    const u32 room = caps.max_per_stage_uab_resources;
    const u32 storage = std::min(granted.storage_images, k_min_storage_images);
    granted.storage_images = storage;
    granted.sampled_images = room > storage ? std::min(granted.sampled_images, room - storage) : 0u;
  }

  note_clamp(clamps, "sampled_images", wanted.sampled_images, granted.sampled_images,
             k_min_sampled_images,
             "one bindless slot per image; the device's update-after-bind budget is smaller than "
             "the default. A scene with more images than this reports a full array rather than "
             "drawing the wrong texture (ADR-0017).");
  note_clamp(clamps, "storage_images", wanted.storage_images, granted.storage_images,
             k_min_storage_images,
             "nothing in the renderer writes a bindless storage image today, so a clamp here "
             "costs nothing but the RHI's own compute tests.");
  note_clamp(clamps, "samplers", wanted.samplers, granted.samplers, k_min_samplers,
             "the renderer creates two samplers; a clamp here is never reached in practice.");
  note_clamp(clamps, "acceleration_structures", wanted.acceleration_structures,
             granted.acceleration_structures, k_min_acceleration_structures,
             "binding 3 exists only on a device with acceleration structures, and a frame needs "
             "one top-level structure per ViewSet.");
  note_clamp(clamps, "push_constant_bytes", wanted.push_constant_bytes, granted.push_constant_bytes,
             k_push_constant_bytes,
             "this one is never a clamp in practice: 128 bytes is Vulkan's guaranteed minimum "
             "and maxPushConstantsSize is a Required row, so a device that gets here has room.");
  return granted;
}

// ---- verdict -------------------------------------------------------------------------------

const char* hardware_tier(const DeviceCaps& caps) noexcept {
  // Ray queries, not ray-tracing pipelines: see the header. The pipeline flag is still reported
  // (it is a row of the table) and decides nothing.
  const bool rt = caps.acceleration_structure != 0 && caps.ray_query != 0;
  if (!rt) return "raster";
  return caps.cluster_acceleration_structure != 0 ? "rt-cluster" : "rt";
}

std::string ray_tracing_degradation(const DeviceCaps& caps) {
  if (caps.acceleration_structure == 0) {
    return "no VK_KHR_acceleration_structure: --raster rt, ray-traced shadows (--shadows rt) and "
           "the reference path tracer are unavailable, and binding 3 of the bindless set does not "
           "exist.";
  }
  if (caps.ray_query == 0) {
    std::string text = caps.cluster_acceleration_structure == 0
                           ? "no VK_KHR_ray_query and no VK_NV_cluster_acceleration_structure: "
                           : "no VK_KHR_ray_query: ";
    text +=
        "every ray the renderer traces is a ray query (the shadowed resolve from the fragment "
        "stage, the ray-traced visibility and the reference path tracer from compute), so --raster "
        "rt, ray-traced shadows (--shadows rt) and the reference path tracer are unavailable and "
        "the tier is \"raster\"";
    if (caps.ray_tracing_pipeline != 0) {
      text +=
          ", although VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline are present "
          "(Pascal's driver advertises both through its compute fallback; nothing here builds a "
          "ray-tracing pipeline)";
    }
    text += ".";
    return text;
  }
  if (caps.cluster_acceleration_structure == 0) {
    return "no VK_NV_cluster_acceleration_structure: the renderer builds its per-frame structures "
           "only as cluster acceleration structures, so --raster rt, ray-traced shadows (--shadows "
           "rt) and the reference path tracer are unavailable. ADR-0025's fallback, one KHR "
           "geometry per visible cluster and a pixel-identical picture for about three times the "
           "memory, is built and traced by gfx's own tests but not by the renderer yet.";
  }
  return {};
}

void device_verdict(const DeviceCaps& caps, std::span<const DeviceRequirement> rows,
                    DeviceVerdict& out) {
  out = DeviceVerdict{};
  for (const DeviceRequirement& row : rows) {
    if (row.level != RequirementLevel::Required || row.pass) continue;
    append_reason(out.blocking, row.name + std::string(": needs ") + row.needed +
                                    ", this device reports " + row.found + " — " + row.why);
  }
  out.usable = out.blocking.empty();
  out.tier = out.usable ? hardware_tier(caps) : "none";

  clamp_bindless(caps, default_bindless_capacity(), &out.clamps);
  for (const DeviceClamp& clamp : out.clamps) {
    append_reason(out.degraded, "bindless " + clamp.name + " clamped from " +
                                    std::to_string(clamp.requested) + " to " +
                                    std::to_string(clamp.granted) + ": " + clamp.why);
  }
  if (!out.usable) return;

  // What will not run although the device is usable. Each sentence names the flag, the passes it
  // costs, and what the renderer does instead — because "mesh_shader: false" in a report tells
  // the owner nothing about whether a picture will appear.
  if (caps.shader_buffer_int64_atomics == 0) {
    std::string text =
        "no shaderBufferInt64Atomics: the visibility buffer is a 64-bit atomic max per pixel, so "
        "the mesh (--raster hw), vertex, software and ray-traced paths cannot write it, and the "
        "material resolve has nothing to read.";
    if (caps.mesh_shader != 0) {
      text +=
          " Only --raster direct, which draws mesh-shader output straight to colour, can put "
          "a picture on the screen.";
    } else {
      text +=
          " With no mesh shaders either, this device cannot draw a frame at all; the "
          "follow-up that would change that is a 32-bit visibility buffer (depth and id "
          "packed into one word), which is not built.";
    }
    append_reason(out.degraded, std::move(text));
  }
  if (caps.fragment_stores_and_atomics == 0) {
    append_reason(out.degraded,
                  "no fragmentStoresAndAtomics: fs_visibility is the fragment stage of both "
                  "hardware rasterizers and writes the visibility buffer from it, so only the "
                  "software rasterizer (--raster sw) is left.");
  }
  if (caps.mesh_shader == 0) {
    append_reason(out.degraded,
                  "no VK_EXT_mesh_shader: --raster hw, direct and auto fall back to the "
                  "vertex-shader baseline tier (ADR-0024), which draws the same visibility "
                  "buffer from the same clusters and the same cull pass.");
    if (caps.geometry_shader == 0 || caps.full_draw_index_uint32 == 0) {
      std::string missing = caps.geometry_shader == 0 ? "geometryShader" : "";
      if (caps.full_draw_index_uint32 == 0) {
        missing += missing.empty() ? "fullDrawIndexUint32" : " or fullDrawIndexUint32";
      }
      append_reason(out.degraded,
                    "no " + missing +
                        " either: the baseline tier cannot draw its cut indexed, so it runs a "
                        "vertex invocation per triangle corner of every cluster's whole "
                        "capacity — on a TITAN Xp drawing 64 FlightHelmets that was 1.8 times the "
                        "indexed draw's raster pass.");
    }
  }
  if (std::string text = ray_tracing_degradation(caps); !text.empty()) {
    append_reason(out.degraded, std::move(text));
  }
  if (caps.swapchain == 0) {
    append_reason(out.degraded,
                  "no VK_KHR_swapchain: engine-view cannot present, and its tests treat that as "
                  "a skip. Every offscreen path still works.");
  }
  if (caps.memory_budget == 0) {
    append_reason(out.degraded,
                  "no VK_EXT_memory_budget: Device::memory_budget() reports the heaps' size "
                  "rather than this process's share of them, so a measurement cannot say whether "
                  "the card was busy.");
  }
}

// ---- description ---------------------------------------------------------------------------

std::string describe_requirements(std::span<const DeviceRequirement> rows) {
  usize width = 0;
  for (const DeviceRequirement& row : rows)
    width = std::max(width, row.name.size());
  std::string text;
  for (const DeviceRequirement& row : rows) {
    const char* mark = row.pass                                  ? "[x] "
                       : row.level == RequirementLevel::Required ? "[!] "
                                                                 : "[ ] ";
    text.append(mark);
    text.append(row.name);
    text.append(width + 2 - row.name.size(), ' ');
    text.append("need ");
    text.append(row.needed);
    text.append(", found ");
    text.append(row.found);
    if (!row.unlocks.empty()) {
      text.append(" (");
      text.append(row.unlocks);
      text.push_back(')');
    }
    text.push_back('\n');
  }
  return text;
}

}  // namespace engine::gfx
