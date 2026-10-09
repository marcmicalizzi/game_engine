# Build options. Keep this list short; most behaviour is fixed by policy, not options.

option(ENGINE_BUILD_TESTS "Build unit tests and register them with CTest" ON)
# The minimal configuration (ADR-0027): every optional capability off, whatever the individual
# ENGINE_WITH_<NAME> options say. The *-minimal presets set it and CI builds one of them; it is
# what proves that nothing in the tree depends on a capability.
option(ENGINE_MINIMAL "Configure the minimal build: every optional capability OFF" OFF)
option(ENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ENGINE_ASAN "Enable AddressSanitizer" OFF)
option(ENGINE_UBSAN "Enable UndefinedBehaviorSanitizer (Clang/GCC only)" OFF)

# Per-tag memory attribution (pointer -> tag table). On for development configurations,
# off for shipping builds where only totals are kept.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
  set(_engine_tracking_default OFF)
else()
  set(_engine_tracking_default ON)
endif()
option(ENGINE_MEMORY_TRACKING "Track allocations per tag (development builds)" ${_engine_tracking_default})

# Compile-time floor for log records: 0 trace, 1 debug, 2 info, 3 warn, 4 error, 5 fatal.
# Empty keeps the per-configuration default (0 in Debug, 1 otherwise); see core/log.
set(ENGINE_LOG_MIN_LEVEL "" CACHE STRING "Compile-time minimum log level (0-5); empty for the default")

# Default to Debug for single-config generators when nothing was requested.
get_property(_engine_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _engine_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Sanitizers.
if(ENGINE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    # MSVC ASan is incompatible with /RTC and incremental linking. The C flags too (T66): SQLite,
    # miniaudio, SDL, flecs and volk are C, and until 2026-10-09 their objects were compiled with
    # both /RTC1 and /fsanitize=address — which MSVC 14.51 compiles without a word, against its
    # documentation, so nothing said the C half of the build was outside the supported combination.
    foreach(_cfg "" _DEBUG _RELWITHDEBINFO)
      string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS${_cfg} "${CMAKE_CXX_FLAGS${_cfg}}")
      string(REPLACE "/RTC1" "" CMAKE_C_FLAGS${_cfg} "${CMAKE_C_FLAGS${_cfg}}")
    endforeach()
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
endif()

if(ENGINE_UBSAN AND NOT MSVC)
  # -fno-sanitize-recover: undefined behaviour stops the process with a report instead of printing
  # one and carrying on, so a CTest test that reaches it fails (T11; docs/ci/what-to-run.md,
  # "Sanitizers"). Recovering would leave the report in a log nobody reads and the test green.
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
endif()

# Third-party files excused from one named check each, every entry with the reason it is the
# dependency's and not ours (cmake/sanitizer-ignorelist.txt, roadmap F15). Compile-time, because a
# fatal check cannot be suppressed at run time; per file, so nothing of the engine's and no other
# check is touched. Clang only, and per language: GCC has no such list and refuses the flag, and
# the C dependencies (SQLite, SDL, flecs, volk) are compiled by the system's `cc` — GCC on the
# Linux images — even in the Clang presets, which name only the C++ compiler. Every entry is a C++
# file today, so the C side loses nothing.
if((ENGINE_ASAN OR ENGINE_UBSAN) AND NOT MSVC)
  set(_engine_ignorelist "${CMAKE_CURRENT_LIST_DIR}/sanitizer-ignorelist.txt")
  add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-fsanitize-ignorelist=${_engine_ignorelist}>"
    "$<$<COMPILE_LANG_AND_ID:C,Clang>:-fsanitize-ignorelist=${_engine_ignorelist}>")
endif()

# Engine targets are built without exceptions where the compiler allows it; tests and tools
# keep them. RTTI stays on everywhere: the engine never uses dynamic_cast or typeid, but a
# polymorphic class compiled without RTTI in a library and subclassed in a test compiled with
# it has no typeinfo to link against (Itanium ABI), and on MSVC disabling RTTI breaks standard
# library components. The cost is one typeinfo per polymorphic class.
set(ENGINE_NO_EXCEPTIONS_FLAGS "")
if(MSVC)
  # /EHsc is the default and is required by the standard library headers we compile;
  # exception *use* is forbidden by convention and lint rather than by flag on MSVC.
else()
  set(ENGINE_NO_EXCEPTIONS_FLAGS -fno-exceptions)
endif()
