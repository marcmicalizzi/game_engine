// What a mask level's reads cost when several threads read it at once (T182; tileset.md, "Read
// without a lock"). T172-2 found the walk's collision workers reading one level through one lock and
// one 2 MiB read-ahead window refilled under it: four workers on tiles far apart kept moving the
// window, and a sward call's mask reads took 56 of its 59 ms.
//
// The level is synthetic and the staging's size: 777 x 777 tiles of 8 cells at 1 m, every one
// present, 632 bytes a record, about 381 MB of archive (the forest's level is 603,905 such tiles).
// Its records are a function of the tile, so a read is checked as it is made. A smoke run makes 32 x
// 32. `ENGINE_TILESET_BENCH_LEVEL=<manifest.json>` reads a level that is already built instead
// (opened read-only; nothing is written beside it), which is how the staging's real level is read
// by hand.
//
//   tileset.masks.far/<threads>  each thread makes 16 calls an iteration of 64 reads each, an 8 x 8
//                                block of tiles round a place drawn at random over the level: the
//                                sward's reads in a collision build, on 1, 4 or 8 threads at once.
//                                Items are reads.
//   tileset.masks.load           every tile once, in the archive's order, on one thread: the
//                                renderer's pass at load. Items are tiles. After its run it prints
//                                the process's commit, working set and mapped residency.
#include "../src/archive.h"
#include "../src/records.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/platform/address_space.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/ground_masks.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/bench/bench.h>
#include <foundation/io/vfs.h>

#include <test_temp_dir.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine;

namespace {

constexpr u32 k_cells = 8;
constexpr i64 k_spacing_mm = 1000;
constexpr i32 k_block = 8;           // a call's block of tiles a side: 64 reads
constexpr u32 k_calls = 16;          // calls a thread makes an iteration

// A record's i-th word: a function of the tile alone, so any read can be checked.
u32 word_of(i32 tx, i32 tz, u32 i) noexcept {
  return static_cast<u32>(hash_combine(hash_combine(k_hash_seed, static_cast<u64>(tx) << 32 |
                                                                     static_cast<u32>(tz)),
                                       i));
}

struct Fixture {
  std::unique_ptr<test::TempDir> tmp;
  tileset::MaskLevel level;
  i32 tx0 = 0, tz0 = 0, side_x = 0, side_z = 0;
  u32 words = 0;
  bool synthetic = true;
  bool ok = false;
};

bool make_level(Fixture& f, i32 side, std::string& manifest_path, std::string& error) {
  f.tmp = std::make_unique<test::TempDir>("engine_tileset_bench");
  const std::string dir = f.tmp->file("masks");
  if (io::make_directories(dir) != io::Status::Ok) {
    error = "no scratch directory";
    return false;
  }
  const u32 words = scene_gen::mask_tile_words(k_cells);
  const u32 record = static_cast<u32>(sizeof(tileset::MaskHeader)) + words * 4;
  tileset::archive::Writer writer("EMSK", k_spacing_mm, record);
  tileset::MaskLevelManifest m;
  m.format = std::string(tileset::k_mask_format);
  m.version = tileset::k_mask_version;
  m.key = "0000000000000000";
  m.set_hash = "0000000000000000";
  m.spacing_mm = k_spacing_mm;
  m.cells = k_cells;
  m.tile_min = {0, 0};
  m.tile_max = {side - 1, side - 1};
  for (const char* name : {"soil", "scree", "rock", "snow"})
    m.palette.push_back(name);
  m.budget = 4;
  std::string bytes(record, '\0');
  for (i32 tz = 0; tz < side; ++tz)
    for (i32 tx = 0; tx < side; ++tx) {
      tileset::MaskHeader h;
      h.cells = k_cells;
      h.words = words;
      h.spacing_mm = k_spacing_mm;
      h.tx = tx;
      h.tz = tz;
      std::memcpy(bytes.data(), &h, sizeof(h));
      for (u32 i = 0; i < words; ++i) {
        const u32 w = word_of(tx, tz, i);
        std::memcpy(bytes.data() + sizeof(h) + u64{i} * 4, &w, 4);
      }
      if (!writer.add(tx, tz, bytes)) {
        error = "a record out of order";
        return false;
      }
      m.present.push_back(tx);
      m.present.push_back(tz);
      m.bytes += record;
    }
  m.archive = tileset::mask_archive(k_spacing_mm);
  m.hash = "0000000000000000";
  if (!writer.write(io::join_path(dir, m.archive), m.archive_bytes, error)) return false;
  manifest_path = io::join_path(dir, tileset::k_manifest_name);
  if (io::write_file_atomic(manifest_path, write_json(schema::to_json(m))) != io::Status::Ok) {
    error = "the manifest could not be written";
    return false;
  }
  return true;
}

Fixture& fixture() {
  static std::unique_ptr<Fixture> made;
  if (made != nullptr) return *made;
  made = std::make_unique<Fixture>();
  Fixture& f = *made;
  std::string error, manifest;
  std::string named;
#if ENGINE_PLATFORM_WINDOWS
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "ENGINE_TILESET_BENCH_LEVEL") == 0 && value != nullptr) {
    named = value;
    std::free(value);
  }
#else
  if (const char* value = std::getenv("ENGINE_TILESET_BENCH_LEVEL")) named = value;
#endif
  if (!named.empty() && !bench::smoke_mode()) {
    manifest = named;
    f.synthetic = false;
  } else if (!make_level(f, bench::smoke_mode() ? 32 : 777, manifest, error)) {
    std::fprintf(stderr, "tileset bench: %s\n", error.c_str());
    return f;
  }
  if (!f.level.open(manifest, error)) {
    std::fprintf(stderr, "tileset bench: %s\n", error.c_str());
    return f;
  }
  const scene_gen::GroundMaskInfo& info = f.level.info();
  f.tx0 = info.tile_min_x;
  f.tz0 = info.tile_min_z;
  f.side_x = info.tile_max_x - info.tile_min_x + 1;
  f.side_z = info.tile_max_z - info.tile_min_z + 1;
  f.words = scene_gen::mask_tile_words(info.cells);
  f.ok = f.side_x >= k_block && f.side_z >= k_block;
  return f;
}

// One call: the 8 x 8 block of tiles at (bx, bz), each read and (on the synthetic level) checked.
// Returns the reads that came back wrong.
u32 call(const Fixture& f, i32 bx, i32 bz, std::vector<u32>& words) {
  u32 wrong = 0;
  for (i32 dz = 0; dz < k_block; ++dz)
    for (i32 dx = 0; dx < k_block; ++dx) {
      const i32 tx = bx + dx, tz = bz + dz;
      const bool got = f.level.tile(tx, tz, {words.data(), words.size()});
      if (f.synthetic && (!got || words[1] != word_of(tx, tz, 1) ||
                          words[f.words - 1] != word_of(tx, tz, f.words - 1)))
        ++wrong;
    }
  return wrong;
}

// Threads kept for the whole run and released together an iteration, so the figure is the reads
// and not the threads' start.
struct Crew {
  explicit Crew(u32 n) : count(n) {
    for (u32 t = 0; t < n; ++t)
      threads.emplace_back([this, t] { run(t); });
  }
  ~Crew() {
    stop.store(true);
    generation.fetch_add(1);
    generation.notify_all();
    for (std::thread& th : threads)
      th.join();
  }
  void iterate() {
    remaining.store(count);
    generation.fetch_add(1);
    generation.notify_all();
    for (u32 left = remaining.load(); left != 0; left = remaining.load())
      remaining.wait(left);
  }
  void run(u32 t) {
    std::vector<u32> words(fixture().words);
    u64 seen = 0;
    u64 seed = hash_combine(k_hash_seed, t + 1);
    for (;;) {
      generation.wait(seen);
      seen = generation.load();
      if (stop.load()) return;
      const Fixture& f = fixture();
      u32 bad = 0;
      for (u32 c = 0; c < k_calls; ++c) {
        seed = hash_combine(seed, c);
        const i32 bx = f.tx0 + static_cast<i32>(seed % static_cast<u64>(f.side_x - k_block + 1));
        const i32 bz =
            f.tz0 + static_cast<i32>((seed >> 32) % static_cast<u64>(f.side_z - k_block + 1));
        bad += call(f, bx, bz, words);
      }
      wrong.fetch_add(bad);
      if (remaining.fetch_sub(1) == 1) remaining.notify_all();
    }
  }
  u32 count;
  std::vector<std::thread> threads;
  std::atomic<u64> generation{0};
  std::atomic<u32> remaining{0};
  std::atomic<u32> wrong{0};
  std::atomic<bool> stop{false};
};

void memory_line(const char* when) {
  if (bench::smoke_mode()) return;
  platform::AddressSpaceReport r;
  if (!platform::walk_address_space(r)) return;
  const auto mib = [](u64 b) { return static_cast<f64>(b) / (1024.0 * 1024.0); };
  const platform::RegionKindTotals& mapped =
      r.kinds[static_cast<u32>(platform::RegionKind::mapped_file)];
  const platform::RegionKindTotals& own =
      r.kinds[static_cast<u32>(platform::RegionKind::private_memory)];
  std::fprintf(stderr,
               "tileset bench memory (%s): commit %.1f MiB (private allocations %.1f MiB, %.1f "
               "resident), working set %.1f MiB, mapped views %.1f MiB of which resident %.1f MiB\n",
               when, mib(r.process_commit), mib(own.committed), mib(own.resident),
               mib(r.working_set), mib(mapped.committed + mapped.reserved_only),
               mib(mapped.resident));
}

}  // namespace

ENGINE_BENCH(masks_load, "tileset.masks.load") {
  Fixture& f = fixture();
  if (!f.ok) return;
  memory_line("before the load");
  std::vector<u32> words(f.words);
  state.set_items(static_cast<u64>(f.side_x) * static_cast<u64>(f.side_z));
  u32 wrong = 0;
  while (state.keep_running())
    for (i32 tz = f.tz0; tz < f.tz0 + f.side_z; ++tz)
      for (i32 tx = f.tx0; tx < f.tx0 + f.side_x; ++tx) {
        const bool got = f.level.tile(tx, tz, {words.data(), words.size()});
        if (f.synthetic && (!got || words[1] != word_of(tx, tz, 1))) ++wrong;
      }
  if (wrong != 0) std::fprintf(stderr, "tileset bench: %u loads came back wrong\n", wrong);
  memory_line("after the load");
}

// Benchmarks run in name order, so the far reads come first, on a level this process has not read:
// a mapped page's first touch is in the figure (in the engine the renderer's load has touched every
// present tile before the walk's workers read).
ENGINE_BENCH_ARGS(masks_far, "tileset.masks.far", 1, 4, 8) {
  Fixture& f = fixture();
  if (!f.ok) return;
  const u32 threads = static_cast<u32>(state.arg());
  Crew crew(threads);
  state.set_items(u64{threads} * k_calls * k_block * k_block);
  while (state.keep_running())
    crew.iterate();
  if (crew.wrong.load() != 0)
    std::fprintf(stderr, "tileset bench: %u reads came back wrong\n", crew.wrong.load());
  if (threads == 8) memory_line("after the far reads");
}
