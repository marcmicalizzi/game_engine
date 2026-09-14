# 128-bit stable identifiers (ADR-0002, ADR-0017): time-sortable random ids, hex encoding,
# and deterministic generators for procedural content.
engine_module(NAME ids LAYER core
  DEPS base hash
  SOURCES src/id128.cpp)

engine_module_tests(NAME ids
  SOURCES tests/id128_tests.cpp tests/size_table.cpp
  DEPS containers)
