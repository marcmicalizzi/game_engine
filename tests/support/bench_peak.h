#pragma once

// The peaks a bench row reports beside its time (roadmap D40): the engine heap's peak since the
// process started (`mem::total_stats`, every engine container and JsonValue; not `std::string`'s
// own buffers) and, on Windows, the process's peak private commit. Both are since the process
// started, so a row that wants its own peak is run alone (`--filter`) on data it did not build in
// the same process: `ENGINE_BENCH_DOC` and `ENGINE_BENCH_SCENE` are how the document and scene rows
// are handed data made by an earlier run.

#include <core/base/types.h>
#include <core/memory/memory.h>

#include <cstdio>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
// minwindef.h's, which core/math's headers use as names when they come after this one.
#undef near
#undef far
#endif

namespace engine::test {

inline u64 process_peak_commit() {
#if ENGINE_PLATFORM_WINDOWS
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return static_cast<u64>(counters.PeakPagefileUsage);
#endif
  return 0;
}

// One line on stderr: the row, its input's size, and the two peaks.
inline void report_peak(const char* row, u64 input_bytes) {
  const mem::Stats heap = mem::total_stats();
  std::fprintf(stderr,
               "peak: %s input %llu B, engine heap peak %llu B, process peak commit %llu B\n", row,
               static_cast<unsigned long long>(input_bytes),
               static_cast<unsigned long long>(heap.bytes_peak),
               static_cast<unsigned long long>(process_peak_commit()));
}

}  // namespace engine::test
