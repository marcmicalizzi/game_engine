#pragma once

// CPUID-derived instruction-set features, with OS support (XCR0) taken into account so that
// `avx2 == true` means the instructions can actually be executed. Used by ISA dispatch.

#include <core/base/types.h>

namespace engine::platform {

struct CpuFeatures {
  char vendor[13] = {};
  char brand[49] = {};

  bool sse3 = false;
  bool ssse3 = false;
  bool sse4_1 = false;
  bool sse4_2 = false;
  bool popcnt = false;
  bool fma = false;
  bool f16c = false;
  bool bmi1 = false;
  bool bmi2 = false;
  // LZCNT (AMD's ABM leaf) and MOVBE are the two members of x86-64-v3 that are neither SSE nor
  // AVX, and the baseline check of `cpu_baseline.h` has to name them, so they are detected here
  // rather than assumed from `avx2`. They are also the two a virtual machine is most likely to
  // mask off while leaving AVX2 alone.
  bool lzcnt = false;
  bool movbe = false;
  bool avx = false;
  bool avx2 = false;
  bool avx512f = false;
  bool avx512dq = false;
  bool avx512bw = false;
  bool avx512vl = false;
  bool avx512_vnni = false;
  bool avx512_bf16 = false;
  bool avx512_fp16 = false;
  bool avx_vnni = false;
  bool hybrid = false;  // CPUID reports a hybrid (P/E core) part

  // The widest SIMD register width the CPU and OS support, in bits: 128, 256, or 512.
  u16 max_simd_bits() const noexcept { return avx512f ? 512 : (avx2 ? 256 : 128); }
};

// Detected once on first use and cached.
const CpuFeatures& cpu_features();
CpuFeatures detect_cpu_features();

}  // namespace engine::platform
