#include <core/base/assert.h>

#include <cstdio>
#include <cstdlib>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif ENGINE_PLATFORM_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace engine {

namespace {

bool debugger_present() noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return ::IsDebuggerPresent() != 0;
#else
  // Linux: a tracer PID other than zero in /proc/self/status means a debugger is attached.
  const int fd = ::open("/proc/self/status", O_RDONLY);
  if (fd < 0) return false;
  char buf[4096];
  const auto n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) return false;
  buf[n] = '\0';
  const char* p = buf;
  const char key[] = "TracerPid:";
  while (*p != '\0') {
    bool match = true;
    for (int i = 0; key[i] != '\0'; ++i) {
      if (p[i] != key[i]) {
        match = false;
        break;
      }
    }
    if (match) {
      p += sizeof(key) - 1;
      while (*p == ' ' || *p == '\t') ++p;
      return *p != '0';
    }
    ++p;
  }
  return false;
#endif
}

}  // namespace

void assert_fail(const char* expression, const char* message, const char* file,
                 int line) noexcept {
  std::fprintf(stderr, "\n[engine] assertion failed: %s\n  %s\n  at %s:%d\n", expression,
               message != nullptr ? message : "", file, line);
  std::fflush(stderr);
  if (debugger_present()) {
    ENGINE_DEBUG_BREAK();
  }
  std::abort();
}

}  // namespace engine
