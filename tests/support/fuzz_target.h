#pragma once

// One target, run two ways (roadmap T13; docs/experiments/fuzzing-design.md, "2. The harness:
// both, with different jobs"; docs/ci/fuzzing.md, "Adding a target").
//
//   ENGINE_FUZZ_TARGET(parse_json, k_parse_json)
//
// declares the doctest case "fuzz: parse_json" running `fuzz::run_target(k_parse_json)` in the
// module's test executable, and, in a libFuzzer build of the same file (`ENGINE_FUZZ_LIBFUZZER`,
// which `engine_module_fuzz()` sets on the `engine_fuzz_<target>` executable of the
// `linux-clang-fuzz` preset), `LLVMFuzzerTestOneInput` calling the same `TargetSpec::one`. So a
// finding in either is a case in both: the function is the target, and the two drivers only choose
// its inputs.
//
// Under libFuzzer the oracles are libFuzzer's and the sanitizers': a broken contract prints one
// line and aborts, which libFuzzer reports as a crash and keeps the input; an `ENGINE_ASSERT`
// aborts by itself, the preset being Debug; an allocation past `-malloc_limit_mb` and an input past
// `-timeout` are libFuzzer's own findings. The in-tree runner's allocation watch and watchdog are
// not compiled in.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fuzz_harness.h>
#include <span>

#if defined(ENGINE_FUZZ_LIBFUZZER)

namespace engine::test::fuzz {

inline int libfuzzer_one(const TargetSpec& spec, const std::uint8_t* data, std::size_t size) {
  if (size > spec.max_len) return 0;  // the same cut the in-tree mutators make
  const Verdict verdict = spec.one(std::span<const u8>(data, size));
  if (!verdict.ok) {
    std::fprintf(stderr, "\nfuzz finding: %s: %s\n", spec.name, verdict.why.c_str());
    std::fflush(stderr);
    std::abort();
  }
  return 0;
}

}  // namespace engine::test::fuzz

// The parameters are not called `name` and `spec`: a parameter named `name` would replace the
// member in `spec.name` as well.
#define ENGINE_FUZZ_TARGET(target_name, target_spec)                                      \
  extern "C" int LLVMFuzzerTestOneInput(const ::std::uint8_t* data, ::std::size_t size);  \
  extern "C" int LLVMFuzzerTestOneInput(const ::std::uint8_t* data, ::std::size_t size) { \
    return ::engine::test::fuzz::libfuzzer_one(target_spec, data, size);                  \
  }

#else

#include <fuzz_runner.h>

#define ENGINE_FUZZ_TARGET(target_name, target_spec)             \
  TEST_CASE("fuzz: " #target_name) {                             \
    REQUIRE(std::string_view(target_spec.name) == #target_name); \
    ::engine::test::fuzz::run_target(target_spec);               \
  }

#endif
