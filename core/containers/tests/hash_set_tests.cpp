#include <core/containers/hash_set.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>  // oracle for the randomized test; tests may use standard containers
#include <vector>

using namespace engine;

namespace {

struct Tracked {
  static inline int live = 0;
  std::string payload;
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

}  // namespace

template <>
struct engine::Hash<Tracked> {
  u64 operator()(const Tracked& t) const noexcept { return StringHash{}(t.payload); }
};

TEST_CASE("HashSet: basics") {
  HashSet<int> s;
  CHECK(s.empty());
  CHECK(s.insert(3).second);
  CHECK(s.insert(1).second);
  CHECK_FALSE(s.insert(3).second);
  CHECK(s.size() == 2);
  CHECK(s.contains(1));
  CHECK_FALSE(s.contains(2));
  CHECK(*s.find(3) == 3);
  CHECK(s.find(9) == s.end());
  CHECK(s.erase(1) == 1);
  CHECK(s.erase(1) == 0);
  CHECK(s.size() == 1);
  static_assert(std::is_same_v<HashSet<int>::iterator, const int*>);
}

TEST_CASE("HashSet: erase by iterator swaps in the last element and the loop terminates") {
  HashSet<int> s;
  for (int k = 0; k < 100; ++k)
    s.insert(k);
  for (auto it = s.begin(); it != s.end();) {
    if (*it % 2 == 0) {
      it = s.erase(it);
    } else {
      ++it;
    }
  }
  CHECK(s.size() == 50);
  for (int k = 0; k < 100; ++k)
    CHECK(s.contains(k) == (k % 2 == 1));
}

TEST_CASE("HashSet: heterogeneous lookup and tracked keys") {
  Tracked::live = 0;
  {
    HashSet<std::string> names;
    names.insert("a");
    names.insert(std::string_view{"b"});
    CHECK(names.contains("a"));
    CHECK(names.contains(std::string_view{"b"}));
    CHECK(names.erase("a") == 1);

    HashSet<Tracked> s;
    for (int k = 0; k < 1000; ++k)
      s.insert(Tracked(std::to_string(k % 700)));
    CHECK(s.size() == 700);
    CHECK(Tracked::live == 700);
    for (int k = 0; k < 700; k += 5)
      CHECK(s.erase(Tracked(std::to_string(k))) == 1);
    CHECK(Tracked::live == static_cast<int>(s.size()));
    HashSet<Tracked> copy = s;
    CHECK(copy == s);
    auto x = copy.extract(Tracked("1"));
    REQUIRE(x.has_value());
    CHECK(x->payload == "1");
    CHECK_FALSE(copy == s);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("HashSet: insert_bulk and standard algorithms over the dense array") {
  HashSet<int> s;
  std::vector<int> batch = {5, 3, 5, 9, 1, 3, 7};
  s.insert_bulk(batch);
  CHECK(s.size() == 5);
  std::vector<int> sorted(s.begin(), s.end());
  std::sort(sorted.begin(), sorted.end());
  CHECK(sorted == std::vector<int>{1, 3, 5, 7, 9});
}

TEST_CASE("HashSet: randomized operations agree with std::unordered_set") {
  HashSet<u32> s;
  std::unordered_set<u32> oracle;
  Lcg rng{0xA3B1C5D7E9F0A1B2ull};
  for (int step = 0; step < 50000; ++step) {
    const u32 op = rng.next() % 3;
    const u32 key = rng.next() % 3000;
    if (op == 0) {
      CHECK(s.insert(key).second == oracle.insert(key).second);
    } else if (op == 1) {
      CHECK(s.erase(key) == oracle.erase(key));
    } else {
      CHECK(s.contains(key) == (oracle.count(key) == 1));
    }
    REQUIRE(s.size() == oracle.size());
  }
  for (u32 k : s)
    CHECK(oracle.count(k) == 1);
}
