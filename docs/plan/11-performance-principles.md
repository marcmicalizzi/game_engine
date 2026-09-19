# 11 — Performance Engineering Principles and Hot-Path Standards

Why this document exists: the project's origin is frustration with engines that ship beautiful features at unplayable frame rates and then downgrade to reach consumers. This engine treats performance as a feature gate from the first renderer milestone. These are the rules and idioms every contributor, human or agent, applies. Validators and CI enforce the ones that can be checked; code review enforces the rest.

## 11.1 Culture: budgets are gates

- Every target configuration ([04 §4.1](04-renderer.md#41-goals-targets-non-goals)) has a frame-budget file: per-pass GPU milliseconds, per-system CPU milliseconds, memory by tag, streaming bandwidth.
- A change that pushes a pass over budget on the reference scenes does not merge. It lands behind a quality tier or gets optimized first.
- "Add a quality option" is not a substitute for optimizing the default path. The default must hit budget at the target.
- Every feature has a cost estimate before implementation and a measured cost after.
- Performance regressions are bugs with the same priority as crashes.
- The reference renderer and FLIP metric exist so that "cheaper" can be checked against "looks the same"; every optimization that changes the image is accepted or rejected on both numbers.
- Efficiency is not optional because hardware is cheap. The habit of accepting less efficient code because compute and memory are plentiful is exactly what produces engines that fall apart at high resolution. Footprint and layout are designed, measured, and reviewed like correctness.
- The frame report attributes every millisecond to an engine system or a game system. When a game runs slowly, its developer can see whose cost it is. This document budgets engine time; making game time easy to keep low is [§11.9](#119-performance-by-default-for-engine-users).

## 11.2 Data layout and footprint first

**Footprint is a first-class metric.** Smaller objects mean more of them per cache line, cheaper copies, cheaper GPU uploads, cheaper serialization, and later cheaper replication. Every operation that moves data scales with its size, so shrinking the data speeds up everything downstream at once.

- Choose layout from the access pattern: SoA for streaming over one field; AoS for touching whole records; AoSoA (SIMD-width blocks) for vectorized kernels over several fields.
- Care about the *resulting stride*, not the nominal size. A 17-byte record with a 32-byte stride wastes half of every cache line; a 14-byte record with a 16-byte stride gained nothing over 16 bytes.
- **Size table.** Every hot type has an expected `sizeof` and alignment recorded in a size table and checked by `static_assert`. Growth requires a justification in the change description, the same way an API change does.
- **Keep the common object minimal; promote on demand.** Rarely used state does not live in the hot object. Put it in a side structure reached through an optional index, or, in the ECS, an optional component added when first needed. In class-based code the same idea is a small base type with a `promote()` that constructs the extended type when the less-frequent interface is required. The default instance pays only for what everything needs.
- No virtual dispatch in hot types: a vtable pointer costs 8 bytes per object and an indirect call per use. Use type tags and dispatch tables ([§11.4](#114-branch-free-hot-paths-and-constexpr-dispatch)).
- Pack deliberately: enums as `uint8_t`, flags as bitfields, quantized values where precision permits (16-bit normals, 8- or 16-bit UVs, fixed-point positions within a tile), 32-bit indices instead of 64-bit pointers.
- Hot and cold split by array as well as by object: data touched every tick lives in a separate array from data touched rarely.
- Small-buffer optimization for strings and vectors that are usually small; arena-backed strings in hot paths.
- No allocations in the frame loop in steady state: arenas, pools, and frame allocators. The per-frame allocation count is a CI metric.

### Containers: the engine set and the rules

Node-based containers spend an allocation, a pointer chase, and 32 to 48 bytes of overhead per element, and iterate in cache-hostile order. The engine ships one container set in `core/containers` and bans the alternatives in engine code.

| Need | Use | Why |
|---|---|---|
| Small map or set (up to a few hundred elements), or any size when reads dominate | `FlatMap` / `FlatSet`: sorted vector with binary search | O(log n) lookup, O(n) insert, one contiguous allocation, no per-node overhead, iterates in order and in cache order. Replaces the vast majority of `std::map` uses in an engine |
| Large map with frequent inserts and erases | `HashMap` / `HashSet`: open-addressing with dense storage | O(1) average, no per-node allocation, iteration over a dense array. The sorted vector's O(n) insert stops paying above a few hundred elements under churn |
| Handle → object | `SlotMap` (generational index pool) | O(1) everything, stable handles, detects stale handles, dense storage |
| Usually-small sequence | `SmallVector<T, N>` | Inline storage for N elements, heap only beyond |
| Bounded sequence | `FixedVector<T, N>` | No heap at all |
| Ordering by a key, many elements, removal from anywhere | `IntrusiveList` | No allocation per link; the element owns its links |
| Dense flag sets | `BitSet` | 64 flags per cache line |
| Producer/consumer | `RingBuffer` | Fixed, contiguous, no allocation |

Rules:

- `std::map`, `std::set`, `std::unordered_map`, `std::unordered_set`, `std::list`, `std::deque`, `std::shared_ptr`, and `std::function` in hot paths are banned in engine code. clang-tidy enforces it. They are allowed in tools, tests, and cold initialization.
- Never iterate a node-based map in a hot loop; if a structure is both looked up and iterated, keep the dense array as the source of truth and index it.
- Choose by measured element count and mutation pattern, not by habit. The size class boundaries above are starting points; the tunables harness settles them per structure.
- The same policy applies to game code through the scripting and gameplay APIs ([§11.9](#119-performance-by-default-for-engine-users)): the engine exposes its containers so that the convenient thing to use is also the fast thing.

## 11.3 Access order and the prefetcher

- Iterate in memory order. The hardware prefetcher detects sequential and constant-stride streams; it does not detect a column walk through row-major data.
- Worked example, from a 4K video oscilloscope (a per-column 2D histogram over row-major YUV frames): walking down each column touches one element per row at a large stride and defeats the prefetcher. Processing each row within a batch and accumulating into per-column histograms that stay in cache ran 20–30× faster. Same arithmetic, same thread count; only the traversal order changed. Expect the same class of win in culling, transform, animation, and histogram-like GPU-feedback kernels.
- Batch to cache: size work items so the batch's working set fits L1 or L2 (from topology detection), then move to the next batch.
- Explicit prefetch (`_mm_prefetch`) only when measured. Irregular access (BVH traversal, hash lookups) is where it sometimes pays; the distance is a tunable.
- Sort or bucket first, then stream: when access is inherently irregular, reorder the indices so the data pass is sequential. Ray sorting by direction, instance sorting by mesh, and entities grouped by archetype are all instances of this.

## 11.4 Branch-free hot paths and `constexpr` dispatch

- No data-dependent branches inside inner loops. Configuration-dependent behavior is a template parameter; the inner loop uses `if constexpr`.
- The standard idiom:

```cpp
enum class Fmt  : uint8_t { Yuv420, Yuv422, Rgb, Count };
enum class Mode : uint8_t { Luma, Chroma, Both, Count };

template <Fmt F, Mode M>
void kernel(Span<const uint8_t> in, Span<uint32_t> out) {
    for (/* batch */) {
        if constexpr (F == Fmt::Yuv420) { /* ... */ } else { /* ... */ }   // resolved at compile time
    }
}

using Fn = void (*)(Span<const uint8_t>, Span<uint32_t>);
constexpr auto table = make_dispatch_table<Fn, Fmt, Mode>(
    []<Fmt F, Mode M>() { return &kernel<F, M>; });

// one indirect call per batch, selected from the data's configuration
table[index(fmt, mode)](in, out);
```

- Instantiate the table from the enums so the variant set is explicit and countable. The build reports instantiation count and code size per kernel.
- Put a dimension in the template only if it changes the loop body. Prefetch distance, batch size, and thread count do not; they are runtime parameters read once before the loop and held in registers ([01 §1.2](01-critique.md#12-re-scoped-hardware-characterization-and-compile-time-specialization)).
- ISPC for SIMD-heavy kernels: one source, multiple ISA targets, automatic runtime dispatch. Use it instead of hand-written intrinsics unless measurements say otherwise. Inline assembly is not planned; any asm block must come with a benchmark showing the compiler could not get there.
- **The baseline is x86-64-v3 and the dispatch above it is AVX-512 only** ([ADR-0031](../adr/0031-minimum-cpu-x86-64-v3.md), [08 §8.9](08-toolchain.md#89-the-cpu-baseline-and-what-each-dependency-does-about-it)). That is a statement about *where runtime selection starts*, and it makes the multi-target machinery cheaper than the plan assumed: AVX2, FMA, BMI1/2 and F16C are compiled in unconditionally, so a kernel needs a variant set only where 512-bit registers change the loop's shape, and the ISPC target list is `avx2` plus `avx512skx` rather than a ladder from SSE2. A variant is still selected once per batch from `platform::cpu_features()` and never inside the loop, and **a kernel with no AVX-512 variant is the normal case, not an omission**: the parts that have it are a minority, they disagree about frequency behaviour, and E3's calibration trigger (two owned machines disagreeing by more than 10% of frame time) is the evidence that would justify writing one.
- Branchless selection (`select`, masks, table lookups) inside the loop; rare-case handling moves to a separate pass over a compacted list.

## 11.5 Threads, cores, and memory locality

- Worker pools are built from topology: one pool per cache domain (CCD, core cluster, or NUMA node); a performance-core pool for simulation and rendering; an efficiency-core pool for streaming, decompression, audio mixing, and background inference. Threads are pinned.
- Work stealing prefers the same cache domain; cross-domain stealing only after local queues have been empty for a threshold.
- Allocate where you compute: per-pool arenas from the local NUMA node; long-lived data one system owns lives in that system's domain.
- False sharing: per-thread accumulators padded to the cache line (`alignas(std::hardware_destructive_interference_size)`); no adjacent hot atomics.
- Synchronization is a budget: prefer frame-phase barriers and per-system output buffers over fine-grained locks; count contended locks per frame in telemetry.
- SMT: treat sibling threads as one core for compute-bound kernels; allow both for latency-bound work (I/O, decompression).
- Cross-domain data handoff (one CCD produces, another consumes) is a measured cost per system, not an accident. Where it is large, move the consumer or the producer.

## 11.6 No hidden limits

- No hard-coded maximum for resolution, view count, lights, bones, entities, instances, texture size, or coordinate range. Every limit is a configurable budget with a validator.
- Compact screen-space or index representations are allowed only through a checked type: constructed from the render configuration, exposing `CanRepresent(config)`, and falling back to the wider layout when it cannot. Clamping, wrapping, or silent truncation is a bug. The wide layout is the default; the compact one is an optimization that must prove itself with cache-density measurements before it is adopted.
- All render targets, atlases, tile grids, and screen-space buffers are sized from the render configuration at startup. No constants.
- World positions are tile-relative; time is 64-bit integer ticks; IDs are 128-bit ([02 §2.7](02-architecture.md#27-generality-what-the-engine-must-not-preclude)).
- CI renders at 11520×2160, 7680×4320, 1080×3840, and 5120×1440 nightly with the UI and all screen-space effects enabled. The known real-world failures this guards against: nothing rendered past 8192 pixels; UI missing past 4096 pixels.

## 11.7 GPU

- Work proportional to what can be seen: cull early and hierarchically; select LOD by projected error; allocate rays by attention region and variance. The periphery in a surround setup is peripheral vision and is rendered like it.
- Indirect everything; no CPU round trips inside the frame (readbacks are a frame late and asynchronous).
- Occupancy over cleverness: register pressure and shared-memory use are checked per kernel; workgroup size is a tunable.
- Bandwidth is the budget: pack G-buffer data, prefer compute where it avoids overdraw, reuse across frames, and never shade at a resolution the display cannot show.
- Async compute for acceleration-structure builds, denoiser preparation, and streaming decompression, scheduled by the render graph.

## 11.8 Measure before and after

- Every optimization change carries: the counter or timing it targets, before/after numbers on named scenes and machines, and the code-size delta.
- Tracy zones on every system; GPU timestamps on every pass; hardware counters (cache misses, branch misses, bandwidth) via the profiling service for CPU kernels.
- The tunables harness exists so that "is this faster?" is a command, not an opinion.
- Distrust microbenchmarks for cross-machine decisions; trust them for layout and traversal decisions, which are machine-independent.
- Memory metrics sit beside time metrics in every report: heap high-water by tag, per-frame allocation count, hot-type sizes from the size table, GPU residency by page type.

## 11.9 Performance by default for engine users

The goal for a production engine: any performance loss in a shipped game is attributable to that game's content and code, never to the engine, and the engine's tools show the developer exactly where. That requires more than fast engine code. The engine's public surface has to make the fast path the easy path, so developers are not fighting the engine to stay performant.

- **Batch APIs are the primary APIs.** Place ten thousand instances, update a thousand transforms, or query a region in one call. Per-object convenience calls exist for tooling and are marked as such in the schema, and the profiler flags them when they appear in per-frame game code.
- **Handles, not pointers,** across every public boundary. Stable, checkable, 32-bit, and free of lifetime questions.
- **Data-oriented layout without asking for it.** Components declared in the schema generate SoA storage, hot/cold splits from a `cold` annotation, and archetype-friendly access. A developer who writes ordinary components gets cache-friendly layout by default.
- **The engine container set is the game's container set.** Gameplay code, and the scripting layer once chosen, use `FlatMap`, `SlotMap`, `SmallVector`, and the rest through the same interfaces, so the convenient choice is the efficient one.
- **Costs are visible where decisions are made.** The asset browser shows each asset's estimated cost (clusters at typical distance, acceleration-structure memory, texture memory) against its category budget. The profiler attributes frame time to engine systems and game systems separately. Budget violations are telemetry events, not silent slowdowns.
- **Validators catch anti-patterns early.** Per-frame allocation from game code, per-entity queries inside loops, hot components over their size budget, scripts scaling quadratically with entity count, textures larger than their on-screen size class, meshes without a cluster hierarchy. Each produces a structured diagnostic with the fix.
- **Defaults are the optimized path.** Quality tiers ship tuned to the target configurations. No feature ships with an expensive mode as its default and a fast mode hidden behind a flag.
- **Engine time is budgeted and tested** ([§11.1](#111-culture-budgets-are-gates)), so on any machine that meets the target configuration, the engine's share of the frame is known in advance. What remains is the game's, and the tools say which part.

## 11.10 Absent capabilities are free

The engine roughs in every simulation capability of any value: the level of abstraction, the asset hook, the LOD policy, and the determinism stance exist for each one, so that a capability can be added or deepened later without rescaffolding the systems around it ([05 §5.15](05-simulation.md#515-capability-inventory)). Roughing in means the interface and the data slot, not the implementation, because a niche capability that eats the schedule has already cost more than it is worth. The price of that generality is one rule, and it is not negotiable: **a capability that is not in use costs nothing at run time.** No pass, no dispatch, no allocation, no per-frame work when it has no instances, and ideally no linked code.

This is a performance rule and an ambition rule at once. An open-source engine aiming at AAA quality should err toward capabilities that let a small team get physical, systemic interaction without authoring every case, and that is only affordable if the ones a given game does not use are invisible in its profile. An engine that charges a millisecond for every capability it *could* have is the thing this document exists to prevent.

The mechanisms, all of which the plan already has for other reasons:

- **A system is a module, and a module registers only when it is linked.** Systems are declared once through `engine_module()` and register themselves with the scheduler and the render graph at initialization ([02 §2.3](02-architecture.md#23-subsystem-boundaries-and-layering)). A game that never links `systems/deformation` has no deformation code in its binary, no registration, and nothing to skip. Capability presence is therefore a link-time fact, not a runtime flag, and no hot loop asks about it.
- **The tick scheduler skips a system with no instances.** Systems declare their read and write component sets ([05 §5.2](05-simulation.md#52-sim-scheduler)); an empty instance set means the system is not in the tick's schedule at all, rather than a per-tick call that returns immediately. The frame report shows it as absent, not as 0.00 ms.
- **A render pass is added to the graph only when its inputs exist.** The graph is rebuilt per frame from declared resources; a pass whose inputs no producer wrote is not compiled into the frame ([04 §4.2](04-renderer.md#42-frame-architecture)). No water in view means no water pass, no transient targets for it, and no barriers.
- **No virtual dispatch and no data-dependent branch for an absent feature in a hot loop.** Already the rule ([§11.4](#114-branch-free-hot-paths-and-constexpr-dispatch)): configuration-dependent behavior is a template parameter resolved at compile time and selected once per batch, so a capability nobody enabled does not appear as a predictable-but-real branch in the inner loop of one that is.
- **Per-capability memory pools allocate on first use.** Cage state, attachment constraints, deformation map pages, thermal fields, scent fields, and every other per-capability store are pools created when the first instance appears and reported by tag ([§11.8](#118-measure-before-and-after)). Zero instances is zero bytes, not a reserved arena, and the memory report distinguishes the two.
- **An empty asset slot is free.** A property block absent from an asset produces no component, no derived-data node, no bytes in the cluster pages, and no validation cost ([07 §7.3](07-content-pipeline.md#73-content-build-the-derived-data-graph)). This is what keeps the asset format general without making every asset pay for the generality.

**The check.** A **"nothing enabled" benchmark frame** belongs in the benchmark corpus ([09 §9.4](09-testing-profiling.md#94-benchmark-scene-corpus)): one scene with a camera, a static mesh, and a light, with every optional capability switched off, recorded per target configuration as four numbers — CPU tick time, GPU frame time, steady-state resident allocation count, and binary size. A change that lands a new capability must not move any of the four. The threshold is the measurement noise floor rather than a percentage allowance; as a starting point, a regression is a median shift above 1% or 0.05 ms, whichever is larger, or any increase at all in the allocation count. Both numbers are invented and get replaced the first time the benchmark runs. They are deliberately tighter than the 3% general performance gate of [09 §9.1](09-testing-profiling.md#91-test-taxonomy), because this frame is supposed to be constant: nothing in it is doing anything. A capability that cannot land without moving them is a design problem, not a budget negotiation: the registration, the pool, or the branch that costs the time is the thing to remove.
