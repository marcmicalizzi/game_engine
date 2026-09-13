# Header-only hashing primitives: 64-bit mixers, byte hashing, and Hash<T> for container keys.
engine_module(NAME hash LAYER core
  DEPS base)

engine_module_tests(NAME hash
  SOURCES tests/hash_tests.cpp)
