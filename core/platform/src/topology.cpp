#include <core/platform/topology.h>

#include <core/base/assert.h>
#include <core/memory/memory.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif ENGINE_PLATFORM_LINUX
#include <dirent.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace engine::platform {

// --- CpuSet ---------------------------------------------------------------------------------

u32 CpuSet::ctz64(u64 v) noexcept { return static_cast<u32>(std::countr_zero(v)); }

void CpuSet::set(u16 cpu) {
  const u32 w = cpu / 64;
  while (words_.size() <= w) words_.push_back(0);
  words_[w] |= (u64{1} << (cpu % 64));
}

void CpuSet::clear(u16 cpu) {
  const u32 w = cpu / 64;
  if (w < words_.size()) words_[w] &= ~(u64{1} << (cpu % 64));
}

bool CpuSet::test(u16 cpu) const noexcept {
  const u32 w = cpu / 64;
  return w < words_.size() && (words_[w] & (u64{1} << (cpu % 64))) != 0;
}

u16 CpuSet::count() const noexcept {
  u32 n = 0;
  for (u64 w : words_) n += static_cast<u32>(std::popcount(w));
  return static_cast<u16>(n);
}

u16 CpuSet::first() const noexcept {
  for (u32 w = 0; w < words_.size(); ++w) {
    if (words_[w] != 0) return static_cast<u16>(w * 64 + std::countr_zero(words_[w]));
  }
  return k_invalid_cpu;
}

bool CpuSet::operator==(const CpuSet& o) const noexcept {
  const u32 n = words_.size() > o.words_.size() ? words_.size() : o.words_.size();
  for (u32 w = 0; w < n; ++w) {
    const u64 a = w < words_.size() ? words_[w] : 0;
    const u64 b = w < o.words_.size() ? o.words_[w] : 0;
    if (a != b) return false;
  }
  return true;
}

// --- Topology helpers -----------------------------------------------------------------------

const LogicalCpu* Topology::find(u16 os_group, u16 os_index) const noexcept {
  for (const LogicalCpu& c : cpus) {
    if (c.os_group == os_group && c.os_index == os_index) return &c;
  }
  return nullptr;
}

namespace {

// Derives cache domains, per-CPU cache sizes, class sets, and counts from cpus + caches.
void finalize(Topology& t) {
  // Last-level cache per CPU defines its cache domain.
  u8 max_level = 0;
  for (const CacheInfo& c : t.caches) {
    if (c.type != CacheType::Instruction && c.level > max_level) max_level = c.level;
  }
  t.cache_domains.clear();
  for (LogicalCpu& cpu : t.cpus) {
    cpu.cache_domain = 0xFFFF;
    for (const CacheInfo& c : t.caches) {
      if (!c.cpus.test(cpu.id)) continue;
      if (c.level == 1 && (c.type == CacheType::Data || c.type == CacheType::Unified)) cpu.l1d_bytes = c.size_bytes;
      if (c.level == 2 && c.type != CacheType::Instruction) cpu.l2_bytes = c.size_bytes;
      if (c.type != CacheType::Instruction && c.level == max_level) {
        // Find or create the domain for this cache.
        for (u32 d = 0; d < t.cache_domains.size(); ++d) {
          if (t.cache_domains[d].cpus == c.cpus) {
            cpu.cache_domain = static_cast<u16>(d);
            break;
          }
        }
        if (cpu.cache_domain == 0xFFFF) {
          CacheDomain dom;
          dom.id = static_cast<u16>(t.cache_domains.size());
          dom.numa_node = cpu.numa_node;
          dom.llc_bytes = c.size_bytes;
          dom.cpus = c.cpus;
          t.cache_domains.push_back(dom);
          cpu.cache_domain = dom.id;
        }
      }
    }
  }
  // CPUs without any reported shared cache all go into one domain.
  bool need_fallback = false;
  for (const LogicalCpu& cpu : t.cpus) {
    if (cpu.cache_domain == 0xFFFF) need_fallback = true;
  }
  if (need_fallback) {
    CacheDomain dom;
    dom.id = static_cast<u16>(t.cache_domains.size());
    for (LogicalCpu& cpu : t.cpus) {
      if (cpu.cache_domain == 0xFFFF) {
        cpu.cache_domain = dom.id;
        dom.cpus.set(cpu.id);
      }
    }
    t.cache_domains.push_back(dom);
  }

  // Efficiency classes.
  u8 max_class = 0;
  u8 min_class = 0xFF;
  for (const LogicalCpu& cpu : t.cpus) {
    if (cpu.efficiency_class > max_class) max_class = cpu.efficiency_class;
    if (cpu.efficiency_class < min_class) min_class = cpu.efficiency_class;
  }
  t.efficiency_class_count = static_cast<u8>(max_class - min_class + 1);
  t.performance_cpus = CpuSet{};
  t.efficiency_cpus = CpuSet{};
  for (const LogicalCpu& cpu : t.cpus) {
    if (cpu.efficiency_class == max_class) {
      t.performance_cpus.set(cpu.id);
    } else {
      t.efficiency_cpus.set(cpu.id);
    }
  }

  // Counts.
  u16 max_core = 0, max_pkg = 0, max_node = 0;
  t.smt = false;
  for (const LogicalCpu& cpu : t.cpus) {
    if (cpu.core_id > max_core) max_core = cpu.core_id;
    if (cpu.package_id > max_pkg) max_pkg = cpu.package_id;
    if (cpu.numa_node > max_node) max_node = cpu.numa_node;
    if (cpu.smt_index > 0) t.smt = true;
  }
  t.core_count = t.cpus.empty() ? 0 : static_cast<u16>(max_core + 1);
  t.package_count = t.cpus.empty() ? 0 : static_cast<u16>(max_pkg + 1);
  t.numa_node_count = t.cpus.empty() ? 0 : static_cast<u16>(max_node + 1);

  t.cache_line_bytes = 64;
  for (const CacheInfo& c : t.caches) {
    if (c.level == 1 && c.type != CacheType::Instruction && c.line_bytes != 0) {
      t.cache_line_bytes = c.line_bytes;
      break;
    }
  }
}

// A single-CPU topology for environments where detection fails outright.
Topology fallback_topology() {
  Topology t;
  LogicalCpu cpu;
  cpu.id = 0;
  t.cpus.push_back(cpu);
  finalize(t);
  return t;
}

#if ENGINE_PLATFORM_WINDOWS

struct GroupMap {
  // (group, index) -> engine id
  SmallVector<u16, 4> group_base;  // engine id of index 0 in each group
  SmallVector<u64, 4> group_mask;

  u16 id_of(WORD group, u32 index) const noexcept {
    if (group >= group_base.size()) return k_invalid_cpu;
    if ((group_mask[group] & (u64{1} << index)) == 0) return k_invalid_cpu;
    // Engine ids are assigned in ascending bit order within each group.
    const u64 below = group_mask[group] & ((u64{1} << index) - 1);
    return static_cast<u16>(group_base[group] + std::popcount(below));
  }
  template <class F>
  void for_each_in(const GROUP_AFFINITY& ga, F&& f) const {
    u64 bits = static_cast<u64>(ga.Mask);
    while (bits != 0) {
      const u32 bit = static_cast<u32>(std::countr_zero(bits));
      const u16 id = id_of(ga.Group, bit);
      if (id != k_invalid_cpu) f(id);
      bits &= bits - 1;
    }
  }
};

Topology detect_windows() {
  DWORD length = 0;
  ::GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
  if (length == 0) return fallback_topology();
  auto* buffer = static_cast<std::byte*>(mem::allocate(length, 16));
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer);
  if (!::GetLogicalProcessorInformationEx(RelationAll, info, &length)) {
    mem::deallocate(buffer, length, 16);
    return fallback_topology();
  }

  Topology t;
  GroupMap gm;

  // Pass 1: groups define the CPU numbering.
  for (const std::byte* p = buffer; p < buffer + length;) {
    const auto* rec = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
    if (rec->Relationship == RelationGroup) {
      for (WORD g = 0; g < rec->Group.ActiveGroupCount; ++g) {
        const u64 mask = static_cast<u64>(rec->Group.GroupInfo[g].ActiveProcessorMask);
        gm.group_base.push_back(static_cast<u16>(t.cpus.size()));
        gm.group_mask.push_back(mask);
        u64 bits = mask;
        while (bits != 0) {
          const u32 bit = static_cast<u32>(std::countr_zero(bits));
          LogicalCpu cpu;
          cpu.id = static_cast<u16>(t.cpus.size());
          cpu.os_group = g;
          cpu.os_index = static_cast<u16>(bit);
          t.cpus.push_back(cpu);
          bits &= bits - 1;
        }
      }
    }
    p += rec->Size;
  }
  if (t.cpus.empty()) {
    mem::deallocate(buffer, length, 16);
    return fallback_topology();
  }

  // Pass 2: cores, packages, NUMA nodes, caches.
  u16 next_core = 0;
  u16 next_package = 0;
  for (const std::byte* p = buffer; p < buffer + length;) {
    const auto* rec = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
    switch (rec->Relationship) {
      case RelationProcessorCore: {
        const u16 core = next_core++;
        u8 smt = 0;
        for (WORD i = 0; i < rec->Processor.GroupCount; ++i) {
          gm.for_each_in(rec->Processor.GroupMask[i], [&](u16 id) {
            t.cpus[id].core_id = core;
            t.cpus[id].smt_index = smt++;
            t.cpus[id].efficiency_class = rec->Processor.EfficiencyClass;
          });
        }
        break;
      }
      case RelationProcessorPackage: {
        const u16 pkg = next_package++;
        for (WORD i = 0; i < rec->Processor.GroupCount; ++i) {
          gm.for_each_in(rec->Processor.GroupMask[i], [&](u16 id) { t.cpus[id].package_id = pkg; });
        }
        break;
      }
      case RelationNumaNode:
      case RelationNumaNodeEx: {
        const u16 node = static_cast<u16>(rec->NumaNode.NodeNumber);
        const WORD count = rec->NumaNode.GroupCount == 0 ? 1 : rec->NumaNode.GroupCount;
        for (WORD i = 0; i < count; ++i) {
          const GROUP_AFFINITY& ga = rec->NumaNode.GroupCount == 0 ? rec->NumaNode.GroupMask
                                                                    : rec->NumaNode.GroupMasks[i];
          gm.for_each_in(ga, [&](u16 id) { t.cpus[id].numa_node = node; });
        }
        break;
      }
      case RelationCache: {
        CacheInfo c;
        c.level = rec->Cache.Level;
        c.size_bytes = rec->Cache.CacheSize;
        c.line_bytes = rec->Cache.LineSize;
        c.associativity = rec->Cache.Associativity == CACHE_FULLY_ASSOCIATIVE ? 0xFFFF : rec->Cache.Associativity;
        switch (rec->Cache.Type) {
          case CacheUnified: c.type = CacheType::Unified; break;
          case CacheInstruction: c.type = CacheType::Instruction; break;
          case CacheData: c.type = CacheType::Data; break;
          default: c.type = CacheType::Trace; break;
        }
        const WORD count = rec->Cache.GroupCount == 0 ? 1 : rec->Cache.GroupCount;
        for (WORD i = 0; i < count; ++i) {
          const GROUP_AFFINITY& ga = rec->Cache.GroupCount == 0 ? rec->Cache.GroupMask : rec->Cache.GroupMasks[i];
          gm.for_each_in(ga, [&](u16 id) { c.cpus.set(id); });
        }
        if (!c.cpus.empty()) t.caches.push_back(c);
        break;
      }
      default:
        break;
    }
    p += rec->Size;
  }
  mem::deallocate(buffer, length, 16);
  finalize(t);
  return t;
}

#elif ENGINE_PLATFORM_LINUX

// Best-effort sysfs parsing. Compiled and reviewed but not yet exercised on hardware in this
// repository; see docs/subsystems/platform.md.

bool read_file(const char* path, char* out, usize capacity) {
  FILE* f = std::fopen(path, "r");
  if (f == nullptr) return false;
  const usize n = std::fread(out, 1, capacity - 1, f);
  std::fclose(f);
  out[n] = '\0';
  return n > 0;
}

long read_long(const char* path, long fallback) {
  char buf[64];
  if (!read_file(path, buf, sizeof(buf))) return fallback;
  return std::strtol(buf, nullptr, 0);
}

// Parses "0-3,8,10-11" into a set of kernel CPU numbers, mapped through `id_of`.
template <class IdOf>
void parse_cpu_list(const char* text, CpuSet& out, IdOf&& id_of) {
  const char* p = text;
  while (*p != '\0' && *p != '\n') {
    char* end = nullptr;
    const long a = std::strtol(p, &end, 10);
    if (end == p) break;
    long b = a;
    p = end;
    if (*p == '-') {
      b = std::strtol(p + 1, &end, 10);
      p = end;
    }
    for (long k = a; k <= b; ++k) {
      const u16 id = id_of(static_cast<u16>(k));
      if (id != k_invalid_cpu) out.set(id);
    }
    if (*p == ',') ++p;
  }
}

Topology detect_linux() {
  Topology t;
  char buf[4096];
  if (!read_file("/sys/devices/system/cpu/online", buf, sizeof(buf))) return fallback_topology();

  // Kernel CPU number -> engine id, assigned in ascending order.
  SmallVector<u16, 64> kernel_to_id;
  {
    CpuSet online;
    parse_cpu_list(buf, online, [](u16 k) { return k; });  // identity for now
    online.for_each([&](u16 k) {
      while (kernel_to_id.size() <= k) kernel_to_id.push_back(k_invalid_cpu);
      kernel_to_id[k] = static_cast<u16>(t.cpus.size());
      LogicalCpu cpu;
      cpu.id = static_cast<u16>(t.cpus.size());
      cpu.os_index = k;
      t.cpus.push_back(cpu);
    });
  }
  auto id_of = [&](u16 k) { return k < kernel_to_id.size() ? kernel_to_id[k] : k_invalid_cpu; };

  // Cores, packages, SMT.
  SmallVector<long, 64> core_keys;  // (package << 16 | core_id) per engine cpu, for renumbering
  for (LogicalCpu& cpu : t.cpus) {
    char path[256];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/core_id", cpu.os_index);
    const long core = read_long(path, cpu.os_index);
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", cpu.os_index);
    const long pkg = read_long(path, 0);
    cpu.package_id = static_cast<u16>(pkg);
    core_keys.push_back((pkg << 16) | core);
  }
  // Renumber cores densely and assign SMT indices by ascending kernel number.
  SmallVector<long, 64> seen;
  for (u32 i = 0; i < t.cpus.size(); ++i) {
    u16 core_index = k_invalid_cpu;
    for (u32 s = 0; s < seen.size(); ++s) {
      if (seen[s] == core_keys[i]) {
        core_index = static_cast<u16>(s);
        break;
      }
    }
    if (core_index == k_invalid_cpu) {
      core_index = static_cast<u16>(seen.size());
      seen.push_back(core_keys[i]);
    }
    t.cpus[i].core_id = core_index;
    u8 smt = 0;
    for (u32 j = 0; j < i; ++j) {
      if (t.cpus[j].core_id == core_index) ++smt;
    }
    t.cpus[i].smt_index = smt;
  }

  // NUMA nodes.
  for (u16 node = 0; node < 1024; ++node) {
    char path[256];
    std::snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", node);
    if (!read_file(path, buf, sizeof(buf))) {
      if (node == 0) break;
      break;
    }
    CpuSet cpus;
    parse_cpu_list(buf, cpus, id_of);
    cpus.for_each([&](u16 id) { t.cpus[id].numa_node = node; });
  }

  // Caches: enumerate each cpu's cache/index* and deduplicate by shared_cpu_list + level + type.
  for (const LogicalCpu& cpu : t.cpus) {
    for (u32 idx = 0; idx < 8; ++idx) {
      char path[256];
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/level", cpu.os_index, idx);
      const long level = read_long(path, -1);
      if (level < 0) break;
      CacheInfo c;
      c.level = static_cast<u8>(level);
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/type", cpu.os_index, idx);
      if (read_file(path, buf, sizeof(buf))) {
        if (std::strncmp(buf, "Data", 4) == 0) c.type = CacheType::Data;
        else if (std::strncmp(buf, "Instruction", 11) == 0) c.type = CacheType::Instruction;
        else c.type = CacheType::Unified;
      }
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/size", cpu.os_index, idx);
      if (read_file(path, buf, sizeof(buf))) {
        char* end = nullptr;
        long size = std::strtol(buf, &end, 10);
        if (end != nullptr && (*end == 'K' || *end == 'k')) size *= 1024;
        if (end != nullptr && (*end == 'M' || *end == 'm')) size *= 1024 * 1024;
        c.size_bytes = static_cast<u32>(size);
      }
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/coherency_line_size", cpu.os_index, idx);
      c.line_bytes = static_cast<u16>(read_long(path, 64));
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/ways_of_associativity", cpu.os_index, idx);
      c.associativity = static_cast<u16>(read_long(path, 0));
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/shared_cpu_list", cpu.os_index, idx);
      if (read_file(path, buf, sizeof(buf))) parse_cpu_list(buf, c.cpus, id_of);
      if (c.cpus.empty()) c.cpus.set(cpu.id);
      bool duplicate = false;
      for (const CacheInfo& existing : t.caches) {
        if (existing.level == c.level && existing.type == c.type && existing.cpus == c.cpus) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) t.caches.push_back(c);
    }
  }

  // Efficiency class heuristic: on Intel hybrid parts efficiency cores share an L2 among a
  // cluster of four single-threaded cores, and report a lower maximum frequency. Class 1 =
  // performance, 0 = efficiency. Homogeneous parts end up all class 1.
  long max_freq = 0;
  SmallVector<long, 64> freqs;
  for (const LogicalCpu& cpu : t.cpus) {
    char path[256];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu.os_index);
    const long f = read_long(path, 0);
    freqs.push_back(f);
    if (f > max_freq) max_freq = f;
  }
  for (u32 i = 0; i < t.cpus.size(); ++i) {
    const bool slower = max_freq > 0 && freqs[i] > 0 && freqs[i] * 100 < max_freq * 85;
    t.cpus[i].efficiency_class = slower ? 0 : 1;
  }
  finalize(t);
  return t;
}

#endif

}  // namespace

Topology detect_topology() {
#if ENGINE_PLATFORM_WINDOWS
  return detect_windows();
#elif ENGINE_PLATFORM_LINUX
  return detect_linux();
#else
  return fallback_topology();
#endif
}

const Topology& topology() {
  static const Topology t = detect_topology();
  return t;
}

usize describe_topology(const Topology& topo, char* out, usize capacity) {
  if (capacity == 0) return 0;
  int n = std::snprintf(out, capacity,
                        "%u logical CPUs, %u cores%s, %u package(s), %u NUMA node(s), %u cache domain(s), "
                        "%u efficiency class(es): %u performance / %u efficiency CPUs, %u-byte lines",
                        topo.cpu_count(), topo.core_count, topo.smt ? " (SMT)" : "", topo.package_count,
                        topo.numa_node_count, static_cast<u32>(topo.cache_domains.size()),
                        topo.efficiency_class_count, topo.performance_cpus.count(),
                        topo.efficiency_cpus.count(), topo.cache_line_bytes);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  usize written = static_cast<usize>(n) < capacity ? static_cast<usize>(n) : capacity - 1;
  for (const CacheDomain& d : topo.cache_domains) {
    if (written + 1 >= capacity) break;
    n = std::snprintf(out + written, capacity - written, "\n  domain %u: %u CPUs, LLC %u KB, NUMA %u", d.id,
                      d.cpus.count(), d.llc_bytes / 1024, d.numa_node);
    if (n < 0) break;
    written += static_cast<usize>(n) < capacity - written ? static_cast<usize>(n) : capacity - written - 1;
  }
  return written;
}

}  // namespace engine::platform
