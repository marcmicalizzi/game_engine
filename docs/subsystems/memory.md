# memory (core)

**Purpose.** The allocation front door and accounting for the whole engine (ADR-0019). Every heap allocation in engine code goes through `mem::allocate` / `mem::deallocate`, directly or through an allocator policy, so footprint is measurable: a global allocation counter, total bytes live and peak, and per-tag bytes live and peak in development builds. Also provides `Arena`, a bump allocator for scratch and per-frame memory.

**Owned data.** The tag registry (fixed budget of `k_max_tags = 256` names), global and per-tag statistics, and, when `ENGINE_MEMORY_TRACKING` is on, a sharded pointer-to-tag table so a free is credited to the tag that allocated, wherever it happens.

**Invariants.**
- `allocate` never returns null (it aborts); `try_allocate` may. `deallocate(nullptr, ...)` is a no-op. Zero-byte requests are rounded up to one byte in both directions.
- With tracking on, every pointer passed to `deallocate` was returned by `allocate` and the byte count matches (both asserted in debug).
- `total_stats().bytes_current` returns to its previous value after every allocation is freed (tested single- and multi-threaded).
- `Arena::reset()` reaches steady state: after the first pass no further heap allocations occur.

**Public API.**
- `core/memory/memory.h`: `TagId`, `register_tag`, `tag_name`, `current_tag`, `TagScope`, `Stats`, `stats(tag)`, `total_stats()`, `tracking_enabled()`, `allocation_counter()`, `allocate`, `try_allocate`, `deallocate`.
- `core/memory/allocator.h`: `AllocatorPolicy` concept, `DefaultAlloc` (empty; the global tagged heap).
- `core/memory/arena.h`: `Arena` (allocate, create, create_array, mark/rewind, reset, release, statistics), `ArenaAlloc` policy (one pointer).

**Tagging model.** Tags follow the code path, not the object: `mem::TagScope scope(tag)` on the current thread attributes every allocation made inside it. Containers therefore store no tag. Tags are registered once per module at startup; names are copied and truncated to 47 characters.

**Depends on.** `base`, `hash` (for the tracker's pointer hashing).

**Testing.** `tools/dev.ps1 test -Filter memory`. Per-tag tests skip themselves when tracking is compiled out (Release builds).

**Performance notes.** Backend is the platform aligned heap (`_aligned_malloc` / `aligned_alloc`); mimalloc is the planned replacement behind the same interface. Tracking costs one sharded spinlock acquisition and a hash-table update per allocation and per free; it is off in shipping builds. The arena's fast path is an aligned pointer bump with one branch. Chunks default to 64 KB; requests larger than a chunk get a dedicated chunk.
