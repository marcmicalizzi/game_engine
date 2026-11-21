// W155 (renderer.md, "The tail's CPU table grows into room made ahead"): the streamed tail's CPU
// table grows into a block made and faulted in ahead of the growth, a stated number of bytes a
// frame, so the frame that passes the capacity allocates nothing for it. No device: the table is
// the CPU's. Each promise is a count — allocations on the thread, frames, bytes — never a clock.
#include <core/memory/memory.h>
#include <systems/renderer/table_room.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::renderer;

namespace {

struct Slot {
  u64 a = 0;
  u64 b = 0;
  u32 c = 0;
  u32 d = 0;
};

Vector<Slot> filled(u32 count) {
  Vector<Slot> table;
  table.reserve(count);
  for (u32 i = 0; i < count; ++i)
    table.push_back(Slot{i, u64{i} * 3, i ^ 0x5a5au, i + 7});
  return table;
}

bool same(const Vector<Slot>& table, u32 count) {
  if (table.size() < count) return false;
  for (u32 i = 0; i < count; ++i) {
    const Slot& s = table[i];
    if (s.a != i || s.b != u64{i} * 3 || s.c != (i ^ 0x5a5au) || s.d != i + 7) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("table room: the growth past capacity takes the room made ahead and allocates nothing") {
  constexpr u32 k_capacity = 4096;
  Vector<Slot> table = filled(k_capacity);
  REQUIRE(table.capacity() == k_capacity);
  TableRoom<Slot> room;
  // Under three quarters full nothing is made; at the capacity it is, half again of it.
  {
    Vector<Slot> half = filled(k_capacity);
    half.resize(k_capacity / 2);
    TableRoom<Slot> early;
    CHECK(early.prepare(half, 1u << 20) == 0);
    CHECK(early.held_bytes() == 0);
  }
  // The room is faulted in 16 KiB a frame, whole slots: its 6,144 slots of 24 bytes, 682 a frame,
  // take ten frames.
  constexpr usize k_budget = 16 * 1024;
  u32 frames = 0;
  usize touched = 0;
  while (!room.ready() && frames < 100) {
    touched += room.prepare(table, k_budget);
    ++frames;
  }
  const usize want = k_capacity + k_capacity / 2;
  CHECK(room.ready());
  CHECK(room.held_bytes() == want * sizeof(Slot));
  CHECK(touched == want * sizeof(Slot));
  CHECK(frames == (want * sizeof(Slot) + k_budget - 1) / (k_budget / sizeof(Slot) * sizeof(Slot)));
  CHECK(room.prepare(table, k_budget) == 0);  // made: nothing more a frame
  // The growth: the table moves into the room, the resize after it allocates nothing, and every
  // slot is where it was.
  const u64 before = mem::allocations_on_thread();
  CHECK(room.take(table, k_capacity + 100));
  table.resize(k_capacity + 100);
  CHECK(mem::allocations_on_thread() - before == 0);
  CHECK(table.capacity() == want);
  CHECK(same(table, k_capacity));
  CHECK(room.taken() == 1);
  // The old block is held until the next frame's `prepare`, which frees it and does nothing else.
  CHECK(room.held_bytes() == k_capacity * sizeof(Slot));
  CHECK_FALSE(room.ready());
  CHECK(room.prepare(table, k_budget) == 0);
  CHECK(room.held_bytes() == 0);
  // Without the room, the same growth allocates on the frame, as the container always did.
  Vector<Slot> plain = filled(k_capacity);
  const u64 plain_before = mem::allocations_on_thread();
  plain.resize(k_capacity + 100);
  CHECK(mem::allocations_on_thread() - plain_before == 1);
  CHECK(plain.capacity() == table.capacity());  // the same block size, made on the frame
}

TEST_CASE("table room: a growth inside the capacity, or past the room, leaves the table as it was") {
  Vector<Slot> table = filled(1000);
  TableRoom<Slot> room;
  while (!room.ready())
    (void)room.prepare(table, 1u << 20);
  CHECK_FALSE(room.take(table, 900));         // no growth: nothing taken
  CHECK_FALSE(room.take(table, 1000 + 1500));  // past the room: the caller grows as before
  CHECK(room.taken() == 0);
  CHECK(room.held_bytes() == 1500 * sizeof(Slot));
  table.resize(2500);
  CHECK(same(table, 1000));
}
