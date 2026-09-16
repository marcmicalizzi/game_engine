#pragma once

// FixedVector: a contiguous sequence with a compile-time capacity and no heap storage.
// push_back beyond capacity is a verified failure; use try_push_back where overflow is
// expected and handled. The object is sizeof(SizeType) plus N * sizeof(T), padded.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>

#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class T, usize N, class SizeType = u32>
class FixedVector {
  static_assert(N > 0, "FixedVector: N must be at least 1");
  static_assert(std::is_unsigned_v<SizeType>, "FixedVector: SizeType must be an unsigned integer");
  static_assert(N <= std::numeric_limits<SizeType>::max(), "FixedVector: N exceeds SizeType");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "FixedVector: T must be nothrow move constructible");

 public:
  using value_type = T;
  using size_type = SizeType;
  using difference_type = isize;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;

  static constexpr size_type capacity() noexcept { return static_cast<size_type>(N); }
  static constexpr size_type max_size() noexcept { return capacity(); }

  FixedVector() noexcept = default;
  FixedVector(std::initializer_list<T> init) { assign(init.begin(), init.end()); }
  template <class InputIt>
  FixedVector(InputIt first, InputIt last) {
    assign(first, last);
  }
  FixedVector(const FixedVector& other) { assign(other.begin(), other.end()); }
  FixedVector(FixedVector&& other) noexcept {
    containers::detail::relocate_n(other.data(), other.size_, data());
    size_ = other.size_;
    other.size_ = 0;
  }
  FixedVector& operator=(const FixedVector& other) {
    if (this != &other) assign(other.begin(), other.end());
    return *this;
  }
  FixedVector& operator=(FixedVector&& other) noexcept {
    if (this != &other) {
      clear();
      containers::detail::relocate_n(other.data(), other.size_, data());
      size_ = other.size_;
      other.size_ = 0;
    }
    return *this;
  }
  ~FixedVector() { clear(); }

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  bool full() const noexcept { return size_ == capacity(); }
  void clear() noexcept {
    containers::detail::destroy_n(data(), size_);
    size_ = 0;
  }

  T& operator[](size_type i) noexcept {
    ENGINE_ASSERT(i < size_, "FixedVector: index out of range");
    return data()[i];
  }
  const T& operator[](size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "FixedVector: index out of range");
    return data()[i];
  }
  T& front() noexcept { return (*this)[0]; }
  const T& front() const noexcept { return (*this)[0]; }
  T& back() noexcept { return (*this)[size_ - 1]; }
  const T& back() const noexcept { return (*this)[size_ - 1]; }
  T* data() noexcept { return storage_.elems; }
  const T* data() const noexcept { return storage_.elems; }

  iterator begin() noexcept { return data(); }
  iterator end() noexcept { return data() + size_; }
  const_iterator begin() const noexcept { return data(); }
  const_iterator end() const noexcept { return data() + size_; }

  operator std::span<T>() noexcept { return {data(), size_}; }
  operator std::span<const T>() const noexcept { return {data(), size_}; }

  template <class... Args>
  T& emplace_back(Args&&... args) {
    ENGINE_VERIFY(size_ < capacity(), "FixedVector: capacity exceeded");
    T* p = std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return *p;
  }
  T& push_back(const T& value) { return emplace_back(value); }
  T& push_back(T&& value) { return emplace_back(std::move(value)); }

  // Returns false (and does nothing) when full.
  template <class... Args>
  bool try_emplace_back(Args&&... args) {
    if (size_ == capacity()) return false;
    std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return true;
  }
  bool try_push_back(const T& value) { return try_emplace_back(value); }
  bool try_push_back(T&& value) { return try_emplace_back(std::move(value)); }

  void pop_back() noexcept {
    ENGINE_ASSERT(size_ > 0, "FixedVector::pop_back on empty vector");
    --size_;
    std::destroy_at(data() + size_);
  }

  iterator erase_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "FixedVector::erase_at: position out of range");
    containers::detail::close_hole(data(), size_, pos);
    --size_;
    return data() + pos;
  }
  iterator erase(const_iterator pos) noexcept {
    return erase_at(static_cast<size_type>(pos - data()));
  }
  iterator erase_unordered_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "FixedVector::erase_unordered_at: position out of range");
    const size_type last = size_ - 1;
    if (pos != last) data()[pos] = std::move(data()[last]);
    std::destroy_at(data() + last);
    --size_;
    return data() + pos;
  }

  void resize(size_type n) {
    ENGINE_VERIFY(n <= capacity(), "FixedVector::resize: capacity exceeded");
    if (n < size_) {
      containers::detail::destroy_n(data() + n, size_ - n);
    } else {
      for (size_type i = size_; i < n; ++i)
        std::construct_at(data() + i);
    }
    size_ = n;
  }

  template <class InputIt>
  void assign(InputIt first, InputIt last) {
    clear();
    for (; first != last; ++first)
      emplace_back(*first);
  }

  friend bool operator==(const FixedVector& a, const FixedVector& b) {
    if (a.size_ != b.size_) return false;
    for (size_type i = 0; i < a.size_; ++i) {
      if (!(a.data()[i] == b.data()[i])) return false;
    }
    return true;
  }

 private:
  union Storage {
    T elems[N];
    Storage() noexcept {}
    ~Storage() {}
  };

  size_type size_ = 0;
  Storage storage_;
};

}  // namespace engine
