# platform (core)

**Purpose.** Facts about the machine and the OS that everything else builds on: processor topology (logical CPUs, cores and SMT siblings, packages, NUMA nodes, cache hierarchy, cache domains, efficiency classes), CPUID instruction-set features with OS support taken into account, the startup check that refuses a machine older than the build ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md)), and thread pinning, priority, naming, and identification. This is Stage 1 of hardware characterization (ADR-0011): detection, no benchmarking.

**Owned data.** The cached `Topology` and `CpuFeatures`, detected once per process.

**Invariants.**
- Engine CPU ids are dense `0..n-1`; `LogicalCpu` keeps OS coordinates (`os_group`, `os_index`) for pinning, and `Topology::find` maps back.
- Every CPU belongs to exactly one cache domain; domains are disjoint and cover all CPUs.
- `performance_cpus` and `efficiency_cpus` partition the CPU set; `performance_cpus` is never empty. On homogeneous parts `efficiency_cpus` is empty.
- `CpuFeatures::avx2 == true` means the instructions can execute (CPUID bit and XCR0 state both checked), likewise for AVX-512.
- No translation unit of this module is compiled with the build's arch flag, whatever `ENGINE_CPU_BASELINE` says (below).
- `missing_baseline_features` never writes past the buffer it is given and always terminates it; the count it returns is the full count, even when the list was truncated.

**Public API.**
- `core/platform/topology.h`: `CpuSet`, `CacheInfo`, `LogicalCpu`, `CacheDomain`, `Topology`, `topology()`, `detect_topology()`, `describe_topology()`.
- `core/platform/cpu_features.h`: `CpuFeatures`, `cpu_features()`, `detect_cpu_features()`.
- `core/platform/cpu_baseline.h`: `CpuBaseline`, `k_build_cpu_baseline`, `k_exit_cpu_too_old`, `cpu_baseline_name`, `missing_baseline_features`, `report_cpu_baseline`, `require_cpu_baseline`.
- `core/platform/thread.h`: `pin_current_thread`, `set_current_thread_priority`, `set_current_thread_name`, `current_cpu`, `current_thread_index`, `yield_thread`, `pause_cpu`, `sleep_ms`.
- `core/platform/spin_lock.h`: `SpinLock`, a constant-initializable test-and-test-and-set lock for registries touched during static initialization and for very short critical sections.
- `core/platform/process.h`: `Process` (spawn with piped stdin/stdout, `write`, `read_line`, `read_all`, `wait`, `kill`) and `executable_directory()`.

**Detection sources.**
- Windows: `GetLogicalProcessorInformationEx(RelationAll)`. Processor groups define the numbering; core records supply SMT and `EfficiencyClass`; cache records supply levels, sizes, line size, and sharing; NUMA and package records supply the rest. Cache domain = the set of CPUs sharing the highest-level non-instruction cache.
- Linux: sysfs (`topology/`, `cache/index*/`, `node*/cpulist`). Efficiency class is a heuristic (max frequency at least 15% below the fastest CPU). Compiled and reviewed but not yet exercised on hardware in this repository; verify on the first Linux build.
- CPUID leaves 0, 1, 7 (subleaves 0 and 1), 0x80000001, and 0x80000002–4; XCR0 via `xgetbv`. LZCNT is read from 0x80000001's ABM bit on both vendors — leaf 7's TZCNT bit is BMI1's and says nothing about LZCNT — and MOVBE from leaf 1. Those two are in `CpuFeatures` because x86-64-v3 contains them and a virtual machine is more likely to mask them off than to mask AVX2.

## The startup check, and why this module gives up the arch flag

[ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md) made x86-64-v3 the promised minimum CPU. A binary built for it that starts on an older machine dies on an illegal instruction, in a stack frame that says nothing about what happened; `core/platform/cpu_baseline.h` turns that into one line on stderr and exit code `k_exit_cpu_too_old` (78):

```
engine: this build needs x86-64-v3 and this CPU has no AVX, AVX2, FMA, BMI1, BMI2, F16C, LZCNT,
MOVBE (Intel(R) Core(TM) i7 CPU X 980 @ 3.33GHz); rebuild with -DENGINE_CPU_BASELINE=v2 ...
```

Two things make that check safe, and both are properties of the build rather than promises made in a comment:

- **Nothing in this module can be an AVX2 instruction.** `cmake/EngineCpuBaseline.cmake`'s `engine_strip_cpu_baseline(engine_platform)` takes the arch flag back off this one target, so `cpu_baseline.cpp` and the `cpu_features.cpp` it calls are compiled at the floor whatever the rest of the tree is built for. `build/<preset>/compile_commands.json` is where that is checked, and it is the artefact the claim rests on rather than this sentence — on 2026-09-19, 846 of `msvc-release`'s 851 translation units carried `/arch:AVX2` and the five that did not were exactly this module's. `dumpbin /SYMBOLS engine_platform.lib` is the second artefact: it shows the `.CRT$XCT` section and `engine_cpu_baseline_init` in it. The module gives up nothing real: CPUID, the topology walk and thread pinning all happen once at start-up, and the rule is one sentence — *the module that decides whether the binary may run is compiled for every machine* — instead of a list of files somebody has to maintain.
- **It runs before anything else can.** Every app's `main()`, the shared test main, and the shared bench main call `require_cpu_baseline()` as their first statement, and `engine_app()`, `engine_test_main` and `engine_bench_main` link `engine::platform` unconditionally so no CMakeLists has to remember. The explicit call is also what pulls this object file out of the static library. On top of it, an initializer in the same translation unit runs the check ahead of every *dynamic initializer* in the binary — `.CRT$XCT` sorts before the `.CRT$XCU` MSVC emits C++ initializers into, and a GCC/Clang `constructor(101)` sorts before the default priority — which is what covers a `static Foo g = expensive();` in a translation unit that *was* compiled with AVX2. Without that half, "the first statement of `main`" would still be after every such initializer — and after `main`'s own prologue, which is generated by a compiler that has been told it may use AVX2. The only code that runs ahead of `.CRT$XCT` is the C runtime's own start-up, which is not compiled with this tree's flags.

The check takes the feature set it judges rather than reading CPUID itself, which is why the interesting case is a unit test (`tests/cpu_baseline_tests.cpp`) and not a machine nobody has: the test hands it a Westmere, a **Sandy Bridge**, a Core 2, a Haswell, and a Haswell with LZCNT and MOVBE masked off the way a hypervisor does, and checks the list of names each produces. The Sandy Bridge case is the project's Linux GPU runner (Xeon E5-2670) and is the one that pins a property the others cannot: it **has** AVX, so the line it prints must not say "no AVX" — the features are checked one at a time rather than in tiers precisely so that a machine which is short of only part of a level is told which part. `require_cpu_baseline()` is four lines on top of that and uses `detect_cpu_features()` rather than the cached `cpu_features()`, because a function-local static reaches for the compiler's thread-safe-statics machinery at a point in start-up where it has no business being asked.

**Untested on GCC and Clang.** The constructor attribute and `-march=x86-64-v2/v3` were written from documented behaviour; hosted CI was unavailable when this landed and no Linux machine here has run them.

**Verified on.** Intel i9-10980XE (36 logical CPUs, 18 cores with SMT, one cache domain, one class). The i9-13980HX laptop (8 P + 16 E cores, hybrid) is the next target and will exercise `efficiency_cpus`.

**Depends on.** `base`, `memory`, `containers`.

**Testing.** `tools/dev.ps1 test -Filter platform`. Tests check the invariants above on whatever machine runs them, that repeated detection is stable, that pinning moves a thread onto the requested CPU, and that CPUID implications hold. `tests/cpu_baseline_tests.cpp` drives the baseline check with forced feature masks: a Westmere against a v3 build names all eight missing features in order and against a v2 build names none; a Core 2 fails even v2; a Haswell passes both; one missing feature is named on its own; a truncated buffer is truncated rather than overrun and still counts in full; and this binary's own `k_build_cpu_baseline` is the one the preset configured.

**Performance notes.** Cold data. `describe_topology` prints a summary for logs; the job system consumes the structure directly.
