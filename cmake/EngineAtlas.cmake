# Atlas dependencies (ADR-0014): xatlas, the charting, parameterization and packing library behind
# the content build's `--atlas repack` (domain/atlas, docs/subsystems/atlas.md).
#
# xatlas is one .cpp and one .h with a premake file and no CMake, and it cuts no releases, so the
# pin is a commit of the repository (third_party/LICENSES.md records it). The archive is only
# populated — SOURCE_SUBDIR names a directory that does not exist, so FetchContent never looks for
# a build of its own — and `xatlas.cpp` is compiled here into a static library whose include
# directory is SYSTEM, so its own warnings never reach the engine's -Werror builds.
#
# The library is created in the top-level directory's scope (this file is include()d from the
# top-level CMakeLists.txt), so it inherits the CPU baseline's compile options like every other
# dependency built here (cmake/EngineCpuBaseline.cmake, docs/plan/08-toolchain.md §8.9). It has
# no arch options and no intrinsics of its own: plain scalar C++.
#
# **XA_MULTITHREADED=0, and why.** xatlas's own task scheduler starts one thread per logical CPU
# for every `xatlas::Create()`, whatever the caller's thread budget. Two things are wrong with that
# here: `engine-content build-all` already runs one mesh per performance-pool worker, so a manifest
# of twenty meshes would start twenty schedulers of thirty-five threads each; and the content
# build's rule is that its bytes do not depend on the thread count (AGENTS.md), which xatlas's
# scheduler is designed to honour — every task writes a slot of its own — but which nothing here
# could vary to test, since its thread count is the machine's. Single-threaded, the question does
# not arise. The parallelism the atlas step has is its own: one job per material, and the rebake's
# rows, both merged by index (docs/subsystems/atlas.md, "Determinism").

include(FetchContent)

set(ENGINE_XATLAS_COMMIT "f700c7790aaa030e794b52ba7791a05c085faf0c"
  CACHE STRING "xatlas commit (the repository has no releases)")

# GIT_SHALLOW is left off because the pin is a commit hash.
FetchContent_Declare(xatlas
  GIT_REPOSITORY https://github.com/jpcy/xatlas.git
  GIT_TAG        ${ENGINE_XATLAS_COMMIT}
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(xatlas)

add_library(xatlas STATIC "${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp")
target_include_directories(xatlas SYSTEM PUBLIC "${xatlas_SOURCE_DIR}/source/xatlas")
target_compile_definitions(xatlas PRIVATE XA_MULTITHREADED=0)
# Third-party code is not held to the engine's warning policy (engine_apply_warnings is never
# called on it); these keep its build log quiet on every compiler rather than merely non-fatal.
if(MSVC)
  target_compile_options(xatlas PRIVATE /W0)
else()
  target_compile_options(xatlas PRIVATE -w)
endif()
