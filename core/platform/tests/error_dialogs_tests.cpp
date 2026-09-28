#include <core/base/macros.h>
#include <core/platform/error_dialogs.h>

#include <doctest/doctest.h>

#if ENGINE_PLATFORM_WINDOWS
#include <crtdbg.h>
#endif

namespace {

using namespace engine;

TEST_CASE("error dialogs: this process was made quiet before main") {
  // The test main links this module, so the initializer beside the CPU baseline check has run.
  // A developer who set ENGINE_ERROR_DIALOGS=1 asked for the boxes and gets them; nothing to
  // assert in that case.
  if (!platform::error_dialogs_quiet()) {
    MESSAGE("ENGINE_ERROR_DIALOGS is set: the boxes were asked for");
    return;
  }
  CHECK(platform::error_dialogs_quiet());

#if ENGINE_PLATFORM_WINDOWS && defined(_DEBUG)
  // Asking with _CRTDBG_REPORT_MODE changes nothing and answers the mode in force. What is
  // asserted is that no report type opens a window, not that the mode is exactly ours: doctest
  // sets the assertion reports to a file and the debugger output for the length of a test case
  // (which is why a test never showed a box and a bench did), and that is as quiet as ours.
  const int report_types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
  for (const int type : report_types) {
    const int mode = ::_CrtSetReportMode(type, _CRTDBG_REPORT_MODE);
    CHECK((mode & _CRTDBG_MODE_WNDW) == 0);
    CHECK((mode & _CRTDBG_MODE_FILE) != 0);
  }
#endif
}

TEST_CASE("error dialogs: asking twice is asking once") {
  const bool before = platform::error_dialogs_quiet();
  platform::quiet_error_dialogs();
  platform::quiet_error_dialogs();
  CHECK(platform::error_dialogs_quiet() == before);
}

}  // namespace
