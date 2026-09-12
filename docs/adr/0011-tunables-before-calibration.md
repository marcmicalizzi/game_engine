# ADR-0011: Tunables registry now; calibration runner only with evidence

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/01-critique.md §1.2, docs/plan/08-toolchain.md §8.7

## Context

The brief proposed empirically characterizing each machine and selecting precompiled kernel variants. The motivation (pinning, cache-domain locality, no branches in hot paths) is sound; a general calibration system built before kernels exist would measure microbenchmarks rather than the engine.

## Decision

Day one: capability and topology detection, pinned worker pools per cache domain, NUMA-aware arenas, and a tunables registry in which every kernel registers its parameters, defaults, ranges, and a benchmark harness. Compile-time specialization is used for data-selected dimensions (format, mode, feature flags, SIMD width via ISPC) behind dispatch tables; hardware-selected dimensions (prefetch distance, batch size, thread counts, workgroup size) are runtime parameters hoisted out of hot loops. An automatic calibration runner that measures real kernels on real data and stores a per-hardware profile is built only after two owned machines disagree on a tunable's best value by more than 10% of frame time (experiment E3).

## Consequences

Kernels stay readable; variant counts grow only with data configurations. The benchmark harness serves humans and agents immediately. Calibration infrastructure is not speculative.

## Revisit when

E3 fires.
