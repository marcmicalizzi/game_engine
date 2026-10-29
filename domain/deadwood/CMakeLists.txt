# Deadwood capability (ADR-0027): posed, damaged remains of the source tree geometry.
# OPTIONAL removes this module and its tests when disabled. WHOLE_ARCHIVE retains the
# mesh-generator registrar and generated schema descriptions. No scheduler or render entry.
# Source geometry comes from another removable capability, so declare that graph edge.
engine_capability_requires(deadwood flora)

# Import the existing flora recipe without generating or compiling a second copy of it.
engine_capability_enabled(deadwood _enabled)
if(_enabled)
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated/deadwood_schemas")
  add_custom_command(
    OUTPUT "${_gen}/include/schemas/deadwood.h" "${_gen}/src/deadwood.cpp"
    COMMAND engine_schemac --out "${_gen}"
      --schema-root "${CMAKE_SOURCE_DIR}/domain/flora/schemas"
      "${CMAKE_CURRENT_SOURCE_DIR}/schemas/deadwood.schema"
    DEPENDS engine_schemac schemas/deadwood.schema ../flora/schemas/flora.schema
    VERBATIM)
  engine_module(NAME deadwood_schemas LAYER core CAPABILITY deadwood WHOLE_ARCHIVE
    DEPS base hash memory containers math ids json schema flora_schemas
    SOURCES "${_gen}/src/deadwood.cpp")
  target_include_directories(engine_deadwood_schemas_impl PUBLIC "${_gen}/include")
endif()

engine_module(NAME deadwood LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture flora deadwood_schemas
  SOURCES src/deadwood.cpp src/container.cpp)

engine_module_tests(NAME deadwood
  SOURCES tests/deadwood_tests.cpp tests/determinism_tests.cpp tests/size_table.cpp)
