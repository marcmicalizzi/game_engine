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
| `Vector<T, SizeType, Alloc>` | `vector.h` | 16 | Heap only; element type may be incomplete at declaration | Many elements; recursive types |
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
- `Vector`: `resize`/`append` grow geometrically and take the size exactly when the vector is empty or far smaller (see "Growth" below); `reserve` and `resize_exact` are the exact paths and `shrink_to_fit` returns the slack. **An argument to `push_back`, `emplace_back`, `emplace`, `insert`, `append` or `resize` must not refer into the vector itself**: the old buffer is freed before the argument is read, so `v.push_back(v[i])` reads freed memory whenever that push grows `v` — where std::vector copies first and permits it. Copy the element out first. The terrain rings' skirts did exactly that and segfaulted once in three suite runs ([terrain](terrain.md#rings), 2026-09-26); a read of freed memory faults only when the block has been unmapped, so ASan is what finds it.
- `SmallVector`: moving an inline vector moves its elements; moving a heap vector steals the pointer. `erase_unordered` is O(1). `resize`/`resize_exact` are `Vector`'s.
- `FixedVector`: `push_back` past capacity is a verified failure; `try_push_back` returns false.

**Growth, and why it is a footprint decision as much as a speed one.**

The contiguous containers grow by `grow_capacity` — **1.5× from a floor of four, but never less than what the call asked for** — and that second clause is the whole policy. It applies wherever a container has to reallocate to make room: `push_back`, `emplace`, `insert`, and (since 2026-09-18) `Vector::resize` and `Vector::append`.

`Vector::resize` used to take the size *exactly*, and a caller that grows an arena by appending a fixed-size run — acquire a slot, resize to the new total — therefore reallocated and relocated on **every single call**, which is O(n²) in the number of runs. That is not "slow", it is "does not finish": `systems/animation`'s pose pool spent ten CPU-minutes filling 65,536 slots in a debug build before it worked around it by growing its own capacity by hand, and the same loop costs **2.0 s** in release against 0.42 ms for the same work with the capacity reserved up front. The fix belongs in the container, because the next capability with an arena would have written the same workaround.

What keeps this from being a memory regression is the "never less than asked for" clause: **a `resize(n)` on an empty or much smaller vector still takes exactly `n`**, because 1.5× of a small capacity is below `n`. That is what almost every caller in this tree does — a capture buffer sized from a pixel count, a mesh array sized from a vertex count, a per-frame array sized from the frame count — and slack appears only where the growth is incremental, which is precisely the case that was quadratic.

Measured rather than argued. The renderer's and geometry's big arrays hold **the same bytes before and after**, checked by reading back `capacity()` on a 512×512 terrain through `build_cluster_lod` (523k triangles) with the old policy and the new one:

| Array | Filled by | Size | Capacity, exact policy | Capacity, geometric |
|---|---|---|---|---|
| `ClusterMesh::vertices` | `push_back` | 725,603 | 1,049,869 (44.7% slack) | 1,049,869 — unchanged |
| `ClusterMesh::vertex_source` | `push_back` | 725,603 | 1,049,869 (44.7%) | unchanged |
| `ClusterMesh::triangles` | `push_back` | 1,047,458 | 1,049,869 (0.2%) | unchanged |
| `ClusterMesh::attributes` | one `resize` | 725,603 | 725,603 (0%) | **725,603 (0%)** |
| `ClusterMesh::quantized` | one `resize` | 2,176,810 | 2,176,810 (0%) | **2,176,810 (0%)** |
| `ClusterMesh::clusters`, `ClusterLodMesh::lod` | `push_back` | 11,746 | 12,138 (3.3%) | unchanged |

Every row is identical, because the slack that exists was always `push_back`'s and the arrays this change could have affected are sized once. The renderer's are the same shape by inspection: `CapturedFrame::ids`/`depth`/`normals`, `cluster_blas`, the per-frame parameter arrays and the Hi-Z level list are each sized once from a count onto a fresh vector, so they take their size exactly.

Two escapes, for the cases the policy is wrong for. **`reserve(n)` is still exact** — it is the caller stating a number, so it is the one place the container does not second-guess it — and **`resize_exact(n)`** is `resize` with the exact capacity, for a long-lived array sized from a count it will not revise, where half again would be real memory. `shrink_to_fit()` gives the slack back when growth is finished. A caller reaching for `resize_exact` should be sizing once; growing one in a loop is the shape this whole note is about.

`SmallVector::resize` had the same exact-capacity behaviour and gets the same treatment, including `resize_exact`, because a policy the container set documents that one container quietly does not follow is a trap for whoever changes a `Vector<T>` into a `SmallVector<T, N>` and gets the quadratic back. `FlatMap`, `FlatSet` and the hash containers were already geometric on every path and are unchanged.

**Invariants.**
- Flat containers: keys strictly increasing under `Compare` outside `insert_bulk`.
- Hash containers: `size * 5 <= bucket_count * 4`; every live dense index is referenced by exactly one bucket; probe runs are gap-free (backward-shift deletion).
- `SlotMap`: a live handle's slot has an odd generation equal to the handle's; the free list contains exactly the slots with even generations.
- All: every constructed element is destroyed exactly once (tests count live instances of a tracked type through growth, erase, copy, move, and bulk operations).

**Public API.** The headers above. `core/containers/detail/` is not public.

**Depends on.** `base`, `hash`, `memory`.

**Testing.** `tools/dev.ps1 test -Filter containers`. Each container has ordering/lookup/erase tests, tracked-type leak tests through growth and relocation, copy/move tests, and a randomized comparison against the corresponding standard container (20k–50k steps). `Vector`'s growth policy has its own case, because it is the one behaviour here that is a promise about memory as well as speed: a first `resize` takes the size exactly, 1,000 fixed-size runs reallocate fewer than 40 times instead of 1,000, `reserve` and `resize_exact` stay exact, `shrink_to_fit` gives the slack back, and `append` behaves the same way. `tests/size_table.cpp` pins every object size.

**Performance notes.** Insert and erase in the flat containers are O(n) element moves; trivially copyable types move with `memmove`. Hash lookup is one bucket probe sequence plus one key access on a hit. The flat containers use a branchless binary search (fixed log2(n) trips, one conditional move per trip), which measured 1.3x (n = 16) to 1.6x (n = 4096) faster than the branchy form on random keys.

Measured with `core/containers/bench` (`tools/dev.ps1 bench -Preset msvc-release -Filter containers.find.*`, i9-10980XE, release, one thread; total time for one lookup of every key in random order):

| n | FlatMap | HashMap | std::map | std::unordered_map |
|---|---|---|---|---|
| 16 | 83 ns | 51 ns | 92 ns | 73 ns |
| 256 | 2.19 us | 0.99 us | 5.30 us | 1.14 us |
| 4096 | 52.9 us | 32.2 us | 325 us | 38.2 us |
| 65536 | 2.22 ms | 0.70 ms | 11.2 ms | 1.10 ms |

Growing an arena by fixed-size runs of 23 elements (`tools/dev.ps1 bench -Preset msvc-release -Filter 'containers.grow.*'`, i9-10980XE, release). **Machine state:** taken twice on 2026-09-18 with `--wait-quiet=1200`; both runs started quiet (others at 9.1% and 9.2% of the CPU, GPU 3% and 2%) and both ended under some load (18.1% and 12.3%), so the harness marked them upper bounds. The two agree within 4% on every row, which is what makes the ratios below worth quoting ([bench](bench.md#measuring-on-a-shared-machine)).

| Runs appended | `resize_exact` (the old policy) | `resize` (geometric) | `reserve` up front |
|---|---|---|---|
| 256 | 409.9 µs | **5.0 µs** (82×) | 1.85 µs |
| 2,048 | 5.74 ms | **33.5 µs** (171×) | 13.5 µs |
| 16,384 | 2.03 s | **975 µs** (2,080×) | 425 µs |

Reading: the exact path is quadratic and the geometric one is not, which is the whole finding; 16,384 runs is the population `systems/animation` actually hits. Geometric growth lands within 2.3–2.7× of the floor a caller that knew its size up front would pay, and that remaining gap is the relocations 1.5× growth still does — which is why `AnimationConfig::reserve_joints` and its equivalents are still worth having. Before the change, `resize` and `resize_exact` measured the same to within noise (442.7 µs / 5.86 ms / 1.97 s against 380.1 µs / 5.84 ms / 2.01 s), which is the control that says the two rows above are the policy and not the benchmark.

Reading: `HashMap` wins lookups at every size measured, including n = 16 (3.2 ns against 5.2 ns per lookup). `FlatMap` wins on footprint (16 bytes, no bucket array), on ordered iteration, and on iteration bandwidth (contiguous SoA values); it beats `std::map` everywhere. So the choice is by access pattern, as 11 §11.2 says, not by size alone: a small map that is mostly iterated or needs order is a `FlatMap`; a map that is mostly looked up is a `HashMap` at any size. Remaining candidates for the harness: growth factor, an Eytzinger layout or prefetch for large flat maps, and the hash-table load cap.
