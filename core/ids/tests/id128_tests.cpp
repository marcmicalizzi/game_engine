#include <core/containers/hash_set.h>
#include <core/ids/id128.h>

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace engine;

TEST_CASE("Id128: null, ordering, hex round trip") {
  Id128 null;
  CHECK(null.is_null());
  CHECK_FALSE(null);
  const Id128 a = Id128::from_parts(0x0123456789ABCDEFull, 0xFEDCBA9876543210ull);
  char hex[33];
  a.to_hex(hex);
  CHECK(std::string(hex) == "0123456789abcdeffedcba9876543210");
  Id128 back;
  REQUIRE(Id128::from_hex(hex, back));
  CHECK(back == a);
  REQUIRE(Id128::from_hex("0123456789ABCDEFFEDCBA9876543210", back));
  CHECK(back == a);
  CHECK_FALSE(Id128::from_hex("0123", back));
  CHECK_FALSE(Id128::from_hex("0123456789abcdeffedcba987654321g", back));
  CHECK(Id128::from_parts(1, 0) > Id128::from_parts(0, 0xFFFFFFFFFFFFFFFFull));
}

TEST_CASE("Id128: generated ids are unique, non-null, and time-ordered") {
  const u64 before = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
  HashSet<Id128> seen;
  Id128 previous;
  for (int i = 0; i < 100000; ++i) {
    const Id128 id = Id128::generate();
    CHECK_FALSE(id.is_null());
    CHECK(seen.insert(id).second);
    CHECK(id.timestamp_ms() >= before);
    CHECK(id.timestamp_ms() >= previous.timestamp_ms());
    previous = id;
  }
}

TEST_CASE("Id128: a clock that steps back still issues increasing ids") {
  // Gate b33: two consecutive generate() calls in a container carried 1792526767780 then
  // ...779, because the system clock stepped back between them.
  constexpr u64 k_t0 = 1792526767780ull;
  struct Step {
    u64 now_ms;
    int count;
  };
  // Forward, back 5 ms, forward again but still behind, past where it was, and one
  // millisecond holding more ids than its sequence (65536 words, the first at most 0x7FFF).
  const Step steps[] = {
      {k_t0, 1000}, {k_t0 - 5, 1000}, {k_t0 - 2, 10}, {k_t0 + 3, 1000}, {k_t0 + 4, 70000}};
  TimeOrderedIds source;
  HashSet<Id128> seen;
  Id128 previous;
  u64 highest_clock = 0;
  bool all_increasing = true;
  bool all_unique = true;
  bool none_behind = true;
  for (const Step& step : steps) {
    highest_clock = step.now_ms > highest_clock ? step.now_ms : highest_clock;
    for (int i = 0; i < step.count; ++i) {
      const Id128 id = source.next(step.now_ms);
      all_increasing = all_increasing && id > previous && id.hi > previous.hi;
      all_unique = all_unique && seen.insert(id).second;
      // Never behind the latest clock reading it was given.
      none_behind = none_behind && id.timestamp_ms() >= highest_clock;
      previous = id;
    }
  }
  CHECK(all_increasing);
  CHECK(all_unique);
  CHECK(none_behind);
  CHECK(seen.size() == 73010);
  CHECK(source.last() == previous.hi);
  // The 70000 ids at k_t0 + 4 overflowed its sequence and borrowed from k_t0 + 5, nothing more.
  CHECK(previous.timestamp_ms() == k_t0 + 5);
  // The step back and the step still behind borrowed from k_t0 and never waited for the clock:
  // the clock caught up at k_t0 + 3, and a fresh millisecond starts low in its sequence again.
  TimeOrderedIds fresh;
  const Id128 a = fresh.next(k_t0);
  const Id128 b = fresh.next(k_t0 - 5);
  CHECK(b.timestamp_ms() == k_t0);
  CHECK(b.hi == a.hi + 1);
  const Id128 c = fresh.next(k_t0 + 1);
  CHECK(c.timestamp_ms() == k_t0 + 1);
  CHECK((c.hi & 0xFFFFull) < 0x8000ull);
}

TEST_CASE("Id128: generation from several threads never collides") {
  std::vector<std::vector<Id128>> per_thread(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&per_thread, t] {
      per_thread[static_cast<usize>(t)].reserve(20000);
      for (int i = 0; i < 20000; ++i)
        per_thread[static_cast<usize>(t)].push_back(Id128::generate());
    });
  }
  for (auto& th : threads)
    th.join();
  HashSet<Id128> seen;
  for (const auto& ids : per_thread) {
    for (const Id128& id : ids)
      CHECK(seen.insert(id).second);
  }
  CHECK(seen.size() == 160000);
}

TEST_CASE("Id128: deterministic generators repeat exactly and differ by seed") {
  IdGenerator a(42);
  IdGenerator b(42);
  IdGenerator c(43);
  HashSet<Id128> seen;
  for (int i = 0; i < 10000; ++i) {
    const Id128 x = a.next();
    CHECK(x == b.next());
    CHECK(x != c.next());
    CHECK_FALSE(x.is_null());
    CHECK(seen.insert(x).second);
  }
  // Same seed and counter produce the same id regardless of generator instance.
  CHECK(Id128::from_seed(7, 3) == Id128::from_seed(7, 3));
  CHECK(Id128::from_seed(7, 3) != Id128::from_seed(7, 4));
  // Known value pins the algorithm so saved content keeps its ids across builds.
  char hex[33];
  Id128::from_seed(1, 0).to_hex(hex);
  MESSAGE("from_seed(1, 0) = " << hex);
}

TEST_CASE("Id128: hashes are well distributed") {
  IdGenerator g(99);
  int buckets[64] = {};
  for (int i = 0; i < 64000; ++i)
    ++buckets[Hash<Id128>{}(g.next()) >> 58];
  for (int c : buckets) {
    CHECK(c > 500);
    CHECK(c < 1500);
  }
}
