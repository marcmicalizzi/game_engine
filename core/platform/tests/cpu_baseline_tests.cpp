// The startup check of ADR-0031. The interesting case — a v3 binary started on a v2 machine —
// cannot be reached on any machine this project can run a test on, so the check takes the
// feature set it judges and the test hands it a doctored one. That is the whole reason
// `missing_baseline_features` and `report_cpu_baseline` are pure and `require_cpu_baseline()`
// is four lines on top of them.
#include <core/platform/cpu_baseline.h>
#include <core/platform/cpu_features.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::platform;

namespace {

void set_brand(CpuFeatures& f, const char* text) {
  std::memset(f.brand, 0, sizeof(f.brand));
  std::memcpy(f.brand, text, std::strlen(text));
}

// A Westmere: the i7-980 that was this project's minimum machine until 2026-09-19. SSE4.2 and
// POPCNT, and nothing above them.
CpuFeatures westmere() {
  CpuFeatures f;
  std::memcpy(f.vendor, "GenuineIntel", 13);
  set_brand(f, "Intel(R) Core(TM) i7 CPU X 980 @ 3.33GHz");
  f.sse3 = true;
  f.ssse3 = true;
  f.sse4_1 = true;
  f.sse4_2 = true;
  f.popcnt = true;
  return f;
}

// A Sandy Bridge-EP: the Xeon E5-2670 in the project's Linux GPU runner. It is the interesting
// one, because it *has* AVX and nothing above it — so the message it prints must not say "no
// AVX", which would send its owner looking for the wrong thing.
CpuFeatures sandy_bridge() {
  CpuFeatures f = westmere();
  set_brand(f, "Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz");
  f.avx = true;
  return f;
}

// A Haswell: the new floor, and the first part that has all of x86-64-v3.
CpuFeatures haswell() {
  CpuFeatures f = westmere();
  set_brand(f, "Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz");
  f.avx = true;
  f.avx2 = true;
  f.fma = true;
  f.bmi1 = true;
  f.bmi2 = true;
  f.f16c = true;
  f.lzcnt = true;
  f.movbe = true;
  return f;
}

std::string missing_of(const CpuFeatures& f, CpuBaseline baseline) {
  char buffer[256];
  missing_baseline_features(f, baseline, buffer, sizeof(buffer));
  return std::string(buffer);
}

}  // namespace

TEST_CASE("cpu_baseline: a v3 build names every feature a Westmere is short of") {
  const CpuFeatures f = westmere();
  char buffer[256];
  const u32 missing = missing_baseline_features(f, CpuBaseline::V3, buffer, sizeof(buffer));
  CHECK(missing == 8);
  CHECK(std::string(buffer) == "AVX, AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE");
  // Same CPU, v2 build: nothing missing, and that is the point of the v2 presets existing.
  CHECK(missing_baseline_features(f, CpuBaseline::V2, buffer, sizeof(buffer)) == 0);
  CHECK(std::string(buffer).empty());
}

TEST_CASE("cpu_baseline: a Sandy Bridge is told it has AVX and not the rest") {
  // The Linux GPU runner (Xeon E5-2670, Pascal Titan Xp) is this CPU, and `linux-clang-debug-v2`
  // exists for it. If somebody builds the ordinary preset there, this is the line they get, and
  // the point of it is the word that is *absent*: AVX is not in the list, because the part has
  // it. A message that blamed AVX would send its reader after the wrong flag.
  const CpuFeatures f = sandy_bridge();
  CHECK(missing_of(f, CpuBaseline::V3) == "AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE");
  CHECK_FALSE(report_cpu_baseline(f, CpuBaseline::V3));
  CHECK(missing_of(f, CpuBaseline::V2).empty());
  CHECK(report_cpu_baseline(f, CpuBaseline::V2));
}

TEST_CASE("cpu_baseline: a Haswell satisfies both baselines") {
  const CpuFeatures f = haswell();
  CHECK(missing_of(f, CpuBaseline::V3).empty());
  CHECK(missing_of(f, CpuBaseline::V2).empty());
  CHECK(report_cpu_baseline(f, CpuBaseline::V3));
  CHECK(report_cpu_baseline(f, CpuBaseline::V2));
}

TEST_CASE("cpu_baseline: the message path fires for a CPU that is too old") {
  // report_cpu_baseline writes the one line to stderr and answers false; the test asserts the
  // answer, and running it is what proves the formatting path executes at all rather than
  // being dead code that only a 2010 machine would ever reach.
  CHECK_FALSE(report_cpu_baseline(westmere(), CpuBaseline::V3));
  CHECK(report_cpu_baseline(westmere(), CpuBaseline::V2));

  // A part older than both: a Core 2, which has SSSE3 and stops there.
  CpuFeatures core2;
  std::memcpy(core2.vendor, "GenuineIntel", 13);
  core2.sse3 = true;
  core2.ssse3 = true;
  CHECK(missing_of(core2, CpuBaseline::V2) == "SSE4.1, SSE4.2, POPCNT");
  CHECK_FALSE(report_cpu_baseline(core2, CpuBaseline::V2));
}

TEST_CASE("cpu_baseline: one missing feature is named on its own") {
  CpuFeatures f = haswell();
  f.bmi2 = false;
  CHECK(missing_of(f, CpuBaseline::V3) == "BMI2");
  // A virtual machine that masks LZCNT and MOVBE and leaves AVX2 alone is the realistic way a
  // v3 binary meets a v3-looking CPU it cannot run on.
  CpuFeatures masked = haswell();
  masked.lzcnt = false;
  masked.movbe = false;
  CHECK(missing_of(masked, CpuBaseline::V3) == "LZCNT, MOVBE");
}

TEST_CASE("cpu_baseline: the list is truncated rather than overrun") {
  char small[8];
  std::memset(small, 0x7f, sizeof(small));
  const u32 missing = missing_baseline_features(westmere(), CpuBaseline::V3, small, sizeof(small));
  CHECK(missing == 8);            // counted in full
  CHECK(std::strlen(small) < 8);  // written short
  CHECK(std::string(small) == "AVX, AV");
}

TEST_CASE("cpu_baseline: this binary's own baseline is the one it was configured with") {
  MESSAGE("build baseline=" << cpu_baseline_name(k_build_cpu_baseline));
#if defined(ENGINE_CPU_BASELINE_V3) && ENGINE_CPU_BASELINE_V3
  CHECK(k_build_cpu_baseline == CpuBaseline::V3);
#else
  CHECK(k_build_cpu_baseline == CpuBaseline::V2);
#endif
  // And the machine running the tests satisfies it, or the process would not have reached main
  // — which is itself the check working, since the test main calls require_cpu_baseline first.
  CHECK(report_cpu_baseline(cpu_features(), k_build_cpu_baseline));
  CHECK(cpu_baseline_name(CpuBaseline::V2) == std::string("x86-64-v2"));
  CHECK(cpu_baseline_name(CpuBaseline::V3) == std::string("x86-64-v3"));
}
