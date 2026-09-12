# ADR-0019: Efficiency as a first-class metric: size table, engine container set, banned containers, fast-path APIs

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/11-performance-principles.md §11.2, §11.9

## Context

The habit of accepting less efficient code because compute and memory are cheap is what produces engines that fall apart at high resolution. Footprint drives everything that copies, uploads, serializes, or replicates data. Node-based containers spend an allocation, a pointer chase, and tens of bytes per element. A production engine should also make its users' games fast by default.

## Decision

Footprint is measured and reviewed like time. Every hot type has an entry in its module's size table checked by `static_assert`; growth requires justification. Hot objects stay minimal and promote on demand (optional components or side tables; a `promote()` to an extended type in class-based code); no virtual dispatch in hot types. The engine ships one container set in `core/containers` (`FlatMap`/`FlatSet` as sorted vectors, open-addressing `HashMap`/`HashSet`, `SlotMap`, `SmallVector`, `FixedVector`, `IntrusiveList`, `BitSet`, `RingBuffer`); `std::map`, `std::set`, `std::unordered_map`, `std::unordered_set`, `std::list`, `std::deque`, `std::shared_ptr`, and hot-path `std::function` are banned in engine code and caught by lint, allowed in tools, tests, and marked cold initialization. Public engine APIs are batch-first with handles rather than pointers; schema-declared components generate SoA storage; costs are visible in the asset browser and profiler; validators flag anti-patterns in game code; defaults are the optimized path.

## Consequences

Any performance loss in a shipped game is attributable to that game's content and code, and the tools show which. Engine contributors choose containers by measured element count and mutation pattern, not habit.

## Revisit when

Never for the principle. Container size-class boundaries are tunables.
