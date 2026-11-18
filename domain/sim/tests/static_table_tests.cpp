// The static registration table (ADR-0027, "The registration table") and tier filtering by
// `SystemDesc::tiers` (docs/subsystems/sim.md, "The static table and tier filtering").

#include <core/containers/vector.h>
#include <domain/sim/scheduler.h>

#include <doctest/doctest.h>

#include <cstring>

using namespace engine;
using namespace engine::sim;

namespace {

// What each system saw, appended in invocation order. No job system in these tests, so the waves
// run on this thread and the order is the schedule's.
struct Seen {
  Vector<const char*> names;
  Vector<u8> tiers;
  u32 begins = 0;
  u32 ends = 0;
};

Seen g_seen;

void record(SystemContext& context, Batch) {
  g_seen.names.push_back(static_cast<const char*>(context.context));
  g_seen.tiers.push_back(context.tiers);
}
void begin(SystemContext&) { ++g_seen.begins; }
void end(SystemContext&) { ++g_seen.ends; }

ComponentMask writes_position() {
  ComponentMask mask;
  mask.set(0);
  return mask;
}

// Three modules, registered in an order that is neither their name order nor their `order`, and a
// fourth that names a module already registered. Every system writes one component, so all of them
// conflict and each runs in its own wave in exactly the declaration order the table produced.
// A constant descriptor: the context is the name, which is what `record` appends.
constexpr SystemDesc named(const char* name) {
  SystemDesc desc;
  desc.name = name;
  desc.phase = TickPhase::Systems;
  desc.context = const_cast<char*>(name);
  desc.tick = &record;
  return desc;
}

constinit const SystemDesc k_zeta_systems[] = {named("zeta.one"), named("zeta.two")};
constinit const SystemDesc k_alpha_systems[] = {named("alpha.one")};
constinit const SystemDesc k_first_systems[] = {named("first.one")};
constinit const SystemDesc k_alpha_again[] = {named("alpha.impostor")};

const SystemRegistration k_zeta{{"zeta", k_zeta_systems, 0}};
const SystemRegistration k_alpha{{"alpha", k_alpha_systems, 0}};
const SystemRegistration k_first{{"first", k_first_systems, -1}};
const SystemRegistration k_alpha_twice{{"alpha", k_alpha_again, 0}};

}  // namespace

TEST_CASE("static table: sorted by order then module, a module named twice refused") {
  // "alpha" is named by two tables, so neither is kept: which of the two the link put first is the
  // toolchain's, and keeping one would let it choose the systems.
  const Vector<const SystemTable*> tables = static_system_tables();
  REQUIRE(tables.size() == 2u);
  CHECK(std::strcmp(tables[0]->module, "first") == 0);
  CHECK(std::strcmp(tables[1]->module, "zeta") == 0);

  // Four registrations reached the list, whatever order the toolchain initialized them in.
  u32 linked = 0;
  for (const SystemRegistration* r = static_system_registrations(); r != nullptr; r = r->next()) {
    ++linked;
  }
  CHECK(linked == 4u);
}

TEST_CASE("static table: a scheduler adds it once, in the table's order") {
  SimScheduler scheduler;
  CHECK(scheduler.add_static_systems() == 3u);
  CHECK(scheduler.add_static_systems() == 0u);
  REQUIRE(scheduler.system_count() == 3u);
  const char* const expected[] = {"first.one", "zeta.one", "zeta.two"};
  for (u16 i = 0; i < 3u; ++i)
    CHECK(std::strcmp(scheduler.system(i).name, expected[i]) == 0);

  // A static system and a runtime one land in one table and the schedule cannot tell them apart.
  SystemDesc runtime;
  runtime.name = "runtime.one";
  runtime.context = const_cast<char*>("runtime.one");
  runtime.tick = &record;
  scheduler.add_system(runtime);

  g_seen = {};
  scheduler.step();
  REQUIRE(g_seen.names.size() == 4u);
  for (u32 i = 0; i < 3u; ++i)
    CHECK(std::strcmp(g_seen.names[i], expected[i]) == 0);
  CHECK(std::strcmp(g_seen.names[3], "runtime.one") == 0);
}

TEST_CASE("tier filtering: a system meeting no active tier is not invoked at all") {
  SimScheduler scheduler;
  const u8 declared[] = {0x01u, 0x06u, 0x0Fu};
  const char* const names[] = {"near", "middle", "every"};
  for (u32 i = 0; i < 3u; ++i) {
    SystemDesc desc;
    desc.name = names[i];
    desc.writes = writes_position();
    desc.tiers = declared[i];
    desc.batches = 2;
    desc.context = const_cast<char*>(names[i]);
    desc.begin_tick = &begin;
    desc.tick = &record;
    desc.end_tick = &end;
    scheduler.add_system(desc);
  }
  const u64 hash = scheduler.schedule_hash();

  // Every tier active: the default, and the behaviour before tier filtering existed.
  g_seen = {};
  scheduler.step();
  CHECK(g_seen.names.size() == 6u);
  CHECK(g_seen.begins == 3u);
  CHECK(g_seen.ends == 3u);
  CHECK(g_seen.tiers[0] == 0x01u);
  CHECK(scheduler.tier_skipped() == 0u);

  // Only tier 2 held: "near" (tier 0) runs nothing, the other two run for tier 2 alone.
  scheduler.set_active_tiers(0x04u);
  g_seen = {};
  scheduler.step();
  REQUIRE(g_seen.names.size() == 4u);
  CHECK(std::strcmp(g_seen.names[0], "middle") == 0);
  CHECK(std::strcmp(g_seen.names[2], "every") == 0);
  for (const u8 tiers : g_seen.tiers)
    CHECK(tiers == 0x04u);
  CHECK(g_seen.begins == 2u);
  CHECK(g_seen.ends == 2u);
  // Two batches, a begin and an end of "near".
  CHECK(scheduler.tier_skipped() == 4u);
  // The schedule is not rebuilt by a mask: the waves and the hash are the registration's.
  CHECK(scheduler.schedule_hash() == hash);
  CHECK(scheduler.wave_count(TickPhase::Systems) == 3u);

  // Nothing held: nothing runs.
  scheduler.set_active_tiers(0);
  g_seen = {};
  scheduler.step();
  CHECK(g_seen.names.empty());
  CHECK(g_seen.begins == 0u);
}

TEST_CASE("tier filtering: a mask set during a tick applies from the next") {
  struct Setter {
    SimScheduler* scheduler = nullptr;
    u32 runs = 0;
    static void tick(SystemContext& context, Batch) {
      auto* self = static_cast<Setter*>(context.context);
      ++self->runs;
      self->scheduler->set_active_tiers(0x08u);
    }
  };
  SimScheduler scheduler;
  Setter setter{&scheduler};
  SystemDesc first;
  first.name = "sets_the_mask";
  first.phase = TickPhase::Lod;
  first.tiers = 0x01u;
  first.context = &setter;
  first.tick = &Setter::tick;
  scheduler.add_system(first);
  SystemDesc later;
  later.name = "near_only";
  later.phase = TickPhase::Systems;
  later.tiers = 0x01u;
  later.context = const_cast<char*>("near_only");
  later.tick = &record;
  scheduler.add_system(later);

  g_seen = {};
  scheduler.step();
  // The Lod system narrowed the mask to tier 3, and the Systems phase of the same tick still ran
  // under the mask the tick began with.
  CHECK(setter.runs == 1u);
  CHECK(g_seen.names.size() == 1u);
  scheduler.step();
  CHECK(setter.runs == 1u);
  CHECK(g_seen.names.size() == 1u);
}

TEST_CASE("tier filtering: the tiers a system declares are part of the schedule hash") {
  SimScheduler a;
  SimScheduler b;
  SystemDesc desc;
  desc.name = "s";
  desc.tick = &record;
  a.add_system(desc);
  desc.tiers = 0x03u;
  b.add_system(desc);
  CHECK(a.schedule_hash() != b.schedule_hash());
}

TEST_CASE("tier filtering: occupied_tiers names the tiers a population holds") {
  const u8 population[] = {3, 3, 0, 2, 9};
  CHECK(occupied_tiers(population) == 0x0Du);
  CHECK(occupied_tiers({}) == 0u);
}
