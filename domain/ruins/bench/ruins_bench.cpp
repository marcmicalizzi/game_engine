// The ruin assembler's cost (docs/subsystems/ruins.md, "Performance notes"; plan 11 §11.8): one
// building at a time on one thread, which is what materializing one tile of the endless desert
// costs, and a thousand buildings on one thread and on the performance pool, which is what a
// scene's `ruins` scatter costs to read — in sections (`ruins.assemble.*`) and laid block by
// block (`ruins.blocks.*`, which includes the assembly of the same building). The kits are the
// synthetic kit of boxes and the synthetic block kit, in memory; the ground is flat, since the
// query is the caller's and its cost is the terrain's.
#include <core/jobs/job_system.h>
#include <domain/ruins/ruins.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/bench/bench.h>

#include <string>

using namespace engine;
using namespace engine::ruins;

namespace {

const Kit& bench_kit() {
  static const Kit kit = [] {
    SyntheticKit synthetic;
    make_synthetic_kit(SyntheticKitOptions{}, synthetic);
    Kit k;
    std::string error;
    (void)kit_from_schema(synthetic.kit, "", k, error);
    return k;
  }();
  return kit;
}

const Vector<TileCoord>& bench_tiles() {
  static const Vector<TileCoord> tiles = [] {
    Vector<TileCoord> t;
    choose_tiles(2026, TileCoord{-50, -50}, TileCoord{49, 49}, 1000, 0.0f, t);
    return t;
  }();
  return tiles;
}

}  // namespace

// One building, the output cleared between them so its arrays stop growing: the steady state of
// materializing tiles one after another.
ENGINE_BENCH(ruins_building, "ruins.assemble.building") {
  const Kit& kit = bench_kit();
  const Vector<TileCoord>& tiles = bench_tiles();
  Assembler assembler(kit);
  Output out;
  Placement placement;
  placement.world_seed = 2026;
  u32 next = 0;
  u64 instances = 0;
  std::string error;
  while (state.keep_running()) {
    out.clear();
    (void)assembler.assemble(placement, tiles[next], out, &error);
    next = next + 1 == tiles.size() ? 0 : next + 1;
    instances += out.instances.size();
    bench::keep(out.instances.data());
  }
  bench::keep(instances);
  state.set_items(1);
}

const BlockKit& bench_blocks() {
  static const BlockKit kit = [] {
    SyntheticBlockKit synthetic;
    make_synthetic_block_kit(SyntheticBlockOptions{}, synthetic);
    BlockKit k;
    std::string error;
    (void)block_kit_from_schema(synthetic.kit, "", k, error);
    return k;
  }();
  return kit;
}

// One building laid block by block (blocks.h): the assembler's building, then its courses, its
// ruin per block and its fallen blocks. What materializing one tile costs in that representation.
ENGINE_BENCH(ruins_blocks_building, "ruins.blocks.building") {
  const Kit& kit = bench_kit();
  const BlockKit& blocks = bench_blocks();
  const Vector<TileCoord>& tiles = bench_tiles();
  BlockAssembler layer(kit, blocks);
  BlockOutput out;
  Placement placement;
  placement.world_seed = 2026;
  u32 next = 0;
  u64 laid = 0;
  std::string error;
  while (state.keep_running()) {
    out.clear();
    (void)layer.assemble(placement, tiles[next], out, &error);
    next = next + 1 == tiles.size() ? 0 : next + 1;
    laid += out.blocks.size();
    bench::keep(out.blocks.data());
  }
  bench::keep(laid);
  state.set_items(1);
}

// A thousand buildings laid block by block into one output, on one thread (arg 0) or a pool.
ENGINE_BENCH_ARGS(ruins_blocks_thousand, "ruins.blocks.1000", 0, 4, 8) {
  const Kit& kit = bench_kit();
  const BlockKit& blocks = bench_blocks();
  const Vector<TileCoord>& tiles = bench_tiles();
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystem* pool = nullptr;
  jobs::JobSystem js(jobs::JobSystemConfig{.performance_workers = workers > 0 ? workers : 1,
                                           .efficiency_workers = 1});
  if (workers > 0) pool = &js;
  Placement placement;
  placement.world_seed = 2026;
  BlockOutput out;
  std::string error;
  while (state.keep_running()) {
    out.clear();
    (void)assemble_block_tiles(kit, blocks, placement,
                               std::span<const TileCoord>(tiles.data(), tiles.size()), pool, out,
                               &error);
    bench::keep(out.blocks.data());
  }
  state.set_items(tiles.size());
}

// A thousand buildings into one output, on one thread (arg 0) or on a pool of that many workers.
ENGINE_BENCH_ARGS(ruins_thousand, "ruins.assemble.1000", 0, 4, 8) {
  const Kit& kit = bench_kit();
  const Vector<TileCoord>& tiles = bench_tiles();
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystem* pool = nullptr;
  jobs::JobSystem js(jobs::JobSystemConfig{.performance_workers = workers > 0 ? workers : 1,
                                           .efficiency_workers = 1});
  if (workers > 0) pool = &js;
  Placement placement;
  placement.world_seed = 2026;
  Output out;
  std::string error;
  while (state.keep_running()) {
    out.clear();
    (void)assemble_tiles(kit, placement, std::span<const TileCoord>(tiles.data(), tiles.size()),
                         pool, out, &error);
    bench::keep(out.instances.data());
  }
  state.set_items(tiles.size());
}
