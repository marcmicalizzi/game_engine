# OS and CPU facts the rest of the engine builds on: processor topology (cores, SMT, cache
# domains, NUMA, efficiency classes), CPUID feature detection, and thread pinning/naming.
engine_module(NAME platform LAYER core
  DEPS base memory containers
  SOURCES src/topology.cpp src/cpu_features.cpp src/thread.cpp)

engine_module_tests(NAME platform
  SOURCES tests/topology_tests.cpp tests/cpu_features_tests.cpp tests/size_table.cpp)
