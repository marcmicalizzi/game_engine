#include <core/memory/arena.h>

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace engine;

TEST_CASE("arena: sequential allocations are contiguous and aligned") {
  mem::Arena arena(1024);
  auto* a = static_cast<char*>(arena.allocate(10, 1));
  auto* b = static_cast<char*>(arena.allocate(10, 1));
  CHECK(b == a + 10);
  auto* c = arena.allocate(10, 64);
  CHECK((reinterpret_cast<usize>(c) & 63) == 0);
  CHECK(arena.chunk_count() == 1);
  CHECK(arena.bytes_allocated() >= 30);
  CHECK(arena.bytes_reserved() == 1024);
}

TEST_CASE("arena: grows into new chunks and dedicates chunks to large requests") {
  mem::Arena arena(256);
  for (int i = 0; i < 10; ++i) {
    void* p = arena.allocate(100, 8);
    std::memset(p, i, 100);
  }
  CHECK(arena.chunk_count() >= 4);
  const usize before = arena.chunk_count();
  void* big = arena.allocate(10000, 8);
  REQUIRE(big != nullptr);
  std::memset(big, 0x5A, 10000);
  CHECK(arena.chunk_count() == before + 1);
  CHECK(arena.bytes_reserved() >= 10000 + 256 * before);
}

TEST_CASE("arena: reset keeps chunks and reuses them with no new reservations") {
  mem::Arena arena(512);
  for (int i = 0; i < 20; ++i)
    (void)arena.allocate(100, 8);
  const usize chunks = arena.chunk_count();
  const usize reserved = arena.bytes_reserved();
  const mem::Stats total_before = mem::total_stats();
  for (int round = 0; round < 5; ++round) {
    arena.reset();
    CHECK(arena.bytes_allocated() == 0);
    for (int i = 0; i < 20; ++i)
      (void)arena.allocate(100, 8);
  }
  CHECK(arena.chunk_count() == chunks);
  CHECK(arena.bytes_reserved() == reserved);
  CHECK(mem::total_stats().allocation_count == total_before.allocation_count);  // steady state
}

TEST_CASE("arena: mark and rewind release later allocations only") {
  mem::Arena arena(256);
  auto* a = static_cast<int*>(arena.allocate(sizeof(int), alignof(int)));
  *a = 42;
  const mem::Arena::Mark m = arena.mark();
  for (int i = 0; i < 50; ++i)
    (void)arena.allocate(40, 8);  // spans several chunks
  arena.rewind(m);
  CHECK(*a == 42);
  auto* b = static_cast<int*>(arena.allocate(sizeof(int), alignof(int)));
  CHECK(b == a + 1);  // resumed right after the mark
}

TEST_CASE("arena: create and create_array construct trivially destructible objects") {
  struct Point {
    float x, y, z;
  };
  mem::Arena arena;
  Point* p = arena.create<Point>(1.0f, 2.0f, 3.0f);
  CHECK(p->y == 2.0f);
  Point* arr = arena.create_array<Point>(16);
  CHECK(arr[15].z == 0.0f);
  CHECK((reinterpret_cast<usize>(arr) & (alignof(Point) - 1)) == 0);
}

TEST_CASE("arena: move semantics transfer ownership") {
  mem::Arena a(256);
  (void)a.allocate(100, 8);
  const usize reserved = a.bytes_reserved();
  mem::Arena b = std::move(a);
  CHECK(a.chunk_count() == 0);
  CHECK(b.bytes_reserved() == reserved);
  mem::Arena c;
  c = std::move(b);
  CHECK(b.chunk_count() == 0);
  CHECK(c.bytes_reserved() == reserved);
}

TEST_CASE("arena: release returns all memory to the heap") {
  const mem::Stats before = mem::total_stats();
  {
    mem::Arena arena(1024);
    for (int i = 0; i < 100; ++i)
      (void)arena.allocate(200, 8);
    CHECK(mem::total_stats().bytes_current > before.bytes_current);
  }
  CHECK(mem::total_stats().bytes_current == before.bytes_current);
}

TEST_CASE("arena: ArenaAlloc satisfies the policy") {
  static_assert(mem::AllocatorPolicy<mem::ArenaAlloc>);
  mem::Arena arena;
  mem::ArenaAlloc alloc{&arena};
  void* p = alloc.allocate(32, 16);
  REQUIRE(p != nullptr);
  alloc.deallocate(p, 32, 16);  // no-op
  CHECK(arena.bytes_allocated() >= 32);
}
