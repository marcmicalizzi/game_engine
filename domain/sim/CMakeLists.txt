# sim: the simulation core of docs/plan/05-simulation.md — the hierarchical timing wheel and its
# budgeted fast-forward (§5.3), LOD tier assignment over the observer set (§5.4), with the tick
# scheduler (§5.2, §5.5) landing into the same module.
#
# Not OPTIONAL: ADR-0027 makes the scheduler a registration point that capabilities attach to,
# and a registration point that can be switched off is one nobody can register with. It depends
# on no capability, and no capability's absence removes anything from it.
engine_module(NAME sim LAYER domain
  DEPS base containers time math jobs log ids
  SOURCES src/timing_wheel.cpp src/tiers.cpp)

engine_module_tests(NAME sim
  SOURCES tests/timing_wheel_tests.cpp tests/fast_forward_tests.cpp tests/tier_tests.cpp
          tests/size_table.cpp
  DEPS jobs)
