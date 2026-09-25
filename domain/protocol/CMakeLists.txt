# The engine protocol (ADR-0001): JSON-RPC 2.0 dispatch with schema-typed methods, sessions over
# stored documents, the day-one method set, and plan 06 §6.5's roles, leases and proposal layers
# (policy.h, leases.h, proposals.h), which live here rather than in a host because the writes they
# guard are this module's `doc.*` methods and every host that serves them must guard them alike.
engine_module(NAME protocol LAYER domain
  DEPS base containers ids json schema log time platform profiling tunables io schemas doc gfx
  SOURCES src/rpc.cpp src/session.cpp src/methods.cpp src/policy.cpp src/leases.cpp
          src/proposals.cpp src/policy_methods.cpp)
target_compile_definitions(engine_protocol PRIVATE ENGINE_VERSION="${PROJECT_VERSION}")

# Types the roles, leases and proposals tests and experiment E12 place in a world: a prop that
# stands in a tile and a note that has none.
engine_schema_library(NAME protocol_test_types
  SCHEMAS tests/protocol_test_types.schema)

# `bench` is for E12 alone (tests/e12_tests.cpp): it records the machine's state around the run
# the experiment page quotes, with the sampler every measurement here uses.
engine_module_tests(NAME protocol
  SOURCES tests/protocol_tests.cpp tests/policy_tests.cpp tests/e12_tests.cpp
  DEPS protocol_test_types bench)
if(TARGET engine_protocol_tests)
  # The shipped roles file (content/roles/roles.json), which one case loads.
  target_compile_definitions(engine_protocol_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
