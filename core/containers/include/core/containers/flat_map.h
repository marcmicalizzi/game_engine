#pragma once

// FlatMap: a sorted associative container over a single contiguous allocation.
//
// Keys and values live in separate arrays inside one heap block (keys first, then values),
// so a binary search touches only keys: for 8-byte keys that is eight keys per cache line
// regardless of how large the value type is. The container object itself is 16 bytes with the
// default 32-bit size type, an empty comparator, and the default allocator, so many small maps
// stay cheap.
//
// Complexity: lookup O(log n); insert and erase O(n) element moves; iteration is a linear walk
// in key order. This is the right structure for small maps and for read-mostly maps of any
// size. Under heavy insert/erase churn above a few hundred elements, use HashMap instead
// (docs/plan/11-performance-principles.md section 11.2).
//
// Descended from search_vector, proven in production in the scheduler project; differences:
// SoA layout, a 16-byte header, transparent heterogeneous lookup through the comparator,
// allocation-free extract, and bulk insertion in O(n log n).
//
// Iteration yields std::pair<const Key&, Value&>; write `for (auto [k, v] : map)`.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>
#include <core/containers/detail/soa_iterator.h>
#include <core/memory/allocator.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class Key, class Value, class Compare = std::less<>, class SizeType = u32,
          class Alloc = mem::DefaultAlloc>
class FlatMap {
  static_assert(std::is_unsigned_v<SizeType>, "FlatMap: SizeType must be an unsigned integer");
  static_assert(std::is_nothrow_move_constructible_v<Key>,
                "FlatMap: Key must be nothrow move constructible");
  static_assert(std::is_nothrow_move_constructible_v<Value>,
                "FlatMap: Value must be nothrow move constructible");
  static_assert(mem::AllocatorPolicy<Alloc>, "FlatMap: Alloc must satisfy AllocatorPolicy");

  friend struct containers::detail::SoaAccess;

 public:
  using key_type = Key;
  using mapped_type = Value;
  using key_compare = Compare;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using reference = std::pair<const Key&, Value&>;
  using const_reference = std::pair<const Key&, const Value&>;
  using iterator = containers::detail::SoaIterator<FlatMap, false>;
  using const_iterator = containers::detail::SoaIterator<FlatMap, true>;

  static constexpr size_type max_size() noexcept { return std::numeric_limits<size_type>::max(); }

  // --- construction -----------------------------------------------------------------------

  FlatMap() noexcept = default;
  explicit FlatMap(Alloc alloc) noexcept : alloc_(std::move(alloc)) {}
  explicit FlatMap(Compare comp, Alloc alloc = Alloc{}) noexcept
      : comp_(std::move(comp)), alloc_(std::move(alloc)) {}

  FlatMap(const FlatMap& other) : comp_(other.comp_), alloc_(other.alloc_) { copy_from(other); }
  FlatMap(FlatMap&& other) noexcept
      : keys_(other.keys_),
        size_(other.size_),
        capacity_(other.capacity_),
        comp_(std::move(other.comp_)),
        alloc_(std::move(other.alloc_)) {
    other.keys_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }
  FlatMap& operator=(const FlatMap& other) {
    if (this != &other) {
      release();
      comp_ = other.comp_;
      alloc_ = other.alloc_;
      copy_from(other);
    }
    return *this;
  }
  FlatMap& operator=(FlatMap&& other) noexcept {
    if (this != &other) {
      release();
      keys_ = other.keys_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      comp_ = std::move(other.comp_);
      alloc_ = std::move(other.alloc_);
      other.keys_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }
  ~FlatMap() { release(); }

  void swap(FlatMap& other) noexcept {
    std::swap(keys_, other.keys_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(comp_, other.comp_);
    std::swap(alloc_, other.alloc_);
  }
  friend void swap(FlatMap& a, FlatMap& b) noexcept { a.swap(b); }

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
    containers::detail::destroy_n(keys_, size_);
    containers::detail::destroy_n(values_ptr(), size_);
    size_ = 0;
  }

  // --- lookup -----------------------------------------------------------------------------

  // Index of the first key not less than `key`; equals size() when all keys are less.
  template <class K>
  size_type lower_bound_index(const K& key) const noexcept {
    size_type lo = 0;
    size_type hi = size_;
    while (lo < hi) {
      const size_type mid = lo + (hi - lo) / 2;
      if (comp_(keys_[mid], key)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  // Index of the first key greater than `key`.
  template <class K>
  size_type upper_bound_index(const K& key) const noexcept {
    size_type lo = 0;
    size_type hi = size_;
    while (lo < hi) {
      const size_type mid = lo + (hi - lo) / 2;
      if (comp_(key, keys_[mid])) {
        hi = mid;
      } else {
        lo = mid + 1;
      }
    }
    return lo;
  }

  // Sets `out_index` to the lower bound and returns whether the key is present there.
  template <class K>
  bool find_index(const K& key, size_type& out_index) const noexcept {
    out_index = lower_bound_index(key);
    return out_index < size_ && !comp_(key, keys_[out_index]);
  }

  template <class K>
  iterator find(const K& key) noexcept {
    size_type i;
    return find_index(key, i) ? iterator(this, i) : end();
  }
  template <class K>
  const_iterator find(const K& key) const noexcept {
    size_type i;
    return find_index(key, i) ? const_iterator(this, i) : end();
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

  // Pointer to the mapped value, or nullptr. The engine's replacement for a throwing at().
  template <class K>
  Value* find_value(const K& key) noexcept {
    size_type i;
    return find_index(key, i) ? values_ptr() + i : nullptr;
  }
  template <class K>
  const Value* find_value(const K& key) const noexcept {
    size_type i;
    return find_index(key, i) ? values_ptr() + i : nullptr;
  }

  template <class K>
  iterator lower_bound(const K& key) noexcept {
    return iterator(this, lower_bound_index(key));
  }
  template <class K>
  const_iterator lower_bound(const K& key) const noexcept {
    return const_iterator(this, lower_bound_index(key));
  }
  template <class K>
  iterator upper_bound(const K& key) noexcept {
    return iterator(this, upper_bound_index(key));
  }
  template <class K>
  const_iterator upper_bound(const K& key) const noexcept {
    return const_iterator(this, upper_bound_index(key));
  }

  // --- element access ---------------------------------------------------------------------

  // Inserts a value-initialized mapped value when the key is absent.
  template <class K>
  Value& operator[](K&& key) {
    return try_emplace(std::forward<K>(key)).first->second;
  }

  const Key& key_at(size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "FlatMap::key_at: index out of range");
    return keys_[i];
  }
  Value& value_at(size_type i) noexcept {
    ENGINE_ASSERT(i < size_, "FlatMap::value_at: index out of range");
    return values_ptr()[i];
  }
  const Value& value_at(size_type i) const noexcept {
    ENGINE_ASSERT(i < size_, "FlatMap::value_at: index out of range");
    return values_ptr()[i];
  }

  std::span<const Key> keys() const noexcept { return {keys_, size_}; }
  std::span<Value> values() noexcept { return {values_ptr(), size_}; }
  std::span<const Value> values() const noexcept { return {values_ptr(), size_}; }

  const Compare& key_comp() const noexcept { return comp_; }

  // --- iteration --------------------------------------------------------------------------

  iterator begin() noexcept { return iterator(this, 0); }
  iterator end() noexcept { return iterator(this, size_); }
  const_iterator begin() const noexcept { return const_iterator(this, 0); }
  const_iterator end() const noexcept { return const_iterator(this, size_); }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  // --- modifiers --------------------------------------------------------------------------

  // Inserts Key(key) -> Value(args...) if the key is absent. Returns the position and
  // whether an insertion happened.
  template <class K, class... Args>
  std::pair<iterator, bool> try_emplace(K&& key, Args&&... args) {
    size_type i;
    if (find_index(key, i)) return {iterator(this, i), false};
    emplace_at(i, Key(std::forward<K>(key)), std::forward<Args>(args)...);
    return {iterator(this, i), true};
  }

  std::pair<iterator, bool> insert(const Key& key, const Value& value) {
    return try_emplace(key, value);
  }
  std::pair<iterator, bool> insert(Key&& key, Value&& value) {
    return try_emplace(std::move(key), std::move(value));
  }
  std::pair<iterator, bool> insert(const std::pair<Key, Value>& kv) {
    return try_emplace(kv.first, kv.second);
  }
  std::pair<iterator, bool> insert(std::pair<Key, Value>&& kv) {
    return try_emplace(std::move(kv.first), std::move(kv.second));
  }

  // Inserts or overwrites. Returns the position and whether an insertion (not an assignment)
  // happened.
  template <class K, class M>
  std::pair<iterator, bool> insert_or_assign(K&& key, M&& value) {
    size_type i;
    if (find_index(key, i)) {
      values_ptr()[i] = std::forward<M>(value);
      return {iterator(this, i), false};
    }
    emplace_at(i, Key(std::forward<K>(key)), std::forward<M>(value));
    return {iterator(this, i), true};
  }

  template <class K>
  size_type erase(const K& key) noexcept {
    size_type i;
    if (!find_index(key, i)) return 0;
    erase_at(i);
    return 1;
  }
  // Both iterator kinds are accepted as exact matches so that erase(iterator) is never routed
  // to the templated erase(key) overload.
  iterator erase(iterator pos) noexcept {
    erase_at(pos.index());
    return iterator(this, pos.index());
  }
  iterator erase(const_iterator pos) noexcept {
    erase_at(pos.index());
    return iterator(this, pos.index());
  }
  void erase_at(size_type i) noexcept {
    ENGINE_ASSERT(i < size_, "FlatMap::erase_at: index out of range");
    containers::detail::close_hole(keys_, size_, i);
    containers::detail::close_hole(values_ptr(), size_, i);
    --size_;
  }

  // Removes the entry and returns its value by move, or nullopt.
  template <class K>
  std::optional<Value> extract(const K& key) noexcept {
    size_type i;
    if (!find_index(key, i)) return std::nullopt;
    std::optional<Value> out(std::in_place, std::move(values_ptr()[i]));
    erase_at(i);
    return out;
  }

  // Appends without searching. Precondition: key is greater than the current last key.
  // Use when building a map from already-sorted input.
  template <class K, class... Args>
  Value& append_sorted(K&& key, Args&&... args) {
    ENGINE_ASSERT(size_ == 0 || comp_(keys_[size_ - 1], key),
                  "FlatMap::append_sorted: keys must be appended in strictly increasing order");
    ENGINE_VERIFY(size_ < max_size(), "FlatMap: size_type overflow");
    if (size_ == capacity_) reallocate(grow(size_ + 1), k_no_hole);
    std::construct_at(keys_ + size_, std::forward<K>(key));
    std::construct_at(values_ptr() + size_, std::forward<Args>(args)...);
    ++size_;
    return values_ptr()[size_ - 1];
  }

  // Inserts a range of pair-like elements in O((n + m) log(n + m)) instead of O(m * n).
  // Semantics match insert_or_assign applied in order: later duplicates win, and new entries
  // override existing ones.
  template <class InputIt>
  void insert_bulk(InputIt first, InputIt last) {
    namespace d = containers::detail;
    const size_type old_size = size_;
    for (; first != last; ++first) {
      ENGINE_VERIFY(size_ < max_size(), "FlatMap: size_type overflow");
      if (size_ == capacity_) reallocate(grow(size_ + 1), k_no_hole);
      std::construct_at(keys_ + size_, first->first);
      std::construct_at(values_ptr() + size_, first->second);
      ++size_;
    }
    if (size_ == old_size) return;

    const usize n = size_;
    auto* order =
        static_cast<size_type*>(alloc_.allocate(n * sizeof(size_type), alignof(size_type)));
    for (usize k = 0; k < n; ++k) order[k] = static_cast<size_type>(k);
    std::stable_sort(order, order + n,
                     [this](size_type a, size_type b) { return comp_(keys_[a], keys_[b]); });

    Key* new_keys = static_cast<Key*>(alloc_.allocate(block_bytes(size_), block_align()));
    Value* new_values = values_at(new_keys, size_);
    Value* old_values = values_ptr();
    usize out = 0;
    for (usize k = 0; k < n; ++k) {
      const bool last_of_run = (k + 1 == n) || comp_(keys_[order[k]], keys_[order[k + 1]]);
      if (last_of_run) {
        std::construct_at(new_keys + out, std::move(keys_[order[k]]));
        std::construct_at(new_values + out, std::move(old_values[order[k]]));
        ++out;
      }
    }
    d::destroy_n(keys_, size_);
    d::destroy_n(old_values, size_);
    alloc_.deallocate(keys_, block_bytes(capacity_), block_align());
    alloc_.deallocate(order, n * sizeof(size_type), alignof(size_type));
    keys_ = new_keys;
    capacity_ = size_;
    size_ = static_cast<size_type>(out);
  }
  template <class Range>
  void insert_bulk(Range&& range) {
    insert_bulk(std::begin(range), std::end(range));
  }

  // --- comparison -------------------------------------------------------------------------

  friend bool operator==(const FlatMap& a, const FlatMap& b) {
    if (a.size_ != b.size_) return false;
    for (size_type i = 0; i < a.size_; ++i) {
      if (!(a.keys_[i] == b.keys_[i]) || !(a.values_ptr()[i] == b.values_ptr()[i])) return false;
    }
    return true;
  }

 private:
  static constexpr size_type k_no_hole = std::numeric_limits<size_type>::max();

  static constexpr usize block_align() noexcept {
    return containers::detail::max_align(alignof(Key), alignof(Value));
  }
  static constexpr usize values_offset(size_type capacity) noexcept {
    return containers::detail::align_up(static_cast<usize>(capacity) * sizeof(Key),
                                        alignof(Value));
  }
  static constexpr usize block_bytes(size_type capacity) noexcept {
    return values_offset(capacity) + static_cast<usize>(capacity) * sizeof(Value);
  }
  static Value* values_at(Key* keys, size_type capacity) noexcept {
    void* base = static_cast<void*>(keys);
    void* p = static_cast<std::byte*>(base) + values_offset(capacity);
    return static_cast<Value*>(p);
  }

  Key* keys_ptr() noexcept { return keys_; }
  const Key* keys_ptr() const noexcept { return keys_; }
  Value* values_ptr() noexcept { return capacity_ == 0 ? nullptr : values_at(keys_, capacity_); }
  const Value* values_ptr() const noexcept {
    return capacity_ == 0 ? nullptr : values_at(keys_, capacity_);
  }

  size_type grow(size_type minimum) const noexcept {
    return containers::detail::grow_capacity(capacity_, minimum, max_size());
  }

  // Moves all elements into a fresh block of `new_capacity`. When `hole` is not k_no_hole,
  // elements at and after `hole` are placed one slot later, leaving `hole` uninitialized for
  // the caller to construct into. Combining growth and insertion avoids moving twice.
  void reallocate(size_type new_capacity, size_type hole) {
    namespace d = containers::detail;
    ENGINE_ASSERT(new_capacity >= size_ + (hole == k_no_hole ? 0 : 1),
                  "FlatMap::reallocate: capacity too small");
    Key* new_keys = static_cast<Key*>(alloc_.allocate(block_bytes(new_capacity), block_align()));
    Value* new_values = values_at(new_keys, new_capacity);
    Value* old_values = values_ptr();
    if (hole == k_no_hole) {
      d::relocate_n(keys_, size_, new_keys);
      d::relocate_n(old_values, size_, new_values);
    } else {
      d::relocate_n(keys_, hole, new_keys);
      d::relocate_n(keys_ + hole, size_ - hole, new_keys + hole + 1);
      d::relocate_n(old_values, hole, new_values);
      d::relocate_n(old_values + hole, size_ - hole, new_values + hole + 1);
    }
    alloc_.deallocate(keys_, block_bytes(capacity_), block_align());
    keys_ = new_keys;
    capacity_ = new_capacity;
  }

  template <class... Args>
  void emplace_at(size_type i, Key&& key, Args&&... args) {
    ENGINE_ASSERT(i <= size_, "FlatMap::emplace_at: index out of range");
    ENGINE_VERIFY(size_ < max_size(), "FlatMap: size_type overflow");
    if (size_ == capacity_) {
      reallocate(grow(size_ + 1), i);
    } else {
      containers::detail::open_hole(keys_, size_, i);
      containers::detail::open_hole(values_ptr(), size_, i);
    }
    std::construct_at(keys_ + i, std::move(key));
    std::construct_at(values_ptr() + i, std::forward<Args>(args)...);
    ++size_;
  }

  void copy_from(const FlatMap& other) {
    if (other.size_ == 0) return;
    keys_ = static_cast<Key*>(alloc_.allocate(block_bytes(other.size_), block_align()));
    capacity_ = other.size_;
    Value* dst_values = values_ptr();
    const Value* src_values = other.values_ptr();
    for (size_type i = 0; i < other.size_; ++i) {
      std::construct_at(keys_ + i, other.keys_[i]);
      std::construct_at(dst_values + i, src_values[i]);
    }
    size_ = other.size_;
  }

  void release() noexcept {
    clear();
    alloc_.deallocate(keys_, block_bytes(capacity_), block_align());
    keys_ = nullptr;
    capacity_ = 0;
  }

  Key* keys_ = nullptr;
  size_type size_ = 0;
  size_type capacity_ = 0;
  ENGINE_NO_UNIQUE_ADDRESS Compare comp_{};
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine
