#pragma once

// HashMap: open-addressing hash map with dense SoA storage (see detail/dense_hash_table.h).
//
// Use it for large maps or maps under insert/erase churn. For small or read-mostly maps,
// FlatMap is denser and has no bucket array. The container object is 32 bytes.
//
// Iteration order is insertion order until an erase, which moves the last element into the
// vacated slot. Iterators are index-based and are invalidated by insert and erase; erase(it)
// returns an iterator at the same index, which now refers to the moved-in element.
// Heterogeneous lookup works when both Hasher and KeyEqual are transparent (the defaults are
// for string-like keys: HashMap<std::string, V> accepts std::string_view and const char*).
//
// Iteration yields std::pair<const Key&, Value&>; write `for (auto [k, v] : map)`.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/detail/dense_hash_table.h>
#include <core/containers/detail/soa_iterator.h>
#include <core/hash/hash.h>
#include <core/memory/allocator.h>

#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class Key, class Value, class Hasher = Hash<Key>, class KeyEqual = std::equal_to<>,
          class SizeType = u32, class Alloc = mem::DefaultAlloc>
class HashMap
    : private containers::detail::DenseHashTable<Key, Value, Hasher, KeyEqual, SizeType, Alloc> {
  using Base = containers::detail::DenseHashTable<Key, Value, Hasher, KeyEqual, SizeType, Alloc>;
  static_assert(std::is_nothrow_move_constructible_v<Value>,
                "HashMap: Value must be nothrow move constructible");

  friend struct containers::detail::SoaAccess;

 public:
  using key_type = Key;
  using mapped_type = Value;
  using hasher = Hasher;
  using key_equal = KeyEqual;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using reference = std::pair<const Key&, Value&>;
  using const_reference = std::pair<const Key&, const Value&>;
  using iterator = containers::detail::SoaIterator<HashMap, false>;
  using const_iterator = containers::detail::SoaIterator<HashMap, true>;

  static constexpr size_type max_size() noexcept { return Base::k_max_size; }

  // --- construction -----------------------------------------------------------------------

  HashMap() noexcept = default;
  explicit HashMap(Alloc alloc) noexcept : Base(Hasher{}, KeyEqual{}, std::move(alloc)) {}
  HashMap(Hasher h, KeyEqual eq = KeyEqual{}, Alloc alloc = Alloc{}) noexcept
      : Base(std::move(h), std::move(eq), std::move(alloc)) {}
  HashMap(const HashMap&) = default;
  HashMap(HashMap&&) noexcept = default;
  HashMap& operator=(const HashMap&) = default;
  HashMap& operator=(HashMap&&) noexcept = default;
  ~HashMap() = default;

  void swap(HashMap& other) noexcept { Base::swap(other); }
  friend void swap(HashMap& a, HashMap& b) noexcept { a.swap(b); }

  // --- capacity ---------------------------------------------------------------------------

  size_type size() const noexcept { return this->size_; }
  bool empty() const noexcept { return this->size_ == 0; }
  size_type capacity() const noexcept { return this->dense_capacity_; }
  size_type bucket_count() const noexcept { return this->bucket_count_; }
  const Alloc& get_allocator() const noexcept { return this->alloc_; }

  void reserve(size_type n) { this->reserve_impl(n); }
  void shrink_to_fit() { this->shrink_impl(); }
  void clear() noexcept { this->clear_impl(); }

  // --- lookup -----------------------------------------------------------------------------

  template <class K>
  iterator find(const K& key) noexcept {
    size_type i;
    return this->find_dense(key, i) ? iterator(this, i) : end();
  }
  template <class K>
  const_iterator find(const K& key) const noexcept {
    size_type i;
    return this->find_dense(key, i) ? const_iterator(this, i) : end();
  }
  template <class K>
  bool contains(const K& key) const noexcept {
    size_type i;
    return this->find_dense(key, i);
  }
  template <class K>
  size_type count(const K& key) const noexcept {
    return contains(key) ? 1 : 0;
  }
  template <class K>
  Value* find_value(const K& key) noexcept {
    size_type i;
    return this->find_dense(key, i) ? this->values_ptr() + i : nullptr;
  }
  template <class K>
  const Value* find_value(const K& key) const noexcept {
    size_type i;
    return this->find_dense(key, i) ? this->values_ptr() + i : nullptr;
  }

  // --- element access ---------------------------------------------------------------------

  template <class K>
  Value& operator[](K&& key) {
    return try_emplace(std::forward<K>(key)).first->second;
  }

  const Key& key_at(size_type i) const noexcept {
    ENGINE_ASSERT(i < this->size_, "HashMap::key_at: index out of range");
    return this->keys_[i];
  }
  Value& value_at(size_type i) noexcept {
    ENGINE_ASSERT(i < this->size_, "HashMap::value_at: index out of range");
    return this->values_ptr()[i];
  }
  const Value& value_at(size_type i) const noexcept {
    ENGINE_ASSERT(i < this->size_, "HashMap::value_at: index out of range");
    return this->values_ptr()[i];
  }

  std::span<const Key> keys() const noexcept { return {this->keys_, this->size_}; }
  std::span<Value> values() noexcept { return {this->values_ptr(), this->size_}; }
  std::span<const Value> values() const noexcept { return {this->values_ptr(), this->size_}; }

  const Hasher& hash_function() const noexcept { return this->hasher_; }
  const KeyEqual& key_eq() const noexcept { return this->equal_; }

  // --- iteration --------------------------------------------------------------------------

  iterator begin() noexcept { return iterator(this, 0); }
  iterator end() noexcept { return iterator(this, this->size_); }
  const_iterator begin() const noexcept { return const_iterator(this, 0); }
  const_iterator end() const noexcept { return const_iterator(this, this->size_); }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  // --- modifiers --------------------------------------------------------------------------

  template <class K, class... Args>
  std::pair<iterator, bool> try_emplace(K&& key, Args&&... args) {
    size_type i;
    if (this->find_dense(key, i)) return {iterator(this, i), false};
    this->ensure_room_for_one();
    const u64 h = this->hasher_(key);
    i = this->emplace_new(h, Key(std::forward<K>(key)), std::forward<Args>(args)...);
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

  template <class K, class M>
  std::pair<iterator, bool> insert_or_assign(K&& key, M&& value) {
    size_type i;
    if (this->find_dense(key, i)) {
      this->values_ptr()[i] = std::forward<M>(value);
      return {iterator(this, i), false};
    }
    this->ensure_room_for_one();
    const u64 h = this->hasher_(key);
    i = this->emplace_new(h, Key(std::forward<K>(key)), std::forward<M>(value));
    return {iterator(this, i), true};
  }

  template <class K>
  size_type erase(const K& key) noexcept {
    u32 b;
    if (!this->find_bucket(key, b)) return 0;
    this->erase_bucket(b);
    return 1;
  }
  iterator erase(iterator pos) noexcept {
    erase_at(pos.index());
    return iterator(this, pos.index());
  }
  iterator erase(const_iterator pos) noexcept {
    erase_at(pos.index());
    return iterator(this, pos.index());
  }
  // Removes the element at dense index i. The last element moves into slot i.
  void erase_at(size_type i) noexcept {
    ENGINE_ASSERT(i < this->size_, "HashMap::erase_at: index out of range");
    this->erase_bucket(this->bucket_for_dense(i));
  }

  template <class K>
  std::optional<Value> extract(const K& key) noexcept {
    u32 b;
    if (!this->find_bucket(key, b)) return std::nullopt;
    std::optional<Value> out(std::in_place, std::move(this->values_ptr()[this->buckets_[b].index]));
    this->erase_bucket(b);
    return out;
  }

  template <class InputIt>
  void insert_bulk(InputIt first, InputIt last) {
    for (; first != last; ++first) insert_or_assign(first->first, first->second);
  }
  template <class Range>
  void insert_bulk(Range&& range) {
    insert_bulk(std::begin(range), std::end(range));
  }

  // --- comparison (order-independent) -----------------------------------------------------

  friend bool operator==(const HashMap& a, const HashMap& b) {
    if (a.size() != b.size()) return false;
    for (size_type i = 0; i < a.size(); ++i) {
      const Value* v = b.find_value(a.keys_[i]);
      if (v == nullptr || !(*v == a.values_ptr()[i])) return false;
    }
    return true;
  }
};

}  // namespace engine
