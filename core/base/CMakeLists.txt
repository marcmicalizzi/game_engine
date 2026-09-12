engine_module(NAME base LAYER core
  SOURCES src/assert.cpp)

engine_module_tests(NAME base
  SOURCES tests/base_tests.cpp tests/size_table.cpp)
