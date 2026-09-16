#pragma once

// SlotMap: stable handles to densely stored values.
//
// insert() returns a 64-bit SlotHandle (32-bit slot index + 32-bit generation). Values live in
// a dense array so iteration is a linear walk; a slot table maps handles to dense indices and
// detects stale handles through the generation counter. All operations are O(1). This is the
// engine's replacement for pointers to owned objects across system boundaries
// (docs/plan/11-performance-principles.md section 11.9).
//
// Erase moves the last value into the vacated dense slot, so dense indices are not stable;
// handles are. The container object is 40 bytes.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/detail/raw_storage.h>
#include <core/memory/allocator.h>

#include <compare>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace engine {

struct SlotHandle {
  u32 index = 0;
  u32 generation = 0;  // 0 is the null handle; live handles have odd generations

  constexpr bool is_null() const noexcept { return generation == 0; }
  constexpr explicit operator bool() const noexcept { return !is_null(); }
  constexpr u64 to_u64() const noexcept { return (static_cast<u64>(generation) << 32) | index; }
  static constexpr SlotHandle from_u64(u64 v) noexcept {
    return SlotHandle{static_cast<u32>(v), static_cast<u32>(v >> 32)};
  }
  constexpr auto operator<=>(const SlotHandle&) const noexcept = default;
};

template <class T, class Alloc = mem::DefaultAlloc>
class SlotMap {
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "SlotMap: T must be nothrow move constructible");
  static_assert(mem::AllocatorPolicy<Alloc>, "SlotMap: Alloc must satisfy AllocatorPolicy");

 public:
  using value_type = T;
  using handle_type = SlotHandle;
  using size_type = u32;
  using allocator_type = Alloc;

  static constexpr size_type max_size() noexcept { return 0xFFFFFFFEu; }

  template <bool IsConst>
  struct Entry {
    SlotHandle handle;
    std::conditional_t<IsConst, const T&, T&> value;
  };

  template <bool IsConst>
  class EntryIterator {
    using map_pointer = std::conditional_t<IsConst, const SlotMap*, SlotMap*>;

   public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = Entry<IsConst>;
    using difference_type = isize;
    using reference = Entry<IsConst>;
    struct pointer {
      reference ref;
      const reference* operator->() const noexcept { return &ref; }
    };

    EntryIterator() noexcept = default;
    EntryIterator(map_pointer m, size_type i) noexcept : m_(m), i_(i) {}
    reference operator*() const noexcept { return reference{m_->handle_at(i_), m_->values_[i_]}; }
    pointer operator->() const noexcept { return pointer{**this}; }
    EntryIterator& operator++() noexcept {
      ++i_;
      return *this;
    }
    EntryIterator operator++(int) noexcept {
      EntryIterator tmp = *this;
      ++i_;
      return tmp;
    }
    bool operator==(const EntryIterator& o) const noexcept { return i_ == o.i_; }
    size_type index() const noexcept { return i_; }

   private:
    map_pointer m_ = nullptr;
    size_type i_ = 0;
  };

  template <bool IsConst>
  class EntryRange {
    using map_pointer = std::conditional_t<IsConst, const SlotMap*, SlotMap*>;

   public:
    explicit EntryRange(map_pointer m) noexcept : m_(m) {}
    EntryIterator<IsConst> begin() const noexcept { return {m_, 0}; }
    EntryIterator<IsConst> end() const noexcept { return {m_, m_->size_}; }

   private:
    map_pointer m_;
  };

  // --- construction -----------------------------------------------------------------------

  SlotMap() noexcept = default;
  explicit SlotMap(Alloc alloc) noexcept : alloc_(std::move(alloc)) {}
  ENGINE_NON_COPYABLE(SlotMap);  // copying would duplicate handles; clone explicitly if needed
  SlotMap(SlotMap&& o) noexcept
      : values_(o.values_),
        slots_(o.slots_),
        size_(o.size_),
        dense_capacity_(o.dense_capacity_),
        slot_count_(o.slot_count_),
        slot_capacity_(o.slot_capacity_),
        free_head_(o.free_head_),
        alloc_(std::move(o.alloc_)) {
    o.forget();
  }
  SlotMap& operator=(SlotMap&& o) noexcept {
    if (this != &o) {
      release();
      values_ = o.values_;
      slots_ = o.slots_;
      size_ = o.size_;
      dense_capacity_ = o.dense_capacity_;
      slot_count_ = o.slot_count_;
      slot_capacity_ = o.slot_capacity_;
      free_head_ = o.free_head_;
      alloc_ = std::move(o.alloc_);
      o.forget();
    }
    return *this;
  }
  ~SlotMap() { release(); }

  // --- capacity ---------------------------------------------------------------------------

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_type capacity() const noexcept { return dense_capacity_; }
  size_type slot_count() const noexcept { return slot_count_; }
  const Alloc& get_allocator() const noexcept { return alloc_; }

  void reserve(size_type n) {
    if (n > dense_capacity_) reallocate_dense(n);
    if (n > slot_capacity_) reallocate_slots(n);
  }

  // Invalidates every handle and releases every value; slots are recycled.
  void clear() noexcept {
    containers::detail::destroy_n(values_, size_);
    for (size_type i = 0; i < size_; ++i)
      retire_slot(dense_to_slot()[i]);
    size_ = 0;
  }

  // --- lookup -----------------------------------------------------------------------------

  bool contains(SlotHandle h) const noexcept {
    return h.index < slot_count_ && (h.generation & 1u) != 0 &&
           slots_[h.index].generation == h.generation;
  }
  T* get(SlotHandle h) noexcept {
    return contains(h) ? values_ + slots_[h.index].dense_or_next : nullptr;
  }
  const T* get(SlotHandle h) const noexcept {
    return contains(h) ? values_ + slots_[h.index].dense_or_next : nullptr;
  }
  // Dense index of a live handle's value, for callers that index values() directly.
  size_type dense_index(SlotHandle h) const noexcept {
    ENGINE_ASSERT(contains(h), "SlotMap::dense_index: stale or null handle");
    return slots_[h.index].dense_or_next;
  }
  SlotHandle handle_at(size_type dense) const noexcept {
    ENGINE_ASSERT(dense < size_, "SlotMap::handle_at: index out of range");
    const size_type slot = dense_to_slot()[dense];
    return SlotHandle{slot, slots_[slot].generation};
  }

  std::span<T> values() noexcept { return {values_, size_}; }
  std::span<const T> values() const noexcept { return {values_, size_}; }
  EntryRange<false> entries() noexcept { return EntryRange<false>(this); }
  EntryRange<true> entries() const noexcept { return EntryRange<true>(this); }

  // --- modifiers --------------------------------------------------------------------------

  template <class... Args>
  SlotHandle emplace(Args&&... args) {
    ENGINE_VERIFY(size_ < max_size(), "SlotMap: size overflow");
    if (size_ == dense_capacity_) {
      reallocate_dense(
          containers::detail::grow_capacity<size_type>(dense_capacity_, size_ + 1, max_size()));
    }
    size_type slot;
    if (free_head_ != k_null) {
      slot = free_head_;
      free_head_ = slots_[slot].dense_or_next;
    } else {
      if (slot_count_ == slot_capacity_) {
        reallocate_slots(containers::detail::grow_capacity<size_type>(slot_capacity_,
                                                                      slot_count_ + 1, max_size()));
      }
      slot = slot_count_++;
      slots_[slot].generation = 0;
    }
    Slot& s = slots_[slot];
    s.generation |= 1u;  // even (free) -> odd (live)
    s.dense_or_next = size_;
    std::construct_at(values_ + size_, std::forward<Args>(args)...);
    dense_to_slot()[size_] = slot;
    ++size_;
    return SlotHandle{slot, s.generation};
  }
  SlotHandle insert(const T& value) { return emplace(value); }
  SlotHandle insert(T&& value) { return emplace(std::move(value)); }

  // Returns false for a null or stale handle.
  bool erase(SlotHandle h) noexcept {
    if (!contains(h)) return false;
    const size_type dense = slots_[h.index].dense_or_next;
    const size_type last = size_ - 1;
    if (dense != last) {
      values_[dense] = std::move(values_[last]);
      const size_type moved_slot = dense_to_slot()[last];
      dense_to_slot()[dense] = moved_slot;
      slots_[moved_slot].dense_or_next = dense;
    }
    std::destroy_at(values_ + last);
    --size_;
    retire_slot(h.index);
    return true;
  }

 private:
  struct Slot {
    size_type dense_or_next;  // dense index when live, next free slot when free
    u32 generation;
  };
  static constexpr size_type k_null = 0xFFFFFFFFu;

  // Dense block: T[dense_capacity_] followed by size_type[dense_capacity_] (slot of each value).
  static constexpr usize dense_align() noexcept {
    return containers::detail::max_align(alignof(T), alignof(size_type));
  }
  static constexpr usize slots_offset(size_type capacity) noexcept {
    return containers::detail::align_up(static_cast<usize>(capacity) * sizeof(T),
                                        alignof(size_type));
  }
  static constexpr usize dense_bytes(size_type capacity) noexcept {
    return slots_offset(capacity) + static_cast<usize>(capacity) * sizeof(size_type);
  }
  static size_type* dense_to_slot_at(T* values, size_type capacity) noexcept {
    void* base = static_cast<void*>(values);
    void* p = static_cast<std::byte*>(base) + slots_offset(capacity);
    return static_cast<size_type*>(p);
  }
  size_type* dense_to_slot() noexcept { return dense_to_slot_at(values_, dense_capacity_); }
  const size_type* dense_to_slot() const noexcept {
    return dense_to_slot_at(values_, dense_capacity_);
  }

  void retire_slot(size_type slot) noexcept {
    Slot& s = slots_[slot];
    s.generation += 1;                        // odd -> even
    if (s.generation == 0) s.generation = 2;  // never let a wrapped generation read as null
    s.dense_or_next = free_head_;
    free_head_ = slot;
  }

  void reallocate_dense(size_type new_capacity) {
    T* new_values = static_cast<T*>(alloc_.allocate(dense_bytes(new_capacity), dense_align()));
    containers::detail::relocate_n(values_, size_, new_values);
    containers::detail::relocate_n(dense_to_slot(), size_,
                                   dense_to_slot_at(new_values, new_capacity));
    alloc_.deallocate(values_, dense_bytes(dense_capacity_), dense_align());
    values_ = new_values;
    dense_capacity_ = new_capacity;
  }

  void reallocate_slots(size_type new_capacity) {
    Slot* new_slots =
        static_cast<Slot*>(alloc_.allocate(sizeof(Slot) * new_capacity, alignof(Slot)));
    containers::detail::relocate_n(slots_, slot_count_, new_slots);
    alloc_.deallocate(slots_, sizeof(Slot) * slot_capacity_, alignof(Slot));
    slots_ = new_slots;
    slot_capacity_ = new_capacity;
  }

  void release() noexcept {
    containers::detail::destroy_n(values_, size_);
    alloc_.deallocate(values_, dense_bytes(dense_capacity_), dense_align());
    alloc_.deallocate(slots_, sizeof(Slot) * slot_capacity_, alignof(Slot));
    forget();
  }
  void forget() noexcept {
    values_ = nullptr;
    slots_ = nullptr;
    size_ = 0;
    dense_capacity_ = 0;
    slot_count_ = 0;
    slot_capacity_ = 0;
    free_head_ = k_null;
  }

  T* values_ = nullptr;
  Slot* slots_ = nullptr;
  size_type size_ = 0;
  size_type dense_capacity_ = 0;
  size_type slot_count_ = 0;
  size_type slot_capacity_ = 0;
  size_type free_head_ = k_null;
  ENGINE_NO_UNIQUE_ADDRESS Alloc alloc_{};
};

}  // namespace engine
