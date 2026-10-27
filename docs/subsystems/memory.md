# memory (core)

**Purpose.** The allocation front door and accounting for the whole engine (ADR-0019). Every heap allocation in engine code goes through `mem::allocate` / `mem::deallocate`, directly or through an allocator policy, so footprint is measurable: a global allocation counter, total bytes live and peak, and per-tag bytes live and peak in development builds. Also provides `Arena`, a bump allocator for scratch and per-frame memory.

**Owned data.** The tag registry (fixed budget of `k_max_tags = 256` names), global and per-tag statistics, and, when `ENGINE_MEMORY_TRACKING` is on, a sharded pointer-to-tag table so a free is credited to the tag that allocated, wherever it happens.

**Invariants.**
- `allocate` never returns null (it aborts); `try_allocate` may. `deallocate(nullptr, ...)` is a no-op. Zero-byte requests are rounded up to one byte in both directions.
- With tracking on, every pointer passed to `deallocate` was returned by `allocate` and the byte count matches (both asserted in debug).
- `total_stats().bytes_current` returns to its previous value after every allocation is freed (tested single- and multi-threaded).
- `Arena::reset()` reaches steady state: after the first pass no further heap allocations occur.
- `allocations_on_thread()` counts the calling thread's allocations and no other thread's, in every build and under any tag (tested). It is the frame-loop metric's counter ([renderer](renderer.md#what-a-frame-waits-for)): a frame's work is snapshotted on the frame's thread, so a job worker building tiles beside it does not count, and a shipping build with tracking compiled out counts the same as a development one. The global `allocation_counter()` sees every thread and so cannot say whether the frame loop allocated.

**Public API.**
- `core/memory/memory.h`: `TagId`, `register_tag`, `tag_name`, `current_tag`, `TagScope`, `Stats`, `stats(tag)`, `total_stats()`, `tracking_enabled()`, `allocation_counter()`, `allocations_on_thread()`, `allocate`, `try_allocate`, `deallocate`.
- `core/memory/allocator.h`: `AllocatorPolicy` concept, `DefaultAlloc` (empty; the global tagged heap).
- `core/memory/arena.h`: `Arena` (allocate, create, create_array, mark/rewind, reset, release, statistics), `ArenaAlloc` policy (one pointer).

**Tagging model.** Tags follow the code path, not the object: `mem::TagScope scope(tag)` on the current thread attributes every allocation made inside it. Containers therefore store no tag. Tags are registered once per module at startup; names are copied and truncated to 47 characters.

**Debug allocation sites** (2026-10-26, W73). `begin_allocation_capture(sites, capacity)` records
16 return addresses per distinct stack into caller-owned `AllocationSite` storage on the current
thread, accumulating its count and requested bytes. It allocates no engine storage. Captures do
not nest; `pause_allocation_capture` returns the previous pause state, and
`end_allocation_capture` stops capture and returns the count whose sites exceeded the supplied
capacity. `print_allocation_capture` writes the sites to stderr (on Windows with the executable's
load base, on Linux also with module-relative offsets). Use the matching executable and debug
symbols to resolve them. These APIs and capture work exist only under `ENGINE_DEBUG`; the
allocation counter remains identical in all builds. The frame-loop flight captures 64 distinct
sites per submission, pauses over its bookkeeping, and prints allocating recorded frames, so a
failed zero-allocation test carries attribution rather than only a total.

**Depends on.** `base`, `hash` (for the tracker's pointer hashing).

**Testing.** `tools/dev.ps1 test -Filter memory`. Per-tag tests skip themselves when tracking is compiled out (Release builds).

**Performance notes.** Backend is the platform aligned heap (`_aligned_malloc` / `aligned_alloc`); mimalloc is the planned replacement behind the same interface. Tracking costs one sharded spinlock acquisition and a hash-table update per allocation and per free; it is off in shipping builds. The arena's fast path is an aligned pointer bump with one branch. Chunks default to 64 KB; requests larger than a chunk get a dedicated chunk.
