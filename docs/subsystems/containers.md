# containers (core)

**Purpose.** The engine's container set (ADR-0019, docs/plan/11-performance-principles.md §11.2). Engine code uses these instead of node-based standard containers, which `tools/lint.ps1` bans.

**Owned data.** Each container owns exactly one heap block (or none when empty). Allocation currently goes through the global aligned `operator new`; it will route through `core/memory` when that module exists.

**Available now**

| Container | Header | Layout | Use for |
|---|---|---|---|
| `FlatMap<K, V, Compare = std::less<>, SizeType = u32>` | `flat_map.h` | One block: sorted keys array, then values array (SoA). 16-byte object | Small maps, read-mostly maps of any size |
| `FlatSet<K, Compare = std::less<>, SizeType = u32>` | `flat_set.h` | One block: sorted keys. 16-byte object. Iterators are `const K*` | Small or read-mostly sets |

Planned: `HashMap`/`HashSet` (open addressing, dense), `SlotMap`, `SmallVector<T, N>`, `FixedVector<T, N>`, `IntrusiveList`, `BitSet`, `RingBuffer`.

**FlatMap notes.**
- Lookup is a binary search over the keys array only, so a 100-entry map with 8-byte keys touches at most two or three cache lines regardless of value size.
- `find_value(key)` returns a pointer or `nullptr`; there is no throwing `at()`.
- `operator[]` value-initializes on miss. Heterogeneous lookup works through the transparent comparator: `FlatMap<std::string, T>` accepts `std::string_view` and `const char*` queries.
- `append_sorted` builds from pre-sorted input without searching; `insert_bulk` merges an unsorted range in O((n+m) log(n+m)) with insert-or-assign semantics (later duplicates win).
- Iteration yields `std::pair<const K&, V&>`; write `for (auto [k, v] : map)`. Iterators are index-based proxies (16 bytes) and are invalidated by any insert or erase.
- `SizeType` bounds a single container at 2^32 entries by default; pass `u64` for the rare structure that needs more (costs 8 bytes per container object).

**Invariants.**
- Keys strictly increasing under `Compare` at all times outside `insert_bulk`.
- `size() <= capacity()`; `capacity() == 0` iff no block is allocated.
- Every constructed element is destroyed exactly once (checked in tests with a live-instance counter).

**Public API.** `core/containers/flat_map.h`, `core/containers/flat_set.h`. `core/containers/detail/` is not public.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter containers`. Covers ordering, heterogeneous lookup, erase/extract, growth with non-trivial types and leak counting, copy/move, bulk insertion, and a 20k-step randomized comparison against `std::map`/`std::set`. `tests/size_table.cpp` pins object sizes.

**Performance notes.** Insert and erase are O(n) element moves; trivially copyable types move with `memmove`. Growth is 1.5× from a floor of 4 and combines reallocation with the insertion hole so elements move once. Candidates for the tunables harness once it exists: growth factor, a branchless binary search, and the size threshold at which `HashMap` should be preferred.
