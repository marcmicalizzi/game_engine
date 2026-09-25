# Schema runtime (ADR-0007): type descriptors emitted by schemac, the type registry, JSON
# (de)serialization through reflection, and schema-version migrations.
engine_module(NAME schema LAYER core
  DEPS base hash memory containers math ids json
  SOURCES src/type_info.cpp src/json_reflect.cpp src/materialize.cpp)

# Generated test types exercising every IDL feature; built by schemac like any other schema.
# `materialize_test.schema` holds a record type, two components and the declaration joining them,
# with a unit conversion, a write-back row and a bare component: the table schemac emits for it is
# what `materialize_tests.cpp` reads (docs/subsystems/schema.md, "Materialization").
engine_schema_library(NAME schema_test_types
  SCHEMAS tests/features.schema tests/materialize_test.schema)

engine_module_tests(NAME schema
  SOURCES tests/schema_tests.cpp tests/materialize_tests.cpp tests/size_table.cpp
  DEPS schema_test_types)
