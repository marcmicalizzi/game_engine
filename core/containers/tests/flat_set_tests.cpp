#include <core/containers/flat_set.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <set>  // oracle for the randomized test; tests may use standard containers
#include <string>
#include <string_view>
#include <vector>

using namespace engine;

namespace {

struct Tracked {
  static inline int live = 0;
  std::string payload;
  explicit Tracked(std::string p) : payload(std::move(p)) { ++live; }
  Tracked(const Tracked& o) : payload(o.payload) { ++live; }
  Tracked(Tracked&& o) noexcept : payload(std::move(o.payload)) { ++live; }
  Tracked& operator=(const Tracked& o) = default;
  Tracked& operator=(Tracked&& o) noexcept = default;
  ~Tracked() { --live; }
  bool operator<(const Tracked& o) const { return payload < o.payload; }
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

TEST_CASE("FlatSet: empty set behaves") {
  FlatSet<int> s;
  CHECK(s.empty());
  CHECK(s.size() == 0);
  CHECK(s.find(1) == s.end());
  CHECK_FALSE(s.contains(1));
  CHECK(s.erase(1) == 0);
  CHECK(s.begin() == s.end());
}

TEST_CASE("FlatSet: insert keeps keys sorted and unique") {
  FlatSet<int> s;
  for (int k : {5, 1, 9, 3, 7, 1, 5, 2, 8, 0, 6, 4})
    s.insert(k);
  CHECK(s.size() == 10);
  CHECK(std::is_sorted(s.begin(), s.end()));
  for (int k = 0; k < 10; ++k) {
    CHECK(s.contains(k));
    CHECK(s.at(static_cast<u32>(k)) == k);
  }
  CHECK(s.front() == 0);
  CHECK(s.back() == 9);
  auto [it, inserted] = s.insert(4);
  CHECK_FALSE(inserted);
  CHECK(*it == 4);
}

TEST_CASE("FlatSet: iterators are plain pointers usable with standard algorithms") {
  FlatSet<int> s;
  for (int k : {4, 2, 8, 6})
    s.insert(k);
  CHECK(std::count_if(s.begin(), s.end(), [](int k) { return k > 3; }) == 3);
  CHECK(std::binary_search(s.begin(), s.end(), 6));
  static_assert(std::is_same_v<FlatSet<int>::iterator, const int*>);
}

TEST_CASE("FlatSet: heterogeneous lookup") {
  FlatSet<std::string> s;
  s.insert("pear");
  s.insert("fig");
  s.insert(std::string_view{"kiwi"});
  CHECK(s.contains(std::string_view{"fig"}));
  CHECK(s.erase(std::string_view{"pear"}) == 1);
  CHECK(s.size() == 2);
  CHECK(*s.lower_bound("g") == "kiwi");
}

TEST_CASE("FlatSet: erase and extract") {
  FlatSet<int> s;
  for (int k = 0; k < 10; ++k)
    s.insert(k);
  CHECK(s.erase(5) == 1);
  auto it = s.erase(s.find(0));
  CHECK(*it == 1);
  s.erase_at(s.size() - 1);
  CHECK_FALSE(s.contains(9));
  auto x = s.extract(4);
  REQUIRE(x.has_value());
  CHECK(*x == 4);
  CHECK_FALSE(s.contains(4));
  CHECK(s.size() == 6);
  CHECK(std::is_sorted(s.begin(), s.end()));
}

TEST_CASE("FlatSet: growth with non-trivial keys leaks nothing") {
  Tracked::live = 0;
  {
    FlatSet<Tracked> s;
    for (int k = 0; k < 150; ++k) {
      s.insert(Tracked(std::to_string((k * 53) % 150)));
      REQUIRE(Tracked::live == static_cast<int>(s.size()));
    }
    CHECK(s.size() == 150);
    CHECK(std::is_sorted(s.begin(), s.end()));
    for (int k = 0; k < 150; k += 2)
      s.erase(Tracked(std::to_string(k)));
    CHECK(Tracked::live == static_cast<int>(s.size()));
    FlatSet<Tracked> copy = s;
    CHECK(copy == s);
    FlatSet<Tracked> moved = std::move(copy);
    CHECK(copy.empty());
    CHECK(moved == s);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("FlatSet: append_sorted and insert_bulk") {
  FlatSet<int> s;
  for (int k = 0; k < 10; ++k)
    s.append_sorted(k * 3);
  CHECK(s.size() == 10);
  std::vector<int> batch = {4, 3, 3, 30, 0, 7, 7, 7, 100};
  s.insert_bulk(batch);
  CHECK(std::is_sorted(s.begin(), s.end()));
  CHECK(std::adjacent_find(s.begin(), s.end()) == s.end());
  CHECK(s.size() ==
        14);  // 0,3,6,...,27 (10) plus the new keys 4, 7, 30, 100; 0 and 3 already present
  CHECK(s.contains(100));
  CHECK(s.contains(4));
}

TEST_CASE("FlatSet: randomized operations agree with std::set") {
  FlatSet<u32> s;
  std::set<u32> oracle;
  Lcg rng{0xD1B54A32D192ED03ull};
  for (int step = 0; step < 20000; ++step) {
    const u32 op = rng.next() % 3;
    const u32 key = rng.next() % 300;
    if (op == 0) {
      CHECK(s.insert(key).second == oracle.insert(key).second);
    } else if (op == 1) {
      CHECK(s.erase(key) == oracle.erase(key));
    } else {
      CHECK(s.contains(key) == (oracle.count(key) == 1));
    }
    REQUIRE(s.size() == oracle.size());
  }
  CHECK(std::equal(s.begin(), s.end(), oracle.begin(), oracle.end()));
}
