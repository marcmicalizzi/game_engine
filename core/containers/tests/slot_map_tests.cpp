#include <core/containers/slot_map.h>

#include <doctest/doctest.h>

#include <string>
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
};

}  // namespace

TEST_CASE("SlotHandle: null, packing, ordering") {
  SlotHandle null;
  CHECK(null.is_null());
  CHECK_FALSE(null);
  SlotHandle h{7, 3};
  CHECK(h);
  CHECK(SlotHandle::from_u64(h.to_u64()) == h);
  CHECK(SlotHandle{1, 1} < SlotHandle{2, 1});
}

TEST_CASE("SlotMap: insert, get, erase, stale detection") {
  SlotMap<int> m;
  CHECK(m.empty());
  const SlotHandle a = m.insert(10);
  const SlotHandle b = m.insert(20);
  const SlotHandle c = m.insert(30);
  CHECK(m.size() == 3);
  REQUIRE(m.get(a) != nullptr);
  CHECK(*m.get(a) == 10);
  CHECK(*m.get(b) == 20);
  CHECK(*m.get(c) == 30);
  CHECK(m.get(SlotHandle{}) == nullptr);
  CHECK(m.get(SlotHandle{99, 1}) == nullptr);

  CHECK(m.erase(b));
  CHECK_FALSE(m.erase(b));  // stale
  CHECK(m.get(b) == nullptr);
  CHECK_FALSE(m.contains(b));
  CHECK(m.size() == 2);
  CHECK(*m.get(a) == 10);
  CHECK(*m.get(c) == 30);  // c moved into b's dense slot but its handle still resolves

  // The freed slot is reused with a new generation; the old handle stays stale.
  const SlotHandle d = m.insert(40);
  CHECK(d.index == b.index);
  CHECK(d.generation != b.generation);
  CHECK(m.get(b) == nullptr);
  CHECK(*m.get(d) == 40);
}

TEST_CASE("SlotMap: dense iteration and handle_at agree") {
  SlotMap<int> m;
  std::vector<SlotHandle> handles;
  for (int k = 0; k < 100; ++k)
    handles.push_back(m.insert(k));
  for (int k = 0; k < 100; k += 3)
    m.erase(handles[static_cast<usize>(k)]);
  int count = 0;
  for (u32 i = 0; i < m.size(); ++i) {
    const SlotHandle h = m.handle_at(i);
    CHECK(m.get(h) == &m.values()[i]);
    CHECK(m.dense_index(h) == i);
    ++count;
  }
  CHECK(count == static_cast<int>(m.size()));
  int sum = 0;
  for (auto [h, v] : m.entries()) {
    CHECK(m.get(h) == &v);
    sum += v;
  }
  int expected = 0;
  for (int k = 0; k < 100; ++k) {
    if (k % 3 != 0) expected += k;
  }
  CHECK(sum == expected);
}

TEST_CASE("SlotMap: growth, clear, and tracked values leak nothing") {
  Tracked::live = 0;
  {
    SlotMap<Tracked> m;
    std::vector<SlotHandle> handles;
    for (int k = 0; k < 5000; ++k) {
      handles.push_back(m.emplace(std::to_string(k)));
      REQUIRE(Tracked::live == static_cast<int>(m.size()));
    }
    for (int k = 0; k < 5000; k += 2)
      CHECK(m.erase(handles[static_cast<usize>(k)]));
    CHECK(Tracked::live == 2500);
    for (int k = 1; k < 5000; k += 2)
      CHECK(m.get(handles[static_cast<usize>(k)])->payload == std::to_string(k));
    m.clear();
    CHECK(Tracked::live == 0);
    CHECK(m.empty());
    for (int k = 1; k < 5000; k += 2)
      CHECK(m.get(handles[static_cast<usize>(k)]) == nullptr);
    // Slots are recycled after clear.
    const SlotHandle h = m.emplace("after clear");
    CHECK(h.index < 5000);
    CHECK(m.slot_count() == 5000);
  }
  CHECK(Tracked::live == 0);
}

TEST_CASE("SlotMap: move semantics keep handles valid") {
  SlotMap<int> a;
  const SlotHandle h = a.insert(5);
  SlotMap<int> b = std::move(a);
  CHECK(a.empty());
  CHECK(*b.get(h) == 5);
  SlotMap<int> c;
  c = std::move(b);
  CHECK(*c.get(h) == 5);
}

TEST_CASE("SlotMap: reserve") {
  SlotMap<int> m;
  m.reserve(100);
  CHECK(m.capacity() >= 100);
  const auto cap = m.capacity();
  for (int k = 0; k < 100; ++k)
    m.insert(k);
  CHECK(m.capacity() == cap);
}
