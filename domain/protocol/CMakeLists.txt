# The engine protocol (ADR-0001): JSON-RPC 2.0 dispatch with schema-typed methods, sessions over
# stored documents, and the day-one method set.
engine_module(NAME protocol LAYER domain
  DEPS base containers ids json schema log time platform tunables io schemas doc
  SOURCES src/rpc.cpp src/session.cpp src/methods.cpp)
target_compile_definitions(engine_protocol PRIVATE ENGINE_VERSION="${PROJECT_VERSION}")

engine_module_tests(NAME protocol
  SOURCES tests/protocol_tests.cpp)
