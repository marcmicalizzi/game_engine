# Header-only linear algebra: vectors, quaternions, matrices, bounds, transforms. Plain
# trivially-copyable structs with fixed layouts so they upload to the GPU and serialize as-is.
engine_module(NAME math LAYER core
  DEPS base)

engine_module_tests(NAME math
  SOURCES tests/math_tests.cpp tests/world_tests.cpp tests/size_table.cpp)

# The hot functions, one row each (docs/subsystems/math.md). Every one of them is inline and used
# across the tree, so a codegen regression here is paid everywhere and shows up nowhere in
# particular: `mat4_from_transform`'s +77% at x86-64-v3 was found from `domain/anim`'s bench, by
# subtraction, because this module had none.
engine_module_bench(NAME math SOURCES bench/math_bench.cpp)
