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
