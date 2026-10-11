# The webview capability's dependencies (ENGINE_WITH_WEBVIEW; foundation/webview,
# docs/subsystems/webview.md; roadmap A37): webview/webview (MIT), the small library that embeds the
# platform's own web view in a native window, and on Windows the headers of Microsoft's WebView2
# SDK (BSD-3-Clause), which declare the COM interfaces of the Edge WebView2 runtime that Windows
# ships. engine-editor puts the web shell (apps/engine_shell) in one beside its Vulkan viewport.
#
# **Only Windows has a backend here.** webview/webview's Linux backend is WebKitGTK and its macOS one
# WKWebView; neither is installed or built on this project's machines (the Linux containers have
# no WebKitGTK), so on any platform but Windows the capability switches itself off with one line
# at configure, and everything that would use it (the editor's embedded shell) says so at run
# time instead. ENGINE_MINIMAL switches it off like every capability.
#
# **No Microsoft binary is used.** The WebView2 SDK package carries WebView2Loader.dll and import
# libraries; none is linked or copied. webview/webview's *built-in* loader
# (WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL, its default, with WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK) finds the
# installed runtime itself and calls it, so the only things taken from the SDK are its headers —
# MIDL output under the SDK package's own LICENSE.txt, read before adoption (2026-10-11): a
# BSD-3-Clause text, Copyright Microsoft Corporation — fetched at configure time and never vendored.
# The package is pinned by URL and SHA-256 (a NuGet package is a zip; DOWNLOAD_NAME gives it the
# extension the extraction step reads).
#
# The library itself is header-only; its implementation is compiled once, into the static library
# `engine_webview_core` from the library's own `core/src/webview.cc`, as third-party code judged by
# its own policy (it uses exceptions internally and catches them at its C API, which is all
# foundation/webview calls).
include_guard(GLOBAL)

if(NOT WIN32)
  set(ENGINE_WITH_WEBVIEW OFF CACHE BOOL "Build the webview capability (ADR-0027)" FORCE)
  message(STATUS "webview: no backend on this platform (WebView2 on Windows only; WebKitGTK is "
                 "not built here), so ENGINE_WITH_WEBVIEW=OFF")
endif()

engine_capability_enabled(webview _engine_webview_enabled)
if(NOT _engine_webview_enabled)
  return()
endif()

include(FetchContent)

set(ENGINE_WEBVIEW_TAG "0.12.0" CACHE STRING "webview/webview tag")
set(ENGINE_WEBVIEW2_SDK_VERSION "1.0.1150.38" CACHE STRING "Microsoft.Web.WebView2 package version")
set(ENGINE_WEBVIEW2_SDK_SHA256 "921c004bd1764b585496b2eb3eec0a59a9a98e698246f1d9a3f1c08d1d84ebd5"
    CACHE STRING "SHA-256 of that package")

# SOURCE_SUBDIR points at a directory that does not exist: the library's own CMake would fetch the
# SDK by itself, build its examples and tests, and declare targets of its own.
FetchContent_Declare(webview
  GIT_REPOSITORY https://github.com/webview/webview.git
  GIT_TAG        ${ENGINE_WEBVIEW_TAG}
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_Declare(webview2_sdk
  URL            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${ENGINE_WEBVIEW2_SDK_VERSION}"
  URL_HASH       SHA256=${ENGINE_WEBVIEW2_SDK_SHA256}
  DOWNLOAD_NAME  webview2_sdk.zip
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(webview webview2_sdk)

add_library(engine_webview_core STATIC "${webview_SOURCE_DIR}/core/src/webview.cc")
add_library(engine::webview_core ALIAS engine_webview_core)
target_include_directories(engine_webview_core SYSTEM PUBLIC "${webview_SOURCE_DIR}/core/include")
target_include_directories(engine_webview_core SYSTEM PRIVATE
  "${webview2_sdk_SOURCE_DIR}/build/native/include")
target_compile_definitions(engine_webview_core PUBLIC WEBVIEW_STATIC
  PRIVATE WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK=1 UNICODE _UNICODE)
target_link_libraries(engine_webview_core PRIVATE advapi32 ole32 shell32 shlwapi user32 version)
set_target_properties(engine_webview_core PROPERTIES FOLDER "third_party")
if(MSVC)
  target_compile_options(engine_webview_core PRIVATE /W0 /EHsc)
else()
  target_compile_options(engine_webview_core PRIVATE -w)
endif()
