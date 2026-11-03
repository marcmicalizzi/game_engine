#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/opacity_micromap.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/shader_library.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::gfx {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_micromap, "gfx.micromap");

void set_message(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
}

constexpr u64 k_input_alignment = 256;  // VUID: data and triangleArray addresses, 256-aligned

u64 align_up(u64 value, u64 alignment) noexcept {
  return (value + alignment - 1) / alignment * alignment;
}

// A bilinear lookup on the CPU with wrapped addressing, texel centres at half-integers: what the
// shader's `SampleLevel(..., 0).a` computes up to the filter's precision.
f32 bilinear(std::span<const u8> alpha, u32 width, u32 height, f32 u, f32 v) noexcept {
  const f32 x = u * static_cast<f32>(width) - 0.5f;
  const f32 y = v * static_cast<f32>(height) - 0.5f;
  const f32 fx = std::floor(x);
  const f32 fy = std::floor(y);
  const f32 ax = x - fx;
  const f32 ay = y - fy;
  auto texel = [&](i64 tx, i64 ty) {
    const i64 w = static_cast<i64>(width);
    const i64 h = static_cast<i64>(height);
    tx = ((tx % w) + w) % w;
    ty = ((ty % h) + h) % h;
    return static_cast<f32>(alpha[static_cast<usize>(ty * w + tx)]) / 255.0f;
  };
  const i64 ix = static_cast<i64>(fx);
  const i64 iy = static_cast<i64>(fy);
  const f32 a0 = texel(ix, iy) + (texel(ix + 1, iy) - texel(ix, iy)) * ax;
  const f32 a1 = texel(ix, iy + 1) + (texel(ix + 1, iy + 1) - texel(ix, iy + 1)) * ax;
  return a0 + (a1 - a0) * ay;
}

}  // namespace

void micromap_micro_triangle(u32 index, u32 level, f32 out[6]) noexcept {
  f32 b0[2] = {0.0f, 0.0f};
  f32 b1[2] = {1.0f, 0.0f};
  f32 b2[2] = {0.0f, 1.0f};
  for (u32 l = level; l > 0; --l) {
    const u32 digit = (index >> (2u * (l - 1u))) & 3u;
    const f32 m01[2] = {(b0[0] + b1[0]) * 0.5f, (b0[1] + b1[1]) * 0.5f};
    const f32 m12[2] = {(b1[0] + b2[0]) * 0.5f, (b1[1] + b2[1]) * 0.5f};
    const f32 m20[2] = {(b2[0] + b0[0]) * 0.5f, (b2[1] + b0[1]) * 0.5f};
    auto set = [](f32 to[2], const f32 from[2]) {
      to[0] = from[0];
      to[1] = from[1];
    };
    if (digit == 0u) {
      set(b1, m01);
      set(b2, m20);
    } else if (digit == 1u) {
      set(b0, m20);
      set(b1, m12);
      set(b2, m01);
    } else if (digit == 2u) {
      set(b0, m01);
      set(b2, m12);
    } else {
      set(b0, m12);
      set(b1, m20);
    }
  }
  out[0] = b0[0];
  out[1] = b0[1];
  out[2] = b1[0];
  out[3] = b1[1];
  out[4] = b2[0];
  out[5] = b2[1];
}

u32 micromap_reference_state(std::span<const u8> alpha, u32 width, u32 height, f32 factor,
                             f32 cutoff, const f32 t0[2], const f32 t1[2],
                             const f32 t2[2]) noexcept {
  auto says = [&](f32 a) -> u32 {
    if (a >= cutoff + k_micromap_alpha_margin) return 1u;
    if (a < cutoff - k_micromap_alpha_margin) return 2u;
    return 3u;
  };
  const f32 w = static_cast<f32>(width);
  const f32 h = static_cast<f32>(height);
  const f32 lo_x = std::min({t0[0], t1[0], t2[0]}) * w - 0.5f - k_micromap_texel_margin;
  const f32 lo_y = std::min({t0[1], t1[1], t2[1]}) * h - 0.5f - k_micromap_texel_margin;
  const f32 hi_x = std::max({t0[0], t1[0], t2[0]}) * w - 0.5f + k_micromap_texel_margin;
  const f32 hi_y = std::max({t0[1], t1[1], t2[1]}) * h - 0.5f + k_micromap_texel_margin;
  const i64 x0 = static_cast<i64>(std::floor(lo_x));
  const i64 y0 = static_cast<i64>(std::floor(lo_y));
  const i64 x1 = static_cast<i64>(std::floor(hi_x));
  const i64 y1 = static_cast<i64>(std::floor(hi_y));
  u32 seen = 0;
  if (u64(x1 - x0 + 1) * u64(y1 - y0 + 1) > k_micromap_max_quads) {
    seen = 3u;
  } else {
    const i64 wi = static_cast<i64>(width);
    const i64 hi = static_cast<i64>(height);
    for (i64 y = y0; y <= y1 + 1; ++y) {
      for (i64 x = x0; x <= x1 + 1; ++x) {
        const i64 tx = ((x % wi) + wi) % wi;
        const i64 ty = ((y % hi) + hi) % hi;
        seen |= says(factor * static_cast<f32>(alpha[static_cast<usize>(ty * wi + tx)]) / 255.0f);
      }
    }
  }
  if (seen == 1u) return k_micromap_opaque;
  if (seen == 2u) return k_micromap_transparent;
  const f32 cu = (t0[0] + t1[0] + t2[0]) / 3.0f;
  const f32 cv = (t0[1] + t1[1] + t2[1]) / 3.0f;
  return factor * bilinear(alpha, width, height, cu, cv) >= cutoff ? k_micromap_unknown_opaque
                                                                   : k_micromap_unknown_transparent;
}

bool opacity_micromap_limits(const Device& device, u32& max_2state_level,
                             u32& max_4state_level) noexcept {
  max_2state_level = 0;
  max_4state_level = 0;
  if (!device.features().opacity_micromap) return false;
  VkPhysicalDeviceOpacityMicromapPropertiesEXT omm{};
  omm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT;
  VkPhysicalDeviceProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &omm;
  vkGetPhysicalDeviceProperties2(device.handles().physical, &props);
  max_2state_level = omm.maxOpacity2StateSubdivisionLevel;
  max_4state_level = omm.maxOpacity4StateSubdivisionLevel;
  return true;
}

void destroy_opacity_micromaps(const Device& device, OpacityMicromapSet& set) noexcept {
  if (set.handle.valid()) {
    vkDestroyMicromapEXT(device.handles().device, vk::native(set.handle), nullptr);
  }
  destroy_buffer(device, set.storage);
  destroy_buffer(device, set.indices);
  destroy_buffer(device, set.cluster_first);
  destroy_buffer(device, set.tables);
  set = OpacityMicromapSet{};
}

namespace {

// **The build**, shared by the bake and `build_opacity_micromap`: one `VkMicromapEXT` over
// `per_level[l]` 4-state entries at each level, its states at `data_address` and its
// `VkMicromapTriangleEXT`s at `triangle_array` (both 256-aligned), into `out.storage` at offset 0.
// Blocking; the scratch is its own.
bool build_micromap(const Device& device, const u32* per_level, u64 data_address,
                    u64 triangle_array, OpacityMicromapSet& out, std::string* error) {
  Vector<VkMicromapUsageEXT> usage;
  for (u32 l = 0; l <= k_micromap_max_level; ++l) {
    if (per_level[l] == 0) continue;
    VkMicromapUsageEXT u{};
    u.count = per_level[l];
    u.subdivisionLevel = l;
    u.format = k_micromap_format_4state;
    usage.push_back(u);
  }
  VkMicromapBuildInfoEXT build{};
  build.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
  build.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
  build.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
  build.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
  build.usageCountsCount = static_cast<u32>(usage.size());
  build.pUsageCounts = usage.data();
  VkMicromapBuildSizesInfoEXT sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
  const VkDevice vk_device = device.handles().device;
  vkGetMicromapBuildSizesEXT(vk_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                             &sizes);
  if (sizes.micromapSize == 0) {
    set_message(error, "build_micromap: the driver gave no size for the micromap");
    return false;
  }
  constexpr BufferUsage k_storage = BufferUsage::Storage | BufferUsage::ShaderDeviceAddress;
  const u32 scratch_alignment = 256;
  BufferResource scratch;
  if (!create_buffer(device, sizes.micromapSize,
                     BufferUsage::MicromapStorage | BufferUsage::ShaderDeviceAddress, false,
                     out.storage, error) ||
      !create_buffer(device, std::max<u64>(sizes.buildScratchSize, 4) + scratch_alignment,
                     k_storage, false, scratch, error)) {
    destroy_buffer(device, scratch);
    return false;
  }
  VkMicromapCreateInfoEXT create{};
  create.sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
  create.buffer = vk::native(out.storage.buffer);
  create.offset = 0;
  create.size = sizes.micromapSize;
  create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
  VkMicromapEXT micromap = VK_NULL_HANDLE;
  if (const VkResult r = vkCreateMicromapEXT(vk_device, &create, nullptr, &micromap);
      r != VK_SUCCESS) {
    destroy_buffer(device, scratch);
    set_message(error, "build_micromap: vkCreateMicromapEXT failed (" +
                           std::to_string(static_cast<i32>(r)) + ")");
    return false;
  }
  out.handle = vk::wrap(micromap);
  // The micromap lives at its storage's start: that is the address a cluster record names.
  out.address = out.storage.address;
  build.dstMicromap = micromap;
  build.data.deviceAddress = data_address;
  build.scratchData.deviceAddress = align_up(scratch.address, scratch_alignment);
  build.triangleArray.deviceAddress = triangle_array;
  build.triangleArrayStride = k_micromap_triangle_bytes;
  const bool built = submit_immediate(
      device,
      [&](CommandList cb) {
        vkCmdBuildMicromapsEXT(vk::native(cb), 1, &build);
        acceleration_build_barrier(cb, PipelineStage::AllCommands,
                                   MemoryAccess::MemoryRead | MemoryAccess::MemoryWrite);
      },
      error);
  destroy_buffer(device, scratch);
  return built;
}

}  // namespace

bool build_opacity_micromap(const Device& device, std::span<const u32> data,
                            std::span<const MicromapTriangle> triangles, OpacityMicromapSet& out,
                            std::string* error) {
  destroy_opacity_micromaps(device, out);
  if (!device.features().opacity_micromap) {
    set_message(error, "build_opacity_micromap: the device has no VK_EXT_opacity_micromap");
    return false;
  }
  if (triangles.empty()) {
    set_message(error, "build_opacity_micromap: no triangles");
    return false;
  }
  u32 per_level[k_micromap_max_level + 1] = {};
  for (const MicromapTriangle& t : triangles) {
    if (t.level > k_micromap_max_level || t.format != k_micromap_format_4state) {
      set_message(error, "build_opacity_micromap: a triangle names a level or format it cannot");
      return false;
    }
    ++per_level[t.level];
  }
  constexpr BufferUsage k_input = BufferUsage::Storage | BufferUsage::ShaderDeviceAddress |
                                  BufferUsage::MicromapBuildInput |
                                  BufferUsage::AccelerationBuildInput;
  const u64 data_bytes = std::max<u64>(u64{data.size()} * sizeof(u32), 4);
  const u64 triangle_bytes = u64{triangles.size()} * sizeof(MicromapTriangle);
  BufferResource input;
  if (!create_buffer(device, k_input_alignment * 2 + align_up(data_bytes, k_input_alignment) +
                                 triangle_bytes,
                     k_input, true, input, error)) {
    return false;
  }
  const u64 data_address = align_up(input.address, k_input_alignment);
  const u64 triangle_array = data_address + align_up(data_bytes, k_input_alignment);
  auto* bytes = static_cast<u8*>(input.mapped);
  if (!data.empty()) {
    std::memcpy(bytes + (data_address - input.address), data.data(), data.size() * sizeof(u32));
  }
  std::memcpy(bytes + (triangle_array - input.address), triangles.data(), triangle_bytes);
  const bool built = build_micromap(device, per_level, data_address, triangle_array, out, error);
  destroy_buffer(device, input);
  if (!built) {
    destroy_opacity_micromaps(device, out);
    return false;
  }
  out.stats.storage_bytes = out.storage.size;
  return true;
}

bool bake_opacity_micromaps(const Device& device, ShaderLibrary& shaders,
                            const BindlessSet& bindless, const MicromapBakeInput& in,
                            OpacityMicromapSet& out, std::string* error) {
  destroy_opacity_micromaps(device, out);
  u32 limit_2 = 0;
  u32 limit_4 = 0;
  if (!opacity_micromap_limits(device, limit_2, limit_4)) {
    set_message(error, "bake_opacity_micromaps: the device has no VK_EXT_opacity_micromap");
    return false;
  }
  const u32 item_count = static_cast<u32>(in.items.size() / 2);
  if (item_count == 0 || in.triangle_counts.size() != item_count) return true;
  const i64 start = time::monotonic_ns();

  // Every masked triangle, as {item, triangle}, and each item's first triangle.
  Vector<u32> refs;
  Vector<u32> cluster_first(in.cluster_count, k_micromap_none);
  u32 triangle_count = 0;
  for (u32 i = 0; i < item_count; ++i) {
    const u32 cluster = in.items[2 * i];
    if (cluster < in.cluster_count) cluster_first[cluster] = triangle_count;
    for (u32 t = 0; t < in.triangle_counts[i]; ++t) {
      refs.push_back(i);
      refs.push_back(t);
    }
    triangle_count += in.triangle_counts[i];
  }
  if (triangle_count == 0) return true;

  const gfx::Shader* shader = shaders.get("opacity_micromap_bake", error);
  if (shader == nullptr) return false;
  const DescriptorSetLayoutHandle set_layout = bindless.layout();
  const std::span<const DescriptorSetLayoutHandle> layouts(&set_layout, 1);
  ComputePipeline pipeline;
  if (!create_compute_pipeline(device, shader->module, "omm_bake_main", layouts,
                               sizeof(MicromapBakeParams), pipeline, error)) {
    return false;
  }
  constexpr BufferUsage k_storage = BufferUsage::Storage | BufferUsage::ShaderDeviceAddress;
  constexpr BufferUsage k_input =
      k_storage | BufferUsage::MicromapBuildInput | BufferUsage::AccelerationBuildInput;
  BufferResource items;
  BufferResource refs_buffer;
  BufferResource codes;
  BufferResource counters;
  BufferResource indices_staging;
  BufferResource entries;
  BufferResource data;
  BufferResource scratch;
  auto cleanup = [&] {
    destroy_buffer(device, items);
    destroy_buffer(device, refs_buffer);
    destroy_buffer(device, codes);
    destroy_buffer(device, counters);
    destroy_buffer(device, indices_staging);
    destroy_buffer(device, entries);
    destroy_buffer(device, data);
    destroy_buffer(device, scratch);
    destroy_compute_pipeline(device, pipeline);
  };
  auto fail = [&] {
    cleanup();
    destroy_opacity_micromaps(device, out);
    return false;
  };
  if (!upload_buffer(device, in.items.data(), u64{item_count} * 2 * sizeof(u32), k_storage, items,
                     error) ||
      !upload_buffer(device, refs.data(), u64{refs.size()} * sizeof(u32), k_storage, refs_buffer,
                     error) ||
      !create_buffer(device, u64{triangle_count} * sizeof(u32), k_storage, true, codes, error) ||
      !create_buffer(device, 3 * sizeof(u64), k_storage, true, counters, error)) {
    return fail();
  }
  std::memset(counters.mapped, 0, 3 * sizeof(u64));
  refs = Vector<u32>{};

  MicromapBakeParams params;
  params.clusters = in.clusters;
  params.triangles = in.triangles;
  params.attributes = in.attributes;
  params.materials = in.materials;
  params.items = items.address;
  params.refs = refs_buffer.address;
  params.codes = codes.address;
  params.counters = counters.address;
  params.count = triangle_count;
  params.target_texels = in.target_texels > 0.0f ? in.target_texels : k_micromap_target_texels;
  const u32 groups = (triangle_count + k_micromap_bake_workgroup - 1) / k_micromap_bake_workgroup;
  const u32 groups_x = groups < k_micromap_bake_row ? groups : k_micromap_bake_row;
  const u32 groups_y = (groups + k_micromap_bake_row - 1) / k_micromap_bake_row;
  auto dispatch = [&](const MicromapBakeParams& p) {
    return submit_immediate(
        device,
        [&](CommandList cb) {
          cb.bind_pipeline(BindPoint::Compute, pipeline.pipeline);
          bindless.bind(cb, BindPoint::Compute);
          cb.push_constants(pipeline.layout, ShaderStage::Compute, 0, sizeof(p), &p);
          cb.dispatch(groups_x, groups_y, 1);
        },
        error);
  };

  // Classify at the cap, then lower it a level at a time while the budget is short: a coarser
  // level is never less conservative, only less decided.
  u32 cap = std::min({in.max_level, limit_4, k_micromap_max_level, 15u});
  const u32 first_cap = cap;
  Vector<i32> index_host(triangle_count, 0);
  Vector<u32> entry_host;
  u64 data_bytes = 0;
  MicromapStats stats;
  for (;;) {
    params.mode = 0u | (cap << 8);
    if (!dispatch(params)) return fail();
    const auto* code = static_cast<const u32*>(codes.mapped);
    stats = MicromapStats{};
    entry_host.clear();
    data_bytes = 0;
    for (u32 t = 0; t < triangle_count; ++t) {
      const u32 level = code[t] & 0xfu;
      const u32 seen = code[t] >> 4;
      if (seen == (1u << k_micromap_transparent)) {
        index_host[t] = k_micromap_fully_transparent;
        ++stats.special_transparent;
      } else if (seen == (1u << k_micromap_opaque)) {
        index_host[t] = k_micromap_fully_opaque;
        ++stats.special_opaque;
      } else if (seen == (1u << k_micromap_unknown_opaque)) {
        index_host[t] = k_micromap_fully_unknown_opaque;
        ++stats.special_unknown;
      } else if (seen == (1u << k_micromap_unknown_transparent)) {
        index_host[t] = k_micromap_fully_unknown_transparent;
        ++stats.special_unknown;
      } else {
        index_host[t] = static_cast<i32>(entry_host.size() / 2);
        entry_host.push_back(static_cast<u32>(data_bytes));
        entry_host.push_back(level | (k_micromap_format_4state << 16));
        data_bytes += micromap_entry_bytes(level);
        ++stats.entries_per_level[level];
      }
    }
    const u64 total = data_bytes + u64{triangle_count} * sizeof(i32) +
                      u64{entry_host.size() / 2} * k_micromap_triangle_bytes;
    if (in.budget_bytes == 0 || total <= in.budget_bytes || cap == 0) break;
    --cap;
  }
  stats.triangles = triangle_count;
  stats.level_cap = cap;
  stats.levels_dropped = first_cap - cap;
  stats.data_bytes = data_bytes;
  stats.index_bytes = u64{triangle_count} * sizeof(i32);
  if (stats.levels_dropped != 0) {
    ENGINE_LOG_WARN(log_micromap, "opacity micromap budget lowered the level cap",
                    log::field("from", first_cap), log::field("to", cap),
                    log::field("budget_bytes", in.budget_bytes));
  }

  // The tables the records read, and the per-triangle indices (special or entry).
  out.cluster_first_host = std::move(cluster_first);
  if (!upload_buffer(device, index_host.data(), u64{triangle_count} * sizeof(i32), k_storage,
                     out.indices, error) ||
      !upload_buffer(device, out.cluster_first_host.data(),
                     u64{std::max<u32>(in.cluster_count, 1u)} * sizeof(u32), k_storage,
                     out.cluster_first, error)) {
    return fail();
  }
  const u32 entry_count = static_cast<u32>(entry_host.size() / 2);
  if (entry_count == 0) {
    // Every triangle is a special index: a micromap with no triangle is not a thing to build, and
    // the records name none (`array` 0), so the ray loops decide every candidate as before.
    out.stats = stats;
    MicromapTables tables;
    tables.array = 0;
    tables.indices = out.indices.address;
    tables.cluster_first = out.cluster_first.address;
    if (!upload_buffer(device, &tables, sizeof(tables), k_storage, out.tables, error)) {
      return fail();
    }
    cleanup();
    out.stats.bake_ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
    return true;
  }

  // The data pass, into a 256-aligned input buffer.
  if (!upload_buffer(device, entry_host.data(), u64{entry_host.size()} * sizeof(u32), k_input,
                     entries, error) ||
      !create_buffer(device, align_up(std::max<u64>(data_bytes, 4), 4) + k_input_alignment, k_input,
                     false, data, error)) {
    return fail();
  }
  const u64 data_address = align_up(data.address, k_input_alignment);
  const u64 entries_address = entries.address;  // re-uploaded below if it is not aligned
  BufferResource entries_aligned;
  u64 triangle_array = entries_address;
  if (entries_address % k_input_alignment != 0) {
    if (!create_buffer(device, u64{entry_host.size()} * sizeof(u32) + k_input_alignment, k_input,
                       true, entries_aligned, error)) {
      return fail();
    }
    triangle_array = align_up(entries_aligned.address, k_input_alignment);
    std::memcpy(
        static_cast<u8*>(entries_aligned.mapped) + (triangle_array - entries_aligned.address),
        entry_host.data(), u64{entry_host.size()} * sizeof(u32));
  }
  params.mode = 1u | (cap << 8);
  params.indices = out.indices.address;
  params.entries = triangle_array;
  params.data = data_address;
  if (!dispatch(params)) {
    destroy_buffer(device, entries_aligned);
    return fail();
  }
  const auto* counted = static_cast<const u64*>(counters.mapped);
  stats.micro_transparent = counted[0];
  stats.micro_opaque = counted[1];
  stats.micro_unknown = counted[2];

  // The build: one micromap, its usage counts per level.
  const bool built = build_micromap(device, stats.entries_per_level, data_address, triangle_array,
                                    out, error);
  destroy_buffer(device, entries_aligned);
  if (!built) return fail();

  MicromapTables tables;
  tables.array = out.address;
  tables.indices = out.indices.address;
  tables.cluster_first = out.cluster_first.address;
  if (!upload_buffer(device, &tables, sizeof(tables), k_storage, out.tables, error)) return fail();
  cleanup();
  stats.storage_bytes = out.storage.size;
  stats.bake_ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  out.stats = stats;
  return true;
}

}  // namespace engine::gfx
