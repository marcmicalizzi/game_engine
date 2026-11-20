# Profiling hooks: Tracy zones when ENGINE_TRACY is on, nothing otherwise; the macros are the
# whole interface, so every module can instrument itself at no cost when off. Beside them the
# startup trace (startup_trace.h): stages, milestones and process samples, recorded only while a
# host has turned it on, written as Chrome trace JSON and summarized (profiling.md, "The startup
# trace"). It is compiled, so the definitions are PUBLIC: the module's own source sees them too.
if(ENGINE_TRACY)
  engine_module(NAME profiling LAYER core
    DEPS base containers json
    SOURCES src/startup_trace.cpp src/frame_capture.cpp
    EXTERNAL_DEPS Tracy::TracyClient)
  target_compile_definitions(engine_profiling PUBLIC ENGINE_PROFILING=1)
  # Tracy's headers are third-party code: our warning policy does not apply to them.
  get_target_property(_tracy_include TracyClient INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(engine_profiling SYSTEM PUBLIC ${_tracy_include})
else()
  engine_module(NAME profiling LAYER core
    DEPS base containers json
    SOURCES src/startup_trace.cpp src/frame_capture.cpp)
  target_compile_definitions(engine_profiling PUBLIC ENGINE_PROFILING=0)
endif()

engine_module_tests(NAME profiling
  SOURCES tests/profile_tests.cpp tests/startup_trace_tests.cpp tests/frame_capture_tests.cpp
          tests/size_table.cpp)

# The startup trace's readers on a streamed session's shape (T210): the summary, the Chrome trace
# and the acyclic check by size, and the copies of them as they were before T210 beside them.
engine_module_bench(NAME profiling SOURCES bench/startup_trace_bench.cpp)
