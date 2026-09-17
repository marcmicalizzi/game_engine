# input: action bindings, one InputState per simulation tick, and a replayable input log
# (docs/plan/05-simulation.md sections 5.2 and 5.10). Deliberately not a dependent of window:
# it consumes plain tick-stamped RawEvents, so a headless server and a replay read from a file
# run the same code as a session with a display. The window -> RawEvent conversion belongs to
# whoever owns the window (apps/engine_view).
engine_module(NAME input LAYER foundation
  DEPS base containers math time json io
  SOURCES src/action_map.cpp src/input_state.cpp src/input_log.cpp)

engine_module_tests(NAME input
  SOURCES tests/input_tests.cpp tests/log_tests.cpp tests/corpus_tests.cpp tests/size_table.cpp)

# The corpus test replays the device logs committed under content/input-logs/, which are found
# relative to the source tree rather than to the executable: a test binary sits wherever the
# preset put it. Same idea as ENGINE_APP_PATH in cmake/EngineModule.cmake.
target_compile_definitions(engine_input_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
