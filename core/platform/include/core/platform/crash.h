#pragma once

// **A fault in the process, and the host's turn after it** (roadmap T216; docs/subsystems/
// platform.md, "Every fatal fault prints one line" and "The host's turn after a fault").
//
// Every fatal fault already prints one line on stderr (the fault, the faulting instruction and a
// stack as module plus offset). On 2026-11-22 the owner's walk of the forest ended in an access
// violation and the line was all that was left: no input log, no frame records, no report, and the
// two marks made with M went with them. A host that holds records registers here for its turn:
//
// - **a crash file** (`set_crash_file`), small JSON the handler writes itself, with nothing
//   allocated, before anything else runs: the line and its parts, so the fault is on disk beside
//   the report even when the host's turn never comes;
// - **the host's hook** (`set_crash_hook`): a function pointer and a context, run on a thread of
//   its own made when it is registered — so a stack overflow or a corrupt stack on the faulting
//   thread does not take the hook's stack with it — while the faulting thread waits. **Bounded**:
//   a hook past its deadline, or a second fault inside the hook (on its thread), ends the process
//   at once, the first line already written; a fault on any other thread meanwhile waits for the
//   first to end the process. The hook may allocate and write files: it runs in a broken process,
//   so it may also hang on a lock the faulting thread held, which is what the deadline is for, and
//   what a host makes durable as it goes is what survives a hook that never finishes.
//
// A process that registered a hook ends with `k_exit_crashed` (77) whatever the hook did; one
// that did not keeps the statuses it always had (the exception's code on Windows, the signal
// re-raised on Linux). No dialog on either platform.
//
// Under a sanitizer (`ENGINE_FAULT_UNDER_SANITIZER`) nothing is installed — the sanitizer's runtime
// reports the fault itself and ends the process — and `set_crash_hook` says so by returning false.

#include <core/base/types.h>

#include <string_view>

// 1 when this build carries a sanitizer whose runtime reports faults itself, with more than these
// handlers can (an AddressSanitizer, ThreadSanitizer or MemorySanitizer build, `msvc-asan` and
// `linux-clang-asan` among them): the fault report then installs nothing and leaves every fault to
// that runtime. Defined once, here, because the module's test and a host's crash case ask the same
// question (roadmap F16, T216) and two copies could disagree. Read from the compiler, which knows
// what it instrumented, rather than from a CMake option.
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

namespace engine::platform {

// The exit code of a process that faulted after its host registered for its turn (T216): the line
// on stderr, the crash file, the hook, then this — whether the hook finished, ran past its
// deadline or faulted itself. Beside 75 (the GPU lock's wait ran out), 76 (the device was lost)
// and 78 (the CPU is older than the build) and used by nothing else. It is not a skip: CTest
// treats only 75 as one here (Automake's convention of 77 for a skip is not this tree's).
inline constexpr i32 k_exit_crashed = 77;

// The most return addresses a report carries after the faulting instruction.
inline constexpr u32 k_crash_stack_frames = 16;

// What the hook is handed. Every string points into the handler's own static buffers and stays
// valid until the process ends; none is null.
struct CrashInfo {
  // What happened: "access violation", "stack overflow", "abort()", "std::terminate()",
  // "assertion failed", "SIGSEGV, address not mapped", ...
  const char* what = "";
  // The exception's code on Windows (0xC0000005, ...; 3 for abort, terminate and an assertion),
  // the signal's number on Linux.
  u64 code = 0;
  // A memory fault's data address and whether it was "reading", "writing" or "executing"; 0 and ""
  // for anything else.
  u64 address = 0;
  const char* access = "";
  // The faulting instruction as module plus offset ("engine-view.exe+0x25b7"), or "" for a failure
  // that is a call (abort, terminate, an assertion), whose stack starts in the runtime.
  const char* at = "";
  // The stack, each entry module plus offset, innermost first.
  u32 stack_count = 0;
  const char* stack[k_crash_stack_frames] = {};
  // The thread: the OS's id, the name it gave `set_current_thread_name` (or ""), and whether it is
  // the process's main thread.
  u64 thread_id = 0;
  const char* thread_name = "";
  bool main_thread = false;
  // A failed ENGINE_VERIFY / ENGINE_ASSERT / ENGINE_UNREACHABLE (core/base/assert.h): its
  // expression, message, file and line; "" and 0 otherwise.
  const char* expression = "";
  const char* message = "";
  const char* file = "";
  i32 line = 0;
  // The line as printed on stderr, without its newline, and where the crash file went ("" none).
  const char* line_text = "";
  const char* crash_file = "";
};

using CrashHook = void (*)(void* context, const CrashInfo& info);

// Registers the host's turn: `hook(context, info)` on the hook's own thread after the line and the
// crash file, for at most `deadline_ms`. One host; a second call replaces the first, and a null
// hook clears it (the process then keeps the system's exit statuses). Makes the hook's thread on
// the first call. False when nothing will run it: a sanitizer build, `ENGINE_ERROR_DIALOGS=1`, or
// a thread that could not be made.
bool set_crash_hook(CrashHook hook, void* context, u32 deadline_ms) noexcept;

// The crash file the handler writes itself, before the hook (format `engine.crash`, version 1:
// the CrashInfo's fields and the exit code). The path is copied; an empty one writes none. A path
// past 1023 bytes is refused (false).
bool set_crash_file(std::string_view path) noexcept;

}  // namespace engine::platform
