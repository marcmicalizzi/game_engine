// A cliff on the world formation (W85; rocks.md, "Geology"): its beds are the field's at the
// world position, so they are the terrain's bands across the foot line; the shader is held to the
// same f64 twin in domain/gfx's formation_tests.
#include "../src/geology.h"

#include <domain/gfx/formation.h>
#include <domain/rocks/rocks.h>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <string>

using namespace engine;
using namespace engine::rocks;

namespace {

scene::Formation dipping() {
  scene::Formation f;
  f.origin = WorldPos{100.0, 250.0, -40.0};
  f.dip_deg = 30.0f;
  f.azimuth_deg = 20.0f;
  f.fold_amplitude_m = 6.0f;
  f.fold_wavelength_m = 220.0f;
  scene::FormationBed a;
  a.name = "sandstone";
  a.thickness_m = 1.6f;
  scene::FormationBed b;
  b.name = "shale";
  b.thickness_m = 0.45f;
  f.beds.push_back(a);
  f.beds.push_back(b);
  return f;
}

Cliff face() {
  Cliff c;
  c.seed = 7;
  c.type = RockType::Sandstone;
  c.height = 18.0f;
  c.triangle_budget = 20000;
  c.quality = RockQuality::Low;
  for (u32 i = 0; i < 4; ++i) {
    CliffPoint p;
    p.x = static_cast<f32>(i) * 12.0f;
    p.y = static_cast<f32>(i) * 1.5f;
    p.z = static_cast<f32>(i * i) * 0.8f;
    c.points.push_back(p);
  }
  return c;
}

}  // namespace

TEST_CASE("rocks: a cliff's beds are the formation's at its world position across the foot line") {
  const scene::Formation field = dipping();
  const WorldPos origin{1234.5, 300.0, -876.25};
  RockGeology geology;
  geology.irregularity = 0.0f;  // the stations' wander off, so the coordinate is the field's own
  const detail::Fabric fabric =
      detail::make_fabric(RockType::Sandstone, geology, 7, &field, origin);
  REQUIRE(fabric.world);
  gfx::FormationFrame frame;
  std::string error;
  REQUIRE(gfx::pack_formation_frame(field, frame, &error));
  // Points on the face's foot line (the recipe's points and between them), where the terrain meets
  // the face: the fabric's bed coordinate is the shader's twin's at the same world point.
  const Cliff c = face();
  for (u32 i = 0; i + 1 < c.points.size(); ++i)
    for (u32 k = 0; k < 8; ++k) {
      const f64 t = static_cast<f64>(k) / 8.0;
      const CliffPoint& a = c.points[i];
      const CliffPoint& b = c.points[i + 1];
      const detail::D3 p{
          static_cast<f64>(a.x) + (static_cast<f64>(b.x) - static_cast<f64>(a.x)) * t,
          static_cast<f64>(a.y) + (static_cast<f64>(b.y) - static_cast<f64>(a.y)) * t,
          static_cast<f64>(a.z) + (static_cast<f64>(b.z) - static_cast<f64>(a.z)) * t};
      const f64 want =
          gfx::formation_coordinate(frame, origin.x + p.x, origin.y + p.y, origin.z + p.z)
              .coordinate;
      CHECK(std::fabs(detail::sample_bed(fabric, p).coordinate - want) < 1e-9);
    }
  // The beds dip 30 degrees: along the dip direction the coordinate falls by sin 30 a metre, and up
  // it rises by cos 30, with the fold's slope beside it.
  CHECK(std::fabs(fabric.normal.y - std::cos(30.0 * std::numbers::pi / 180.0)) < 1e-6);
  // An override: the recipe's own dip and azimuth, the field ignored.
  RockGeology own = geology;
  own.from_formation = false;
  own.dip_deg = 10.0f;
  const detail::Fabric local = detail::make_fabric(RockType::Sandstone, own, 7, &field, origin);
  const detail::Fabric plain = detail::make_fabric(RockType::Sandstone, own, 7);
  CHECK_FALSE(local.world);
  CHECK(detail::sample_bed(local, {3.0, 4.0, 5.0}).coordinate ==
        detail::sample_bed(plain, {3.0, 4.0, 5.0}).coordinate);
}

TEST_CASE("rocks: a cliff with a formation is carved by it, and one without keeps its key") {
  Cliff without = face();
  RockFile file;
  file.shape = RockShape::Cliff;
  file.cliff = without;
  const std::string text = canonical_rock_json(file);
  // No field: none of the formation's fields reach the canonical parameters.
  CHECK(text.find("formation") == std::string::npos);
  CHECK(text.find("world_origin") == std::string::npos);
  Cliff with = without;
  with.formation = dipping();
  with.world_origin = WorldPos{1234.5, 300.0, -876.25};
  file.cliff = with;
  CHECK(canonical_rock_json(file).find("\"formation\"") != std::string::npos);
  assets::MeshData a, b;
  std::string error;
  REQUIRE_MESSAGE(solve_cliff(without, a, nullptr, &error), error);
  REQUIRE_MESSAGE(solve_cliff(with, b, nullptr, &error), error);
  REQUIRE(a.positions.size() > 0);
  bool differs = a.positions.size() != b.positions.size();
  for (u32 i = 0; !differs && i < a.positions.size(); ++i)
    differs = a.positions[i].x != b.positions[i].x || a.positions[i].y != b.positions[i].y ||
              a.positions[i].z != b.positions[i].z;
  CHECK(differs);
  // A field out of range is refused with the scene's sentence.
  with.formation->dip_deg = 89.0f;
  CHECK_FALSE(solve_cliff(with, b, nullptr, &error));
  CHECK(error.find("dip_deg") != std::string::npos);
}
