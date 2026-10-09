# Image dependencies (ADR-0014): stb_image, the decoder behind foundation/image.
#
# stb ships single headers out of one repository and cuts no per-file releases, so the pin is a
# commit of the repository; the version comment inside each header at that commit is recorded in
# third_party/LICENSES.md. Nothing is built here: stb_image.h is compiled into
# foundation/image/src/decode.cpp, and stb_image_write.h (which comes along for the ride) is
# included only by the image tests, never by the module.

include(FetchContent)

set(ENGINE_STB_COMMIT "2c980bb59875b0d32144a71867fbdebb2f77cd20"
  CACHE STRING "stb commit (the repository has no releases)")

# Headers only. SOURCE_SUBDIR points at a directory that does not exist so FetchContent never
# adds stb's own CMake files, and GIT_SHALLOW is left off because the pin is a commit hash.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        ${ENGINE_STB_COMMIT}
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(stb)

# tinyexr (BSD-3-Clause, Syoyo Fujita and contributors): the half-float OpenEXR writer and reader
# behind foundation/image's `exr.h`, which is what an HDR picture is captured as (E39, roadmap
# R79). The release is pinned by its commit. Its zlib is the miniz the release carries in
# `deps/miniz` (MIT; third_party/LICENSES.md records both), because the alternatives were worse:
# stb's zlib needs stb_image_write's implementation inside the module, which the image tests
# already compile for themselves, and a system zlib is a dependency the engine has nowhere else.
# The engine's own deflate (`deflate.h`) has no inflater, and tinyexr takes no callback.
#
# Only `tinyexr.cc` (the implementation and nothing else) and `miniz.c` are compiled, into a
# static library made in the top-level directory's scope so it inherits the CPU baseline and
# -ffp-contract=off; its warnings are off and its include directories SYSTEM, because third-party
# code is not held to the engine's -Werror. Threads, OpenMP and ZFP are off: a capture is written
# once and read once. Only foundation/image/src/exr.cpp includes tinyexr.h, through the memory
# API, so the file itself is read and written by foundation/io like every other file.
set(ENGINE_TINYEXR_COMMIT "4946b5d92e13bcc8102ac2c8efd129596a90bf75"
  CACHE STRING "tinyexr commit (tag v1.0.13)")

FetchContent_Declare(tinyexr
  GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
  GIT_TAG        ${ENGINE_TINYEXR_COMMIT}
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(tinyexr)

add_library(engine_tinyexr STATIC
  "${tinyexr_SOURCE_DIR}/tinyexr.cc"
  "${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c")
target_include_directories(engine_tinyexr SYSTEM PUBLIC
  "${tinyexr_SOURCE_DIR}" "${tinyexr_SOURCE_DIR}/deps/miniz")
target_compile_definitions(engine_tinyexr PUBLIC
  TINYEXR_USE_MINIZ=1 TINYEXR_USE_STB_ZLIB=0 TINYEXR_USE_NANOZLIB=0 TINYEXR_USE_THREAD=0
  TINYEXR_USE_OPENMP=0 TINYEXR_USE_ZFP=0)
if(MSVC)
  target_compile_options(engine_tinyexr PRIVATE /W0)
else()
  target_compile_options(engine_tinyexr PRIVATE -w)
endif()
