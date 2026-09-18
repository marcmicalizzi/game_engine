#include <core/containers/small_vector.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <string>
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

}  // namespace

TEST_CASE("SmallVector: stays inline up to N then moves to the heap") {
  SmallVector<int, 4> v;
  CHECK(v.is_inline());
  CHECK(v.capacity() == 4);
  for (int i = 0; i < 4; ++i)
    v.push_back(i);
  CHECK(v.is_inline());
  CHECK(v.size() == 4);
  v.push_back(4);
  CHECK_FALSE(v.is_inline());
  CHECK(v.capacity() >= 5);
  for (int i = 0; i < 5; ++i)
    CHECK(v[static_cast<u32>(i)] == i);
  v.shrink_to_fit();
  CHECK(v.capacity() == 5);
  v.pop_back();
  v.shrink_to_fit();
  CHECK(v.is_inline());
  CHECK(v.size() == 4);
  CHECK(v.back() == 3);
}

TEST_CASE("SmallVector: initializer list, iteration, span conversion") {
  SmallVector<int, 8> v = {3, 1, 2};
  CHECK(v.size() == 3);
  std::sort(v.begin(), v.end());
  CHECK(v[0] == 1);
  std::span<const int> s = v;
  CHECK(s.size() == 3);
  CHECK(std::accumulate(v.begin(), v.end(), 0) == 6);
  v = {9, 8};
  CHECK(v.size() == 2);
  CHECK(v.front() == 9);
}

TEST_CASE("SmallVector: insert and erase preserve order; erase_unordered is O(1)") {
  SmallVector<int, 2> v;
  for (int i = 0; i < 6; ++i)
    v.push_back(i);
  v.insert(2u, 100);
  CHECK(v.size() == 7);
  CHECK(v[2] == 100);
  CHECK(v[3] == 2);
  v.erase(v.begin() + 2);
  CHECK(v[2] == 2);
  v.erase(v.begin(), v.begin() + 2);
  CHECK(v.size() == 4);
  CHECK(v[0] == 2);
  v.erase_unordered_at(0);
  CHECK(v.size() == 3);
  CHECK(v[0] == 5);
  v.insert(v.end(), 7);
  CHECK(v.back() == 7);
}

TEST_CASE("SmallVector: resize and assign") {
  SmallVector<int, 4> v;
  v.resize(3);
  CHECK(v.size() == 3);
  CHECK(v[2] == 0);
  v.resize(10, 7);
  CHECK(v.size() == 10);
  CHECK(v[9] == 7);
  v.resize(2);
  CHECK(v.size() == 2);
  v.assign(5u, 1);
  CHECK(v.size() == 5);
  std::vector<int> src = {4, 5, 6};
  v.assign(src.begin(), src.end());
  CHECK(v.size() == 3);
  v.append(std::span<const int>(src));
  CHECK(v.size() == 6);
  CHECK(v[5] == 6);
}

TEST_CASE("SmallVector: move from inline moves elements; move from heap steals the pointer") {
  Tracked::live = 0;
  {
    SmallVector<Tracked, 4> inline_v;
    inline_v.emplace_back("a");
    inline_v.emplace_back("b");
    SmallVector<Tracked, 4> moved(std::move(inline_v));
    CHECK(moved.is_inline());
    CHECK(moved.size() == 2);
    CHECK(inline_v.empty());
    CHECK(moved[1].payload == "b");
    CHECK(Tracked::live == 2);

    SmallVector<Tracked, 4> heap_v;
    for (int i = 0; i < 10; ++i)
      heap_v.emplace_back(std::to_string(i));
    CHECK_FALSE(heap_v.is_inline());
    const Tracked* data = heap_v.data();
    SmallVector<Tracked, 4> stolen(std::move(heap_v));
    CHECK(stolen.data() == data);
    CHECK(heap_v.is_inline());
    CHECK(heap_v.empty());
    CHECK(Tracked::live == 12);

    heap_v = std::move(stolen);  // move-assign back
    CHECK(heap_v.size() == 10);
    CHECK(stolen.empty());
    stolen = moved;  // copy-assign
    CHECK(stolen == moved);
    CHECK(Tracked::live == 14);
    swap(stolen, heap_v);
    CHECK(stolen.size() == 10);
    CHECK(heap_v.size() == 2);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("SmallVector: resize follows Vector's growth policy") {
  // A policy the container set documents that one container quietly does not follow is a trap for
  // whoever changes a `Vector<T>` to a `SmallVector<T, N>` and gets the quadratic back. Same three
  // promises as `Vector`: a first resize is exact, incremental growth is geometric, `resize_exact`
  // is the escape.
  SmallVector<int, 8> once;
  once.resize(1000);
  CHECK(once.capacity() == 1000);

  SmallVector<int, 8> runs;
  usize reallocations = 0;
  const int* previous = nullptr;
  for (u32 run = 0; run < 500; ++run) {
    runs.resize((run + 1) * 23);
    if (runs.data() != previous) ++reallocations;
    previous = runs.data();
  }
  CHECK(runs.size() == 11500);
  CHECK(reallocations < 40);  // 500 with an exact resize

  SmallVector<int, 8> exact;
  exact.resize_exact(37);
  CHECK(exact.capacity() == 37);
  exact.resize_exact(74, 3);
  CHECK(exact.capacity() == 74);
  CHECK(exact[73] == 3);
}

TEST_CASE("SmallVector: growth relocates tracked elements exactly") {
  Tracked::live = 0;
  {
    SmallVector<Tracked, 3> v;
    for (int i = 0; i < 200; ++i) {
      v.emplace_back(std::to_string(i));
      REQUIRE(Tracked::live == static_cast<int>(v.size()));
    }
    for (int i = 0; i < 200; i += 2)
      v.erase_at(static_cast<u32>(i / 2));
    CHECK(v.size() == 100);
    CHECK(Tracked::live == 100);
    for (u32 i = 0; i < 100; ++i)
      CHECK(v[i].payload == std::to_string(2 * i + 1));
    v.clear();
    CHECK(Tracked::live == 0);
  }
  CHECK(Tracked::live == 0);
}
