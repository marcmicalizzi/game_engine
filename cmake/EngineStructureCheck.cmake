# Ceilings for structure (roadmap T81; docs/ci/structure-and-citations.md): the hub files every
# feature edits, pinned the way a size table pins a hot type's bytes. It reads files only, so it
# runs under every preset, the minimal ones and the Linux containers included, and gives the same
# numbers on each (line endings normalised, paths compared by the tree's own spelling).
#
# A file of its own, included last from the top-level CMakeLists.txt, rather than two more tests in
# cmake/EngineTesting.cmake: the two are registered the same way docs_check and tools.docs_check
# are, and `tools/dev.ps1 test -Affected` selects structure_check in every case through
# tools/lib/Affected.psm1's list of the checks that always run.
#
# ENGINE_PWSH is found by cmake/EngineTesting.cmake, which the top-level file includes first.
if(ENGINE_PWSH)
  add_test(NAME structure_check
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/structure-check.ps1" -Root "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(structure_check PROPERTIES LABELS "lint")

  # And the check's own tests, over fixture trees in a temporary directory.
  add_test(NAME tools.structure_check
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/structure-check.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.structure_check PROPERTIES LABELS "tools")
else()
  message(WARNING "pwsh not found; structure_check and tools.structure_check tests not registered")
endif()
