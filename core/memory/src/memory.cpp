#include <core/memory/memory.h>

#include <core/base/assert.h>
#include <core/hash/hash.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if ENGINE_PLATFORM_WINDOWS
#include <malloc.h>
#endif

#ifndef ENGINE_MEMORY_TRACKING
#define ENGINE_MEMORY_TRACKING 0
#endif

namespace engine::mem {

namespace {

// --- backend ------------------------------------------------------------------------------

void* backend_allocate(usize bytes, usize align) noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return ::_aligned_malloc(bytes, align);
#else
  // C11 aligned_alloc requires the size to be a multiple of the alignment.
  const usize rounded = (bytes + align - 1) & ~(align - 1);
  return std::aligned_alloc(align, rounded);
#endif
}

void backend_free(void* p) noexcept {
#if ENGINE_PLATFORM_WINDOWS
  ::_aligned_free(p);
#else
  std::free(p);
#endif
}

// --- tags -----------------------------------------------------------------------------------

struct TagName {
  char text[48];
};

TagName g_tag_names[k_max_tags];
std::atomic<u16> g_tag_count{0};
std::mutex g_tag_mutex;
thread_local TagId t_current_tag{};

// Bounded copy with guaranteed termination; avoids the CRT's deprecated strncpy.
void copy_name(char* dst, usize dst_size, const char* src) noexcept {
  usize i = 0;
  for (; i + 1 < dst_size && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

void ensure_untagged_registered() {
  // Called under g_tag_mutex.
  if (g_tag_count.load(std::memory_order_relaxed) == 0) {
    copy_name(g_tag_names[0].text, sizeof(TagName::text), "untagged");
    g_tag_count.store(1, std::memory_order_release);
  }
}

// --- statistics -----------------------------------------------------------------------------

struct AtomicStats {
  std::atomic<u64> bytes_current{0};
  std::atomic<u64> bytes_peak{0};
  std::atomic<u64> allocation_count{0};
  std::atomic<u64> free_count{0};

  void record_allocation(usize bytes) noexcept {
    const u64 now = bytes_current.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    allocation_count.fetch_add(1, std::memory_order_relaxed);
    u64 peak = bytes_peak.load(std::memory_order_relaxed);
    while (now > peak &&
           !bytes_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
  }
  void record_free(usize bytes) noexcept {
    bytes_current.fetch_sub(bytes, std::memory_order_relaxed);
    free_count.fetch_add(1, std::memory_order_relaxed);
  }
  Stats snapshot() const noexcept {
    return Stats{bytes_current.load(std::memory_order_relaxed),
                 bytes_peak.load(std::memory_order_relaxed),
                 allocation_count.load(std::memory_order_relaxed),
                 free_count.load(std::memory_order_relaxed)};
  }
};

AtomicStats g_total;
std::atomic<u64> g_allocation_counter{0};

#if ENGINE_MEMORY_TRACKING

AtomicStats g_tag_stats[k_max_tags];

// Pointer -> (bytes, tag) table so a free can be attributed to the tag that allocated it,
// wherever it happens. Sharded open addressing with linear probing and backward-shift
// deletion. Its own storage comes straight from the backend and is never tracked.
struct TrackEntry {
  void* ptr;
  usize bytes;
  TagId tag;
};

class Shard {
 public:
  void lock() noexcept {
    while (locked_.exchange(true, std::memory_order_acquire)) {
    }
  }
  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

  void insert(void* p, usize bytes, TagId tag) noexcept {
    if ((count_ + 1) * 4 > capacity_ * 3) grow();
    u32 i = index_of(p);
    while (entries_[i].ptr != nullptr) i = (i + 1) & mask();
    entries_[i] = TrackEntry{p, bytes, tag};
    ++count_;
  }

  bool remove(void* p, TrackEntry& out) noexcept {
    if (capacity_ == 0) return false;
    u32 i = index_of(p);
    while (entries_[i].ptr != p) {
      if (entries_[i].ptr == nullptr) return false;
      i = (i + 1) & mask();
    }
    out = entries_[i];
    // Backward shift: pull later entries in the run back over the hole when their ideal
    // position allows it.
    u32 hole = i;
    u32 k = (i + 1) & mask();
    while (entries_[k].ptr != nullptr) {
      const u32 ideal = index_of(entries_[k].ptr);
      if (((k - ideal) & mask()) >= ((k - hole) & mask())) {
        entries_[hole] = entries_[k];
        hole = k;
      }
      k = (k + 1) & mask();
    }
    entries_[hole] = TrackEntry{};
    --count_;
    return true;
  }

 private:
  u32 mask() const noexcept { return capacity_ - 1; }
  u32 index_of(void* p) const noexcept {
    return static_cast<u32>(mix64(static_cast<u64>(reinterpret_cast<usize>(p)) >> 6) >> shift_);
  }
  void grow() noexcept {
    const u32 new_capacity = capacity_ == 0 ? 1024 : capacity_ * 2;
    auto* new_entries = static_cast<TrackEntry*>(
        backend_allocate(sizeof(TrackEntry) * new_capacity, alignof(TrackEntry)));
    ENGINE_VERIFY(new_entries != nullptr, "memory tracker allocation failed");
    std::memset(static_cast<void*>(new_entries), 0, sizeof(TrackEntry) * new_capacity);
    TrackEntry* old_entries = entries_;
    const u32 old_capacity = capacity_;
    entries_ = new_entries;
    capacity_ = new_capacity;
    shift_ = static_cast<u8>(64 - std::countr_zero(new_capacity));
    count_ = 0;
    for (u32 i = 0; i < old_capacity; ++i) {
      if (old_entries[i].ptr != nullptr) insert(old_entries[i].ptr, old_entries[i].bytes, old_entries[i].tag);
    }
    backend_free(old_entries);
  }

  std::atomic<bool> locked_{false};
  TrackEntry* entries_ = nullptr;
  u32 capacity_ = 0;
  u32 count_ = 0;
  u8 shift_ = 64;
};

constexpr usize k_shards = 64;
Shard g_shards[k_shards];

Shard& shard_for(void* p) noexcept {
  return g_shards[mix64(static_cast<u64>(reinterpret_cast<usize>(p))) & (k_shards - 1)];
}

#endif  // ENGINE_MEMORY_TRACKING

}  // namespace

// --- tags -----------------------------------------------------------------------------------

TagId register_tag(const char* name) {
  ENGINE_VERIFY(name != nullptr && name[0] != '\0', "register_tag: name must be non-empty");
  std::lock_guard<std::mutex> lock(g_tag_mutex);
  ensure_untagged_registered();
  const u16 count = g_tag_count.load(std::memory_order_relaxed);
  for (u16 i = 0; i < count; ++i) {
    if (std::strncmp(g_tag_names[i].text, name, sizeof(TagName::text) - 1) == 0) return TagId{i};
  }
  ENGINE_VERIFY(count < k_max_tags, "register_tag: k_max_tags exceeded; raise the budget");
  copy_name(g_tag_names[count].text, sizeof(TagName::text), name);
  g_tag_count.store(static_cast<u16>(count + 1), std::memory_order_release);
  return TagId{count};
}

const char* tag_name(TagId tag) noexcept {
  if (tag.value >= g_tag_count.load(std::memory_order_acquire)) return "<unregistered>";
  return g_tag_names[tag.value].text;
}

u16 tag_count() noexcept {
  const u16 count = g_tag_count.load(std::memory_order_acquire);
  return count == 0 ? 1 : count;  // the untagged tag always exists conceptually
}

TagId current_tag() noexcept { return t_current_tag; }

TagScope::TagScope(TagId tag) noexcept : previous_(t_current_tag) { t_current_tag = tag; }
TagScope::~TagScope() { t_current_tag = previous_; }

// --- statistics -----------------------------------------------------------------------------

Stats stats([[maybe_unused]] TagId tag) noexcept {
#if ENGINE_MEMORY_TRACKING
  if (tag.value >= k_max_tags) return Stats{};
  return g_tag_stats[tag.value].snapshot();
#else
  return Stats{};
#endif
}

Stats total_stats() noexcept { return g_total.snapshot(); }

bool tracking_enabled() noexcept { return ENGINE_MEMORY_TRACKING != 0; }

u64 allocation_counter() noexcept { return g_allocation_counter.load(std::memory_order_relaxed); }

// --- allocation -----------------------------------------------------------------------------

void* try_allocate(usize bytes, usize align) noexcept {
  ENGINE_ASSERT(align != 0 && (align & (align - 1)) == 0, "allocate: alignment must be a power of two");
  if (bytes == 0) bytes = 1;
  void* p = backend_allocate(bytes, align);
  if (p == nullptr) return nullptr;

  g_allocation_counter.fetch_add(1, std::memory_order_relaxed);
  g_total.record_allocation(bytes);
#if ENGINE_MEMORY_TRACKING
  const TagId tag = t_current_tag;
  g_tag_stats[tag.value].record_allocation(bytes);
  Shard& shard = shard_for(p);
  shard.lock();
  shard.insert(p, bytes, tag);
  shard.unlock();
#endif
  return p;
}

void* allocate(usize bytes, usize align) {
  void* p = try_allocate(bytes, align);
  ENGINE_VERIFY(p != nullptr, "out of memory");
  return p;
}

void deallocate(void* p, usize bytes, [[maybe_unused]] usize align) noexcept {
  if (p == nullptr) return;
  if (bytes == 0) bytes = 1;
#if ENGINE_MEMORY_TRACKING
  TrackEntry entry{};
  Shard& shard = shard_for(p);
  shard.lock();
  const bool found = shard.remove(p, entry);
  shard.unlock();
  ENGINE_ASSERT(found, "deallocate: pointer was not allocated through mem::allocate");
  if (found) {
    ENGINE_ASSERT(entry.bytes == bytes, "deallocate: size does not match the allocation");
    g_tag_stats[entry.tag.value].record_free(entry.bytes);
    bytes = entry.bytes;
  }
#endif
  g_total.record_free(bytes);
  backend_free(p);
}

}  // namespace engine::mem
