#pragma once

// **Where this process's memory is** (docs/subsystems/platform.md, "Where the commit is"; roadmap
// T129): the address space walked region by region — `VirtualQuery` on Windows,
// `/proc/self/smaps` on Linux — and summed by kind (private, mapped file, image) and by state
// (committed, reserved only), with the largest private allocations listed and, beside every
// figure, how much of it is resident: what the process has touched and still holds.
//
// **Why it exists.** The task manager's two numbers, the commit charge and the working set, said
// in November 2026 that a forest walk committed 79 GB and touched 22: about 57 GB promised by the
// OS and never used, on a machine whose commit limit is 118 GB. Neither number says who holds the
// difference. This walk says what kind of memory it is and where the largest pieces sit, so they
// can be matched to the owner that sized them (the engine's heap, the GPU driver, a mapped file).
//
// **Touched, against committed.** Windows gives a page's residency through `QueryWorkingSetEx`,
// one entry a page; a walk of an 80 GB process would ask about twenty million pages, so the walk
// samples: every `sample_stride_pages`-th committed page in address order, at most
// `k_address_space_samples` of them, and each sampled page stands for the stride after it. A large
// allocation is sampled thousands of times and its estimate is good to a fraction of a percent; an
// allocation smaller than the stride may be missed entirely, which moves the kind's total by less
// than one stride's worth per allocation. Linux gives the exact figure (`Rss:`) per mapping, and
// the stride there is 1.
//
// **Cost.** A cold call: tens to a few hundred milliseconds on a process of tens of gigabytes, a
// system call per region and a `QueryWorkingSetEx` per thousand samples. It allocates nothing (its
// buffers are on the stack), so a host may call it once at its first frame and once at exit with
// the frame loop's allocation count unchanged. **It never runs unless a host asks** (engine-view's
// `--memory-report`): there is no counter, hook or thread behind it.

#include <core/base/macros.h>
#include <core/base/types.h>

namespace engine::platform {

enum class RegionKind : u8 {
  private_memory = 0,  // VirtualAlloc, the heaps, thread stacks, the driver's own allocations
  mapped_file = 1,     // a view of a file or of a section (MEM_MAPPED; a file mapping on Linux)
  image = 2,           // an executable or a library's sections (MEM_IMAGE; a mapped ELF object)
};
inline constexpr u32 k_region_kinds = 3;
const char* region_kind_name(RegionKind kind) noexcept;

// One allocation: on Windows every region sharing an `AllocationBase` (one VirtualAlloc, one view),
// on Linux one mapping.
struct AddressRegion {
  u64 base = 0;
  u64 reserved = 0;   // the address space it spans, committed or not
  u64 committed = 0;  // of which committed: what the OS has promised and charged to the process
  u64 resident = 0;   // of which resident (estimated from the samples on Windows; exact on Linux)
  RegionKind kind = RegionKind::private_memory;
  // Windows: the protection of its first committed region (`PAGE_*`, write-combined and no-cache
  // bits included), which tells the driver's mapped memory from a heap's; 0 on Linux.
  u32 protect = 0;
};

// Private allocations by committed size: under 1 MiB, then 1, 4, 16, 64, 256 MiB and 1 GiB up.
inline constexpr u32 k_address_space_size_buckets = 7;
inline constexpr u32 k_address_space_protections = 8;
struct SizeBucket {
  u64 committed = 0;
  u64 resident = 0;
  u32 allocations = 0;
};

struct RegionKindTotals {
  u64 committed = 0;
  u64 resident = 0;
  u64 reserved_only = 0;  // address space reserved and not committed: costs no commit
  u32 allocations = 0;
};

inline constexpr u32 k_address_space_largest = 24;
inline constexpr u64 k_address_space_samples = u64{1} << 16;

struct AddressSpaceReport {
  bool valid = false;
  RegionKindTotals kinds[k_region_kinds];
  // The OS's own figures for the process, read beside the walk: the commit charge
  // (`PrivateUsage`; on Linux the private writable mappings, which is what the kernel's overcommit
  // accounting charges) and its peak (Windows only; 0 on Linux), the working set and its peak.
  u64 process_commit = 0;
  u64 peak_process_commit = 0;
  u64 working_set = 0;
  u64 peak_working_set = 0;
  u64 page_bytes = 0;
  u64 sample_stride_pages = 0;  // 1: every committed page was looked at
  u64 sampled_pages = 0;
  u32 allocations = 0;
  // The largest private allocations by committed bytes, largest first, and the private allocations
  // with the most committed and not resident, most first.
  u32 largest_count = 0;
  AddressRegion largest[k_address_space_largest];
  u32 untouched_count = 0;
  AddressRegion untouched[k_address_space_largest];
  SizeBucket private_sizes[k_address_space_size_buckets];
  // Private allocations by the protection of their first committed region (Windows; one entry with
  // protect 0 on Linux), the first `k_address_space_protections` distinct values met; the rest are
  // summed into the last entry, whose protect then reads 0xffffffff.
  u32 protection_count = 0;
  u32 protections[k_address_space_protections] = {};
  SizeBucket private_protections[k_address_space_protections];
};

// Walks this process's address space into `out`. False with `out.valid` false where the platform
// cannot say (no smaps, a query refused). Any thread; the process keeps running meanwhile, so the
// totals are a picture of a moving thing, good to what changed during the walk.
bool walk_address_space(AddressSpaceReport& out) noexcept;

// How much of `[p, p + bytes)` is resident, estimated from at most `max_samples` pages spread
// evenly over it (`QueryWorkingSetEx` on Windows, `mincore` on Linux): what the memory report asks
// of each block the large-allocation log holds, to say whether its owner ever touched it.
u64 resident_estimate(const void* p, u64 bytes, u32 max_samples) noexcept;

// The C runtime's heaps as their own allocator counts them: what the heaps have committed from the
// OS and how much of it is handed out (`HeapSummary` over every heap of the process on Windows;
// glibc's `mallinfo2` on Linux, where `committed` is the arena plus the mmapped blocks). Every
// `malloc`, the engine's `mem::allocate` (whose backend is the C runtime's aligned heap), the C++
// runtime and any library that uses them land here; the GPU driver's mapped memory does not.
struct HeapTotals {
  bool valid = false;
  u64 committed = 0;
  u64 allocated = 0;
  u32 heaps = 0;
};
bool heap_totals(HeapTotals& out) noexcept;

}  // namespace engine::platform
