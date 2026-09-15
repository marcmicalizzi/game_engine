# Structured logging (docs/plan/02-architecture.md §2.3, 09 §9.3): categories with run-time
# levels, typed fields, text and JSON-lines sinks, and an in-memory ring for tests and agents.
engine_module(NAME log LAYER core
  DEPS base containers time platform
  SOURCES src/log.cpp)

# Compile-time floor for log records; empty means 0 (trace) in Debug and 1 (debug) otherwise.
if(NOT ENGINE_LOG_MIN_LEVEL STREQUAL "")
  target_compile_definitions(engine_log PUBLIC ENGINE_LOG_MIN_LEVEL=${ENGINE_LOG_MIN_LEVEL})
endif()

engine_module_tests(NAME log
  SOURCES tests/log_tests.cpp tests/size_table.cpp
  DEPS ids)
