# The no-listeners check's own tests (tools/ci/check-no-listeners.Tests.ps1, roadmap T80): its rules
# (tools/lib/NoListeners.psm1) over data written in the test, and its live detection against this
# test's own process listening on 127.0.0.1 — no engine binary, no prompt, nothing off loopback. A
# CPU test of a few seconds, so it runs in every Windows suite like the other tools tests.
#
# The check itself is deliberately not a CTest test: it starts every executable of a build from a
# fresh path, which belongs once at the merge with the machine to itself, after the suites; a
# CTest test cannot be left out of an author's run without a configure switch the gate would also
# have to set (a skipped one turns `tools/dev.ps1 test` into exit 75), and a CTest timeout would end
# it without its own clean-up. docs/ci/self-hosted-runners.md, "tools/ci/check-no-listeners.ps1",
# has the gate's command line.
#
# Windows only, as the check is: the firewall prompt and Get-NetTCPConnection are Windows's. Its
# own file, included after EngineTesting (which finds pwsh), so the tests' registration is not
# edited to add it.
if(WIN32 AND ENGINE_PWSH)
  add_test(NAME tools.check_no_listeners
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/ci/check-no-listeners.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.check_no_listeners PROPERTIES LABELS "tools")
endif()
