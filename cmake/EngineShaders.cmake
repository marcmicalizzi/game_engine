# Shader compilation (ADR-0008): Slang prebuilt compiler fetched per host platform, `.slang`
# files compiled to SPIR-V at build time, and the bytes embedded in generated headers so tests
# and tools need no file lookup. Runtime loading through the VFS and a hash-keyed cache arrive
# with the renderer's shader library.
#
#   engine_shaders(NAME <name> SOURCES <file.slang>... [PROFILE spirv_1_6] [FLAGS ...])
#
# Produces the INTERFACE target engine_shaders_<name> whose include directory holds
# <stem>.spv.h for every source, each declaring
#   namespace engine::shaders { inline constexpr unsigned char k_<stem>_spirv[] = {...}; }
# aligned for VkShaderModuleCreateInfo. Every entry point in a file is compiled; entry point
# names are preserved in the SPIR-V.

include(FetchContent)

option(ENGINE_SHADERS "Fetch the Slang compiler and build shaders" ON)
set(ENGINE_SLANG_VERSION "2026.17.1" CACHE STRING "Slang release to fetch")

if(ENGINE_SHADERS)
  if(WIN32)
    set(_slang_archive "slang-${ENGINE_SLANG_VERSION}-windows-x86_64.zip")
    set(_slangc_name "slangc.exe")
  else()
    set(_slang_archive "slang-${ENGINE_SLANG_VERSION}-linux-x86_64.tar.gz")
    set(_slangc_name "slangc")
  endif()
  FetchContent_Declare(slang
    URL "https://github.com/shader-slang/slang/releases/download/v${ENGINE_SLANG_VERSION}/${_slang_archive}")
  FetchContent_MakeAvailable(slang)
  find_program(ENGINE_SLANGC NAMES ${_slangc_name}
    PATHS "${slang_SOURCE_DIR}/bin" NO_DEFAULT_PATH REQUIRED)
  message(STATUS "engine: slangc ${ENGINE_SLANG_VERSION} at ${ENGINE_SLANGC}")
endif()

function(engine_shaders)
  set(_one NAME PROFILE)
  set(_multi SOURCES FLAGS)
  cmake_parse_arguments(SH "" "${_one}" "${_multi}" ${ARGN})
  if(NOT SH_NAME OR NOT SH_SOURCES)
    message(FATAL_ERROR "engine_shaders: NAME and SOURCES are required")
  endif()
  if(NOT SH_PROFILE)
    set(SH_PROFILE spirv_1_6)
  endif()
  set(_target engine_shaders_${SH_NAME})
  add_library(${_target} INTERFACE)
  if(NOT ENGINE_SHADERS)
    message(STATUS "engine shaders: ${SH_NAME} skipped (ENGINE_SHADERS is OFF)")
    return()
  endif()

  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SH_NAME}")
  set(_headers "")
  foreach(_src IN LISTS SH_SOURCES)
    get_filename_component(_abs "${_src}" ABSOLUTE)
    get_filename_component(_stem "${_src}" NAME_WE)
    set(_spv "${_gen}/${_stem}.spv")
    set(_header "${_gen}/include/shaders/${_stem}.spv.h")
    add_custom_command(
      OUTPUT "${_spv}"
      COMMAND "${ENGINE_SLANGC}" "${_abs}"
              -target spirv -profile ${SH_PROFILE} -emit-spirv-directly
              -fvk-use-entrypoint-name -O2 -warnings-disable 41012 ${SH_FLAGS}
              -o "${_spv}"
      DEPENDS "${_abs}"
      COMMENT "slangc: ${_src}"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND "${CMAKE_COMMAND}"
              "-DINPUT=${_spv}" "-DOUTPUT=${_header}" "-DSYMBOL=k_${_stem}_spirv"
              -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
      DEPENDS "${_spv}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
      COMMENT "embed: ${_stem}.spv"
      VERBATIM)
    list(APPEND _headers "${_header}")
  endforeach()
  add_custom_target(${_target}_build DEPENDS ${_headers})
  add_dependencies(${_target} ${_target}_build)
  target_include_directories(${_target} INTERFACE "${_gen}/include")
  message(STATUS "engine shaders: ${SH_NAME} (${SH_PROFILE})")
endfunction()
