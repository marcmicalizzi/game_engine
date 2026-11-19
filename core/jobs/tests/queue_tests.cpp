#include <core/jobs/detail/mpmc_queue.h>
#include <core/jobs/detail/work_deque.h>

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::jobs::detail;

TEST_CASE("WorkDeque: owner push/pop is LIFO and reports full") {
  WorkDeque<int> d(8);
  for (int i = 0; i < 8; ++i)
    CHECK(d.push(i));
  CHECK_FALSE(d.push(99));
  CHECK(d.size() == 8);
  int v;
  for (int i = 7; i >= 0; --i) {
    REQUIRE(d.pop(v));
    CHECK(v == i);
  }
  CHECK_FALSE(d.pop(v));
  // Wraps around the ring.
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 5; ++i)
      CHECK(d.push(i));
    for (int i = 0; i < 5; ++i)
      REQUIRE(d.pop(v));
  }
}

TEST_CASE("WorkDeque: thieves take from the top; every item is taken exactly once") {
  constexpr int k_items = 1 << 16;
  WorkDeque<int> d(1 << 17);
  std::atomic<int> taken{0};
  std::atomic<long long> sum{0};
  std::atomic<bool> go{false};
  std::atomic<bool> done_pushing{false};

  auto thief = [&] {
    while (!go.load()) {
    }
    int v;
    while (!done_pushing.load() || d.size() > 0) {
      if (d.steal(v)) {
        taken.fetch_add(1);
        sum.fetch_add(v);
      }
    }
  };
  std::vector<std::thread> thieves;
  for (int i = 0; i < 4; ++i)
    thieves.emplace_back(thief);

  go.store(true);
  long long owner_sum = 0;
  int owner_taken = 0;
  for (int i = 0; i < k_items; ++i) {
    REQUIRE(d.push(i));
    if ((i & 3) == 0) {
      int v;
      if (d.pop(v)) {
        ++owner_taken;
        owner_sum += v;
      }
    }
  }
  done_pushing.store(true);
  int v;
  while (d.pop(v)) {
    ++owner_taken;
    owner_sum += v;
  }
  for (auto& t : thieves)
    t.join();
  // Drain anything left after thieves exited.
  while (d.pop(v)) {
    ++owner_taken;
    owner_sum += v;
  }
  CHECK(taken.load() + owner_taken == k_items);
  CHECK(sum.load() + owner_sum == static_cast<long long>(k_items) * (k_items - 1) / 2);
}

// A ring of eight slots, so every slot is written again thousands of times while thieves read it:
// a thief's read before its CAS meets the owner's rewrite of the same slot here (T186), which the
// case above, whose ring never wraps, cannot reach. Each item is two words that must agree, so a
// steal that kept a torn or stale copy shows as a pair that does not match.
TEST_CASE("WorkDeque: a small ring reused under thieves hands each item over once and whole") {
  struct Pair {
    u64 value;
    u64 check;
  };
  constexpr u64 k_items = 200000;
  constexpr u64 k_mix = 0x9E3779B97F4A7C15ull;
  WorkDeque<Pair> d(8);
  std::atomic<u64> taken{0};
  std::atomic<u64> sum{0};
  std::atomic<u64> torn{0};
  std::atomic<bool> done_pushing{false};
  auto take = [&](const Pair& p) {
    if (p.check != (p.value ^ k_mix)) torn.fetch_add(1);
    taken.fetch_add(1);
    sum.fetch_add(p.value);
  };
  auto thief = [&] {
    Pair p;
    while (!done_pushing.load() || d.size() > 0) {
      if (d.steal(p)) take(p);
    }
  };
  std::vector<std::thread> thieves;
  for (int i = 0; i < 3; ++i)
    thieves.emplace_back(thief);
  Pair p;
  for (u64 i = 0; i < k_items; ++i) {
    while (!d.push(Pair{i, i ^ k_mix})) {
      if (d.pop(p)) take(p);  // full: the owner takes its newest back, as a worker would
    }
    if ((i & 7) == 0 && d.pop(p)) take(p);
  }
  done_pushing.store(true);
  for (auto& t : thieves)
    t.join();
  while (d.pop(p))
    take(p);
  CHECK(torn.load() == 0);
  CHECK(taken.load() == k_items);
  CHECK(sum.load() == k_items * (k_items - 1) / 2);
}

TEST_CASE("MpmcQueue: FIFO single-threaded and full/empty reporting") {
  MpmcQueue<int> q(4);
  int v;
  CHECK_FALSE(q.try_pop(v));
  for (int i = 0; i < 4; ++i)
    CHECK(q.try_push(i));
  CHECK_FALSE(q.try_push(4));
  for (int i = 0; i < 4; ++i) {
    REQUIRE(q.try_pop(v));
    CHECK(v == i);
  }
  CHECK_FALSE(q.try_pop(v));
  for (int round = 0; round < 10; ++round) {
    CHECK(q.try_push(round));
    REQUIRE(q.try_pop(v));
    CHECK(v == round);
  }
}

TEST_CASE("MpmcQueue: many producers and consumers move every item exactly once") {
  constexpr int k_producers = 4;
  constexpr int k_consumers = 4;
  constexpr int k_per_producer = 20000;
  MpmcQueue<int> q(1024);
  std::atomic<int> consumed{0};
  std::atomic<long long> sum{0};
  std::atomic<int> producers_done{0};

  std::vector<std::thread> threads;
  for (int p = 0; p < k_producers; ++p) {
    threads.emplace_back([&, p] {
      for (int i = 0; i < k_per_producer; ++i) {
        while (!q.try_push(p * k_per_producer + i)) {
        }
      }
      producers_done.fetch_add(1);
    });
  }
  for (int c = 0; c < k_consumers; ++c) {
    threads.emplace_back([&] {
      int v;
      while (true) {
        if (q.try_pop(v)) {
          consumed.fetch_add(1);
          sum.fetch_add(v);
        } else if (producers_done.load() == k_producers) {
          if (!q.try_pop(v)) break;
          consumed.fetch_add(1);
          sum.fetch_add(v);
        }
      }
    });
  }
  for (auto& t : threads)
    t.join();
  const long long n = static_cast<long long>(k_producers) * k_per_producer;
  CHECK(consumed.load() == n);
  CHECK(sum.load() == n * (n - 1) / 2);
}
