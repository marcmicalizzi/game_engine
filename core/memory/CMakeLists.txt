# Allocation front door, per-tag accounting, and arenas. Every engine allocation goes through
# mem::allocate so footprint is measurable per tag (ADR-0019).
engine_module(NAME memory LAYER core
  DEPS base hash
  SOURCES src/memory.cpp src/arena.cpp)

target_compile_definitions(engine_memory PRIVATE
  ENGINE_MEMORY_TRACKING=$<BOOL:${ENGINE_MEMORY_TRACKING}>)

engine_module_tests(NAME memory
  SOURCES tests/memory_tests.cpp tests/arena_tests.cpp tests/size_table.cpp)
