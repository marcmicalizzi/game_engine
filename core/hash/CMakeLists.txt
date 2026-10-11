# Header-only hashing primitives: 64-bit mixers, byte hashing, and Hash<T> for container keys;
# SHA-1 and base64 for the WebSocket handshake and the host's session token (foundation/net).
engine_module(NAME hash LAYER core
  DEPS base)

engine_module_tests(NAME hash
  SOURCES tests/hash_tests.cpp tests/sha1_base64_tests.cpp)
