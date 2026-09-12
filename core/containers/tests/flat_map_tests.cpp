#include <core/containers/flat_map.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <map>  // oracle for the randomized test; tests may use standard containers
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace engine;

namespace {

// Counts live instances so relocation, erase, and destruction bugs show up as leaks or
// double-destroys. Deliberately not trivially copyable so the move/destroy paths are exercised.
struct Tracked {
  static inline int live = 0;
  static inline int moves = 0;
  std::string payload;

  Tracked() : payload("default") { ++live; }
  explicit Tracked(std::string p) : payload(std::move(p)) { ++live; }
  Tracked(const Tracked& o) : payload(o.payload) { ++live; }
  Tracked(Tracked&& o) noexcept : payload(std::move(o.payload)) {
    ++live;
    ++moves;
  }
  Tracked& operator=(const Tracked& o) {
    payload = o.payload;
    return *this;
  }
  Tracked& operator=(Tracked&& o) noexcept {
    payload = std::move(o.payload);
    ++moves;
    return *this;
  }
  ~Tracked() { --live; }
  bool operator==(const Tracked& o) const { return payload == o.payload; }
};

static_assert(!std::is_trivially_copyable_v<Tracked>);

template <class Map>
void check_sorted_invariant(const Map& m) {
  const auto keys = m.keys();
  for (usize i = 1; i < keys.size(); ++i) {
    CHECK(keys[i - 1] < keys[i]);
  }
  CHECK(m.keys().size() == m.size());
  CHECK(m.values().size() == m.size());
}

// Small deterministic generator so the randomized test is reproducible.
struct Lcg {
  u64 state;
  u32 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(state >> 33);
  }
};

}  // namespace

TEST_CASE("FlatMap: empty map behaves") {
  FlatMap<int, int> m;
  CHECK(m.empty());
  CHECK(m.size() == 0);
  CHECK(m.capacity() == 0);
  CHECK(m.find(1) == m.end());
  CHECK_FALSE(m.contains(1));
  CHECK(m.find_value(1) == nullptr);
  CHECK(m.erase(1) == 0);
  CHECK(m.begin() == m.end());
  CHECK(m.keys().empty());
  CHECK(m.values().empty());
  m.clear();
  m.shrink_to_fit();
  CHECK(m.empty());
}

TEST_CASE("FlatMap: insert keeps keys sorted and unique") {
  FlatMap<int, int> m;
  const int keys[] = {5, 1, 9, 3, 7, 1, 5, 2, 8, 0, 6, 4};
  for (int k : keys) {
    auto [it, inserted] = m.insert(k, k * 10);
    CHECK(it->first == k);
    CHECK(it->second == k * 10);
  }
  CHECK(m.size() == 10);
  check_sorted_invariant(m);
  for (int k = 0; k < 10; ++k) {
    REQUIRE(m.contains(k));
    CHECK(*m.find_value(k) == k * 10);
    CHECK(m.find(k)->second == k * 10);
    CHECK(m.key_at(static_cast<u32>(k)) == k);
    CHECK(m.value_at(static_cast<u32>(k)) == k * 10);
  }
  CHECK_FALSE(m.contains(10));
  CHECK_FALSE(m.contains(-1));
}

TEST_CASE("FlatMap: insert reports existing keys without overwriting") {
  FlatMap<int, std::string> m;
  CHECK(m.insert(1, "one").second);
  auto [it, inserted] = m.insert(1, "uno");
  CHECK_FALSE(inserted);
  CHECK(it->second == "one");
  CHECK(m.size() == 1);
}

TEST_CASE("FlatMap: insert_or_assign overwrites and reports") {
  FlatMap<int, std::string> m;
  CHECK(m.insert_or_assign(1, "one").second);
  auto [it, inserted] = m.insert_or_assign(1, "uno");
  CHECK_FALSE(inserted);
  CHECK(it->second == "uno");
  CHECK(m.size() == 1);
}

TEST_CASE("FlatMap: operator[] value-initializes and returns a stable slot until mutation") {
  FlatMap<int, int> m;
  CHECK(m[3] == 0);
  m[3] = 30;
  m[1] = 10;
  CHECK(m[3] == 30);
  CHECK(m[1] == 10);
  CHECK(m.size() == 2);
  check_sorted_invariant(m);
}

TEST_CASE("FlatMap: heterogeneous lookup with string keys and string_view queries") {
  FlatMap<std::string, int> m;
  m.insert("banana", 2);
  m.insert("apple", 1);
  m.insert("cherry", 3);
  const std::string_view query = "banana";
  CHECK(m.contains(query));
  CHECK(*m.find_value(query) == 2);
  CHECK(m.find("zzz") == m.end());
  CHECK(m.erase(std::string_view{"apple"}) == 1);
  CHECK(m.size() == 2);
  CHECK(m.key_at(0) == "banana");
  // operator[] with a string_view constructs the std::string key.
  m[std::string_view{"date"}] = 4;
  CHECK(m.size() == 3);
  check_sorted_invariant(m);
}

TEST_CASE("FlatMap: erase by key, by iterator, and by index") {
  FlatMap<int, int> m;
  for (int k = 0; k < 10; ++k) m.insert(k, k);

  CHECK(m.erase(5) == 1);
  CHECK(m.erase(5) == 0);
  CHECK(m.size() == 9);
  CHECK_FALSE(m.contains(5));

  auto it = m.erase(m.find(0));
  CHECK(it->first == 1);
  CHECK(m.size() == 8);

  m.erase_at(m.size() - 1);  // removes 9
  CHECK_FALSE(m.contains(9));
  CHECK(m.size() == 7);
  check_sorted_invariant(m);

  // Erase the rest through the iterator loop.
  for (auto pos = m.begin(); pos != m.end();) pos = m.erase(pos);
  CHECK(m.empty());
}

TEST_CASE("FlatMap: extract moves the value out and removes the entry") {
  FlatMap<int, std::string> m;
  m.insert(1, "one");
  m.insert(2, "two");
  auto v = m.extract(1);
  REQUIRE(v.has_value());
  CHECK(*v == "one");
  CHECK(m.size() == 1);
  CHECK_FALSE(m.contains(1));
  CHECK_FALSE(m.extract(1).has_value());
}

TEST_CASE("FlatMap: iteration is in key order and supports structured bindings") {
  FlatMap<int, int> m;
  for (int k : {3, 1, 2}) m.insert(k, k * 100);
  std::vector<int> seen;
  for (auto [k, v] : m) {
    seen.push_back(k);
    v += 1;  // v binds to the stored value
  }
  CHECK(seen == std::vector<int>{1, 2, 3});
  CHECK(*m.find_value(1) == 101);
  CHECK(*m.find_value(3) == 301);

  const FlatMap<int, int>& cm = m;
  int sum = 0;
  for (auto [k, v] : cm) sum += k + v;
  CHECK(sum == 1 + 101 + 2 + 201 + 3 + 301);

  // Random access and iterator arithmetic.
  auto it = m.begin();
  CHECK((it + 2)->first == 3);
  CHECK((m.end() - m.begin()) == 3);
  CHECK(it[1].first == 2);
  FlatMap<int, int>::const_iterator cit = it;  // non-const converts to const
  CHECK(cit == it);
  CHECK(cit < m.end());
  CHECK(std::distance(m.begin(), m.end()) == 3);
}

TEST_CASE("FlatMap: lower_bound and upper_bound") {
  FlatMap<int, int> m;
  for (int k : {10, 20, 30}) m.insert(k, k);
  CHECK(m.lower_bound(20)->first == 20);
  CHECK(m.upper_bound(20)->first == 30);
  CHECK(m.lower_bound(25)->first == 30);
  CHECK(m.lower_bound(35) == m.end());
  CHECK(m.lower_bound_index(5) == 0);
  CHECK(m.upper_bound_index(30) == 3);
}

TEST_CASE("FlatMap: growth relocates non-trivial elements exactly and leaks nothing") {
  Tracked::live = 0;
  {
    FlatMap<int, Tracked> m;
    // Insert in an order that forces middle insertions across several reallocations.
    for (int k = 0; k < 200; ++k) {
      const int key = (k * 37) % 200;
      m.try_emplace(key, std::to_string(key));
      REQUIRE(Tracked::live == static_cast<int>(m.size()));
    }
    CHECK(m.size() == 200);
    check_sorted_invariant(m);
    for (int k = 0; k < 200; ++k) {
      REQUIRE(m.contains(k));
      CHECK(m.find_value(k)->payload == std::to_string(k));
    }
    // Erase every third from the middle outward.
    for (int k = 0; k < 200; k += 3) m.erase(k);
    CHECK(Tracked::live == static_cast<int>(m.size()));
    check_sorted_invariant(m);
    m.shrink_to_fit();
    CHECK(m.capacity() == m.size());
    CHECK(Tracked::live == static_cast<int>(m.size()));
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("FlatMap: copy and move semantics") {
  Tracked::live = 0;
  {
    FlatMap<int, Tracked> a;
    for (int k = 0; k < 20; ++k) a.try_emplace(k, std::to_string(k));

    FlatMap<int, Tracked> b = a;  // copy
    CHECK(b == a);
    CHECK(Tracked::live == 40);
    b.find_value(3)->payload = "changed";
    CHECK_FALSE(b == a);

    FlatMap<int, Tracked> c = std::move(a);  // move steals storage
    CHECK(a.empty());
    CHECK(a.capacity() == 0);
    CHECK(c.size() == 20);
    CHECK(Tracked::live == 40);

    a = c;  // copy assign into moved-from
    CHECK(a == c);
    CHECK(Tracked::live == 60);

    b = std::move(c);  // move assign releases b's old contents
    CHECK(c.empty());
    CHECK(b.size() == 20);
    CHECK(Tracked::live == 40);

    swap(a, b);
    CHECK(a.size() == 20);
    CHECK(b.size() == 20);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("FlatMap: reserve avoids reallocation and clear keeps capacity") {
  FlatMap<int, int> m;
  m.reserve(100);
  CHECK(m.capacity() >= 100);
  const auto cap = m.capacity();
  for (int k = 0; k < 100; ++k) m.insert(k, k);
  CHECK(m.capacity() == cap);
  m.clear();
  CHECK(m.empty());
  CHECK(m.capacity() == cap);
  m.shrink_to_fit();
  CHECK(m.capacity() == 0);
}

TEST_CASE("FlatMap: append_sorted builds from sorted input without searching") {
  FlatMap<int, int> m;
  for (int k = 0; k < 50; ++k) m.append_sorted(k * 2, k);
  CHECK(m.size() == 50);
  check_sorted_invariant(m);
  CHECK(*m.find_value(98) == 49);
}

TEST_CASE("FlatMap: insert_bulk sorts, dedupes with last-wins, and overrides existing") {
  FlatMap<int, std::string> m;
  m.insert(5, "existing-five");
  m.insert(100, "existing-hundred");

  std::vector<std::pair<int, std::string>> batch = {
      {3, "three"}, {1, "one"}, {5, "five-a"}, {2, "two"}, {5, "five-b"}, {1, "one-late"}};
  m.insert_bulk(batch);

  CHECK(m.size() == 5);
  check_sorted_invariant(m);
  CHECK(*m.find_value(1) == "one-late");
  CHECK(*m.find_value(2) == "two");
  CHECK(*m.find_value(3) == "three");
  CHECK(*m.find_value(5) == "five-b");
  CHECK(*m.find_value(100) == "existing-hundred");

  // Empty batch is a no-op.
  std::vector<std::pair<int, std::string>> none;
  m.insert_bulk(none.begin(), none.end());
  CHECK(m.size() == 5);
}

TEST_CASE("FlatMap: insert_bulk with tracked values leaks nothing") {
  Tracked::live = 0;
  {
    FlatMap<int, Tracked> m;
    std::vector<std::pair<int, Tracked>> batch;
    for (int k = 0; k < 100; ++k) batch.emplace_back(k % 40, Tracked(std::to_string(k)));
    m.insert_bulk(batch);
    CHECK(m.size() == 40);
    CHECK(Tracked::live == 40 + static_cast<int>(batch.size()));
    for (int k = 0; k < 40; ++k) {
      // Key k appears at batch indices k, k + 40, and (for k < 20) k + 80; the last one wins.
      const int last = (k + 80 < 100) ? k + 80 : k + 40;
      CHECK(m.find_value(k)->payload == std::to_string(last));
    }
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("FlatMap: randomized operations agree with std::map") {
  FlatMap<u32, u32> m;
  std::map<u32, u32> oracle;
  Lcg rng{0x9E3779B97F4A7C15ull};

  for (int step = 0; step < 20000; ++step) {
    const u32 op = rng.next() % 10;
    const u32 key = rng.next() % 512;
    const u32 val = rng.next();
    if (op < 4) {
      const bool a = m.insert(key, val).second;
      const bool b = oracle.emplace(key, val).second;
      CHECK(a == b);
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
  // Final full comparison in order.
  auto oit = oracle.begin();
  for (auto [k, v] : m) {
    REQUIRE(oit != oracle.end());
    CHECK(k == oit->first);
    CHECK(v == oit->second);
    ++oit;
  }
  CHECK(oit == oracle.end());
}

TEST_CASE("FlatMap: custom comparator and 64-bit size type") {
  FlatMap<int, int, std::greater<>> desc;
  for (int k : {1, 3, 2}) desc.insert(k, k);
  CHECK(desc.key_at(0) == 3);
  CHECK(desc.key_at(2) == 1);
  CHECK(desc.contains(2));

  FlatMap<u64, u64, std::less<>, u64> big;
  big.insert(1, 2);
  CHECK(big.size() == 1);
  CHECK(big.max_size() == std::numeric_limits<u64>::max());
}
