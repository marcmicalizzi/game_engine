#pragma once

// Arena: a bump allocator over a chain of chunks. Allocation is a pointer increment; nothing
// is freed individually. Use it for per-frame scratch, per-task scratch, and build steps whose
// lifetime ends all at once. reset() rewinds to the start and keeps the chunks for reuse, so a
// frame arena reaches steady state with zero heap traffic.
//
// Objects placed in an arena do not have destructors run; store only trivially destructible
// types, or destroy them yourself before reset().

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/memory/allocator.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace engine::mem {

class Arena {
 public:
  static constexpr usize k_default_chunk_bytes = 64 * 1024;

  explicit Arena(usize chunk_bytes = k_default_chunk_bytes) noexcept;
  ~Arena();
  ENGINE_NON_COPYABLE(Arena);
  Arena(Arena&& other) noexcept;
  Arena& operator=(Arena&& other) noexcept;

  // `align` must be a power of two. Requests larger than the chunk size get a dedicated chunk.
  [[nodiscard]] void* allocate(usize bytes, usize align);
  void deallocate(void*, usize, usize) noexcept {}  // arenas free in bulk

  template <class T, class... Args>
    requires std::is_trivially_destructible_v<T>
  T* create(Args&&... args) {
    return std::construct_at(static_cast<T*>(allocate(sizeof(T), alignof(T))),
                             std::forward<Args>(args)...);
  }

  template <class T>
    requires std::is_trivially_destructible_v<T>
  T* create_array(usize count) {
    T* p = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    for (usize i = 0; i < count; ++i) std::construct_at(p + i);
    return p;
  }

  struct Mark {
    void* chunk = nullptr;
    usize used = 0;
  };
  Mark mark() const noexcept;
  void rewind(Mark mark) noexcept;  // everything allocated after the mark is released
  void reset() noexcept;            // rewind to the beginning; chunks are kept
  void release() noexcept;          // free every chunk

  usize bytes_allocated() const noexcept;  // live bytes, including alignment padding
  usize bytes_reserved() const noexcept { return bytes_reserved_; }
  usize chunk_count() const noexcept { return chunk_count_; }

 private:
  struct Chunk {
    Chunk* next;
    usize capacity;  // usable bytes after the header
    usize used;
  };
  static constexpr usize k_chunk_align = 64;
  static constexpr usize k_header_bytes = (sizeof(Chunk) + k_chunk_align - 1) & ~(k_chunk_align - 1);

  static std::byte* data_of(Chunk* c) noexcept;
  Chunk* new_chunk(usize capacity);
  void* allocate_slow(usize bytes, usize align);

  Chunk* first_ = nullptr;
  Chunk* current_ = nullptr;
  usize chunk_bytes_;
  usize bytes_reserved_ = 0;
  usize chunk_count_ = 0;
};

// Policy that routes a container's allocations into an arena. One pointer per container.
struct ArenaAlloc {
  Arena* arena = nullptr;
  void* allocate(usize bytes, usize align) { return arena->allocate(bytes, align); }
  void deallocate(void*, usize, usize) noexcept {}
  constexpr bool operator==(const ArenaAlloc&) const noexcept = default;
};

static_assert(AllocatorPolicy<ArenaAlloc>);

}  // namespace engine::mem
