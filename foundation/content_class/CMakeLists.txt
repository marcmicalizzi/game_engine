# content_class: content classes, view policies and the one decision every output calls
# (ADR-0033, ADR-0073; docs/subsystems/content_class.md). Not OPTIONAL: a build switch is what
# ADR-0033 forbids as the mechanism, so ENGINE_MINIMAL and every ENGINE_WITH_ switch keep it.
engine_module(NAME content_class LAYER foundation
  DEPS base containers json schema schemas
  SOURCES src/content_class.cpp)

engine_module_tests(NAME content_class
  SOURCES tests/content_class_tests.cpp tests/size_table.cpp)

engine_module_bench(NAME content_class SOURCES bench/content_class_bench.cpp)
