// `--memory-report` (memory_report.h; docs/subsystems/apps.md, "--memory-report: where the commit
// is"; the numbers it found are docs/experiments/memory-commit-accounting.md).
#include "memory_report.h"

#include <core/containers/vector.h>
#include <core/log/log.h>
#include <core/memory/large_allocations.h>
#include <core/memory/memory.h>
#include <core/platform/address_space.h>
#include <core/time/time.h>
#include <domain/gfx/device.h>
#include <domain/gfx/device_memory.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace engine::view {

ENGINE_LOG_CATEGORY_DEFINE(log_view_memory, "view.memory");

namespace {

// Resident samples per large allocation: a 1 GiB block is 262,144 pages, so 512 samples give its
// touched share to about a fifth of a percent of the block.
constexpr u32 k_samples_per_block = 512;
// The sites (distinct call stacks) the report lists, largest untouched first.
constexpr u32 k_sites_listed = 40;

f64 mib(u64 bytes) { return std::round(static_cast<f64>(bytes) / (1024.0 * 1024.0) * 10.0) / 10.0; }

// A return address as `module+0xoffset`: symbolized afterwards against the module's debug
// information (cdb `ln`, addr2line), because loading symbols in the process would move the very
// numbers the report is about.
std::string frame_name(void* address) {
  char text[320];
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (::GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCSTR>(address), &module) &&
      module != nullptr) {
    char path[MAX_PATH] = {};
    ::GetModuleFileNameA(module, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    base = base != nullptr ? base + 1 : path;
    std::snprintf(text, sizeof(text), "%s+0x%llx", base,
                  static_cast<unsigned long long>(reinterpret_cast<usize>(address) -
                                                  reinterpret_cast<usize>(module)));
    return text;
  }
#else
  Dl_info info{};
  if (::dladdr(address, &info) != 0 && info.dli_fname != nullptr) {
    const char* base = std::strrchr(info.dli_fname, '/');
    base = base != nullptr ? base + 1 : info.dli_fname;
    std::snprintf(text, sizeof(text), "%s+0x%llx", base,
                  static_cast<unsigned long long>(reinterpret_cast<usize>(address) -
                                                  reinterpret_cast<usize>(info.dli_fbase)));
    return text;
  }
#endif
  std::snprintf(text, sizeof(text), "0x%llx",
                static_cast<unsigned long long>(reinterpret_cast<usize>(address)));
  return text;
}

JsonValue stack_json(const mem::LargeAllocation& a) {
  JsonValue out = JsonValue::array();
  for (u32 i = 0; i < a.depth; ++i)
    out.push_back(frame_name(a.stack[i]));
  return out;
}

bool same_stack(const mem::LargeAllocation& a, const mem::LargeAllocation& b) {
  return a.depth == b.depth && std::memcmp(a.stack, b.stack, sizeof(void*) * a.depth) == 0;
}

struct Site {
  u32 first = 0;  // an allocation with this stack, in the copied table
  u32 count = 0;
  u64 bytes = 0;
  u64 resident = 0;
};

JsonValue kind_json(const platform::RegionKindTotals& k) {
  JsonValue out = JsonValue::object();
  out.set("committed_mib", mib(k.committed));
  out.set("resident_mib", mib(k.resident));
  out.set("untouched_mib", mib(k.committed > k.resident ? k.committed - k.resident : 0));
  out.set("reserved_only_mib", mib(k.reserved_only));
  out.set("allocations", k.allocations);
  return out;
}

JsonValue owner_json(const char* name, u64 committed, u64 used, const char* note) {
  JsonValue out = JsonValue::object();
  out.set("name", name);
  out.set("committed_mib", mib(committed));
  out.set("used_mib", mib(used));
  out.set("note", note);
  return out;
}

JsonValue snapshot(const char* when, const gfx::Device* device) {
  const i64 t0 = time::monotonic_ns();
  platform::AddressSpaceReport walk;
  (void)platform::walk_address_space(walk);
  platform::HeapTotals heaps;
  (void)platform::heap_totals(heaps);
  gfx::DeviceMemoryReport gpu;
  if (device != nullptr) (void)gfx::device_memory_report(*device, gpu);
  const mem::Stats heap = mem::total_stats();

  // The live large allocations, each with its resident estimate, summed by call stack.
  Vector<mem::LargeAllocation> large;
  large.resize(mem::k_large_allocation_slots);
  const u32 live = mem::large_allocations(large.data(), mem::k_large_allocation_slots);
  const u32 copied = live < mem::k_large_allocation_slots ? live : mem::k_large_allocation_slots;
  Vector<u64> resident;
  resident.resize(copied);
  u64 large_bytes = 0, large_resident = 0;
  Vector<Site> sites;
  for (u32 i = 0; i < copied; ++i) {
    const mem::LargeAllocation& a = large[i];
    resident[i] = platform::resident_estimate(reinterpret_cast<const void*>(a.address), a.bytes,
                                              k_samples_per_block);
    large_bytes += a.bytes;
    large_resident += resident[i];
    u32 s = 0;
    while (s < sites.size() && !same_stack(large[sites[s].first], a))
      ++s;
    if (s == sites.size()) sites.push_back(Site{i, 0, 0, 0});
    sites[s].count += 1;
    sites[s].bytes += a.bytes;
    sites[s].resident += resident[i];
  }
  // Largest untouched first (a short list: an insertion sort is plenty).
  for (u32 i = 1; i < sites.size(); ++i) {
    for (u32 j = i;
         j > 0 && sites[j - 1].bytes - sites[j - 1].resident < sites[j].bytes - sites[j].resident;
         --j) {
      const Site t = sites[j];
      sites[j] = sites[j - 1];
      sites[j - 1] = t;
    }
  }

  JsonValue out = JsonValue::object();
  out.set("when", when);
  JsonValue process = JsonValue::object();
  process.set("commit_mib", mib(walk.process_commit));
  process.set("peak_commit_mib", mib(walk.peak_process_commit));
  process.set("working_set_mib", mib(walk.working_set));
  process.set("peak_working_set_mib", mib(walk.peak_working_set));
  out.set("process", std::move(process));
  JsonValue kinds = JsonValue::object();
  for (u32 k = 0; k < platform::k_region_kinds; ++k)
    kinds.set(platform::region_kind_name(static_cast<platform::RegionKind>(k)),
              kind_json(walk.kinds[k]));
  out.set("kinds", std::move(kinds));
  out.set("allocations", walk.allocations);
  out.set("sample_stride_pages", walk.sample_stride_pages);
  out.set("sampled_pages", walk.sampled_pages);

  const u64 private_committed =
      walk.kinds[static_cast<u32>(platform::RegionKind::private_memory)].committed;
  JsonValue owners = JsonValue::array();
  owners.push_back(owner_json("engine heap", heap.bytes_current, heap.bytes_current,
                              "mem::allocate, live bytes (the C runtime heap holds them)"));
  if (heaps.valid)
    owners.push_back(owner_json("c runtime heaps", heaps.committed, heaps.allocated,
                                "every heap of the process as it counts itself"));
  if (gpu.valid) {
    owners.push_back(owner_json("device host-visible blocks", gpu.host_block_bytes,
                                gpu.host_allocation_bytes,
                                "the allocator's blocks in system memory the driver maps"));
    owners.push_back(owner_json("device-local blocks", gpu.device_local_block_bytes,
                                gpu.device_local_allocation_bytes,
                                "on the GPU; listed to compare with the private total"));
  }
  owners.push_back(owner_json("large allocations (logged)", large_bytes, large_resident,
                              "live blocks of 16 MiB or more since the log was turned on; "
                              "used is the resident estimate"));
  // On Windows the driver backs every device-local byte with a byte of the process's private
  // commit that is never touched unless the memory is evicted (gfx's device_memory_tests: 512 MiB
  // uploaded, +513 MiB private, +1 MiB resident). Linux charges nothing for it.
#if defined(_WIN32)
  const u64 backing = gpu.valid ? gpu.device_local_block_bytes : 0;
  if (gpu.valid)
    owners.push_back(owner_json("driver backing for device-local blocks", backing, 0,
                                "the driver's system-memory backing, one byte per device-local "
                                "byte, untouched unless evicted"));
#else
  const u64 backing = 0;
#endif
  const u64 named = (heaps.valid ? heaps.committed : heap.bytes_current) +
                    (gpu.valid ? gpu.host_block_bytes : 0) + backing;
  owners.push_back(owner_json("private, no named owner",
                              private_committed > named ? private_committed - named : 0, 0,
                              "private commit outside the heaps, the allocator's host blocks and "
                              "the driver's backing: the driver's other allocations, thread "
                              "stacks, other libraries' VirtualAlloc"));
  out.set("owners", std::move(owners));
  if (gpu.valid) {
    JsonValue device_block = JsonValue::object();
    device_block.set("device_local_usage_mib", mib(gpu.device_local_usage_bytes));
    device_block.set("host_usage_mib", mib(gpu.host_usage_bytes));
    device_block.set("blocks", gpu.block_count);
    device_block.set("allocations", gpu.allocation_count);
    out.set("device", std::move(device_block));
  }

  // A region, matched to the logged large allocation that made it when there is one.
  const auto region_json = [&](const platform::AddressRegion& r) {
    JsonValue e = JsonValue::object();
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(r.base));
    e.set("address", text);
    e.set("committed_mib", mib(r.committed));
    e.set("resident_mib", mib(r.resident));
    e.set("reserved_mib", mib(r.reserved));
    std::snprintf(text, sizeof(text), "0x%x", r.protect);
    e.set("protect", text);
    for (u32 a = 0; a < copied; ++a) {
      if (large[a].address < r.base || large[a].address >= r.base + r.reserved) continue;
      e.set("owner", "engine heap");
      e.set("bytes_mib", mib(large[a].bytes));
      e.set("tag", mem::tag_name(large[a].tag));
      e.set("stack", stack_json(large[a]));
      break;
    }
    return e;
  };
  JsonValue largest = JsonValue::array();
  for (u32 i = 0; i < walk.largest_count; ++i)
    largest.push_back(region_json(walk.largest[i]));
  out.set("largest_private", std::move(largest));
  JsonValue untouched = JsonValue::array();
  for (u32 i = 0; i < walk.untouched_count; ++i)
    untouched.push_back(region_json(walk.untouched[i]));
  out.set("most_untouched_private", std::move(untouched));
  static constexpr const char* k_bucket_names[platform::k_address_space_size_buckets] = {
      "under_1mib",   "1_to_4mib",      "4_to_16mib", "16_to_64mib",
      "64_to_256mib", "256mib_to_1gib", "1gib_up"};
  JsonValue sizes = JsonValue::object();
  for (u32 b = 0; b < platform::k_address_space_size_buckets; ++b) {
    const platform::SizeBucket& s = walk.private_sizes[b];
    JsonValue e = JsonValue::object();
    e.set("allocations", s.allocations);
    e.set("committed_mib", mib(s.committed));
    e.set("resident_mib", mib(s.resident));
    e.set("untouched_mib", mib(s.committed > s.resident ? s.committed - s.resident : 0));
    sizes.set(k_bucket_names[b], std::move(e));
  }
  out.set("private_by_size", std::move(sizes));
  JsonValue protections = JsonValue::array();
  for (u32 p = 0; p < walk.protection_count; ++p) {
    const platform::SizeBucket& s = walk.private_protections[p];
    JsonValue e = JsonValue::object();
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", walk.protections[p]);
    e.set("protect", text);
    e.set("allocations", s.allocations);
    e.set("committed_mib", mib(s.committed));
    e.set("resident_mib", mib(s.resident));
    protections.push_back(std::move(e));
  }
  out.set("private_by_protect", std::move(protections));

  JsonValue site_list = JsonValue::array();
  for (u32 s = 0; s < sites.size() && s < k_sites_listed; ++s) {
    const Site& site = sites[s];
    JsonValue e = JsonValue::object();
    e.set("count", site.count);
    e.set("bytes_mib", mib(site.bytes));
    e.set("resident_mib", mib(site.resident));
    e.set("untouched_mib", mib(site.bytes - site.resident));
    e.set("tag", mem::tag_name(large[site.first].tag));
    e.set("stack", stack_json(large[site.first]));
    site_list.push_back(std::move(e));
  }
  JsonValue large_block = JsonValue::object();
  large_block.set("live", live);
  large_block.set("dropped", mem::large_allocations_dropped());
  large_block.set("bytes_mib", mib(large_bytes));
  large_block.set("resident_mib", mib(large_resident));
  large_block.set("sites", static_cast<u32>(sites.size()));
  large_block.set("by_site", std::move(site_list));
  out.set("large_allocations", std::move(large_block));

  // The device's large buffers by call stack (gfx::set_large_buffer_log): what sized what the GPU
  // holds, and so — on Windows, where the driver backs every device-local byte with a byte of the
  // process's commit — what sized that commit too.
  Vector<gfx::LargeBuffer> buffers;
  buffers.resize(gfx::k_large_buffer_slots);
  const u32 live_buffers = gfx::large_buffers(buffers.data(), gfx::k_large_buffer_slots);
  const u32 buffer_count =
      live_buffers < gfx::k_large_buffer_slots ? live_buffers : gfx::k_large_buffer_slots;
  struct BufferSite {
    u32 first = 0;
    u32 count = 0;
    u64 bytes = 0;
    u64 largest = 0;
  };
  Vector<BufferSite> buffer_sites;
  u64 device_local_bytes = 0, host_visible_bytes = 0;
  for (u32 i = 0; i < buffer_count; ++i) {
    const gfx::LargeBuffer& b = buffers[i];
    (b.host_visible ? host_visible_bytes : device_local_bytes) += b.bytes;
    u32 s = 0;
    while (s < buffer_sites.size()) {
      const gfx::LargeBuffer& f = buffers[buffer_sites[s].first];
      if (f.host_visible == b.host_visible && f.depth == b.depth &&
          std::memcmp(f.stack, b.stack, sizeof(void*) * b.depth) == 0)
        break;
      ++s;
    }
    if (s == buffer_sites.size()) buffer_sites.push_back(BufferSite{i, 0, 0, 0});
    buffer_sites[s].count += 1;
    buffer_sites[s].bytes += b.bytes;
    if (b.bytes > buffer_sites[s].largest) buffer_sites[s].largest = b.bytes;
  }
  for (u32 i = 1; i < buffer_sites.size(); ++i) {
    for (u32 j = i; j > 0 && buffer_sites[j - 1].bytes < buffer_sites[j].bytes; --j) {
      const BufferSite t = buffer_sites[j];
      buffer_sites[j] = buffer_sites[j - 1];
      buffer_sites[j - 1] = t;
    }
  }
  JsonValue buffer_list = JsonValue::array();
  for (u32 s = 0; s < buffer_sites.size() && s < k_sites_listed; ++s) {
    const BufferSite& site = buffer_sites[s];
    const gfx::LargeBuffer& b = buffers[site.first];
    JsonValue e = JsonValue::object();
    e.set("count", site.count);
    e.set("bytes_mib", mib(site.bytes));
    e.set("largest_mib", mib(site.largest));
    e.set("host_visible", b.host_visible);
    JsonValue stack = JsonValue::array();
    for (u32 f = 0; f < b.depth; ++f)
      stack.push_back(frame_name(b.stack[f]));
    e.set("stack", std::move(stack));
    buffer_list.push_back(std::move(e));
  }
  JsonValue buffer_block = JsonValue::object();
  buffer_block.set("live", live_buffers);
  buffer_block.set("dropped", gfx::large_buffers_dropped());
  buffer_block.set("device_local_mib", mib(device_local_bytes));
  buffer_block.set("host_visible_mib", mib(host_visible_bytes));
  buffer_block.set("sites", static_cast<u32>(buffer_sites.size()));
  buffer_block.set("by_site", std::move(buffer_list));
  out.set("device_buffers", std::move(buffer_block));
  const f64 walk_ms = static_cast<f64>(time::monotonic_ns() - t0) / 1e6;
  out.set("walk_ms", walk_ms);

  const platform::RegionKindTotals& priv =
      walk.kinds[static_cast<u32>(platform::RegionKind::private_memory)];
  ENGINE_LOG_INFO(
      log_view_memory, "memory report", log::field("when", when),
      log::field("commit_mib", mib(walk.process_commit)),
      log::field("working_set_mib", mib(walk.working_set)),
      log::field("private_mib", mib(priv.committed)),
      log::field("private_resident_mib", mib(priv.resident)),
      log::field("mapped_mib",
                 mib(walk.kinds[static_cast<u32>(platform::RegionKind::mapped_file)].committed)),
      log::field("image_mib",
                 mib(walk.kinds[static_cast<u32>(platform::RegionKind::image)].committed)),
      log::field("engine_heap_mib", mib(heap.bytes_current)),
      log::field("crt_heaps_mib", mib(heaps.committed)),
      log::field("device_host_mib", mib(gpu.host_block_bytes)),
      log::field("device_local_mib", mib(gpu.device_local_block_bytes)),
      log::field("large_mib", mib(large_bytes)),
      log::field("large_resident_mib", mib(large_resident)), log::field("walk_ms", walk_ms));
  return out;
}

}  // namespace

void MemoryReport::enable() noexcept {
  on_ = true;
  mem::set_large_allocation_log(true);
  gfx::set_large_buffer_log(true);
}

void MemoryReport::take(const char* when, const gfx::Device* device) {
  if (!on_) return;
  const bool first = std::strcmp(when, "first_frame") == 0;
  JsonValue& slot = first ? first_frame_ : exit_;
  if (!slot.is_null()) return;
  slot = snapshot(when, device);
}

JsonValue MemoryReport::block() const {
  if (!on_) return JsonValue::null();
  JsonValue out = JsonValue::object();
  out.set("first_frame", first_frame_);
  out.set("exit", exit_);
  return out;
}

MemoryReport& memory_report() {
  static MemoryReport report;
  return report;
}

}  // namespace engine::view
