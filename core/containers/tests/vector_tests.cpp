#include <core/containers/vector.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
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

// A recursive type: Vector must accept an incomplete element type at declaration.
struct Node {
  int value = 0;
  Vector<Node> children;
};

}  // namespace

TEST_CASE("Vector: basics, growth, and algorithms") {
  Vector<int> v;
  CHECK(v.empty());
  CHECK(v.capacity() == 0);
  for (int i = 0; i < 100; ++i)
    v.push_back(i);
  CHECK(v.size() == 100);
  CHECK(v.capacity() >= 100);
  CHECK(std::accumulate(v.begin(), v.end(), 0) == 4950);
  std::reverse(v.begin(), v.end());
  CHECK(v[0] == 99);
  v.shrink_to_fit();
  CHECK(v.capacity() == 100);
  v.clear();
  CHECK(v.empty());
  CHECK(v.capacity() == 100);
  v.shrink_to_fit();
  CHECK(v.capacity() == 0);
}

TEST_CASE("Vector: insert, erase, erase_unordered, resize, assign") {
  Vector<int> v = {0, 1, 2, 3, 4, 5};
  v.insert(2u, 100);
  CHECK(v[2] == 100);
  CHECK(v[3] == 2);
  v.erase(v.begin() + 2);
  CHECK(v[2] == 2);
  v.erase(v.begin(), v.begin() + 2);
  CHECK(v.size() == 4);
  CHECK(v[0] == 2);
  v.erase_unordered_at(0);
  CHECK(v[0] == 5);
  v.resize(10, 7);
  CHECK(v.size() == 10);
  CHECK(v[9] == 7);
  v.resize(1);
  CHECK(v.size() == 1);
  v.assign(3u, 9);
  CHECK(v == Vector<int>{9, 9, 9});
  std::span<const int> s = v;
  CHECK(s.size() == 3);
}

TEST_CASE("Vector: the growth policy, which is a footprint decision as much as a speed one") {
  // Half of this is the fix — a pool that appends a run and resizes to the new total must not
  // relocate on every call — and half is the reason it is not a memory regression: a single
  // `resize` from an empty or much smaller vector still takes the size exactly, which is what
  // almost every caller in this tree does.
  SUBCASE("a first resize takes the size exactly") {
    Vector<int> v;
    v.resize(1000);
    CHECK(v.capacity() == 1000);
    // And so does one that steps far past what 1.5x of the current capacity would give.
    v.resize(100000);
    CHECK(v.capacity() == 100000);
  }

  SUBCASE("growing by fixed-size runs reallocates a logarithmic number of times") {
    Vector<int> v;
    usize reallocations = 0;
    const int* previous = nullptr;
    for (u32 run = 0; run < 1000; ++run) {
      v.resize((run + 1) * 23);
      if (v.data() != previous) ++reallocations;
      previous = v.data();
    }
    CHECK(v.size() == 23000);
    // Exact growth would be 1000 of them. 1.5x from a floor of four needs about log(23000)/log(1.5)
    // steps; the bound is loose on purpose, because the assertion worth making is "not linear".
    CHECK(reallocations < 40);
    // Slack is bounded by the policy, not by luck.
    CHECK(v.capacity() < v.size() * 3 / 2 + 4);
  }

  SUBCASE("reserve and resize_exact stay exact, and shrink_to_fit gives the slack back") {
    Vector<int> v;
    v.reserve(777);
    CHECK(v.capacity() == 777);
    v.reserve(1);  // never shrinks
    CHECK(v.capacity() == 777);

    Vector<int> exact;
    for (u32 run = 0; run < 8; ++run)
      exact.resize_exact((run + 1) * 23);
    CHECK(exact.size() == 184);
    CHECK(exact.capacity() == 184);
    exact.resize_exact(300, 5);
    CHECK(exact.capacity() == 300);
    CHECK(exact[299] == 5);

    Vector<int> grown;
    for (u32 run = 0; run < 64; ++run)
      grown.resize((run + 1) * 23);
    CHECK(grown.capacity() >= grown.size());
    grown.shrink_to_fit();
    CHECK(grown.capacity() == grown.size());
  }

  SUBCASE("append grows the same way and is exact on the first one") {
    const Vector<int> chunk(50u, 1);
    Vector<int> v;
    v.append(std::span<const int>(chunk.data(), chunk.size()));
    CHECK(v.capacity() == 50);
    usize reallocations = 0;
    const int* previous = v.data();
    for (u32 i = 0; i < 500; ++i) {
      v.append(std::span<const int>(chunk.data(), chunk.size()));
      if (v.data() != previous) ++reallocations;
      previous = v.data();
    }
    CHECK(v.size() == 25050);
    CHECK(reallocations < 30);
  }
}

TEST_CASE("Vector: recursive element type") {
  Node root;
  root.value = 1;
  root.children.push_back(Node{2, {}});
  root.children.push_back(Node{3, {}});
  root.children[0].children.push_back(Node{4, {}});
  CHECK(root.children.size() == 2);
  CHECK(root.children[0].children[0].value == 4);
  Node copy = root;
  CHECK(copy.children[0].children[0].value == 4);
  static_assert(sizeof(Vector<Node>) == 16);
}

TEST_CASE("Vector: tracked elements through growth, copy, and move leak nothing") {
  Tracked::live = 0;
  {
    Vector<Tracked> v;
    for (int i = 0; i < 500; ++i) {
      v.emplace_back(std::to_string(i));
      REQUIRE(Tracked::live == static_cast<int>(v.size()));
    }
    Vector<Tracked> copy = v;
    CHECK(copy == v);
    CHECK(Tracked::live == 1000);
    Vector<Tracked> moved = std::move(v);
    CHECK(v.empty());
    CHECK(v.capacity() == 0);
    CHECK(moved.size() == 500);
    CHECK(Tracked::live == 1000);
    copy = std::move(moved);
    CHECK(Tracked::live == 500);
    for (int i = 0; i < 500; i += 2)
      copy.erase_at(static_cast<u32>(i / 2));
    CHECK(copy.size() == 250);
    CHECK(Tracked::live == 250);
    swap(copy, v);
    CHECK(v.size() == 250);
  }
  CHECK(Tracked::live == 0);
}
