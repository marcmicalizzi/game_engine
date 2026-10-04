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
  # Tracy's client installs crash handlers of its own (a top-level exception filter on Windows,
  # sigaction on Linux) over the engine's, which report to a connected profiler and otherwise end
  # the process with nothing on stderr — on Linux by abort(), whatever the fault was. Every engine
  # binary prints one line for a fatal fault instead (core/platform, docs/subsystems/platform.md,
  # "Every fatal fault prints one line"), and a release build is where that line matters most.
  set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
  # The Tracy client opens a listening socket when the process starts. On every interface, that
  # makes Windows Firewall raise a prompt once per executable *path*, and every agent worktree
  # and preset has its own paths: on 2026-09-18 thirty-five pending prompts, each one a GPU
  # client process, pushed the machine past the number of clients the NVIDIA driver survives
  # and every vkCreateDevice crashed inside the driver until they were dismissed
  # (docs/subsystems/profiling.md). Loopback listeners do not prompt, so that is the default;
  # profiling another machine is the opt-in.
  option(ENGINE_TRACY_REMOTE "Let the Tracy client accept connections from other machines" OFF)
  if(NOT ENGINE_TRACY_REMOTE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(tracy)
  message(STATUS "engine: Tracy profiling enabled (on-demand)")
endif()
