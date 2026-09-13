#include <core/platform/cpu_features.h>

#include <core/base/macros.h>

#include <cstring>

#if ENGINE_COMPILER_MSVC
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace engine::platform {

namespace {

struct Regs {
  u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Regs cpuid(u32 leaf, u32 subleaf) noexcept {
  Regs r;
#if ENGINE_COMPILER_MSVC
  int out[4];
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  r.eax = static_cast<u32>(out[0]);
  r.ebx = static_cast<u32>(out[1]);
  r.ecx = static_cast<u32>(out[2]);
  r.edx = static_cast<u32>(out[3]);
#else
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
  return r;
}

u64 read_xcr0() noexcept {
#if ENGINE_COMPILER_MSVC
  return _xgetbv(0);
#else
  u32 eax, edx;
  __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
  return (static_cast<u64>(edx) << 32) | eax;
#endif
}

constexpr bool bit(u32 value, u32 index) noexcept { return ((value >> index) & 1u) != 0; }

}  // namespace

CpuFeatures detect_cpu_features() {
  CpuFeatures f;

  const Regs leaf0 = cpuid(0, 0);
  const u32 max_leaf = leaf0.eax;
  std::memcpy(f.vendor + 0, &leaf0.ebx, 4);
  std::memcpy(f.vendor + 4, &leaf0.edx, 4);
  std::memcpy(f.vendor + 8, &leaf0.ecx, 4);
  f.vendor[12] = '\0';

  if (max_leaf >= 1) {
    const Regs l1 = cpuid(1, 0);
    f.sse3 = bit(l1.ecx, 0);
    f.ssse3 = bit(l1.ecx, 9);
    f.fma = bit(l1.ecx, 12);
    f.sse4_1 = bit(l1.ecx, 19);
    f.sse4_2 = bit(l1.ecx, 20);
    f.popcnt = bit(l1.ecx, 23);
    const bool osxsave = bit(l1.ecx, 27);
    const bool avx_cpu = bit(l1.ecx, 28);
    f.f16c = bit(l1.ecx, 29);

    bool ymm_ok = false;
    bool zmm_ok = false;
    if (osxsave) {
      const u64 xcr0 = read_xcr0();
      ymm_ok = (xcr0 & 0x6) == 0x6;          // XMM and YMM state
      zmm_ok = ymm_ok && (xcr0 & 0xE0) == 0xE0;  // opmask, ZMM_Hi256, Hi16_ZMM
    }
    f.avx = avx_cpu && ymm_ok;
    f.fma = f.fma && f.avx;
    f.f16c = f.f16c && f.avx;

    if (max_leaf >= 7) {
      const Regs l7 = cpuid(7, 0);
      f.bmi1 = bit(l7.ebx, 3);
      f.avx2 = bit(l7.ebx, 5) && ymm_ok;
      f.bmi2 = bit(l7.ebx, 8);
      f.avx512f = bit(l7.ebx, 16) && zmm_ok;
      f.avx512dq = bit(l7.ebx, 17) && f.avx512f;
      f.avx512bw = bit(l7.ebx, 30) && f.avx512f;
      f.avx512vl = bit(l7.ebx, 31) && f.avx512f;
      f.avx512_vnni = bit(l7.ecx, 11) && f.avx512f;
      f.avx512_fp16 = bit(l7.edx, 23) && f.avx512f;
      f.hybrid = bit(l7.edx, 15);
      if (l7.eax >= 1) {
        const Regs l7s1 = cpuid(7, 1);
        f.avx_vnni = bit(l7s1.eax, 4) && f.avx2;
        f.avx512_bf16 = bit(l7s1.eax, 5) && f.avx512f;
      }
    }
  }

  const Regs ext0 = cpuid(0x80000000u, 0);
  if (ext0.eax >= 0x80000004u) {
    char* out = f.brand;
    for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
      const Regs r = cpuid(leaf, 0);
      std::memcpy(out + 0, &r.eax, 4);
      std::memcpy(out + 4, &r.ebx, 4);
      std::memcpy(out + 8, &r.ecx, 4);
      std::memcpy(out + 12, &r.edx, 4);
      out += 16;
    }
    f.brand[48] = '\0';
    // Trim leading spaces some vendors pad with.
    usize start = 0;
    while (f.brand[start] == ' ') ++start;
    if (start > 0) std::memmove(f.brand, f.brand + start, 49 - start);
  }
  return f;
}

const CpuFeatures& cpu_features() {
  static const CpuFeatures f = detect_cpu_features();
  return f;
}

}  // namespace engine::platform
