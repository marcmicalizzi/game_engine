#pragma once

// Vector: a heap-only contiguous sequence with a 16-byte object (pointer + two 32-bit
// counts by default). Prefer SmallVector when the common size is known and small; use Vector
// when elements are many, or when the element type is incomplete at the point of declaration
// (recursive types such as JsonValue), which inline storage cannot support.
//
// The element type is not checked at class scope, so `struct Node { Vector<Node> children; }`
// is well-formed; the checks run when member functions are instantiated.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>
#include <core/memory/allocator.h>

#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class T, class SizeType = u32, class Alloc = mem::DefaultAlloc>
class Vector {
  static_assert(std::is_unsigned_v<SizeType>, "Vector: SizeType must be an unsigned integer");
  static_assert(mem::AllocatorPolicy<Alloc>, "Vector: Alloc must satisfy AllocatorPolicy");

 public:
  using value_type = T;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;

  static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }

  // --- construction -----------------------------------------------------------------------

  Vector() noexcept = default;
  explicit Vector(Alloc alloc) noexcept : alloc_(std::move(alloc)) {}
  explicit Vector(size_type count) { resize(count); }
  Vector(size_type count, const T& value) { assign(count, value); }
  Vector(std::initializer_list<T> init) { assign(init.begin(), init.end()); }
  template <class InputIt>
  Vector(InputIt first, InputIt last) {
    assign(first, last);
  }

  Vector(const Vector& other) : alloc_(other.alloc_) { assign(other.begin(), other.end()); }
  Vector(Vector&& other) noexcept
      : data_(other.data_), size_(other.size_), capacity_(other.capacity_), alloc_(std::move(other.alloc_)) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }
  Vector& operator=(const Vector& other) {
    if (this != &other) {
      alloc_ = other.alloc_;
      assign(other.begin(), other.end());
    }
    return *this;
  }
  Vector& operator=(Vector&& other) noexcept {
    if (this != &other) {
      release();
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      alloc_ = std::move(other.alloc_);
      other.data_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }
  Vector& operator=(std::initializer_list<T> init) {
    assign(init.begin(), init.end());
    return *this;
  }
  ~Vector() { release(); }

  void swap(Vector& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(alloc_, other.alloc_);
  }
  friend void swap(Vector& a, Vector& b) noexcept { a.swap(b); }

  // --- capacity ---------------------------------------------------------------------------

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_type capacity() const noexcept { return capacity_; }
  const Alloc& get_allocator() const noexcept { return alloc_; }

  void reserve(size_type n) {
    if (n > capacity_) reallocate(n);
  }
  void shrink_to_fit() {
    if (capacity_ == size_) return;
    if (size_ == 0) {
      release();
    } else {
      reallocate(size_);
    }
  }
  void clear() noexcept {
    containers::detail::destroy_n(data_, size_);
    size_ = 0;
  }

  // --- element access ---------------------------------------------------------------------

  T& operator[](size_type i) noexcept {
    ENGINE_ASSERT(i < size_, "Vector: index out of range");
    return data_[i];
  }
  const T& operator[](size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "Vector: index out of range");
    return data_[i];
  }
  T& front() noexcept { return (*this)[0]; }
  const T& front() const noexcept { return (*this)[0]; }
  T& back() noexcept { return (*this)[size_ - 1]; }
  const T& back() const noexcept { return (*this)[size_ - 1]; }
  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }

  iterator begin() noexcept { return data_; }
  iterator end() noexcept { return data_ + size_; }
  const_iterator begin() const noexcept { return data_; }
  const_iterator end() const noexcept { return data_ + size_; }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  operator std::span<T>() noexcept { return {data_, size_}; }
  operator std::span<const T>() const noexcept { return {data_, size_}; }

  // --- modifiers --------------------------------------------------------------------------

  template <class... Args>
  T& emplace_back(Args&&... args) {
    check_type();
    ENGINE_VERIFY(size_ < max_size(), "Vector: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1));
    T* p = std::construct_at(data_ + size_, std::forward<Args>(args)...);
    ++size_;
    return *p;
  }
  T& push_back(const T& value) { return emplace_back(value); }
  T& push_back(T&& value) { return emplace_back(std::move(value)); }

  void pop_back() noexcept {
    ENGINE_ASSERT(size_ > 0, "Vector::pop_back on empty vector");
    --size_;
    std::destroy_at(data_ + size_);
  }

  template <class... Args>
  iterator emplace(size_type pos, Args&&... args) {
    check_type();
    ENGINE_ASSERT(pos <= size_, "Vector::emplace: position out of range");
    ENGINE_VERIFY(size_ < max_size(), "Vector: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1));
    containers::detail::open_hole(data_, size_, pos);
    std::construct_at(data_ + pos, std::forward<Args>(args)...);
    ++size_;
    return data_ + pos;
  }
  iterator insert(size_type pos, const T& value) { return emplace(pos, value); }
  iterator insert(size_type pos, T&& value) { return emplace(pos, std::move(value)); }
  iterator insert(const_iterator pos, const T& value) { return emplace(index_of(pos), value); }
  iterator insert(const_iterator pos, T&& value) { return emplace(index_of(pos), std::move(value)); }

  iterator erase(const_iterator pos) noexcept { return erase_at(index_of(pos)); }
  iterator erase_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "Vector::erase_at: position out of range");
    containers::detail::close_hole(data_, size_, pos);
    --size_;
    return data_ + pos;
  }
  iterator erase(const_iterator first, const_iterator last) noexcept {
    const size_type b = index_of(first);
    const size_type e = index_of(last);
    ENGINE_ASSERT(b <= e && e <= size_, "Vector::erase: bad range");
    const size_type count = e - b;
    for (size_type k = b; k + count < size_; ++k) data_[k] = std::move(data_[k + count]);
    containers::detail::destroy_n(data_ + (size_ - count), count);
    size_ -= count;
    return data_ + b;
  }
  iterator erase_unordered(const_iterator pos) noexcept { return erase_unordered_at(index_of(pos)); }
  iterator erase_unordered_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "Vector::erase_unordered_at: position out of range");
    const size_type last = size_ - 1;
    if (pos != last) data_[pos] = std::move(data_[last]);
    std::destroy_at(data_ + last);
    --size_;
    return data_ + pos;
  }

  void resize(size_type n) {
    check_type();
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      reserve(n);
      for (size_type i = size_; i < n; ++i) std::construct_at(data_ + i);
    }
    size_ = n;
  }
  void resize(size_type n, const T& value) {
    check_type();
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      reserve(n);
      for (size_type i = size_; i < n; ++i) std::construct_at(data_ + i, value);
    }
    size_ = n;
  }

  template <class InputIt>
  void assign(InputIt first, InputIt last) {
    clear();
    for (; first != last; ++first) emplace_back(*first);
  }
  void assign(size_type count, const T& value) {
    check_type();
    clear();
    reserve(count);
    for (size_type i = 0; i < count; ++i) std::construct_at(data_ + i, value);
    size_ = count;
  }
  template <class InputIt>
  void append(InputIt first, InputIt last) {
    for (; first != last; ++first) emplace_back(*first);
  }
  void append(std::span<const T> items) {
    check_type();
    reserve(static_cast<size_type>(size_ + items.size()));
    for (const T& item : items) std::construct_at(data_ + size_++, item);
  }

  // --- comparison -------------------------------------------------------------------------

  friend bool operator==(const Vector& a, const Vector& b) {
    if (a.size_ != b.size_) return false;
    for (size_type i = 0; i < a.size_; ++i) {
      if (!(a.data_[i] == b.data_[i])) return false;
    }
    return true;
  }

 private:
  static constexpr void check_type() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>, "Vector: T must be nothrow move constructible");
  }

  size_type index_of(const_iterator pos) const noexcept {
    ENGINE_ASSERT(pos >= data_ && pos <= data_ + size_, "Vector: iterator not from this vector");
    return static_cast<size_type>(pos - data_);
  }

  size_type grow(size_type minimum) const noexcept {
    return containers::detail::grow_capacity(capacity_, minimum, max_size());
  }

  void reallocate(size_type new_capacity) {
    ENGINE_ASSERT(new_capacity >= size_, "Vector::reallocate: capacity too small");
    T* new_data = static_cast<T*>(alloc_.allocate(sizeof(T) * new_capacity, alignof(T)));
    containers::detail::relocate_n(data_, size_, new_data);
    alloc_.deallocate(data_, sizeof(T) * capacity_, alignof(T));
    data_ = new_data;
    capacity_ = new_capacity;
  }

  void release() noexcept {
    clear();
    alloc_.deallocate(data_, sizeof(T) * capacity_, alignof(T));
    data_ = nullptr;
    capacity_ = 0;
  }

  T* data_ = nullptr;
  size_type size_ = 0;
  size_type capacity_ = 0;
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine
