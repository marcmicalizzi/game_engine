# platform (core)

**Purpose.** Facts about the machine and the OS that everything else builds on: processor topology (logical CPUs, cores and SMT siblings, packages, NUMA nodes, cache hierarchy, cache domains, efficiency classes), CPUID instruction-set features with OS support taken into account, and thread pinning, priority, naming, and identification. This is Stage 1 of hardware characterization (ADR-0011): detection, no benchmarking.

**Owned data.** The cached `Topology` and `CpuFeatures`, detected once per process.

**Invariants.**
- Engine CPU ids are dense `0..n-1`; `LogicalCpu` keeps OS coordinates (`os_group`, `os_index`) for pinning, and `Topology::find` maps back.
- Every CPU belongs to exactly one cache domain; domains are disjoint and cover all CPUs.
- `performance_cpus` and `efficiency_cpus` partition the CPU set; `performance_cpus` is never empty. On homogeneous parts `efficiency_cpus` is empty.
- `CpuFeatures::avx2 == true` means the instructions can execute (CPUID bit and XCR0 state both checked), likewise for AVX-512.

**Public API.**
- `core/platform/topology.h`: `CpuSet`, `CacheInfo`, `LogicalCpu`, `CacheDomain`, `Topology`, `topology()`, `detect_topology()`, `describe_topology()`.
- `core/platform/cpu_features.h`: `CpuFeatures`, `cpu_features()`, `detect_cpu_features()`.
- `core/platform/thread.h`: `pin_current_thread`, `set_current_thread_priority`, `set_current_thread_name`, `current_cpu`, `current_thread_index`, `yield_thread`, `pause_cpu`, `sleep_ms`.

**Detection sources.**
- Windows: `GetLogicalProcessorInformationEx(RelationAll)`. Processor groups define the numbering; core records supply SMT and `EfficiencyClass`; cache records supply levels, sizes, line size, and sharing; NUMA and package records supply the rest. Cache domain = the set of CPUs sharing the highest-level non-instruction cache.
- Linux: sysfs (`topology/`, `cache/index*/`, `node*/cpulist`). Efficiency class is a heuristic (max frequency at least 15% below the fastest CPU). Compiled and reviewed but not yet exercised on hardware in this repository; verify on the first Linux build.
- CPUID leaves 0, 1, 7 (subleaves 0 and 1), and 0x80000002–4; XCR0 via `xgetbv`.

**Verified on.** Intel i9-10980XE (36 logical CPUs, 18 cores with SMT, one cache domain, one class). The i9-13980HX laptop (8 P + 16 E cores, hybrid) is the next target and will exercise `efficiency_cpus`.

**Depends on.** `base`, `memory`, `containers`.

**Testing.** `tools/dev.ps1 test -Filter platform`. Tests check the invariants above on whatever machine runs them, that repeated detection is stable, that pinning moves a thread onto the requested CPU, and that CPUID implications hold.

**Performance notes.** Cold data. `describe_topology` prints a summary for logs; the job system consumes the structure directly.
