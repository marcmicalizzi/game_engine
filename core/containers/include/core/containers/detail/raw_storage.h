#pragma once

// Low-level helpers shared by the engine containers: element relocation and hole open/close
// for contiguous storage. Allocation itself goes through an allocator policy
// (core/memory/allocator.h). Everything here assumes engine types do not throw from
// constructors or assignment.

#include <core/base/types.h>

#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace engine::containers::detail {

constexpr usize align_up(usize value, usize alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr usize max_align(usize a, usize b) noexcept { return a > b ? a : b; }

// Conservative: trivially copyable types can be moved with memcpy/memmove without running
// constructors or destructors. This will become an opt-in trait for types that are
// relocatable but not trivially copyable.
template <class T>
inline constexpr bool is_trivially_relocatable_v = std::is_trivially_copyable_v<T>;

template <class T>
void destroy_n([[maybe_unused]] T* p, [[maybe_unused]] usize n) noexcept {
  if constexpr (!std::is_trivially_destructible_v<T>) {
    for (usize i = 0; i < n; ++i) {
      std::destroy_at(p + i);
    }
  }
}

// Moves n elements from src to non-overlapping uninitialized dst and destroys the sources.
template <class T>
void relocate_n(T* src, usize n, T* dst) noexcept {
  if (n == 0) return;
  if constexpr (is_trivially_relocatable_v<T>) {
    std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), n * sizeof(T));
  } else {
    for (usize i = 0; i < n; ++i) {
      std::construct_at(dst + i, std::move(src[i]));
      std::destroy_at(src + i);
    }
  }
}

// Opens an uninitialized slot at index i in a live range of n elements by shifting [i, n)
// to [i + 1, n + 1). Storage for n + 1 elements must exist. Slot i is left uninitialized.
template <class T>
void open_hole(T* data, usize n, usize i) noexcept {
  if (i == n) return;
  if constexpr (is_trivially_relocatable_v<T>) {
    std::memmove(static_cast<void*>(data + i + 1), static_cast<const void*>(data + i),
                 (n - i) * sizeof(T));
  } else {
    std::construct_at(data + n, std::move(data[n - 1]));
    for (usize k = n - 1; k > i; --k) {
      data[k] = std::move(data[k - 1]);
    }
    std::destroy_at(data + i);
  }
}

// Removes the live element at index i from a range of n elements by shifting [i + 1, n) to
// [i, n - 1). Slot n - 1 is left uninitialized.
template <class T>
void close_hole(T* data, usize n, usize i) noexcept {
  if constexpr (is_trivially_relocatable_v<T>) {
    if (i + 1 < n) {
      std::memmove(static_cast<void*>(data + i), static_cast<const void*>(data + i + 1),
                   (n - i - 1) * sizeof(T));
    }
  } else {
    for (usize k = i; k + 1 < n; ++k) {
      data[k] = std::move(data[k + 1]);
    }
    std::destroy_at(data + n - 1);
  }
}

// Growth policy shared by the contiguous containers: 1.5x from a floor of four, clamped.
template <class SizeType>
constexpr SizeType grow_capacity(SizeType current, SizeType minimum, SizeType maximum) noexcept {
  usize cap = current < 4 ? 4 : static_cast<usize>(current) + static_cast<usize>(current) / 2;
  if (cap < minimum) cap = minimum;
  if (cap > maximum) cap = maximum;
  return static_cast<SizeType>(cap);
}

}  // namespace engine::containers::detail
