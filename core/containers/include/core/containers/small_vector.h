#pragma once

// SmallVector: a contiguous sequence with inline storage for N elements and heap storage
// beyond. data() is branch-free (the pointer always points at the live buffer). Use it for
// sequences that are usually small: per-entity lists, argument packs, scratch buffers.
//
// The object is 16 bytes plus N * sizeof(T). Moving a SmallVector that is inline moves its
// elements; moving one on the heap steals the pointer.

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

template <class T, usize N, class SizeType = u32, class Alloc = mem::DefaultAlloc>
class SmallVector {
  static_assert(N > 0, "SmallVector: N must be at least 1");
  static_assert(std::is_unsigned_v<SizeType>, "SmallVector: SizeType must be an unsigned integer");
  static_assert(N <= std::numeric_limits<SizeType>::max(), "SmallVector: N exceeds SizeType");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "SmallVector: T must be nothrow move constructible");
  static_assert(mem::AllocatorPolicy<Alloc>, "SmallVector: Alloc must satisfy AllocatorPolicy");

 public:
  using value_type = T;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;

  static constexpr size_type inline_capacity() noexcept { return static_cast<size_type>(N); }
  static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }

  // --- construction -----------------------------------------------------------------------

  SmallVector() noexcept : data_(inline_ptr()) {}
  explicit SmallVector(Alloc alloc) noexcept : data_(inline_ptr()), alloc_(std::move(alloc)) {}
  explicit SmallVector(size_type count) : SmallVector() { resize(count); }
  SmallVector(size_type count, const T& value) : SmallVector() { assign(count, value); }
  SmallVector(std::initializer_list<T> init) : SmallVector() { assign(init.begin(), init.end()); }
  template <class InputIt>
  SmallVector(InputIt first, InputIt last) : SmallVector() {
    assign(first, last);
  }

  SmallVector(const SmallVector& other) : data_(inline_ptr()), alloc_(other.alloc_) {
    assign(other.begin(), other.end());
  }
  SmallVector(SmallVector&& other) noexcept : data_(inline_ptr()), alloc_(std::move(other.alloc_)) {
    steal(other);
  }
  SmallVector& operator=(const SmallVector& other) {
    if (this != &other) {
      alloc_ = other.alloc_;
      assign(other.begin(), other.end());
    }
    return *this;
  }
  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this != &other) {
      release();
      alloc_ = std::move(other.alloc_);
      steal(other);
    }
    return *this;
  }
  SmallVector& operator=(std::initializer_list<T> init) {
    assign(init.begin(), init.end());
    return *this;
  }
  ~SmallVector() { release(); }

  void swap(SmallVector& other) noexcept {
    SmallVector tmp(std::move(other));
    other = std::move(*this);
    *this = std::move(tmp);
  }
  friend void swap(SmallVector& a, SmallVector& b) noexcept { a.swap(b); }

  // --- capacity ---------------------------------------------------------------------------

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_type capacity() const noexcept { return capacity_; }
  bool is_inline() const noexcept { return data_ == inline_ptr(); }
  const Alloc& get_allocator() const noexcept { return alloc_; }

  void reserve(size_type n) {
    if (n > capacity_) reallocate(n);
  }
  void shrink_to_fit() {
    if (is_inline() || capacity_ == size_) return;
    if (size_ <= N) {
      T* old = data_;
      const size_type old_capacity = capacity_;
      containers::detail::relocate_n(old, size_, inline_ptr());
      alloc_.deallocate(old, sizeof(T) * old_capacity, alignof(T));
      data_ = inline_ptr();
      capacity_ = inline_capacity();
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
    ENGINE_ASSERT(i < size_, "SmallVector: index out of range");
    return data_[i];
  }
  const T& operator[](size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "SmallVector: index out of range");
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
    ENGINE_VERIFY(size_ < max_size(), "SmallVector: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1));
    T* p = std::construct_at(data_ + size_, std::forward<Args>(args)...);
    ++size_;
    return *p;
  }
  T& push_back(const T& value) { return emplace_back(value); }
  T& push_back(T&& value) { return emplace_back(std::move(value)); }

  void pop_back() noexcept {
    ENGINE_ASSERT(size_ > 0, "SmallVector::pop_back on empty vector");
    --size_;
    std::destroy_at(data_ + size_);
  }

  // Inserts before `pos` (by index) with an O(n) shift.
  template <class... Args>
  iterator emplace(size_type pos, Args&&... args) {
    ENGINE_ASSERT(pos <= size_, "SmallVector::emplace: position out of range");
    ENGINE_VERIFY(size_ < max_size(), "SmallVector: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1));
    containers::detail::open_hole(data_, size_, pos);
    std::construct_at(data_ + pos, std::forward<Args>(args)...);
    ++size_;
    return data_ + pos;
  }
  iterator insert(size_type pos, const T& value) { return emplace(pos, value); }
  iterator insert(size_type pos, T&& value) { return emplace(pos, std::move(value)); }
  iterator insert(const_iterator pos, const T& value) { return emplace(index_of(pos), value); }
  iterator insert(const_iterator pos, T&& value) {
    return emplace(index_of(pos), std::move(value));
  }

  // Order-preserving erase, O(n).
  iterator erase(const_iterator pos) noexcept { return erase_at(index_of(pos)); }
  iterator erase_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "SmallVector::erase_at: position out of range");
    containers::detail::close_hole(data_, size_, pos);
    --size_;
    return data_ + pos;
  }
  iterator erase(const_iterator first, const_iterator last) noexcept {
    const size_type b = index_of(first);
    const size_type e = index_of(last);
    ENGINE_ASSERT(b <= e && e <= size_, "SmallVector::erase: bad range");
    const size_type count = e - b;
    for (size_type k = b; k + count < size_; ++k)
      data_[k] = std::move(data_[k + count]);
    containers::detail::destroy_n(data_ + (size_ - count), count);
    size_ -= count;
    return data_ + b;
  }
  // Order-destroying erase, O(1): the last element takes the vacated place.
  iterator erase_unordered(const_iterator pos) noexcept {
    return erase_unordered_at(index_of(pos));
  }
  iterator erase_unordered_at(size_type pos) noexcept {
    ENGINE_ASSERT(pos < size_, "SmallVector::erase_unordered_at: position out of range");
    const size_type last = size_ - 1;
    if (pos != last) data_[pos] = std::move(data_[last]);
    std::destroy_at(data_ + last);
    --size_;
    return data_ + pos;
  }

  // Geometric when it has to reallocate, exact otherwise — `Vector`'s policy, for the reason it is
  // `Vector`'s: a policy the container set documents that one container quietly does not follow is
  // a trap for whoever changes `Vector<T>` to `SmallVector<T, N>` and gets the quadratic back.
  void resize(size_type n) {
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      if (n > capacity_) reallocate(grow(n));
      for (size_type i = size_; i < n; ++i)
        std::construct_at(data_ + i);
    }
    size_ = n;
  }
  void resize(size_type n, const T& value) {
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      if (n > capacity_) reallocate(grow(n));
      for (size_type i = size_; i < n; ++i)
        std::construct_at(data_ + i, value);
    }
    size_ = n;
  }
  // `resize` with the capacity taken exactly; see `Vector::resize_exact`.
  void resize_exact(size_type n) {
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      reserve(n);
      for (size_type i = size_; i < n; ++i)
        std::construct_at(data_ + i);
    }
    size_ = n;
  }
  void resize_exact(size_type n, const T& value) {
    if (n < size_) {
      containers::detail::destroy_n(data_ + n, size_ - n);
    } else {
      reserve(n);
      for (size_type i = size_; i < n; ++i)
        std::construct_at(data_ + i, value);
    }
    size_ = n;
  }

  template <class InputIt>
  void assign(InputIt first, InputIt last) {
    clear();
    for (; first != last; ++first)
      emplace_back(*first);
  }
  void assign(size_type count, const T& value) {
    clear();
    reserve(count);
    for (size_type i = 0; i < count; ++i)
      std::construct_at(data_ + i, value);
    size_ = count;
  }
  template <class InputIt>
  void append(InputIt first, InputIt last) {
    for (; first != last; ++first)
      emplace_back(*first);
  }
  void append(std::span<const T> items) {
    reserve(static_cast<size_type>(size_ + items.size()));
    for (const T& item : items)
      std::construct_at(data_ + size_++, item);
  }

  // --- comparison -------------------------------------------------------------------------

  friend bool operator==(const SmallVector& a, const SmallVector& b) {
    if (a.size_ != b.size_) return false;
    for (size_type i = 0; i < a.size_; ++i) {
      if (!(a.data_[i] == b.data_[i])) return false;
    }
    return true;
  }

 private:
  union Storage {
    T elems[N];
    Storage() noexcept {}
    ~Storage() {}
  };

  T* inline_ptr() noexcept { return inline_.elems; }
  const T* inline_ptr() const noexcept { return inline_.elems; }

  size_type index_of(const_iterator pos) const noexcept {
    ENGINE_ASSERT(pos >= data_ && pos <= data_ + size_,
                  "SmallVector: iterator not from this vector");
    return static_cast<size_type>(pos - data_);
  }

  size_type grow(size_type minimum) const noexcept {
    return containers::detail::grow_capacity(capacity_, minimum, max_size());
  }

  void reallocate(size_type new_capacity) {
    ENGINE_ASSERT(new_capacity >= size_, "SmallVector::reallocate: capacity too small");
    T* new_data = static_cast<T*>(alloc_.allocate(sizeof(T) * new_capacity, alignof(T)));
    containers::detail::relocate_n(data_, size_, new_data);
    if (!is_inline()) alloc_.deallocate(data_, sizeof(T) * capacity_, alignof(T));
    data_ = new_data;
    capacity_ = new_capacity;
  }

  // Precondition: *this holds no elements and owns no heap block.
  void steal(SmallVector& other) noexcept {
    if (other.is_inline()) {
      containers::detail::relocate_n(other.data_, other.size_, inline_ptr());
      data_ = inline_ptr();
      capacity_ = inline_capacity();
    } else {
      data_ = other.data_;
      capacity_ = other.capacity_;
      other.data_ = other.inline_ptr();
      other.capacity_ = inline_capacity();
    }
    size_ = other.size_;
    other.size_ = 0;
  }

  void release() noexcept {
    clear();
    if (!is_inline()) {
      alloc_.deallocate(data_, sizeof(T) * capacity_, alignof(T));
      data_ = inline_ptr();
      capacity_ = inline_capacity();
    }
  }

  T* data_;
  size_type size_ = 0;
  size_type capacity_ = inline_capacity();
  Storage inline_;
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine
