// Shared doctest entry point linked into every engine_<module>_tests executable.
//
// A main() of our own rather than DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, for one reason: the CPU
// baseline check of ADR-0031 has to be the first thing a binary of this tree does, and a test
// binary is a binary of this tree — the one that would otherwise be the first to meet a machine
// too old for the build, since `ctest` is what a new machine runs first.
// engine-lint: allow-exceptions test harness
#define DOCTEST_CONFIG_IMPLEMENT
#include <core/platform/cpu_baseline.h>

#include <doctest/doctest.h>

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  return doctest::Context(argc, argv).run();
}
