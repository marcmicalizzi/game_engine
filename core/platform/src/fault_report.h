#pragma once

// The fatal-fault line (docs/subsystems/platform.md, "Every fatal fault prints one line"): what
// `quiet_error_dialogs()` installs beside switching the boxes off, so that a fault turns into one
// line on stderr and a documented exit status instead of a silent death. Internal to the module:
// the public promise is error_dialogs.h's, and nothing outside this module calls these.

// 1 when this build carries a sanitizer whose runtime reports faults itself, with more than these
// handlers can (an AddressSanitizer, ThreadSanitizer or MemorySanitizer build, `msvc-asan` and
// `linux-clang-asan` among them): `install_fault_report` then installs nothing and leaves every
// fault to that runtime. Defined here, once, because the module's test asks the same question —
// under a sanitizer it checks that the engine stands aside instead of reading a line nothing
// prints (roadmap F16, docs/ci/what-to-run.md "Sanitizers") — and two copies of the test could
// disagree. Read from the compiler, which knows what it instrumented, rather than from a CMake
// option.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ENGINE_FAULT_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define ENGINE_FAULT_UNDER_SANITIZER 1
#endif
#endif
#ifndef ENGINE_FAULT_UNDER_SANITIZER
#define ENGINE_FAULT_UNDER_SANITIZER 0
#endif

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
