// The litter entry on a cooked mask level (T125; litter.md, "Placement"; the forest's engine needs
// 66): the level's key is its content, not where its inputs live, so the same masks cooked from two
// directories are one entry and a second load cooks nothing; two cooks into two roots are the same
// bytes; and the entry placed on either level, on one thread or on the pool, is the same bytes.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/schema/json_reflect.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace engine;

namespace {

// A low mound in a 192 m square at 1 m (10 m high, under 19 degrees: soil throughout) and a painted
// litter ramp on one side, so the substrate weight varies across the window and no tile names more
// than two layers.
tileset::Heightfield mound() {
  tileset::Heightfield f;
  f.width = f.height = 193;
  f.spacing_mm = 1000;
  f.origin_x_mm = f.origin_z_mm = -96000;
  f.data.resize(193u * 193u);
  for (u32 z = 0; z < 193; ++z)
    for (u32 x = 0; x < 193; ++x) {
      const f64 dx = static_cast<f64>(x) - 96.0, dz = static_cast<f64>(z) - 96.0;
      const f64 q = (dx * dx + dz * dz) / 3600.0;
      f.data[z * 193 + x] = static_cast<f32>(q < 1.0 ? 10.0 * (1.0 - q) : 0.0);
    }
  return f;
}

// One copy of the inputs in `dir`: the heightfield and the painted image, the same bytes in every
// directory, and the masks naming the image by its path there.
struct Copy {
  std::string dir;
  tileset::TileSet set;
  tileset::MaskInputs in;
  explicit Copy(const std::string& at, const std::string& ddc) : dir(at) {
    std::string error;
    REQUIRE(io::make_directories(dir) == io::Status::Ok);
    REQUIRE_MESSAGE(tileset::write_heightfield(io::join_path(dir, "mound.json"), mound(), error),
                    error);
    tileset::BuildOptions o;
    o.cells = 32;
    o.ring_spacing_mm = 2000;
    o.far_ratio = 2;
    o.far_levels = 1;
    tileset::BuildResult built;
    REQUIRE_MESSAGE(tileset::ensure_tileset(io::join_path(dir, "mound.json"), o, std::string(), ddc,
                                            false, built, error),
                    error);
    REQUIRE_MESSAGE(set.open(built.manifest, error), error);
    Vector<u8> pixels(64u * 64u * 4u, 255);
    for (u32 z = 0; z < 64; ++z)
      for (u32 x = 0; x < 64; ++x) {
        const u32 p = (z * 64 + x) * 4;
        const u8 v = x > 40 ? static_cast<u8>(std::min<u32>(255, (x - 40) * 20)) : 0;
        pixels[p] = pixels[p + 1] = pixels[p + 2] = v;
      }
    REQUIRE(image::write_png(io::join_path(dir, "litter.png"), 64, 64, 4,
                             {pixels.data(), pixels.size()}) == io::Status::Ok);
    for (const char* name : {"soil", "scree", "rock", "snow", "litter"})
      in.palette.push_back(name);
    in.budget = 4;
    in.map_x0_mm = in.map_z0_mm = -96000;
    in.map_size_mm = 192000;
    in.masks.spacing_mm = 1000;
    in.masks.tile_cells = 16;
    scene::TerrainMaskPriors p;
    p.soil = "soil";
    p.scree = "scree";
    p.bedrock = "rock";
    p.snow = "snow";
    p.snowline_m = 200;
    in.masks.priors = p;
    scene::TerrainMaskPaint paint;
    paint.material = "litter";
    paint.image = io::join_path(dir, "litter.png");
    in.masks.paint.push_back(paint);
  }
};

struct Pool {
  jobs::JobSystem system{jobs::JobSystemConfig{.performance_workers = 5, .pin_threads = false}};
  static jobs::JobSystem* get(void* self) { return &static_cast<Pool*>(self)->system; }
};

// The ground and the masks a host hands: the set's surface and a cooked level.
struct HostGround {
  const tileset::TileSet* set = nullptr;
  const tileset::MaskLevel* level = nullptr;
};
f64 host_surface(const void* g, i64 x, i64 z) noexcept {
  return static_cast<f64>(static_cast<const HostGround*>(g)->set->height_mm(x, z));
}
bool host_masks(const void* g, scene_gen::GroundMasks& out, std::string*) {
  out = static_cast<const HostGround*>(g)->level->masks();
  return true;
}

u64 place(const tileset::TileSet& set, const tileset::MaskLevel& level, Pool* pool, u32& count) {
  HostGround g{&set, &level};
  scene_gen::Context c;
  c.tile_size = 16;
  c.ground.surface_fn = host_surface;
  c.ground.context = &g;
  c.ground.masks_fn = host_masks;
  if (pool != nullptr) {
    c.jobs_fn = &Pool::get;
    c.jobs_context = pool;
  }
  litter::Layer p;
  p.meshes.push_back("leaf.litter.json");
  p.min_x = p.min_z = -5;
  p.max_x = p.max_z = 4;
  p.background = .9f;
  p.max_slope = 2;
  p.substrate = 4;  // "litter": the painted side
  const auto* g_litter = scene_gen::GeneratorRegistry::global().find_placement("litter");
  REQUIRE(g_litter != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(g_litter->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements out;
  const bool ok = g_litter->expand(state, c, out, &error);
  g_litter->close(state);
  REQUIRE_MESSAGE(ok, error);
  count = out.instances.size();
  u64 h = k_hash_seed;
  for (const scene_gen::Placement& q : out.instances) {
    h = hash_combine(h, q.id);
    h = hash_combine(h, hash_bytes(&q.position, sizeof(q.position)));
    h = hash_combine(h, hash_bytes(&q.rotation, sizeof(q.rotation)));
  }
  return h;
}

}  // namespace

TEST_CASE(
    "litter: the same masks cooked from two directories are one level, and the entry's bytes") {
  test::TempDir tmp{"engine_litter_masks"};
  const std::string ddc = tmp.file("ddc");
  Copy a(tmp.file("a"), ddc), b(tmp.file("b"), ddc);
  std::string error;
  tileset::MaskBuildResult first, second;
  REQUIRE_MESSAGE(tileset::ensure_mask_level(a.set, a.in, std::string(), ddc, false, first, error),
                  error);
  CHECK_FALSE(first.hit);
  const auto written = std::filesystem::last_write_time(first.manifest);
  // The second directory's inputs: other paths, the same bytes. One key, found, nothing cooked.
  REQUIRE_MESSAGE(tileset::ensure_mask_level(b.set, b.in, std::string(), ddc, false, second, error),
                  error);
  CHECK(second.hit);
  CHECK(second.key == first.key);
  CHECK(second.manifest == first.manifest);
  CHECK(second.hash == first.hash);
  CHECK(std::filesystem::last_write_time(first.manifest) == written);
  // Cooked again into a second root: the same key and the same bytes.
  tileset::MaskBuildResult other;
  REQUIRE_MESSAGE(
      tileset::ensure_mask_level(b.set, b.in, std::string(), tmp.file("ddc2"), false, other, error),
      error);
  CHECK_FALSE(other.hit);
  CHECK(other.key == first.key);
  CHECK(other.hash == first.hash);
  // The entry on either level, on one thread and on the pool: one set of bytes.
  tileset::MaskLevel one, two;
  REQUIRE_MESSAGE(one.open(first.manifest, error), error);
  REQUIRE_MESSAGE(two.open(other.manifest, error), error);
  u32 n1 = 0, n2 = 0, n3 = 0;
  const u64 h1 = place(a.set, one, nullptr, n1);
  const u64 h2 = place(b.set, two, nullptr, n2);
  Pool pool;
  const u64 h3 = place(b.set, two, &pool, n3);
  CHECK(n1 > 100);
  CHECK(n1 == n2);
  CHECK(n1 == n3);
  CHECK(h1 == h2);
  CHECK(h1 == h3);
}
