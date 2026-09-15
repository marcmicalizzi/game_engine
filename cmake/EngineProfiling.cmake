# Profiler integration (ADR-0012, docs/plan/09-testing-profiling.md §9.3).
#
# ENGINE_TRACY compiles Tracy client zones into every module that uses core/profiling. Off, the
# macros expand to nothing and no third-party code is fetched. On, Tracy v0.14.1 (BSD-3) is
# fetched at configure time and built as a static library in on-demand mode: instrumentation
# costs a predictable few nanoseconds per zone until a profiler connects. Release presets turn
# it on; debug presets leave it off.

option(ENGINE_TRACY "Compile Tracy profiler zones in (fetches Tracy at configure time)" OFF)

if(ENGINE_TRACY)
  include(FetchContent)
  set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
  set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
  set(TRACY_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(tracy)
  message(STATUS "engine: Tracy profiling enabled (on-demand)")
endif()
