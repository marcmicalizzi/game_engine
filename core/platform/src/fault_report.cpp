// Every fatal fault prints one line (fault_report.h; docs/subsystems/platform.md, "Every fatal
// fault prints one line").
//
// Before this, `quiet_error_dialogs()` switched the system's fault box off and installed nothing in
// its place, so an access violation ended the process with its exception code and not a byte of
// output: on 2026-10-03 a debug engine-view read past the end of a terrain field and stopped
// without a word, the GPU lock's wrapper reported the code as 1, and an agent spent an hour
// bisecting what one line naming the fault, the instruction and the thread would have said.
//
// **What a handler may do.** It runs where the process is already broken — a corrupt heap, a
// stack with no room left, a lock held by the thread that faulted — so it allocates nothing, takes
// no lock of ours and uses no stdio: the line is built in a static buffer (one thread at a time
// writes it, by `g_owner`) and goes out through `WriteFile` / `write(2)`. The module of an address
// comes from the loader's own lookup on Windows (`RtlPcToFileHeader`, which exception dispatch
// itself uses) and from /proc/self/maps read with `open` and `read` on Linux. The stack is the
// system's unwinder over the faulting context on Windows (`RtlVirtualUnwind`, in a DLL every
// process has loaded) and glibc's `backtrace` on Linux, called once at installation so that its
// one library load happens there and never in a handler. No symbol library is loaded: an address
// is printed as module plus offset, which is what a symbol tool takes afterwards.
#include "fault_report.h"

#include <core/base/macros.h>
#include <core/base/types.h>

#include <atomic>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <csignal>
#include <stdlib.h>
#include <windows.h>
#elif ENGINE_PLATFORM_LINUX
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// A sanitizer's runtime reports faults itself, with more than this can; it keeps them
// (ENGINE_FAULT_UNDER_SANITIZER, fault_report.h).

namespace engine::platform::detail {

namespace {

// The most return addresses the line carries after the faulting instruction.
constexpr u32 k_stack_frames = 16;

// A line in a buffer that is never allocated, long enough for the description and the stack; a
// line that would run past it is cut, never overrun.
struct Line {
  char text[1536];
  u32 size = 0;

  void put(char c) noexcept {
    if (size < sizeof(text)) text[size++] = c;
  }
  void put(const char* s) noexcept {
    while (*s != '\0')
      put(*s++);
  }
  void hex(u64 v) noexcept {
    put("0x");
    char digits[16];
    u32 n = 0;
    do {
      digits[n++] = "0123456789abcdef"[v & 15u];
      v >>= 4;
    } while (v != 0);
    while (n > 0)
      put(digits[--n]);
  }
  void hex8(u32 v) noexcept {  // an exception code, all eight digits
    put("0x");
    for (i32 shift = 28; shift >= 0; shift -= 4)
      put("0123456789abcdef"[(v >> shift) & 15u]);
  }
  void dec(u64 v) noexcept {
    char digits[20];
    u32 n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (n > 0)
      put(digits[--n]);
  }
};

Line g_line;
std::atomic<bool> g_installed{false};

}  // namespace

#if ENGINE_PLATFORM_WINDOWS

// ---- Windows -----------------------------------------------------------------------------------

namespace {

constexpr DWORD k_cpp_exception = 0xE06D7363u;    // what MSVC throws; its runtime terminates
constexpr DWORD k_heap_corruption = 0xC0000374u;  // STATUS_HEAP_CORRUPTION
// What the runtime's own invalid-parameter handler ends the process with: it fails fast
// (FAST_FAIL_INVALID_ARG), and a fail-fast exits STATUS_STACK_BUFFER_OVERRUN. Kept, so an exit code
// that meant this before means it still.
constexpr DWORD k_invalid_parameter_exit = 0xC0000409u;
constexpr DWORD k_abort_exit = 3;  // what the runtime's abort() exits with

std::atomic<DWORD> g_owner{0};  // the thread writing the line
DWORD g_code = 0;               // what it will exit with, for a second fault on that thread
DWORD g_main_thread = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
wchar_t g_path[MAX_PATH];
CONTEXT g_walk;

void write_line() noexcept {
  const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
  if (err != nullptr && err != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    (void)::WriteFile(err, g_line.text, g_line.size, &written, nullptr);
  }
  g_line.size = 0;
}

// `module+0xoffset`, the offset from the image's base (its RVA), which is what a debugger's
// `ln module+0x...` and `llvm-symbolizer --obj=<module> --relative-address 0x...` take with the
// module's PDB beside it. A bare address when no image holds it.
void put_address(u64 pc) noexcept {
  void* base = nullptr;
  (void)::RtlPcToFileHeader(reinterpret_cast<void*>(pc), &base);
  if (base == nullptr) {
    g_line.hex(pc);
    return;
  }
  const DWORD n = ::GetModuleFileNameW(static_cast<HMODULE>(base), g_path, MAX_PATH);
  DWORD start = 0;
  for (DWORD i = 0; i < n; ++i)
    if (g_path[i] == L'\\' || g_path[i] == L'/') start = i + 1;
  if (start >= n) g_line.put('?');
  for (DWORD i = start; i < n; ++i)
    g_line.put(g_path[i] >= 32 && g_path[i] < 127 ? static_cast<char>(g_path[i]) : '?');
  g_line.put('+');
  g_line.hex(pc - reinterpret_cast<u64>(base));
}

void put_thread() noexcept {
  const DWORD self = ::GetCurrentThreadId();
  g_line.put(" in thread ");
  g_line.dec(self);
  const char* name = fault_thread_name();
  if (name[0] != '\0') {
    g_line.put(" \"");
    g_line.put(name);
    g_line.put('"');
  }
  if (self == g_main_thread) g_line.put(" (main)");
}

void put_exit(DWORD code) noexcept {
  g_line.put("; exit code ");
  if (code < 0x10000u) {
    g_line.dec(code);
  } else {
    g_line.hex8(code);
  }
}

// The return addresses above a context, by the system's own unwinder: every function the compiler
// emitted has unwind data, and a leaf has its return address at the stack pointer. Stops at the
// first frame outside the thread's stack, so a corrupt frame ends the list rather than the
// process. `skip` frames are walked and not printed.
void put_stack(const CONTEXT& start, u32 skip) noexcept {
  g_line.put("; stack:");
#if defined(_M_X64)
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  ::GetCurrentThreadStackLimits(&low, &high);
  g_walk = start;
  CONTEXT& c = g_walk;
  u32 printed = 0;
  for (u32 frame = 0; frame < k_stack_frames + skip && c.Rip != 0; ++frame) {
    if (frame >= skip) {
      g_line.put(' ');
      put_address(c.Rip);
      ++printed;
    }
    DWORD64 image = 0;
    RUNTIME_FUNCTION* function = ::RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (function == nullptr) {
      if (c.Rsp < low || c.Rsp + sizeof(DWORD64) > high) break;
      c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
      c.Rsp += sizeof(DWORD64);
    } else {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      (void)::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, function, &c, &handler_data,
                               &establisher, nullptr);
    }
    if (c.Rsp < low || c.Rsp >= high) break;
  }
  if (printed == 0) g_line.put(" (none)");
#else
  (void)start;
  (void)skip;
  g_line.put(" (not walked on this architecture)");
#endif
}

// Room left on this thread's stack: a stack overflow on a thread with no guarantee has a page or
// two, and the unwinder's own frames want a few kilobytes of it.
u64 stack_room() noexcept {
  const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
  const char here = 0;
  return static_cast<u64>(reinterpret_cast<uintptr_t>(&here) -
                          reinterpret_cast<uintptr_t>(tib->StackLimit));
}

// Claims the line for this thread. A second thread that faults meanwhile waits for the first to
// end the process; a fault inside the handler itself ends it at once with the first code.
void claim(DWORD code) noexcept {
  const DWORD self = ::GetCurrentThreadId();
  DWORD expected = 0;
  if (!g_owner.compare_exchange_strong(expected, self)) {
    if (expected == self) ::TerminateProcess(::GetCurrentProcess(), g_code);
    for (;;)
      ::Sleep(INFINITE);
  }
  g_code = code;
}

[[noreturn]] void finish(DWORD code) noexcept {
  g_line.put('\n');
  write_line();
  // TerminateProcess, not ExitProcess: nothing that runs at exit — atexit handlers, DLL detach,
  // static destructors — gets a turn in a process whose state is broken.
  ::TerminateProcess(::GetCurrentProcess(), code);
  for (;;)
    ::Sleep(INFINITE);
}

const char* exception_name(DWORD code) noexcept {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "floating-point invalid operation";
    case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
    case EXCEPTION_FLT_UNDERFLOW: return "floating-point underflow";
    case EXCEPTION_FLT_INEXACT_RESULT: return "floating-point inexact result";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "floating-point denormal operand";
    case EXCEPTION_FLT_STACK_CHECK: return "floating-point stack check";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data access";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
    case k_heap_corruption: return "heap corruption";
    default: return nullptr;
  }
}

LONG WINAPI fault_filter(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  const DWORD code = record.ExceptionCode;
  // A C++ exception nobody caught is the runtime's to end: its filter calls terminate(), which
  // aborts, and the abort handler prints the line.
  if (code == k_cpp_exception && g_previous != nullptr) return g_previous(info);
  claim(code);
  g_line.put("engine: fatal: ");
  if (const char* name = exception_name(code)) {
    g_line.put(name);
  } else {
    g_line.put("exception ");
    g_line.hex8(code);
  }
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    g_line.put(kind == 1 ? " writing " : (kind == 8 ? " executing " : " reading "));
    g_line.hex(record.ExceptionInformation[1]);
  }
  g_line.put(" at ");
  put_address(reinterpret_cast<u64>(record.ExceptionAddress));
  put_thread();
  put_exit(code);
  // The description goes out before the stack is walked: a walk that faults on a broken stack
  // ends the process (claim) with the first half of the line already written.
  write_line();
  if (code != EXCEPTION_STACK_OVERFLOW || stack_room() > 16 * 1024) {
    put_stack(*info->ContextRecord, 1);
  } else {
    g_line.put("; stack: (no room left to walk it)");
  }
  finish(code);
}

// Failures that are not exceptions: abort(), a pure virtual call, an invalid argument to the C
// runtime. There is no faulting instruction; the stack from the runtime's call down says where.
[[noreturn]] ENGINE_NO_INLINE void report_call(const char* lead, const char* what,
                                               const wchar_t* detail, DWORD exit_code) noexcept {
  claim(exit_code);
  g_line.put(lead);
  g_line.put("engine: fatal: ");
  g_line.put(what);
  if (detail != nullptr && detail[0] != L'\0') {
    g_line.put(" in ");
    for (const wchar_t* p = detail; *p != L'\0'; ++p)
      g_line.put(*p >= 32 && *p < 127 ? static_cast<char>(*p) : '?');
  }
  put_thread();
  put_exit(exit_code);
  write_line();
  CONTEXT here;
  ::RtlCaptureContext(&here);
  put_stack(here, 2);  // this function and the handler that called it
  finish(exit_code);
}

// The debug runtime has written "abort() has been called" to stderr before it raises SIGABRT
// (quiet_error_dialogs() sends its reports there), with no newline after it: the line starts on a
// line of its own. The release runtime writes nothing.
#if defined(_DEBUG)
constexpr const char* k_after_abort_message = "\n";
#else
constexpr const char* k_after_abort_message = "";
#endif

void __cdecl on_abort(int) { report_call(k_after_abort_message, "abort()", nullptr, k_abort_exit); }

void __cdecl on_pure_call() {
  report_call("", "pure virtual function call", nullptr, k_abort_exit);
}

// The debug runtime names the function the bad argument went to (and has already reported the
// expression through its assertion report, a line of its own on stderr here); the release runtime
// passes nothing at all.
void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t* function, const wchar_t*,
                                  unsigned int, uintptr_t) {
  report_call("", "invalid parameter passed to the C runtime", function, k_invalid_parameter_exit);
}

}  // namespace

void install_fault_report() noexcept {
  if (ENGINE_FAULT_UNDER_SANITIZER || error_dialogs_wanted()) return;
  if (g_installed.exchange(true)) return;
  g_main_thread = ::GetCurrentThreadId();
  // Room for the filter on the main thread after a stack overflow, where the system otherwise
  // leaves a page or two. Other threads keep their default; the filter checks what it has.
  ULONG guarantee = 32 * 1024;
  (void)::SetThreadStackGuarantee(&guarantee);
  g_previous = ::SetUnhandledExceptionFilter(fault_filter);
  (void)std::signal(SIGABRT, on_abort);
  (void)::_set_purecall_handler(on_pure_call);
  (void)::_set_invalid_parameter_handler(on_invalid_parameter);
}

#elif ENGINE_PLATFORM_LINUX

// ---- Linux -------------------------------------------------------------------------------------

namespace {

constexpr int k_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

std::atomic<pid_t> g_owner{0};
// The main thread's alternate signal stack, so that a stack overflow there still has somewhere
// to run the handler. Static: nothing before main and nothing in a handler may allocate.
alignas(16) char g_alt_stack[64 * 1024];
char g_maps[4096];
char g_maps_line[1024];
char g_path[512];
char g_base_path[512];

pid_t thread_id() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); }

void write_line() noexcept {
  u32 done = 0;
  while (done < g_line.size) {
    const ssize_t n = ::write(STDERR_FILENO, g_line.text + done, g_line.size - done);
    if (n <= 0) break;
    done += static_cast<u32>(n);
  }
  g_line.size = 0;
}

bool same(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

void copy(char* to, usize capacity, const char* from) noexcept {
  usize i = 0;
  for (; i + 1 < capacity && from[i] != '\0'; ++i)
    to[i] = from[i];
  to[i] = '\0';
}

u64 parse_hex(const char*& p) noexcept {
  u64 v = 0;
  for (;; ++p) {
    const char c = *p;
    if (c >= '0' && c <= '9') {
      v = v * 16 + static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v = v * 16 + static_cast<u64>(c - 'a' + 10);
    } else {
      return v;
    }
  }
}

const char* skip_field(const char* p) noexcept {
  while (*p == ' ')
    ++p;
  while (*p != ' ' && *p != '\0')
    ++p;
  while (*p == ' ')
    ++p;
  return p;
}

// The image holding `pc` and its base, from /proc/self/maps by `open` and `read` alone. A line is
// "start-end perms offset dev inode path"; the mapping holding the pc names the file, and its base
// is that file's mapping at offset 0 — the load bias of a position-independent executable or a
// shared object, which is what `addr2line -e <module>` wants subtracted. A pc in an anonymous
// mapping has no module (false).
bool find_image(u64 pc, u64& base) noexcept {
  const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  u64 file_base = 0;
  g_base_path[0] = '\0';
  bool found = false;
  bool done = false;  // the mapping holding the pc was seen, file or not
  u32 used = 0;
  while (!done) {
    const ssize_t n = ::read(fd, g_maps, sizeof(g_maps));
    if (n <= 0) break;
    for (ssize_t i = 0; i < n && !done; ++i) {
      if (g_maps[i] != '\n') {
        if (used + 1 < sizeof(g_maps_line)) g_maps_line[used++] = g_maps[i];
        continue;
      }
      g_maps_line[used] = '\0';
      used = 0;
      const char* p = g_maps_line;
      const u64 start = parse_hex(p);
      if (*p != '-') continue;
      ++p;
      const u64 end = parse_hex(p);
      p = skip_field(p);  // perms
      const u64 offset = parse_hex(p);
      p = skip_field(skip_field(p));  // dev, inode
      if (offset == 0 && p[0] == '/') {
        file_base = start;
        copy(g_base_path, sizeof(g_base_path), p);
      }
      if (pc < start || pc >= end) continue;
      done = true;
      found = p[0] == '/';  // not for an anonymous map, the stack or the vDSO: no file to name
      if (found) {
        copy(g_path, sizeof(g_path), p);
        base = same(p, g_base_path) ? file_base : start - offset;
      }
    }
  }
  ::close(fd);
  return found;
}

// `module+0xoffset`, or a bare address where no file is mapped.
void put_address(u64 pc) noexcept {
  u64 base = 0;
  if (!find_image(pc, base)) {
    g_line.hex(pc);
    return;
  }
  const char* name = g_path;
  for (const char* p = g_path; *p != '\0'; ++p)
    if (*p == '/') name = p + 1;
  g_line.put(name);
  g_line.put('+');
  g_line.hex(pc - base);
}

const char* signal_name(int sig) noexcept {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
  }
}

const char* code_name(int sig, int code) noexcept {
  if (code <= 0) return sig == SIGABRT ? "abort()" : "sent by a process";
  switch (sig) {
    case SIGSEGV:
      return code == SEGV_MAPERR   ? "address not mapped"
             : code == SEGV_ACCERR ? "access denied"
                                   : "segmentation fault";
    case SIGBUS:
      return code == BUS_ADRALN   ? "misaligned address"
             : code == BUS_ADRERR ? "no such physical address"
             : code == BUS_OBJERR ? "object-specific hardware error"
                                  : "bus error";
    case SIGFPE:
      switch (code) {
        case FPE_INTDIV: return "integer divide by zero";
        case FPE_INTOVF: return "integer overflow";
        case FPE_FLTDIV: return "floating-point divide by zero";
        case FPE_FLTOVF: return "floating-point overflow";
        case FPE_FLTUND: return "floating-point underflow";
        case FPE_FLTRES: return "floating-point inexact result";
        case FPE_FLTINV: return "floating-point invalid operation";
        default: return "arithmetic fault";
      }
    case SIGILL: return code == ILL_PRVOPC ? "privileged instruction" : "illegal instruction";
    default: return "fault";
  }
}

// The default action for `sig`, delivered as the handler returns: the signal is blocked while
// its handler runs, so `raise` leaves it pending, and an instruction that is re-executed after
// the return meets the default too. Either way the parent's wait says the process died of it.
void default_action(int sig) noexcept {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  (void)::sigaction(sig, &dfl, nullptr);
  (void)::raise(sig);
}

void on_signal(int sig, siginfo_t* info, void* context) {
  const pid_t self = thread_id();
  pid_t expected = 0;
  if (!g_owner.compare_exchange_strong(expected, self)) {
    if (expected == self) {  // a fault inside the handler: the default action, now
      default_action(sig);
      return;
    }
    for (;;)
      ::pause();  // the first fault's thread is ending the process
  }
  const ucontext_t* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  const u64 pc = static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]);
  const u64 sp = static_cast<u64>(uc->uc_mcontext.gregs[REG_RSP]);
  const u64 error = static_cast<u64>(uc->uc_mcontext.gregs[REG_ERR]);
#else
  (void)uc;
  const u64 pc = 0;
  const u64 sp = 0;
  const u64 error = 0;
#endif
  const u64 address = reinterpret_cast<u64>(info->si_addr);
  const bool from_hardware = info->si_code > 0;
  g_line.put("engine: fatal: ");
  g_line.put(signal_name(sig));
  g_line.put(", ");
  // A fault within a few pages of the stack pointer is the stack running out: the kernel says only
  // SEGV_MAPERR or SEGV_ACCERR, and what was touched is the guard below the stack.
  const bool overflow =
      sig == SIGSEGV && from_hardware && address + 65536 > sp && address < sp + 65536;
  g_line.put(overflow ? "stack overflow" : code_name(sig, info->si_code));
  if ((sig == SIGSEGV || sig == SIGBUS) && from_hardware) {
    // The page fault's error code (x86-64): bit 1 a write, bit 4 an instruction fetch.
    g_line.put((error & 16u) != 0 ? " executing "
                                  : ((error & 2u) != 0 ? " writing " : " reading "));
    g_line.hex(address);
  }
  if (pc != 0) {
    g_line.put(" at ");
    put_address(pc);
  }
  g_line.put(" in thread ");
  g_line.dec(static_cast<u64>(self));
  char name[17] = {};
  if (::prctl(PR_GET_NAME, name, 0, 0, 0) == 0 && name[0] != '\0') {
    g_line.put(" \"");
    g_line.put(name);
    g_line.put('"');
  }
  if (self == ::getpid()) g_line.put(" (main)");
  g_line.put("; signal ");
  g_line.dec(static_cast<u64>(sig));
  g_line.put(", re-raised");
  write_line();
  // The stack, by glibc's unwinder through the signal frame. Its first frames are this handler and
  // the kernel's trampoline; the faulting pc is the one after them, and the list starts past it.
  void* frames[k_stack_frames + 8];
  const int count = ::backtrace(frames, static_cast<int>(k_stack_frames + 8));
  int from = count < 2 ? count : 2;
  for (int i = 0; i < count; ++i) {
    if (reinterpret_cast<u64>(frames[i]) == pc) {
      from = i + 1;
      break;
    }
  }
  g_line.put("; stack:");
  u32 printed = 0;
  // A jump into memory that is not code leaves the unwinder nothing to read at the pc, so it stops
  // there; the call that went there left its return address where the stack pointer points.
  if (from >= count && sig == SIGSEGV && from_hardware && (error & 16u) != 0 && sp != 0) {
    g_line.put(' ');
    put_address(*reinterpret_cast<const u64*>(sp));
    ++printed;
  }
  for (int i = from; i < count && printed < k_stack_frames; ++i, ++printed) {
    g_line.put(' ');
    put_address(reinterpret_cast<u64>(frames[i]));
  }
  if (printed == 0) g_line.put(" (none)");
  g_line.put('\n');
  write_line();
  default_action(sig);
}

}  // namespace

void install_fault_report() noexcept {
  if (ENGINE_FAULT_UNDER_SANITIZER || error_dialogs_wanted()) return;
  if (g_installed.exchange(true)) return;
  stack_t alt{};
  alt.ss_sp = g_alt_stack;
  alt.ss_size = sizeof(g_alt_stack);
  (void)::sigaltstack(&alt, nullptr);
  for (const int sig : k_signals) {
    struct sigaction old{};
    if (::sigaction(sig, nullptr, &old) != 0) continue;
    // Somebody was here first (a sanitizer's runtime, a host that embeds the engine): theirs.
    if ((old.sa_flags & SA_SIGINFO) != 0 || old.sa_handler != SIG_DFL) continue;
    struct sigaction sa{};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    (void)::sigaction(sig, &sa, nullptr);
  }
  // glibc's backtrace loads libgcc's unwinder on its first call; that call is made here, so that
  // a handler never loads a library.
  void* warm[2];
  (void)::backtrace(warm, 2);
}

#else

void install_fault_report() noexcept { g_installed.store(true); }

#endif

}  // namespace engine::platform::detail
