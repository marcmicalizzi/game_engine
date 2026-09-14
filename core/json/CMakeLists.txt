# JSON document model, parser, and canonical writer. The authoring document, the protocol, and
# all generated serialization go through this module (ADR-0002).
engine_module(NAME json LAYER core
  DEPS base memory containers
  SOURCES src/json_value.cpp src/json_parse.cpp src/json_write.cpp)

engine_module_tests(NAME json
  SOURCES tests/json_tests.cpp tests/size_table.cpp)
