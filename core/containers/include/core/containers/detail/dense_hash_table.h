#pragma once

// DenseHashTable: the shared core of HashMap and HashSet.
//
// Layout: keys (and values, for maps) live in dense parallel arrays in one heap block, in
// insertion order until an erase swaps the last element into the vacated slot. A separate
// bucket array of 8-byte {distance|fingerprint, dense index} entries maps hashes to dense
// indices using Robin Hood linear probing with backward-shift deletion. Lookups compare the
// 8-bit fingerprint before touching a key, so most probes never leave the bucket array;
// iteration is a linear walk over the dense arrays. Load factor is capped at 80%.
//
// This is the design used by ankerl::unordered_dense, chosen over inline Robin Hood storage
// because dense arrays make iteration and bulk operations cache-friendly and keep the SoA
// layout the rest of the engine expects. It costs one indirection on a hit.
//
// Value may be void, in which case no value array exists (HashSet).

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>
#include <core/memory/allocator.h>

#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace engine::containers::detail {

template <class SizeType>
struct HashBucket {
  u32 dist_and_fingerprint;  // (probe distance + 1) << 8 | low 8 hash bits; 0 means empty
  SizeType index;            // dense index
};

template <class Key, class Value, class Hasher, class KeyEqual, class SizeType, class Alloc>
class DenseHashTable {
  static_assert(std::is_unsigned_v<SizeType>, "hash table: SizeType must be an unsigned integer");
  static_assert(mem::AllocatorPolicy<Alloc>, "hash table: Alloc must satisfy AllocatorPolicy");
  // Key requirements are checked in release() so Value may be incomplete at declaration.

 protected:
  static constexpr bool k_has_value = !std::is_void_v<Value>;
  // A stand-in so sizeof/alignof are well-formed in set mode; never instantiated as storage.
  using ValueStorage = std::conditional_t<k_has_value, Value, char>;
  using Bucket = HashBucket<SizeType>;

  static constexpr u32 k_dist_inc = 1u << 8;
  static constexpr u32 k_fingerprint_mask = 0xFFu;
  static constexpr SizeType k_initial_buckets = 16;
  static constexpr SizeType k_max_size = std::numeric_limits<SizeType>::max() - 1;

  DenseHashTable() noexcept = default;
  explicit DenseHashTable(Hasher h, KeyEqual eq, Alloc a) noexcept
      : hasher_(std::move(h)), equal_(std::move(eq)), alloc_(std::move(a)) {}
  DenseHashTable(const DenseHashTable& o) : hasher_(o.hasher_), equal_(o.equal_), alloc_(o.alloc_) {
    copy_from(o);
  }
  DenseHashTable(DenseHashTable&& o) noexcept
      : keys_(o.keys_),
        buckets_(o.buckets_),
        size_(o.size_),
        dense_capacity_(o.dense_capacity_),
        bucket_count_(o.bucket_count_),
        shift_(o.shift_),
        hasher_(std::move(o.hasher_)),
        equal_(std::move(o.equal_)),
        alloc_(std::move(o.alloc_)) {
    o.forget();
  }
  DenseHashTable& operator=(const DenseHashTable& o) {
    if (this != &o) {
      release();
      hasher_ = o.hasher_;
      equal_ = o.equal_;
      alloc_ = o.alloc_;
      copy_from(o);
    }
    return *this;
  }
  DenseHashTable& operator=(DenseHashTable&& o) noexcept {
    if (this != &o) {
      release();
      keys_ = o.keys_;
      buckets_ = o.buckets_;
      size_ = o.size_;
      dense_capacity_ = o.dense_capacity_;
      bucket_count_ = o.bucket_count_;
      shift_ = o.shift_;
      hasher_ = std::move(o.hasher_);
      equal_ = std::move(o.equal_);
      alloc_ = std::move(o.alloc_);
      o.forget();
    }
    return *this;
  }
  ~DenseHashTable() { release(); }

  void swap(DenseHashTable& o) noexcept {
    std::swap(keys_, o.keys_);
    std::swap(buckets_, o.buckets_);
    std::swap(size_, o.size_);
    std::swap(dense_capacity_, o.dense_capacity_);
    std::swap(bucket_count_, o.bucket_count_);
    std::swap(shift_, o.shift_);
    std::swap(hasher_, o.hasher_);
    std::swap(equal_, o.equal_);
    std::swap(alloc_, o.alloc_);
  }

  // --- storage geometry -------------------------------------------------------------------

  static constexpr usize block_align() noexcept {
    return k_has_value ? max_align(alignof(Key), alignof(ValueStorage)) : alignof(Key);
  }
  static constexpr usize values_offset(SizeType capacity) noexcept {
    return align_up(static_cast<usize>(capacity) * sizeof(Key), alignof(ValueStorage));
  }
  static constexpr usize dense_block_bytes(SizeType capacity) noexcept {
    if constexpr (k_has_value) {
      return values_offset(capacity) + static_cast<usize>(capacity) * sizeof(ValueStorage);
    } else {
      return static_cast<usize>(capacity) * sizeof(Key);
    }
  }
  static ValueStorage* values_at(Key* keys, SizeType capacity) noexcept {
    void* base = static_cast<void*>(keys);
    void* p = static_cast<std::byte*>(base) + values_offset(capacity);
    return static_cast<ValueStorage*>(p);
  }
  static constexpr usize bucket_bytes(SizeType count) noexcept {
    return static_cast<usize>(count) * sizeof(Bucket);
  }

  Key* keys_ptr() noexcept { return keys_; }
  const Key* keys_ptr() const noexcept { return keys_; }
  ValueStorage* values_ptr() noexcept {
    if constexpr (k_has_value) {
      return dense_capacity_ == 0 ? nullptr : values_at(keys_, dense_capacity_);
    } else {
      return nullptr;
    }
  }
  const ValueStorage* values_ptr() const noexcept {
    if constexpr (k_has_value) {
      return dense_capacity_ == 0 ? nullptr : values_at(keys_, dense_capacity_);
    } else {
      return nullptr;
    }
  }

  // --- probing ----------------------------------------------------------------------------

  static constexpr u32 dist_fp_of(u64 h) noexcept {
    return k_dist_inc | (static_cast<u32>(h) & k_fingerprint_mask);
  }
  u32 bucket_of(u64 h) const noexcept { return static_cast<u32>(h >> shift_); }
  u32 next_bucket(u32 b) const noexcept { return (b + 1) & (static_cast<u32>(bucket_count_) - 1); }

  // Finds the bucket holding `key`. Returns false if absent.
  template <class K>
  bool find_bucket(const K& key, u32& out_bucket) const noexcept {
    if (size_ == 0) return false;
    const u64 h = hasher_(key);
    u32 dist_fp = dist_fp_of(h);
    u32 b = bucket_of(h);
    while (true) {
      const Bucket& bk = buckets_[b];
      if (bk.dist_and_fingerprint == dist_fp && equal_(keys_[bk.index], key)) {
        out_bucket = b;
        return true;
      }
      if (bk.dist_and_fingerprint < dist_fp) return false;  // Robin Hood: it would be here
      dist_fp += k_dist_inc;
      b = next_bucket(b);
    }
  }

  template <class K>
  bool find_dense(const K& key, SizeType& out_dense) const noexcept {
    u32 b;
    if (!find_bucket(key, b)) return false;
    out_dense = buckets_[b].index;
    return true;
  }

  // Finds the bucket that references dense index `dense` (which must be live).
  u32 bucket_for_dense(SizeType dense) const noexcept {
    u32 b = bucket_of(hasher_(keys_[dense]));
    while (buckets_[b].dist_and_fingerprint == 0 || buckets_[b].index != dense)
      b = next_bucket(b);
    return b;
  }

  // Inserts a bucket for dense index `dense` whose key hashes to `h`, displacing richer
  // entries (Robin Hood).
  void place_bucket(u64 h, SizeType dense) noexcept {
    Bucket b{dist_fp_of(h), dense};
    u32 pos = bucket_of(h);
    while (true) {
      Bucket& cur = buckets_[pos];
      if (cur.dist_and_fingerprint == 0) {
        cur = b;
        return;
      }
      if (cur.dist_and_fingerprint < b.dist_and_fingerprint) std::swap(cur, b);
      b.dist_and_fingerprint += k_dist_inc;
      pos = next_bucket(pos);
    }
  }

  // --- growth -----------------------------------------------------------------------------

  static constexpr SizeType buckets_for(SizeType n) noexcept {
    // Smallest power of two such that n fits under the 80% load cap.
    usize needed = (static_cast<usize>(n) * 5 + 3) / 4;
    if (needed < k_initial_buckets) needed = k_initial_buckets;
    return static_cast<SizeType>(std::bit_ceil(needed));
  }

  void ensure_room_for_one() {
    ENGINE_VERIFY(size_ < k_max_size, "hash table: size_type overflow");
    if (size_ == dense_capacity_) {
      reallocate_dense(grow_capacity<SizeType>(dense_capacity_, size_ + 1, k_max_size));
    }
    if (bucket_count_ == 0 ||
        (static_cast<usize>(size_) + 1) * 5 > static_cast<usize>(bucket_count_) * 4) {
      rehash(bucket_count_ == 0 ? k_initial_buckets : static_cast<SizeType>(bucket_count_ * 2));
    }
  }

  void reallocate_dense(SizeType new_capacity) {
    ENGINE_ASSERT(new_capacity >= size_, "hash table: dense capacity too small");
    Key* new_keys =
        static_cast<Key*>(alloc_.allocate(dense_block_bytes(new_capacity), block_align()));
    relocate_n(keys_, size_, new_keys);
    if constexpr (k_has_value) {
      relocate_n(values_ptr(), size_, values_at(new_keys, new_capacity));
    }
    alloc_.deallocate(keys_, dense_block_bytes(dense_capacity_), block_align());
    keys_ = new_keys;
    dense_capacity_ = new_capacity;
  }

  void rehash(SizeType new_bucket_count) {
    ENGINE_ASSERT(std::has_single_bit(static_cast<usize>(new_bucket_count)),
                  "hash table: bucket count must be a power of two");
    auto* new_buckets =
        static_cast<Bucket*>(alloc_.allocate(bucket_bytes(new_bucket_count), alignof(Bucket)));
    std::memset(static_cast<void*>(new_buckets), 0, bucket_bytes(new_bucket_count));
    alloc_.deallocate(buckets_, bucket_bytes(bucket_count_), alignof(Bucket));
    buckets_ = new_buckets;
    bucket_count_ = new_bucket_count;
    shift_ = static_cast<u8>(64 - std::countr_zero(static_cast<usize>(new_bucket_count)));
    for (SizeType i = 0; i < size_; ++i)
      place_bucket(hasher_(keys_[i]), i);
  }

  void reserve_impl(SizeType n) {
    if (n > dense_capacity_) reallocate_dense(n);
    const SizeType want = buckets_for(n);
    if (want > bucket_count_) rehash(want);
  }

  void shrink_impl() {
    if (size_ == 0) {
      release();
      return;
    }
    if (dense_capacity_ > size_) reallocate_dense(size_);
    const SizeType want = buckets_for(size_);
    if (want < bucket_count_) rehash(want);
  }

  // --- element construction and removal ---------------------------------------------------

  // Appends to the dense arrays (room must exist) and links a bucket. Returns the dense index.
  template <class K, class... Args>
  SizeType emplace_new(u64 h, K&& key, Args&&... args) {
    std::construct_at(keys_ + size_, std::forward<K>(key));
    if constexpr (k_has_value) {
      std::construct_at(values_ptr() + size_, std::forward<Args>(args)...);
    }
    place_bucket(h, size_);
    return size_++;
  }

  // Removes the element referenced by bucket `b`. The dense slot it occupied receives the
  // former last element (if different), so an iterator at that index sees a new element.
  void erase_bucket(u32 b) noexcept {
    const SizeType dense = buckets_[b].index;

    // Backward-shift deletion keeps the probe runs gap-free without tombstones.
    u32 hole = b;
    u32 next = next_bucket(hole);
    while (buckets_[next].dist_and_fingerprint >= 2 * k_dist_inc) {
      buckets_[hole] =
          Bucket{buckets_[next].dist_and_fingerprint - k_dist_inc, buckets_[next].index};
      hole = next;
      next = next_bucket(hole);
    }
    buckets_[hole] = Bucket{0, 0};

    const SizeType last = size_ - 1;
    if (dense != last) {
      keys_[dense] = std::move(keys_[last]);
      if constexpr (k_has_value) {
        values_ptr()[dense] = std::move(values_ptr()[last]);
      }
      buckets_[bucket_for_dense(last)].index = dense;
    }
    std::destroy_at(keys_ + last);
    if constexpr (k_has_value) {
      std::destroy_at(values_ptr() + last);
    }
    --size_;
  }

  void clear_impl() noexcept {
    destroy_n(keys_, size_);
    if constexpr (k_has_value) {
      destroy_n(values_ptr(), size_);
    }
    size_ = 0;
    if (buckets_ != nullptr)
      std::memset(static_cast<void*>(buckets_), 0, bucket_bytes(bucket_count_));
  }

  void copy_from(const DenseHashTable& o) {
    if (o.size_ == 0) return;
    keys_ = static_cast<Key*>(alloc_.allocate(dense_block_bytes(o.size_), block_align()));
    dense_capacity_ = o.size_;
    for (SizeType i = 0; i < o.size_; ++i)
      std::construct_at(keys_ + i, o.keys_[i]);
    if constexpr (k_has_value) {
      ValueStorage* dst = values_ptr();
      const ValueStorage* src = o.values_ptr();
      for (SizeType i = 0; i < o.size_; ++i)
        std::construct_at(dst + i, src[i]);
    }
    size_ = o.size_;
    // Bucket indices refer to dense positions, which are identical, so the array copies verbatim.
    buckets_ =
        static_cast<Bucket*>(alloc_.allocate(bucket_bytes(o.bucket_count_), alignof(Bucket)));
    std::memcpy(static_cast<void*>(buckets_), static_cast<const void*>(o.buckets_),
                bucket_bytes(o.bucket_count_));
    bucket_count_ = o.bucket_count_;
    shift_ = o.shift_;
  }

  void release() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Key>,
                  "hash table: Key must be nothrow move constructible");
    destroy_n(keys_, size_);
    if constexpr (k_has_value) {
      destroy_n(values_ptr(), size_);
    }
    alloc_.deallocate(keys_, dense_block_bytes(dense_capacity_), block_align());
    alloc_.deallocate(buckets_, bucket_bytes(bucket_count_), alignof(Bucket));
    forget();
  }

  void forget() noexcept {
    keys_ = nullptr;
    buckets_ = nullptr;
    size_ = 0;
    dense_capacity_ = 0;
    bucket_count_ = 0;
    shift_ = 64;
  }

  Key* keys_ = nullptr;
  Bucket* buckets_ = nullptr;
  SizeType size_ = 0;
  SizeType dense_capacity_ = 0;
  SizeType bucket_count_ = 0;
  u8 shift_ = 64;
  ENGINE_NO_UNIQUE_ADDRESS Hasher hasher_{};
  ENGINE_NO_UNIQUE_ADDRESS KeyEqual equal_{};
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine::containers::detail
