#pragma once

// No engine binary opens a dialog when it fails.
//
// On Windows the C runtime and the system both answer a failure with a modal window: the debug
// runtime's "Debug Error! abort() has been called" with its Abort / Retry / Ignore, the
// assertion box, and Windows Error Reporting's "has stopped working". A window needs somebody to
// click it. A test or a bench run by an agent, by CTest or by CI has nobody: the process stands
// until the run's timeout, and an agent's whole session with it — which is what happened on
// 2026-09-28, when a bench of a module under construction called abort() and its box sat on the
// owner's screen while the agent that ran it waited.
//
// `quiet_error_dialogs()` turns every one of them into a line on stderr and an exit code: the
// abort message and the debug runtime's reports go to stderr, the system's fault boxes are
// switched off for this process, and the fault-reporting hand-off abort() makes is skipped. It
// runs beside the CPU baseline check, before every other dynamic initializer, so it covers every
// executable that links this module without any of them having to ask.
//
// A developer who wants the box — to press Retry and land in a debugger — sets
// ENGINE_ERROR_DIALOGS=1 in the environment, and the process is left as the system made it.
// Nothing here changes what fails or what it exits with; on platforms other than Windows there
// is nothing to switch off and the function does nothing.

namespace engine::platform {

// Idempotent, and safe before main.
void quiet_error_dialogs() noexcept;

// What the process is set to now, for the test and for a summary line: true when the system's
// fault boxes are off for this process (always true where there are none).
[[nodiscard]] bool error_dialogs_quiet() noexcept;

}  // namespace engine::platform
