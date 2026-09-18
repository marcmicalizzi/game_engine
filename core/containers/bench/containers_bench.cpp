// Container micro-benchmarks: the measurements behind the size-class guidance in
// docs/plan/11-performance-principles.md §11.2. Standard containers appear as comparison
// baselines only (bench/ directories are exempt from the container lint).
#include <core/containers/flat_map.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <foundation/bench/bench.h>

#include <algorithm>
#include <map>
#include <random>
#include <unordered_map>

using namespace engine;

namespace {

// Distinct pseudo-random keys in a shuffled lookup order, deterministic per size.
Vector<u32> make_keys(u32 n) {
  Vector<u32> keys;
  keys.reserve(n);
  for (u32 i = 0; i < n; ++i)
    keys.push_back(static_cast<u32>(mix64(i + 1)));
  std::mt19937 rng(12345);
  std::shuffle(keys.begin(), keys.end(), rng);
  return keys;
}

template <class Map>
void find_loop(bench::State& state, const Map& map, const Vector<u32>& keys) {
  while (state.keep_running()) {
    u64 sum = 0;
    for (const u32 k : keys)
      sum += *map.find_value(k);
    bench::keep(sum);
  }
  state.set_items(keys.size());
}

template <class StdMap>
void std_find_loop(bench::State& state, const StdMap& map, const Vector<u32>& keys) {
  while (state.keep_running()) {
    u64 sum = 0;
    for (const u32 k : keys)
      sum += map.find(k)->second;
    bench::keep(sum);
  }
  state.set_items(keys.size());
}

}  // namespace

// ---- find: random keys, all present ------------------------------------------------------------

ENGINE_BENCH_ARGS(flat_map_find, "containers.find.flat_map", 16, 64, 256, 1024, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  FlatMap<u32, u32> map;
  for (const u32 k : keys)
    map.insert(k, k);
  find_loop(state, map, keys);
}

ENGINE_BENCH_ARGS(hash_map_find, "containers.find.hash_map", 16, 64, 256, 1024, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  HashMap<u32, u32> map;
  for (const u32 k : keys)
    map.insert(k, k);
  find_loop(state, map, keys);
}

ENGINE_BENCH_ARGS(std_map_find, "containers.find.std_map", 16, 64, 256, 1024, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  std::map<u32, u32> map;
  for (const u32 k : keys)
    map.emplace(k, k);
  std_find_loop(state, map, keys);
}

ENGINE_BENCH_ARGS(std_unordered_map_find, "containers.find.std_unordered_map", 16, 64, 256, 1024,
                  4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  std::unordered_map<u32, u32> map;
  for (const u32 k : keys)
    map.emplace(k, k);
  std_find_loop(state, map, keys);
}

// ---- insert: build a map of n random keys from empty ------------------------------------------

ENGINE_BENCH_ARGS(flat_map_insert, "containers.insert.flat_map", 16, 64, 256, 1024, 4096) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  while (state.keep_running()) {
    FlatMap<u32, u32> map;
    for (const u32 k : keys)
      map.insert(k, k);
    bench::keep(map.size());
  }
  state.set_items(keys.size());
}

ENGINE_BENCH_ARGS(hash_map_insert, "containers.insert.hash_map", 16, 64, 256, 1024, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  while (state.keep_running()) {
    HashMap<u32, u32> map;
    for (const u32 k : keys)
      map.insert(k, k);
    bench::keep(map.size());
  }
  state.set_items(keys.size());
}

ENGINE_BENCH_ARGS(std_unordered_map_insert, "containers.insert.std_unordered_map", 16, 64, 256,
                  1024, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  while (state.keep_running()) {
    std::unordered_map<u32, u32> map;
    for (const u32 k : keys)
      map.emplace(k, k);
    bench::keep(map.size());
  }
  state.set_items(keys.size());
}

// ---- iterate: sum every value ----------------------------------------------------------------

ENGINE_BENCH_ARGS(flat_map_iterate, "containers.iterate.flat_map", 256, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  FlatMap<u32, u32> map;
  for (const u32 k : keys)
    map.insert(k, k);
  while (state.keep_running()) {
    u64 sum = 0;
    for (const u32 v : map.values())
      sum += v;
    bench::keep(sum);
  }
  state.set_items(keys.size());
}

ENGINE_BENCH_ARGS(hash_map_iterate, "containers.iterate.hash_map", 256, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  HashMap<u32, u32> map;
  for (const u32 k : keys)
    map.insert(k, k);
  while (state.keep_running()) {
    u64 sum = 0;
    for (const u32 v : map.values())
      sum += v;
    bench::keep(sum);
  }
  state.set_items(keys.size());
}

ENGINE_BENCH_ARGS(std_map_iterate, "containers.iterate.std_map", 256, 4096, 65536) {
  const Vector<u32> keys = make_keys(static_cast<u32>(state.arg()));
  std::map<u32, u32> map;
  for (const u32 k : keys)
    map.emplace(k, k);
  while (state.keep_running()) {
    u64 sum = 0;
    for (const auto& kv : map)
      sum += kv.second;
    bench::keep(sum);
  }
  state.set_items(keys.size());
}

// ---- growing a pool by fixed-size runs ---------------------------------------------------------
//
// The shape a capability that owns an arena actually has: a slot is a run of `k_run` elements, and
// acquiring one appends a run by resizing to the new total. `systems/animation`'s pose pool is this
// exactly, and it is the reason this benchmark exists — a `resize` that reserves *exactly* what it
// was asked for reallocates and relocates on every single call, which is O(n^2) in the number of
// runs and cost ten CPU-minutes in a debug build before the pool grew its own capacity by hand.
//
// `resize_exact` is the same loop with the exact path, so the two rows are the policy's before and
// after and stay comparable after the policy changed.
namespace {

constexpr u32 k_run = 23;  // a character's joints, which is where the number comes from

}  // namespace

ENGINE_BENCH_ARGS(vector_resize_runs, "containers.grow.vector_resize_runs", 256, 2048, 16384) {
  const u32 runs = static_cast<u32>(state.arg());
  while (state.keep_running()) {
    Vector<u32> pool;
    for (u32 i = 0; i < runs; ++i)
      pool.resize((i + 1) * k_run);
    bench::keep(pool.size());
  }
  state.set_items(runs);
  state.set_bytes(u64{runs} * k_run * sizeof(u32));
}

ENGINE_BENCH_ARGS(vector_resize_exact_runs, "containers.grow.vector_resize_exact_runs", 256, 2048,
                  16384) {
  const u32 runs = static_cast<u32>(state.arg());
  while (state.keep_running()) {
    Vector<u32> pool;
    for (u32 i = 0; i < runs; ++i)
      pool.resize_exact((i + 1) * k_run);
    bench::keep(pool.size());
  }
  state.set_items(runs);
  state.set_bytes(u64{runs} * k_run * sizeof(u32));
}

// What a caller that knows its population pays: one allocation, no slack. The row is here so the
// two above have a floor to be read against.
ENGINE_BENCH_ARGS(vector_reserved_runs, "containers.grow.vector_reserved_runs", 256, 2048, 16384) {
  const u32 runs = static_cast<u32>(state.arg());
  while (state.keep_running()) {
    Vector<u32> pool;
    pool.reserve(runs * k_run);
    for (u32 i = 0; i < runs; ++i)
      pool.resize((i + 1) * k_run);
    bench::keep(pool.size());
  }
  state.set_items(runs);
  state.set_bytes(u64{runs} * k_run * sizeof(u32));
}
