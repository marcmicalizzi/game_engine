// Shared entry point linked into every engine_<module>_bench executable. The CPU baseline check
// of ADR-0031 runs first, like it does in every app's main and in the test main.
#include <core/platform/cpu_baseline.h>
#include <foundation/bench/bench.h>

int main(int argc, char** argv) {
  engine::platform::require_cpu_baseline();
  return engine::bench::run_main(argc, argv);
}
