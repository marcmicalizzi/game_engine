# webview: the platform's own web view embedded in a native window (docs/subsystems/webview.md;
# roadmap A37) — on Windows the Edge WebView2 runtime that Windows ships, through webview/webview's
# C API and its built-in loader (cmake/EngineWebview.cmake has the dependency and why no Microsoft
# binary is used). engine-editor puts the web shell beside its native viewport in one.
#
# OPTIONAL (ADR-0027): ENGINE_WITH_WEBVIEW=OFF and the minimal build take the module and the
# fetch with it, and on every platform but Windows the switch turns itself off at configure, since
# there is no backend to build. Nothing below an app depends on it: a host that has it links it.
# It takes no window module dependency: the parent is the platform's own handle
# (`window::Window::os_handle()`), so SDL stays inside foundation/window.
engine_module(NAME webview LAYER foundation
  OPTIONAL CAPABILITY webview
  DEPS base log
  EXTERNAL_DEPS engine::webview_core
  SOURCES src/webview.cpp)
if(TARGET engine_webview AND WIN32)
  # RegGetValueW: the runtime check (webview.h, `runtime_available`).
  target_link_libraries(engine_webview PRIVATE advapi32)
endif()

engine_module_tests(NAME webview
  SOURCES tests/webview_tests.cpp)
