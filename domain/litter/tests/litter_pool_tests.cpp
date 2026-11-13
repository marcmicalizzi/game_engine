// A whole read on the job system's performance pool (T125; litter.md, "Placement"): the window's
// tiles in contiguous runs, joined in run order, so one thread and many give the same bytes; the
// earlier footprints found by tile give what the one-tile scan gives; and each mask tile is read
// once a litter tile rather than once a cell.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <atomic>
#include <limits>

using namespace engine;

namespace {

struct Open {
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement("litter");
  void* state = nullptr;
  ~Open() {
    if (state) g->close(state);
  }
};

struct Pool {
  explicit Pool(u32 workers)
      : system(jobs::JobSystemConfig{.performance_workers = workers, .pin_threads = false}) {}
  jobs::JobSystem system;
  static jobs::JobSystem* get(void* self) { return &static_cast<Pool*>(self)->system; }
};

// The ground and its masks, one state for both (`Ground::context`): integer terraces, so a step
// is a hollow beside it, and a mask level of 8 m tiles whose weights vary by tile and point. No
// C library, so the pin below is the same on every toolchain.
struct World {
  std::atomic<u32> reads{0};
};
f64 terraces(const void*, i64 x, i64 z) noexcept {
  const i64 a = (x >= 0 ? x : -x) / 700, b = (z >= 0 ? z : -z) / 900;
  return .05 * static_cast<f64>(a % 5) + .03 * static_cast<f64>(b % 7);
}
const scene_gen::GroundMaskOps k_ops{
    .info =
        [](const void*, scene_gen::GroundMaskInfo& info) noexcept {
          info = {};
          info.spacing_mm = 1000;
          info.cells = 8;
          info.tile_min_x = -16;
          info.tile_min_z = -16;
          info.tile_max_x = 16;
          info.tile_max_z = 16;
          info.palette = 2;
          info.budget = 2;
          return true;
        },
    .tile =
        [](const void* state, i32 tx, i32 tz, std::span<u32> words) noexcept {
          static_cast<World*>(const_cast<void*>(state))->reads.fetch_add(1);
          for (u32 p = 1; p < words.size(); ++p) {
            const u32 v = (static_cast<u32>(tx * 37 + tz * 91) + p * 13u) & 255u;
            words[p] = v | ((255u - v) << 8);
          }
          words[0] = 0xffff0100u;
          return true;
        }};

u64 hash_placements(const scene_gen::Placements& p) {
  u64 h = k_hash_seed;
  for (const scene_gen::Placement& q : p.instances) {
    h = hash_combine(h, q.id);
    h = hash_combine(h, q.mesh);
    h = hash_combine(h, hash_bytes(&q.position, sizeof(q.position)));
    h = hash_combine(h, hash_bytes(&q.rotation, sizeof(q.rotation)));
  }
  return h;
}

}  // namespace

TEST_CASE("litter: a whole read is the same bytes on one thread and on the pool, in tile order") {
  World world;
  scene_gen::Context c;
  c.tile_size = 16;
  c.world_seed = 7;
  c.ground.surface_fn = terraces;
  c.ground.context = &world;
  c.ground.masks_fn = [](const void* state, scene_gen::GroundMasks& out, std::string*) {
    out = {&k_ops, state};
    return true;
  };
  // Earlier footprints over the window, some on its tiles' borders (16 m), where the anchor test's
  // f64 metres and the sort's integer millimetres may disagree by a tile.
  Vector<scene_gen::Footprint> earlier;
  for (i64 k = 0; k < 240; ++k) {
    const i64 x = -40000 + (k * 7919) % 136000, z = -24000 + (k * 104729) % 104000;
    earlier.push_back(scene_gen::Footprint{x, z, .5 + .01 * static_cast<f64>(k % 50)});
  }
  for (i64 b = -2; b <= 5; ++b)
    earlier.push_back(scene_gen::Footprint{b * 16000, 3 * 16000 - 1 + b, 1.0});
  c.earlier = std::span<const scene_gen::Footprint>(earlier.data(), earlier.size());
  litter::Layer p;
  for (const char* mesh : {"leaf.litter.json", "needles.litter.json", "bark.litter.json"})
    p.meshes.push_back(mesh);
  p.min_x = -3;
  p.max_x = 5;
  p.min_z = -2;
  p.max_z = 3;
  p.cell_m = 2;
  p.background = .55f;
  p.max_slope = 2;
  p.substrate = 0;
  Open a;
  REQUIRE(a.g != nullptr);
  std::string error;
  REQUIRE_MESSAGE(a.g->open(schema::to_json(p), c, &a.state, &error), error);
  scene_gen::Placements serial;
  world.reads = 0;
  REQUIRE_MESSAGE(a.g->expand(a.state, c, serial, &error), error);
  const u32 reads = world.reads.load();
  REQUIRE(serial.instances.size() > 300);
  // Each mask tile once a litter tile: a 16 m tile over 8 m mask tiles reads four, not one a cell.
  const u32 tiles = static_cast<u32>((p.max_x - p.min_x + 1) * (p.max_z - p.min_z + 1));
  CHECK(reads <= 4 * tiles);
  // In tile order: the ids rise, and the read is its tiles one after another.
  for (u32 i = 1; i < serial.instances.size(); ++i)
    CHECK(serial.instances[i].id > serial.instances[i - 1].id);
  scene_gen::Placements joined, one;
  for (i32 z = p.min_z; z <= p.max_z; ++z)
    for (i32 x = p.min_x; x <= p.max_x; ++x) {
      REQUIRE_MESSAGE(a.g->tile(a.state, {x, z}, c, one, &error), error);
      for (const scene_gen::Placement& q : one.instances)
        joined.instances.push_back(q);
    }
  CHECK(hash_placements(joined) == hash_placements(serial));
  // The pin, taken on MSVC with the serial walk this replaced (b7681ac9): the pool's merge
  // changed no byte of what a whole read places.
  CHECK(hash_placements(serial) == 0x0ull);
  // Three and seven workers (and the calling thread): the bytes of one.
  for (const u32 workers : {3u, 7u}) {
    CAPTURE(workers);
    Pool pool(workers);
    c.jobs_fn = &Pool::get;
    c.jobs_context = &pool;
    for (u32 again = 0; again < 3; ++again) {
      scene_gen::Placements many;
      REQUIRE_MESSAGE(a.g->expand(a.state, c, many, &error), error);
      REQUIRE(many.instances.size() == serial.instances.size());
      CHECK(hash_placements(many) == hash_placements(serial));
      CHECK(many.meshes.size() == serial.meshes.size());
    }
    c.jobs_fn = nullptr;
    c.jobs_context = nullptr;
  }
}

TEST_CASE("litter: a refusal on the pool is the earliest tile's, as the serial walk's") {
  scene_gen::Context c;
  c.tile_size = 16;
  // Two refusals with two sentences: the ground is not finite in the window's first tile, and the
  // standing ground not in its last. The walk meets the first; so must every number of threads,
  // whichever job finishes first, and nothing is placed.
  c.ground.surface_fn = [](const void*, i64 x, i64 z) noexcept {
    return x < -32000 && z < -16000 ? std::numeric_limits<f64>::quiet_NaN() : 0.0;
  };
  c.stand.surface_fn = [](const void*, i64 x, i64 z) noexcept {
    return x > 80000 && z > 48000 ? std::numeric_limits<f64>::quiet_NaN() : 0.0;
  };
  litter::Layer p;
  p.meshes.push_back("leaf.litter.json");
  p.min_x = -3;
  p.max_x = 5;
  p.min_z = -2;
  p.max_z = 3;
  p.background = 1;
  Open a;
  std::string error;
  REQUIRE_MESSAGE(a.g->open(schema::to_json(p), c, &a.state, &error), error);
  scene_gen::Placements out;
  CHECK_FALSE(a.g->expand(a.state, c, out, &error));
  const std::string serial = error;
  CHECK(serial.find("nonfinite ground") != std::string::npos);
  CHECK(out.instances.empty());
  scene_gen::Placements last;
  CHECK_FALSE(a.g->tile(a.state, {p.max_x, p.max_z}, c, last, &error));
  CHECK(error.find("nonfinite standing ground") != std::string::npos);
  Pool pool(5);
  c.jobs_fn = &Pool::get;
  c.jobs_context = &pool;
  error.clear();
  CHECK_FALSE(a.g->expand(a.state, c, out, &error));
  CHECK(error == serial);
  CHECK(out.instances.empty());
}
