# Header-only container set (ADR-0019). Every engine module uses these instead of
# node-based standard containers.
engine_module(NAME containers LAYER core
  DEPS base hash memory)

engine_module_tests(NAME containers
  SOURCES
    tests/flat_map_tests.cpp
    tests/flat_set_tests.cpp
    tests/hash_map_tests.cpp
    tests/hash_set_tests.cpp
    tests/slot_map_tests.cpp
    tests/small_vector_tests.cpp
    tests/fixed_vector_tests.cpp
    tests/vector_tests.cpp
    tests/size_table.cpp)

# Container micro-benchmarks (docs/plan/11 §11.2); standard containers as comparison baselines.
engine_module_bench(NAME containers
  SOURCES bench/containers_bench.cpp
  DEPS hash)
