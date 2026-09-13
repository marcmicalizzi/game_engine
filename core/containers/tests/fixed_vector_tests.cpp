#include <core/containers/fixed_vector.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

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

}  // namespace

TEST_CASE("FixedVector: push, try_push, full, pop") {
  FixedVector<int, 3> v;
  CHECK(v.empty());
  CHECK(v.capacity() == 3);
  v.push_back(1);
  v.push_back(2);
  CHECK(v.try_push_back(3));
  CHECK(v.full());
  CHECK_FALSE(v.try_push_back(4));
  CHECK(v.size() == 3);
  v.pop_back();
  CHECK(v.back() == 2);
  CHECK_FALSE(v.full());
}

TEST_CASE("FixedVector: erase, erase_unordered, resize, algorithms") {
  FixedVector<int, 8> v = {5, 3, 1, 4};
  std::sort(v.begin(), v.end());
  CHECK(v[0] == 1);
  v.erase(v.begin());
  CHECK(v[0] == 3);
  v.erase_unordered_at(0);
  CHECK(v[0] == 5);
  CHECK(v.size() == 2);
  v.resize(6);
  CHECK(v.size() == 6);
  CHECK(v[5] == 0);
  std::span<const int> s = v;
  CHECK(s.size() == 6);
}

TEST_CASE("FixedVector: copy and move with tracked elements leak nothing") {
  Tracked::live = 0;
  {
    FixedVector<Tracked, 4> a;
    a.emplace_back("x");
    a.emplace_back("y");
    FixedVector<Tracked, 4> b = a;
    CHECK(b == a);
    CHECK(Tracked::live == 4);
    FixedVector<Tracked, 4> c = std::move(a);
    CHECK(a.empty());
    CHECK(c.size() == 2);
    CHECK(Tracked::live == 4);
    b = std::move(c);
    CHECK(c.empty());
    CHECK(b[1].payload == "y");
    CHECK(Tracked::live == 2);
  }
  CHECK(Tracked::live == 0);
}
