#pragma once

// Allocator policies for containers.
//
// Containers take an allocator policy as a template parameter and hold it with
// ENGINE_NO_UNIQUE_ADDRESS, so the default (global heap) costs no bytes per container and an
// arena-backed container costs one pointer. A policy is any type with:
//
//     void* allocate(usize bytes, usize align);
//     void  deallocate(void* p, usize bytes, usize align) noexcept;
//
// allocate never returns nullptr (it aborts on exhaustion), and deallocate accepts nullptr.

#include <core/base/types.h>
#include <core/memory/memory.h>

#include <concepts>

namespace engine::mem {

template <class A>
concept AllocatorPolicy = requires(A a, void* p, usize n) {
  { a.allocate(n, n) } -> std::same_as<void*>;
  { a.deallocate(p, n, n) } noexcept;
};

// The global tagged heap. Empty; folds into padding.
struct DefaultAlloc {
  static void* allocate(usize bytes, usize align) { return mem::allocate(bytes, align); }
  static void deallocate(void* p, usize bytes, usize align) noexcept {
    mem::deallocate(p, bytes, align);
  }
  constexpr bool operator==(const DefaultAlloc&) const noexcept = default;
};

static_assert(AllocatorPolicy<DefaultAlloc>);

}  // namespace engine::mem
