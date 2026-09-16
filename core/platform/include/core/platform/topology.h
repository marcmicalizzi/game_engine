#pragma once

// Processor topology as reported by the operating system: logical CPUs, physical cores and
// SMT siblings, packages, NUMA nodes, cache hierarchy, cache domains (the set of CPUs that
// share a last-level cache, i.e. a CCD on AMD or a die/tile on Intel), and efficiency classes
// (performance vs efficiency cores on hybrid parts).
//
// The engine numbers logical CPUs itself, 0..n-1, independent of the OS's group/index scheme;
// LogicalCpu keeps the OS coordinates for pinning. Detection runs once and is cached.

#include <core/base/types.h>
#include <core/containers/small_vector.h>

namespace engine::platform {

inline constexpr u16 k_invalid_cpu = 0xFFFF;

// Variable-length bit set over engine CPU ids. No hidden limit on CPU count.
class CpuSet {
 public:
  void set(u16 cpu);
  void clear(u16 cpu);
  bool test(u16 cpu) const noexcept;
  u16 count() const noexcept;
  bool empty() const noexcept { return count() == 0; }
  u16 first() const noexcept;  // k_invalid_cpu when empty
  template <class F>
  void for_each(F&& f) const {
    for (u32 w = 0; w < words_.size(); ++w) {
      u64 bits = words_[w];
      while (bits != 0) {
        const u32 bit = static_cast<u32>(ctz64(bits));
        f(static_cast<u16>(w * 64 + bit));
        bits &= bits - 1;
      }
    }
  }
  bool operator==(const CpuSet&) const noexcept;

 private:
  static u32 ctz64(u64 v) noexcept;
  SmallVector<u64, 2> words_;
};

enum class CacheType : u8 { Unified, Data, Instruction, Trace };

struct CacheInfo {
  u8 level = 0;  // 1, 2, 3
  CacheType type = CacheType::Unified;
  u32 size_bytes = 0;
  u16 line_bytes = 0;
  u16 associativity = 0;  // 0 when unknown, 0xFFFF when fully associative
  CpuSet cpus;            // logical CPUs sharing this cache
};

struct LogicalCpu {
  u16 id = k_invalid_cpu;  // engine index
  u16 os_group = 0;        // Windows processor group; 0 on Linux
  u16 os_index = 0;        // index within the group (Windows) or the kernel CPU number (Linux)
  u16 core_id = 0;         // physical core, engine numbering
  u16 package_id = 0;
  u16 numa_node = 0;
  u16 cache_domain = 0;     // index into Topology::cache_domains
  u8 smt_index = 0;         // 0 for the first logical CPU of a core, 1 for its sibling
  u8 efficiency_class = 0;  // higher is faster; the maximum class is "performance"
  u32 l1d_bytes = 0;
  u32 l2_bytes = 0;
};

struct CacheDomain {
  u16 id = 0;
  u16 numa_node = 0;
  u32 llc_bytes = 0;  // last-level cache shared by these CPUs
  CpuSet cpus;
};

struct Topology {
  SmallVector<LogicalCpu, 64> cpus;
  SmallVector<CacheDomain, 8> cache_domains;
  SmallVector<CacheInfo, 16> caches;  // distinct caches; every level and instance
  u16 core_count = 0;
  u16 package_count = 0;
  u16 numa_node_count = 0;
  u8 efficiency_class_count = 1;
  u16 cache_line_bytes = 64;
  bool smt = false;
  CpuSet performance_cpus;  // highest efficiency class
  CpuSet efficiency_cpus;   // every other class; empty on homogeneous parts

  u16 cpu_count() const noexcept { return static_cast<u16>(cpus.size()); }
  const LogicalCpu* find(u16 os_group, u16 os_index) const noexcept;
};

// Detected once on first use and cached for the process lifetime.
const Topology& topology();

// Fresh detection; use for tests and diagnostics.
Topology detect_topology();

// Formats a compact human-readable summary (for logs and the profile tool). Writes at most
// `capacity - 1` characters and terminates; returns the number written.
usize describe_topology(const Topology& topo, char* out, usize capacity);

}  // namespace engine::platform
