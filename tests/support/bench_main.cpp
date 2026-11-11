// Shared entry point linked into every engine_<module>_bench executable. The CPU baseline check
// of ADR-0031 runs first, like it does in every app's main and in the test main.
#include <core/platform/cpu_baseline.h>
#include <foundation/bench/bench.h>

#include <test_environment.h>

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  // A bench is a test (AGENTS.md, "Test hygiene"): it reads no machine's material pack either
  // (test_main.cpp, `read_no_default_materials`; docs/subsystems/materials.md, "Where the hosts
  // find it"). Held for the whole run.
  [[maybe_unused]] const engine::test::ScopedEnv no_default_materials(
      engine::test::k_default_materials_env, "0");
  return engine::bench::run_main(argc, argv);
}
