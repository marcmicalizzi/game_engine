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
