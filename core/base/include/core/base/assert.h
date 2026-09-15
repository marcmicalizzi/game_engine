#pragma once

#include <core/base/macros.h>

namespace engine {

// Reports a failed assertion and terminates. Never returns. Breaks into the debugger first
// when one is attached.
[[noreturn]] void assert_fail(const char* expression, const char* message, const char* file,
                              int line) noexcept;

// Called by assert_fail before it prints and terminates, so a higher module (core/log) can
// record the failure in its own sinks. The hook returns normally; termination follows. Passing
// nullptr removes the hook.
using AssertHook = void (*)(const char* expression, const char* message, const char* file,
                            int line) noexcept;
void set_assert_hook(AssertHook hook) noexcept;

}  // namespace engine

// ENGINE_VERIFY: checked in every build configuration. Use for invariants whose violation
// means memory corruption or an unrecoverable engine state. Never use it on external input;
// validate that and return an error.
#define ENGINE_VERIFY(expr, message)                             \
  do {                                                           \
    if (!(expr)) [[unlikely]] {                                  \
      ::engine::assert_fail(#expr, message, __FILE__, __LINE__); \
    }                                                            \
  } while (false)

// ENGINE_ASSERT: checked in debug builds only. The expression is not evaluated in release
// builds, so it must have no side effects.
#if ENGINE_DEBUG
#define ENGINE_ASSERT(expr, message) ENGINE_VERIFY(expr, message)
#else
#define ENGINE_ASSERT(expr, message) \
  do {                               \
    (void)sizeof(expr);              \
  } while (false)
#endif

#define ENGINE_UNREACHABLE(message) \
  ::engine::assert_fail("unreachable code reached", message, __FILE__, __LINE__)
