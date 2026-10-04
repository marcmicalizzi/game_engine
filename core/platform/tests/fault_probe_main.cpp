// engine_platform_fault_probe: a process that fails on request, one argument per kind, for
// fault_report_tests.cpp to start as a child and read the line its death prints
// (docs/subsystems/platform.md, "Every fatal fault prints one line").
//
//   engine_platform_fault_probe <kind>
//
//   none               exit 0 having faulted at nothing (the control)
//   read | write       an access through the address 0x10
//   execute            a call into a data page, which is not executable
//   stack-overflow     recursion until the stack runs out
//   illegal            ud2
//   divide             an integer divided by zero
//   abort              std::abort()
//   worker             `write`, on a thread named "fault-worker" while main waits to join it
//   pure-virtual       a pure virtual function called during construction (Windows)
//   invalid-parameter  fclose(nullptr), which the C runtime validates (Windows)
//   bus                a read of a shared map past the end of its file (Linux)
//
// It calls `require_cpu_baseline()` first, as every main does, which is what links the
// translation unit whose pre-main initializer installs the handlers. Every address and divisor
// comes through a volatile, so no compiler can see the fault coming and fold it away or refuse it.
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/platform/cpu_baseline.h>
#include <core/platform/thread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace engine;

namespace {

volatile uintptr_t g_bad_address = 0x10;
volatile int g_zero = 0;
volatile u64 g_never = ~u64{0};
// A `ret`, in a writable data page, which no system here maps executable.
unsigned char g_not_code[16] = {0xC3};

ENGINE_NO_INLINE void fault_read() {
  const int value = *reinterpret_cast<volatile int*>(g_bad_address);
  (void)value;
}

ENGINE_NO_INLINE void fault_write() { *reinterpret_cast<volatile int*>(g_bad_address) = 1; }

ENGINE_NO_INLINE void fault_execute() {
  using Code = void (*)();
  volatile uintptr_t where = reinterpret_cast<uintptr_t>(&g_not_code[0]);
  reinterpret_cast<Code>(where)();
}

ENGINE_NO_INLINE u64 recurse(u64 depth) {
  volatile char frame[512];
  frame[0] = static_cast<char>(depth);
  if (depth == g_never) return 0;
  return recurse(depth + 1) + static_cast<u64>(frame[0]);
}

ENGINE_NO_INLINE void fault_illegal() {
#if defined(_WIN32)
  __ud2();
#else
  __builtin_trap();
#endif
}

// The quotient goes to a volatile: a quotient nobody reads is dead code to an optimizer that
// treats division as free of side effects, which GCC does (it removed this division once, through
// an unused return value).
volatile int g_sink = 0;

ENGINE_NO_INLINE void fault_divide() {
  volatile int numerator = 1;
  g_sink = numerator / g_zero;
}

#if defined(_WIN32)
struct Base {
  Base() { call(); }
  virtual ~Base() = default;
  // Out of line, so the call goes through the vtable under construction — Base's, whose slot is
  // the runtime's _purecall — rather than being devirtualized to a function that does not exist.
  ENGINE_NO_INLINE void call() { pure(); }
  virtual void pure() = 0;
};
struct Derived final : Base {
  void pure() override {}
};

ENGINE_NO_INLINE void fault_pure_virtual() {
  Derived d;
  (void)d;
}

ENGINE_NO_INLINE void fault_invalid_parameter() {
  FILE* volatile none = nullptr;
  (void)std::fclose(none);
}
#else
ENGINE_NO_INLINE void fault_bus() {
  const int fd = ::memfd_create("engine-fault-probe", 0);
  if (fd < 0 || ::ftruncate(fd, 4096) != 0) std::exit(2);
  void* map = ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED || ::ftruncate(fd, 0) != 0) std::exit(2);
  const char value = *static_cast<volatile const char*>(map);
  (void)value;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  platform::require_cpu_baseline();
#if !defined(_WIN32)
  // A core file per run is not what the test is for: the process still dies of its signal.
  rlimit no_core{0, 0};
  (void)::setrlimit(RLIMIT_CORE, &no_core);
#endif
  if (argc != 2) {
    std::fprintf(stderr, "usage: engine_platform_fault_probe <kind>\n");
    return 2;
  }
  const char* kind = argv[1];
  const auto is = [kind](const char* name) { return std::strcmp(kind, name) == 0; };
  if (is("none")) return 0;
  if (is("read")) fault_read();
  if (is("write")) fault_write();
  if (is("execute")) fault_execute();
  if (is("stack-overflow")) (void)recurse(0);
  if (is("illegal")) fault_illegal();
  if (is("divide")) fault_divide();
  if (is("abort")) std::abort();
  if (is("worker")) {
    std::thread worker([] {
      (void)platform::set_current_thread_name("fault-worker");
      fault_write();
      // Not a tail call, so this frame — the probe's — is on the stack the line walks.
      g_sink = 1;
    });
    worker.join();
  }
#if defined(_WIN32)
  if (is("pure-virtual")) fault_pure_virtual();
  if (is("invalid-parameter")) fault_invalid_parameter();
#else
  if (is("bus")) fault_bus();
#endif
  std::fprintf(stderr, "engine_platform_fault_probe: '%s' did not fault\n", kind);
  return 1;
}
