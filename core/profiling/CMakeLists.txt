# Profiling hooks: Tracy zones when ENGINE_TRACY is on, nothing otherwise. Header-only; the
# macros are the whole interface, so every module can instrument itself at no cost when off.
if(ENGINE_TRACY)
  engine_module(NAME profiling LAYER core
    DEPS base
    EXTERNAL_DEPS Tracy::TracyClient)
  target_compile_definitions(engine_profiling INTERFACE ENGINE_PROFILING=1)
  # Tracy's headers are third-party code: our warning policy does not apply to them.
  get_target_property(_tracy_include TracyClient INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(engine_profiling SYSTEM INTERFACE ${_tracy_include})
else()
  engine_module(NAME profiling LAYER core
    DEPS base)
  target_compile_definitions(engine_profiling INTERFACE ENGINE_PROFILING=0)
endif()

engine_module_tests(NAME profiling
  SOURCES tests/profile_tests.cpp)
