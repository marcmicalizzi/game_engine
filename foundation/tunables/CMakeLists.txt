# Tunables registry (ADR-0011): named run-time parameters with defaults, ranges, docs, text
# and JSON forms, for kernels to read once outside hot loops and for tools, config files, the
# bench harness, and the protocol to set by name.
engine_module(NAME tunables LAYER foundation
  DEPS base containers json platform
  SOURCES src/tunables.cpp)

engine_module_tests(NAME tunables
  SOURCES tests/tunables_tests.cpp tests/size_table.cpp)
