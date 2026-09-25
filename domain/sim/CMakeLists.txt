# sim: the simulation core of docs/plan/05-simulation.md — the hierarchical timing wheel and its
# budgeted fast-forward (§5.3), LOD tier assignment over the observer set (§5.4), the tick
# scheduler with the materialization contract (§5.2, 03 §3.4) and tile reconciliation (§5.5).
#
# Not OPTIONAL: ADR-0027 makes the scheduler a registration point that capabilities attach to,
# and a registration point that can be switched off is one nobody can register with. It depends
# on no capability, and no capability's absence removes anything from it.
#
# `doc`, `schema` and `json` are the materialization driver's (docs/subsystems/sim.md, "The driver"):
# it reads a document through its composed index and the mapping table schemac compiled. Both are
# domain- and core-layer and neither is a capability, so the scheduler stays switch-free.
engine_module(NAME sim LAYER domain
  DEPS base containers time math jobs log ids json schema memory doc
  SOURCES src/timing_wheel.cpp src/tiers.cpp src/scheduler.cpp src/materialize.cpp)

# Record types, components and the declarations joining them, for the driver's tests: the table
# schemac emits is what the driver reads, so the tests read one schemac emitted.
engine_schema_library(NAME sim_test_types
  SCHEMAS tests/sim_world.schema)

engine_module_tests(NAME sim
  SOURCES tests/timing_wheel_tests.cpp tests/fast_forward_tests.cpp tests/tier_tests.cpp
          tests/scheduler_tests.cpp tests/materialize_tests.cpp tests/size_table.cpp
  DEPS jobs sim_test_types)

# 10^6 timers through insert, cancel and advance; tier assignment over 10^5 entities at 1/4/8
# workers (docs/subsystems/sim.md, "Performance notes").
engine_module_bench(NAME sim
  SOURCES bench/sim_bench.cpp
  DEPS jobs)
