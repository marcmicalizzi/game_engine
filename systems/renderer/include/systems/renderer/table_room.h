#pragma once

// **A CPU table that grows into room made ahead of need** (W155; docs/subsystems/renderer.md, "The
// tail's CPU table grows into room made ahead"). The streamed world's instance table on the CPU
// (`GpuScene::instance_table_`, 96 bytes a slot, and the keys beside it) grows by the container's
// half again when a hand-over passes its capacity, and on the forest staging's walk that growth —
// 470,000 slots, a new 67 MB block, 45 MB copied and every page of it faulted in for the first time
// — was the slot write's 28 to 48 ms on the frame that also grew the GPU's tables. `TableRoom` makes
// the next capacity before it is needed: once the table is three quarters full, `prepare` reserves
// the block the next growth would take and faults it in a stated number of bytes a frame, so that
// when the growth comes `take` copies the table into pages already resident and swaps, with no
// allocation and no fault on that frame. The table's contents and order are exactly what the
// container's own growth would have given; only when its memory was touched moves.
//
// The room costs a block the size of the next capacity for as long as it is held, a quarter to a
// half again of the table beside it; a table that never grows past three quarters never makes one.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <algorithm>
#include <span>

namespace engine::renderer {

template <class T>
class TableRoom {
 public:
  // Between frames: once `table` is three quarters full, make the block its next growth takes (the
  // container's own half again) and fault in `budget_bytes` more of it. Returns the bytes touched.
  usize prepare(const Vector<T>& table, usize budget_bytes) {
    using size_type = typename Vector<T>::size_type;
    const u64 capacity = table.capacity();
    if (capacity == 0 || u64{table.size()} * 4 < capacity * 3) return 0;
    const u64 want = std::min<u64>(capacity + capacity / 2, table.max_size());
    if (room_.capacity() < want) {
      room_.clear();
      room_.reserve(static_cast<size_type>(want));  // exactly: an address range, no page touched
    }
    if (room_.size() >= want) return 0;
    const u64 step = std::max<u64>(1, budget_bytes / sizeof(T));
    const u64 to = std::min<u64>(want, u64{room_.size()} + step);
    const usize touched = static_cast<usize>((to - room_.size()) * sizeof(T));
    // Inside the reserve: constructs, which writes, which faults the pages in.
    room_.resize(static_cast<size_type>(to));
    return touched;
  }

  // Before `table` is sized to `end`: when that passes its capacity and the room holds it, the
  // table moves into the room (a copy into resident pages, no allocation) and the old block goes.
  // False when it did not, and the caller's resize grows the table as it always did.
  bool take(Vector<T>& table, usize end) {
    if (end <= table.capacity() || room_.capacity() < end) return false;
    room_.clear();
    room_.append(std::span<const T>(table.data(), table.size()));
    table.swap(room_);
    Vector<T>().swap(room_);
    ++taken_;
    return true;
  }

  usize held_bytes() const noexcept { return usize{room_.capacity()} * sizeof(T); }
  bool ready() const noexcept { return room_.capacity() != 0 && room_.size() == room_.capacity(); }
  u64 taken() const noexcept { return taken_; }

 private:
  Vector<T> room_;
  u64 taken_ = 0;
};

}  // namespace engine::renderer
