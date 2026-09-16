# window: OS windows, input events, and Vulkan surface glue over SDL3 (ADR-0012). Headless
# builds and machines without a display keep working: init() reports failure and callers skip.
engine_module(NAME window LAYER foundation
  DEPS base containers log
  EXTERNAL_DEPS SDL3::SDL3
  SOURCES src/window.cpp)
target_include_directories(engine_window SYSTEM PRIVATE
  $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME window
  SOURCES tests/window_tests.cpp tests/size_table.cpp)
