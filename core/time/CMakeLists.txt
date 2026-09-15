# Clocks and time types (docs/plan/02-architecture.md §2.4): monotonic wall time, 64-bit sim
# ticks, integer game time, and the fixed-step accumulator that decouples sim from render.
engine_module(NAME time LAYER core
  DEPS base
  SOURCES src/clock.cpp)

engine_module_tests(NAME time
  SOURCES tests/time_tests.cpp tests/size_table.cpp)
