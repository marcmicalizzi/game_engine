#include <core/base/macros.h>
#include <core/platform/error_dialogs.h>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
#endif

namespace engine::platform {

#if ENGINE_PLATFORM_WINDOWS

namespace {

constexpr UINT k_quiet_modes =
    SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;

// GetEnvironmentVariableA rather than getenv: this runs before main, and MSVC's getenv is both
// deprecated (C4996) and a walk of a table the runtime may not have finished building.
bool dialogs_wanted() noexcept {
  char value[4] = {};
  const DWORD length = ::GetEnvironmentVariableA("ENGINE_ERROR_DIALOGS", value, sizeof(value));
  return length == 1 && value[0] == '1';
}

}  // namespace

void quiet_error_dialogs() noexcept {
  if (dialogs_wanted()) return;

  // The system's boxes: a critical error, a fault, a file that will not open.
  ::SetErrorMode(::GetErrorMode() | k_quiet_modes);

  // abort(): keep its message, send it to stderr, and skip the hand-off to fault reporting,
  // which is where "has stopped working" comes from.
  ::_set_abort_behavior(0, _CALL_REPORTFAULT);
  ::_set_error_mode(_OUT_TO_STDERR);

#if defined(_DEBUG)
  // The debug runtime's reports — warnings, errors (abort's box is one) and assertions — are
  // windows by default. A file, and the file is stderr.
  const int report_types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
  for (const int type : report_types) {
    ::_CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    ::_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
}

bool error_dialogs_quiet() noexcept { return (::GetErrorMode() & k_quiet_modes) == k_quiet_modes; }

#else

void quiet_error_dialogs() noexcept {}

bool error_dialogs_quiet() noexcept { return true; }

#endif

}  // namespace engine::platform
