// The matgen capability's cost (docs/subsystems/matgen.md, "Budgets"): one set built, by kind and
// by texels a side. It is paid once per recipe, by `engine-content matgen`, never in a frame.
#include <domain/matgen/matgen.h>
#include <foundation/bench/bench.h>

#include <string>

using namespace engine;

namespace {

void build(bench::State& state, const char* role) {
  matgen::Set set;
  matgen::default_set(role, set);
  // The CTest smoke run builds each once at 256: does it run at all, in seconds.
  set.texels = bench::smoke_mode() ? 256u : static_cast<u32>(state.arg());
  matgen::SetImages images;
  std::string error;
  while (state.keep_running()) {
    matgen::build_set(set, images, nullptr, error);
    bench::keep(images.base_color.size());
  }
  state.set_items(static_cast<u64>(set.texels) * set.texels);
}

}  // namespace

// arg: texels a side.
ENGINE_BENCH_ARGS(matgen_gravel, "matgen.gravel_bed", 256, 1024, 2048) {
  build(state, "gravel/bed");
}
ENGINE_BENCH_ARGS(matgen_scree, "matgen.rock_scree", 256, 1024, 2048) {
  build(state, "rock/scree");
}
ENGINE_BENCH_ARGS(matgen_granite, "matgen.rock_granite", 256, 1024, 2048) {
  build(state, "rock/granite");
}
ENGINE_BENCH_ARGS(matgen_turf, "matgen.ground_turf", 256, 1024, 2048) {
  build(state, "ground/turf");
}
