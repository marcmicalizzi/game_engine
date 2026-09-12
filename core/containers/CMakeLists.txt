# Header-only container set (ADR-0019). Every engine module uses these instead of
# node-based standard containers.
engine_module(NAME containers LAYER core
  DEPS base)

engine_module_tests(NAME containers
  SOURCES
    tests/flat_map_tests.cpp
    tests/flat_set_tests.cpp
    tests/size_table.cpp)
