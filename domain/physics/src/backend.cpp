// Jolt's process-wide state, and the bridge from its diagnostics to core/log.

#include "backend.h"

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/log/log.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace engine::physics {

ENGINE_LOG_CATEGORY_DEFINE(log_physics, "physics");

const char* status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::NotFound: return "not-found";
    case Status::LimitReached: return "limit-reached";
    case Status::Unsupported: return "unsupported";
    case Status::BackendError: return "backend-error";
  }
  return "unknown";
}

const char* layer_name(Layer layer) noexcept {
  switch (layer) {
    case Layer::Static: return "static";
    case Layer::Moving: return "moving";
    case Layer::Debris: return "debris";
    case Layer::Kinematic: return "kinematic";
    case Layer::Query: return "query";
    case Layer::Count: break;
  }
  return "invalid";
}

namespace {

std::mutex g_backend_mutex;  // engine-lint: allow-std-container cold init, not a container
u32 g_backend_users = 0;

// GCC and Clang check printf-style arguments, and under -Wformat=2 they ask to be told which
// parameter is the format string; MSVC has no such attribute.
#if ENGINE_COMPILER_MSVC
#define ENGINE_PHYSICS_PRINTF_LIKE(fmt, first)
#else
#define ENGINE_PHYSICS_PRINTF_LIKE(fmt, first) __attribute__((format(printf, fmt, first)))
#endif

// Jolt formats its diagnostics with printf. The engine does not log that way (AGENTS.md), so
// the formatted line becomes one field of a structured record rather than the message itself.
ENGINE_PHYSICS_PRINTF_LIKE(1, 2) void trace_impl(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  ENGINE_LOG_INFO(log_physics, "backend trace", log::field("text", buffer));
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed_impl(const char* expression, const char* message, const char* file,
                        JPH::uint line) {
  ENGINE_LOG_ERROR(log_physics, "backend assert", log::field("expr", expression),
                   log::field("message", message != nullptr ? message : ""),
                   log::field("file", file), log::field("line", static_cast<u64>(line)));
  return true;  // break into the debugger, as Jolt's own samples do
}
#endif

}  // namespace

void backend_acquire() {
  const std::lock_guard<std::mutex> lock(g_backend_mutex);
  if (g_backend_users++ != 0) return;

  JPH::RegisterDefaultAllocator();
  JPH::Trace = trace_impl;
  JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assert_failed_impl;)
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
}

void backend_release() noexcept {
  const std::lock_guard<std::mutex> lock(g_backend_mutex);
  ENGINE_ASSERT(g_backend_users > 0, "physics: backend released more often than acquired");
  if (--g_backend_users != 0) return;

  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
}

}  // namespace engine::physics
