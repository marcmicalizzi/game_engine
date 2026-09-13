#pragma once

// FlatSet: a sorted set over a single contiguous allocation. Iterators are plain pointers to
// const Key, so every standard algorithm works on it directly. The container object is 16
// bytes with the default 32-bit size type, an empty comparator, and the default allocator.
//
// Complexity and guidance are the same as FlatMap: O(log n) lookup, O(n) insert and erase,
// ideal for small or read-mostly sets; use HashSet under heavy churn at large sizes.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>
#include <core/memory/allocator.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class Key, class Compare = std::less<>, class SizeType = u32,
          class Alloc = mem::DefaultAlloc>
class FlatSet {
  static_assert(std::is_unsigned_v<SizeType>, "FlatSet: SizeType must be an unsigned integer");
  static_assert(std::is_nothrow_move_constructible_v<Key>,
                "FlatSet: Key must be nothrow move constructible");
  static_assert(mem::AllocatorPolicy<Alloc>, "FlatSet: Alloc must satisfy AllocatorPolicy");

 public:
  using key_type = Key;
  using value_type = Key;
  using key_compare = Compare;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using iterator = const Key*;
  using const_iterator = const Key*;

  static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }

  // --- construction -----------------------------------------------------------------------

  FlatSet() noexcept = default;
  explicit FlatSet(Alloc alloc) noexcept : alloc_(std::move(alloc)) {}
  explicit FlatSet(Compare comp, Alloc alloc = Alloc{}) noexcept
      : comp_(std::move(comp)), alloc_(std::move(alloc)) {}

  FlatSet(const FlatSet& other) : comp_(other.comp_), alloc_(other.alloc_) { copy_from(other); }
  FlatSet(FlatSet&& other) noexcept
      : data_(other.data_),
        size_(other.size_),
        capacity_(other.capacity_),
        comp_(std::move(other.comp_)),
        alloc_(std::move(other.alloc_)) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }
  FlatSet& operator=(const FlatSet& other) {
    if (this != &other) {
      release();
      comp_ = other.comp_;
      alloc_ = other.alloc_;
      copy_from(other);
    }
    return *this;
  }
  FlatSet& operator=(FlatSet&& other) noexcept {
    if (this != &other) {
      release();
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      comp_ = std::move(other.comp_);
      alloc_ = std::move(other.alloc_);
      other.data_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }
  ~FlatSet() { release(); }

  void swap(FlatSet& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(comp_, other.comp_);
    std::swap(alloc_, other.alloc_);
  }
  friend void swap(FlatSet& a, FlatSet& b) noexcept { a.swap(b); }

  // --- capacity ---------------------------------------------------------------------------

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_type capacity() const noexcept { return capacity_; }
  const Alloc& get_allocator() const noexcept { return alloc_; }

  void reserve(size_type n) {
    if (n > capacity_) reallocate(n, k_no_hole);
  }
  void shrink_to_fit() {
    if (capacity_ == size_) return;
    if (size_ == 0) {
      release();
    } else {
      reallocate(size_, k_no_hole);
    }
  }
  void clear() noexcept {
    containers::detail::destroy_n(data_, size_);
    size_ = 0;
  }

  // --- lookup -----------------------------------------------------------------------------

  template <class K>
  size_type lower_bound_index(const K& key) const noexcept {
    size_type lo = 0;
    size_type hi = size_;
    while (lo < hi) {
      const size_type mid = lo + (hi - lo) / 2;
      if (comp_(data_[mid], key)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }
  template <class K>
  size_type upper_bound_index(const K& key) const noexcept {
    size_type lo = 0;
    size_type hi = size_;
    while (lo < hi) {
      const size_type mid = lo + (hi - lo) / 2;
      if (comp_(key, data_[mid])) {
        hi = mid;
      } else {
        lo = mid + 1;
      }
    }
    return lo;
  }
  template <class K>
  bool find_index(const K& key, size_type& out_index) const noexcept {
    out_index = lower_bound_index(key);
    return out_index < size_ && !comp_(key, data_[out_index]);
  }

  template <class K>
  const_iterator find(const K& key) const noexcept {
    size_type i;
    return find_index(key, i) ? data_ + i : end();
  }
  template <class K>
  bool contains(const K& key) const noexcept {
    size_type i;
    return find_index(key, i);
  }
  template <class K>
  size_type count(const K& key) const noexcept {
    return contains(key) ? 1 : 0;
  }
  template <class K>
  const_iterator lower_bound(const K& key) const noexcept {
    return data_ + lower_bound_index(key);
  }
  template <class K>
  const_iterator upper_bound(const K& key) const noexcept {
    return data_ + upper_bound_index(key);
  }

  // --- element access ---------------------------------------------------------------------

  const Key& at(size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "FlatSet::at: index out of range");
    return data_[i];
  }
  const Key& front() const noexcept { return at(0); }
  const Key& back() const noexcept { return at(size_ - 1); }
  std::span<const Key> keys() const noexcept { return {data_, size_}; }
  const Compare& key_comp() const noexcept { return comp_; }

  const_iterator begin() const noexcept { return data_; }
  const_iterator end() const noexcept { return data_ + size_; }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  // --- modifiers --------------------------------------------------------------------------

  // Inserts Key(key) if absent. Returns the position and whether an insertion happened.
  template <class K>
  std::pair<const_iterator, bool> insert(K&& key) {
    size_type i;
    if (find_index(key, i)) return {data_ + i, false};
    emplace_at(i, Key(std::forward<K>(key)));
    return {data_ + i, true};
  }

  template <class K>
  size_type erase(const K& key) noexcept {
    size_type i;
    if (!find_index(key, i)) return 0;
    erase_at(i);
    return 1;
  }
  const_iterator erase(const_iterator pos) noexcept {
    const auto i = static_cast<size_type>(pos - data_);
    erase_at(i);
    return data_ + i;
  }
  void erase_at(size_type i) noexcept {
    ENGINE_ASSERT(i < size_, "FlatSet::erase_at: index out of range");
    containers::detail::close_hole(data_, size_, i);
    --size_;
  }

  template <class K>
  std::optional<Key> extract(const K& key) noexcept {
    size_type i;
    if (!find_index(key, i)) return std::nullopt;
    std::optional<Key> out(std::in_place, std::move(data_[i]));
    erase_at(i);
    return out;
  }

  // Appends without searching. Precondition: key is greater than the current last key.
  template <class K>
  const Key& append_sorted(K&& key) {
    ENGINE_ASSERT(size_ == 0 || comp_(data_[size_ - 1], key),
                  "FlatSet::append_sorted: keys must be appended in strictly increasing order");
    ENGINE_VERIFY(size_ < max_size(), "FlatSet: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1), k_no_hole);
    std::construct_at(data_ + size_, std::forward<K>(key));
    ++size_;
    return data_[size_ - 1];
  }

  // Inserts a range in O((n + m) log(n + m)). Duplicates collapse to one element.
  template <class InputIt>
  void insert_bulk(InputIt first, InputIt last) {
    const size_type old_size = size_;
    for (; first != last; ++first) {
      ENGINE_VERIFY(size_ < max_size(), "FlatSet: size_type overflow");
      if (size_ == capacity_) reallocate(grow(size_ + 1), k_no_hole);
      std::construct_at(data_ + size_, *first);
      ++size_;
    }
    if (size_ == old_size) return;
    std::sort(data_, data_ + size_, comp_);
    // Collapse runs of equal keys in place, keeping the first of each run.
    size_type out = 1;
    for (size_type k = 1; k < size_; ++k) {
      if (comp_(data_[out - 1], data_[k])) {
        if (out != k) data_[out] = std::move(data_[k]);
        ++out;
      }
    }
    containers::detail::destroy_n(data_ + out, size_ - out);
    size_ = out;
  }
  template <class Range>
  void insert_bulk(Range&& range) {
    insert_bulk(std::begin(range), std::end(range));
  }

  // --- comparison -------------------------------------------------------------------------

  friend bool operator==(const FlatSet& a, const FlatSet& b) {
    return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
  }

 private:
  static constexpr size_type k_no_hole = std::numeric_limits<size_type>::max();

  static constexpr usize block_bytes(size_type capacity) noexcept {
    return static_cast<usize>(capacity) * sizeof(Key);
  }

  size_type grow(size_type minimum) const noexcept {
    return containers::detail::grow_capacity(capacity_, minimum, max_size());
  }

  void reallocate(size_type new_capacity, size_type hole) {
    namespace d = containers::detail;
    ENGINE_ASSERT(new_capacity >= size_ + (hole == k_no_hole ? 0 : 1),
                  "FlatSet::reallocate: capacity too small");
    Key* new_data = static_cast<Key*>(alloc_.allocate(block_bytes(new_capacity), alignof(Key)));
    if (hole == k_no_hole) {
      d::relocate_n(data_, size_, new_data);
    } else {
      d::relocate_n(data_, hole, new_data);
      d::relocate_n(data_ + hole, size_ - hole, new_data + hole + 1);
    }
    alloc_.deallocate(data_, block_bytes(capacity_), alignof(Key));
    data_ = new_data;
    capacity_ = new_capacity;
  }

  void emplace_at(size_type i, Key&& key) {
    ENGINE_ASSERT(i <= size_, "FlatSet::emplace_at: index out of range");
    ENGINE_VERIFY(size_ < max_size(), "FlatSet: size_type overflow");
    if (size_ == capacity_) {
      reallocate(grow(size_ + 1), i);
    } else {
      containers::detail::open_hole(data_, size_, i);
    }
    std::construct_at(data_ + i, std::move(key));
    ++size_;
  }

  void copy_from(const FlatSet& other) {
    if (other.size_ == 0) return;
    data_ = static_cast<Key*>(alloc_.allocate(block_bytes(other.size_), alignof(Key)));
    capacity_ = other.size_;
    for (size_type i = 0; i < other.size_; ++i) {
      std::construct_at(data_ + i, other.data_[i]);
    }
    size_ = other.size_;
  }

  void release() noexcept {
    clear();
    alloc_.deallocate(data_, block_bytes(capacity_), alignof(Key));
    data_ = nullptr;
    capacity_ = 0;
  }

  Key* data_ = nullptr;
  size_type size_ = 0;
  size_type capacity_ = 0;
  ENGINE_NO_UNIQUE_ADDRESS Compare comp_{};
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine
