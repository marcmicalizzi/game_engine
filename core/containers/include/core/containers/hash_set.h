#pragma once

// HashSet: open-addressing hash set with dense storage (see detail/dense_hash_table.h).
// Iterators are plain `const Key*` over the dense array. The container object is 32 bytes.
// Erase moves the last element into the vacated slot; erase(it) returns `it`, which now
// refers to that moved-in element.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/containers/detail/dense_hash_table.h>
#include <core/hash/hash.h>
#include <core/memory/allocator.h>

#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

template <class Key, class Hasher = Hash<Key>, class KeyEqual = std::equal_to<>,
          class SizeType = u32, class Alloc = mem::DefaultAlloc>
class HashSet
    : private containers::detail::DenseHashTable<Key, void, Hasher, KeyEqual, SizeType, Alloc> {
  using Base = containers::detail::DenseHashTable<Key, void, Hasher, KeyEqual, SizeType, Alloc>;

 public:
  using key_type = Key;
  using value_type = Key;
  using hasher = Hasher;
  using key_equal = KeyEqual;
  using size_type = SizeType;
  using allocator_type = Alloc;
  using difference_type = isize;
  using iterator = const Key*;
  using const_iterator = const Key*;

  static constexpr size_type max_size() noexcept { return Base::k_max_size; }

  HashSet() noexcept = default;
  explicit HashSet(Alloc alloc) noexcept : Base(Hasher{}, KeyEqual{}, std::move(alloc)) {}
  HashSet(Hasher h, KeyEqual eq = KeyEqual{}, Alloc alloc = Alloc{}) noexcept
      : Base(std::move(h), std::move(eq), std::move(alloc)) {}
  HashSet(const HashSet&) = default;
  HashSet(HashSet&&) noexcept = default;
  HashSet& operator=(const HashSet&) = default;
  HashSet& operator=(HashSet&&) noexcept = default;
  ~HashSet() = default;

  void swap(HashSet& other) noexcept { Base::swap(other); }
  friend void swap(HashSet& a, HashSet& b) noexcept { a.swap(b); }

  size_type size() const noexcept { return this->size_; }
  bool empty() const noexcept { return this->size_ == 0; }
  size_type capacity() const noexcept { return this->dense_capacity_; }
  size_type bucket_count() const noexcept { return this->bucket_count_; }
  const Alloc& get_allocator() const noexcept { return this->alloc_; }

  void reserve(size_type n) { this->reserve_impl(n); }
  void shrink_to_fit() { this->shrink_impl(); }
  void clear() noexcept { this->clear_impl(); }

  template <class K>
  const_iterator find(const K& key) const noexcept {
    size_type i;
    return this->find_dense(key, i) ? this->keys_ + i : end();
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

  const Key& at(size_type i) const noexcept {
    ENGINE_ASSERT(i < this->size_, "HashSet::at: index out of range");
    return this->keys_[i];
  }
  std::span<const Key> keys() const noexcept { return {this->keys_, this->size_}; }
  const Hasher& hash_function() const noexcept { return this->hasher_; }
  const KeyEqual& key_eq() const noexcept { return this->equal_; }

  const_iterator begin() const noexcept { return this->keys_; }
  const_iterator end() const noexcept { return this->keys_ + this->size_; }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }

  template <class K>
  std::pair<const_iterator, bool> insert(K&& key) {
    size_type i;
    if (this->find_dense(key, i)) return {this->keys_ + i, false};
    this->ensure_room_for_one();
    const u64 h = this->hasher_(key);
    i = this->emplace_new(h, Key(std::forward<K>(key)));
    return {this->keys_ + i, true};
  }

  template <class K>
  size_type erase(const K& key) noexcept {
    u32 b;
    if (!this->find_bucket(key, b)) return 0;
    this->erase_bucket(b);
    return 1;
  }
  const_iterator erase(const_iterator pos) noexcept {
    const auto i = static_cast<size_type>(pos - this->keys_);
    erase_at(i);
    return this->keys_ + i;
  }
  void erase_at(size_type i) noexcept {
    ENGINE_ASSERT(i < this->size_, "HashSet::erase_at: index out of range");
    this->erase_bucket(this->bucket_for_dense(i));
  }

  template <class K>
  std::optional<Key> extract(const K& key) noexcept {
    u32 b;
    if (!this->find_bucket(key, b)) return std::nullopt;
    std::optional<Key> out(std::in_place, std::move(this->keys_[this->buckets_[b].index]));
    this->erase_bucket(b);
    return out;
  }

  template <class InputIt>
  void insert_bulk(InputIt first, InputIt last) {
    for (; first != last; ++first)
      insert(*first);
  }
  template <class Range>
  void insert_bulk(Range&& range) {
    insert_bulk(std::begin(range), std::end(range));
  }

  friend bool operator==(const HashSet& a, const HashSet& b) {
    if (a.size() != b.size()) return false;
    for (const Key& k : a) {
      if (!b.contains(k)) return false;
    }
    return true;
  }
};

}  // namespace engine
