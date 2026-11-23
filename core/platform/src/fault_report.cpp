// Every fatal fault prints one line (fault_report.h; docs/subsystems/platform.md, "Every fatal
// fault prints one line"), and a host that registered for it gets its turn after (crash.h, T216).
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
//
// **The host's turn** (T216) keeps to the same rule on the faulting thread: the crash file is
// built in a static buffer and written with `CreateFileW`/`open`, and the hook runs on a thread
// made when it was registered, woken by an event (`SetEvent`) or a semaphore (`sem_post`, which a
// signal handler may call), while the faulting thread waits on a clock for it to finish or for its
// deadline. The hook itself may do what it likes; nothing here depends on it returning.
#include "fault_report.h"

#include <core/base/assert_failure.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/platform/crash.h>

#include <atomic>
#include <exception>
#include <string_view>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <csignal>
#include <stdlib.h>
#include <windows.h>
#elif ENGINE_PLATFORM_LINUX
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// A sanitizer's runtime reports faults itself, with more than this can; it keeps them
// (ENGINE_FAULT_UNDER_SANITIZER, crash.h).

namespace engine::platform {

namespace detail {

namespace {

constexpr u32 k_stack_frames = k_crash_stack_frames;

// Text in a buffer that is never allocated; text that would run past it is cut, never overrun.
template <u32 N>
struct Text {
  char text[N];
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
  // Ends a piece with a NUL and returns where it began: `CrashInfo`'s strings are pieces of one.
  const char* end_piece(u32 begin) noexcept {
    if (begin >= sizeof(text)) return "";
    if (size >= sizeof(text)) size = sizeof(text) - 1;
    text[size++] = '\0';
    return text + begin;
  }
  // A JSON string's body: quotes, backslashes and control characters escaped.
  void json(const char* s) noexcept {
    for (; *s != '\0'; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        put('\\');
        put(static_cast<char>(c));
      } else if (c < 0x20) {
        put("\\u00");
        put("0123456789abcdef"[c >> 4]);
        put("0123456789abcdef"[c & 15u]);
      } else {
        put(static_cast<char>(c));
      }
    }
  }
};

using Line = Text<1536>;
Line g_line;
Text<3072> g_full;    // everything the line wrote, for the crash file and the hook
Text<4096> g_pieces;  // the CrashInfo's strings, NUL-separated
Text<8192> g_file_text;
CrashInfo g_info;
u64 g_pcs[k_stack_frames];
u32 g_pc_count = 0;
std::atomic<bool> g_installed{false};
// abort() came from std::terminate (`on_terminate`), so the line names it.
std::atomic<bool> g_terminating{false};

// ---- the host's turn (crash.h) ----
std::atomic<CrashHook> g_hook{nullptr};
std::atomic<void*> g_hook_context{nullptr};
std::atomic<u32> g_deadline_ms{0};
std::atomic<bool> g_hook_thread_made{false};
std::atomic<u64> g_hook_thread_id{0};
// 0 nothing yet, 1 the hook is running, 2 it returned.
std::atomic<u32> g_hook_state{0};
char g_crash_path[1024];
std::atomic<bool> g_have_crash_path{false};
Text<256> g_second;  // the one line a second fault inside the hook writes

bool hook_armed() noexcept {
  return g_hook.load(std::memory_order_acquire) != nullptr &&
         g_hook_thread_made.load(std::memory_order_acquire);
}

const char* piece_of(const char* s) noexcept {
  const u32 begin = g_pieces.size;
  g_pieces.put(s);
  return g_pieces.end_piece(begin);
}

// The crash file's body (format `engine.crash`, version 1): the CrashInfo as JSON.
void build_crash_file(u64 exit_code) noexcept {
  auto& f = g_file_text;
  f.size = 0;
  const CrashInfo& i = g_info;
  f.put("{\"format\":\"engine.crash\",\"version\":1,\"what\":\"");
  f.json(i.what);
  f.put("\",\"code\":\"");
  f.hex(i.code);
  f.put("\",\"address\":\"");
  f.hex(i.address);
  f.put("\",\"access\":\"");
  f.json(i.access);
  f.put("\",\"at\":\"");
  f.json(i.at);
  f.put("\",\"stack\":[");
  for (u32 s = 0; s < i.stack_count; ++s) {
    if (s > 0) f.put(',');
    f.put('"');
    f.json(i.stack[s]);
    f.put('"');
  }
  f.put("],\"thread\":");
  f.dec(i.thread_id);
  f.put(",\"thread_name\":\"");
  f.json(i.thread_name);
  f.put("\",\"main_thread\":");
  f.put(i.main_thread ? "true" : "false");
  f.put(",\"assertion\":");
  if (i.expression[0] != '\0' || i.file[0] != '\0') {
    f.put("{\"expression\":\"");
    f.json(i.expression);
    f.put("\",\"message\":\"");
    f.json(i.message);
    f.put("\",\"file\":\"");
    f.json(i.file);
    f.put("\",\"line\":");
    f.dec(static_cast<u64>(i.line < 0 ? 0 : i.line));
    f.put('}');
  } else {
    f.put("null");
  }
  f.put(",\"exit_code\":");
  f.dec(exit_code);
  f.put(",\"hook\":");
  f.put(hook_armed() ? "true" : "false");
  f.put(",\"line\":\"");
  f.json(i.line_text);
  f.put("\"}\n");
}

// Fills the info's pieces that every platform shares: the line as written, the assertion.
void finish_info() noexcept {
  // Without the newline a debug runtime's abort message needs before it, and without its own.
  g_info.line_text = g_full.end_piece(g_full.size > 0 && g_full.text[0] == '\n' ? 1 : 0);
  if (g_full.size >= 2 && g_full.text[g_full.size - 2] == '\n') g_full.text[g_full.size - 2] = '\0';
  if (const AssertFailure* a = failed_assertion()) {
    g_info.expression = a->expression;
    g_info.message = a->message;
    g_info.file = a->file;
    g_info.line = a->line;
  }
  g_info.crash_file = g_have_crash_path.load(std::memory_order_acquire) ? g_crash_path : "";
}

// abort()'s name in the line: an assertion that failed or std::terminate, when either is why.
const char* abort_name() noexcept {
  if (failed_assertion() != nullptr) return "assertion failed";
  if (g_terminating.load(std::memory_order_acquire)) return "std::terminate()";
  return "abort()";
}

void put_assertion(Line& line) noexcept {
  const AssertFailure* a = failed_assertion();
  if (a == nullptr) return;
  line.put(": ");
  line.put(a->expression);
  if (a->message[0] != '\0') {
    line.put(" (");
    line.put(a->message);
    line.put(')');
  }
  line.put(" at ");
  line.put(a->file);
  line.put(':');
  line.dec(static_cast<u64>(a->line < 0 ? 0 : a->line));
}

[[noreturn]] void on_terminate() noexcept {
  g_terminating.store(true, std::memory_order_release);
  std::abort();
}

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
wchar_t g_crash_wpath[1024];
CONTEXT g_walk;
HANDLE g_hook_event = nullptr;

void write_text(const char* text, u32 size) noexcept {
  const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
  if (err != nullptr && err != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    (void)::WriteFile(err, text, size, &written, nullptr);
  }
}

void write_line() noexcept {
  write_text(g_line.text, g_line.size);
  for (u32 i = 0; i < g_line.size; ++i)
    g_full.put(g_line.text[i]);
  g_line.size = 0;
}

// `module+0xoffset`, the offset from the image's base (its RVA), which is what a debugger's
// `ln module+0x...` and `llvm-symbolizer --obj=<module> --relative-address 0x...` take with the
// module's PDB beside it. A bare address when no image holds it.
template <class T>
void put_address(T& out, u64 pc) noexcept {
  void* base = nullptr;
  (void)::RtlPcToFileHeader(reinterpret_cast<void*>(pc), &base);
  if (base == nullptr) {
    out.hex(pc);
    return;
  }
  const DWORD n = ::GetModuleFileNameW(static_cast<HMODULE>(base), g_path, MAX_PATH);
  DWORD start = 0;
  for (DWORD i = 0; i < n; ++i)
    if (g_path[i] == L'\\' || g_path[i] == L'/') start = i + 1;
  if (start >= n) out.put('?');
  for (DWORD i = start; i < n; ++i)
    out.put(g_path[i] >= 32 && g_path[i] < 127 ? static_cast<char>(g_path[i]) : '?');
  out.put('+');
  out.hex(pc - reinterpret_cast<u64>(base));
}

const char* address_piece(u64 pc) noexcept {
  const u32 begin = g_pieces.size;
  put_address(g_pieces, pc);
  return g_pieces.end_piece(begin);
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
  g_info.thread_id = self;
  g_info.thread_name = piece_of(name);
  g_info.main_thread = self == g_main_thread;
}

void put_exit(DWORD code) noexcept {
  g_line.put("; exit code ");
  if (hook_armed()) {
    g_line.dec(static_cast<u64>(k_exit_crashed));
    g_line.put(" after the host's turn");
    return;
  }
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
  g_pc_count = 0;
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
      put_address(g_line, c.Rip);
      g_pcs[g_pc_count++] = c.Rip;
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

// A fault on the hook's own thread while it runs: one short line, and the process ends now with
// the crashed code — the first line is already out.
[[noreturn]] void second_fault_in_hook(DWORD code) noexcept {
  g_second.size = 0;
  g_second.put("engine: fatal: a second fault (");
  g_second.hex8(code);
  g_second.put(") inside the host's crash hook; exit code ");
  g_second.dec(static_cast<u64>(k_exit_crashed));
  g_second.put('\n');
  write_text(g_second.text, g_second.size);
  ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(k_exit_crashed));
  for (;;)
    ::Sleep(INFINITE);
}

// Claims the line for this thread. A second thread that faults meanwhile waits for the first to
// end the process; a fault inside the handler itself ends it at once with the first code, and one
// inside the host's hook with the crashed code.
void claim(DWORD code) noexcept {
  const DWORD self = ::GetCurrentThreadId();
  if (self == g_hook_thread_id.load(std::memory_order_acquire) &&
      g_hook_state.load(std::memory_order_acquire) == 1)
    second_fault_in_hook(code);
  DWORD expected = 0;
  if (!g_owner.compare_exchange_strong(expected, self)) {
    if (expected == self) ::TerminateProcess(::GetCurrentProcess(), g_code);
    for (;;)
      ::Sleep(INFINITE);
  }
  g_code = hook_armed() ? static_cast<DWORD>(k_exit_crashed) : code;
}

void write_crash_file(u64 exit_code) noexcept {
  if (!g_have_crash_path.load(std::memory_order_acquire)) return;
  build_crash_file(exit_code);
  const HANDLE file = ::CreateFileW(g_crash_wpath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  (void)::WriteFile(file, g_file_text.text, g_file_text.size, &written, nullptr);
  (void)::CloseHandle(file);
}

// The host's turn, bounded by its deadline; returns when it is over either way.
void run_hook() noexcept {
  if (!hook_armed() || g_hook_event == nullptr) return;
  g_hook_state.store(1, std::memory_order_release);
  (void)::SetEvent(g_hook_event);
  const ULONGLONG start = ::GetTickCount64();
  const ULONGLONG deadline = g_deadline_ms.load(std::memory_order_acquire);
  while (g_hook_state.load(std::memory_order_acquire) != 2 && ::GetTickCount64() - start < deadline)
    ::Sleep(5);
}

[[noreturn]] void finish(DWORD code) noexcept {
  g_line.put('\n');
  write_line();
  for (u32 s = 0; s < g_pc_count; ++s)
    g_info.stack[s] = address_piece(g_pcs[s]);
  g_info.stack_count = g_pc_count;
  finish_info();
  const DWORD exit_code = hook_armed() ? static_cast<DWORD>(k_exit_crashed) : code;
  write_crash_file(exit_code);
  run_hook();
  // TerminateProcess, not ExitProcess: nothing that runs at exit — atexit handlers, DLL detach,
  // static destructors — gets a turn in a process whose state is broken.
  ::TerminateProcess(::GetCurrentProcess(), exit_code);
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
  g_info.code = code;
  if (const char* name = exception_name(code)) {
    g_line.put(name);
    g_info.what = name;
  } else {
    const u32 begin = g_pieces.size;
    g_pieces.put("exception ");
    g_pieces.hex8(code);
    g_info.what = g_pieces.end_piece(begin);
    g_line.put(g_info.what);
  }
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    g_info.access = kind == 1 ? "writing" : (kind == 8 ? "executing" : "reading");
    g_info.address = record.ExceptionInformation[1];
    g_line.put(' ');
    g_line.put(g_info.access);
    g_line.put(' ');
    g_line.hex(record.ExceptionInformation[1]);
  }
  g_line.put(" at ");
  put_address(g_line, reinterpret_cast<u64>(record.ExceptionAddress));
  g_info.at = address_piece(reinterpret_cast<u64>(record.ExceptionAddress));
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
                                               const wchar_t* detail, DWORD exit_code,
                                               bool assertion) noexcept {
  claim(exit_code);
  g_line.put(lead);
  g_line.put("engine: fatal: ");
  g_line.put(what);
  g_info.what = what;
  g_info.code = exit_code;
  if (assertion) put_assertion(g_line);
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

void __cdecl on_abort(int) {
  report_call(k_after_abort_message, abort_name(), nullptr, k_abort_exit,
              failed_assertion() != nullptr);
}

void __cdecl on_pure_call() {
  report_call("", "pure virtual function call", nullptr, k_abort_exit, false);
}

// The debug runtime names the function the bad argument went to (and has already reported the
// expression through its assertion report, a line of its own on stderr here); the release runtime
// passes nothing at all.
void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t* function, const wchar_t*,
                                  unsigned int, uintptr_t) {
  report_call("", "invalid parameter passed to the C runtime", function, k_invalid_parameter_exit,
              false);
}

DWORD WINAPI hook_thread(void*) {
  g_hook_thread_id.store(::GetCurrentThreadId(), std::memory_order_release);
  for (;;) {
    (void)::WaitForSingleObject(g_hook_event, INFINITE);
    if (g_hook_state.load(std::memory_order_acquire) != 1) continue;
    if (const CrashHook hook = g_hook.load(std::memory_order_acquire))
      hook(g_hook_context.load(std::memory_order_acquire), g_info);
    g_hook_state.store(2, std::memory_order_release);
    for (;;)
      ::Sleep(INFINITE);  // the faulting thread ends the process
  }
}

bool make_hook_thread() noexcept {
  if (g_hook_thread_made.load(std::memory_order_acquire)) return true;
  g_hook_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (g_hook_event == nullptr) return false;
  // A megabyte, as the main thread has: a host's hook writes JSON and files.
  const HANDLE thread = ::CreateThread(nullptr, 1u << 20, hook_thread, nullptr, 0, nullptr);
  if (thread == nullptr) return false;
  (void)::SetThreadDescription(thread, L"crash-hook");
  (void)::CloseHandle(thread);
  while (g_hook_thread_id.load(std::memory_order_acquire) == 0)
    ::Sleep(0);
  g_hook_thread_made.store(true, std::memory_order_release);
  return true;
}

bool set_crash_path(std::string_view path) noexcept {
  if (path.size() >= sizeof(g_crash_path)) return false;
  g_have_crash_path.store(false, std::memory_order_release);
  if (path.empty()) return true;
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                      g_crash_wpath, static_cast<int>(1023));
  if (n <= 0) return false;
  g_crash_wpath[n] = L'\0';
  for (usize i = 0; i < path.size(); ++i)
    g_crash_path[i] = path[i];
  g_crash_path[path.size()] = '\0';
  g_have_crash_path.store(true, std::memory_order_release);
  return true;
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
  (void)std::set_terminate(on_terminate);
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
sem_t g_hook_go;

pid_t thread_id() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); }

void write_text(const char* text, u32 size) noexcept {
  u32 done = 0;
  while (done < size) {
    const ssize_t n = ::write(STDERR_FILENO, text + done, size - done);
    if (n <= 0) break;
    done += static_cast<u32>(n);
  }
}

void write_line() noexcept {
  write_text(g_line.text, g_line.size);
  for (u32 i = 0; i < g_line.size; ++i)
    g_full.put(g_line.text[i]);
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
template <class T>
void put_address(T& out, u64 pc) noexcept {
  u64 base = 0;
  if (!find_image(pc, base)) {
    out.hex(pc);
    return;
  }
  const char* name = g_path;
  for (const char* p = g_path; *p != '\0'; ++p)
    if (*p == '/') name = p + 1;
  out.put(name);
  out.put('+');
  out.hex(pc - base);
}

const char* address_piece(u64 pc) noexcept {
  const u32 begin = g_pieces.size;
  put_address(g_pieces, pc);
  return g_pieces.end_piece(begin);
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
  if (code <= 0) return sig == SIGABRT ? abort_name() : "sent by a process";
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

[[noreturn]] void second_fault_in_hook(int sig) noexcept {
  g_second.size = 0;
  g_second.put("engine: fatal: a second fault (signal ");
  g_second.dec(static_cast<u64>(sig));
  g_second.put(") inside the host's crash hook; exit code ");
  g_second.dec(static_cast<u64>(k_exit_crashed));
  g_second.put('\n');
  write_text(g_second.text, g_second.size);
  ::_exit(k_exit_crashed);
}

void write_crash_file(u64 exit_code) noexcept {
  if (!g_have_crash_path.load(std::memory_order_acquire)) return;
  build_crash_file(exit_code);
  const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  u32 done = 0;
  while (done < g_file_text.size) {
    const ssize_t n = ::write(fd, g_file_text.text + done, g_file_text.size - done);
    if (n <= 0) break;
    done += static_cast<u32>(n);
  }
  ::close(fd);
}

i64 monotonic_ms() noexcept {
  timespec t{};
  (void)::clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<i64>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

void run_hook() noexcept {
  if (!hook_armed()) return;
  g_hook_state.store(1, std::memory_order_release);
  (void)::sem_post(&g_hook_go);
  const i64 start = monotonic_ms();
  const i64 deadline = static_cast<i64>(g_deadline_ms.load(std::memory_order_acquire));
  while (g_hook_state.load(std::memory_order_acquire) != 2 && monotonic_ms() - start < deadline) {
    timespec nap{0, 5'000'000};
    (void)::nanosleep(&nap, nullptr);
  }
}

void on_signal(int sig, siginfo_t* info, void* context) {
  const pid_t self = thread_id();
  if (static_cast<u64>(self) == g_hook_thread_id.load(std::memory_order_acquire) &&
      g_hook_state.load(std::memory_order_acquire) == 1)
    second_fault_in_hook(sig);
  pid_t expected = 0;
  if (!g_owner.compare_exchange_strong(expected, self)) {
    if (expected == self) {  // a fault inside the handler: the default action, now
      if (hook_armed()) ::_exit(k_exit_crashed);
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
  const char* what = overflow ? "stack overflow" : code_name(sig, info->si_code);
  g_line.put(what);
  {
    const u32 begin = g_pieces.size;
    g_pieces.put(signal_name(sig));
    g_pieces.put(", ");
    g_pieces.put(what);
    g_info.what = g_pieces.end_piece(begin);
  }
  g_info.code = static_cast<u64>(sig);
  if (sig == SIGABRT && !from_hardware && failed_assertion() != nullptr) put_assertion(g_line);
  if ((sig == SIGSEGV || sig == SIGBUS) && from_hardware) {
    // The page fault's error code (x86-64): bit 1 a write, bit 4 an instruction fetch.
    g_info.access = (error & 16u) != 0 ? "executing" : ((error & 2u) != 0 ? "writing" : "reading");
    g_info.address = address;
    g_line.put(' ');
    g_line.put(g_info.access);
    g_line.put(' ');
    g_line.hex(address);
  }
  if (pc != 0) {
    g_line.put(" at ");
    put_address(g_line, pc);
    if (from_hardware) g_info.at = address_piece(pc);
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
  g_info.thread_id = static_cast<u64>(self);
  g_info.thread_name = piece_of(name);
  g_info.main_thread = self == ::getpid();
  g_line.put("; signal ");
  g_line.dec(static_cast<u64>(sig));
  if (hook_armed()) {
    g_line.put(", exit code ");
    g_line.dec(static_cast<u64>(k_exit_crashed));
    g_line.put(" after the host's turn");
  } else {
    g_line.put(", re-raised");
  }
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
  g_pc_count = 0;
  // A jump into memory that is not code leaves the unwinder nothing to read at the pc, so it stops
  // there; the call that went there left its return address where the stack pointer points.
  if (from >= count && sig == SIGSEGV && from_hardware && (error & 16u) != 0 && sp != 0) {
    const u64 back = *reinterpret_cast<const u64*>(sp);
    g_line.put(' ');
    put_address(g_line, back);
    g_pcs[g_pc_count++] = back;
  }
  for (int i = from; i < count && g_pc_count < k_stack_frames; ++i) {
    g_line.put(' ');
    put_address(g_line, reinterpret_cast<u64>(frames[i]));
    g_pcs[g_pc_count++] = reinterpret_cast<u64>(frames[i]);
  }
  if (g_pc_count == 0) g_line.put(" (none)");
  g_line.put('\n');
  write_line();
  for (u32 s = 0; s < g_pc_count; ++s)
    g_info.stack[s] = address_piece(g_pcs[s]);
  g_info.stack_count = g_pc_count;
  finish_info();
  if (hook_armed()) {
    write_crash_file(static_cast<u64>(k_exit_crashed));
    run_hook();
    ::_exit(k_exit_crashed);
  }
  // A crash file still says what the parent's wait will: the signal (128 + it, as a shell does).
  write_crash_file(128u + static_cast<u64>(sig));
  default_action(sig);
}

void* hook_thread(void*) {
  g_hook_thread_id.store(static_cast<u64>(thread_id()), std::memory_order_release);
  (void)::prctl(PR_SET_NAME, "crash-hook", 0, 0, 0);
  for (;;) {
    while (::sem_wait(&g_hook_go) != 0) {
    }
    if (g_hook_state.load(std::memory_order_acquire) != 1) continue;
    if (const CrashHook hook = g_hook.load(std::memory_order_acquire))
      hook(g_hook_context.load(std::memory_order_acquire), g_info);
    g_hook_state.store(2, std::memory_order_release);
    for (;;)
      ::pause();  // the faulting thread ends the process
  }
  return nullptr;
}

bool make_hook_thread() noexcept {
  if (g_hook_thread_made.load(std::memory_order_acquire)) return true;
  if (::sem_init(&g_hook_go, 0, 0) != 0) return false;
  pthread_attr_t attr;
  if (::pthread_attr_init(&attr) != 0) return false;
  (void)::pthread_attr_setstacksize(&attr, 1u << 20);
  (void)::pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  // Its signal mask is the caller's: a fault signal blocked on it would be delivered with its
  // default action by the kernel, past the handler that ends a second fault with the crashed code.
  pthread_t thread{};
  const int rc = ::pthread_create(&thread, &attr, hook_thread, nullptr);
  (void)::pthread_attr_destroy(&attr);
  if (rc != 0) return false;
  while (g_hook_thread_id.load(std::memory_order_acquire) == 0)
    ::sched_yield();
  g_hook_thread_made.store(true, std::memory_order_release);
  return true;
}

bool set_crash_path(std::string_view path) noexcept {
  if (path.size() >= sizeof(g_crash_path)) return false;
  g_have_crash_path.store(false, std::memory_order_release);
  if (path.empty()) return true;
  for (usize i = 0; i < path.size(); ++i)
    g_crash_path[i] = path[i];
  g_crash_path[path.size()] = '\0';
  g_have_crash_path.store(true, std::memory_order_release);
  return true;
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
  (void)std::set_terminate(on_terminate);
  // glibc's backtrace loads libgcc's unwinder on its first call; that call is made here, so that
  // a handler never loads a library.
  void* warm[2];
  (void)::backtrace(warm, 2);
}

#else

namespace {
bool make_hook_thread() noexcept { return false; }
bool set_crash_path(std::string_view) noexcept { return false; }
}  // namespace

void install_fault_report() noexcept { g_installed.store(true); }

#endif

}  // namespace detail

bool set_crash_hook(CrashHook hook, void* context, u32 deadline_ms) noexcept {
  if (ENGINE_FAULT_UNDER_SANITIZER || detail::error_dialogs_wanted()) return false;
  if (hook == nullptr) {
    detail::g_hook.store(nullptr, std::memory_order_release);
    return true;
  }
  if (!detail::make_hook_thread()) return false;
  detail::g_hook_context.store(context, std::memory_order_release);
  detail::g_deadline_ms.store(deadline_ms, std::memory_order_release);
  detail::g_hook.store(hook, std::memory_order_release);
  return true;
}

bool set_crash_file(std::string_view path) noexcept {
  if (ENGINE_FAULT_UNDER_SANITIZER) return false;
  return detail::set_crash_path(path);
}

}  // namespace engine::platform
