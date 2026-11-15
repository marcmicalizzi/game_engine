#include <core/platform/address_space.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// PSAPI_VERSION 2: GetProcessMemoryInfo and QueryWorkingSetEx are kernel32's K32 forms, so the
// walk costs no extra import library (process.cpp does the same).
#define PSAPI_VERSION 2
#include <psapi.h>
#else
#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace engine::platform {

const char* region_kind_name(RegionKind kind) noexcept {
  switch (kind) {
    case RegionKind::private_memory: return "private";
    case RegionKind::mapped_file: return "mapped";
    case RegionKind::image: return "image";
  }
  return "private";
}

namespace {

u64 untouched(const AddressRegion& r) noexcept {
  return r.committed > r.resident ? r.committed - r.resident : 0;
}

// Keeps the `k_address_space_largest` regions that rank highest by `rank`, highest first (an
// insertion into a short sorted array: the walk visits each allocation once).
template <class Rank>
void keep_top(AddressRegion* list, u32& count, const AddressRegion& region, Rank rank) noexcept {
  const u64 value = rank(region);
  if (value == 0) return;
  u32 at = count;
  while (at > 0 && rank(list[at - 1]) < value)
    --at;
  if (at >= k_address_space_largest) return;
  const u32 last = count < k_address_space_largest ? count : k_address_space_largest - 1;
  for (u32 i = last; i > at; --i)
    list[i] = list[i - 1];
  list[at] = region;
  if (count < k_address_space_largest) ++count;
}

u32 size_bucket(u64 committed) noexcept {
  u32 b = 0;
  u64 edge = u64{1} << 20;  // 1 MiB, then x4
  while (b + 1 < k_address_space_size_buckets && committed >= edge) {
    ++b;
    edge <<= 2;
  }
  return b;
}

// One allocation, walked: into its kind's totals, and for private memory into the size histogram
// and the two ranked lists.
void finish_allocation(AddressSpaceReport& out, const AddressRegion& region) noexcept {
  RegionKindTotals& k = out.kinds[static_cast<u32>(region.kind)];
  k.committed += region.committed;
  k.resident += region.resident;
  k.reserved_only += region.reserved - region.committed;
  ++k.allocations;
  ++out.allocations;
  if (region.kind != RegionKind::private_memory || region.committed == 0) return;
  SizeBucket& bucket = out.private_sizes[size_bucket(region.committed)];
  bucket.committed += region.committed;
  bucket.resident += region.resident;
  ++bucket.allocations;
  u32 p = 0;
  while (p < out.protection_count && out.protections[p] != region.protect)
    ++p;
  if (p == out.protection_count) {
    if (out.protection_count < k_address_space_protections) {
      out.protections[out.protection_count++] = region.protect;
    } else {
      p = k_address_space_protections - 1;
      out.protections[p] = 0xffffffffu;
    }
  }
  SizeBucket& by_protect = out.private_protections[p];
  by_protect.committed += region.committed;
  by_protect.resident += region.resident;
  ++by_protect.allocations;
  keep_top(out.largest, out.largest_count, region,
           [](const AddressRegion& r) { return r.committed; });
  keep_top(out.untouched, out.untouched_count, region, untouched);
}

#if ENGINE_PLATFORM_WINDOWS

RegionKind kind_of(DWORD type) noexcept {
  if (type == MEM_IMAGE) return RegionKind::image;
  if (type == MEM_MAPPED) return RegionKind::mapped_file;
  return RegionKind::private_memory;
}

// The address-space walk as a sequence of allocations: consecutive regions with one
// AllocationBase are one allocation (VirtualQuery returns them in address order).
template <class Fn>
void for_each_region(Fn&& fn) noexcept {
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  auto* at = static_cast<u8*>(si.lpMinimumApplicationAddress);
  auto* const end = static_cast<u8*>(si.lpMaximumApplicationAddress);
  MEMORY_BASIC_INFORMATION mbi{};
  while (at < end && ::VirtualQuery(at, &mbi, sizeof(mbi)) == sizeof(mbi)) {
    if (mbi.RegionSize == 0) break;
    if (mbi.State != MEM_FREE) fn(mbi);
    at = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
  }
}

// The sampled pages of one allocation, asked in batches; an allocation's batches never hold another
// allocation's pages, so its resident count is its own.
struct SampleBatch {
  static constexpr u32 k_size = 1024;
  PSAPI_WORKING_SET_EX_INFORMATION pages[k_size];
  u32 count = 0;
  u64 valid = 0;  // resident samples of the allocation being walked
};

void flush(SampleBatch& batch) noexcept {
  if (batch.count == 0) return;
  if (::QueryWorkingSetEx(
          ::GetCurrentProcess(), batch.pages,
          static_cast<DWORD>(sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * batch.count))) {
    for (u32 i = 0; i < batch.count; ++i)
      batch.valid += batch.pages[i].VirtualAttributes.Valid ? 1u : 0u;
  }
  batch.count = 0;
}

bool walk(AddressSpaceReport& out) noexcept {
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  const u64 page = si.dwPageSize;
  out.page_bytes = page;

  // Pass 1: how many pages are committed, which sets the sampling stride.
  u64 committed_pages = 0;
  for_each_region([&](const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State == MEM_COMMIT) committed_pages += mbi.RegionSize / page;
  });
  const u64 stride =
      committed_pages <= k_address_space_samples
          ? 1
          : (committed_pages + k_address_space_samples - 1) / k_address_space_samples;
  out.sample_stride_pages = stride;

  // Pass 2: allocation by allocation, every stride-th committed page (counted over the whole walk,
  // in address order) asked whether it is resident; each sample stands for `stride` pages.
  SampleBatch batch;
  AddressRegion current{};
  bool open = false;
  u64 next = 0;  // the committed-page index of the next sample
  u64 index = 0;
  auto close_allocation = [&]() {
    if (!open) return;
    flush(batch);
    const u64 estimate = batch.valid * stride * page;
    current.resident = estimate < current.committed ? estimate : current.committed;
    batch.valid = 0;
    finish_allocation(out, current);
    open = false;
  };
  for_each_region([&](const MEMORY_BASIC_INFORMATION& mbi) {
    const u64 alloc = reinterpret_cast<u64>(mbi.AllocationBase);
    if (!open || alloc != current.base) {
      close_allocation();
      current = AddressRegion{};
      current.base = alloc;
      current.kind = kind_of(mbi.Type);
      open = true;
    }
    current.reserved += mbi.RegionSize;
    if (mbi.State != MEM_COMMIT) return;
    if (current.committed == 0) current.protect = mbi.Protect;
    current.committed += mbi.RegionSize;
    const u64 pages = mbi.RegionSize / page;
    while (next < index + pages) {
      if (batch.count == SampleBatch::k_size) flush(batch);
      batch.pages[batch.count] = PSAPI_WORKING_SET_EX_INFORMATION{};
      batch.pages[batch.count].VirtualAddress =
          static_cast<u8*>(mbi.BaseAddress) + (next - index) * page;
      ++batch.count;
      ++out.sampled_pages;
      next += stride;
    }
    index += pages;
  });
  close_allocation();

  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
    out.process_commit = counters.PrivateUsage;
    out.peak_process_commit = counters.PeakPagefileUsage;
    out.working_set = counters.WorkingSetSize;
    out.peak_working_set = counters.PeakWorkingSetSize;
  }
  return true;
}

#else  // Linux: /proc/self/smaps, exact per mapping.

// "lo-hi perms offset dev inode   path"
bool parse_header(const char* line, AddressRegion& region, bool& reserved_only,
                  bool& writable_private) {
  unsigned long long lo = 0, hi = 0, offset = 0, inode = 0;
  char perms[8] = {};
  char dev[32] = {};
  int consumed = 0;
  if (std::sscanf(line, "%llx-%llx %7s %llx %31s %llu %n", &lo, &hi, perms, &offset, &dev[0],
                  &inode, &consumed) < 6)
    return false;
  const char* path = line + consumed;
  usize path_len = std::strlen(path);
  while (path_len > 0 && (path[path_len - 1] == '\n' || path[path_len - 1] == ' '))
    --path_len;
  region = AddressRegion{};
  region.base = lo;
  region.reserved = hi - lo;
  reserved_only = perms[0] == '-' && perms[1] == '-' && perms[2] == '-';
  writable_private = perms[1] == 'w' && perms[3] == 'p';
  const bool anonymous = inode == 0 || path_len == 0 || path[0] == '[';
  if (anonymous) {
    region.kind = RegionKind::private_memory;
  } else {
    // A shared object, or the executable: its mappings carry an execute bit somewhere; the file's
    // name is the cheap test, and a mapping of the same file without x (its data) is still image.
    bool image = perms[2] == 'x';
    for (usize i = 0; !image && i + 3 <= path_len; ++i)
      image = path[i] == '.' && path[i + 1] == 's' && path[i + 2] == 'o';
    region.kind = image ? RegionKind::image : RegionKind::mapped_file;
  }
  return true;
}

u64 field_kib(const char* line, const char* key) noexcept {
  const usize n = std::strlen(key);
  if (std::strncmp(line, key, n) != 0) return ~u64{0};
  unsigned long long kib = 0;
  if (std::sscanf(line + n, " %llu", &kib) != 1) return ~u64{0};
  return static_cast<u64>(kib) * 1024u;
}

bool walk(AddressSpaceReport& out) noexcept {
  std::FILE* f = std::fopen("/proc/self/smaps", "rb");
  if (f == nullptr) return false;
  out.page_bytes = static_cast<u64>(::sysconf(_SC_PAGESIZE));
  out.sample_stride_pages = 1;
  char line[4352];
  AddressRegion current{};
  bool open = false, reserved_only = false, writable_private = false;
  auto finish = [&]() {
    if (!open) return;
    if (reserved_only) {
      current.committed = 0;
      current.resident = 0;
    } else {
      current.committed = current.reserved;
      if (current.kind == RegionKind::private_memory && writable_private)
        out.process_commit += current.committed;
    }
    finish_allocation(out, current);
    open = false;
  };
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    const char c = line[0];
    const bool header = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (header) {
      AddressRegion region{};
      bool ro = false, wp = false;
      if (!parse_header(line, region, ro, wp)) continue;
      finish();
      current = region;
      reserved_only = ro;
      writable_private = wp;
      open = true;
      continue;
    }
    if (!open) continue;
    const u64 rss = field_kib(line, "Rss:");
    if (rss != ~u64{0}) {
      current.resident = rss;
      out.sampled_pages += out.page_bytes != 0 ? rss / out.page_bytes : 0;
    }
  }
  finish();
  std::fclose(f);

  std::FILE* s = std::fopen("/proc/self/status", "rb");
  if (s != nullptr) {
    while (std::fgets(line, sizeof(line), s) != nullptr) {
      u64 v = field_kib(line, "VmRSS:");
      if (v != ~u64{0}) out.working_set = v;
      v = field_kib(line, "VmHWM:");
      if (v != ~u64{0}) out.peak_working_set = v;
    }
    std::fclose(s);
  }
  return true;
}

#endif

}  // namespace

bool walk_address_space(AddressSpaceReport& out) noexcept {
  out = AddressSpaceReport{};
  out.valid = walk(out);
  return out.valid;
}

u64 resident_estimate(const void* p, u64 bytes, u32 max_samples) noexcept {
  if (p == nullptr || bytes == 0 || max_samples == 0) return 0;
#if ENGINE_PLATFORM_WINDOWS
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  const u64 page = si.dwPageSize;
#else
  const u64 page = static_cast<u64>(::sysconf(_SC_PAGESIZE));
#endif
  const u64 first = reinterpret_cast<u64>(p) & ~(page - 1);
  const u64 pages = (reinterpret_cast<u64>(p) + bytes - first + page - 1) / page;
  const u64 stride = pages <= max_samples ? 1 : (pages + max_samples - 1) / max_samples;
  u64 resident_pages = 0;
  u64 sampled = 0;
#if ENGINE_PLATFORM_WINDOWS
  PSAPI_WORKING_SET_EX_INFORMATION batch[256];
  u32 n = 0;
  auto flush_batch = [&]() {
    if (n == 0) return;
    if (::QueryWorkingSetEx(::GetCurrentProcess(), batch,
                            static_cast<DWORD>(sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * n))) {
      for (u32 i = 0; i < n; ++i)
        resident_pages += batch[i].VirtualAttributes.Valid ? 1u : 0u;
    }
    n = 0;
  };
  for (u64 i = 0; i < pages; i += stride) {
    if (n == 256) flush_batch();
    batch[n] = PSAPI_WORKING_SET_EX_INFORMATION{};
    batch[n].VirtualAddress = reinterpret_cast<void*>(first + i * page);
    ++n;
    ++sampled;
  }
  flush_batch();
#else
  for (u64 i = 0; i < pages; i += stride) {
    unsigned char vec = 0;
    if (::mincore(reinterpret_cast<void*>(first + i * page), page, &vec) == 0 && (vec & 1u) != 0)
      ++resident_pages;
    ++sampled;
  }
#endif
  if (sampled == 0) return 0;
  const u64 estimate = resident_pages * pages / sampled * page;
  return estimate < bytes ? estimate : bytes;
}

bool heap_totals(HeapTotals& out) noexcept {
  out = HeapTotals{};
#if ENGINE_PLATFORM_WINDOWS
  HANDLE heaps[256];
  const DWORD count = ::GetProcessHeaps(256, heaps);
  const DWORD n = count < 256 ? count : 256;
  for (DWORD i = 0; i < n; ++i) {
    HEAP_SUMMARY summary{};
    summary.cb = sizeof(summary);
    if (!::HeapSummary(heaps[i], 0, &summary)) continue;
    out.committed += summary.cbCommitted;
    out.allocated += summary.cbAllocated;
    ++out.heaps;
  }
  out.valid = out.heaps > 0;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  const struct mallinfo2 info = ::mallinfo2();
  out.committed = static_cast<u64>(info.arena) + static_cast<u64>(info.hblkhd);
  out.allocated = static_cast<u64>(info.uordblks) + static_cast<u64>(info.hblkhd);
  out.heaps = 1;
  out.valid = true;
#endif
  return out.valid;
}

}  // namespace engine::platform
