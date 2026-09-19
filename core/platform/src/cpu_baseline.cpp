// The startup check of ADR-0031. Two things make it safe, and both are properties of the
// build rather than of this file's text:
//
//   1. The whole of `core/platform` is compiled at the floor baseline — `EngineCpuBaseline`'s
//      `engine_strip_cpu_baseline()` removes the arch flag from this target's COMPILE_OPTIONS,
//      so nothing here and nothing in `cpu_features.cpp` can be a VEX-encoded instruction.
//      `build/<preset>/compile_commands.json` is where that is checked.
//   2. It runs before anything else can. Every app's `main()`, the shared test main, and the
//      shared bench main call `require_cpu_baseline()` as their first statement, and the
//      initializer below runs it ahead of every *dynamic initializer* in the binary as well —
//      `.CRT$XCT` on MSVC sorts before the `.CRT$XCU` the compiler emits C++ initializers
//      into, and a GCC/Clang constructor at priority 101 sorts before the default priority
//      ordinary initializers get. The explicit call is what guarantees this object file is
//      pulled out of the static library at all; the initializer is what covers a static
//      `Foo g_foo = expensive();` in a translation unit that *was* compiled with AVX2.
//
// Nothing here calls into another engine module: a non-inline function in `core/containers`
// or `core/memory` is compiled with the arch flag, so calling one would be the very thing the
// check exists to prevent. `<cstdio>` and `<cstdlib>` only.
#include <core/base/macros.h>
#include <core/platform/cpu_baseline.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::platform {

namespace {

struct Feature {
  const char* name;
  bool CpuFeatures::* field;
};

// x86-64-v2 is SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT (and CMPXCHG16B and LAHF-SAHF, which
// every 64-bit part this century has and `CpuFeatures` does not carry a bit for). x86-64-v3
// adds AVX, AVX2, FMA, BMI1, BMI2, F16C, LZCNT and MOVBE. The order is the order the names
// appear in the message, so the list reads from oldest to newest.
constexpr Feature k_v2_features[] = {
    {"SSE3", &CpuFeatures::sse3},     {"SSSE3", &CpuFeatures::ssse3},
    {"SSE4.1", &CpuFeatures::sse4_1}, {"SSE4.2", &CpuFeatures::sse4_2},
    {"POPCNT", &CpuFeatures::popcnt},
};

constexpr Feature k_v3_features[] = {
    {"AVX", &CpuFeatures::avx},     {"AVX2", &CpuFeatures::avx2},   {"FMA", &CpuFeatures::fma},
    {"BMI1", &CpuFeatures::bmi1},   {"BMI2", &CpuFeatures::bmi2},   {"F16C", &CpuFeatures::f16c},
    {"LZCNT", &CpuFeatures::lzcnt}, {"MOVBE", &CpuFeatures::movbe},
};

// Appends to a fixed buffer, never past it, always NUL-terminated. `*used` is the length
// written so far, not counting the terminator.
void append(char* out, usize capacity, usize* used, const char* text) noexcept {
  while (*text != '\0' && *used + 1 < capacity) {
    out[*used] = *text;
    ++*used;
    ++text;
  }
  out[*used] = '\0';
}

u32 collect(const CpuFeatures& features, const Feature* table, usize count, char* out,
            usize capacity, usize* used) noexcept {
  u32 missing = 0;
  for (usize i = 0; i < count; ++i) {
    if (features.*(table[i].field)) continue;
    if (*used != 0) append(out, capacity, used, ", ");
    append(out, capacity, used, table[i].name);
    ++missing;
  }
  return missing;
}

}  // namespace

const char* cpu_baseline_name(CpuBaseline baseline) noexcept {
  return baseline == CpuBaseline::V3 ? "x86-64-v3" : "x86-64-v2";
}

u32 missing_baseline_features(const CpuFeatures& features, CpuBaseline baseline, char* out,
                              usize capacity) noexcept {
  if (out == nullptr || capacity == 0) return 0;
  usize used = 0;
  out[0] = '\0';
  u32 missing = collect(features, k_v2_features, sizeof(k_v2_features) / sizeof(Feature), out,
                        capacity, &used);
  if (baseline == CpuBaseline::V3) {
    missing += collect(features, k_v3_features, sizeof(k_v3_features) / sizeof(Feature), out,
                       capacity, &used);
  }
  return missing;
}

bool report_cpu_baseline(const CpuFeatures& features, CpuBaseline baseline) noexcept {
  char missing[256];
  if (missing_baseline_features(features, baseline, missing, sizeof(missing)) == 0) return true;
  // One line, and it has to carry everything the reader needs: what the build needs, what this
  // CPU is short of, which CPU that is, and the way out. A second line would be the one that
  // scrolls away.
  const char* brand = features.brand[0] != '\0' ? features.brand : "unknown CPU";
  std::fprintf(stderr,
               "engine: this build needs %s and this CPU has no %s (%s); rebuild with "
               "-DENGINE_CPU_BASELINE=v2 (preset msvc-release-v2 or linux-gcc-release-v2), "
               "see docs/adr/0031-minimum-cpu-x86-64-v3.md\n",
               cpu_baseline_name(baseline), missing, brand);
  std::fflush(stderr);
  return false;
}

void require_cpu_baseline() noexcept {
  // `detect_cpu_features()` rather than the cached `cpu_features()`: this runs before main, and
  // a function-local static would reach for the compiler's thread-safe-statics machinery at a
  // point in the CRT's start-up where it has no business being asked. Two CPUID sequences cost
  // nothing and the second caller is `main()` a moment later.
  if (report_cpu_baseline(detect_cpu_features(), k_build_cpu_baseline)) return;
  // _Exit rather than exit: this may run before main, so there is nothing constructed that
  // wants destroying and an atexit handler registered by a half-initialized module is the last
  // thing that should get a turn. The message is already flushed.
  std::_Exit(k_exit_cpu_too_old);
}

}  // namespace engine::platform

// --- before every other dynamic initializer ------------------------------------------------
//
// Untested on GCC and Clang: hosted CI has no runner for either as this lands (ADR-0031), and
// the constructor attribute below is written from the documented behaviour of both.

extern "C" void engine_cpu_baseline_preinit(void) { engine::platform::require_cpu_baseline(); }

#if ENGINE_COMPILER_MSVC
#pragma section(".CRT$XCT", long, read)
// C4075: MSVC warns whenever an initializer is placed in a section it does not itself manage,
// which is the whole point here — the section name is chosen so the linker sorts it ahead of
// the .CRT$XCU the compiler emits C++ initializers into.
#pragma warning(push)
#pragma warning(disable : 4075)
extern "C" __declspec(allocate(".CRT$XCT")) void (*engine_cpu_baseline_init)(void) =
    engine_cpu_baseline_preinit;
#pragma warning(pop)
// /OPT:REF would otherwise be free to drop a pointer nothing reads.
#pragma comment(linker, "/include:engine_cpu_baseline_init")
#else
namespace {
__attribute__((constructor(101))) void engine_cpu_baseline_ctor() { engine_cpu_baseline_preinit(); }
}  // namespace
#endif
