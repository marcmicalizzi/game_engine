#include <core/memory/allocator.h>
#include <core/memory/memory.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace engine;

TEST_CASE("memory: tags register once and are named") {
  const mem::TagId a = mem::register_tag("test.alpha");
  const mem::TagId b = mem::register_tag("test.beta");
  const mem::TagId a2 = mem::register_tag("test.alpha");
  CHECK(a == a2);
  CHECK_FALSE(a == b);
  CHECK(std::string(mem::tag_name(a)) == "test.alpha");
  CHECK(std::string(mem::tag_name(mem::k_untagged)) == "untagged");
  CHECK(std::string(mem::tag_name(mem::TagId{60000})) == "<unregistered>");
  CHECK(mem::tag_count() >= 3);
}

TEST_CASE("memory: TagScope nests and restores") {
  const mem::TagId a = mem::register_tag("test.scope.a");
  const mem::TagId b = mem::register_tag("test.scope.b");
  const mem::TagId before = mem::current_tag();
  {
    mem::TagScope sa(a);
    CHECK(mem::current_tag() == a);
    {
      mem::TagScope sb(b);
      CHECK(mem::current_tag() == b);
    }
    CHECK(mem::current_tag() == a);
  }
  CHECK(mem::current_tag() == before);
}

TEST_CASE("memory: allocate honours alignment and rounds zero-byte requests") {
  for (usize align : {usize{8}, usize{16}, usize{64}, usize{256}, usize{4096}}) {
    void* p = mem::allocate(100, align);
    REQUIRE(p != nullptr);
    CHECK((reinterpret_cast<usize>(p) & (align - 1)) == 0);
    std::memset(p, 0xAB, 100);
    mem::deallocate(p, 100, align);
  }
  void* z = mem::allocate(0, 8);
  REQUIRE(z != nullptr);
  mem::deallocate(z, 0, 8);
  mem::deallocate(nullptr, 0, 8);  // no-op
}

TEST_CASE("memory: totals and the allocation counter move with allocations") {
  const mem::Stats before = mem::total_stats();
  const u64 counter_before = mem::allocation_counter();
  void* p = mem::allocate(1000, 16);
  const mem::Stats during = mem::total_stats();
  CHECK(during.bytes_current == before.bytes_current + 1000);
  CHECK(during.allocation_count == before.allocation_count + 1);
  CHECK(during.bytes_peak >= during.bytes_current);
  CHECK(mem::allocation_counter() == counter_before + 1);
  mem::deallocate(p, 1000, 16);
  const mem::Stats after = mem::total_stats();
  CHECK(after.bytes_current == before.bytes_current);
  CHECK(after.free_count == before.free_count + 1);
  CHECK(mem::allocation_counter() == counter_before + 1);  // frees do not count
}

TEST_CASE("memory: per-tag attribution follows the allocating scope, not the freeing one") {
  if (!mem::tracking_enabled()) {
    MESSAGE("memory tracking compiled out; skipping per-tag checks");
    return;
  }
  const mem::TagId a = mem::register_tag("test.attr.a");
  const mem::TagId b = mem::register_tag("test.attr.b");
  const mem::Stats a0 = mem::stats(a);
  const mem::Stats b0 = mem::stats(b);

  void* p;
  {
    mem::TagScope scope(a);
    p = mem::allocate(4096, 64);
  }
  CHECK(mem::stats(a).bytes_current == a0.bytes_current + 4096);
  CHECK(mem::stats(a).bytes_peak >= a0.bytes_current + 4096);
  CHECK(mem::stats(b).bytes_current == b0.bytes_current);
  {
    mem::TagScope scope(b);  // freeing under a different tag still credits tag a
    mem::deallocate(p, 4096, 64);
  }
  CHECK(mem::stats(a).bytes_current == a0.bytes_current);
  CHECK(mem::stats(a).free_count == a0.free_count + 1);
  CHECK(mem::stats(b).bytes_current == b0.bytes_current);
  CHECK(mem::stats(b).free_count == b0.free_count);
}

TEST_CASE("memory: many concurrent allocations balance to zero") {
  const mem::TagId tag = mem::register_tag("test.concurrent");
  const mem::Stats before = mem::stats(tag);
  const mem::Stats total_before = mem::total_stats();

  constexpr int k_threads = 8;
  constexpr int k_per_thread = 2000;
  std::vector<std::thread> threads;
  for (int t = 0; t < k_threads; ++t) {
    threads.emplace_back([tag, t] {
      mem::TagScope scope(tag);
      std::vector<std::pair<void*, usize>> live;
      live.reserve(k_per_thread);
      for (int i = 0; i < k_per_thread; ++i) {
        const usize bytes = static_cast<usize>(1 + ((i * 7 + t) % 300));
        live.emplace_back(mem::allocate(bytes, 16), bytes);
        if ((i % 3) == 0 && !live.empty()) {
          mem::deallocate(live.back().first, live.back().second, 16);
          live.pop_back();
        }
      }
      for (auto [p, bytes] : live)
        mem::deallocate(p, bytes, 16);
    });
  }
  for (auto& th : threads)
    th.join();

  CHECK(mem::total_stats().bytes_current == total_before.bytes_current);
  if (mem::tracking_enabled()) {
    CHECK(mem::stats(tag).bytes_current == before.bytes_current);
    CHECK(mem::stats(tag).allocation_count == before.allocation_count + k_threads * k_per_thread);
  }
}

TEST_CASE("memory: DefaultAlloc satisfies the policy and is empty") {
  static_assert(mem::AllocatorPolicy<mem::DefaultAlloc>);
  static_assert(std::is_empty_v<mem::DefaultAlloc>);
  mem::DefaultAlloc alloc;
  void* p = alloc.allocate(64, 16);
  REQUIRE(p != nullptr);
  alloc.deallocate(p, 64, 16);
}
