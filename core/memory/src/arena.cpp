#include <core/memory/arena.h>

#include <core/base/assert.h>

#include <cstddef>

namespace engine::mem {

namespace {

constexpr usize align_up(usize value, usize alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

Arena::Arena(usize chunk_bytes) noexcept : chunk_bytes_(chunk_bytes == 0 ? k_default_chunk_bytes : chunk_bytes) {}

Arena::~Arena() { release(); }

Arena::Arena(Arena&& other) noexcept
    : first_(other.first_),
      current_(other.current_),
      chunk_bytes_(other.chunk_bytes_),
      bytes_reserved_(other.bytes_reserved_),
      chunk_count_(other.chunk_count_) {
  other.first_ = nullptr;
  other.current_ = nullptr;
  other.bytes_reserved_ = 0;
  other.chunk_count_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
  if (this != &other) {
    release();
    first_ = other.first_;
    current_ = other.current_;
    chunk_bytes_ = other.chunk_bytes_;
    bytes_reserved_ = other.bytes_reserved_;
    chunk_count_ = other.chunk_count_;
    other.first_ = nullptr;
    other.current_ = nullptr;
    other.bytes_reserved_ = 0;
    other.chunk_count_ = 0;
  }
  return *this;
}

std::byte* Arena::data_of(Chunk* c) noexcept {
  return static_cast<std::byte*>(static_cast<void*>(c)) + k_header_bytes;
}

Arena::Chunk* Arena::new_chunk(usize capacity) {
  auto* c = static_cast<Chunk*>(mem::allocate(k_header_bytes + capacity, k_chunk_align));
  c->next = nullptr;
  c->capacity = capacity;
  c->used = 0;
  bytes_reserved_ += capacity;
  ++chunk_count_;
  return c;
}

void* Arena::allocate(usize bytes, usize align) {
  ENGINE_ASSERT(align != 0 && (align & (align - 1)) == 0, "Arena::allocate: alignment must be a power of two");
  if (bytes == 0) bytes = 1;
  if (current_ != nullptr) {
    const usize start = align_up(current_->used, align);
    if (start + bytes <= current_->capacity) {
      current_->used = start + bytes;
      return data_of(current_) + start;
    }
  }
  return allocate_slow(bytes, align);
}

void* Arena::allocate_slow(usize bytes, usize align) {
  // Chunk data is 64-byte aligned; larger alignments are satisfied by padding inside the chunk.
  const usize worst_case = bytes + (align > k_chunk_align ? align : 0);

  if (worst_case > chunk_bytes_) {
    // Dedicated chunk, linked after the current one so rewind/reset semantics stay simple.
    Chunk* c = new_chunk(worst_case);
    if (current_ == nullptr) {
      first_ = current_ = c;
    } else {
      c->next = current_->next;
      current_->next = c;
      current_ = c;
    }
  } else if (current_ != nullptr && current_->next != nullptr) {
    // Reuse a chunk retained by reset() or rewind().
    current_ = current_->next;
    current_->used = 0;
    if (worst_case > current_->capacity) return allocate_slow(bytes, align);  // dedicated chunk too small; skip past it
  } else {
    Chunk* c = new_chunk(chunk_bytes_);
    if (current_ == nullptr) {
      first_ = current_ = c;
    } else {
      current_->next = c;
      current_ = c;
    }
  }

  const usize start = align_up(current_->used, align);
  ENGINE_ASSERT(start + bytes <= current_->capacity, "Arena: chunk sizing error");
  current_->used = start + bytes;
  return data_of(current_) + start;
}

Arena::Mark Arena::mark() const noexcept {
  return Mark{current_, current_ != nullptr ? current_->used : 0};
}

void Arena::rewind(Mark mark) noexcept {
  if (mark.chunk == nullptr) {
    reset();
    return;
  }
  current_ = static_cast<Chunk*>(mark.chunk);
  current_->used = mark.used;
  // Chunks after the mark are kept for reuse; allocate_slow resets `used` when it re-enters them.
}

void Arena::reset() noexcept {
  current_ = first_;
  if (current_ != nullptr) current_->used = 0;
}

void Arena::release() noexcept {
  Chunk* c = first_;
  while (c != nullptr) {
    Chunk* next = c->next;
    mem::deallocate(c, k_header_bytes + c->capacity, k_chunk_align);
    c = next;
  }
  first_ = current_ = nullptr;
  bytes_reserved_ = 0;
  chunk_count_ = 0;
}

usize Arena::bytes_allocated() const noexcept {
  usize total = 0;
  for (Chunk* c = first_; c != nullptr; c = c->next) {
    total += c->used;
    if (c == current_) break;
  }
  return total;
}

}  // namespace engine::mem
