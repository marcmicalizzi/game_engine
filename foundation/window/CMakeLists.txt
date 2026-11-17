# window: OS windows, input events, and Vulkan surface glue over SDL3 (ADR-0012). Headless
# builds and machines without a display keep working: init() reports failure and callers skip.
engine_module(NAME window LAYER foundation
  DEPS base containers log gpu_lock
  EXTERNAL_DEPS SDL3::SDL3
  SOURCES src/window.cpp)
target_include_directories(engine_window SYSTEM PRIVATE
  $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>)
# open_file_dialog: the platform's own open-file dialog (GetOpenFileNameW, comdlg32); desktop_state:
# whether the session is locked (WTSQuerySessionInformationW, wtsapi32).
if(WIN32)
  target_link_libraries(engine_window PRIVATE comdlg32 wtsapi32)
endif()

# NO_DEVICE: no case opens a GPU device. A Vulkan window loads the loader and takes the hold it would
# share with a device, and the one case that shows one holds a lock file in its own scratch
# directory, never the machine's (roadmap T170, what-to-run.md, "Tests that open no device").
engine_module_tests(NAME window NO_DEVICE
  SOURCES tests/window_tests.cpp tests/size_table.cpp)
