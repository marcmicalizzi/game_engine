#include <core/platform/topology.h>

#include <core/platform/thread.h>

#include <doctest/doctest.h>

#include <string>
#include <thread>

using namespace engine;
using namespace engine::platform;

TEST_CASE("CpuSet: set, test, count, first, for_each across word boundaries") {
  CpuSet s;
  CHECK(s.empty());
  CHECK(s.first() == k_invalid_cpu);
  s.set(3);
  s.set(64);
  s.set(130);
  CHECK(s.count() == 3);
  CHECK(s.test(3));
  CHECK(s.test(64));
  CHECK(s.test(130));
  CHECK_FALSE(s.test(4));
  CHECK_FALSE(s.test(1000));
  CHECK(s.first() == 3);
  int sum = 0;
  s.for_each([&](u16 c) { sum += c; });
  CHECK(sum == 3 + 64 + 130);
  s.clear(64);
  CHECK(s.count() == 2);
  CpuSet t;
  t.set(3);
  t.set(130);
  CHECK(s == t);
}

TEST_CASE("topology: detection produces a consistent picture of this machine") {
  const Topology& t = topology();
  char buf[2048];
  describe_topology(t, buf, sizeof(buf));
  MESSAGE(buf);

  REQUIRE(t.cpu_count() >= 1);
  CHECK(t.core_count >= 1);
  CHECK(t.core_count <= t.cpu_count());
  CHECK(t.package_count >= 1);
  CHECK(t.numa_node_count >= 1);
  CHECK(t.cache_domains.size() >= 1);
  CHECK(t.cache_line_bytes >= 32);
  CHECK(t.efficiency_class_count >= 1);

  // Every CPU has a valid id, core, and cache domain; classes partition the CPU set.
  CpuSet all;
  for (u32 i = 0; i < t.cpus.size(); ++i) {
    const LogicalCpu& c = t.cpus[i];
    CHECK(c.id == i);
    CHECK(c.core_id < t.core_count);
    CHECK(c.package_id < t.package_count);
    CHECK(c.numa_node < t.numa_node_count);
    REQUIRE(c.cache_domain < t.cache_domains.size());
    CHECK(t.cache_domains[c.cache_domain].cpus.test(c.id));
    CHECK(t.performance_cpus.test(c.id) != t.efficiency_cpus.test(c.id));
    all.set(c.id);
  }
  CHECK(t.performance_cpus.count() + t.efficiency_cpus.count() == t.cpu_count());
  CHECK_FALSE(t.performance_cpus.empty());

  // Cache domains are disjoint and cover every CPU.
  CpuSet covered;
  u32 total = 0;
  for (const CacheDomain& d : t.cache_domains) {
    d.cpus.for_each([&](u16 id) {
      CHECK_FALSE(covered.test(id));
      covered.set(id);
    });
    total += d.cpus.count();
  }
  CHECK(total == t.cpu_count());
  CHECK(covered == all);

  // SMT siblings share a core and have distinct smt indices.
  if (t.smt) {
    bool found_sibling = false;
    for (const LogicalCpu& a : t.cpus) {
      for (const LogicalCpu& b : t.cpus) {
        if (a.id != b.id && a.core_id == b.core_id) {
          CHECK(a.smt_index != b.smt_index);
          found_sibling = true;
        }
      }
    }
    CHECK(found_sibling);
  }

  // find() round-trips OS coordinates.
  for (const LogicalCpu& c : t.cpus) {
    const LogicalCpu* f = t.find(c.os_group, c.os_index);
    REQUIRE(f != nullptr);
    CHECK(f->id == c.id);
  }
}

TEST_CASE("topology: repeated detection is stable") {
  const Topology a = detect_topology();
  const Topology b = detect_topology();
  CHECK(a.cpu_count() == b.cpu_count());
  CHECK(a.cache_domains.size() == b.cache_domains.size());
  CHECK(a.performance_cpus == b.performance_cpus);
  CHECK(a.core_count == b.core_count);
}

TEST_CASE("thread: pinning moves the thread onto the requested CPU") {
  const Topology& t = topology();
  const u16 target = t.cpus[t.cpu_count() - 1].id;
  bool pinned = false;
  u16 observed = k_invalid_cpu;
  std::thread th([&] {
    pinned = pin_current_thread(target);
    if (pinned) {
      // Give the scheduler a moment to migrate us.
      for (int i = 0; i < 1000 && observed != target; ++i) {
        yield_thread();
        observed = current_cpu();
      }
    }
  });
  th.join();
  CHECK(pinned);
  if (pinned) CHECK(observed == target);
}

TEST_CASE("thread: naming, priority, and thread index") {
  std::thread th([] {
    CHECK(set_current_thread_name("engine-test-thread"));
    CHECK(set_current_thread_priority(ThreadPriority::BelowNormal));
    CHECK(set_current_thread_priority(ThreadPriority::Normal));
  });
  th.join();
  const u32 mine = current_thread_index();
  CHECK(current_thread_index() == mine);
  u32 other = mine;
  std::thread th2([&] { other = current_thread_index(); });
  th2.join();
  CHECK(other != mine);
}
