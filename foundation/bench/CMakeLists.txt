# Micro-benchmark harness (ADR-0011, docs/plan/11 §11.8): registration, calibrated timing,
# statistics, tunable sweeps, text and JSON-lines reports. Module bench executables link it
# through engine_module_bench().
engine_module(NAME bench LAYER foundation
  DEPS base containers json platform time tunables
  SOURCES src/bench.cpp)

engine_module_tests(NAME bench
  SOURCES tests/bench_tests.cpp)
