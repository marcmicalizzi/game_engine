# Asset import dependencies (docs/plan/07-content-pipeline.md section 7.2, ADR-0014): cgltf,
# a single-header glTF 2.0 parser. It has no build system of its own, so the archive is only
# populated (SOURCE_SUBDIR names a directory without a CMakeLists.txt, which tells
# FetchContent_MakeAvailable to skip add_subdirectory) and wrapped in an interface target whose
# include directory is SYSTEM, so cgltf's own warnings never reach our -Werror builds. The
# implementation is compiled once, in domain/assets/src/gltf.cpp.

include(FetchContent)

set(ENGINE_CGLTF_TAG "v1.15" CACHE STRING "cgltf tag")

FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG        ${ENGINE_CGLTF_TAG}
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  header-only-no-cmake)
FetchContent_MakeAvailable(cgltf)

add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
