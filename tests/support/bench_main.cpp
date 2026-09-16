// Shared entry point linked into every engine_<module>_bench executable.
#include <foundation/bench/bench.h>

int main(int argc, char** argv) { return engine::bench::run_main(argc, argv); }
