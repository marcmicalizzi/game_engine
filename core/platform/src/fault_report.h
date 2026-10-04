#pragma once

// The fatal-fault line (docs/subsystems/platform.md, "Every fatal fault prints one line"): what
// `quiet_error_dialogs()` installs beside switching the boxes off, so that a fault turns into one
// line on stderr and a documented exit status instead of a silent death. Internal to the module:
// the public promise is error_dialogs.h's, and nothing outside this module calls these.

namespace engine::platform::detail {

// The handlers, once: on Windows a top-level exception filter, a SIGABRT handler, the runtime's
// pure-virtual and invalid-parameter handlers, and a stack guarantee for the main thread; on Linux
// sigaction handlers for SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT on an alternate stack for the
// main thread. Called by `require_cpu_baseline()` beside `quiet_error_dialogs()`, so it has the
// same reach. Nothing when ENGINE_ERROR_DIALOGS=1 asked for the process as the system made it,
// nothing under a sanitizer, which reports faults itself, and on Linux nothing over a handler
// somebody installed first. Idempotent; safe before main.
void install_fault_report() noexcept;

// ENGINE_ERROR_DIALOGS=1 is set (error_dialogs.cpp): a developer wants the system's own answer to
// a failure, a box and a debugger, and gets neither the quiet modes nor these handlers.
bool error_dialogs_wanted() noexcept;

// The name this thread gave itself through `set_current_thread_name`, or "" — read by the handler,
// so it is a thread-local array with no initializer to run.
const char* fault_thread_name() noexcept;

}  // namespace engine::platform::detail
