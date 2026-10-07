#pragma once

// The engine's allocation front door.
//
// Every heap allocation in engine code goes through mem::allocate / mem::deallocate (directly
// or via an allocator policy) so that footprint is measurable: a global allocation counter for
// the "no allocations in the frame loop" metric, total bytes live and peak, and, when tracking
// is compiled in, bytes live and peak per tag. Tags are attached by call site through a
// thread-local TagScope rather than stored in containers, so accounting costs no bytes per
// object.
//
// Backend: the platform's aligned heap for now; mimalloc is a planned replacement behind the
// same interface (ADR-0012).

#include <core/base/macros.h>
#include <core/base/types.h>

namespace engine::mem {

struct TagId {
  u16 value = 0;
  constexpr bool operator==(const TagId&) const noexcept = default;
};

// Tags are a small, fixed set registered at startup by each module. Raise the constant if a
// project needs more; it is a compile-time budget, not a silent limit (register_tag verifies).
inline constexpr usize k_max_tags = 256;
inline constexpr TagId k_untagged{0};

// Registers a tag by name or returns the existing id for that name. Names are copied and
// truncated to 47 characters. Cold path; call once per module.
TagId register_tag(const char* name);
const char* tag_name(TagId tag) noexcept;
u16 tag_count() noexcept;

// The tag attributed to allocations on the current thread.
TagId current_tag() noexcept;

class TagScope {
 public:
  explicit TagScope(TagId tag) noexcept;
  ~TagScope();
  ENGINE_NON_COPYABLE(TagScope);

 private:
  TagId previous_;
};

struct Stats {
  u64 bytes_current = 0;
  u64 bytes_peak = 0;
  u64 allocation_count = 0;
  u64 free_count = 0;
};

// Per-tag stats are maintained only when tracking is compiled in (ENGINE_MEMORY_TRACKING,
// on in Debug and RelWithDebInfo); otherwise they read as zero. Totals are always maintained.
Stats stats(TagId tag) noexcept;
Stats total_stats() noexcept;
bool tracking_enabled() noexcept;

// Monotonically increasing count of allocations since startup. Snapshot it around a frame to
// count allocations inside the frame.
u64 allocation_counter() noexcept;
// The allocations the calling thread has made since it started, in every build (tracking or not)
// and whatever tag is current. What a frame loop's metric counts: snapshot it around the frame's
// work on the frame's thread, and the job workers' allocations — which `allocation_counter` sees
// — stay out of it (docs/subsystems/renderer.md, "What a frame waits for").
u64 allocations_on_thread() noexcept;

// `align` must be a power of two. Zero-byte requests are rounded up to one byte.
[[nodiscard]] void* allocate(usize bytes, usize align);               // aborts on failure
[[nodiscard]] void* try_allocate(usize bytes, usize align) noexcept;  // nullptr on failure
void deallocate(void* p, usize bytes, usize align) noexcept;          // nullptr is a no-op

}  // namespace engine::mem
