#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/soil/soil.h>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <vector>
using namespace engine;
TEST_CASE("soil recipes validate strict schema and budgets") {
  soil::SoilFile f;
  std::string e;
  CHECK(soil::read_file("{}", f, e));
  CHECK_FALSE(soil::read_file("{\"width_m\":0}", f, e));
  CHECK_FALSE(soil::read_file("{\"typo\":1}", f, e));
  f.depth_m = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(soil::in_range(f, e));
  CHECK(assets::MeshGeneratorRegistry::global().find("soil") != nullptr);
}
TEST_CASE("soil shapes fit source budgets with finite outward normals") {
  for (auto kind :
       {soil::Kind::Scarp, soil::Kind::Gully, soil::Kind::Undercut, soil::Kind::TreeThrow}) {
    soil::SoilFile f;
    f.kind = kind;
    assets::MeshData m;
    soil::Report r;
    std::string e;
    REQUIRE_MESSAGE(soil::solve(f, m, r, e), e);
    CHECK(r.triangles <= soil::triangle_budget(f));
    CHECK(r.roots == f.roots);
    CHECK(r.stones == f.stones);
    CHECK(m.positions.size() == m.normals.size());
    for (auto p : m.positions)
      CHECK(std::isfinite(p.x + p.y + p.z));
    for (auto n : m.normals)
      CHECK(dot(n, n) == doctest::Approx(1).epsilon(0.001));
    f.triangle_budget = 2000;
    f.roots = 32;
    f.stones = 32;
    CHECK_FALSE(soil::solve(f, m, r, e));
    CHECK(m.indices.empty());
  }
}
TEST_CASE("tree throw uses the source pit dimensions and real terrain edits") {
  soil::SoilFile f;
  f.kind = soil::Kind::TreeThrow;
  CHECK(f.width_m == doctest::Approx(4.23));
  CHECK(f.length_m == doctest::Approx(2.74));
  CHECK(f.plate_height_m == doctest::Approx(2.35));
  CHECK(soil::terrain_delta(f, 0, 0) == doctest::Approx(-0.95));
  CHECK(soil::terrain_delta(f, 0, f.length_m * 0.75f) == doctest::Approx(0.71));
  CHECK(soil::terrain_delta(f, f.width_m, 0) == 0);
  auto terrain = tileset::synth_heightfield(tileset::SynthKind::flat, 257, 125, 0, -16000, -16000);
  std::string e;
  REQUIRE_MESSAGE(soil::cook(f, terrain, {0, 0, 0}, e), e);
  CHECK(terrain.data[128 * 257 + 128] == doctest::Approx(-0.95));
  CHECK(terrain.data.front() == 0);
  CHECK(f.ground.side > 0);
  const u64 before = hash_bytes(terrain.data.data(), terrain.data.size() * sizeof(f32));
  CHECK_FALSE(soil::cook(f, terrain, {16, 0, 0}, e));
  CHECK(hash_bytes(terrain.data.data(), terrain.data.size() * sizeof(f32)) == before);
}
TEST_CASE("soil ground sampling retains far world millimetres") {
  scene_gen::Ground g;
  g.surface_fn = [](const void*, i64 x, i64 z) noexcept -> f64 {
    return static_cast<f64>((x % 64000) + (z % 64000)) / 1000;
  };
  soil::GroundPatch a, b;
  std::string e;
  REQUIRE(soil::sample_ground(g, {16, 0, 16}, 4, 0.125f, a, e));
  REQUIRE(soil::sample_ground(g, {64000016, 0, 64000016}, 4, 0.125f, b, e));
  CHECK(a.heights == b.heights);
}

TEST_CASE("soil shells are closed and consistently wound after exact weld") {
  for (f32 root_radius : {0.005f, 0.045f}) {
    for (auto kind :
         {soil::Kind::Scarp, soil::Kind::Gully, soil::Kind::Undercut, soil::Kind::TreeThrow}) {
      soil::SoilFile f;
      f.kind = kind;
      f.root_radius_m = root_radius;
      assets::MeshData m;
      soil::Report r;
      std::string e;
      REQUIRE(soil::solve(f, m, r, e));
      std::map<std::tuple<f32, f32, f32>, u32> vertices;
      std::vector<u32> ids;
      for (Vec3 p : m.positions) {
        const auto [it, inserted] =
            vertices.try_emplace({p.x, p.y, p.z}, static_cast<u32>(vertices.size()));
        (void)inserted;
        ids.push_back(it->second);
      }
      std::map<std::pair<u32, u32>, u32> edges;
      f64 volume = 0;
      for (u32 i = 0; i < m.indices.size(); i += 3) {
        for (u32 k = 0; k < 3; ++k)
          ++edges[{ids[m.indices[i + k]], ids[m.indices[i + (k + 1) % 3]]}];
        const auto a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]],
                   c = m.positions[m.indices[i + 2]];
        volume += static_cast<f64>(dot(a, cross(b, c))) / 6;
      }
      u32 unmatched = 0, repeated = 0;
      for (const auto& [edge, count] : edges) {
        repeated += count != 1;
        const auto it = edges.find({edge.second, edge.first});
        unmatched += it == edges.end() || it->second != count;
      }
      INFO(static_cast<unsigned>(kind));
      CHECK(unmatched == 0);
      CHECK(repeated == 0);
      CHECK(volume > 0);
    }
  }
}

TEST_CASE("soil cook follows sloping ground and keys the original samples") {
  auto terrain = tileset::synth_heightfield(tileset::SynthKind::flat, 129, 250, 0, -16000, -16000);
  for (u32 j = 0; j < 129; ++j)
    for (u32 i = 0; i < 129; ++i)
      terrain.data[j * 129 + i] = static_cast<f32>(i) * 0.05f;
  soil::SoilFile f;
  f.kind = soil::Kind::Undercut;
  const u64 original = soil::source_hash(f);
  std::string e;
  REQUIRE_MESSAGE(soil::cook(f, terrain, {0, 3.2, 0}, e), e);
  CHECK(soil::source_hash(f) != original);
  CHECK(soil::ground_height(f.ground, 1, 0) == doctest::Approx(0.2));
  CHECK(soil::ground_height(f.ground, -1, 0) == doctest::Approx(-0.2));
  CHECK(terrain.data[64 * 129 + 64] == doctest::Approx(3.2 - 0.95));
  assets::MeshData m;
  soil::Report r;
  REQUIRE(soil::solve(f, m, r, e));
}

TEST_CASE("long soil banks curve with their excavation and bury their end caps") {
  for (auto kind : {soil::Kind::Scarp, soil::Kind::Gully, soil::Kind::Undercut}) {
    soil::SoilFile f;
    f.kind = kind;
    f.width_m = 24;
    f.roots = f.stones = 0;
    CHECK(soil::terrain_delta(f, std::numeric_limits<f32>::max(), 0) == 0);
    CHECK(soil::terrain_delta(f, -std::numeric_limits<f32>::max(), 0) == 0);
    assets::MeshData m;
    soil::Report r;
    std::string e;
    REQUIRE_MESSAGE(soil::solve(f, m, r, e), e);
    CHECK(r.triangles <= soil::triangle_budget(f));
    u32 ends = 0;
    for (auto p : m.positions)
      if (std::abs(p.x) == 12) {
        ++ends;
        CHECK(p.y < 0);
        CHECK(soil::terrain_delta(f, p.x, p.z) == 0);
      }
    CHECK(ends > 0);
    // Samples along the visible lip must stay over the excavation, including
    // off-axis bends; otherwise the terrain fills the bank's lower face.
    f32 min_z = 100, max_z = -100;
    for (const auto& primitive : m.primitives) {
      if (primitive.material != 0) continue;
      for (u32 i = primitive.first_index; i < primitive.first_index + primitive.index_count; ++i) {
        const auto p = m.positions[m.indices[i]];
        if (p.y != 0 || std::abs(p.x) > 3) continue;
        if (soil::terrain_delta(f, p.x, p.z) < -f.depth_m * 0.99f) {
          min_z = std::min(min_z, p.z);
          max_z = std::max(max_z, p.z);
        }
      }
    }
    CHECK(max_z - min_z > 0.1f);
  }
}
