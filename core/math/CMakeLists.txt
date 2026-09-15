# Header-only linear algebra: vectors, quaternions, matrices, bounds, transforms. Plain
# trivially-copyable structs with fixed layouts so they upload to the GPU and serialize as-is.
engine_module(NAME math LAYER core
  DEPS base)

engine_module_tests(NAME math
  SOURCES tests/math_tests.cpp tests/size_table.cpp)
