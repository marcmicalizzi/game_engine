#pragma once

// What the renderer needs of a Vulkan device, as data (docs/subsystems/gfx.md "What a device
// has to have", docs/plan/04-renderer.md §4.1, ADR-0023).
//
// **One table, two readers.** `Device::create()` refuses a device whose Required rows fail, and
// `enumerate_adapters()` puts every row into `AdapterInfo::requirements` so that a machine
// nobody here has ever run on can say exactly why it can or cannot run the renderer. Before
// this table the two were different code — creation checked a hand-written boolean expression
// and the report printed an extension list — so a limit the renderer depended on was in neither.
// They walk the same rows now, and a row added here is checked and reported in the same commit.
//
// The rows are checked against `DeviceCaps`, a flat copy of every property and feature they
// read, filled from a physical device by `read_device_caps()` (domain/gfx/vulkan.h, because that
// half needs Vulkan). `DeviceOverrides` edits that copy before the check, which is how a machine
// that has everything tests the refusal and the clamping of a machine that does not: the
// overrides name rows by the same `name` the table and the report use, so a typo is an error
// rather than a test that silently checks nothing.
//
// Nothing here includes Vulkan, so the evaluation, the verdict and the clamping are testable on
// a machine with no driver at all.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/gfx.h>
#include <span>
#include <string>

namespace engine::gfx {

// ---- what the renderer asks for ---------------------------------------------------------
//
// Every constant below is a floor derived from code in this tree, and its comment says from
// which. A number with no such derivation does not belong in the table: it would refuse a
// machine for a reason nobody could check.

// The push-constant block every pipeline shares (ADR-0023). `gfx::ClusterDrawParams` is exactly
// this large and is the largest block the renderer allows, so this is a floor, not a wish. It is
// also Vulkan's guaranteed minimum, so no conforming device fails it.
inline constexpr u32 k_push_constant_bytes = 128;

// The widest compute workgroup the engine dispatches: `deform_alloc.slang` is 256 x 1 x 1 and
// `hiz_build.slang` is 16 x 16 x 1. Vulkan guarantees only 128 invocations, so this one is a
// real question on a device nobody has tried.
inline constexpr u32 k_workgroup_invocations = 256;
inline constexpr u32 k_workgroup_size_x = 256;
inline constexpr u32 k_workgroup_size_y = 16;

// `cluster_sw_raster.slang` holds 255 screen positions (float3) and 255 front-facing flags in
// shared memory: 255 * 16 bytes rounded up to 4 KiB. Vulkan guarantees 16 KiB.
inline constexpr u32 k_shared_memory_bytes = 4096;

// VMA suballocates, so the engine's device memory allocations are counted in dozens: the GPU
// scene's buffers, the frame slots' pools, and the acceleration structures. 256 is the point at
// which a large scene plus its acceleration structures still fits with room to spare; Vulkan
// guarantees 4096.
inline constexpr u32 k_memory_allocations = 256;

// Bindless floors (ADR-0023). Above the floor the engine clamps to the device and keeps going;
// below it there is nothing to run, so it refuses and says which array.
//
// 256 sampled images: one slot per *image*, shared by every material that names it. The Khronos
// samples use 3 to 12; 256 is a scene of some 85 textured materials with a base colour, a
// metallic-roughness and a normal map each.
inline constexpr u32 k_min_sampled_images = 256;
// 8 storage images: nothing in the renderer writes a bindless storage image today — its targets
// are attachments and buffers behind device addresses — so this is the RHI's own compute tests
// and room for one tool pass.
inline constexpr u32 k_min_storage_images = 8;
// 4 samplers: the renderer creates a nearest and a linear one.
inline constexpr u32 k_min_samplers = 4;
// 1 acceleration structure: a `ViewSet` shares one top-level structure across its views.
inline constexpr u32 k_min_acceleration_structures = 1;

// ---- the device, flattened ----------------------------------------------------------------

// Every property and feature the table reads, in one struct so that a check, a report and a
// pretend-weaker-device all see the same thing. Filled by `read_device_caps()`.
struct DeviceCaps {
  // major * 1000 + minor, so 1.3 is 1003 and a comparison is one integer compare.
  u32 api_version = 0;

  // Vulkan 1.0 features.
  u32 multi_draw_indirect = 0;
  u32 shader_int64 = 0;
  u32 fragment_stores_and_atomics = 0;
  u32 sampler_anisotropy = 0;
  u32 texture_compression_bc = 0;  // BC1-BC7 sampled as stored: the content build's textures
  u32 fill_mode_non_solid = 0;
  u32 shader_int16 = 0;
  u32 shader_storage_image_write_without_format = 0;
  u32 shader_storage_image_read_without_format = 0;
  u32 vertex_pipeline_stores_and_atomics = 0;
  u32 geometry_shader = 0;
  u32 full_draw_index_uint32 = 0;

  // Vulkan 1.1 features.
  u32 shader_draw_parameters = 0;
  u32 storage_buffer_16bit_access = 0;

  // Vulkan 1.2 features.
  u32 buffer_device_address = 0;
  u32 descriptor_indexing = 0;
  u32 runtime_descriptor_array = 0;
  u32 descriptor_binding_partially_bound = 0;
  u32 descriptor_binding_variable_descriptor_count = 0;
  u32 descriptor_binding_sampled_image_update_after_bind = 0;
  u32 descriptor_binding_storage_image_update_after_bind = 0;
  u32 descriptor_binding_storage_buffer_update_after_bind = 0;
  u32 shader_sampled_image_array_non_uniform_indexing = 0;
  u32 shader_storage_buffer_array_non_uniform_indexing = 0;
  u32 timeline_semaphore = 0;
  u32 scalar_block_layout = 0;
  u32 host_query_reset = 0;
  u32 draw_indirect_count = 0;
  u32 shader_buffer_int64_atomics = 0;
  u32 shader_int8 = 0;
  u32 storage_buffer_8bit_access = 0;
  u32 shader_float16 = 0;
  u32 sampler_filter_minmax = 0;

  // Vulkan 1.3 features.
  u32 dynamic_rendering = 0;
  u32 synchronization2 = 0;
  u32 maintenance4 = 0;
  u32 shader_demote_to_helper_invocation = 0;
  u32 subgroup_size_control = 0;
  u32 compute_full_subgroups = 0;

  // Extensions, already AND-ed with the feature bit the engine would enable with them.
  u32 swapchain = 0;
  u32 mesh_shader = 0;
  // The task stage of VK_EXT_mesh_shader. No row of its own: no pass uses a task shader yet, and
  // it is here so that the enabled chain asks for it only where the device offers it.
  u32 task_shader = 0;
  u32 deferred_host_operations = 0;
  u32 acceleration_structure = 0;
  u32 ray_tracing_pipeline = 0;
  u32 ray_query = 0;
  u32 cluster_acceleration_structure = 0;
  u32 descriptor_buffer = 0;
  u32 memory_decompression = 0;
  u32 memory_budget = 0;
  u32 fragment_shading_rate = 0;
  u32 index_type_uint8 = 0;

  // Limits.
  u32 max_push_constants_size = 0;
  u32 max_compute_workgroup_invocations = 0;
  u32 max_compute_workgroup_size_x = 0;
  u32 max_compute_workgroup_size_y = 0;
  u32 max_compute_shared_memory_size = 0;
  u32 max_memory_allocation_count = 0;
  u32 max_storage_buffer_range = 0;
  u32 max_per_stage_uab_sampled_images = 0;
  u32 max_per_stage_uab_storage_images = 0;
  u32 max_per_stage_uab_samplers = 0;
  u32 max_per_stage_uab_resources = 0;
  u32 max_set_uab_sampled_images = 0;
  u32 max_set_uab_storage_images = 0;
  u32 max_set_uab_samplers = 0;
  u32 max_set_uab_acceleration_structures = 0;
  u32 subgroup_size = 0;

  // Queues: families that carry graphics and compute together, which is what the engine's
  // primary queue has to be.
  u32 graphics_compute_families = 0;
};

// ---- pretending to be a weaker device -----------------------------------------------------

// One limit, by the table's own row name.
struct LimitOverride {
  std::string name;
  u32 value = 0;
};

// A device that reports less than it has, so that the refusal, the clamping and the degraded
// paths can be exercised on hardware that has everything (`DeviceOptions::overrides`). It is a
// test and diagnosis lever, not a configuration knob: nothing in the engine sets it, and a
// device created with one enables only what the overridden caps say, so a profile that removes
// a feature really removes it from the created device.
struct DeviceOverrides {
  // Feature, extension or queue rows to report absent, by the table's `name`.
  Vector<std::string> absent;
  // Limit rows to report this value instead of the device's own.
  Vector<LimitOverride> limits;
  // major * 1000 + minor, as `DeviceCaps::api_version`; 0 leaves the device's own alone.
  u32 api_version = 0;

  bool empty() const noexcept { return absent.empty() && limits.empty() && api_version == 0; }
};

// Applies the overrides to `caps`. False with `error` when a name matches no row of the table,
// so a misspelled profile fails loudly rather than checking nothing.
bool apply_overrides(DeviceCaps& caps, const DeviceOverrides& overrides,
                     std::string* error = nullptr);

// ---- the table ----------------------------------------------------------------------------

// Fills `out` with one entry per row, in table order. Pure: no Vulkan, no device.
void evaluate_requirements(const DeviceCaps& caps, Vector<DeviceRequirement>& out);

// True when every Required row of `rows` passed.
bool requirements_met(std::span<const DeviceRequirement> rows) noexcept;

// The bindless set's sizes, before and after the device's update-after-bind limits.
struct BindlessCapacity {
  u32 sampled_images = 0;
  u32 storage_images = 0;
  u32 samplers = 0;
  u32 acceleration_structures = 0;
  u32 push_constant_bytes = 0;
};

// Clamps `wanted` to what `caps` allows, appending one `DeviceClamp` per array that moved.
// Arrays are clamped and not refused: a device with room for 2,048 textures runs the renderer
// perfectly well, it just cannot hold a scene of 16,384 of them, and refusing it would turn a
// smaller scene into no picture at all. An array clamped *below* its floor is a different
// answer, and `device_verdict` turns it into a blocking reason.
BindlessCapacity clamp_bindless(const DeviceCaps& caps, const BindlessCapacity& wanted,
                                Vector<DeviceClamp>* clamps);

// The default configuration the renderer asks for, which is what the report clamps and prints.
BindlessCapacity default_bindless_capacity() noexcept;

// Tier, usability, and the plain-words reasons. `rows` must come from `evaluate_requirements`
// over the same `caps`.
void device_verdict(const DeviceCaps& caps, std::span<const DeviceRequirement> rows,
                    DeviceVerdict& out);

// The tier the *hardware* advertises, from the caps alone: "raster", "rt", or "rt-cluster".
// `AdapterInfo::tier` is this; `DeviceVerdict::tier` is this or "none".
//
// "rt" is acceleration structures **and ray queries**, because every ray this engine traces is a
// ray query — the shadowed resolve from the fragment stage, the ray-traced visibility and the
// reference path tracer from compute — and nothing builds a ray-tracing pipeline. The rule used
// to key on `VK_KHR_ray_tracing_pipeline` instead, and the first GPU run on a Pascal TITAN Xp
// (driver 580) showed what that costs: the driver's compute fallback advertises acceleration
// structures and ray-tracing pipelines and no ray query, so the card reported "rt" and could not
// trace one ray the renderer asks for. Such a device is "raster", and its verdict says why.
// "rt-cluster" is "rt" plus `VK_NV_cluster_acceleration_structure`.
const char* hardware_tier(const DeviceCaps& caps) noexcept;

// The one sentence the verdict carries about ray tracing on `caps`, or empty when every ray-traced
// path of the renderer runs there: `--raster rt`, ray-traced shadows, and the reference path
// tracer, which today need acceleration structures, ray queries and cluster acceleration
// structures (the renderer builds only the cluster kind; ADR-0025's KHR fallback is gfx's and not
// wired into the renderer yet). It names exactly what is missing. `device_verdict` puts it in
// `degraded`, and a host that refuses a ray-traced request prints it
// (`renderer::unavailable_reason`), so the report a machine's owner reads before running anything
// and the refusal they get when they run it say the same thing in the same words.
std::string ray_tracing_degradation(const DeviceCaps& caps);

// One line per requirement for a log or a terminal: "  [x] maxPushConstantsSize  need >= 128,
// found 256".
std::string describe_requirements(std::span<const DeviceRequirement> rows);

}  // namespace engine::gfx
