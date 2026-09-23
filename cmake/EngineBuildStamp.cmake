# The build stamp: which commit an app was built from, so a tool that measures with a binary can
# refuse one older than the source it reports (tools/e10-harness.ps1; docs/content-generation.md,
# "Which binaries"). Every app links it (engine_app) and `--version` prints it.
#
# **Captured at build time, not at configure time.** CMake does not re-run on a commit, so a commit
# captured at configure would name the tree as it was when the build directory was configured —
# days before, in a long-lived build tree — and a harness trusting it would refuse every freshly
# built binary or wave through a stale one. An always-run target asks git on every build instead
# (one `rev-parse` and one lock-free `status`, tens of milliseconds) and rewrites the generated
# source only when the answer changed; the custom command's byproduct is restat, so after a build
# with no commit in between Ninja recompiles and relinks nothing.
#
# **Only apps link it**, so a commit recompiles one two-line file and relinks the handful of
# executables in bin/, not the hundreds of test and bench binaries.
set(ENGINE_BUILD_STAMP_DIR "${CMAKE_BINARY_DIR}/generated/build_stamp")
set(_engine_stamp_script "${CMAKE_CURRENT_LIST_DIR}/WriteBuildStamp.cmake")

# Once now, so the source and the header exist before the first build asks for them.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
  "-DENGINE_STAMP_DIR=${ENGINE_BUILD_STAMP_DIR}" -P "${_engine_stamp_script}")

add_custom_target(engine_build_stamp_update
  COMMAND "${CMAKE_COMMAND}" "-DENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
          "-DENGINE_STAMP_DIR=${ENGINE_BUILD_STAMP_DIR}" -P "${_engine_stamp_script}"
  BYPRODUCTS "${ENGINE_BUILD_STAMP_DIR}/engine_build_stamp.cpp"
             "${ENGINE_BUILD_STAMP_DIR}/include/engine_build_stamp.h"
  COMMENT "Build stamp"
  VERBATIM)

add_library(engine_build_stamp STATIC "${ENGINE_BUILD_STAMP_DIR}/engine_build_stamp.cpp")
add_dependencies(engine_build_stamp engine_build_stamp_update)
target_include_directories(engine_build_stamp PUBLIC "${ENGINE_BUILD_STAMP_DIR}/include")
target_compile_options(engine_build_stamp PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
engine_apply_warnings(engine_build_stamp)
