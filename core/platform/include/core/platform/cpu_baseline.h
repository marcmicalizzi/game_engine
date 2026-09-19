#pragma once

// The instruction-set floor this binary was compiled for, and the check that turns "started on
// a CPU older than the build" from an illegal-instruction crash into one sentence
// (docs/adr/0031-minimum-cpu-x86-64-v3.md).
//
// `ENGINE_CPU_BASELINE_V3` is defined by `cmake/EngineCpuBaseline.cmake`: 1 for the x86-64-v3
// build every preset but the two `*-v2` ones makes, 0 for x86-64-v2.
//
// Everything here but `require_cpu_baseline()` is pure and takes the feature set it judges, so
// the failure path is a unit test rather than a machine nobody has to hand. The whole module
// (`core/platform`) is compiled at the floor baseline — see its `CMakeLists.txt` — because it
// is the code that decides whether the rest of the binary may run.

#include <core/base/types.h>
#include <core/platform/cpu_features.h>

namespace engine::platform {

enum class CpuBaseline : u8 {
  V2,  // x86-64-v2: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT — Nehalem/Westmere and newer
  V3   // x86-64-v3: v2 plus AVX, AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE — Haswell/Zen
};
// Note that V2 says nothing about AVX: Sandy Bridge has AVX and none of the rest of v3, and it
// is one of the two machines the v2 presets exist for, so a message that named AVX as missing
// there would send its reader after the wrong flag. The features are checked one at a time
// rather than in tiers for exactly that reason.

#if defined(ENGINE_CPU_BASELINE_V3) && ENGINE_CPU_BASELINE_V3
inline constexpr CpuBaseline k_build_cpu_baseline = CpuBaseline::V3;
#else
inline constexpr CpuBaseline k_build_cpu_baseline = CpuBaseline::V2;
#endif

// The exit code a binary that cannot run on this CPU leaves with. Outside the 0..3 every app
// here already uses for ok, error, usage, and "no display", so a launcher or a CI script can
// tell "this machine is too old for this build" from any of them; 78 is sysexits' EX_CONFIG,
// which is the closest thing to a convention for "binary and machine do not match".
inline constexpr int k_exit_cpu_too_old = 78;

// "x86-64-v2" or "x86-64-v3".
const char* cpu_baseline_name(CpuBaseline baseline) noexcept;

// The features `baseline` requires that `features` does not report, written into `out` as a
// comma-separated, NUL-terminated list (truncated to `capacity`, which must be at least 1).
// Returns how many were missing; 0 means this CPU can run a binary built for `baseline`.
u32 missing_baseline_features(const CpuFeatures& features, CpuBaseline baseline, char* out,
                              usize capacity) noexcept;

// True when `features` can run `baseline`. Otherwise writes one line to stderr naming the
// baseline, the missing features, the CPU, and the way out, and returns false.
bool report_cpu_baseline(const CpuFeatures& features, CpuBaseline baseline) noexcept;

// What every `main()` calls first, and what a pre-main initializer in the same translation unit
// calls ahead of every other dynamic initializer: detect this CPU, judge it against
// `k_build_cpu_baseline`, and leave with `k_exit_cpu_too_old` after one line when it is too old.
void require_cpu_baseline() noexcept;

}  // namespace engine::platform
