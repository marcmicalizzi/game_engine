# Texture dependencies (ADR-0014, ADR-0036): the block-compression encoders behind domain/texture.
#
# richgel999/bc7enc_rdo carries the three pieces the texture build needs, all by Richard Geldreich,
# Jr., under "MIT or public domain (Unlicense), choose whichever you prefer" — the repository's
# LICENSE, read at the pinned commit before this was added; third_party/LICENSES.md records it:
#
#   bc7enc.cpp/.h     the BC7 encoder (modes 1, 5, 6 and 7), scalar C++
#   rgbcx.cpp/.h      the BC1, BC3, BC4 and BC5 encoders and decoders, scalar C++, with its
#                     rgbcx_table4.h of precomputed total orderings
#   bc7decomp.cpp/.h  a BC7 decoder (integer SSE2 on x86-64): what `engine-content texture`
#                     measures its own error with, and the tests' reference decoder for BC7
#
# Nothing else in the repository is compiled: not the rate-distortion post-processors (ert,
# rdo_bc_encoder), not the ISPC encoder (bc7e.ispc, Apache-2.0, and a toolchain of its own), not
# lodepng or miniz, not the command-line tool. The repository cuts no releases, so the pin is a
# commit, and GIT_SHALLOW is left off because of that.
#
# **Why these encoders.** They are scalar C++ with no intrinsics in the encode path and no C
# library call but `sqrtf`, `fabsf`, `floor` and `floorf` — every one correctly rounded on every
# platform — so with contraction off (ADR-0035) a `.tex` is the same bytes from every toolchain;
# their sorts sort integers with the element index packed into the low bits, so they are total
# orders. That is checked, not assumed: domain/texture's determinism test pins section hashes
# taken on MSVC (docs/subsystems/texture.md, "Determinism"). ISPC's bc7e is faster and is none of
# those things.
#
# The library is created in the top-level directory's scope, so it inherits the CPU baseline and
# -ffp-contract=off like every other dependency (cmake/EngineCpuBaseline.cmake,
# cmake/EngineFpContraction.cmake). Its include directory is SYSTEM and its own warnings are off:
# third-party code is not held to the engine's -Werror.

include(FetchContent)

set(ENGINE_BC7ENC_COMMIT "b9438627eef73a1157e84201b6fa6eb2ffd6d9f0"
  CACHE STRING "bc7enc_rdo commit (the repository has no releases)")

# SOURCE_SUBDIR names a directory that does not exist, so the repository's own CMakeLists — which
# builds the command-line tool with OpenMP and ISPC — is never added.
FetchContent_Declare(bc7enc_rdo
  GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
  GIT_TAG        ${ENGINE_BC7ENC_COMMIT}
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(bc7enc_rdo)

add_library(bc7enc STATIC
  "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
  "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp"
  "${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp")
target_include_directories(bc7enc SYSTEM PUBLIC "${bc7enc_rdo_SOURCE_DIR}")
if(MSVC)
  target_compile_options(bc7enc PRIVATE /W0)
else()
  target_compile_options(bc7enc PRIVATE -w)
endif()
