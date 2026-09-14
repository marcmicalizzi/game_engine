# Schema runtime (ADR-0007): type descriptors emitted by schemac, the type registry, JSON
# (de)serialization through reflection, and schema-version migrations.
engine_module(NAME schema LAYER core
  DEPS base hash memory containers ids json
  SOURCES src/type_info.cpp src/json_reflect.cpp)

# Generated test types exercising every IDL feature; built by schemac like any other schema.
engine_schema_library(NAME schema_test_types
  SCHEMAS tests/features.schema)

engine_module_tests(NAME schema
  SOURCES tests/schema_tests.cpp tests/size_table.cpp
  DEPS schema_test_types)
