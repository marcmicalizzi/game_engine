#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/flora_species.h>
#include <domain/litter/litter.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
using namespace engine;
namespace {
u64 hash_mesh(const assets::MeshData& m) {
  u64 h = hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(m.normals.data(), m.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(m.uvs.data(), m.uvs.size() * sizeof(Vec2)));
  return hash_combine(h, hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
}
void valid(const assets::MeshData& m) {
  REQUIRE(!m.indices.empty());
  REQUIRE(m.positions.size() == m.normals.size());
  REQUIRE(m.positions.size() == m.uvs.size());
  for (auto v : m.positions) {
    CHECK(std::isfinite(v.x + v.y + v.z));
    CHECK(v.y >= -1e-6f);
  }
  for (auto n : m.normals)
    CHECK(length(n) == doctest::Approx(1).epsilon(.0001));
  for (u32 i = 0; i < m.indices.size(); i += 3) {
    REQUIRE(m.indices[i] < m.positions.size());
    REQUIRE(m.indices[i + 1] < m.positions.size());
    REQUIRE(m.indices[i + 2] < m.positions.size());
    const auto a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]],
               c = m.positions[m.indices[i + 2]];
    CHECK(length(cross(b - a, c - a)) > 0);
  }
}
struct Open {
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement("litter");
  void* state = nullptr;
  ~Open() {
    if (state) g->close(state);
  }
};
f64 hollow(const void*, i64 x, i64 z) noexcept {
  const f64 dx = static_cast<f64>(x) * .001 - 8, dz = static_cast<f64>(z) * .001 - 8;
  return .04 * (dx * dx + dz * dz);
}
}  // namespace
TEST_CASE("litter: researched dimensions, six valid kinds and strict budgets") {
  CHECK(litter::profile(litter::Kind::leaf).length_m >= .079f);
  CHECK(litter::profile(litter::Kind::leaf).length_m <= .230f);
  CHECK(litter::profile(litter::Kind::needles).length_m >= 3 * .0254f);
  CHECK(litter::profile(litter::Kind::cone).length_m <= 8 * .0254f);
  for (u32 k = 0; k < 6; ++k) {
    litter::Recipe p;
    p.kind = static_cast<litter::Kind>(k);
    assets::MeshData m, again;
    litter::Report r;
    std::string error;
    CAPTURE(k);
    REQUIRE_MESSAGE(litter::solve(p, m, r, error), error);
    valid(m);
    CHECK(r.triangles <= r.budget);
    CHECK(r.particles == litter::profile(p.kind).count);
    REQUIRE(litter::solve(p, again, r, error));
    CHECK(hash_mesh(m) == hash_mesh(again));
    p.seed++;
    REQUIRE(litter::solve(p, again, r, error));
    CHECK(hash_mesh(m) != hash_mesh(again));
    p.triangle_budget = 32;
    CHECK_FALSE(litter::solve(p, again, r, error));
    CHECK(again.positions.empty());
  }
}
TEST_CASE("litter: schema validation and immutable shared wood recipe") {
  litter::Recipe p;
  std::string error;
  CHECK_FALSE(litter::read_file("{\"made_up\":1}", p, error));
  CHECK_FALSE(litter::read_file("{\"curl\":-1}", p, error));
  CHECK_FALSE(litter::read_file("{\"kind\":\"unknown\"}", p, error));
  p = {};
  p.radius_m = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(litter::in_range(p, error));
  p = {};
  p.kind = litter::Kind::twig;
  p.count = 1;
  p.radius_m = .05f;
  p.depth_m = 0;
  const auto wood = litter::wood_recipe(p);
  const auto before = deadwood::source_hash(wood);
  assets::MeshData m, source;
  flora::Motion motion;
  deadwood::Report wr;
  litter::Report r;
  REQUIRE_MESSAGE(deadwood::solve(wood, source, motion, wr, error), error);
  REQUIRE_MESSAGE(litter::solve(p, m, r, error), error);
  CHECK(m.indices.size() == source.indices.size());
  CHECK(std::equal(m.uvs.begin(), m.uvs.end(), source.uvs.begin(), source.uvs.end()));
  CHECK(before == deadwood::source_hash(wood));
  litter::Recipe read;
  REQUIRE(litter::read_file(litter::canonical_json(p), read, error));
  CHECK(litter::source_hash(p) == litter::source_hash(read));
}
TEST_CASE("litter: tile order, stable IDs, gaps and obstacle exclusion") {
  Open a;
  REQUIRE(a.g != nullptr);
  litter::Layer p;
  p.meshes.push_back("leaf.litter.json");
  p.background = 1;
  p.max_x = 1;
  scene_gen::Context c;
  c.tile_size = 16;
  const scene_gen::Footprint footprints[] = {{8000, 8000, 2}, {24000, 8000, 2}};
  c.earlier = footprints;
  std::string error;
  REQUIRE_MESSAGE(a.g->open(schema::to_json(p), c, &a.state, &error), error);
  scene_gen::Placements all, one, other;
  REQUIRE(a.g->expand(a.state, c, all, &error));
  REQUIRE(a.g->tile(a.state, {1, 0}, c, other, &error));
  REQUIRE(a.g->tile(a.state, {0, 0}, c, one, &error));
  REQUIRE(all.instances.size() == one.instances.size() + other.instances.size());
  u32 i = 0, previous = 0;
  for (const auto* list : {&one, &other})
    for (const auto& q : list->instances) {
      CHECK(q.id > previous);
      previous = q.id;
      CHECK(q.id == all.instances[i].id);
      CHECK(q.position == all.instances[i].position);
      ++i;
      for (const auto& f : footprints) {
        const f64 dx = q.position.x - f.x_mm * .001, dz = q.position.z - f.z_mm * .001;
        CHECK(dx * dx + dz * dz >= (f.radius + static_cast<f64>(p.extent_m)) *
                                       (f.radius + static_cast<f64>(p.extent_m)));
      }
    }
  for (u32 j = 0; j < all.instances.size(); ++j)
    for (u32 k = j + 1; k < all.instances.size(); ++k) {
      const auto u = all.instances[j].position, v = all.instances[k].position;
      CHECK((u.x - v.x) * (u.x - v.x) + (u.z - v.z) * (u.z - v.z) > 4 * p.extent_m * p.extent_m);
    }
  CHECK(all.instances.size() < 128);
}
TEST_CASE("litter: hollows increase accumulation and bad budgets refuse at open") {
  litter::Layer p;
  p.meshes.push_back("a.litter.json");
  p.background = 0;
  p.hollow_gain = 1;
  p.max_slope = 2;
  scene_gen::Context c;
  c.tile_size = 16;
  std::string error;
  Open a;
  REQUIRE(a.g->open(schema::to_json(p), c, &a.state, &error));
  scene_gen::Placements flat, basin;
  REQUIRE(a.g->tile(a.state, {0, 0}, c, flat, &error));
  CHECK(flat.instances.empty());
  c.ground.surface_fn = hollow;
  REQUIRE(a.g->tile(a.state, {0, 0}, c, basin, &error));
  CHECK(basin.instances.size() > 0);
  for (const auto& v : basin.instances)
    CHECK(length(v.rotation) == doctest::Approx(1));
  Open b;
  p.max_cells = 1;
  CHECK_FALSE(b.g->open(schema::to_json(p), c, &b.state, &error));
  p.max_cells = 4096;
  p.max_total_cells = 1;
  CHECK_FALSE(b.g->open(schema::to_json(p), c, &b.state, &error));
  p.max_total_cells = 1048576;
  p.min_x = std::numeric_limits<i32>::min();
  p.max_x = std::numeric_limits<i32>::max();
  CHECK_FALSE(b.g->open(schema::to_json(p), c, &b.state, &error));
  p = {};
  p.meshes.push_back("a.litter.json");
  p.substrate = 0;
  CHECK_FALSE(b.g->open(schema::to_json(p), c, &b.state, &error));
}
TEST_CASE("litter: substrate weights and failed reads are transactional") {
  struct Mask {
    bool fail = false;
    u32 weight = 255;
  } mask;
  static const scene_gen::GroundMaskOps ops{
      .info =
          [](const void*, scene_gen::GroundMaskInfo& info) noexcept {
            info = {};
            info.spacing_mm = 1000;
            info.cells = 16;
            info.tile_max_x = 0;
            info.tile_max_z = 0;
            info.palette = 2;
            info.budget = 2;
            return true;
          },
      .tile =
          [](const void* state, i32, i32, std::span<u32> words) noexcept {
            const auto& m = *static_cast<const Mask*>(state);
            if (m.fail) return false;
            std::fill(words.begin(), words.end(), m.weight | ((255 - m.weight) << 8));
            words[0] = 0xffff0100u;
            return true;
          }};
  scene_gen::Context c;
  c.tile_size = 16;
  c.ground.context = &mask;
  c.ground.masks_fn = [](const void* state, scene_gen::GroundMasks& out, std::string*) {
    out = {&ops, state};
    return true;
  };
  litter::Layer p;
  p.meshes.push_back("a.litter.json");
  p.background = 1;
  p.substrate = 0;
  Open a;
  std::string error;
  REQUIRE(a.g->open(schema::to_json(p), c, &a.state, &error));
  scene_gen::Placements out;
  REQUIRE(a.g->tile(a.state, {0, 0}, c, out, &error));
  CHECK(out.instances.size() == 64);
  mask.weight = 0;
  REQUIRE(a.g->tile(a.state, {0, 0}, c, out, &error));
  CHECK(out.instances.empty());
  mask.fail = true;
  CHECK_FALSE(a.g->tile(a.state, {0, 0}, c, out, &error));
  CHECK(out.instances.empty());
}
TEST_CASE("litter: far coordinates retain local placement and ground errors clear output") {
  scene_gen::Context c;
  c.tile_size = 16;
  litter::Layer p;
  p.meshes.push_back("a.litter.json");
  p.background = 1;
  Open near, far;
  std::string error;
  REQUIRE(near.g->open(schema::to_json(p), c, &near.state, &error));
  p.min_x = p.max_x = 1000000000;
  p.min_z = p.max_z = -1000000000;
  REQUIRE(far.g->open(schema::to_json(p), c, &far.state, &error));
  scene_gen::Placements a, b;
  REQUIRE(near.g->tile(near.state, {0, 0}, c, a, &error));
  REQUIRE(far.g->tile(far.state, {p.min_x, p.min_z}, c, b, &error));
  REQUIRE(a.instances.size() == b.instances.size());
  for (u32 i = 0; i < a.instances.size(); ++i) {
    CHECK(a.instances[i].id == b.instances[i].id);
    CHECK(b.instances[i].position.x - 16000000000.0 ==
          doctest::Approx(a.instances[i].position.x).epsilon(.00001));
    CHECK(b.instances[i].position.z + 16000000000.0 ==
          doctest::Approx(a.instances[i].position.z).epsilon(.00001));
  }
  c.ground.surface_fn = [](const void*, i64 x, i64) noexcept {
    return x > 4000 ? std::numeric_limits<f64>::quiet_NaN() : 0.0;
  };
  CHECK_FALSE(near.g->tile(near.state, {0, 0}, c, a, &error));
  CHECK(a.instances.empty());
}
TEST_CASE("litter: registry container has rigid noncolliding policy") {
  const auto* g = assets::MeshGeneratorRegistry::global().find("litter");
  REQUIRE(g != nullptr);
  litter::Recipe p;
  p.count = 4;
  geometry::ClusterFileData out;
  litter::Report r;
  std::string error;
  REQUIRE_MESSAGE(litter::build_container(p, {}, {}, 65536, out, r, error), error);
  CHECK(out.generator == "litter");
  CHECK(out.generator_kind == "litter");
  CHECK(out.structural_parts[0].flags == 0);
  CHECK(out.motion_vertices.size() == out.mesh.mesh.vertices.size());
  CHECK(!out.mesh.mesh.quantized.empty());
  CHECK(!out.mesh.lod.empty());
  p.collider = true;
  REQUIRE(litter::build_container(p, {}, {}, 0, out, r, error));
  CHECK(out.structural_parts[0].flags == geometry::k_structural_collider);
}
TEST_CASE("litter: terminal needle form preserves every particle") {
  litter::Recipe p;
  p.kind = litter::Kind::needles;
  geometry::ClusterFileData out;
  litter::Report r;
  std::string error;
  REQUIRE(litter::build_container(p, {}, {}, 0, out, r, error));
  u32 triangles = 0;
  for (u32 i = 0; i < out.mesh.lod.size(); ++i)
    if (out.mesh.lod[i].parent_error == geometry::k_lod_terminal_error)
      triangles += out.mesh.mesh.clusters[i].triangle_count;
  CHECK(triangles == 2 * r.particles);
}
TEST_CASE("litter: obstacles collect patches only in their exterior band") {
  litter::Layer p;
  p.meshes.push_back("a.litter.json");
  p.background = 0;
  p.hollow_gain = 0;
  p.obstacle_gain = 1;
  p.edge_m = 4;
  scene_gen::Context c;
  c.tile_size = 16;
  const scene_gen::Footprint obstacle{8000, 8000, 2};
  c.earlier = {&obstacle, 1};
  Open a;
  std::string error;
  REQUIRE(a.g->open(schema::to_json(p), c, &a.state, &error));
  scene_gen::Placements out;
  REQUIRE(a.g->tile(a.state, {0, 0}, c, out, &error));
  REQUIRE(!out.instances.empty());
  for (const auto& v : out.instances) {
    const f64 dx = v.position.x - 8, dz = v.position.z - 8;
    const f64 gap = std::sqrt(dx * dx + dz * dz) - 2 - static_cast<f64>(p.extent_m);
    CHECK(gap >= 0);
    CHECK(gap < static_cast<f64>(p.edge_m));
  }
}
