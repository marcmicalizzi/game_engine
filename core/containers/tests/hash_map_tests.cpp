#include <core/containers/hash_map.h>

#include <core/memory/arena.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>  // oracle for the randomized test; tests may use standard containers
#include <utility>
#include <vector>

using namespace engine;

namespace {

struct Tracked {
  static inline int live = 0;
  std::string payload;
  Tracked() : payload("default") { ++live; }
  explicit Tracked(std::string p) : payload(std::move(p)) { ++live; }
  Tracked(const Tracked& o) : payload(o.payload) { ++live; }
  Tracked(Tracked&& o) noexcept : payload(std::move(o.payload)) { ++live; }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --live; }
  bool operator==(const Tracked& o) const { return payload == o.payload; }
};

struct Lcg {
  u64 state;
  u32 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(state >> 33);
  }
};

// A deliberately terrible hash to exercise long probe runs and displacement.
struct BadHash {
  u64 operator()(int v) const noexcept { return static_cast<u64>(v % 4) << 60; }
};

}  // namespace

TEST_CASE("HashMap: empty map behaves") {
  HashMap<int, int> m;
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.bucket_count() == 0);
  CHECK(m.find(1) == m.end());
  CHECK_FALSE(m.contains(1));
  CHECK(m.find_value(1) == nullptr);
  CHECK(m.erase(1) == 0);
  CHECK(m.begin() == m.end());
  m.clear();
  m.shrink_to_fit();
  CHECK(m.empty());
}

TEST_CASE("HashMap: insert, find, overwrite") {
  HashMap<int, std::string> m;
  for (int k = 0; k < 100; ++k) {
    auto [it, inserted] = m.insert(k, std::to_string(k));
    CHECK(inserted);
    CHECK(it->first == k);
  }
  CHECK(m.size() == 100);
  for (int k = 0; k < 100; ++k) {
    REQUIRE(m.contains(k));
    CHECK(*m.find_value(k) == std::to_string(k));
  }
  CHECK_FALSE(m.insert(5, "x").second);
  CHECK(*m.find_value(5) == "5");
  CHECK_FALSE(m.insert_or_assign(5, "five").second);
  CHECK(*m.find_value(5) == "five");
  CHECK(m.insert_or_assign(1000, "thousand").second);
  CHECK(m.size() == 101);
  m[2000] = "two thousand";
  CHECK(m[2000] == "two thousand");
  CHECK(m[3000].empty());  // value-initialized
  CHECK(m.size() == 103);
}

TEST_CASE("HashMap: load factor stays under 80% and rehash preserves contents") {
  HashMap<int, int> m;
  for (int k = 0; k < 10000; ++k) {
    m.insert(k * 7919, k);
    CHECK(static_cast<u64>(m.size()) * 5 <= static_cast<u64>(m.bucket_count()) * 4);
  }
  for (int k = 0; k < 10000; ++k) {
    const int* v = m.find_value(k * 7919);
    REQUIRE(v != nullptr);
    CHECK(*v == k);
  }
  CHECK_FALSE(m.contains(-1));
  CHECK_FALSE(m.contains(7919 * 10000));
}

TEST_CASE("HashMap: erase by key keeps every other element reachable") {
  HashMap<int, int> m;
  for (int k = 0; k < 2000; ++k) m.insert(k, k * 3);
  for (int k = 0; k < 2000; k += 2) CHECK(m.erase(k) == 1);
  CHECK(m.erase(0) == 0);
  CHECK(m.size() == 1000);
  for (int k = 0; k < 2000; ++k) {
    const int* v = m.find_value(k);
    if (k % 2 == 0) {
      CHECK(v == nullptr);
    } else {
      REQUIRE(v != nullptr);
      CHECK(*v == k * 3);
    }
  }
  // Re-insert the erased keys; the freed slots and shifted buckets must accept them.
  for (int k = 0; k < 2000; k += 2) CHECK(m.insert(k, -k).second);
  CHECK(m.size() == 2000);
  for (int k = 0; k < 2000; k += 2) CHECK(*m.find_value(k) == -k);
}

TEST_CASE("HashMap: erase by iterator and erase_at swap in the last element") {
  HashMap<int, int> m;
  for (int k = 0; k < 10; ++k) m.insert(k, k);
  const int last_key = m.key_at(9);
  auto it = m.erase(m.begin());
  CHECK(it.index() == 0);
  CHECK(it->first == last_key);
  CHECK(m.size() == 9);
  // Erase everything through the iterator loop.
  for (auto pos = m.begin(); pos != m.end();) pos = m.erase(pos);
  CHECK(m.empty());
}

TEST_CASE("HashMap: pathological hash still works") {
  HashMap<int, int, BadHash> m;
  for (int k = 0; k < 500; ++k) m.insert(k, k);
  CHECK(m.size() == 500);
  for (int k = 0; k < 500; ++k) REQUIRE(m.contains(k));
  for (int k = 0; k < 500; k += 3) m.erase(k);
  for (int k = 0; k < 500; ++k) CHECK(m.contains(k) == (k % 3 != 0));
}

TEST_CASE("HashMap: heterogeneous lookup with string keys") {
  HashMap<std::string, int> m;
  m.insert("alpha", 1);
  m.insert("beta", 2);
  CHECK(m.contains(std::string_view{"alpha"}));
  CHECK(m.contains("beta"));
  CHECK(*m.find_value(std::string_view{"alpha"}) == 1);
  CHECK(m.erase(std::string_view{"beta"}) == 1);
  m[std::string_view{"gamma"}] = 3;
  CHECK(m.size() == 2);
  CHECK(*m.find_value("gamma") == 3);
}

TEST_CASE("HashMap: extract, iteration, spans") {
  HashMap<int, std::string> m;
  m.insert(1, "one");
  m.insert(2, "two");
  m.insert(3, "three");
  auto v = m.extract(2);
  REQUIRE(v.has_value());
  CHECK(*v == "two");
  CHECK_FALSE(m.extract(2).has_value());
  CHECK(m.size() == 2);

  int key_sum = 0;
  for (auto [k, val] : m) {
    key_sum += k;
    val += "!";
  }
  CHECK(key_sum == 4);
  CHECK(std::all_of(m.values().begin(), m.values().end(),
                    [](const std::string& s) { return s.back() == '!'; }));
  CHECK(m.keys().size() == 2);
  const HashMap<int, std::string>& cm = m;
  for (auto [k, val] : cm) CHECK(val.size() >= 4);
}

TEST_CASE("HashMap: growth and erase with tracked values leak nothing") {
  Tracked::live = 0;
  {
    HashMap<int, Tracked> m;
    for (int k = 0; k < 3000; ++k) {
      m.try_emplace(k, std::to_string(k));
      REQUIRE(Tracked::live == static_cast<int>(m.size()));
    }
    for (int k = 0; k < 3000; k += 2) m.erase(k);
    CHECK(Tracked::live == 1500);
    for (int k = 1; k < 3000; k += 2) CHECK(m.find_value(k)->payload == std::to_string(k));
    m.shrink_to_fit();
    CHECK(m.capacity() == m.size());
    CHECK(Tracked::live == 1500);
    m.clear();
    CHECK(Tracked::live == 0);
    CHECK(m.bucket_count() > 0);  // clear keeps the bucket array
    m.insert(1, Tracked("again"));
    CHECK(m.size() == 1);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("HashMap: copy and move semantics") {
  Tracked::live = 0;
  {
    HashMap<int, Tracked> a;
    for (int k = 0; k < 50; ++k) a.try_emplace(k, std::to_string(k));
    HashMap<int, Tracked> b = a;
    CHECK(b == a);
    CHECK(Tracked::live == 100);
    b.find_value(7)->payload = "changed";
    CHECK_FALSE(b == a);
    HashMap<int, Tracked> c = std::move(a);
    CHECK(a.empty());
    CHECK(a.bucket_count() == 0);
    CHECK(c.size() == 50);
    a = c;
    CHECK(a == c);
    CHECK(Tracked::live == 150);
    b = std::move(c);
    CHECK(c.empty());
    CHECK(b.size() == 50);
    CHECK(Tracked::live == 100);
    swap(a, b);
    CHECK(a.size() == 50);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("HashMap: reserve avoids rehash during fill") {
  HashMap<int, int> m;
  m.reserve(1000);
  const auto buckets = m.bucket_count();
  const auto cap = m.capacity();
  CHECK(cap >= 1000);
  for (int k = 0; k < 1000; ++k) m.insert(k, k);
  CHECK(m.bucket_count() == buckets);
  CHECK(m.capacity() == cap);
}

TEST_CASE("HashMap: arena-backed map allocates from the arena") {
  mem::Arena arena;
  HashMap<int, int, Hash<int>, std::equal_to<>, u32, mem::ArenaAlloc> m(mem::ArenaAlloc{&arena});
  for (int k = 0; k < 500; ++k) m.insert(k, k);
  CHECK(m.size() == 500);
  CHECK(arena.bytes_allocated() > 500 * 8);
}

TEST_CASE("HashMap: randomized operations agree with std::unordered_map") {
  HashMap<u32, u32> m;
  std::unordered_map<u32, u32> oracle;
  Lcg rng{0x2545F4914F6CDD1Dull};
  for (int step = 0; step < 50000; ++step) {
    const u32 op = rng.next() % 10;
    const u32 key = rng.next() % 4096;
    const u32 val = rng.next();
    if (op < 4) {
      CHECK(m.insert(key, val).second == oracle.emplace(key, val).second);
    } else if (op < 6) {
      m.insert_or_assign(key, val);
      oracle[key] = val;
    } else if (op < 8) {
      CHECK(m.erase(key) == oracle.erase(key));
    } else {
      const u32* p = m.find_value(key);
      auto it = oracle.find(key);
      REQUIRE((p == nullptr) == (it == oracle.end()));
      if (p != nullptr) CHECK(*p == it->second);
    }
    REQUIRE(m.size() == oracle.size());
  }
  for (auto [k, v] : m) {
    auto it = oracle.find(k);
    REQUIRE(it != oracle.end());
    CHECK(it->second == v);
  }
}
