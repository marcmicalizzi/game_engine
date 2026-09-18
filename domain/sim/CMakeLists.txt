# sim: the simulation core of docs/plan/05-simulation.md — the hierarchical timing wheel and its
# budgeted fast-forward (§5.3), LOD tier assignment over the observer set (§5.4), the tick
# scheduler with the materialization contract (§5.2, 03 §3.4) and tile reconciliation (§5.5).
#
# Not OPTIONAL: ADR-0027 makes the scheduler a registration point that capabilities attach to,
# and a registration point that can be switched off is one nobody can register with. It depends
# on no capability, and no capability's absence removes anything from it.
engine_module(NAME sim LAYER domain
  DEPS base containers time math jobs log ids
  SOURCES src/timing_wheel.cpp src/tiers.cpp src/scheduler.cpp)

engine_module_tests(NAME sim
  SOURCES tests/timing_wheel_tests.cpp tests/fast_forward_tests.cpp tests/tier_tests.cpp
          tests/scheduler_tests.cpp tests/size_table.cpp
  DEPS jobs)

# 10^6 timers through insert, cancel and advance; tier assignment over 10^5 entities at 1/4/8
# workers (docs/subsystems/sim.md, "Performance notes").
engine_module_bench(NAME sim
  SOURCES bench/sim_bench.cpp
  DEPS jobs)
