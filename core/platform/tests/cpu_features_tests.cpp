#include <core/platform/cpu_features.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::platform;

TEST_CASE("cpu_features: detection is sane on x86-64") {
  const CpuFeatures& f = cpu_features();
  MESSAGE("vendor=" << f.vendor << " brand=" << f.brand << " simd=" << f.max_simd_bits()
                    << (f.hybrid ? " hybrid" : ""));
  CHECK(std::strlen(f.vendor) == 12);
  CHECK(std::strlen(f.brand) > 0);
  // Every x86-64 CPU this engine targets has at least SSE4.2 and POPCNT.
  CHECK(f.sse4_2);
  CHECK(f.popcnt);
  // Feature implications.
  if (f.avx2) CHECK(f.avx);
  if (f.avx512f) CHECK(f.avx2);
  if (f.avx512bw) CHECK(f.avx512f);
  if (f.fma) CHECK(f.avx);
  CHECK((f.max_simd_bits() == 128 || f.max_simd_bits() == 256 || f.max_simd_bits() == 512));
  CHECK(detect_cpu_features().avx2 == f.avx2);
}
