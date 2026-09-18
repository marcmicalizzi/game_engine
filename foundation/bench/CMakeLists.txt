# Micro-benchmark harness (ADR-0011, docs/plan/11 §11.8): registration, calibrated timing,
# statistics, tunable sweeps, text and JSON-lines reports, and what else the machine was doing
# while the numbers were taken. Module bench executables link it through engine_module_bench().
engine_module(NAME bench LAYER foundation
  DEPS base containers json platform time tunables
  SOURCES src/bench.cpp src/machine_state.cpp)

engine_module_tests(NAME bench
  SOURCES tests/bench_tests.cpp tests/machine_state_tests.cpp)
