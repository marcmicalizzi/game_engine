#include "dynamic_library.h"

#include <core/base/macros.h>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <string>

namespace engine::hotload::detail {

#if ENGINE_PLATFORM_WINDOWS

namespace {

std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<usize>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

std::string last_error_text() {
  const DWORD code = GetLastError();
  char* buffer = nullptr;
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string out = "error " + std::to_string(code);
  if (n > 0 && buffer != nullptr) {
    std::string text(buffer, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    out += ": " + text;
  }
  if (buffer != nullptr) LocalFree(buffer);
  return out;
}

}  // namespace

void* open_library(const std::string& path, std::string& error) {
  // No dialog for a library that is not an image, or one whose dependency is missing: a load is
  // a status, never a window waiting for a click (AGENTS.md, "No engine binary opens a dialog").
  DWORD previous = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
  HMODULE module = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) error = last_error_text();
  SetThreadErrorMode(previous, nullptr);
  return reinterpret_cast<void*>(module);
}

void* find_symbol(void* library, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

void close_library(void* library) {
  if (library != nullptr) FreeLibrary(static_cast<HMODULE>(library));
}

u64 process_id() { return static_cast<u64>(GetCurrentProcessId()); }

#else

void* open_library(const std::string& path, std::string& error) {
  // RTLD_NOW: an unresolved symbol is a load failure now, not a crash at the first call.
  // RTLD_LOCAL: the module's symbols never satisfy another library's, nor another build's.
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* text = dlerror();
    error = text != nullptr ? text : "dlopen failed";
  }
  return handle;
}

void* find_symbol(void* library, const char* name) {
  dlerror();
  return dlsym(library, name);
}

void close_library(void* library) {
  if (library != nullptr) dlclose(library);
}

u64 process_id() { return static_cast<u64>(getpid()); }

#endif

}  // namespace engine::hotload::detail
