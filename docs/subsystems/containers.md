# containers (core)

**Purpose.** The engine's container set (ADR-0019, docs/plan/11-performance-principles.md §11.2). Engine code uses these instead of node-based standard containers, which `tools/lint.ps1` bans.

**Owned data.** Each container owns its heap blocks (one or two) or none when empty. Allocation goes through an allocator policy (`core/memory/allocator.h`); the default is the global tagged heap and costs no bytes per container.

**Available**

| Container | Header | Object size | Layout | Use for |
|---|---|---|---|---|
| `FlatMap<K, V, Compare, SizeType, Alloc>` | `flat_map.h` | 16 | One block: sorted keys, then values (SoA) | Small maps; read-mostly maps of any size |
| `FlatSet<K, Compare, SizeType, Alloc>` | `flat_set.h` | 16 | One block: sorted keys; iterators are `const K*` | Small or read-mostly sets |
| `HashMap<K, V, Hasher, KeyEqual, SizeType, Alloc>` | `hash_map.h` | 32 | Dense SoA keys/values block + Robin Hood bucket array | Large maps; insert/erase churn |
| `HashSet<K, Hasher, KeyEqual, SizeType, Alloc>` | `hash_set.h` | 32 | Dense keys block + bucket array; iterators are `const K*` | Large sets; churn |
| `SlotMap<T, Alloc>` | `slot_map.h` | 40 | Dense values block + slot table; 64-bit generational handles | Owned objects referenced across systems |
| `SmallVector<T, N, SizeType, Alloc>` | `small_vector.h` | 16 + N·sizeof(T) | Inline storage for N, heap beyond; branch-free `data()` | Usually-small sequences |
| `FixedVector<T, N, SizeType>` | `fixed_vector.h` | sizeof(SizeType) + N·sizeof(T) | Inline only | Bounded sequences, no heap |

Planned: `IntrusiveList`, `BitSet`, `RingBuffer`.

**Choosing.** Sorted vector (`FlatMap`) up to a few hundred elements or whenever reads dominate; `HashMap` above that under churn. `SlotMap` whenever something outside the owner needs a reference that must survive reallocation and detect staleness. `SmallVector` when the common size is known and small. The size-class boundaries are starting points for the tunables harness.

**Common conventions.**
- `find_value(key)` returns a pointer or `nullptr`; there is no throwing `at()`. `operator[]` value-initializes on miss.
- Heterogeneous lookup: flat containers through a transparent comparator (`std::less<>`), hash containers when both `Hasher` and `KeyEqual` are transparent (the defaults are for string-like keys).
- `extract(key)` returns `std::optional<Value>` by move. `insert_bulk(range)` for building in one pass.
- Map iteration yields `std::pair<const K&, V&>`; write `for (auto [k, v] : map)`. Map iterators are index-based proxies, 16 bytes, invalidated by insert and erase.
- `SizeType` bounds a container at 2^32 entries by default; pass `u64` where a single structure needs more (8 bytes more per object).
- Allocator policies: `mem::DefaultAlloc` (empty) or `mem::ArenaAlloc{&arena}` (one pointer).

**Per-container notes.**
- `FlatMap`/`FlatSet`: binary search touches only keys; `append_sorted` for pre-sorted input; `insert_bulk` is O((n+m) log(n+m)) with last-wins semantics. Growth combines reallocation with the insertion hole so elements move once.
- `HashMap`/`HashSet`: iteration is in insertion order until an erase, which moves the last element into the hole; `erase(it)` returns `it`, now referring to the moved-in element. Fingerprints are compared before keys, so most probes never touch the key array. Load factor is capped at 80%; buckets are powers of two. Design follows ankerl::unordered_dense.
- `SlotMap`: `SlotHandle` is 32-bit index + 32-bit generation; live generations are odd. Stale and null handles resolve to `nullptr`. Dense indices are not stable across erase; handles are. Not copyable (a copy would duplicate handles).
- `SmallVector`: moving an inline vector moves its elements; moving a heap vector steals the pointer. `erase_unordered` is O(1).
- `FixedVector`: `push_back` past capacity is a verified failure; `try_push_back` returns false.

**Invariants.**
- Flat containers: keys strictly increasing under `Compare` outside `insert_bulk`.
- Hash containers: `size * 5 <= bucket_count * 4`; every live dense index is referenced by exactly one bucket; probe runs are gap-free (backward-shift deletion).
- `SlotMap`: a live handle's slot has an odd generation equal to the handle's; the free list contains exactly the slots with even generations.
- All: every constructed element is destroyed exactly once (tests count live instances of a tracked type through growth, erase, copy, move, and bulk operations).

**Public API.** The headers above. `core/containers/detail/` is not public.

**Depends on.** `base`, `hash`, `memory`.

**Testing.** `tools/dev.ps1 test -Filter containers`. Each container has ordering/lookup/erase tests, tracked-type leak tests through growth and relocation, copy/move tests, and a randomized comparison against the corresponding standard container (20k–50k steps). `tests/size_table.cpp` pins every object size.

**Performance notes.** Insert and erase in the flat containers are O(n) element moves; trivially copyable types move with `memmove`. Hash lookup is one bucket probe sequence plus one key access on a hit. Candidates for the tunables harness: growth factor, a branchless binary search, the flat-vs-hash size threshold, and the hash-table load cap.
